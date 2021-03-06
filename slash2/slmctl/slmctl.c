/* $Id$ */
/*
 * %GPL_START_LICENSE%
 * ---------------------------------------------------------------------
 * Copyright 2015, Google, Inc.
 * Copyright (c) 2007-2015, Pittsburgh Supercomputing Center (PSC).
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed WITHOUT ANY WARRANTY; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License contained in the file
 * `COPYING-GPL' at the top of this distribution or at
 * https://www.gnu.org/licenses/gpl-2.0.html for more details.
 * ---------------------------------------------------------------------
 * %END_LICENSE%
 */

#include <curses.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <term.h>
#include <unistd.h>

#include "pfl/pfl.h"
#include "pfl/cdefs.h"
#include "pfl/ctl.h"
#include "pfl/ctlcli.h"
#include "pfl/fmt.h"
#include "pfl/log.h"

#include "ctl.h"
#include "ctlcli.h"
#include "pathnames.h"
#include "slashrpc.h"

#include "slashd/bmap_mds.h"
#include "slashd/ctl_mds.h"
#include "slashd/repl_mds.h"

void
slmrmcthr_pr(const struct psc_ctlmsg_thread *pcst)
{
	printf(" #open %8d #close %8d #stat %8d",
	    pcst->pcst_nopen, pcst->pcst_nclose, pcst->pcst_nstat);
}

void
slmrmmthr_pr(const struct psc_ctlmsg_thread *pcst)
{
	printf(" #open %8d", pcst->pcst_nopen);
}

void
packshow_bmaps(__unusedx char *bmaps)
{
	psc_ctlmsg_push(SLMCMT_GETBMAP, sizeof(struct slctlmsg_bmap));
}

void
packshow_conns(__unusedx char *conn)
{
	psc_ctlmsg_push(SLMCMT_GETCONNS, sizeof(struct slctlmsg_conn));
}

void
packshow_fcmhs(char *fid)
{
	struct slctlmsg_fcmh *scf;

	scf = psc_ctlmsg_push(SLMCMT_GETFCMHS, sizeof(*scf));
	scf->scf_fg.fg_fid = FID_ANY;

	if (fid) {
		if (strcmp(fid, "busy") == 0)
			scf->scf_fg.fg_gen = SLCTL_FCL_BUSY;
		else
			psc_fatalx("unrecognized fidcache class: %s",
			    fid);
	}
}

void
packshow_replqueued(char *res)
{
	struct slmctlmsg_replqueued *scrq;

	scrq = psc_ctlmsg_push(SLMCMT_GETREPLQUEUED, sizeof(*scrq));
	if (res && strcasecmp(res, "busy") == 0)
		strlcpy(scrq->scrq_resname, SLMC_REPLQ_BUSY,
		    sizeof(scrq->scrq_resname));
}

void
packshow_statfs(__unusedx char *s)
{
	psc_ctlmsg_push(SLMCMT_GETSTATFS,
	    sizeof(struct slmctlmsg_statfs));
}

void
packshow_bml(__unusedx char *s)
{
	psc_ctlmsg_push(SLMCMT_GETBML, sizeof(struct slmctlmsg_bml));
}

void
slm_replqueued_prhdr(__unusedx struct psc_ctlmsghdr *mh,
    __unusedx const void *m)
{
	printf("%-32s %7s %7s %7s %7s %7s %7s\n",
	    "resource", "in-q", "in-bw", "out-q", "out-bw", "aggr-q",
	    "aggr-bw");
}

void
slm_replqueued_prdat(__unusedx const struct psc_ctlmsghdr *mh,
    const void *m)
{
	const struct slmctlmsg_replqueued *scrq = m;

	printf("%-32s ", scrq->scrq_resname);
	psc_ctl_prnumber(0, scrq->scrq_ingress_queued * BW_UNITSZ, 0, " ");
	psc_ctl_prnumber(0, scrq->scrq_ingress_assigned * BW_UNITSZ, 0, " ");
	psc_ctl_prnumber(0, scrq->scrq_egress_queued * BW_UNITSZ, 0, " ");
	psc_ctl_prnumber(0, scrq->scrq_egress_assigned * BW_UNITSZ, 0, " ");
	psc_ctl_prnumber(0, scrq->scrq_aggr_queued * BW_UNITSZ, 0, " ");
	psc_ctl_prnumber(0, scrq->scrq_aggr_assigned * BW_UNITSZ, 0, "\n");
}

