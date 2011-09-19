/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2009-2011, Pittsburgh Supercomputing Center (PSC).
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

#define PSC_SUBSYS SLSS_BMAP
#include "slsubsys.h"

#include <time.h>

#include "pfl/fsmod.h"
#include "psc_ds/lockedlist.h"
#include "psc_util/atomic.h"
#include "psc_util/pool.h"

#include "bmpc.h"
#include "mount_slash.h"

struct psc_poolmaster	 bmpcePoolMaster;
struct psc_poolmgr	*bmpcePoolMgr;
struct bmpc_mem_slbs	 bmpcSlabs;
struct psc_listcache	 bmpcLru;

__static SPLAY_GENERATE(bmap_pagecachetree, bmap_pagecache_entry,
			bmpce_tentry, bmpce_cmp);

/**
 * bmpce_init - Initialize a bmap page cache entry.
 */
int
bmpce_init(__unusedx struct psc_poolmgr *poolmgr, void *p)
{
	struct bmap_pagecache_entry *bmpce = p;

	memset(bmpce, 0, sizeof(*bmpce));
	INIT_PSC_LISTENTRY(&bmpce->bmpce_lentry);
	INIT_PSC_LISTENTRY(&bmpce->bmpce_ralentry);
	INIT_SPINLOCK(&bmpce->bmpce_lock);
	pll_init(&bmpce->bmpce_pndgaios, struct msl_fsrqinfo,
	    mfsrq_lentry, &bmpce->bmpce_lock);
	bmpce->bmpce_flags = BMPCE_NEW;
	return (0);
}

struct bmap_pagecache_entry *
bmpce_lookup_locked(struct bmap_pagecache *bmpc, struct bmpc_ioreq *biorq,
		    uint32_t off, struct psc_waitq *wq)
{
	struct bmap_pagecache_entry bmpce_search, *bmpce = NULL,
		*bmpce_new = NULL;

	LOCK_ENSURE(&bmpc->bmpc_lock);

	bmpce_search.bmpce_off = off;

	while (!bmpce) {
		bmpce = SPLAY_FIND(bmap_pagecachetree, &bmpc->bmpc_tree,
		    &bmpce_search);
		if (bmpce)
			break;

		else if (bmpce_new == NULL) {
			BMPC_ULOCK(bmpc);
			bmpce_new = psc_pool_get(bmpcePoolMgr);
			BMPC_LOCK(bmpc);
			continue;
		} else {
			bmpce = bmpce_new;
			bmpce_new = NULL;
			bmpce->bmpce_off = bmpce_search.bmpce_off;
			bmpce_useprep(bmpce, biorq, wq);

			SPLAY_INSERT(bmap_pagecachetree,
			     &bmpc->bmpc_tree, bmpce);
		}
	}
	if (bmpce_new)
		psc_pool_return(bmpcePoolMgr, bmpce_new);

	return (bmpce);
}

void
bmpce_getbuf(struct bmap_pagecache_entry *e)
{
	int locked;
	void *tmp;

	locked = BMPCE_RLOCK(e);
	psc_assert(e->bmpce_flags & BMPCE_GETBUF);
	psc_assert(!e->bmpce_base);
	psc_assert(e->bmpce_waitq);
	BMPCE_ULOCK(e);

	tmp = bmpc_alloc();

	BMPCE_LOCK(e);
	psc_assert(e->bmpce_flags & BMPCE_GETBUF);
	e->bmpce_base = tmp;
	e->bmpce_flags &= ~BMPCE_GETBUF;
	psc_waitq_wakeall(e->bmpce_waitq);
	BMPCE_URLOCK(e, locked);
}

__static void
bmpce_release_locked(struct bmap_pagecache_entry *, struct bmap_pagecache *);

void
bmpce_handle_lru_locked(struct bmap_pagecache_entry *bmpce,
			struct bmap_pagecache *bmpc, int op, int incref)
{
	psc_assert(op == BIORQ_WRITE || op == BIORQ_READ);

