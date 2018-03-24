/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1997-2005
 *	Herbert Xu <herbert@gondor.apana.org.au>.  All rights reserved.
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

#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#ifdef HAVE_GETPWNAM
#include <pwd.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>
#ifdef HAVE_GLOB
#include <glob.h>
#endif
#include <ctype.h>
#include <stdbool.h>

/*
 * Routines to expand arguments to commands.  We have to deal with
 * backquotes, shell variables, and file metacharacters.
 */

#include "shell.h"
#include "main.h"
#include "nodes.h"
#include "eval.h"
#include "expand.h"
#include "syntax.h"
#include "parser.h"
#include "jobs.h"
#include "options.h"
#include "var.h"
#include "output.h"
#include "memalloc.h"
#include "error.h"
#include "mystring.h"
#include "show.h"
#include "system.h"

/*
 * pmatch() flags
 */
#define PM_MATCHMAX   0x01 /* Look for the longest match. Combine with PM_MATCHLEFT or PM_MATCHRIGHT. */
#define PM_MATCHRIGHT 0x02 /* Match a suffix, not the full string. Return the start of the suffix. */
#define PM_MATCHLEFT  0x04 /* Match a prefix, not the full string. Return the end of the prefix. */
#define PM_CTLESC     0x08 /* Skip over CTLESC characters. */

/*
 * _rmescape() flags
 */
#define RMESCAPE_ALLOC	0x1	/* Allocate a new string */
#define RMESCAPE_GLOB	0x2	/* Add backslashes for glob */
#define RMESCAPE_GROW	0x8	/* Grow strings instead of stalloc */
#define RMESCAPE_HEAP	0x10	/* Malloc strings instead of stalloc */

/* Add CTLESC when necessary. */
#define QUOTES_ESC	(EXP_FULL | EXP_CASE)
/* Do not skip NUL characters. */
#define QUOTES_KEEPNUL	EXP_TILDE

/*
 * Structure specifying which parts of the string should be searched
 * for IFS characters.
 */

struct ifsregion {
	struct ifsregion *next;	/* next region in list */
	int begoff;		/* offset of start of region */
	int endoff;		/* offset of end of region */
	int nulonly;		/* search for nul bytes only */
};

/* output of current string */
static char *expdest;
/* list of back quote expressions */
static struct nodelist *argbackq;
/* first struct in list of ifs regions */
static struct ifsregion ifsfirst;
/* last struct in list */
static struct ifsregion *ifslastp;
/* holds expanded arg list */
static struct arglist exparg;

STATIC void argstr(char *, int);
STATIC char *exptilde(char *, char *, int);
STATIC void expbackq(union node *, int);
STATIC const char *subevalvar(char *, char *, int, int, int, int, int);
STATIC char *evalvar(char *, int);
STATIC size_t strtodest(const char *, int);
STATIC void memtodest(const char *, size_t, int);
STATIC ssize_t varvalue(char *, int, int);
STATIC void expandmeta(struct strlist *, int);
#ifdef HAVE_GLOB
STATIC void addglob(const glob_t *);
#else
STATIC void expmeta(char *, char *);
STATIC struct strlist *expsort(struct strlist *);
STATIC struct strlist *msort(struct strlist *, int);
#endif
STATIC void addfname(char *);
STATIC int patmatch(char *, const char *);
STATIC const char *pmatch(const char *, const char *, int);
STATIC int cvtnum(intmax_t);
STATIC size_t esclen(const char *, const char *);
STATIC void varunset(const char *, const char *, const char *, int)
	__attribute__((__noreturn__));


/*
 * Prepare a pattern for a glob(3) call.
 *
 * Returns an stalloced string.
 */

STATIC inline char *
preglob(const char *pattern, int flag) {
	flag |= RMESCAPE_GLOB;
	return _rmescapes((char *)pattern, flag);
}


STATIC size_t
esclen(const char *start, const char *p) {
	size_t esc = 0;

	while (p > start && *--p == (char)CTLESC) {
		esc++;
	}
	return esc;
}


static inline const char *getpwhome(const char *name)
{
#ifdef HAVE_GETPWNAM
	struct passwd *pw = getpwnam(name);
	return pw ? pw->pw_dir : 0;
#else
	return 0;
#endif
}


/*
 * Perform variable substitution and command substitution on an argument,
 * placing the resulting list of arguments in arglist.  If EXP_FULL is true,
 * perform splitting and file name expansion.  When arglist is NULL, perform
 * here document expansion.
 */

void
expandarg(union node *arg, struct arglist *arglist, int flag)
{
	struct strlist *sp;
	char *p;

	argbackq = arg->narg.backquote;
	STARTSTACKSTR(expdest);
	argstr(arg->narg.text, flag);
	p = _STPUTC('\0', expdest);
	expdest = p - 1;
	if (arglist == NULL) {
		/* here document expanded */
		goto out;
	}
	p = grabstackstr(p);
	exparg.lastp = &exparg.list;
	/*
	 * TODO - EXP_REDIR
	 */
	if (flag & EXP_FULL) {
		ifsbreakup(p, -1, &exparg);
		*exparg.lastp = NULL;
		exparg.lastp = &exparg.list;
		expandmeta(exparg.list, flag);
	} else {
		sp = (struct strlist *)stalloc(sizeof (struct strlist));
		sp->text = p;
		*exparg.lastp = sp;
		exparg.lastp = &sp->next;
	}
	*exparg.lastp = NULL;
	if (exparg.list) {
		*arglist->lastp = exparg.list;
		arglist->lastp = exparg.lastp;
	}

out:
	ifsfree();
}