void
slm_statfs_prhdr(__unusedx struct psc_ctlmsghdr *mh,
    __unusedx const void *m)
{
	printf("%-27s %2s %8s %7s %7s %6s %-17s\n",
	    "resource", "fl", "capacity", "used", "remain", "utiliz", "type");
}

void
slm_statfs_prdat(__unusedx const struct psc_ctlmsghdr *mh, const void *m)
{
	struct class {
		unsigned frac;
		int	 col;
	} *c, classes[] = {
		{ 85, COLOR_RED },
		{ 60, COLOR_YELLOW },
		{  0, COLOR_GREEN }
	};
	const struct slmctlmsg_statfs *scsf = m;
	const struct srt_statfs *b = &scsf->scsf_ssfb;
	char cbuf[PSCFMT_RATIO_BUFSIZ];
	char name[RES_NAME_MAX];
	int j, col = 0;

	if (b->sf_blocks)
		for (c = classes, j = 0; j < nitems(classes); j++, c++)
			if (100 * (b->sf_blocks - b->sf_bavail) /
			     b->sf_blocks >= c->frac) {
				col = c->col;
				break;
			}
	strlcpy(name, scsf->scsf_resname, sizeof(name));
	printf("%-27s %c%c ", name,
	    scsf->scsf_flags & (SIF_DISABLE_LEASE |
	      SIF_DISABLE_ADVLEASE)		? 'W' : '-',
	    scsf->scsf_flags & SIF_DISABLE_GC   ? 'G' : '-');
	printf(" ");
	/*
	 * The following uses the formula from df.c in GNU coreutils.
	 * However, we don't do integer arithmetic.
	 */
	b->sf_blocks ? psc_ctl_prnumber(0, b->sf_blocks * b->sf_frsize,
	    0, "") : printf("%7s", "-");
	printf(" ");
	b->sf_blocks ? psc_ctl_prnumber(0, (b->sf_blocks - b->sf_bfree) *
	    b->sf_frsize, 0, "") : printf("%7s", "-");
	printf(" ");
	b->sf_blocks ? psc_ctl_prnumber(0, b->sf_bavail * b->sf_frsize,
	    0, "") : printf("%7s", "-");
	printf(" ");

	setcolor(col);
	if (b->sf_blocks)
		psc_fmt_ratio(cbuf, b->sf_blocks - b->sf_bfree,
		    b->sf_blocks - b->sf_bfree + b->sf_bavail);
	else
		strlcpy(cbuf, "-", sizeof(cbuf));
	printf("%6s", cbuf);
	uncolor();
	printf(" %-17s\n", b->sf_type);
}

void
slmctlcmd_stop(int ac, __unusedx char *av[])
{
	if (ac > 1)
		errx(1, "stop: unknown arguments");
	psc_ctlmsg_push(SLMCMT_STOP, 0);
}

void
slmctlcmd_upsch_query(int ac, char *av[])
{
	struct slmctlmsg_upsch_query *scuq;
	size_t len;

	if (ac != 1)
		errx(1, "upsch_query: no query specified");
	len = strlen(av[0]) + 1;
	scuq = psc_ctlmsg_push(SLMCMT_UPSCH_QUERY, sizeof(*scuq) + len);
	strlcpy(scuq->scuq_query, av[0], len);
}

void
slm_bml_prhdr(__unusedx struct psc_ctlmsghdr *mh,
    __unusedx const void *m)
{
	printf("%-16s %6s %-16s "
	    "%-15s %11s %9s\n",
	    "bmap-lease-fid", "bmapno", "io-system",
	    "client", "flags", "seqno");
}

void
slm_bml_prdat(__unusedx const struct psc_ctlmsghdr *mh, const void *m)
{
	const struct slmctlmsg_bml *scbl = m;
	char *p, cli[PSCRPC_NIDSTR_SIZE];
	size_t n;

	strlcpy(cli, scbl->scbl_client, sizeof(cli));
	p = strchr(cli, '@');
	if (p)
		*p = '\0';
	p = cli;
	n = strcspn(p, "-");
	if (n)
		p += n + 1;

	printf("%016"SLPRIxFID" %6u "
	    "%-16.16s %-15s "
	    "%c%c%c%c%c%c%c%c%c%c%c "
	    "%9"PRIu64"\n",
	    scbl->scbl_fg.fg_fid, scbl->scbl_bno,
	    scbl->scbl_resname, p,
	    scbl->scbl_flags & BML_READ		? 'R' : '-',
	    scbl->scbl_flags & BML_WRITE	? 'W' : '-',
	    scbl->scbl_flags & BML_DIO		? 'D' : '-',
	    scbl->scbl_flags & BML_DIOCB	? 'C' : '-',
	    scbl->scbl_flags & BML_TIMEOQ	? 'T' : '-',
	    scbl->scbl_flags & BML_BMI		? 'B' : '-',
	    scbl->scbl_flags & BML_RECOVER	? 'V' : '-',
	    scbl->scbl_flags & BML_CHAIN	? 'N' : '-',
	    scbl->scbl_flags & BML_FREEING	? 'F' : '-',
	    scbl->scbl_flags & BML_ASSFAIL	? 'S' : '-',
	    scbl->scbl_flags & BML_RECOVERFAIL	? 'L' : '-',
	    scbl->scbl_seq);
}