	LOCK_ENSURE(&bmpc->bmpc_lock);
	LOCK_ENSURE(&bmpce->bmpce_lock);

	DEBUG_BMPCE((bmpce->bmpce_flags & BMPCE_EIO) ? PLL_WARN : PLL_INFO,
	    bmpce, "op=%d incref=%d", op, incref);

	psc_assert(psc_atomic16_read(&bmpce->bmpce_wrref) >= 0);
	psc_assert(psc_atomic16_read(&bmpce->bmpce_rdref) >= 0);

	if (psc_atomic16_read(&bmpce->bmpce_wrref)) {
		psc_assert(!(bmpce->bmpce_flags & BMPCE_LRU));
		psc_assert(!pll_conjoint(&bmpc->bmpc_lru, bmpce));

	} else {
		if (bmpce->bmpce_flags & BMPCE_GETBUF)
			psc_assert(!bmpce->bmpce_base);
		else
			if (bmpce->bmpce_flags & BMPCE_LRU)
				psc_assert(pll_conjoint(&bmpc->bmpc_lru,
					bmpce));
	}

	if (incref) {
		PFL_GETTIMESPEC(&bmpce->bmpce_laccess);

		if (op == BIORQ_WRITE) {
			if (bmpce->bmpce_flags & BMPCE_LRU) {
				pll_remove(&bmpc->bmpc_lru, bmpce);
				bmpce->bmpce_flags &= ~BMPCE_LRU;
			}
			psc_atomic16_inc(&bmpce->bmpce_wrref);

		} else {
			if (bmpce->bmpce_flags & BMPCE_LRU) {
				pll_remove(&bmpc->bmpc_lru, bmpce);
				pll_add_sorted(&bmpc->bmpc_lru, bmpce,
					       bmpce_lrusort_cmp1);
			} else
				psc_assert(
				   psc_atomic16_read(&bmpce->bmpce_wrref) ||
				   psc_atomic16_read(&bmpce->bmpce_rdref) ||
				   (bmpce->bmpce_flags & BMPCE_READPNDG)  ||
				   (bmpce->bmpce_flags & BMPCE_GETBUF)    ||
				   (bmpce->bmpce_flags & BMPCE_DATARDY)   ||
				   (bmpce->bmpce_flags & BMPCE_INIT));

			psc_atomic16_inc(&bmpce->bmpce_rdref);
		}

	} else {
		psc_assert(bmpce->bmpce_base);

		//if (!(bmpce->bmpce_flags & BMPCE_EIO))
		//	psc_assert(bmpce->bmpce_flags & BMPCE_DATARDY);

		if (op == BIORQ_WRITE) {
			psc_assert(psc_atomic16_read(&bmpce->bmpce_wrref) > 0);
			psc_assert(!(bmpce->bmpce_flags & BMPCE_LRU));
			psc_atomic16_dec(&bmpce->bmpce_wrref);

		} else {
			psc_assert(psc_atomic16_read(&bmpce->bmpce_rdref) > 0);
			psc_atomic16_dec(&bmpce->bmpce_rdref);
			if (!psc_atomic16_read(&bmpce->bmpce_rdref))
				bmpce->bmpce_flags &= ~BMPCE_READPNDG;
		}

		if (!(psc_atomic16_read(&bmpce->bmpce_wrref) ||
		      psc_atomic16_read(&bmpce->bmpce_rdref))) {
			/* Last ref on an EIO page so remove it.
			 */
			if (bmpce->bmpce_flags & BMPCE_EIO) {
				DEBUG_BMPCE(PLL_WARN, bmpce, "freeing EIO");
				psc_assert(bmpce->bmpce_waitq);
				BMPCE_WAKE(bmpce);
				bmpce_freeprep(bmpce);
				bmpce_release_locked(bmpce, bmpc);
				return;

			} else if (!(bmpce->bmpce_flags & BMPCE_LRU)) {
				bmpce->bmpce_flags |= BMPCE_LRU;
				pll_add_sorted(&bmpc->bmpc_lru, bmpce,
					       bmpce_lrusort_cmp1);
				psc_waitq_wakeall(&bmpcSlabs.bmms_waitq);
			}

		} else if (bmpce->bmpce_flags & BMPCE_EIO) {
			/* In cases where EIO is present the lock must be
			 *   freed no matter what.  This is because we
			 *   try to free the bmpce above, which when
			 *   successful, replaces the bmpce to the pool.
			 */
			BMPCE_WAKE(bmpce);
			BMPCE_ULOCK(bmpce);
		}
	}