/*
 * Perform variable and command substitution.  If EXP_FULL is set, output CTLESC
 * characters to allow for further processing.  Otherwise treat
 * $@ like $* since no splitting will be performed.
 */

STATIC void
argstr(char *p, int flag)
{
	static const char spclchars[] = {
		'=',
		':',
		CTLQUOTEMARK,
		CTLENDVAR,
		CTLESC,
		CTLVAR,
		CTLBACKQ,
		CTLENDARI,
		0
	};
	const char *reject = spclchars;
	int prev, c = 0;
	int breakall = (flag & (EXP_WORD | EXP_QUOTED)) == EXP_WORD;
	size_t length;
	int startloc;

	if (!(flag & EXP_VARTILDE)) {
		reject += 2;
	} else if (flag & EXP_VARTILDE2) {
		reject++;
	}
	length = 0;
	if (flag & EXP_TILDE) {
		char *q;

		flag &= ~EXP_TILDE;
tilde:
		q = p;
		if (*q == '~')
			p = exptilde(p, q, flag);
	}
start:
	startloc = expdest - (char *)stackblock();
	for (;;) {
		length += strcspn(p + length, reject);
		prev = c;
		c = (signed char)p[length];
		if (c && (!(c & 0x80) || c == CTLENDARI)) {
			/* c == '=' || c == ':' || c == CTLENDARI */
			length++;
		}
		if (length > 0) {
			int newloc;
			expdest = stnputs(p, length, expdest);
			newloc = expdest - (char *)stackblock();
			if (breakall && !(flag & EXP_QUOTED) && newloc > startloc) {
				recordregion(startloc, newloc, 0);
			}
			startloc = newloc;
		}
		p += length + 1;
		length = 0;

		switch (c) {
		case '\0':
			goto breakloop;
		case '=':
			if (flag & EXP_VARTILDE2) {
				p--;
				continue;
			}
			flag |= EXP_VARTILDE2;
			reject++;
			/* fall through */
		case ':':
			/*
			 * sort of a hack - expand tildes in variable
			 * assignments (after the first '=' and after ':'s).
			 */
			if (*--p == '~') {
				goto tilde;
			}
			continue;
		}

		switch (c) {
			int dolatstrhack;
		case CTLENDVAR: /* ??? */
			goto breakloop;
		case CTLQUOTEMARK:
			flag ^= EXP_QUOTED;
addquote:
			if (flag & QUOTES_ESC) {
				p--;
				length++;
				startloc++;
			}
			break;
		case CTLESC:
			if (!(flag & EXP_QUOTED) && (c >= CTL_FIRST && c <= CTL_LAST)) {
				length++;
				if (flag & QUOTES_ESC) {
					p--;
					length++;
				}
				break;
			}
			startloc++;
			length++;
			goto addquote;
		case CTLVAR:
			/* "$@" syntax adherence hack */
			dolatstrhack = p[1] == '@' && (*p & VSTYPE) != VSMINUS && (*p & VSTYPE) != VSLENGTH && !shellparam.nparam && flag & QUOTES_ESC;
			p = evalvar(p, flag);
			if (dolatstrhack && prev == (char)CTLQUOTEMARK && *p == (char)CTLQUOTEMARK) {
				expdest--;
				flag ^= EXP_QUOTED;
				p++;
			}
			goto start;
		case CTLBACKQ:
			expbackq(argbackq->n, flag);
			argbackq = argbackq->next;
			goto start;
		case CTLENDARI:
			p--;
			expari(flag);
			goto start;
		}
	}
breakloop:
	;
}

STATIC char *
exptilde(char *startp, char *p, int flag)
{
	signed char c;
	char *name;
	const char *home;
	int quotes = flag & QUOTES_ESC;

	name = p + 1;

	while ((c = *++p) != '\0') {
		switch(c) {
		case CTLESC:
			return (startp);
		case CTLQUOTEMARK:
			return (startp);
		case ':':
			if (flag & EXP_VARTILDE)
				goto done;
			break;
		case '/':
		case CTLENDVAR:
			goto done;
		}
	}
done:
	*p = '\0';
	if (*name == '\0') {
		home = lookupvar(homestr);
	} else {
		home = getpwhome(name);
	}
	if (!home || !*home)
		goto lose;
	*p = c;
	if (quotes)
		STPUTC(CTLQUOTEMARK, expdest);
	strtodest(home, quotes | EXP_QUOTED);
	if (quotes)
		STPUTC(CTLQUOTEMARK, expdest);
	return (p);
lose:
	*p = c;
	return (startp);
}


