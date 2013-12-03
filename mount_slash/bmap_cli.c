/* $Id$ */
/*
 * %PSCGPL_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2008-2013, Pittsburgh Supercomputing Center (PSC).
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
 *
 * Pittsburgh Supercomputing Center	phone: 412.268.4960  fax: 412.268.5832
 * 300 S. Craig Street			e-mail: remarks@psc.edu
 * Pittsburgh, PA 15213			web: http://www.psc.edu/
 * -----------------------------------------------------------------------------
 * %PSC_END_COPYRIGHT%
 */

#define PSC_SUBSYS SLSS_BMAP
#include "slsubsys.h"

#include <stddef.h>

#include "pfl/rpc.h"
#include "pfl/ctlsvr.h"
#include "pfl/iostats.h"
#include "pfl/random.h"

#include "slconfig.h"
#include "bmap_cli.h"
#include "pgcache.h"
#include "fidc_cli.h"
#include "mount_slash.h"
#include "rpc_cli.h"
#include "slashrpc.h"
#include "slerr.h"

extern psc_spinlock_t		bmapTimeoutLock;
extern struct psc_waitq		bmapTimeoutWaitq;

/*
 * msl_bmap_free(): avoid ENOMEM and clean up TOFREE bmap to avoid stalls.
 */
void
msl_bmap_free(void)
{
	while (lc_nitems(&bmapTimeoutQ) > BMAP_CACHE_MAX) { 
		spinlock(&bmapTimeoutLock);
		psc_waitq_wakeall(&bmapTimeoutWaitq);
		freelock(&bmapTimeoutLock);
		OPSTAT_INCR(SLC_OPST_BMAP_ALLOC_STALL);
		sleep(1);
	}
}

/**
 * msl_bmap_init - Initialize CLI-specific data of a bmap structure.
 * @b: the bmap struct
 */
void
msl_bmap_init(struct bmap *b)
{
	struct bmap_cli_info *bci;

	bci = bmap_2_bci(b);
	bmpc_init(&bci->bci_bmpc);

	INIT_PSC_LISTENTRY(&bci->bci_lentry);
}

/**
 * msl_bmap_modeset - Set READ or WRITE as access mode on an open file
 *	block map.
 * @b: bmap.
 * @rw: access mode to set the bmap to.
 */
__static int
msl_bmap_modeset(struct bmap *b, enum rw rw, __unusedx int flags)
{
	struct slashrpc_cservice *csvc = NULL;
	struct pscrpc_request *rq = NULL;
	struct srm_bmap_chwrmode_req *mq;
	struct srm_bmap_chwrmode_rep *mp;
	struct fcmh_cli_info *fci;
	struct fidc_membh *f;
	struct sl_resource *r;
	int rc, nretries = 0;

	f = b->bcm_fcmh;
	fci = fcmh_2_fci(f);

	psc_assert(rw == SL_WRITE || rw == SL_READ);
 retry:
	psc_assert(b->bcm_flags & BMAP_MDCHNG);

	if (b->bcm_flags & BMAP_WR)
		/*
		 * Write enabled bmaps are allowed to read with no
		 * further action being taken.
		 */
		return (0);

	/* Add write mode to this bmap. */
	psc_assert(rw == SL_WRITE && (b->bcm_flags & BMAP_RD));

	rc = slc_rmc_getcsvc1(&csvc, fci->fci_resm);
	if (rc)
		goto out;

	rc = SL_RSX_NEWREQ(csvc, SRMT_BMAPCHWRMODE, rq, mq, mp);
	if (rc)
		goto out;

	memcpy(&mq->sbd, bmap_2_sbd(b), sizeof(struct srt_bmapdesc));
	mq->prefios[0] = prefIOS;
	rc = SL_RSX_WAITREP(csvc, rq, mp);
	if (rc == 0)
		rc = mp->rc;

	if (rc == 0)
		memcpy(bmap_2_sbd(b), &mp->sbd,
		    sizeof(struct srt_bmapdesc));
	else
		goto out;

	r = libsl_id2res(bmap_2_sbd(b)->sbd_ios);
	psc_assert(r);
	if (r->res_type == SLREST_ARCHIVAL_FS) {
		/*
		 * Prepare for archival write by ensuring that all
		 * subsequent IO's are direct.
		 */
		BMAP_LOCK(b);
		b->bcm_flags |= BMAP_DIO;
		BMAP_ULOCK(b);

		msl_bmap_cache_rls(b);
	}

 out:
	if (rq) {
		pscrpc_req_finished(rq);
		rq = NULL;
	}
	if (csvc) {
		sl_csvc_decref(csvc);
		csvc = NULL;
	}

	if (rc == -SLERR_BMAP_DIOWAIT) {
		DEBUG_BMAP(PLL_WARN, b, "SLERR_BMAP_DIOWAIT rt=%d",
		    nretries);
		nretries++;
		/*
		 * XXX need some sort of randomizer here so that many
		 * clients do not flood mds.
		 */
		usleep(10000 * (nretries * nretries));
		goto retry;
	}

	return (rc);
}