	if (pll_nitems(&bmpc->bmpc_lru) > 0) {
		bmpce = pll_peekhead(&bmpc->bmpc_lru);
		memcpy(&bmpc->bmpc_oldest, &bmpce->bmpce_laccess,
		       sizeof(struct timespec));
	}
}

int
bmpc_biorq_cmp(const void *x, const void *y)
{
	const struct bmpc_ioreq *a = x, *b = y;

	if (a->biorq_off == b->biorq_off)
		/*
		 * Larger requests with the same start offset should
		 * have ordering priority.
		 */
		return (CMP(b->biorq_len, a->biorq_len));
	return (CMP(a->biorq_off, b->biorq_off));
}

static void
bmpc_slb_init(struct sl_buffer *slb)
{
	slb->slb_inuse = psc_vbitmap_new(BMPC_SLB_NBLKS);
	slb->slb_blksz = BMPC_BUFSZ;
	slb->slb_nblks = BMPC_SLB_NBLKS;
	slb->slb_base  = psc_alloc((BMPC_SLB_NBLKS * BMPC_BUFSZ), 
			   PAF_PAGEALIGN);
	atomic_set(&slb->slb_ref, 0);
	atomic_set(&slb->slb_unmapd_ref, 0);
	atomic_set(&slb->slb_inflight, 0);
	INIT_SPINLOCK(&slb->slb_lock);
	slb->slb_flags = SLB_FRESH;
	INIT_PSCLIST_HEAD(&slb->slb_iov_list);
	INIT_PSC_LISTENTRY(&slb->slb_fcmh_lentry);
	INIT_PSC_LISTENTRY(&slb->slb_mgmt_lentry);
	DEBUG_SLB(PLL_TRACE, slb, "new slb");
}

static void
bmpc_slb_free(struct sl_buffer *slb)
{
	DEBUG_SLB(PLL_NOTIFY, slb, "freeing slb");
	psc_assert(psc_vbitmap_nfree(slb->slb_inuse) == BMPC_SLB_NBLKS);
//	psc_assert(psclist_disjoint(&slb->slb_mgmt_lentry));
//	psc_assert(psclist_disjoint(&slb->slb_fcmh_lentry));
//	psc_assert(psc_listhd_empty(&slb->slb_iov_list));
	psc_assert(!atomic_read(&slb->slb_ref));
	psc_vbitmap_free(slb->slb_inuse);
	PSCFREE(slb->slb_base);
	PSCFREE(slb);
}

static struct sl_buffer *
bmpc_slb_new(void)
{
	struct sl_buffer *slb;

	slb = TRY_PSCALLOC(sizeof(*slb));
	if (slb)
		bmpc_slb_init(slb);

	DEBUG_SLB(PLL_NOTIFY, slb, "adding slb");

	return (slb);
}

int
bmpc_grow(int nslbs)
{
	struct sl_buffer *slb;
	int i=0, nalloced, rc=0;

	BMPCSLABS_LOCK();

	nalloced = pll_nitems(&bmpcSlabs.bmms_slbs);
	psc_assert(nalloced <= BMPC_MAXSLBS);

	psclog_info("nalloced (%d/%d)", nalloced, BMPC_MAXSLBS);

	if (nalloced == BMPC_MAXSLBS) {
		rc = -ENOMEM;
		goto out;
	}

	if (nslbs > (BMPC_MAXSLBS - nalloced))
		nslbs = BMPC_MAXSLBS - nalloced;

	for (i=0; i < nslbs; i++) {
		slb = bmpc_slb_new();
		if (!slb) {
			/* Only complain if nothing was allocated.
			 */
			if (!i)
				rc = -ENOMEM;
			goto out;
		}
		pll_add(&bmpcSlabs.bmms_slbs, slb);
	}
 out:
	psc_waitq_wakeall(&bmpcSlabs.bmms_waitq);
	BMPCSLABS_ULOCK();

	return (rc);
}

