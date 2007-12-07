/* $Id$ */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "myrandom.h"
#include "cdefs.h"

const char *progname;

__dead void
usage(void)
{
	fprintf(stderr, "usage: %s\n", progname);
	exit(1);
}

int
main(int argc, char *argv[])
{
	struct vbitmap *vb;
	size_t elem;
	int i, c;

	progname = argv[0];
	while ((c = getopt(argc, argv, "")) != -1)
		switch (c) {
		default:
			usage();
		}
	argc -= optind;
	if (argc)
		usage();

	printf("%u\n", myrandom32());
	printf("%016"ZLPX64"\n", myrandom64());
	exit(0);
}
