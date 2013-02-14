/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2007-2012, Pittsburgh Supercomputing Center (PSC).
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

#include <curses.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <term.h>
#include <unistd.h>

#include "pfl/pfl.h"
#include "pfl/cdefs.h"
#include "psc_util/ctl.h"
#include "psc_util/ctlcli.h"
#include "psc_util/fmt.h"
#include "psc_util/log.h"

#include "ctl.h"
#include "ctlcli.h"
#include "pathnames.h"
#include "slashrpc.h"

#include "slashd/bmap_mds.h"
#include "slashd/ctl_mds.h"
#include "slashd/repl_mds.h"

int				 has_col;

void
setcolor(int col)
{
	if (!has_col || col == -1)
		return;
	putp(tparm(enter_bold_mode));
	putp(tparm(set_a_foreground, col));
}

void
uncolor(void)
{
	if (!has_col)
		return;
	putp(tparm(orig_pair));
	putp(tparm(exit_attribute_mode));
}

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
packshow_conns(__unusedx char *conn)
{
	psc_ctlmsg_push(SLMCMT_GETCONNS, sizeof(struct slctlmsg_conn));
}

void
packshow_fcmhs(__unusedx char *fid)
{
	struct slctlmsg_fcmh *scf;

	scf = psc_ctlmsg_push(SLMCMT_GETFCMHS, sizeof(struct slctlmsg_fcmh));
	scf->scf_fg.fg_fid = FID_ANY;
}

void
packshow_replpairs(__unusedx char *pair)
{
	psc_ctlmsg_push(SLMCMT_GETREPLPAIRS,
	    sizeof(struct slmctlmsg_replpair));
}

void
packshow_statfs(__unusedx char *pair)
{
	psc_ctlmsg_push(SLMCMT_GETSTATFS,
	    sizeof(struct slmctlmsg_statfs));
}

void
packshow_bml(__unusedx char *pair)
{
	psc_ctlmsg_push(SLMCMT_GETBMAP, sizeof(struct slctlmsg_bmap));
}

void
slm_replpair_prhdr(__unusedx struct psc_ctlmsghdr *mh,
    __unusedx const void *m)
{
	printf("%-28s %-28s  %10s %10s\n",
	    "repl-resm-A", "repl-resm-B", "used", "avail");
}

void
slm_replpair_prdat(__unusedx const struct psc_ctlmsghdr *mh,
    const void *m)
{
	const struct slmctlmsg_replpair *scrp = m;
	char abuf[PSCFMT_HUMAN_BUFSIZ], ubuf[PSCFMT_HUMAN_BUFSIZ];
	char *p, addr[2][RESM_ADDRBUF_SZ];
	int i, j;

	for (i = 0; i < 2; i++) {
		strlcpy(addr[i], scrp->scrp_addrbuf[i],
		    sizeof(addr[i]));
		p = addr[i];
		for (j = 0; j < 2 && p; j++)
			p = strchr(p + 1, '@');
		if (p)
			*p = '\0';
	}

	psc_fmt_human(ubuf, scrp->scrp_used);
	psc_fmt_human(abuf, scrp->scrp_avail);
	printf("%-28s %-28s  %8s/s %8s/s\n", addr[0], addr[1], ubuf,
	    abuf);
}

void
slm_statfs_prhdr(__unusedx struct psc_ctlmsghdr *mh,
    __unusedx const void *m)
{
	printf("%-27s %2s %7s %7s %7s %8s %-16s\n",
	    "resource", "fl", "size", "used", "avail", "capacity", "type");
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
	char *p, name[RES_NAME_MAX];
	int j, col = 0;

	if (b->sf_blocks)
		for (c = classes, j = 0; j < nitems(classes); j++, c++)
			if (100 * (b->sf_blocks - b->sf_bavail) /
			     b->sf_blocks >= c->frac) {
				col = c->col;
				break;
			}
	strlcpy(name, scsf->scsf_resname, sizeof(name));
	p = strchr(name, '@');
	if (p)
		*p = '\0';
	printf("%-27s %c%c ", name,
	    scsf->scsf_flags & SIF_DISABLE_BIA ? 'W' : '-',
	    scsf->scsf_flags & SIF_DISABLE_GC  ? 'G' : '-');
	b->sf_blocks ? psc_ctl_prhuman(b->sf_blocks * b->sf_bsize) :
	    printf("%7s", "-");
	printf(" ");
	b->sf_blocks ? psc_ctl_prhuman((b->sf_blocks - b->sf_bfree) *
	    b->sf_bsize) : printf("%7s", "-");
	printf(" ");
	b->sf_blocks ? psc_ctl_prhuman(b->sf_bavail * b->sf_bsize) :
	    printf("%7s", "-");

	if (col)
		setcolor(col);
	if (b->sf_blocks)
		psc_fmt_ratio(cbuf, b->sf_blocks -
		    (int64_t)b->sf_bavail, b->sf_blocks);
	else
		strlcpy(cbuf, "-", sizeof(cbuf));
	printf("%8s", cbuf);
	if (col)
		uncolor();
	printf(" %-16s\n", b->sf_type);
}

void
slmctlcmd_stop(int ac, __unusedx char *av[])
{
	if (ac > 1)
		errx(1, "stop: unknown arguments");
	psc_ctlmsg_push(SLMCMT_STOP, 0);
}

struct psc_ctlshow_ent psc_ctlshow_tab[] = {
	PSC_CTLSHOW_DEFS,
	{ "bml",		packshow_bml },
	{ "connections",	packshow_conns },
	{ "fcmhs",		packshow_fcmhs },
	{ "replpairs",		packshow_replpairs },
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
	{ slm_replpair_prhdr,	slm_replpair_prdat,	sizeof(struct slmctlmsg_replpair),	NULL },
	{ slm_statfs_prhdr,	slm_statfs_prdat,	sizeof(struct slmctlmsg_statfs),	NULL },
	{ NULL,			NULL,			0,					NULL }
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
/* TIOS		*/ NULL,
/* UPSCHED	*/ NULL,
/* USKLNDPL	*/ NULL,
/* WORKER	*/ NULL,
/* ZFS_KSTAT	*/ NULL
};

struct psc_ctlcmd_req psc_ctlcmd_reqs[] = {
	{ "stop",	slmctlcmd_stop }
};

PFLCTL_CLI_DEFS;

const char *progname;
const char *daemon_name = "slashd";

__dead void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [-HIn] [-p paramspec] [-S socket] [-s value] [cmd arg ...]\n",
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
	int rc;

	pfl_init();
	progname = argv[0];

	setupterm(NULL, STDOUT_FILENO, &rc);
	start_color();
	has_col = isatty(STDOUT_FILENO) && has_colors();

	psc_ctlcli_main(SL_PATH_SLMCTLSOCK, argc, argv, opts,
	    nitems(opts));
	exit(0);
}