__static int
msl_bmap_lease_reassign_cb(struct pscrpc_request *rq,
    struct pscrpc_async_args *args)
{
	struct bmap *b = args->pointer_arg[MSL_CBARG_BMAP];
	struct slashrpc_cservice *csvc = args->pointer_arg[MSL_CBARG_CSVC];
	struct srm_reassignbmap_rep *mp =
	    pscrpc_msg_buf(rq->rq_repmsg, 0, sizeof(*mp));
	struct bmap_cli_info  *bci = bmap_2_bci(b);
	int rc;

	psc_assert(&rq->rq_async_args == args);

	BMAP_LOCK(b);
	psc_assert(b->bcm_flags & BMAP_CLI_REASSIGNREQ);

	SL_GET_RQ_STATUS(csvc, rq, mp, rc);
	if (rc) {
		/*
		 * If the MDS replies with SLERR_ION_OFFLINE then don't
		 * bother with further retry attempts.
		 */
		if (rc == -SLERR_ION_OFFLINE)
			bmap_2_bci(b)->bci_nreassigns = SLERR_ION_OFFLINE;
		OPSTAT_INCR(SLC_OPST_BMAP_REASSIGN_FAIL);
	} else {

		memcpy(&bmap_2_bci(b)->bci_sbd, &mp->sbd,
		    sizeof(struct srt_bmapdesc));

		PFL_GETTIMESPEC(&bmap_2_bci(b)->bci_etime);
		timespecadd(&bmap_2_bci(b)->bci_etime, &msl_bmap_max_lease,
		    &bmap_2_bci(b)->bci_etime);
		OPSTAT_INCR(SLC_OPST_BMAP_REASSIGN_DONE);
	}

	b->bcm_flags &= ~BMAP_CLI_REASSIGNREQ;

	DEBUG_BMAP(rc ? PLL_ERROR : PLL_INFO, b,
	    "lease reassign: rc=%d, nseq=%"PRId64", "
	    "etime="PSCPRI_TIMESPEC"", rc, bci->bci_sbd.sbd_seq,
	    PFLPRI_PTIMESPEC_ARGS(&bci->bci_etime));

	bmap_op_done_type(b, BMAP_OPCNT_REASSIGN);

	sl_csvc_decref(csvc);

	return (rc);
}

__static int
msl_bmap_lease_tryext_cb(struct pscrpc_request *rq,
    struct pscrpc_async_args *args)
{
	struct bmap *b = args->pointer_arg[MSL_CBARG_BMAP];
	struct slashrpc_cservice *csvc = args->pointer_arg[MSL_CBARG_CSVC];
	struct srm_leasebmapext_rep *mp =
	    pscrpc_msg_buf(rq->rq_repmsg, 0, sizeof(*mp));
	int rc;
	struct timespec ts;
	struct bmap_cli_info  *bci = bmap_2_bci(b);

	BMAP_LOCK(b);
	psc_assert(b->bcm_flags & BMAP_CLI_LEASEEXTREQ);

