/* $Id$ */
/*
 * %GPL_START_LICENSE%
 * ---------------------------------------------------------------------
 * Copyright 2015, Google, Inc.
 * Copyright (c) 2010-2015, Pittsburgh Supercomputing Center (PSC).
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

/*
 * This API implements the core READDIR dirents caching into buffers
 * called 'dircache_page'.
 *
 * TODO
 *  - use the fuse async API to automatically insert entries into the
 *    name cache after READDIR instead of waiting for LOOKUPs
 */

#include <sys/types.h>
#include <sys/time.h>

#include <stdint.h>
#include <string.h>

#include "pfl/alloc.h"
#include "pfl/ctlsvr.h"
#include "pfl/dynarray.h"
#include "pfl/fs.h"
#include "pfl/listcache.h"
#include "pfl/lock.h"
#include "pfl/opstats.h"
#include "pfl/pool.h"
#include "pfl/str.h"
#include "pfl/workthr.h"

#include "dircache.h"
#include "fidcache.h"
#include "mount_slash.h"
#include "sltypes.h"
#include "slutil.h"

struct psc_poolmaster	 dircache_page_poolmaster;
struct psc_poolmgr	*dircache_page_pool;

struct psc_poolmaster	 dircache_ent_poolmaster;
struct psc_poolmgr	*dircache_ent_pool;

struct psc_lockedlist	 msl_dircache_pages_lru;

/*
 * Initialize per-fcmh dircache structures.
 */
void
dircache_init(struct fidc_membh *d)
{
	struct fcmh_cli_info *fci = fcmh_2_fci(d);

	if ((d->fcmh_flags & FCMH_CLI_INITDIRCACHE) == 0) {
		pll_init(&fci->fci_dc_pages, struct dircache_page,
		    dcp_lentry, &d->fcmh_lock);
		pfl_rwlock_init(&fci->fcid_dircache_rwlock);
		d->fcmh_flags |= FCMH_CLI_INITDIRCACHE;
	}
}

/*
 * Initialize global dircache structures.
 */
void
dircache_mgr_init(void)
{
	psc_poolmaster_init(&dircache_page_poolmaster,
	    struct dircache_page, dcp_lentry, PPMF_AUTO,
	    DIRCACHE_NPAGES, DIRCACHE_NPAGES, 0, NULL, NULL, NULL,
	    "dircachepg");
	dircache_page_pool = psc_poolmaster_getmgr(
	    &dircache_page_poolmaster);

	psc_poolmaster_init(&dircache_ent_poolmaster,
	    struct dircache_ent, dce_lentry, PPMF_AUTO, DIRCACHE_NPAGES,
	    DIRCACHE_NPAGES, 0, NULL, NULL, NULL, "dircachent");
	dircache_ent_pool = psc_poolmaster_getmgr(
	    &dircache_ent_poolmaster);
}

/*
 * Perform a dircache_ent comparison for use by the hash table API to
 * disambiguate entries with the same hash key.
 */
int
dircache_ent_cmp(const void *a, const void *b)
{
	const struct dircache_ent *da = a, *db = b;

	return (da->dce_pfid == db->dce_pfid &&
	    da->dce_pfd->pfd_namelen == db->dce_pfd->pfd_namelen &&
	    strncmp(da->dce_pfd->pfd_name, db->dce_pfd->pfd_name,
	    da->dce_pfd->pfd_namelen) == 0);
}

/*
 * Perform a comparison for use by the hash table search to disambiguate
 * entries with the same hash key against a query structures.
 */
int
dircache_entq_cmp(const void *a, const void *b)
{
	const struct dircache_ent_query *da = a;
	const struct dircache_ent *db = b;

	return (da->dcq_pfid == db->dce_pfid &&
	    da->dcq_namelen == db->dce_pfd->pfd_namelen &&
	    strncmp(da->dcq_name, db->dce_pfd->pfd_name,
	    da->dcq_namelen) == 0);
}

/*
 * Release memory for a dircache_ent.
 *
 * If the entry came from a READDIR page, it is marked as stale and the
 * entire page is marked as expired so it is not used.  The entry itself
 * is not released so as to not screw with the page dcp_dents_off list
 * (although the page shouldn't be used).
 *
 * If the entry came from a individual population, it is removed and
 * memory released.
 */