__static void
bmpce_release_locked(struct bmap_pagecache_entry *bmpce,
		     struct bmap_pagecache *bmpc)
{
	psc_assert(!psc_atomic16_read(&bmpce->bmpce_rdref));
	psc_assert(!psc_atomic16_read(&bmpce->bmpce_wrref));
	psc_assert(pll_empty(&bmpce->bmpce_pndgaios));
	psc_assert(bmpce->bmpce_flags == BMPCE_FREEING);

	DEBUG_BMPCE(PLL_INFO, bmpce, "freeing");

	psc_assert(SPLAY_REMOVE(bmap_pagecachetree, &bmpc->bmpc_tree, bmpce));
	if (pll_conjoint(&bmpc->bmpc_lru, bmpce))
		pll_remove(&bmpc->bmpc_lru, bmpce);
	/* Replace the bmpc memory.
	 */
	bmpc_free(bmpce->bmpce_base);
	bmpce->bmpce_base = NULL;
	bmpce_init(bmpcePoolMgr, bmpce);
	psc_pool_return(bmpcePoolMgr, bmpce);
}

/**
 * bmpc_freeall_locked - Called when a bmap is being released.  Iterate
 *	across the tree freeing each bmpce.  Prior to being invoked, all
 *	bmpce's must be idle (ie have zero refcnts) and be present on
 *	bmpc_lru.
 */
void
bmpc_freeall_locked(struct bmap_pagecache *bmpc)
{
	struct bmap_pagecache_entry *a, *b;

	LOCK_ENSURE(&bmpc->bmpc_lock);
	psc_assert(pll_empty(&bmpc->bmpc_pndg_biorqs));
	psc_assert(pll_empty(&bmpc->bmpc_new_biorqs));
	psc_assert(pll_empty(&bmpc->bmpc_pndg_ra));

	for (a = SPLAY_MIN(bmap_pagecachetree, &bmpc->bmpc_tree); a; a = b) {
		b = SPLAY_NEXT(bmap_pagecachetree, &bmpc->bmpc_tree, a);

		BMPCE_LOCK(a);
		bmpce_freeprep(a);
		BMPCE_ULOCK(a);

		bmpce_release_locked(a, bmpc);
	}
	psc_assert(SPLAY_EMPTY(&bmpc->bmpc_tree));
	psc_assert(pll_empty(&bmpc->bmpc_lru));
}

/**
 * bmpc_lru_tryfree - Attempt to free 'nfree' blocks from the provided
 *    bmap_pagecache structure.
 * @bmpc:   bmap_pagecache
 * @nfree:  number of blocks to free.
 */