void 
removerecordregions(int endoff)
{
	if (ifslastp == NULL)
		return;

	if (ifsfirst.endoff > endoff) {
		while (ifsfirst.next != NULL) {
			struct ifsregion *ifsp;
			INTOFF;
			ifsp = ifsfirst.next->next;
			ckfree(ifsfirst.next);
			ifsfirst.next = ifsp;
			INTON;
		}
		if (ifsfirst.begoff > endoff)
			ifslastp = NULL;
		else {
			ifslastp = &ifsfirst;
			ifsfirst.endoff = endoff;
		}
		return;
	}
	
	ifslastp = &ifsfirst;
	while (ifslastp->next && ifslastp->next->begoff < endoff)
		ifslastp=ifslastp->next;
	while (ifslastp->next != NULL) {
		struct ifsregion *ifsp;
		INTOFF;
		ifsp = ifslastp->next->next;
		ckfree(ifslastp->next);
		ifslastp->next = ifsp;
		INTON;
	}
	if (ifslastp->endoff > endoff)
		ifslastp->endoff = endoff;
}


/*
 * Expand arithmetic expression.  Backup to start of expression,
 * evaluate, place result in (backed up) result, adjust string position.
 */
void
expari(int flag)
{
	struct stackmark sm;
	char *p, *start;
	int begoff;
	int len;
	intmax_t result;

	/*	ifsfree(); */

	/*
	 * This routine is slightly over-complicated for
	 * efficiency.  Next we scan backwards looking for the
	 * start of arithmetic.
	 */
	start = stackblock();
	p = expdest;
	pushstackmark(&sm, p - start);
	*--p = '\0';
	p--;
	do {
		int esc;

		while (*p != (char)CTLARI) {
			p--;
#ifdef DEBUG
			if (p < start) {
				sh_error("missing CTLARI (shouldn't happen)");
			}
#endif
		}

		esc = esclen(start, p);
		if (!(esc % 2)) {
			break;
		}

		p -= esc + 1;
	} while (1);

	begoff = p - start;

	removerecordregions(begoff);

	expdest = p;

	if (likely(flag & QUOTES_ESC))
		rmescapes(p + 1);

	result = arith(p + 1);
	popstackmark(&sm);

	len = cvtnum(result);

	if (likely(!(flag & EXP_QUOTED)))
		recordregion(begoff, begoff + len, 0);
}


/*
 * Expand stuff in backwards quotes.
 */

STATIC void
expbackq(union node *cmd, int flag)
{
	struct backcmd in;
	int i;
	char buf[128];
	char *p;
	char *dest;
	int startloc;
	struct stackmark smark;

	INTOFF;
	startloc = expdest - (char *)stackblock();
	pushstackmark(&smark, startloc);
	evalbackcmd(cmd, (struct backcmd *) &in);
	popstackmark(&smark);

	p = in.buf;
	i = in.nleft;
	if (i == 0)
		goto read;
	for (;;) {
		memtodest(p, i, flag & (QUOTES_ESC | EXP_QUOTED));
read:
		if (in.fd < 0)
			break;
		do {
			i = read(in.fd, buf, sizeof buf);
		} while (i < 0 && errno == EINTR);
		TRACE(("expbackq: read returns %d\n", i));
		if (i <= 0)
			break;
		p = buf;
	}

	if (in.buf)
		ckfree(in.buf);
	if (in.fd >= 0) {
		close(in.fd);
		back_exitstatus = waitforjob(in.jp);
	}
	INTON;

	/* Eat all trailing newlines */
	dest = expdest;
	for (; dest > (char *)stackblock() && dest[-1] == '\n';)
		STUNPUTC(dest);
	expdest = dest;

	if (!(flag & EXP_QUOTED))
		recordregion(startloc, dest - (char *)stackblock(), 0);
	TRACE(("evalbackq: size=%d: \"%.*s\"\n",
		(dest - (char *)stackblock()) - startloc,
		(dest - (char *)stackblock()) - startloc,
		stackblock() + startloc));
}


STATIC const char *
subevalvar(char *p, char *str, int strloc, int subtype, int startloc, int varflags, int flag)
{
	int quotes = flag & QUOTES_ESC;
	char *startp;
	char *loc;
	struct nodelist *saveargbackq = argbackq;
	int amount;

	argstr(p, EXP_TILDE | (subtype != VSASSIGN && subtype != VSQUESTION ?
			       EXP_CASE : 0));
	STPUTC('\0', expdest);
	argbackq = saveargbackq;
	startp = stackblock() + startloc;

	switch (subtype) {
	case VSASSIGN:
		setvar(str, startp, 0);
		amount = startp - expdest;
		STADJUST(amount, expdest);
		return startp;

	case VSQUESTION:
		varunset(p, str, startp, varflags);
		/* NOTREACHED */
	}

#ifdef DEBUG
	if (subtype < VSTRIMRIGHT || subtype > VSTRIMLEFTMAX)
		abort();
#endif

	str = stackblock() + strloc;
	preglob(str, 0);

	loc = (char *)pmatch(str, startp, (quotes ? PM_CTLESC : 0) | (subtype & (PM_MATCHLEFT | PM_MATCHRIGHT | PM_MATCHMAX)));
	if (loc) {
		if (subtype & PM_MATCHLEFT) {
			memmove(startp, loc, str - loc);
			loc = startp + (str - loc) - 1;
		}
		*loc = '\0';
		amount = loc - expdest;
		STADJUST(amount, expdest);
	}
	return loc;
}