void
dircache_ent_zap(struct fidc_membh *d, struct dircache_ent *dce)
{
	struct fcmh_cli_info *fci;
	struct dircache_page *p;

	OPSTAT_INCR("dircache-ent-zap");

	PFLOG_DIRCACHENT(PLL_DEBUG, dce, "delete");

	fci = fcmh_2_fci(d);
	DIRCACHE_WRLOCK(d);
	p = dce->dce_page;
	dce->dce_pfd->pfd_ino = FID_ANY;
	if (p) {
		/* force expiration of the entire page */
		PFL_GETPTIMESPEC(&p->dcp_local_tm);
		p->dcp_local_tm.tv_sec -= DIRCACHEPG_HARD_TIMEO;
	} else {
		PSCFREE(dce->dce_pfd);
		psc_dynarray_removeitem(&fci->fcid_ents, dce);
	}
	DIRCACHE_ULOCK(d);

	if (p == NULL)
		psc_pool_return(dircache_ent_pool, dce);
}

/*
 * Release a page of dirents from cache.
 * @d: directory handle.
 * @p: page to release.
 * @wait: whether to wait for all other references to release.
 */
int
_dircache_free_page(const struct pfl_callerinfo *pci,
    struct fidc_membh *d, struct dircache_page *p, int wait)
{
	struct fcmh_cli_info *fci;
	struct dircache_ent *dce;
	int i;

	DIRCACHE_WR_ENSURE(d);
	fci = fcmh_2_fci(d);

	if (p->dcp_flags & DIRCACHEPGF_FREEING)
		return (0);

	if (p->dcp_refcnt && !wait)
		return (0);

	p->dcp_flags |= DIRCACHEPGF_FREEING;

	if ((p->dcp_flags & DIRCACHEPGF_READ) == 0)
		OPSTAT_INCR("dircache-unused-page");

	while (p->dcp_refcnt)
		DIRCACHE_WAIT(d);

	pll_remove(&fci->fci_dc_pages, p);

	if (p->dcp_dents_off) {
		DYNARRAY_FOREACH(dce, i, p->dcp_dents_off) {
			PFLOG_DIRCACHENT(PLL_DEBUG, dce, "free_page");
			if (dce->dce_pfd->pfd_ino != FID_ANY)
				psc_hashent_remove(
				    &msl_namecache_hashtbl, dce);
			psc_pool_return(dircache_ent_pool, dce);
		}
		psc_dynarray_free(p->dcp_dents_off);
		PSCFREE(p->dcp_dents_off);
	}
	PSCFREE(p->dcp_base);
	PFLOG_DIRCACHEPG(PLL_DEBUG, p, "free dir=%p", d);
	psc_pool_return(dircache_page_pool, p);

	DIRCACHE_WAKE(d);

	return (1);
}

/*
 * Perform a batch operation across all cached dirents.
 * @d: directory handle.
 * @cbf: callback to run.
 * @cbarg: callback argument.
 */
int
dircache_walk_wkcb(void *arg)
{
	struct slc_wkdata_dircache *wk = arg;
	struct dircache_page *p, *np;
	struct fcmh_cli_info *fci;
	struct dircache_ent *dce;
	int n;

	fci = fcmh_2_fci(wk->d);
	DIRCACHE_RDLOCK(wk->d);
	PLL_FOREACH_SAFE(p, np, &fci->fci_dc_pages) {
		if (p->dcp_rc || p->dcp_flags & DIRCACHEPGF_LOADING)
			continue;
		DYNARRAY_FOREACH(dce, n, p->dcp_dents_off)
			wk->cbf(p, dce, wk->cbarg);
	}
	DYNARRAY_FOREACH(dce, n, &fci->fcid_ents)
		wk->cbf(NULL, dce, wk->cbarg);
	DIRCACHE_ULOCK(wk->d);

	fcmh_op_done_type(wk->d, FCMH_OPCNT_DIRCACHE);

	if (wk->compl)
		psc_compl_ready(wk->compl, 1);

	return (0);
}

void
dircache_walk_async(struct fidc_membh *d, void (*cbf)(
    struct dircache_page *, struct dircache_ent *, void *), void *cbarg,
    struct psc_compl *compl)
{
	struct slc_wkdata_dircache *wk;

	wk = pfl_workq_getitem(dircache_walk_wkcb,
	    struct slc_wkdata_dircache);
	fcmh_op_start_type(d, FCMH_OPCNT_DIRCACHE);
	wk->d = d;
	wk->cbf = cbf;
	wk->cbarg = cbarg;
	wk->compl = compl;
	pfl_workq_putitem(wk);
}