__static int
bmpc_lru_tryfree(struct bmap_pagecache *bmpc, int nfree)
{
	struct bmap_pagecache_entry *bmpce, *tmp;
	struct timespec ts, expire;
	int freed = 0;

	PFL_GETTIMESPEC(&ts);
	timespecsub(&ts, &bmpcSlabs.bmms_minage, &ts);

	timespecsub(&bmpc->bmpc_oldest, &ts, &expire);
	psclog_dbg("bmpc oldest ("PSCPRI_TIMESPEC")",
	    PSCPRI_TIMESPEC_ARGS(&expire));

	PLL_LOCK(&bmpc->bmpc_lru);
	PLL_FOREACH_SAFE(bmpce, tmp, &bmpc->bmpc_lru) {
		BMPCE_LOCK(bmpce);

		psc_assert(!psc_atomic16_read(&bmpce->bmpce_wrref));

		if (psc_atomic16_read(&bmpce->bmpce_rdref)) {
			DEBUG_BMPCE(PLL_INFO, bmpce, "rd ref, skip");
			BMPCE_ULOCK(bmpce);
			continue;
		}

		if (bmpce->bmpce_flags & BMPCE_EIO) {
			/* The thread who sets BMPCE_EIO will remove
			 *   this page from the cache.
			 */
			DEBUG_BMPCE(PLL_WARN, bmpce, "BMPCE_EIO, skip");
			BMPCE_ULOCK(bmpce);
			continue;
		}

		timespecsub(&bmpce->bmpce_laccess, &ts, &expire);

		if (timespeccmp(&ts, &bmpce->bmpce_laccess, <)) {
			DEBUG_BMPCE(PLL_NOTICE, bmpce,
			    "expire=("PSCPRI_TIMESPEC") too recent, skip",
			    PSCPRI_TIMESPEC_ARGS(&expire));

			BMPCE_ULOCK(bmpce);
			break;

		} else {
			DEBUG_BMPCE(PLL_NOTICE, bmpce,
			    "freeing expire=("PSCPRI_TIMESPEC")",
			    PSCPRI_TIMESPEC_ARGS(&expire));

			bmpce_freeprep(bmpce);
			bmpce_release_locked(bmpce, bmpc);
			if (++freed >= nfree)
				break;
		}
	}

	/* Save CPU, assume that the head of the list is the oldest entry.
	 */
	if (pll_nitems(&bmpc->bmpc_lru) > 0) {
		bmpce = pll_peekhead(&bmpc->bmpc_lru);
		memcpy(&bmpc->bmpc_oldest, &bmpce->bmpce_laccess,
		       sizeof(struct timespec));
	}
	PLL_ULOCK(&bmpc->bmpc_lru);

	return (freed);
}

/**
 * bmpc_reap_locked - Reap bmpce from the LRU list.  Sometimes we free
 *	bmpce directly into the pool, so we can't wait here forever.
 */
__static int
bmpc_reap_locked(void)
{
	struct bmap_pagecache *bmpc;
	struct timespec ts;
	int nfreed=0, waiters=1;

	LOCK_ENSURE(&bmpcSlabs.bmms_lock);

	waiters += atomic_read(&bmpcSlabs.bmms_waiters);

	psclog_info("ENTRY waiters=%d", waiters);

	if (bmpcSlabs.bmms_reap) {
		/* Wait and return, the thread holding the reap lock
		 *   should have freed a block for us.
		 */
		atomic_inc(&bmpcSlabs.bmms_waiters);
		psc_waitq_wait(&bmpcSlabs.bmms_waitq, &bmpcSlabs.bmms_lock);
		return 1;
	}
	/* This thread now holds the reap lock.
	 */
	bmpcSlabs.bmms_reap = 1;
	BMPCSLABS_ULOCK();

	LIST_CACHE_LOCK(&bmpcLru);

	lc_sort(&bmpcLru, qsort, bmpc_lru_cmp);
	/* Should be sorted from oldest bmpc to newest.  Skip bmpc whose
	 *   bmpc_oldest time is too recent.
	 */
	PFL_GETTIMESPEC(&ts);
	timespecsub(&ts, &bmpcSlabs.bmms_minage, &ts);

	LIST_CACHE_FOREACH(bmpc, &bmpcLru) {
#if 0
		psclog_dbg("bmpc=%p npages=%d age(%ld:%ld)",
			   bmpc, pll_nitems(&bmpc->bmpc_lru),
			   bmpc->bmpc_oldest.tv_sec, bmpc->bmpc_oldest.tv_nsec);
#endif

		/* First check for LRU items.
		 */
		if (!pll_nitems(&bmpc->bmpc_lru)) {
			psclog_debug("skip bmpc=%p, nothing on lru", bmpc);
			continue;
		}
		/* Second, check for age.
		 */
		if (timespeccmp(&ts, &bmpc->bmpc_oldest, <)) {
			psclog_info("skip bmpc=%p, too recent", bmpc);
			continue;
		}

		nfreed += bmpc_lru_tryfree(bmpc, waiters);

		if (nfreed >= waiters)
			break;
	}
	LIST_CACHE_ULOCK(&bmpcLru);

	/* XXX we probably want the waiter to decrement its owner counter */
	if (nfreed) {
		atomic_sub(nfreed, &bmpcSlabs.bmms_waiters);
		if (atomic_read(&bmpcSlabs.bmms_waiters) < 0)
			atomic_set(&bmpcSlabs.bmms_waiters, 0);
	}

	psclog_info("nfreed=%d, waiters=%d", nfreed, waiters);

	if (waiters > nfreed) {
		int nslbs = (waiters - nfreed) / BMPC_SLB_NBLKS;

		/* Try to increase the number of slbs, if this fails
		 *   then decrease the LRU minimum age.
		 */
		if (bmpc_grow(nslbs ? nslbs : 1) == -ENOMEM)
			bmpc_decrease_minage();
	}

	BMPCSLABS_LOCK();
	bmpcSlabs.bmms_reap = 0;
	psc_waitq_wakeall(&bmpcSlabs.bmms_waitq);
	BMPCSLABS_ULOCK();

	return (nfreed);
}

