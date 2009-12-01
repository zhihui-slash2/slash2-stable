/* $Id$ */

#include <sys/types.h>
#include <sys/wait.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "psc_util/bitflag.h"

const char *progname;

#define NENTS 5000
int buf[NENTS];

__dead void
usage(void)
{
	fprintf(stderr, "usage: %s\n", progname);
	exit(1);
}

#define CNT_ASSERT(code, xcode)				\
	switch (fork()) {				\
	case -1:					\
		psc_fatal("fork");			\
	case 0: /* child */				\
		code;					\
		exit(0);				\
	default: /* parent */				\
		if (wait(&st) == -1)			\
			psc_fatal("wait");		\
		if (!(xcode))				\
			psc_fatalx("want %s, got %d",	\
			    # xcode, st);		\
		break;					\
	}

#define CNT_ASSERT0(code)	CNT_ASSERT(code, st == 0)
#define CNT_ASSERTA(code)	CNT_ASSERT(code, WCOREDUMP(st))

#define B0	(1 << 0)
#define B1	(1 << 1)
#define B2	(1 << 2)
#define B3	(1 << 3)
#define B4	(1 << 4)
#define B5	(1 << 5)
#define B6	(1 << 6)
#define B7	(1 << 7)

int
main(int argc, char *argv[])
{
	unsigned char *in, *out;
	int st, f, c;
	int64_t v;

	progname = argv[0];
	while ((c = getopt(argc, argv, "")) != -1)
		switch (c) {
		default:
			usage();
		}
	argc -= optind;
	if (argc)
		usage();

	f = 0;
	CNT_ASSERT0(f = B1; psc_assert( pfl_bitstr_setchk(&f, NULL, B1, 0, 0, 0, 0)));
	CNT_ASSERT0(f = B1; psc_assert(!pfl_bitstr_setchk(&f, NULL, 0, B1, 0, 0, 0)));
	CNT_ASSERT0(f = B2; psc_assert( pfl_bitstr_setchk(&f, NULL, B2, B3, 0, 0, 0)));

	CNT_ASSERT0(psc_assert( pfl_bitstr_setchk(&f, NULL, 0, 0, B1, 0, PFL_BITSTR_SETCHK_STRICT));
		    psc_assert(!pfl_bitstr_setchk(&f, NULL, 0, 0, B1, 0, PFL_BITSTR_SETCHK_STRICT));
		    psc_assert(!pfl_bitstr_setchk(&f, NULL, B1|B2, 0, 0, 0, PFL_BITSTR_SETCHK_STRICT));
		    psc_assert( pfl_bitstr_setchk(&f, NULL, 0, 0, B2, 0, PFL_BITSTR_SETCHK_STRICT));
		    psc_assert( pfl_bitstr_setchk(&f, NULL, B1|B2, 0, 0, 0, PFL_BITSTR_SETCHK_STRICT));
		    psc_assert( pfl_bitstr_setchk(&f, NULL, B1, 0, B3, 0, PFL_BITSTR_SETCHK_STRICT));
		    psc_assert(f==(B1|B2|B3));
		    psc_assert( pfl_bitstr_setchk(&f, NULL, B3, 0, 0, B2, PFL_BITSTR_SETCHK_STRICT));
		    psc_assert(f==(B1|B3));
		    );

	CNT_ASSERT0(psc_assert(pfl_bitstr_setchk(&f, NULL, 0, 0, B6, 0, 0)); psc_assert(f == B6));

	v = 0xf;
	psc_assert(pfl_bitstr_nset(&v, sizeof(v)) == 4);
	v = 0xf00;
	psc_assert(pfl_bitstr_nset(&v, sizeof(v)) == 4);
	v = 0xf1f0;
	psc_assert(pfl_bitstr_nset(&v, 1) == 4);
	v = 0xffff;
	psc_assert(pfl_bitstr_nset(&v, 2) == 16);
	v = 0xffffffffffffffff;
	psc_assert(pfl_bitstr_nset(&v, sizeof(v)) == 64);

	out = malloc(4);
	in = malloc(4);

	memset(out, 0, 4);
	in[0] = 0xff; in[1] = 0xff;
	in[2] = 0xff; in[3] = 0x7f;
	pfl_bitstr_copy(out, 0, in, 0, NBBY * 4);
	psc_assert(out[0] == 0xff && out[1] == 0xff);
	psc_assert(out[2] == 0xff && out[3] == 0x7f);

	memset(out, 0, 4);
	in[0] = 0xff; in[1] = 0xff;
	in[2] = 0xff; in[3] = 0x7f;
	pfl_bitstr_copy(out, 0, in, 1, NBBY * 4);
	psc_assert(out[0] == 0xff && out[1] == 0xff);
	psc_assert(out[2] == 0xff && out[3] == 0x3f);

	memset(out, 0, 4);
	in[0] = 0xff; in[1] = 0xff;
	in[2] = 0xff; in[3] = 0x7f;
	pfl_bitstr_copy(out, 1, in, 0, NBBY * 4);
	psc_assert(out[0] == 0xfe && out[1] == 0xff);
	psc_assert(out[2] == 0xff && out[3] == 0xff);

	memset(out, 0, 4);
	in[0] = 0xff; in[1] = 0xff;
	in[2] = 0xff; in[3] = 0xff;
	pfl_bitstr_copy(out, 3, in, 10, 1);
	psc_assert(out[0] == 0x08 && out[1] == 0x00);
	psc_assert(out[2] == 0x00 && out[3] == 0x00);

	memset(out, 0, 4);
	in[0] = 0xff; in[1] = 0xff;
	in[2] = 0xff; in[3] = 0xff;
	pfl_bitstr_copy(out, 3, in, 10, 13);
	psc_assert(out[0] == 0xf8 && out[1] == 0xff);
	psc_assert(out[2] == 0x00 && out[3] == 0x00);

	exit(0);
}