/*
 * Expand a variable, and return a pointer to the next character in the
 * input string.
 */
STATIC char *
evalvar(char *p, int flag)
{
	int subtype;
	int varflags;
	char *var;
	int patloc;
	int c;
	int startloc;
	ssize_t varlen;
	int easy;
	int quoted;

	varflags = *p++;
	subtype = varflags & VSTYPE;

	if (!subtype)
		sh_error("Bad substitution");

	quoted = flag & EXP_QUOTED;
	var = p;
	easy = (!quoted || (*var == '@' && shellparam.nparam));
	startloc = expdest - (char *)stackblock();
	p = strchr(p, '=') + 1;

again:
	varlen = varvalue(var, varflags, flag);
	if (varflags & VSNUL)
		varlen--;

	if (subtype == VSPLUS) {
		varlen = -1 - varlen;
		goto vsplus;
	}

	if (subtype == VSMINUS) {
vsplus:
		if (varlen < 0) {
			argstr(p, flag | EXP_TILDE | EXP_WORD);
			goto end;
		}
		goto record;
	}

	if (subtype == VSASSIGN || subtype == VSQUESTION) {
		if (varlen >= 0)
			goto record;

		subevalvar(p, var, 0, subtype, startloc, varflags,
			   flag & ~QUOTES_ESC);
		varflags &= ~VSNUL;
		/* 
		 * Remove any recorded regions beyond 
		 * start of variable 
		 */
		removerecordregions(startloc);
		goto again;
	}

	if (varlen < 0 && uflag && (*var != '@' && *var != '*'))
		varunset(p, var, 0, 0);

	if (subtype == VSLENGTH) {
		cvtnum(varlen > 0 ? varlen : 0);
		goto record;
	}

	if (subtype == VSNORMAL) {
record:
		if (!easy)
			goto end;
		recordregion(startloc, expdest - (char *)stackblock(), quoted);
		goto end;
	}

#ifdef DEBUG
	switch (subtype) {
	case VSTRIMLEFT:
	case VSTRIMLEFTMAX:
	case VSTRIMRIGHT:
	case VSTRIMRIGHTMAX:
		break;
	default:
		abort();
	}
#endif

	if (varlen >= 0) {
		/*
		 * Terminate the string and start recording the pattern
		 * right after it
		 */
		STPUTC('\0', expdest);
		patloc = expdest - (char *)stackblock();
		if (subevalvar(p, NULL, patloc, subtype,
			       startloc, varflags, flag) == 0) {
			int amount = expdest - (
				(char *)stackblock() + patloc - 1
			);
			STADJUST(-amount, expdest);
		}
		/* Remove any recorded regions beyond start of variable */
		removerecordregions(startloc);
		goto record;
	}

end:
	if (subtype != VSNORMAL) {	/* skip to end of alternative */
		int nesting = 1;
		for (;;) {
			if ((c = (signed char)*p++) == CTLESC)
				p++;
			else if (c == CTLBACKQ) {
				if (varlen >= 0)
					argbackq = argbackq->next;
			} else if (c == CTLVAR) {
				if ((*p++ & VSTYPE) != VSNORMAL)
					nesting++;
			} else if (c == CTLENDVAR) {
				if (--nesting == 0)
					break;
			}
		}
	}
	return p;
}


/*
 * Put a string on the stack.
 */

STATIC void
memtodest(const char *p, size_t len, int quotes) {
	char *q;

	if (unlikely(!len))
		return;

	q = makestrspace(len * 2, expdest);

	do {
		int c = (signed char)*p++;
		if (c) {
			if (quotes & QUOTES_ESC) {
				switch (c) {
					case '\\':
					case '!': case '*': case '?': case '[': case '=':
					case '~': case ':': case '/': case '-': case ']':
						if (quotes & EXP_QUOTED)
					case CTLCHARS:
							USTPUTC(CTLESC, q);
						break;
				}
			}
		} else if (!(quotes & QUOTES_KEEPNUL))
			continue;
		USTPUTC(c, q);
	} while (--len);

	expdest = q;
}


STATIC size_t
strtodest(const char *p, int quotes)
{
	size_t len = strlen(p);
	memtodest(p, len, quotes);
	return len;
}



/*
 * Add the value of a specialized variable to the stack string.
 */