void
bmpc_free(void *base)
{
	unsigned long sptr, uptr = (unsigned long)base;
	struct sl_buffer *slb;
	int found=0, freeslb=0;

	BMPCSLABS_LOCK();
	PLL_FOREACH(slb, &bmpcSlabs.bmms_slbs) {
		sptr = (unsigned long)slb->slb_base;
		if (uptr >= sptr &&
		    (uptr < sptr + (BMPC_SLB_NBLKS * BMPC_BUFSZ))) {
			found = 1;
			break;
		}
	}
	psc_assert(found);

	psc_assert(!((uptr-sptr) % BMPC_BLKSZ));
	spinlock(&slb->slb_lock);
	psc_vbitmap_unset(slb->slb_inuse, (size_t)(((uptr-sptr) / BMPC_BLKSZ)));

	if ((psc_vbitmap_nfree(slb->slb_inuse) == BMPC_SLB_NBLKS) &&
	    pll_nitems(&bmpcSlabs.bmms_slbs) > BMPC_DEFSLBS) {
		/* The entire slb has been freed, let's remove it
		 *   remove it from the cache and free the memory.
		 */
		pll_remove(&bmpcSlabs.bmms_slbs, slb);
		freeslb = 1;
	}
	freelock(&slb->slb_lock);

	psc_waitq_wakeall(&bmpcSlabs.bmms_waitq);
	BMPCSLABS_ULOCK();

	if (freeslb) {
		bmpc_increase_minage();
		bmpc_slb_free(slb);
	}
}

/**
 * bmpc_mem_alloc - Return a pointer to a single block of cache memory.
 */
void *
bmpc_alloc(void)
{
	struct sl_buffer *slb;
	void *base=NULL;
	size_t elem;
	int nfree, found=0;
	struct timespec ts = { 0, 1000000 };

 retry:
	BMPCSLABS_LOCK();
	PLL_FOREACH(slb, &bmpcSlabs.bmms_slbs) {
		spinlock(&slb->slb_lock);
		if (psc_vbitmap_next(slb->slb_inuse, &elem))
			found = 1;
		freelock(&slb->slb_lock);
		if (found)
			break;
	}

	if (!found) {
		/* bmpc_reap_locked() will drop the lock.
		 */
		nfree = bmpc_reap_locked();
		if (!nfree)
			psc_waitq_waitrel(&bmpcSlabs.bmms_waitq, NULL,
			    &ts);
		goto retry;

	} else {
		BMPCSLABS_ULOCK();
		base = (char *)slb->slb_base + (elem * BMPC_BLKSZ);
		memset(base, 0, BMPC_BLKSZ);
	}

	psc_assert(base);
	return (base);
}