	PFL_GETTIMESPEC(&ts);
	SL_GET_RQ_STATUS(csvc, rq, mp, rc);
	if (!rc) {
		memcpy(&bmap_2_bci(b)->bci_sbd, &mp->sbd,
		    sizeof(struct srt_bmapdesc));

		timespecadd(&ts,
		    &msl_bmap_max_lease, &bmap_2_bci(b)->bci_etime);

		OPSTAT_INCR(SLC_OPST_BMAP_LEASE_EXT_DONE);
	} else {
		/*
		 * Unflushed data in this bmap is now invalid.  Move the
		 * bmap out of the fid cache so that others don't
		 * stumble across it while its active I/O's are failed.
		 */
		psc_assert(!(b->bcm_flags & BMAP_CLI_LEASEFAILED));
		bci->bci_etime = ts;
		bmap_2_bci(b)->bci_error = rc;
		b->bcm_flags |= BMAP_CLI_LEASEFAILED;
		OPSTAT_INCR(SLC_OPST_BMAP_LEASE_EXT_FAIL);
	}

	b->bcm_flags &= ~BMAP_CLI_LEASEEXTREQ;

	DEBUG_BMAP(rc ? PLL_ERROR : PLL_INFO, b,
	    "lease extension: rc=%d, nseq=%"PRId64", "
	    "etime="PSCPRI_TIMESPEC"", rc, bci->bci_sbd.sbd_seq,
	    PFLPRI_PTIMESPEC_ARGS(&bci->bci_etime));

	bmap_op_done_type(b, BMAP_OPCNT_LEASEEXT);

	sl_csvc_decref(csvc);

	return (rc);
}

int
msl_bmap_lease_secs_remaining(struct bmap *b)
{
	struct timespec ts;
	int secs;

	BMAP_LOCK(b);
	PFL_GETTIMESPEC(&ts);
	secs = bmap_2_bci(b)->bci_etime.tv_sec - ts.tv_sec;
	BMAP_ULOCK(b);

	return (secs);
}

void
msl_bmap_lease_tryreassign(struct bmap *b)
{
	struct bmap_pagecache *bmpc = bmap_2_bmpc(b);
	struct bmap_cli_info  *bci  = bmap_2_bci(b);
	struct slashrpc_cservice *csvc = NULL;
	struct pscrpc_request *rq = NULL;
	struct srm_reassignbmap_req *mq;
	struct srm_reassignbmap_rep *mp;
	int rc;

	BMAP_LOCK(b);

	/*
	 * For lease reassignment to take place we must have the full
	 * complement of biorq's still in the cache.
	 *
	 * Additionally, no biorqs may be on the wire since those could
	 * be committed by the sliod.
	 */
	if ((b->bcm_flags & BMAP_CLI_REASSIGNREQ) ||
	    SPLAY_EMPTY(&bmpc->bmpc_new_biorqs)   ||
	    !pll_empty(&bmpc->bmpc_pndg_biorqs)   ||
	    bci->bci_nreassigns >= SL_MAX_IOSREASSIGN) {
		BMAP_ULOCK(b);
		OPSTAT_INCR(SLC_OPST_BMAP_REASSIGN_BAIL);
		return;
	}

	bci->bci_prev_sliods[bci->bci_nreassigns] =
	    bci->bci_sbd.sbd_ios;
	bci->bci_nreassigns++;

	b->bcm_flags |= BMAP_CLI_REASSIGNREQ;

	DEBUG_BMAP(PLL_WARN, b, "reassign from ios=%u "
	    "(nreassigns=%d)", bci->bci_sbd.sbd_ios,
	    bci->bci_nreassigns);

	bmap_op_start_type(b, BMAP_OPCNT_REASSIGN);

	BMAP_ULOCK(b);

	rc = slc_rmc_getcsvc1(&csvc, fcmh_2_fci(b->bcm_fcmh)->fci_resm);
	if (rc)
		goto out;

	rc = SL_RSX_NEWREQ(csvc, SRMT_REASSIGNBMAPLS, rq, mq, mp);
	if (rc)
		goto out;

	memcpy(&mq->sbd, &bci->bci_sbd, sizeof(struct srt_bmapdesc));
	memcpy(&mq->prev_sliods, &bci->bci_prev_sliods,
	    sizeof(sl_ios_id_t) * (bci->bci_nreassigns + 1));
	mq->nreassigns = bci->bci_nreassigns;
	mq->pios = prefIOS;

	authbuf_sign(rq, PSCRPC_MSG_REQUEST);

	rq->rq_async_args.pointer_arg[MSL_CBARG_BMAP] = b;
	rq->rq_async_args.pointer_arg[MSL_CBARG_CSVC] = csvc;
	rq->rq_interpret_reply = msl_bmap_lease_reassign_cb;
	pscrpc_req_setcompl(rq, &rpcComp);

	rc = pscrpc_nbreqset_add(pndgBmaplsReqs, rq);
	if (!rc)
		OPSTAT_INCR(SLC_OPST_BMAP_REASSIGN_SEND);
 out:
	DEBUG_BMAP(rc ? PLL_ERROR : PLL_DIAG, b,
	    "lease reassign req (rc=%d)", rc);
	if (rc) {
		BMAP_LOCK(b);
		b->bcm_flags &= ~BMAP_CLI_REASSIGNREQ;
		bmap_op_done_type(b, BMAP_OPCNT_REASSIGN);

		if (rq)
			pscrpc_req_finished(rq);
		if (csvc)
			sl_csvc_decref(csvc);
		OPSTAT_INCR(SLC_OPST_BMAP_REASSIGN_ABRT);
	}
}

