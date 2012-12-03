/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2009-2012, Pittsburgh Supercomputing Center (PSC).
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

#include <sys/time.h>

#include "psc_ds/dynarray.h"
#include "psc_ds/listcache.h"
#include "psc_rpc/rpc.h"
#include "psc_rpc/rsx.h"
#include "psc_util/ctlsvr.h"
#include "psc_util/lock.h"
#include "psc_util/log.h"

#include "bmap_iod.h"
#include "fidcache.h"
#include "rpc_iod.h"
#include "sliod.h"
#include "slvr.h"

struct bmap_iod_minseq	 bimSeq;
static struct timespec	 bim_timeo = { BIM_MINAGE, 0 };

struct psc_listcache	 bmapRlsQ;

struct psc_poolmaster    bmap_rls_poolmaster;
struct psc_poolmgr      *bmap_rls_pool;

void
bim_init(void)
{
	INIT_SPINLOCK(&bimSeq.bim_lock);
	psc_waitq_init(&bimSeq.bim_waitq);
	bimSeq.bim_minseq = 0;
}

int
bim_updateseq(uint64_t seq)
{
	int invalid = 0;

	spinlock(&bimSeq.bim_lock);

	if (bimSeq.bim_minseq == BMAPSEQ_ANY ||
	    (seq >= bimSeq.bim_minseq && seq != BMAPSEQ_ANY)) {
		bimSeq.bim_minseq = seq;
		PFL_GETTIMESPEC(&bimSeq.bim_age);
	} else
		invalid = 1;

	freelock(&bimSeq.bim_lock);

	if (invalid)
		psclog_notify("seq %"PRId64" is invalid (bim_minseq=%"PRId64")",
			  seq, bimSeq.bim_minseq);
	return (invalid);
}

uint64_t
bim_getcurseq(void)
{
	struct slashrpc_cservice *csvc = NULL;
	struct pscrpc_request *rq = NULL;
	struct timespec crtime;
	uint64_t seqno = BMAPSEQ_ANY;

 retry:
	PFL_GETTIMESPEC(&crtime);
	timespecsub(&crtime, &bim_timeo, &crtime);

	spinlock(&bimSeq.bim_lock);
	if (bimSeq.bim_flags & BIM_RETRIEVE_SEQ) {
		psc_waitq_wait(&bimSeq.bim_waitq, &bimSeq.bim_lock);
		goto retry;
	}

	if (timespeccmp(&crtime, &bimSeq.bim_age, >) ||
	    bimSeq.bim_minseq == BMAPSEQ_ANY) {
		struct srm_getbmapminseq_req *mq;
		struct srm_getbmapminseq_rep *mp;
		int rc;

		bimSeq.bim_flags |= BIM_RETRIEVE_SEQ;
		freelock(&bimSeq.bim_lock);

		rc = sli_rmi_getcsvc(&csvc);
		if (rc)
			goto out;

		rc = SL_RSX_NEWREQ(csvc, SRMT_GETBMAPMINSEQ, rq, mq, mp);
		if (rc)
			goto out;

		rc = SL_RSX_WAITREP(csvc, rq, mp);
		if (rc)
			goto out;

		seqno = mp->seqno;

		rc = bim_updateseq(seqno);
	out:
		if (rq) {
			pscrpc_req_finished(rq);
			rq = NULL;
		}
		if (csvc) {
			sl_csvc_decref(csvc);
			csvc = NULL;
		}

		spinlock(&bimSeq.bim_lock);
		bimSeq.bim_flags &= ~BIM_RETRIEVE_SEQ;
		psc_waitq_wakeall(&bimSeq.bim_waitq);
		if (rc) {
			psclog_warnx("failed to get bmap seqno rc=%d", rc);
			freelock(&bimSeq.bim_lock);
			goto retry;
		}
	}

	seqno = bimSeq.bim_minseq;
	freelock(&bimSeq.bim_lock);

	return (seqno);
}

void
bcr_hold_2_ready(struct biod_infl_crcs *inf, struct biod_crcup_ref *bcr)
{
	int locked;

	BII_LOCK_ENSURE(bcr->bcr_bii);

	locked = reqlock(&inf->binfcrcs_lock);
	pll_remove(&inf->binfcrcs_hold, bcr);
	pll_addtail(&inf->binfcrcs_ready, bcr);
	ureqlock(&inf->binfcrcs_lock, locked);

	bcr->bcr_bii->biod_bcr = NULL;
}