struct psc_ctlshow_ent psc_ctlshow_tab[] = {
	PSC_CTLSHOW_DEFS,
	{ "bmaps",		packshow_bmaps },
	{ "bml",		packshow_bml },
	{ "connections",	packshow_conns },
	{ "fcmhs",		packshow_fcmhs },
	{ "replqueued",		packshow_replqueued },
	{ "statfs",		packshow_statfs },

	/* aliases */
	{ "conns",		packshow_conns },
	{ "fidcache",		packshow_fcmhs },
	{ "files",		packshow_fcmhs }
};

struct psc_ctlmsg_prfmt psc_ctlmsg_prfmts[] = {
	PSC_CTLMSG_PRFMT_DEFS,
	{ sl_bmap_prhdr,	sl_bmap_prdat,		sizeof(struct slctlmsg_bmap),		NULL },
	{ sl_conn_prhdr,	sl_conn_prdat,		sizeof(struct slctlmsg_conn),		NULL },
	{ sl_fcmh_prhdr,	sl_fcmh_prdat,		sizeof(struct slctlmsg_fcmh),		NULL },
	{ slm_replqueued_prhdr,	slm_replqueued_prdat,	sizeof(struct slmctlmsg_replqueued),	NULL },
	{ slm_statfs_prhdr,	slm_statfs_prdat,	sizeof(struct slmctlmsg_statfs),	NULL },
	{ NULL,			NULL,			0,					NULL },
	{ slm_bml_prhdr,	slm_bml_prdat,		sizeof(struct slmctlmsg_bml),		NULL }
};

psc_ctl_prthr_t psc_ctl_prthrs[] = {
/* BMAPTIMEO	*/ NULL,
/* COH		*/ NULL,
/* CONN		*/ NULL,
/* CTL		*/ psc_ctlthr_pr,
/* CTLAC	*/ psc_ctlacthr_pr,
/* CURSOR	*/ NULL,
/* JNAMESPACE	*/ NULL,
/* JRECLAIM	*/ NULL,
/* JRNL		*/ NULL,
/* LNETAC	*/ NULL,
/* NBRQ		*/ NULL,
/* RCM		*/ NULL,
/* RMC		*/ slmrmcthr_pr,
/* RMI		*/ NULL,
/* RMM		*/ slmrmmthr_pr,
/* OPSTIMER	*/ NULL,
/* UPSCHED	*/ NULL,
/* USKLNDPL	*/ NULL,
/* WORKER	*/ NULL,
/* ZFS_KSTAT	*/ NULL
};

struct psc_ctlcmd_req psc_ctlcmd_reqs[] = {
	{ "stop",		slmctlcmd_stop },
	{ "upsch-query",	slmctlcmd_upsch_query }
};

PFLCTL_CLI_DEFS;

const char *progname;
const char *daemon_name = "slashd";

__dead void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [-HIn] [-p paramspec] [-S socket] [-s value] "
	    "[cmd arg ...]\n",
	    progname);
	exit(1);
}

struct psc_ctlopt opts[] = {
	{ 'H', PCOF_FLAG, &psc_ctl_noheader },
	{ 'I', PCOF_FLAG, &psc_ctl_inhuman },
	{ 'n', PCOF_FLAG, &psc_ctl_nodns },
	{ 'p', PCOF_FUNC, psc_ctlparse_param },
	{ 's', PCOF_FUNC, psc_ctlparse_show }
};

int
main(int argc, char *argv[])
{
	pfl_init();
	progname = argv[0];

	psc_ctlcli_main(SL_PATH_SLMCTLSOCK, argc, argv, opts,
	    nitems(opts));
	exit(0);
}