/**
 * msl_bmap_lease_tryext - Attempt to extend the lease time on a bmap.
 *	If successful, this will result in the creation and assignment
 *	of a new lease sequence number from the MDS.
 * @blockable:  means the caller will not block if a renew RPC is
 *	outstanding.  Currently, only fsthreads which try lease
 *	extension prior to initiating I/O are 'blockable'.  This is so
 *	the system doesn't take more work on bmaps whose leases are
 *	about to expire.
 * Notes: should the lease extension fail, all dirty write buffers must
 *	be expelled and the flush error code should be set to notify the
 *	holders of open file descriptors.
 */
int
msl_bmap_lease_tryext(struct bmap *b, int blockable)
{
	struct slashrpc_cservice *csvc = NULL;
	struct pscrpc_request *rq = NULL;
	struct srm_leasebmapext_req *mq;
	struct srm_leasebmapext_rep *mp;
	int secs, rc;
	struct timespec ts;

	BMAP_LOCK(b);
	if (b->bcm_flags & BMAP_TOFREE) {
		psc_assert(!blockable);
		BMAP_ULOCK(b);
		return 0;
	}

	if (b->bcm_flags & BMAP_CLI_LEASEFAILED) {
		/*
		 * Catch the case where another thread has already
		 * marked this bmap as expired.
		 */
		rc = bmap_2_bci(b)->bci_error;
		BMAP_ULOCK(b);
		return rc;
	}

	if (b->bcm_flags & BMAP_CLI_LEASEEXTREQ) {
		if (!blockable) {
			BMAP_ULOCK(b);
			return 0;
		}
		DEBUG_BMAP(PLL_ERROR, b,
		    "blocking on lease renewal");
		bmap_op_start_type(b, BMAP_OPCNT_LEASEEXT);
		bmap_wait_locked(b, (b->bcm_flags & BMAP_CLI_LEASEEXTREQ));

		rc = bmap_2_bci(b)->bci_error;

		bmap_op_done_type(b, BMAP_OPCNT_LEASEEXT);
		return rc;
	}

	PFL_GETTIMESPEC(&ts);
	secs = (int)(bmap_2_bci(b)->bci_etime.tv_sec - ts.tv_sec);
	if (secs >= BMAP_CLI_EXTREQSECS && !(b->bcm_flags & BMAP_CLI_LEASEEXPIRED)) {
		if (blockable)
			OPSTAT_INCR(SLC_OPST_BMAP_LEASE_EXT_HIT);
		BMAP_ULOCK(b);
		return 0;
	}

	if (b->bcm_flags & BMAP_CLI_LEASEEXPIRED)
		b->bcm_flags &= ~BMAP_CLI_LEASEEXPIRED;

	b->bcm_flags |= BMAP_CLI_LEASEEXTREQ;
	bmap_op_start_type(b, BMAP_OPCNT_LEASEEXT);

	BMAP_ULOCK(b);

	rc = slc_rmc_getcsvc1(&csvc,
	    fcmh_2_fci(b->bcm_fcmh)->fci_resm);
	if (rc)
		goto out;
	rc = SL_RSX_NEWREQ(csvc, SRMT_EXTENDBMAPLS, rq, mq, mp);
	if (rc)
		goto out;

	memcpy(&mq->sbd, &bmap_2_bci(b)->bci_sbd, sizeof(struct srt_bmapdesc));
	authbuf_sign(rq, PSCRPC_MSG_REQUEST);

	rq->rq_async_args.pointer_arg[MSL_CBARG_BMAP] = b;
	rq->rq_async_args.pointer_arg[MSL_CBARG_CSVC] = csvc;
	rq->rq_interpret_reply = msl_bmap_lease_tryext_cb;
	pscrpc_req_setcompl(rq, &rpcComp);

	rc = pscrpc_nbreqset_add(pndgBmaplsReqs, rq);
	if (!rc)
		OPSTAT_INCR(SLC_OPST_BMAP_LEASE_EXT_SEND);
 out:
	BMAP_LOCK(b);
	DEBUG_BMAP(rc ? PLL_ERROR : PLL_DIAG, b,
	    "lease extension req (rc=%d) (secs=%d)", rc, secs);
	if (rc) {
		if (rq)
			pscrpc_req_finished(rq);
		if (csvc)
			sl_csvc_decref(csvc);

		bmap_2_bci(b)->bci_error = rc;
		b->bcm_flags &= ~BMAP_CLI_LEASEEXTREQ;
		b->bcm_flags |= BMAP_CLI_LEASEFAILED;

		bmap_wake_locked(b);
		bmap_op_done_type(b, BMAP_OPCNT_LEASEEXT);
		OPSTAT_INCR(SLC_OPST_BMAP_LEASE_EXT_ABRT);
	} else if (blockable) {
		/*
		 * We should never cache data without a lease.
		 * However, let us turn off this for now until
		 * we fix the performance dip.
		 */
		OPSTAT_INCR(SLC_OPST_BMAP_LEASE_EXT_WAIT);
		bmap_wait_locked(b, (b->bcm_flags & BMAP_CLI_LEASEEXTREQ));
		rc = bmap_2_bci(b)->bci_error;
		BMAP_ULOCK(b);
	}

	return (rc);
}