void
bcr_hold_add(struct biod_infl_crcs *inf, struct biod_crcup_ref *bcr)
{
	psc_assert(psclist_disjoint(&bcr->bcr_lentry));
	pll_addtail(&inf->binfcrcs_hold, bcr);
	atomic_inc(&inf->binfcrcs_nbcrs);
}

void
bcr_ready_add(struct biod_infl_crcs *inf, struct biod_crcup_ref *bcr)
{
	psc_assert(psclist_disjoint(&bcr->bcr_lentry));
	pll_addtail(&inf->binfcrcs_ready, bcr);
	atomic_inc(&inf->binfcrcs_nbcrs);
}

void
bcr_hold_requeue(struct biod_infl_crcs *inf, struct biod_crcup_ref *bcr)
{
	int locked;

	locked = reqlock(&inf->binfcrcs_lock);
	pll_remove(&inf->binfcrcs_hold, bcr);
	pll_addtail(&inf->binfcrcs_hold, bcr);
	ureqlock(&inf->binfcrcs_lock, locked);
}

void
bcr_xid_check(struct biod_crcup_ref *bcr)
{
	int locked;

	locked = BII_RLOCK(bcr->bcr_bii);
	psc_assert(bcr->bcr_xid < bcr->bcr_bii->biod_bcr_xid);
	psc_assert(bcr->bcr_xid == bcr->bcr_bii->biod_bcr_xid_last);
	/* bcr_xid_check() must be called prior to bumping xid_last.
	 */
	psc_assert(bcr->bcr_bii->biod_bcr_xid >
		   bcr->bcr_bii->biod_bcr_xid_last);

	BII_URLOCK(bcr->bcr_bii, locked);
}

static void
bcr_xid_last_bump(struct biod_crcup_ref *bcr)
{
	bcr_xid_check(bcr);
	bcr->bcr_bii->biod_bcr_xid_last++;
	bcr_2_bmap(bcr)->bcm_flags &= ~BMAP_IOD_INFLIGHT;
}

void
biod_rlssched_locked(struct bmap_iod_info *bii)
{
	BII_LOCK_ENSURE(bii);

	DEBUG_BMAP(PLL_INFO, bii_2_bmap(bii), "crcdrty_slvrs=%d "
	   "BMAP_IOD_RLSSEQ=(%d) biod_bcr_xid=%"PRId64" biod_bcr_xid_last=%"
	   PRId64, psc_atomic32_read(&bii->biod_crcdrty_slvrs),
	   !!(bii_2_flags(bii) & BMAP_IOD_RLSSEQ), bii->biod_bcr_xid,
	   bii->biod_bcr_xid_last);

	if (bii_2_bmap(bii)->bcm_flags & BMAP_IOD_RLSSCHED)
		/*
		 * Don't test for list membership, the bmaprlsthr may
		 * have already removed the bii in preparation for
		 * release.
		 */
		psc_assert(bii_2_bmap(bii)->bcm_flags & BMAP_IOD_RLSSEQ);

	else {
		psc_assert(psclist_disjoint(&bii->bii_lentry));

		if (!psc_atomic32_read(&bii->biod_crcdrty_slvrs) &&
		    (bii_2_bmap(bii)->bcm_flags & BMAP_IOD_RLSSEQ) &&
		    (bii->biod_bcr_xid == bii->biod_bcr_xid_last)) {
			BMAP_SETATTR(bii_2_bmap(bii), BMAP_IOD_RLSSCHED);
			lc_addtail(&bmapRlsQ, bii);
		}
	}
}

/**
 * bcr_ready_remove - We are done with this batch of CRC updates.   Drop
 *	its reference to the bmap and free the CRC update structure.
 */