/*
 * Destroy all dirent pages belonging to a directory.
 * @d: directory handle.
 */
void
dircache_purge(struct fidc_membh *d)
{
	struct dircache_page *p, *np;
	struct fcmh_cli_info *fci;

	fci = fcmh_2_fci(d);
	DIRCACHE_WRLOCK(d);
	PLL_FOREACH_SAFE(p, np, &fci->fci_dc_pages)
		dircache_free_page(d, p);
	DIRCACHE_ULOCK(d);
}

/*
 * Determine if a dircache page contains an entry at the specified
 * directory 'offset' (which is more like a cookie/ID).
 * @d: directory handle.
 * @off: offset of desired dirent.
 */
int
dircache_hasoff(struct dircache_page *p, off_t off)
{
	struct dircache_ent *dce;
	int n;

	if (off == p->dcp_off)
		return (1);
	if (p->dcp_rc)
		return (0);
	if (off == p->dcp_nextoff)
		return (0);

	n = psc_dynarray_bsearch(p->dcp_dents_off, &off,
	    dce_cmp_off_search);
	if (n >= psc_dynarray_len(p->dcp_dents_off))
		return (0);
	dce = psc_dynarray_getpos(p->dcp_dents_off, n);
	return (dce->dce_pfd->pfd_off == (uint64_t)off);
}

/*
 * Allocate a new page of dirents.
 * @d: directory handle.
 * @off: offset into directory for this slew of dirents.
 * @wait: whether this call should be non-blocking or not.
 */
struct dircache_page *
dircache_new_page(struct fidc_membh *d, off_t off, int wait)
{
	struct dircache_page *p, *np, *newp;
	struct dircache_expire dexp;
	struct fcmh_cli_info *fci;

	newp = psc_pool_get(dircache_page_pool);

	DIRCACHE_WRLOCK(d);

 restart:
	DIRCACHEPG_INITEXP(&dexp);

	fci = fcmh_2_fci(d);
	PLL_FOREACH_SAFE(p, np, &fci->fci_dc_pages) {
		if (DIRCACHEPG_EXPIRED(d, p, &dexp)) {
			dircache_free_page(d, p);
			continue;
		}
		if (p->dcp_flags & DIRCACHEPGF_LOADING) {
			if (p->dcp_off == off) {
				if (wait) {
					DIRCACHE_WAIT(d);
					goto restart;
				}

				/*
				 * Someone is already taking care of
				 * this page for us.
				 */
				p = NULL;
				goto out;
			}
			if (wait) {
				DIRCACHE_WAIT(d);
				goto restart;
			}
			continue;
		}
		if (dircache_hasoff(p, off)) {
			/* Stale page in cache; purge and refresh. */
			if (wait)
				dircache_free_page(d, p);
			else if (!dircache_free_page_nowait(d, p)) {
				p = NULL;
				goto out;
			}
			p = NULL;
			break;
		}
	}

	if (p == NULL) {
		memset(newp, 0, sizeof(*newp));
		INIT_PSC_LISTENTRY(&newp->dcp_lentry);
		newp->dcp_off = off;
		pll_addtail(&fci->fci_dc_pages, newp);
		p = newp;
		newp = NULL;
	}
	psc_assert((p->dcp_flags & DIRCACHEPGF_LOADING) == 0);
	p->dcp_flags |= DIRCACHEPGF_LOADING;
	p->dcp_refcnt++;
	PFLOG_DIRCACHEPG(PLL_DEBUG, p, "incref");

 out:
	DIRCACHE_ULOCK(d);

	if (newp)
		psc_pool_return(dircache_page_pool, newp);

	return (p);
}

/*
 * Compute a hash key for a dircache_ent based on the parent
 * directory's FID and the entry's basename.
 * @dce: entry to compute hash for.
 */
uint64_t
dircache_ent_hash(uint64_t pfid, const char *name, size_t namelen)
{
	return (pfid ^ psc_strn_hashify(name, namelen));
}

/*
 * Register directory entries with our cache.
 * @d: directory.
 * @p: buffer of dirent objects.
 * @nents: number of dirent objects in @p.
 * @base: pointer to buffer of pscfs_dirents from RPC.
 * @size: size of @base buffer.
 * @eof: whether this signifies the last READDIR for this directory.
 */
