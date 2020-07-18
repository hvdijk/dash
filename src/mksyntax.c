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

/*
 * This program creates syntax.h and syntax.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>


struct synclass {
	char *name;
	char *comment;
};

/*
 * Syntax classes for is_ functions.  Warning:  if you add new classes
 * you may have to change the definition of the is_in_name macro.
 */
struct synclass is_entry[] = {
	{ "ISDIGIT",	"a digit" },
	{ "ISUPPER",	"an upper case letter" },
	{ "ISLOWER",	"a lower case letter" },
	{ "ISODIGIT",	"an octal digit" },
	{ "ISXDIGIT",	"a hexadecimal digit" },
	{ "ISUNDER",	"an underscore" },
	{ "ISSPECL",	"the name of a special parameter" },
	{ "ISSPACE",	"a space character" },
	{ NULL, 	NULL }
};

static char writer[] = "\
/*\n\
 * This file was generated by the mksyntax program.\n\
 */\n\
\n\
#include \"config.h\"\n\
\n";


static FILE *cfile;
static FILE *hfile;
static char *syntax[256];

static void filltable(char *);
static void add(char *, char *);
static void print(char *);
static void output_type_macros(void);
int main(int, char **);

int
main(int argc, char **argv)
{
	int i;
	char buf[80];
	int pos;

	/* Create output files */
	if ((cfile = fopen("syntax.c", "w")) == NULL) {
		perror("syntax.c");
		exit(2);
	}
	if ((hfile = fopen("syntax.h", "w")) == NULL) {
		perror("syntax.h");
		exit(2);
	}
	fputs(writer, hfile);
	fputs(writer, cfile);

	fputs("#ifdef CEOF\n", hfile);
	fputs("#undef CEOF\n", hfile);
	fputs("#endif\n", hfile);
	fputs("\n", hfile);

	/* Generate the #define statements in the header file */
	fputs("/* Syntax classes for is_ functions */\n", hfile);
	for (i = 0 ; is_entry[i].name ; i++) {
		sprintf(buf, "#define %s %#o", is_entry[i].name, 1 << i);
		fputs(buf, hfile);
		for (pos = strlen(buf) ; pos < 32 ; pos = (pos + 8) & ~07)
			putc('\t', hfile);
		fprintf(hfile, "/* %s */\n", is_entry[i].comment);
	}
	putc('\n', hfile);
	fputs("#ifdef WITH_LOCALE\n", hfile);
	fprintf(hfile, "#define PMBW %d\n\n", -258);
	fprintf(hfile, "#define PMBB %d\n\n", -257);
	fputs("#endif\n\n", hfile);
	fprintf(hfile, "#define SYNBASE %d\n", 128);
	fprintf(hfile, "#define PEOF %d\n\n", -256);
	putc('\n', hfile);
	output_type_macros();		/* is_digit, etc. */
	putc('\n', hfile);

	/* Generate the syntax tables. */
	fputs("#include \"shell.h\"\n", cfile);
	fputs("#include \"syntax.h\"\n\n", cfile);
	filltable("0");
	fputs("\n/* character classification table */\n", cfile);
	add("01234567", "ISODIGIT | ISDIGIT | ISXDIGIT");
	add("89", "ISDIGIT | ISXDIGIT");
	add("abcdef", "ISLOWER | ISXDIGIT");
	add("ABCDEF", "ISUPPER | ISXDIGIT");
	add("ghijklmnopqrstuvwxyz", "ISLOWER");
	add("GHIJKLMNOPQRSTUVWXYZ", "ISUPPER");
	add("_", "ISUNDER");
	add("#?$!-*@", "ISSPECL");
	add(" \f\n\r\t\v", "ISSPACE");
	print("is_type");
	exit(0);
	/* NOTREACHED */
}



/*
 * Clear the syntax table.
 */

static void
filltable(char *dftval)
{
	int i;

	for (i = 0 ; i < 256; i++)
		syntax[i] = dftval;
}


/*
 * Add entries to the syntax table.
 */

static void
add(char *p, char *type)
{
	while (*p)
		syntax[(signed char)*p++ + 128] = type;
}



/*
 * Output the syntax table.
 */

static void
print(char *name)
{
	int i;

	fprintf(hfile, "extern const unsigned char %s[];\n", name);
	fprintf(cfile, "const unsigned char %s[] = {\n      ", name);
	for (i = 0 ;; i++) {
		fputs(syntax[i], cfile);
		if (i == 255)
			break;
		fputs(",\n      ", cfile);
	}
	fputs("\n};\n", cfile);
}



/*
 * Output character classification macros (e.g. is_digit).  If digits are
 * contiguous, we can test for them quickly.
 */

static char *macro[] = {
	"#define ctype(c)\t((unsigned char) *(is_type+SYNBASE+(signed char)(c)))\n",
	"#define is_odigit(c)\t((unsigned) ((c) - '0') <= 7)\n",
	"#define is_digit(c)\t((unsigned) ((c) - '0') <= 9)\n",
	"#define is_xdigit(c)\t(ctype((c)) & ISXDIGIT)\n",
	"#define is_alpha(c)\t(ctype((c)) & (ISUPPER|ISLOWER))\n",
	"#define is_alnum(c)\t(ctype((c)) & (ISUPPER|ISLOWER|ISDIGIT))\n",
	"#define is_name(c)\t(ctype((c)) & (ISUPPER|ISLOWER|ISUNDER))\n",
	"#define is_in_name(c)\t(ctype((c)) & (ISUPPER|ISLOWER|ISUNDER|ISDIGIT))\n",
	"#define is_special(c)\t(ctype((c)) & (ISSPECL|ISDIGIT))\n",
	"#define is_space(c)\t(ctype((c)) & ISSPACE)\n",
	NULL
};

static void
output_type_macros(void)
{
	char **pp;

	for (pp = macro ; *pp ; pp++)
		fputs(*pp, hfile);
	fputs("#define digit_val(c)\t((c) - '0')\n", hfile);
}
