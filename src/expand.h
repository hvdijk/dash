/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1997-2005
 *	Herbert Xu <herbert@gondor.apana.org.au>.  All rights reserved.
 * Copyright (c) 2019, 2021
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
 *
 *	@(#)expand.h	8.2 (Berkeley) 5/4/95
 */

#ifndef H_EXPAND
#define H_EXPAND 1

#include "config.h"

#include <inttypes.h>

struct strlist {
	struct strlist *next;
	char *text;
};


struct arglist {
	struct strlist *list;
	struct strlist **lastp;
};

/*
 * expandarg() flags
 */
#define EXP_FULL	0x1	/* perform word splitting & file globbing */
#define EXP_TILDE	0x2	/* do normal tilde expansion */
#define EXP_VARTILDE2	0x4	/* expand tildes after colons only */
#define EXP_VARTILDE	0x8	/* expand tildes in an assignment */
#define EXP_REDIR	0x10	/* file glob for a redirection (1 match only) */
#define EXP_CASE	0x20	/* keeps quotes around for CASE pattern */
#define EXP_WORD	0x80	/* expand word in parameter expansion */
#define EXP_QUOTED	0x100	/* expand word in double quotes */
#define EXP_DISCARD	0x200	/* discard result of expansion */
#define EXP_XTRACE	0x400	/* expand xtrace prompt; disable xtrace */
#ifdef ENABLE_INTERNAL_COMPLETION
#define EXP_BACKSLASH	0x800	/* got unquoted backslash */
#define EXP_RECURSE	0x1000	/* recursive expansion */
#define EXP_COMPLETE	0x2000	/* expand completion */
#define EXP_NOMETA	0x4000	/* expand to no words if no matches are found */
#define EXP_COMMAND	0x8000	/* expand command */
#define EXP_PATH	0x10000	/* search in $PATH */
#endif


union node;
void expandarg(union node *, struct arglist *, int);
#define rmescapes(p) _rmescapes((p), 0)
char *_rmescapes(char *, int);
int casematch(union node *, char *);
void recordregion(int, int, int);
void removerecordregions(int); 
void ifsbreakup(char *, int, struct arglist *);
void ifsfree(void);
void varunset(const char *, const char *, const char *, int)
	attribute((noreturn));

/* From arith.y */
intmax_t arith(const char *);
int expcmd(int , char **);
#ifdef USE_LEX
void arith_lex_reset(void);
#else
#define arith_lex_reset()
#endif
int yylex(void);

#endif