void
dircache_reg_ents(struct fidc_membh *d, struct dircache_page *p,
    size_t nents, void *base, size_t size, int eof)
{
	struct pscfs_dirent *dirent = NULL;
	struct dircache_ent *dce, *dce2;
	struct psc_dynarray *da_off;
	struct psc_hashbkt *b;
	off_t adj;
	int i;

	OPSTAT_INCR("dircache-reg-ents");
	PFLOG_DIRCACHEPG(PLL_DEBUG, p, "registering");

	da_off = PSCALLOC(sizeof(*da_off));
	psc_dynarray_init(da_off);

	psc_dynarray_ensurelen(da_off, nents);

	for (i = 0, adj = 0; i < (int)nents; i++, dce++) {
		dirent = PSC_AGP(base, adj);

		psclog_debug("fid="SLPRI_FID" pfd_off=%"PRId64" "
		    "nlen=%u type=%#o "
		    "name=%.*s dirent=%p adj=%"PRId64,
		    dirent->pfd_ino, dirent->pfd_off,
		    dirent->pfd_namelen, dirent->pfd_type,
		    dirent->pfd_namelen, dirent->pfd_name, dirent, adj);

		dce = psc_pool_get(dircache_ent_pool);
		memset(dce, 0, sizeof(*dce));
		INIT_PSC_LISTENTRY(&dce->dce_lentry);
		dce->dce_page = p;
		dce->dce_pfd = dirent;
		dce->dce_pfid = fcmh_2_fid(d);
		dce->dce_key = dircache_ent_hash(dce->dce_pfid,
		    dce->dce_pfd->pfd_name, dce->dce_pfd->pfd_namelen);
		psc_dynarray_add(da_off, dce);

		PFLOG_DIRCACHENT(PLL_DEBUG, dce, "init");

		adj += PFL_DIRENT_SIZE(dirent->pfd_namelen);
	}

	psc_dynarray_sort(da_off, qsort, dce_sort_cmp_off);

	psc_assert(p->dcp_flags & DIRCACHEPGF_LOADING);

	DYNARRAY_FOREACH(dce, i, da_off) {
		psc_hashent_init(&msl_namecache_hashtbl, dce);
		b = psc_hashbkt_get(&msl_namecache_hashtbl,
		    &dce->dce_key);
#if 0
		if (d->fcmh_sstb.sst_fg.fg_gen !=
		    mq->fg.fg_gen) {
			OPSTAT_INCR("readdir-stale");
			PFL_GOTOERR(out, mp->rc = -PFLERR_STALE);
		}
#endif
		dce2 = _psc_hashbkt_search(&msl_namecache_hashtbl, b, 0,
		    dircache_ent_cmp, dce, NULL, NULL, &dce->dce_key);
		if (dce2 && dce2->dce_flags & DCEF_HOLD) {
			PFLOG_DIRCACHENT(PLL_DEBUG, dce, "skip");
			dce->dce_pfd->pfd_ino = FID_ANY;
		} else {
			if (dce2) {
				psc_hashbkt_del_item(
				    &msl_namecache_hashtbl, b, dce2);
				dircache_ent_zap(d, dce2);
			}
			psc_hashbkt_add_item(&msl_namecache_hashtbl, b,
			    dce);
		}
		psc_hashbkt_put(&msl_namecache_hashtbl, b);
	}

	DIRCACHE_WRLOCK(d);
	psc_assert(p->dcp_dents_off == NULL);

	if (dirent)
		p->dcp_nextoff = dirent->pfd_off;
	else
		p->dcp_nextoff = p->dcp_off;
	p->dcp_dents_off = da_off;
	p->dcp_base = base;
	p->dcp_size = size;
	p->dcp_dirgen = fcmh_2_gen(d);
	PFL_GETPTIMESPEC(&p->dcp_local_tm);
	p->dcp_remote_tm = d->fcmh_sstb.sst_mtim;
	p->dcp_flags &= ~DIRCACHEPGF_LOADING;
	if (eof)
		p->dcp_flags |= DIRCACHEPGF_EOF;
	DIRCACHE_WAKE(d);
	DIRCACHE_ULOCK(d);
}

/*
 * Eradicate all entries in the namecache hashtable cache belonging to
 * the given directory.  We loop through each dircache page and remove
 * all entries and all anonymous (i.e. not referenced by a dircache
 * page) entries referenced by fcid_ents.
 * @d: directory to remove entries for.
 */