void
bmpc_global_init(void)
{
	struct timespec ts = BMPC_DEF_MINAGE;

	timespecclear(&bmpcSlabs.bmms_minage);
	timespecadd(&bmpcSlabs.bmms_minage, &ts, &bmpcSlabs.bmms_minage);

	INIT_SPINLOCK(&bmpcSlabs.bmms_lock);
	psc_waitq_init(&bmpcSlabs.bmms_waitq);

	pll_init(&bmpcSlabs.bmms_slbs, struct sl_buffer,
	    slb_mgmt_lentry, &bmpcSlabs.bmms_lock);

	psc_poolmaster_init(&bmpcePoolMaster, struct
	    bmap_pagecache_entry, bmpce_lentry,
	    PPMF_AUTO, 512, 512, 16384,
	    bmpce_init, NULL, NULL, "bmpce");

	bmpcePoolMgr = psc_poolmaster_getmgr(&bmpcePoolMaster);

	lc_reginit(&bmpcLru, struct bmap_pagecache, bmpc_lentry,
	    "bmpclru");

	psc_assert(!bmpc_grow(BMPC_DEFSLBS));
}

#if PFL_DEBUG > 0
void
dump_bmpce_flags(uint32_t flags)
{
	int seq = 0;

	PFL_PRFLAG(BMPCE_NEW, &flags, &seq);
	PFL_PRFLAG(BMPCE_GETBUF, &flags, &seq);
	PFL_PRFLAG(BMPCE_DATARDY, &flags, &seq);
	PFL_PRFLAG(BMPCE_DIRTY2LRU, &flags, &seq);
	PFL_PRFLAG(BMPCE_LRU, &flags, &seq);
	PFL_PRFLAG(BMPCE_FREE, &flags, &seq);
	PFL_PRFLAG(BMPCE_FREEING, &flags, &seq);
	PFL_PRFLAG(BMPCE_INIT, &flags, &seq);
	PFL_PRFLAG(BMPCE_READPNDG, &flags, &seq);
	PFL_PRFLAG(BMPCE_RBWPAGE, &flags, &seq);
	PFL_PRFLAG(BMPCE_RBWRDY, &flags, &seq);
	PFL_PRFLAG(BMPCE_INFLIGHT, &flags, &seq);
	PFL_PRFLAG(BMPCE_EIO, &flags, &seq);
	PFL_PRFLAG(BMPCE_READA, &flags, &seq);
	PFL_PRFLAG(BMPCE_AIOWAIT, &flags, &seq);
	PFL_PRFLAG(BMPCE_SYNCWAIT, &flags, &seq);
	if (flags)
		printf(" unknown: %#x", flags);
	printf("\n");
}

void
dump_biorq_flags(uint32_t flags)
{
	int seq = 0;

	PFL_PRFLAG(BIORQ_READ, &flags, &seq);
	PFL_PRFLAG(BIORQ_WRITE, &flags, &seq);
	PFL_PRFLAG(BIORQ_RBWFP, &flags, &seq);
	PFL_PRFLAG(BIORQ_RBWLP, &flags, &seq);
	PFL_PRFLAG(BIORQ_SCHED, &flags, &seq);
	PFL_PRFLAG(BIORQ_INFL, &flags, &seq);
	PFL_PRFLAG(BIORQ_DIO, &flags, &seq);
	PFL_PRFLAG(BIORQ_FORCE_EXPIRE, &flags, &seq);
	PFL_PRFLAG(BIORQ_DESTROY, &flags, &seq);
	PFL_PRFLAG(BIORQ_FLUSHRDY, &flags, &seq);
	PFL_PRFLAG(BIORQ_NOFHENT, &flags, &seq);
	PFL_PRFLAG(BIORQ_APPEND, &flags, &seq);
	PFL_PRFLAG(BIORQ_READAHEAD, &flags, &seq);
	PFL_PRFLAG(BIORQ_RBWFAIL, &flags, &seq);
	PFL_PRFLAG(BIORQ_AIOWAIT, &flags, &seq);
	PFL_PRFLAG(BIORQ_RESCHED, &flags, &seq);
	if (flags)
		printf(" unknown: %#x", flags);
	printf("\n");
}
#endif