STATIC ssize_t
varvalue(char *name, int varflags, int flags)
{
	int num;
	char *p;
	int i;
	int sep;
	char sepc;
	char **ap;
	int subtype = varflags & VSTYPE;
	int discard = subtype == VSPLUS || subtype == VSLENGTH;
	int quotes = (flags & (EXP_QUOTED | (discard ? 0 : QUOTES_ESC))) | QUOTES_KEEPNUL;
	ssize_t len = 0;

	flags &= EXP_QUOTED | EXP_FULL;

	switch (*name) {
	case '$':
		num = rootpid;
		goto numvar;
	case '?':
		num = exitstatus;
		goto numvar;
	case '#':
		num = shellparam.nparam;
		goto numvar;
	case '!':
		num = backgndpid;
		if (num == 0)
			return -1;
numvar:
		len = cvtnum(num);
		break;
	case '-':
		p = makestrspace(NOPTS, expdest);
		for (i = NOPTS - 1; i >= 0; i--) {
			if (optlist[i] && optletters[i]) {
				USTPUTC(optletters[i], p);
				len++;
			}
		}
		expdest = p;
		break;
	case '@':
		flags &= EXP_FULL;
		/* fall through */
	case '*':
		if (flags == EXP_FULL)
			sep = 1 << CHAR_BIT;
		else
			sep = ifsset() ? ifsval()[0] : ' ';
		sepc = sep;
		if (!*(ap = shellparam.p))
			return -1;
		while ((p = *ap++)) {
			len += strtodest(p, quotes);

			if (*ap && sep) {
				len++;
				memtodest(&sepc, 1, quotes);
			}
		}
		break;
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
		num = atoi(name);
		if (num < 0 || num > shellparam.nparam)
			return -1;
		p = num ? shellparam.p[num - 1] : arg0;
		goto value;
	default:
		p = lookupvar(name);
value:
		if (!p)
			return -1;

		len = strtodest(p, quotes);
		break;
	}

	if (discard)
		STADJUST(-len, expdest);
	return len;
}



/*
 * Record the fact that we have to scan this region of the
 * string for IFS characters.
 */

void
recordregion(int start, int end, int nulonly)
{
	struct ifsregion *ifsp;

	if (ifslastp == NULL) {
		ifsp = &ifsfirst;
	} else {
		INTOFF;
		ifsp = (struct ifsregion *)ckmalloc(sizeof (struct ifsregion));
		ifsp->next = NULL;
		ifslastp->next = ifsp;
		INTON;
	}
	ifslastp = ifsp;
	ifslastp->begoff = start;
	ifslastp->endoff = end;
	ifslastp->nulonly = nulonly;
}



/*
 * Break the argument string into pieces based upon IFS and add the
 * strings to the argument list.  The regions of the string to be
 * searched for IFS characters have been stored by recordregion.
 * If maxargs is non-negative, at most maxargs arguments will be created, by
 * joining together the last arguments.
 */
void
ifsbreakup(char *string, int maxargs, struct arglist *arglist)
{
	struct ifsregion *ifsp;
	struct strlist *sp;
	char *start;
	char *p;
	char *q;
	char *r = NULL;
	const char *ifs, *realifs;
	int ifsspc;
	int nulonly;


	start = string;
	if (ifslastp != NULL) {
		ifsspc = 0;
		nulonly = 0;
		realifs = ifsset() ? ifsval() : defifs;
		ifsp = &ifsfirst;
		do {
			p = string + ifsp->begoff;
			nulonly = ifsp->nulonly;
			ifs = nulonly ? nullstr : realifs;
			ifsspc = 0;
			while (p < string + ifsp->endoff) {
				int c;
				bool isifs;
				bool isdefifs;

				q = p;
				c = *p++;
				if (c == (char)CTLESC)
					c = *p++;

				isifs = strchr(ifs, c);
				isdefifs = false;
				if (isifs)
					isdefifs = strchr(defifs, c);

				/* If only reading one more argument:
				 * If we have exactly one field,
				 * read that field without its terminator.
				 * If we have more than one field,
				 * read all fields including their terminators,
				 * except for trailing IFS whitespace.
				 *
				 * This means that if we have only IFS
				 * characters left, and at most one
				 * of them is non-whitespace, we stop
				 * reading here.
				 * Otherwise, we read all the remaining
				 * characters except for trailing
				 * IFS whitespace.
				 *
				 * In any case, r indicates the start
				 * of the characters to remove, or NULL
				 * if no characters should be removed.
				 */
				if (!maxargs) {
					if (isdefifs) {
						if (!r)
							r = q;
						continue;
					}

					if (!(isifs && ifsspc))
						r = NULL;

					ifsspc = 0;
					continue;
				}

				if (ifsspc) {
					if (isifs)
						q = p;

					start = q;

					if (isdefifs)
						continue;

					isifs = false;
				}

				if (isifs) {
					if (!nulonly)
						ifsspc = isdefifs;
					/* Ignore IFS whitespace at start */
					if (q == start && ifsspc) {
						start = p;
						ifsspc = 0;
						continue;
					}
					if (maxargs > 0 && !--maxargs) {
						r = q;
						continue;
					}
					*q = '\0';
					sp = (struct strlist *)stalloc(sizeof *sp);
					sp->text = start;
					*arglist->lastp = sp;
					arglist->lastp = &sp->next;
					start = p;
					continue;
				}

				ifsspc = 0;
			}
		} while ((ifsp = ifsp->next) != NULL);
		if (nulonly)
			goto add;
	}

	if (r)
		*r = '\0';

	if (!*start)
		return;

add:
	sp = (struct strlist *)stalloc(sizeof *sp);
	sp->text = start;
	*arglist->lastp = sp;
	arglist->lastp = &sp->next;
}

void ifsfree(void)
{
	struct ifsregion *p = ifsfirst.next;

	if (!p)
		goto out;

	INTOFF;
	do {
		struct ifsregion *ifsp;
		ifsp = p->next;
		ckfree(p);
		p = ifsp;
	} while (p);
	ifsfirst.next = NULL;
	INTON;

out:
	ifslastp = NULL;
}