void
namecache_purge(struct fidc_membh *d)
{
	struct fcmh_cli_info *fci;
	struct dircache_ent *dce;
	int i;

	fci = fcmh_get_pri(d);
	DYNARRAY_FOREACH(dce, i, &fci->fcid_ents) {
		PFLOG_DIRCACHENT(PLL_DEBUG, dce, "purge");
		psc_hashent_remove(&msl_namecache_hashtbl, dce);
		PSCFREE(dce->dce_pfd);
		psc_pool_return(dircache_ent_pool, dce);
	}
	psc_dynarray_free(&fci->fcid_ents);
}

/*
 * Helper routine for namecache_get_entry() that conditionally marks a
 * dirent as immutable in anticipation of an update.
 */
void
dircache_ent_tryhold(void *p, void *arg)
{
	struct dircache_ent *dce = p;

	if ((dce->dce_flags & DCEF_HOLD) == 0) {
		dce->dce_flags |= DCEF_HOLD;
		PFLOG_DIRCACHENT(PLL_DEBUG, dce, "set HOLD");
		*(int *)arg = 1;
	}
}

/*
 * Grab a dirent from the namecache, optionally inserting it if
 * non-existent.
 */
int
_namecache_get_entry(const struct pfl_callerinfo *pci,
    struct dircache_ent_update *dcu, struct fidc_membh *d,
    const char *name, int blocking)
{
	struct dircache_ent *dce, *new_dce;
	struct dircache_ent_query q;
	struct psc_hashbkt *b;
	int mine = 0;

	dcu->dcu_d = d;

	q.dcq_pfid = fcmh_2_fid(d);
	q.dcq_name = name;
	q.dcq_namelen = strlen(name);
	q.dcq_key = dircache_ent_hash(q.dcq_pfid, name, q.dcq_namelen);
	dcu->dcu_bkt = b = psc_hashbkt_get(&msl_namecache_hashtbl,
	    &q.dcq_key);
 retry_hold:
	dcu->dcu_dce = dce = psc_hashbkt_search_cmpf(
	    &msl_namecache_hashtbl, b, dircache_entq_cmp, &q,
	    dircache_ent_tryhold, &mine, &q.dcq_key);
	if (dce && !mine) {
		if (blocking) {
			psc_hashbkt_unlock(b);
			usleep(1);
			psc_hashbkt_lock(b);
			goto retry_hold;
		}
		psc_hashbkt_put(&msl_namecache_hashtbl, b);
		OPSTAT_INCR("namecache-get-hold");
		return (-1);
	}
	psc_hashbkt_unlock(b);
	if (dce) {
		OPSTAT_INCR("namecache-get-hit");
		return (0);
	}

	new_dce = psc_pool_get(dircache_ent_pool);
	memset(new_dce, 0, sizeof(*new_dce));
	INIT_PSC_LISTENTRY(&new_dce->dce_lentry);
	new_dce->dce_pfd = PSCALLOC(PFL_DIRENT_SIZE(q.dcq_namelen));

 retry_add:
	psc_assert(!mine);
	psc_hashbkt_lock(b);
	dce = psc_hashbkt_search_cmpf(&msl_namecache_hashtbl, b,
	    dircache_entq_cmp, &q, dircache_ent_tryhold, &mine,
	    &q.dcq_key);
	if (dce && !mine) {
		if (blocking) {
			psc_hashbkt_unlock(b);
			usleep(1);
			goto retry_add;
		}
		psc_hashbkt_put(&msl_namecache_hashtbl, b);
		PSCFREE(new_dce->dce_pfd);
		psc_pool_return(dircache_ent_pool, new_dce);
		OPSTAT_INCR("namecache-race-hold");
		return (-1);
	}
	if (dce) {
		OPSTAT_INCR("namecache-race-lost");
	} else {
		OPSTAT_INCR("namecache-race-won");

		dce = new_dce;
		new_dce = NULL;

		dce->dce_key = q.dcq_key;
		dce->dce_pfid = q.dcq_pfid;
		dce->dce_flags = DCEF_HOLD;
		dce->dce_pfd->pfd_ino = FID_ANY;
		dce->dce_pfd->pfd_namelen = q.dcq_namelen;
		strncpy(dce->dce_pfd->pfd_name, name, q.dcq_namelen);
		psc_hashent_init(&msl_namecache_hashtbl, dce);
		psc_hashbkt_add_item(&msl_namecache_hashtbl, b, dce);

		PFLOG_DIRCACHENT(PLL_DEBUG, dce, "init");
	}
	psc_hashbkt_unlock(b);

	if (new_dce) {
		PSCFREE(new_dce->dce_pfd);
		psc_pool_return(dircache_ent_pool, new_dce);
	} else {
		struct fcmh_cli_info *fci;

		fci = fcmh_2_fci(d);
		DIRCACHE_WRLOCK(d);
		psc_dynarray_add(&fci->fcid_ents, dce);
		DIRCACHE_ULOCK(d);
	}

	dcu->dcu_dce = dce;

	return (0);
}

