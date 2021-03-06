/*
 * Copyright (c) 1999 Herbert Xu <herbert@gondor.apana.org.au>
 * Copyright (c) 2018 Harald van Dijk <harald@gigawatt.nl>
 * This file contains code for the times builtin.
 */

#include "config.h"

#include <sys/times.h>
#include <unistd.h>
#include "bltin.h"
#include "system.h"

int timescmd(int argc, char *argv[]) {
	struct tms buf;
	long int clk_tck = sysconf(_SC_CLK_TCK);

	endargs();

	times(&buf);
	printf("%dm%fs %dm%fs\n%dm%fs %dm%fs\n",
	       (int) (buf.tms_utime / clk_tck / 60),
	       ((double) buf.tms_utime) / clk_tck,
	       (int) (buf.tms_stime / clk_tck / 60),
	       ((double) buf.tms_stime) / clk_tck,
	       (int) (buf.tms_cutime / clk_tck / 60),
	       ((double) buf.tms_cutime) / clk_tck,
	       (int) (buf.tms_cstime / clk_tck / 60),
	       ((double) buf.tms_cstime) / clk_tck);
	return 0;
}