__static void
bcr_ready_remove(struct biod_infl_crcs *inf, struct biod_crcup_ref *bcr)
{
	struct bmap_iod_info *bii = bcr->bcr_bii;

	spinlock(&inf->binfcrcs_lock);
	pll_remove(&inf->binfcrcs_ready, bcr);
	atomic_dec(&inf->binfcrcs_nbcrs);
	freelock(&inf->binfcrcs_lock);

	BII_LOCK(bcr->bcr_bii);
	psc_assert(bcr->bcr_flags & BCR_SCHEDULED);
	bcr_xid_last_bump(bcr);

	if (bii->biod_bcr_xid == bii->biod_bcr_xid_last) {
		/* This was the last bcr. */
		psc_assert(pll_empty(&bii->biod_bklog_bcrs));
		psc_assert(!bii->biod_bcr);
		BMAP_CLEARATTR(bii_2_bmap(bii), BMAP_IOD_BCRSCHED);

		DEBUG_BMAP(PLL_INFO, bii_2_bmap(bii),
		   "descheduling drtyslvrs=%u",
		   psc_atomic32_read(&bii->biod_crcdrty_slvrs));

		biod_rlssched_locked(bii);
	}
	BII_ULOCK(bcr->bcr_bii);

	bmap_op_done_type(bcr_2_bmap(bcr), BMAP_OPCNT_BCRSCHED);
	PSCFREE(bcr);
}

void
bcr_finalize(struct biod_infl_crcs *inf, struct biod_crcup_ref *bcr)
{
	struct bmap_iod_info *bii = bcr->bcr_bii;

	DEBUG_BCR(PLL_INFO, bcr, "finalize");

	BII_LOCK(bii);
	psc_assert(bii_2_bmap(bii)->bcm_flags & BMAP_IOD_BCRSCHED);

	/*
	 * bii->biod_bcr_xid_last is bumped in bcr_ready_remove().
	 * bcr_ready_remove() may release the bmap so it must be issued
	 * at the end of this call.
	 */
	if (bii->biod_bcr_xid > bii->biod_bcr_xid_last + 1) {
		struct biod_crcup_ref *tmp;

		tmp = pll_gethead(&bii->biod_bklog_bcrs);
		if (tmp) {
			DEBUG_BCR(PLL_INFO, tmp, "backlogged bcr, nblklog=%d",
				  pll_nitems(&bii->biod_bklog_bcrs));

			if (pll_empty(&bii->biod_bklog_bcrs)) {
				/*
				 * I am the only one on the backlog list of
				 * the bmap.  A NULL biod_bcr is ok as long
				 * as the bcr has been filled.
				 */
				psc_assert(bii->biod_bcr == tmp ||
					   !bii->biod_bcr);
				if (tmp->bcr_crcup.nups ==
				    MAX_BMAP_INODE_PAIRS) {
					bii->biod_bcr = NULL;
					bcr_ready_add(inf, tmp);
				} else
					bcr_hold_add(inf, tmp);

			} else {
				/* Only the tail of the bklog may be the
				 *    active bcr.
				 */
				psc_assert(bii->biod_bcr != tmp);
				bcr_ready_add(inf, tmp);
			}
		}
	}
	BII_ULOCK(bii);

	bcr_ready_remove(inf, bcr);
}