/**
 * msl_bmap_retrieve - Perform a blocking 'LEASEBMAP' operation to
 *	retrieve one or more bmaps from the MDS.
 * @b: the bmap ID to retrieve.
 * @rw: read or write access
 */
int
msl_bmap_retrieve(struct bmap *bmap, enum rw rw,
    __unusedx int flags)
{
	struct slashrpc_cservice *csvc = NULL;
	struct pscrpc_request *rq = NULL;
	struct srm_leasebmap_req *mq;
	struct srm_leasebmap_rep *mp;
	struct fcmh_cli_info *fci;
	struct fidc_membh *f;
	int rc, nretries = 0;

	psc_assert(bmap->bcm_flags & BMAP_INIT);
	psc_assert(bmap->bcm_fcmh);

	f = bmap->bcm_fcmh;
	fci = fcmh_2_fci(f);

 retry:
	OPSTAT_INCR(SLC_OPST_BMAP_RETRIEVE);
	rc = slc_rmc_getcsvc1(&csvc, fci->fci_resm);
	if (rc)
		goto out;
	rc = SL_RSX_NEWREQ(csvc, SRMT_GETBMAP, rq, mq, mp);
	if (rc)
		goto out;

	mq->fg = f->fcmh_fg;
	mq->prefios[0] = prefIOS; /* Tell MDS of our preferred ION */
	mq->bmapno = bmap->bcm_bmapno;
	mq->rw = rw;
	mq->flags |= SRM_LEASEBMAPF_GETINODE;

	DEBUG_FCMH(PLL_DIAG, f, "retrieving bmap (bmapno=%u) (rw=%s)",
	    bmap->bcm_bmapno, (rw == SL_READ) ? "read" : "write");

	rc = SL_RSX_WAITREP(csvc, rq, mp);
	if (rc == 0)
		rc = mp->rc;
	if (rc)
		goto out;
	memcpy(&bmap->bcm_corestate, &mp->bcs, sizeof(mp->bcs));

	FCMH_LOCK(f);

	msl_bmap_reap_init(bmap, &mp->sbd);

	fci->fci_inode = mp->ino;
	f->fcmh_flags |= FCMH_CLI_HAVEINODE;

	DEBUG_BMAP(PLL_DIAG, bmap, "rw=%d repls=%d ios=%#x seq=%"PRId64,
	    rw, mp->ino.nrepls, mp->sbd.sbd_ios, mp->sbd.sbd_seq);

	psc_waitq_wakeall(&f->fcmh_waitq);
	FCMH_ULOCK(f);

 out:
	if (rq) {
		pscrpc_req_finished(rq);
		rq = NULL;
	}
	if (csvc) {
		sl_csvc_decref(csvc);
		csvc = NULL;
	}

	if (rc == -SLERR_BMAP_DIOWAIT) {
		/* Retry for bmap to be DIO ready. */
		DEBUG_BMAP(PLL_WARN, bmap,
		    "SLERR_BMAP_DIOWAIT (rt=%d)", nretries);

		usleep(200000);
		if (nretries > BMAP_CLI_MAX_LEASE * 8 * 5)
			return (-ETIMEDOUT);
		goto retry;
	}

	return (rc);
}

