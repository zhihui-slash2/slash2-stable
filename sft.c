/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2006-2014, Pittsburgh Supercomputing Center (PSC).
 *
 * Permission to use, copy, and modify this software and its documentation
 * without fee for personal use or non-commercial use within your organization
 * is hereby granted, provided that the above copyright notice is preserved in
 * all copies and that the copyright and this permission notice appear in
 * supporting documentation.  Permission to redistribute this software to other
 * organizations or individuals is not permitted without the written permission
 * of the Pittsburgh Supercomputing Center.  PSC makes no representations about
 * the suitability of this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 * -----------------------------------------------------------------------------
 * %PSC_END_COPYRIGHT%
 */

#include <sys/param.h>
#include <sys/stat.h>

#include <ctype.h>
#include <err.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pfl/alloc.h"
#include "pfl/cdefs.h"
#include "pfl/crc.h"
#include "pfl/fmt.h"
#include "pfl/iostats.h"
#include "pfl/listcache.h"
#include "pfl/log.h"
#include "pfl/pfl.h"
#include "pfl/pool.h"
#include "pfl/str.h"
#include "pfl/thread.h"
#include "pfl/timerthr.h"
#include "pfl/types.h"
#include "pfl/walk.h"

#define THRT_TIOS		0	/* timed I/O stats */

struct wk {
	struct psc_listentry lentry;
	const char	*fn;
	int		 fd;
	int		 eof;
	int		 chunkid;
	size_t		 off;
};

#define NTHR_AUTO (-1)

int			 docrc;
int			 doread = 1;
int			 progress;
int			 chunk;
int			 checkzero;
int			 nthr = 1;
ssize_t			 bufsz = 128 * 1024;
psc_atomic64_t		 resid;
uint64_t		 filecrc;
off_t			 seekoff;

struct psc_poolmaster	 wk_poolmaster;
struct psc_poolmgr	*wk_pool;
struct psc_iostats	 ist;

const char		*progname;

struct psc_listcache	 wkq;
struct psc_listcache	 doneq;

void
thrmain(struct psc_thread *thr)
{
	int skip, lastfd = -2;
	struct wk *wk;
	uint64_t crc;
	ssize_t rc;
	char *buf;

	buf = PSCALLOC(bufsz);

	while (pscthr_run(thr)) {
		wk = lc_getwait(&wkq);
		if (wk == NULL)
			break;
		skip = lastfd == -1;
		lastfd = wk->fd;

		/* eof signal */
		if (wk->fd == -1) {
			if (skip)
				lc_add(&wkq, wk);
			else
				lc_add(&doneq, wk);
			continue;
		}

		if (doread)
			rc = pread(wk->fd, buf, bufsz, wk->off);
		else {
			rc = pwrite(wk->fd, buf, bufsz, wk->off);
			if (rc > 0 && rc != bufsz) {
				rc = -1;
				errno = EIO;
			}
		}
		if (rc == -1)
			err(1, "%s", wk->fn);

		psc_atomic64_add(&resid, rc);
		psc_iostats_intv_add(&ist, rc);

		if (docrc) {
			if (chunk) {
				psc_crc64_calc(&crc, buf, rc);

				flockfile(stdout);
				fprintf(stdout, "F '%s' %5d %c "
				    "CRC=%"PSCPRIxCRC64"\n",
				    wk->fn, wk->chunkid,
				    checkzero && pfl_memchk(buf, 0, rc) ?
				    'Z' : ' ', crc);
				funlockfile(stdout);
			} else
				psc_crc64_add(&filecrc, buf, rc);
		}

		psc_pool_return(wk_pool, wk);
	}
}

void
addwk(const char *fn, int fd, size_t off, int chunkid)
{
	struct wk *wk;

	wk = psc_pool_get(wk_pool);
	wk->fn = fn;
	wk->fd = fd;
	wk->off = off;
	wk->chunkid = chunkid;
	lc_add(&wkq, wk);
}

void
display(__unusedx struct psc_thread *thr)
{
	char ratebuf[PSCFMT_HUMAN_BUFSIZ];
	struct psc_iostats myist;
	int n, t;

	n = printf("%8s %7s", "time (s)", "rate");
	printf("\n");
	for (t = 0; t < n; t++)
		putchar('=');
	printf("\n");

	n = 0;
	for (;;) {
		sleep(1);
		memcpy(&myist, &ist, sizeof(myist));
		psc_fmt_human(ratebuf,
		    psc_iostats_getintvrate(&myist, 0));
		t = printf("\r%7.3fs %7s",
		    psc_iostats_getintvdur(&myist, 0),
		    ratebuf);
		n = MAX(n - t, 0);
		printf("%*.*s ", n, n, "");
		n = t;
		fflush(stdout);
	}
}