/*
 * Expand shell metacharacters.  At this point, the only control characters
 * should be escapes.  The results are stored in the list exparg.
 */

#ifdef HAVE_GLOB
STATIC void
expandmeta(str, flag)
	struct strlist *str;
	int flag;
{
	/* TODO - EXP_REDIR */

	while (str) {
		const char *p;
		glob_t pglob;
		int i;

		if (fflag)
			goto nometa;
		INTOFF;
		p = preglob(str->text, RMESCAPE_ALLOC | RMESCAPE_HEAP);
		i = glob(p, GLOB_NOMAGIC, 0, &pglob);
		if (p != str->text)
			ckfree(p);
		switch (i) {
		case 0:
			if (!(pglob.gl_flags & GLOB_MAGCHAR))
				goto nometa2;
			addglob(&pglob);
			globfree(&pglob);
			INTON;
			break;
		case GLOB_NOMATCH:
nometa2:
			globfree(&pglob);
			INTON;
nometa:
			*exparg.lastp = str;
			rmescapes(str->text);
			exparg.lastp = &str->next;
			break;
		default:	/* GLOB_NOSPACE */
			sh_error("Out of space");
		}
		str = str->next;
	}
}


/*
 * Add the result of glob(3) to the list.
 */

STATIC void
addglob(pglob)
	const glob_t *pglob;
{
	char **p = pglob->gl_pathv;

	do {
		addfname(*p);
	} while (*++p);
}


#else	/* HAVE_GLOB */
STATIC char *expdir;


STATIC void
expandmeta(struct strlist *str, int flag)
{
	static const char metachars[] = {
		'*', '?', '[', 0
	};
	/* TODO - EXP_REDIR */

	while (str) {
		struct strlist **savelastp;
		struct strlist *sp;
		char *p;

		if (fflag)
			goto nometa;
		if (!strpbrk(str->text, metachars))
			goto nometa;
		savelastp = exparg.lastp;

		INTOFF;
		p = preglob(str->text, RMESCAPE_ALLOC | RMESCAPE_HEAP);
		{
			int i = strlen(str->text);
			expdir = ckmalloc(i < 2048 ? 2048 : i); /* XXX */
		}

		expmeta(expdir, p);
		ckfree(expdir);
		if (p != str->text)
			ckfree(p);
		INTON;
		if (exparg.lastp == savelastp) {
			/*
			 * no matches
			 */
nometa:
			*exparg.lastp = str;
			rmescapes(str->text);
			exparg.lastp = &str->next;
		} else {
			*exparg.lastp = NULL;
			*savelastp = sp = expsort(*savelastp);
			while (sp->next != NULL)
				sp = sp->next;
			exparg.lastp = &sp->next;
		}
		str = str->next;
	}
}


/*
 * Do metacharacter (i.e. *, ?, [...]) expansion.
 */

STATIC void
expmeta(char *enddir, char *name)
{
	char *p;
	const char *cp;
	char *start;
	char *endname;
	int metaflag;
	struct stat64 statb;
	DIR *dirp;
	struct dirent *dp;
	int atend;
	int matchdot;
	int esc;

	metaflag = 0;
	start = name;
	for (p = name; esc = 0, *p; p += esc + 1) {
		if (*p == '*' || *p == '?')
			metaflag = 1;
		else if (*p == '[') {
			char *q = p + 1;
			if (*q == '!')
				q++;
			for (;;) {
				if (*q == '\\')
					q++;
				if (*q == '/' || *q == '\0')
					break;
				if (*++q == ']') {
					metaflag = 1;
					break;
				}
			}
		} else {
			if (*p == '\\')
				esc++;
			if (p[esc] == '/') {
				if (metaflag)
					break;
				start = p + esc + 1;
			}
		}
	}
	if (metaflag == 0) {	/* we've reached the end of the file name */
		if (enddir != expdir)
			metaflag++;
		p = name;
		do {
			if (*p == '\\')
				p++;
			*enddir++ = *p;
		} while (*p++);
		if (metaflag == 0 || lstat64(expdir, &statb) >= 0)
			addfname(expdir);
		return;
	}
	endname = p;
	if (name < start) {
		p = name;
		do {
			if (*p == '\\')
				p++;
			*enddir++ = *p++;
		} while (p < start);
	}
	if (enddir == expdir) {
		cp = ".";
	} else if (enddir == expdir + 1 && *expdir == '/') {
		cp = "/";
	} else {
		cp = expdir;
		enddir[-1] = '\0';
	}
	if ((dirp = opendir(cp)) == NULL)
		return;
	if (enddir != expdir)
		enddir[-1] = '/';
	if (*endname == 0) {
		atend = 1;
	} else {
		atend = 0;
		*endname = '\0';
		endname += esc + 1;
	}
	matchdot = 0;
	p = start;
	if (*p == '\\')
		p++;
	if (*p == '.')
		matchdot++;
	while (! int_pending() && (dp = readdir(dirp)) != NULL) {
		if (dp->d_name[0] == '.' && ! matchdot)
			continue;
		if (pmatch(start, dp->d_name, 0)) {
			if (atend) {
				scopy(dp->d_name, enddir);
				addfname(expdir);
			} else {
				for (p = enddir, cp = dp->d_name;
				     (*p++ = *cp++) != '\0';)
					continue;
				p[-1] = '/';
				expmeta(p, endname);
			}
		}
	}
	closedir(dirp);
	if (! atend)
		endname[-esc - 1] = esc ? '\\' : '/';
}
#endif	/* HAVE_GLOB */