/**
 * msl_bmap_cache_rls - Called from rcm.c (SRMT_BMAPDIO).
 * @b:  the bmap whose cached pages should be released.
 */
void
msl_bmap_cache_rls(struct bmap *b)
{
	struct bmap_pagecache_entry *e;
	struct bmap_pagecache *bmpc = bmap_2_bmpc(b);

	BMAP_LOCK(b);
	for (e = SPLAY_MIN(bmap_pagecachetree, &bmpc->bmpc_tree); e;) {
		BMPCE_LOCK(e);
		e->bmpce_flags |= BMPCE_DISCARD;
		BMPCE_ULOCK(e);
		e = SPLAY_NEXT(bmap_pagecachetree, &bmpc->bmpc_tree, e);
	}
	BMAP_ULOCK(b);
}

void
msl_bmap_reap_init(struct bmap *b, const struct srt_bmapdesc *sbd)
{
	struct bmap_cli_info *bci = bmap_2_bci(b);
	int locked;

	psc_assert(!pfl_memchk(sbd, 0, sizeof(*sbd)));

	locked = BMAP_RLOCK(b);

	bci->bci_sbd = *sbd;
	/*
	 * Record the start time,
	 *  XXX the directio status of the bmap needs to be returned by
	 *	the MDS so we can set the proper expiration time.
	 */
	PFL_GETTIMESPEC(&bci->bci_etime);

	timespecadd(&bci->bci_etime, &msl_bmap_max_lease,
	    &bci->bci_etime);

	/*
	 * Take the reaper ref cnt early and place the bmap onto the
	 * reap list
	 */
	b->bcm_flags |= BMAP_TIMEOQ;
	if (sbd->sbd_flags & SRM_LEASEBMAPF_DIO)
		b->bcm_flags |= BMAP_DIO;

	/*
	 * Is this a write for an archival fs?  If so, set the bmap for
	 * DIO.
	 */
	if (sbd->sbd_ios != IOS_ID_ANY && !(b->bcm_flags & BMAP_DIO)) {
		struct sl_resource *r = libsl_id2res(sbd->sbd_ios);

		psc_assert(r);
		psc_assert(b->bcm_flags & BMAP_WR);

		if (r->res_type == SLREST_ARCHIVAL_FS)
			b->bcm_flags |= BMAP_DIO;
	}
	bmap_op_start_type(b, BMAP_OPCNT_REAPER);

	BMAP_URLOCK(b, locked);

	DEBUG_BMAP(PLL_INFO, b,
	    "reap init: nseq=%"PRId64", "
	    "etime="PSCPRI_TIMESPEC"", bci->bci_sbd.sbd_seq,
	    PFLPRI_PTIMESPEC_ARGS(&bci->bci_etime));

	/*
	 * Add ourselves here otherwise zero length files will not be
	 * removed.
	 */
	lc_addtail(&bmapTimeoutQ, bci);
}

struct reptbl_lookup {
	sl_ios_id_t		 id;
	int			 idx;
	struct sl_resource	*r;
};