#define namecache_release_entry(h)					\
	_namecache_release_entry((h), 0)
#define namecache_release_entry_locked(h)				\
	_namecache_release_entry((h), 1)

/*
 * Unset 'HOLD' on a dircache_ent and release hash bucket reference.
 * 'HOLD' behaves like a reference count and can't go away until we
 * release
 */
__static void
_namecache_release_entry(struct dircache_ent_update *dcu, int locked)
{
	if (!locked)
		psc_hashbkt_lock(dcu->dcu_bkt);
	psc_assert(dcu->dcu_dce->dce_flags & DCEF_HOLD);
	dcu->dcu_dce->dce_flags &= ~DCEF_HOLD;
	PFLOG_DIRCACHENT(PLL_DEBUG, dcu->dcu_dce, "release HOLD");
	psc_hashbkt_put(&msl_namecache_hashtbl, dcu->dcu_bkt);
}

/*
 * Acquire HOLDs on two entries.  If the HOLD on the second entry isn't
 * immediately successful, release HOLD from the first entry and restart
 * from the beginning to avoid deadlock.
 *
 * This is a dumb approach and could conceivably spin forever, and
 * perhaps an ordering strategy would be better, but it's simple.
 */
void
namecache_get_entries(struct dircache_ent_update *odcu,
    struct fidc_membh *op, const char *oldname,
    struct dircache_ent_update *ndcu,
    struct fidc_membh *np, const char *newname)
{
	for (;;) {
		namecache_hold_entry(odcu, op, oldname);
		if (!namecache_get_entry(ndcu, np, newname))
			break;
		namecache_release_entry(odcu);
	}
}

/*
 * Finish a namecache entry update window, releasing the HOLD setting
 * previously acquired.  If there was an error, release (or potentially
 * delete, if the entry was a placeholder that failed) the entry
 * instead.
 */
void
_namecache_update(const struct pfl_callerinfo *pci,
    struct dircache_ent_update *dcu, uint64_t fid, int rc)
{
	if (dcu->dcu_d == NULL)
		return;
	if (rc) {
		namecache_delete(dcu, -1);
	} else {
		psc_hashbkt_lock(dcu->dcu_bkt);
		dcu->dcu_dce->dce_pfd->pfd_ino = fid;
		namecache_release_entry_locked(dcu);
		OPSTAT_INCR("namecache-update");
	}
	dcu->dcu_d = NULL;
}

/*
 * Remove an entry marked HOLD from the namecache.
 */
void
namecache_delete(struct dircache_ent_update *dcu, int rc)
{
	if (rc && dcu->dcu_dce->dce_pfd->pfd_ino != FID_ANY)
		namecache_release_entry(dcu);
	else {
		psc_hashbkt_lock(dcu->dcu_bkt);
		psc_hashbkt_del_item(&msl_namecache_hashtbl,
		    dcu->dcu_bkt, dcu->dcu_dce);
		psc_hashbkt_put(&msl_namecache_hashtbl, dcu->dcu_bkt);

		dircache_ent_zap(dcu->dcu_d, dcu->dcu_dce);
	}
}

void
dircache_ent_dbgpr(struct dircache_page *p, struct dircache_ent *e,
    void *a)
{
	struct dircache_page **pp = a;

	if (*pp != p) {
		PFLOG_DIRCACHEPG(PLL_MAX, p, "");
		*pp = p;
	}

	fprintf(stderr, "   ent %.*s\n", e->dce_pfd->pfd_namelen,
	    e->dce_pfd->pfd_name);
}

void
dircache_dbgpr(struct fidc_membh *d)
{
	struct slc_wkdata_dircache wk;
	void *p = NULL;

	printf("pages %d\n", pll_nitems(&fcmh_2_fci(d)->fci_dc_pages));
	memset(&wk, 0, sizeof(wk));
	wk.d = d;
	wk.cbf = dircache_ent_dbgpr;
	wk.cbarg = &p;
	dircache_walk_wkcb(&wk);
}