void
slibmaprlsthr_main(__unusedx struct psc_thread *thr)
{
	struct psc_dynarray a = DYNARRAY_INIT;
	struct srm_bmap_release_req *brr, *mq;
	struct srm_bmap_release_rep *mp;
	struct pscrpc_request *rq = NULL;
	struct slashrpc_cservice *csvc;
	struct bmap_iod_info *bii;
	struct bmap_iod_rls *brls;
	struct bmapc_memb *b;
	int nrls, rc, i;

	brr = PSCALLOC(sizeof(struct srm_bmap_release_req));

	while (pscthr_run()) {
		nrls = 0;

		bii = lc_getwait(&bmapRlsQ);
		if (lc_sz(&bmapRlsQ) < MAX_BMAP_RELEASE)
			/* Try to coalesce, wait for others.
			 *   yes, this is a bit ugly.
			 */
			sleep(SLIOD_BMAP_RLS_WAIT_SECS);

		do {
			b = bii_2_bmap(bii);
			BII_LOCK(bii);
			psc_assert(pll_nitems(&bii->biod_rls));

			DEBUG_BMAP(PLL_INFO, b, "ndrty=%u nrls=%d xid=%"PRIu64
			   " xid_last=%"PRIu64,
			   psc_atomic32_read(&bii->biod_crcdrty_slvrs),
			   pll_nitems(&bii->biod_rls), bii->biod_bcr_xid,
			   bii->biod_bcr_xid_last);

			psc_assert(bii_2_flags(bii) & BMAP_IOD_RLSSEQ);
			psc_assert(bii_2_flags(bii) & BMAP_IOD_RLSSCHED);

			if (psc_atomic32_read(&bii->biod_crcdrty_slvrs) ||
			    (bii->biod_bcr_xid != bii->biod_bcr_xid_last)) {
				/* Temporarily remove unreapable bii's */
				psc_dynarray_add(&a, bii);
				BII_ULOCK(bii);
				continue;
			}

			i = 0;
			while (nrls < MAX_BMAP_RELEASE &&
			    (brls = pll_get(&bii->biod_rls))) {
				memcpy(&brr->sbd[nrls++], &brls->bir_sbd,
				    sizeof(struct srt_bmapdesc));
				psc_pool_return(bmap_rls_pool, brls);
				i++;
			}
			/* The last entry (or entries) did not fit, so
			 *    reschedule.
			 */
			if (pll_nitems(&bii->biod_rls))
				psc_dynarray_add(&a, bii);
			else
				BMAP_CLEARATTR(bii_2_bmap(bii),
				       BMAP_IOD_RLSSEQ | BMAP_IOD_RLSSCHED);
			BII_ULOCK(bii);

			while (i--)
				bmap_op_done_type(b, BMAP_OPCNT_RLSSCHED);

		} while ((nrls < MAX_BMAP_RELEASE) &&
		    (bii = lc_getnb(&bmapRlsQ)));

		if (!nrls)
			goto end;

		brr->nbmaps = nrls;

		OPSTAT_INCR(SLI_OPST_SRMT_RELEASE);

		/*
		 * The system can tolerate the loss of these messages so
		 *   errors here should not be considered fatal.
		 */
		rc = sli_rmi_getcsvc(&csvc);
		if (rc) {
			psclog_errorx("Failed to get MDS import rc=%d", rc);
			goto end;
		}

		rc = SL_RSX_NEWREQ(csvc, SRMT_RELEASEBMAP, rq, mq, mp);
		if (rc) {
			psclog_errorx("Failed to generate new req rc=%d", rc);
			sl_csvc_decref(csvc);
			goto end;
		}

		memcpy(mq, brr, sizeof(*mq));
		rc = SL_RSX_WAITREP(csvc, rq, mp);
		if (rc)
			psclog_errorx("RELEASEBMAP req failed rc=%d", rc);

		pscrpc_req_finished(rq);
		sl_csvc_decref(csvc);
 end:
		/* put any unreapable biods back to the list */
		rc = lc_sz(&bmapRlsQ);

		DYNARRAY_FOREACH(bii, nrls, &a)
			lc_addtail(&bmapRlsQ, bii);

		if (!rc)
			sleep(SLIOD_BMAP_RLS_WAIT_SECS);

		psc_dynarray_free(&a);
	}
}

void
slibmaprlsthr_spawn(void)
{
	lc_reginit(&bmapRlsQ, struct bmap_iod_info, bii_lentry,
	    "bmapRlsQ");

	pscthr_init(SLITHRT_BMAPRLS, 0, slibmaprlsthr_main, NULL, 0,
	    "slibmaprlsthr");
}

void
iod_bmap_init(struct bmapc_memb *b)
{
	struct bmap_iod_info *bii;

	bii = bmap_2_bii(b);

	memset(bii, 0, sizeof(*bii));
	INIT_PSC_LISTENTRY(&bii->bii_lentry);
	SPLAY_INIT(&bii->biod_slvrs);
	pll_init(&bii->biod_bklog_bcrs, struct biod_crcup_ref,
	    bcr_lentry, NULL);

	pll_init(&bii->biod_rls, struct bmap_iod_rls,
	    bir_lentry, NULL);

	PFL_GETTIMESPEC(&bii->bii_age);

	psc_atomic32_set(&bii->biod_crcdrty_slvrs, 0);

	/* XXX At some point we'll want to let bmaps hang around in the
	 *   cache to prevent extra reads and CRC table fetches.
	 */
	//bmap_op_start_type(b, BMAP_OPCNT_REAPER);
	//lc_addtail(b, &bmapReapQ);
}