int
slc_reptbl_cmp(const void *a, const void *b)
{
	const struct reptbl_lookup *x = a, *y = b;
	struct resprof_cli_info *xi, *yi;
	struct sl_resource *r;
	struct sl_resm *m;
	int rc, xv, yv, i;

	/* try preferred IOS */
	r = libsl_id2res(prefIOS);
	xv = yv = 1;
	DYNARRAY_FOREACH(m, i, &r->res_members) {
		if (x->id == m->resm_res_id) {
			xv = -1;
			if (yv == -1)
				break;
		} else if (y->id == m->resm_res_id) {
			yv = -1;
			if (xv == -1)
				break;
		}
	}
	rc = CMP(xv, yv);
	if (rc)
		return (rc);

	/* try non-archival and non-degraded IOS */
	xv = x->r->res_type == SLREST_ARCHIVAL_FS ? 1 : -1;
	yv = y->r->res_type == SLREST_ARCHIVAL_FS ? 1 : -1;
	rc = CMP(xv, yv);
	if (rc)
		return (rc);

	/* try degraded IOS */
	xi = res2rpci(x->r);
	yi = res2rpci(y->r);
	xv = xi->rpci_flags & RPCIF_AVOID ? 1 : -1;
	yv = yi->rpci_flags & RPCIF_AVOID ? 1 : -1;
	return (CMP(xv, yv));
}

static int
msl_bmap_check_replica(struct bmap *b)
{
	int i, off;
	struct fcmh_cli_info *fci;

	fci = fcmh_get_pri(b->bcm_fcmh);
	for (i = 0, off = 0; i < fci->fci_inode.nrepls;
	    i++, off += SL_BITS_PER_REPLICA)
		if (SL_REPL_GET_BMAP_IOS_STAT(b->bcm_repls,
		    off))
			break;
	if (i == fci->fci_inode.nrepls) {
		DEBUG_BMAP(PLL_ERROR, b,
		    "corrupt bmap!  no valid replicas!");
		return (1);
	}
	return 0;
}

/**
 * msl_bmap_to_csvc - Given a bmap, perform a series of lookups to
 *	locate the ION csvc.  The ION was chosen by the MDS and
 *	returned in the msl_bmap_retrieve routine.
 * @b: the bmap
 * @exclusive: whether to return connections to the specific ION the MDS
 *	told us to use instead of any ION in any IOS whose state is
 *	marked VALID for this bmap.
 * XXX: If the bmap is a read-only then any replica may be accessed (so
 *	long as it is recent).
 */
struct slashrpc_cservice *
msl_bmap_to_csvc(struct bmap *b, int exclusive)
{
	int n, i, j, locked;
	struct reptbl_lookup order[SL_MAX_REPLICAS], *lk;
	struct slashrpc_cservice *csvc;
	struct fcmh_cli_info *fci;
	struct psc_multiwait *mw;
	struct rnd_iterator it;
	struct sl_resm *m;
	void *p;

	psc_assert(atomic_read(&b->bcm_opcnt) > 0);

	if (exclusive) {
		locked = BMAP_RLOCK(b);
		m = libsl_ios2resm(bmap_2_ios(b));
		psc_assert(m->resm_res->res_id == bmap_2_ios(b));
		BMAP_URLOCK(b, locked);

		return (slc_geticsvc(m));
	}

	fci = fcmh_get_pri(b->bcm_fcmh);
	mw = msl_getmw();

	if (msl_bmap_check_replica(b))
		return NULL;

	n = 0;
	FOREACH_RND(&it, fci->fci_inode.nrepls) {
		lk = &order[n++];
		lk->id = fci->fci_inode.reptbl[it.ri_rnd_idx].bs_id;
		lk->idx = it.ri_rnd_idx;
		lk->r = libsl_id2res(lk->id);
		if (lk->r == NULL) {
			DEBUG_FCMH(PLL_ERROR, b->bcm_fcmh,
			    "unknown resource %#x", lk->id);
			n--;
		}
	}
	qsort(order, n, sizeof(order[0]), slc_reptbl_cmp);

	for (j = 0; j < 2; j++) {
		psc_multiwait_reset(mw);
		psc_multiwait_entercritsect(mw);
		for (i = 0; i < n; i++) {
			csvc = msl_try_get_replica_res(b, order[i].idx);
			if (csvc) {
				psc_multiwait_leavecritsect(mw);
				return (csvc);
			}
		}

		/*
		 * No connection was immediately available; wait a small
		 * amount of time for any to finish connection
		 * (re)establishment.
		 */
		if (!psc_dynarray_len(&mw->mw_conds))
			break;
		psc_multiwait_secs(mw, &p, BMAP_CLI_MAX_LEASE);
	}
	psc_multiwait_leavecritsect(mw);
	return (NULL);
}