/*
 * Add a file name to the list.
 */

STATIC void
addfname(char *name)
{
	struct strlist *sp;

	sp = (struct strlist *)stalloc(sizeof *sp);
	sp->text = sstrdup(name);
	*exparg.lastp = sp;
	exparg.lastp = &sp->next;
}


#ifndef HAVE_GLOB
/*
 * Sort the results of file name expansion.  It calculates the number of
 * strings to sort and then calls msort (short for merge sort) to do the
 * work.
 */

STATIC struct strlist *
expsort(struct strlist *str)
{
	int len;
	struct strlist *sp;

	len = 0;
	for (sp = str ; sp ; sp = sp->next)
		len++;
	return msort(str, len);
}


STATIC struct strlist *
msort(struct strlist *list, int len)
{
	struct strlist *p, *q = NULL;
	struct strlist **lpp;
	int half;
	int n;

	if (len <= 1)
		return list;
	half = len >> 1;
	p = list;
	for (n = half ; --n >= 0 ; ) {
		q = p;
		p = p->next;
	}
	q->next = NULL;			/* terminate first half of list */
	q = msort(list, half);		/* sort first half of list */
	p = msort(p, len - half);		/* sort second half */
	lpp = &list;
	for (;;) {
		if (strcmp(p->text, q->text) < 0) {
			*lpp = p;
			lpp = &p->next;
			if ((p = *lpp) == NULL) {
				*lpp = q;
				break;
			}
		} else {
			*lpp = q;
			lpp = &q->next;
			if ((q = *lpp) == NULL) {
				*lpp = p;
				break;
			}
		}
	}
	return list;
}
#endif


/*
 * Returns true if the pattern matches the string.
 */

STATIC inline int
patmatch(char *pattern, const char *string)
{
	return !!pmatch(preglob(pattern, 0), string, 0);
}


STATIC int ccmatch(const char *p, int chr, const char **r)
{
	static const struct class {
		char name[10];
		int (*fn)(int);
	} classes[] = {
		{ .name = ":alnum:]", .fn = isalnum },
		{ .name = ":cntrl:]", .fn = iscntrl },
		{ .name = ":lower:]", .fn = islower },
		{ .name = ":space:]", .fn = isspace },
		{ .name = ":alpha:]", .fn = isalpha },
		{ .name = ":digit:]", .fn = isdigit },
		{ .name = ":print:]", .fn = isprint },
		{ .name = ":upper:]", .fn = isupper },
		{ .name = ":blank:]", .fn = isblank },
		{ .name = ":graph:]", .fn = isgraph },
		{ .name = ":punct:]", .fn = ispunct },
		{ .name = ":xdigit:]", .fn = isxdigit },
	};
	const struct class *class, *end;

	end = classes + sizeof(classes) / sizeof(classes[0]);
	for (class = classes; class < end; class++) {
		const char *q;

		q = prefix(p, class->name);
		if (!q)
			continue;
		*r = q;
		return class->fn(chr);
	}

	*r = 0;
	return 0;
}