void
iod_bmap_finalcleanup(struct bmapc_memb *b)
{
	struct bmap_iod_info *bii;

	bii = bmap_2_bii(b);
	psc_assert(pll_empty(&bii->biod_rls));
	psc_assert(SPLAY_EMPTY(&bii->biod_slvrs));
	psc_assert(psclist_disjoint(&bii->bii_lentry));

	psc_assert(!psc_atomic32_read(&bii->biod_crcdrty_slvrs));
}

/**
 * iod_bmap_retrieve - Load the relevant bmap information from the
 *	metadata server.  In the case of the ION the bmap sections of
 *	interest are the CRC table and the CRC states bitmap.  For now
 *	we only load this information on read.
 * @b: bmap to load.
 * @rw: the bmap access mode.
 * Return zero on success or errno code on failure (likely an RPC
 *	problem).
 */
int
iod_bmap_retrieve(struct bmapc_memb *b, enum rw rw, __unusedx int flags)
{
	struct pscrpc_request *rq = NULL;
	struct srm_getbmap_full_req *mq;
	struct srm_getbmap_full_rep *mp;
	struct slashrpc_cservice *csvc;
	int rc, i, n;

	if (rw != SL_READ)
		return (0);

	BMAP_LOCK(b);
	for (i = 0, n = BMAP_SLVR_DATA; i < SLASH_SLVRS_PER_BMAP; i++) {
		if (b->bcm_crcstates[i] & BMAP_SLVR_DATA)
			n--;
	}

	if (!n) {
		DEBUG_BMAP(PLL_INFO, b, "CRC table exists locally");
		BMAP_ULOCK(b);
		return (0);
	} else
		BMAP_ULOCK(b);

	rc = sli_rmi_getcsvc(&csvc);
	if (rc)
		goto out;

	rc = SL_RSX_NEWREQ(csvc, SRMT_GETBMAPCRCS, rq, mq, mp);
	if (rc) {
		DEBUG_BMAP(PLL_ERROR, b,
		    "could not create request rc=%d", rc);
		goto out;
	}

	mq->rw = rw;
	mq->bmapno = b->bcm_bmapno;
	memcpy(&mq->fg, &b->bcm_fcmh->fcmh_fg, sizeof(mq->fg));
	//memcpy(&mq->sbdb, sbdb, sizeof(*sbdb));

	rc = SL_RSX_WAITREP(csvc, rq, mp);
	if (rc == 0)
		rc = mp->rc;
	if (rc) {
		DEBUG_BMAP(PLL_ERROR, b, "req failed (%d)", rc);
		goto out;
	}

	BMAP_LOCK(b); /* equivalent to BII_LOCK() */

	for (i = 0; i < SLASH_SLVRS_PER_BMAP; i++) {
		//XXX set BMAP_SLVR_DATA before do_crc()
		//XXX  shoudln't retrieve if all slvrs are BMAP_SLVR_DATA
		if (b->bcm_crcstates[i] & BMAP_SLVR_DATA)
			continue;

		b->bcm_crcstates[i] = mp->bod.bod_crcstates[i];
		bmap_2_ondisk(b)->bod_crcs[i] = mp->bod.bod_crcs[i];
	}

	DEBUG_BMAPOD(PLL_INFO, b, "retrieved");
	BMAP_ULOCK(b);
 out:
	/* Unblock threads no matter what.
	 *  XXX need some way to denote that a CRCGET RPC failed?
	 */
	if (rc)
		DEBUG_BMAP(PLL_ERROR, b, "rc=%d", rc);
	if (rq)
		pscrpc_req_finished(rq);
	if (csvc)
		sl_csvc_decref(csvc);
	return (rc);
}

#if PFL_DEBUG > 0
void
dump_bmap_flags(uint32_t flags)
{
	int seq = 0;

	_dump_bmap_flags_common(&flags, &seq);
	PFL_PRFLAG(BMAP_IOD_INFLIGHT, &flags, &seq);
	PFL_PRFLAG(BMAP_IOD_RLSSEQ, &flags, &seq);
	PFL_PRFLAG(BMAP_IOD_BCRSCHED, &flags, &seq);
	PFL_PRFLAG(BMAP_IOD_RLSSCHED, &flags, &seq);
	if (flags)
		printf(" unknown: %#x", flags);
	printf("\n");
}
#endif

struct bmap_ops bmap_ops = {
	iod_bmap_init,
	iod_bmap_retrieve,
	iod_bmap_retrieve,
	iod_bmap_finalcleanup
};