void
bmap_biorq_waitempty(struct bmap *b)
{
	struct bmap_pagecache *bmpc;

	bmpc = bmap_2_bmpc(b);
	BMAP_LOCK(b);
	bmap_wait_locked(b, (!pll_empty(&bmpc->bmpc_pndg_biorqs)  ||
			     !SPLAY_EMPTY(&bmpc->bmpc_new_biorqs) ||
			     !pll_empty(&bmpc->bmpc_pndg_ra)      ||
			     (b->bcm_flags & BMAP_FLUSHQ)));

	psc_assert(pll_empty(&bmpc->bmpc_pndg_biorqs));
	psc_assert(SPLAY_EMPTY(&bmpc->bmpc_new_biorqs));
	psc_assert(pll_empty(&bmpc->bmpc_pndg_ra));
	BMAP_ULOCK(b);
}

void
bmap_biorq_expire(struct bmap *b)
{
	struct bmap_pagecache *bmpc;
	struct bmpc_ioreq *r;

	/*
	 * Note that the following two lists and the bmap
	 * structure itself all share the same lock.
	 */
	bmpc = bmap_2_bmpc(b);
	BMAP_LOCK(b);
	PLL_FOREACH(r, &bmpc->bmpc_pndg_biorqs)
		BIORQ_SETATTR(r, BIORQ_FORCE_EXPIRE);
	BMAP_ULOCK(b);

	bmap_flushq_wake(BMAPFLSH_RPCWAIT);
}

/**
 * msl_bmap_final_cleanup - Implement bmo_final_cleanupf() operation.
 */
void
msl_bmap_final_cleanup(struct bmap *b)
{
	struct bmap_pagecache *bmpc = bmap_2_bmpc(b);

	BMAP_LOCK(b);
	psc_assert(!(b->bcm_flags & BMAP_FLUSHQ));

	psc_assert(pll_empty(&bmpc->bmpc_pndg_biorqs));
	psc_assert(SPLAY_EMPTY(&bmpc->bmpc_new_biorqs));
	psc_assert(pll_empty(&bmpc->bmpc_pndg_ra));

	DEBUG_BMAP(PLL_DIAG, b, "start freeing");

	/* Mind lock ordering; remove from LRU first. */
	if (b->bcm_flags & BMAP_DIO &&
	    psclist_disjoint(&bmpc->bmpc_lentry)) {
		psc_assert(SPLAY_EMPTY(&bmpc->bmpc_tree));
		psc_assert(pll_empty(&bmpc->bmpc_lru));
	} else {
		bmpc_lru_del(bmpc);
	}

	/*
	 * Assert that this bmap can no longer be scheduled by the write
	 * back cache thread.
	 */
	psc_assert(psclist_disjoint(&b->bcm_lentry));

	/*
	 * Assert that this thread cannot be seen by the page cache
	 * reaper (it was lc_remove'd above by bmpc_lru_del()).
	 */
	psc_assert(psclist_disjoint(&bmpc->bmpc_lentry));

	bmpc_freeall_locked(bmpc);
	BMAP_ULOCK(b);

	DEBUG_BMAP(PLL_DIAG, b, "done freeing");
}

#if PFL_DEBUG > 0
void
dump_bmap_flags(uint32_t flags)
{
	int seq = 0;

	_dump_bmap_flags_common(&flags, &seq);
	PFL_PRFLAG(BMAP_CLI_LEASEEXTREQ, &flags, &seq);
	PFL_PRFLAG(BMAP_CLI_REASSIGNREQ, &flags, &seq);
	PFL_PRFLAG(BMAP_CLI_LEASEFAILED, &flags, &seq);
	PFL_PRFLAG(BMAP_CLI_LEASEEXPIRED, &flags, &seq);
	if (flags)
		printf(" unknown: %#x\n", flags);
	printf("\n");
}
#endif

struct bmap_ops sl_bmap_ops = {
	msl_bmap_free,			/* bmo_free() */
	msl_bmap_init,			/* bmo_init_privatef() */
	msl_bmap_retrieve,		/* bmo_retrievef() */
	msl_bmap_modeset,		/* bmo_mode_chngf() */
	msl_bmap_final_cleanup		/* bmo_final_cleanupf() */
};