__dead void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [-BcKPZ] [-b bufsz] [-n nthr] [-O offset] file ...\n",
	    progname);
	exit(1);
}

int
nprocessors(void)
{
	int n = 0;
#ifdef HAVE_SCHED_GETAFFINITY
	cpu_set_t *mask = NULL;
	int np = 1024, rc, i;
	size_t size;

	for (;;) {
		mask = CPU_ALLOC(np);
		size = CPU_ALLOC_SIZE(np);
		CPU_ZERO_S(size, mask);
		rc = sched_getaffinity(0, size, mask);
		if (rc == 0)
			break;
		if (rc != EINVAL)
			err(1, "sched_getaffinity");

		rc = errno;
		CPU_FREE(mask);
		np = np << 2;
	}
	for (i = 0; i < np; i++)
		if (CPU_ISSET_S(i, size, mask))
			n++;
	CPU_FREE(mask);
#endif
	return (n);
}

int
proc(const char *fn,
    __unusedx const struct pfl_stat *pst, __unusedx int info,
    __unusedx int level, __unusedx void *arg)
{
	int fd, chunkid, n;
	struct stat stb;
	struct wk *wk;
	off_t off;

	fd = open(fn, O_RDONLY);
	if (fd == -1)
		err(1, "open %s", fn);
	if (fstat(fd, &stb) == -1)
		err(1, "stat %s", fn);

	psc_atomic64_set(&resid, seekoff);

	chunkid = 0;
	for (off = seekoff; off < stb.st_size; off += bufsz)
		addwk(fn, fd, off, chunkid++);

	for (n = 0; n < nthr; n++)
		addwk(NULL, -1, 0, 0);

	for (n = 0; n < nthr && (wk = lc_getwait(&doneq)); n++)
		psc_pool_return(wk_pool, wk);

	if (doread && psc_atomic64_read(&resid) < stb.st_size)
		warn("premature EOF");

	if (docrc && !chunk) {
		PSC_CRC64_FIN(&filecrc);
		fprintf(stdout, "F '%s' CRC=%"PSCPRIxCRC64"\n",
		    fn, filecrc);
	}
	close(fd);

	return (0);
}

int
main(int argc, char *argv[])
{
	int c, n, flags = 0;
	char *endp;

	pfl_init();
	progname = argv[0];
	while ((c = getopt(argc, argv, "Bb:cKn:O:PRvZ")) != -1)
		switch (c) {
		case 'B': /* display bandwidth */
			pscthr_init(0, 0, display, NULL, 0, "disp");
			break;
		case 'b': /* I/O block size */
			bufsz = pfl_humantonum(optarg);
			if (bufsz <= 0)
				errx(1, "%s: %s", optarg, strerror(
				    bufsz ? -bufsz : EINVAL));
			break;
		case 'c': /* perform CRC of entire file */
			docrc = 1;
			PSC_CRC64_INIT(&filecrc);
			break;
		case 'K': /* report checksum of each file chunk */
			chunk = 1;
			break;
		case 'n': /* #threads */
			if (strcmp(optarg, "a") == 0)
				nthr = nprocessors();
			else {
				nthr = strtol(optarg, &endp, 10);
				/* XXX check */
			}
			break;
		case 'O': /* offset */
			seekoff = pfl_humantonum(optarg);
			if (seekoff < 0)
				errx(1, "%s: %s", optarg,
				    strerror(-seekoff));
			break;
		case 'R': /* recursive */
			flags |= PFL_FILEWALKF_RECURSIVE;
			break;
		case 'P': /* chart progress */
			progress = 1;
			break;
		case 'w': /* write */
			doread = 0;
			break;
		case 'v': /* verbose */
			flags |= PFL_FILEWALKF_VERBOSE;
			break;
		case 'Z': /* report if file chunk is all zeroes */
			checkzero = 1;
			break;
		default:
			usage();
		}
	argc -= optind;
	argv += optind;
	if (argc == 0)
		usage();

	if (nthr && docrc && !chunk)
		errx(1, "cannot parallelize filewide CRC");

	lc_init(&wkq, struct wk, lentry);
	lc_init(&doneq, struct wk, lentry);
	psc_poolmaster_init(&wk_poolmaster, struct wk, lentry,
	    0, nthr, nthr, 0, NULL, NULL, NULL, "wk");
	wk_pool = psc_poolmaster_getmgr(&wk_poolmaster);

	psc_tiosthr_spawn(THRT_TIOS, "tiosthr");
	psc_iostats_init(&ist, "ist");

	for (n = 0; n < nthr; n++)
		pscthr_init(0, 0, thrmain, NULL, 0, "thr%d", n);

	for (; *argv; argv++)
		pfl_filewalk(*argv, flags, NULL, proc, NULL);

	return (0);
}
