/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1997-2005
 *	Herbert Xu <herbert@gondor.apana.org.au>.  All rights reserved.
 * Copyright (c) 2018-2020
 *	Harald van Dijk <harald@gigawatt.nl>.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Kenneth Almquist.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"

#include <stdio.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>


#include "shell.h"
#include "main.h"
#include "mail.h"
#include "options.h"
#include "output.h"
#include "parser.h"
#include "nodes.h"
#include "expand.h"
#include "eval.h"
#include "jobs.h"
#include "input.h"
#include "trap.h"
#include "var.h"
#include "show.h"
#include "memalloc.h"
#include "error.h"
#include "init.h"
#include "mystring.h"
#ifndef SMALL
#include "myhistedit.h"
#endif
#include "exec.h"
#include "cd.h"

#define PROFILE 0

int rootpid;
int shlvl;
#ifdef __GLIBC__
int *gwsh_errno;
#endif
#if PROFILE
short profile_buf[16384];
extern int etext();
#endif

STATIC void read_profile(const char *);
STATIC char *find_dot_file(char *);
int main(int, char **);

/*
 * Main routine.  We initialize things, parse the arguments, execute
 * profiles if we're a login shell, and then call cmdloop to execute
 * commands.  The setjmp call sets up the location to jump to when an
 * exception occurs.  When an exception occurs the variable "state"
 * is used to figure out how far we had gotten.
 */

int
main(int argc, char **argv)
{
	char *shinit;
#if HAVE_COMPUTED_GOTO
	void *volatile state = NULL;
#define SET_STATE(s) (state = &&state##s)
#else
	volatile int state = 0;
#define SET_STATE(s) (state = s)
#endif
	struct jmploc jmploc;
	struct stackmark smark;
	int login;

#ifdef __GLIBC__
	gwsh_errno = __errno_location();
#endif

#if PROFILE
	monitor(4, etext, profile_buf, sizeof profile_buf, 50);
#endif
	if (unlikely(setjmp(jmploc.loc))) {
		int e;
#if HAVE_COMPUTED_GOTO
		void *s;
#else
		int s;
#endif

		exitreset();
		envreset();

		e = exception;

		s = state;
		if (e == EXEXIT || !s || iflag == 0 || shlvl)
			exitshell();

		reset();

		if (e == EXINT) {
			out2c('\n');
#ifdef FLUSHERR
			flushout(out2);
#endif
		}
		popstackmark(&smark);
		FORCEINTON;				/* enable interrupts */
#if HAVE_COMPUTED_GOTO
		goto *s;
#else
		switch (s) {
		default:
		case 4: goto state4;
		case 3: goto state3;
		case 2: goto state2;
		case 1: goto state1;
		}
#endif
	}
	handler = &jmploc;
#ifdef DEBUG
	opentrace();
	trputs("Shell args:  ");  trargs(argv);
#endif
	rootpid = getpid();
	init();
	setstackmark(&smark);
	login = procargs(argc, argv);
	if (login) {
		SET_STATE(1);
		read_profile("/etc/profile");
state1:
		SET_STATE(2);
		if (!pflag) {
			read_profile("${HOME-}/.profile");
		}
	}
state2:
	SET_STATE(3);
	if (!pflag && iflag) {
		if ((shinit = lookupvar("ENV")) != NULL && *shinit != '\0') {
			read_profile(shinit);
		}
	}
	popstackmark(&smark);
state3:
	SET_STATE(4);
#ifndef SMALL
	read_histfile();
#endif
	if (minusc)
		evalstring(minusc, sflag ? 0 : EV_EXIT);

state4:
	if (sflag || minusc == NULL) {
		cmdloop(1);
	}
#ifndef SMALL
	write_histfile();
#endif
#if PROFILE
	monitor(0);
#endif
#if GPROF
	{
		extern void _mcleanup(void);
		_mcleanup();
	}
#endif
	exitshell();
	/* NOTREACHED */
}


/*
 * Read and execute commands.  "Top" is nonzero for the top level command
 * loop; it turns on prompting if the shell is interactive.
 */

int
cmdloop(int top)
{
	union node *n;
	struct stackmark smark;
	int inter;
	int status = 0;
	int numeof = 0;

	TRACE(("cmdloop(%d) called\n", top));
	for (;;) {
		int skip;

		setstackmark(&smark);
		if (jobctl)
			showjobs(out2, SHOW_CHANGED);
		inter = 0;
		if (iflag && top) {
			inter++;
			chkmail();
		}
		n = parsecmd(inter);
		/* showtree(n); DEBUG */
		if (n == NEOF) {
			if (!top || numeof >= 50)
				break;
			if (!stoppedjobs()) {
				if (!Iflag) {
					if (iflag) {
						out2c('\n');
#ifdef FLUSHERR
						flushout(out2);
#endif
					}
					break;
				}
				out2str("\nUse \"exit\" to leave shell.\n");
			}
			numeof++;
		} else {
			int i;

			job_warning = (job_warning == 2) ? 1 : 0;
			numeof = 0;
			i = evaltree(n, 0);
			if (n)
				status = i;
		}
		popstackmark(&smark);

		skip = evalskip;
		if (skip) {
			evalskip &= ~SKIPFUNC;
			break;
		}
	}

	return status;
}



/*
 * Read /etc/profile or .profile.  Return on error.
 */

STATIC void
read_profile(const char *name)
{
	name = expandstr(name, 0);
	if (setinputfile(name, INPUT_PUSH_FILE | INPUT_NOFILE_OK) < 0)
		return;

	cmdloop(0);
	popfile();
}



/*
 * Read a file containing shell functions.
 */

void
readcmdfile(char *name)
{
	setinputfile(name, INPUT_PUSH_FILE);
	cmdloop(0);
	popfile();
}



/*
 * Take commands from a file.  To be compatible we should do a path
 * search for the file, which is necessary to find sub-commands.
 */


STATIC char *
find_dot_file(char *basename)
{
	char *fullname;
	const char *path = pathval();
	struct stat statb;
	int len;

	/* don't try this for absolute or relative paths */
	if (strchr(basename, '/'))
		return basename;

	while ((len = padvance(&path, basename)) >= 0) {
		fullname = stackblock();
		if ((stat(fullname, &statb) == 0) && S_ISREG(statb.st_mode)) {
			/* This will be freed by the caller. */
			return stalloc(len);
		}
	}

	/* not found in the PATH */
	sh_error("%s: not found", basename);
	/* NOTREACHED */
}

int
dotcmd(int argc, char **argv)
{
	int status = 0;
	char *name;
	char *savedotfile;

	nextopt(nullstr);
	name = nextarg(1);
	endargs();

	name = find_dot_file(name);
	setinputfile(name, INPUT_PUSH_FILE);
	savedotfile = dotfile;
	dotfile = name;
	commandname = NULL;
	status = cmdloop(0);
	dotfile = savedotfile;
	popfile();

	return status;
}


int
exitcmd(int argc, char **argv)
{
	if (stoppedjobs())
		return 0;

	if (argc > 1)
		exitstatus = number(argv[1]);
	else if (savestatus >= 0)
		exitstatus = savestatus;

	exraise(EXEXIT);
	/* NOTREACHED */
}