STATIC const char *
pmatch(const char *pattern, const char *string, int flags)
{
	const bool ctlesc     = flags & PM_CTLESC;
	const bool matchleft  = flags & PM_MATCHLEFT;
	const bool matchright = flags & PM_MATCHRIGHT;
	const bool matchmax   = flags & PM_MATCHMAX;

	const char *p, *q;
	char c;

	const char *result = NULL;

	static size_t *sstbuf, sstbuflen;
	size_t *stbuf, stbuflen;
	size_t ste, stp, stc, stcc;
	if (sstbuf == NULL) {
		INTOFF;
		sstbuflen = 128;
		sstbuf = ckmalloc(sstbuflen * sizeof *sstbuf);
		INTON;
	}
	stbuflen = sstbuflen;
	stbuf = sstbuf;

	ste = 0;
	stbuf[ste++] = 0;
	if (matchright)
		stbuf[ste++] = 0;

	q = string;
	for (;;) {
		char chr;
		if (!matchright)
			string = q;
		if (ctlesc && *q == (char)CTLESC)
			q++;
		chr = *q++;
		stcc = -1;
		stc = ste;
		if (matchright) {
			stbuf[stc++] = 0;
			stbuf[stc++] = q - string;
		}
		for (stp = 0; stp != ste; stp += 1 + matchright) {
			bool ast = false, found = false;
			p = pattern + stbuf[stp];
again:
			switch (c = *p++) {
			case '\0':
				if (matchleft || chr == '\0') {
					result = string + (matchright ? stbuf[stp + 1] : 0);
					if (!matchmax)
						return result;
				}
				break;
			case '\\':
				if (*p) {
					c = *p++;
				}
				goto dft;
			case '*':
				ast = true;
				c = *p;
				while (c == '*')
					c = *++p;
				goto again;
			case '?':
				if (chr != '\0')
					found = true;
				break;
			case '[': {
				bool invert = false;
				if (*p == '!') {
					invert = true;
					p++;
				}
				if (chr == '\0')
					break;
				c = *p++;
				do {
					if (!c) {
						p = pattern + stbuf[stp] + 1;
						c = '[';
						goto dft;
					}
					if (c == '[') {
						const char *r;
						found |= !!ccmatch(p, chr, &r);
						if (r) {
							p = r;
							continue;
						}
					} else if (c == '\\')
						c = *p++;
					if (*p == '-' && p[1] != ']') {
						p++;
						if (*p == '\\')
							p++;
						if (chr >= c && chr <= *p)
							found = true;
						p++;
					} else {
						if (chr == c)
							found = true;
					}
				} while ((c = *p++) != ']');
				if (invert)
					found = !found;
				break;
			}
dft:		default:
				found = chr == c;
				break;
			}

			if (stbuflen - stc < (ast + found) << matchright) {
				INTOFF;
				sstbuflen *= 2;
				sstbuf = ckrealloc(stbuf, stbuflen);
				INTON;
				stbuf = sstbuf;
				stbuflen = sstbuflen;
			}

			if (ast) {
				if (stcc == stbuf[stp]) {
					if (matchright && matchmax)
						stbuf[stc - 1] = stbuf[stp + 1];
				} else {
					stcc = stbuf[stc++] = stbuf[stp];
					if (matchright)
						stbuf[stc++] = stbuf[stp + 1];
				}
			}

			if (found) {
				if (stcc == p - pattern) {
					if (matchright && matchmax)
						stbuf[stc - 1] = stbuf[stp + 1];
				} else {
					stcc = stbuf[stc++] = p - pattern;
					if (matchright)
						stbuf[stc++] = stbuf[stp + 1];
				}
			}
		}
		if (stc == stp || !chr)
			return result;
		ste = stc - stp;
		memmove(stbuf, stbuf + stp, ste * sizeof *stbuf);
	}
}



/*
 * Remove any CTLESC characters from a string.
 */

char *
_rmescapes(char *str, int flag)
{
	char *p, *q, *r;
	unsigned inquotes;
	int notescaped;
	int globbing;

	p = strpbrk(str, qchars);
	if (!p) {
		return str;
	}
	q = p;
	r = str;
	if (flag & RMESCAPE_ALLOC) {
		size_t len = p - str;
		size_t fulllen = len + strlen(p) + 1;

		if (flag & RMESCAPE_GROW) {
			int strloc = str - (char *)stackblock();

			r = makestrspace(fulllen, expdest);
			str = (char *)stackblock() + strloc;
			p = str + len;
		} else if (flag & RMESCAPE_HEAP) {
			r = ckmalloc(fulllen);
		} else {
			r = stalloc(fulllen);
		}
		q = r;
		if (len > 0) {
			q = mempcpy(q, str, len);
		}
	}
	inquotes = 0;
	globbing = flag & RMESCAPE_GLOB;
	notescaped = globbing;
	while (*p) {
		if (*p == (char)CTLQUOTEMARK) {
			inquotes = ~inquotes;
			p++;
			notescaped = globbing;
			continue;
		}
		if (*p == (char)CTLESC) {
			p++;
			if (notescaped)
				*q++ = '\\';
		} else if (*p == '\\' && !inquotes) {
			/* naked back slash */
			notescaped = 0;
			goto copy;
		}
		notescaped = globbing;
copy:
		*q++ = *p++;
	}
	*q = '\0';
	if (flag & RMESCAPE_GROW) {
		expdest = r;
		STADJUST(q - r + 1, expdest);
	}
	return r;
}



/*
 * See if a pattern matches in a case statement.
 */

int
casematch(union node *pattern, char *val)
{
	struct stackmark smark;
	int result;

	setstackmark(&smark);
	argbackq = pattern->narg.backquote;
	STARTSTACKSTR(expdest);
	argstr(pattern->narg.text, EXP_TILDE | EXP_CASE);
	STACKSTRNUL(expdest);
	ifsfree();
	result = patmatch(stackblock(), val);
	popstackmark(&smark);
	return result;
}

/*
 * Our own itoa().
 */

STATIC int
cvtnum(intmax_t num)
{
	int len = max_int_length(sizeof(num));

	expdest = makestrspace(len, expdest);
	len = fmtstr(expdest, len, "%" PRIdMAX, num);
	STADJUST(len, expdest);
	return len;
}

STATIC void
varunset(const char *end, const char *var, const char *umsg, int varflags)
{
	const char *msg;
	const char *tail;

	tail = nullstr;
	msg = "parameter not set";
	if (umsg) {
		if (*end == (char)CTLENDVAR) {
			if (varflags & VSNUL)
				tail = " or null";
		} else
			msg = umsg;
	}
	sh_error("%.*s: %s%s", end - var - 1, var, msg, tail);
}

#ifdef mkinit

INCLUDE "expand.h"

RESET {
	ifsfree();
}

#endif
