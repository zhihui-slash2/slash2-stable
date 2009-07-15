/* $Id$ */

#ifndef _BMAP_H_
#define _BMAP_H_

#include <inttypes.h>
#include <time.h>

#include "psc_types.h"
#include "psc_ds/tree.h"
#include "psc_rpc/rpc.h"
#include "psc_util/atomic.h"
#include "psc_util/lock.h"
#include "psc_util/waitq.h"

#include "fidcache.h"
#include "fid.h"

struct fidc_membh;
struct fidc_open_obj;

struct bmap_refresh {
	struct slash_fidgen	bmrfr_fg;
	sl_blkno_t		bmrfr_blk;
	u8			bmrfr_flags;
};

/*
 * bmapc_memb - central structure for block map caching used in
 *    all slash service contexts (mds, ios, client).
 *
 * bmapc_memb sits in the middle of the GFC stratum.
 * XXX some of these elements may need to be moved into the bcm_info_pri
 *     area (as part of new structures?) so save space on the mds.
 */

struct bmapc_memb {
	sl_blkno_t		 bcm_blkno;	/* Bmap blk number        */
	struct fidc_membh	*bcm_fcmh;	/* pointer to fid info    */
	void			*bcm_pri;
	atomic_t		 bcm_rd_ref;	/* one ref per write fd    */
	atomic_t		 bcm_wr_ref;	/* one ref per read fd     */
	struct timespec		 bcm_ts;
	atomic_t		 bcm_opcnt;	/* pending opcnt           */
	u32			 bcm_mode;
	psc_spinlock_t		 bcm_lock;
	psc_waitq_t		 bcm_waitq;
	SPLAY_ENTRY(bmapc_memb)	 bcm_tentry;	/* fcm tree entry    */
};

#define BMAP_LOCK_ENSURE(b)	LOCK_ENSURE(&(b)->bcm_lock)
#define BMAP_LOCK(b)		spinlock(&(b)->bcm_lock)
#define BMAP_ULOCK(b)		freelock(&(b)->bcm_lock)
#define BMAP_RLOCK(b)		reqlock(&(b)->bcm_lock)
#define BMAP_URLOCK(b, lk)	ureqlock(&(b)->bcm_lock, lk)

#define DEBUG_BMAP(level, b, fmt, ...)					\
	psc_logs((level), PSS_OTHER,					\
		 " bmap@%p b:%u m:%u i:%"PRIx64				\
		 " rref=%u wref=%u opcnt=%u "fmt,			\
		 (b), (b)->bcm_blkno,					\
		 (b)->bcm_mode,						\
		 (b)->bcm_fcmh ? fcmh_2_fid((b)->bcm_fcmh) : 0,		\
		 atomic_read(&(b)->bcm_rd_ref),				\
		 atomic_read(&(b)->bcm_wr_ref),				\
		 atomic_read(&(b)->bcm_opcnt),				\
		 ## __VA_ARGS__)

#define bmap_set_accesstime(b)						\
	clock_gettime(CLOCK_REALTIME, &(b)->bcm_ts)

void	bmapc_memb_init(struct bmapc_memb *, struct fidc_membh *);
int	bmapc_cmp(const void *, const void *);

SPLAY_PROTOTYPE(bmap_cache, bmapc_memb, bcm_tentry, bmapc_cmp);

static inline struct bmapc_memb *
bmap_lookup_locked(struct fidc_open_obj *fcoo, sl_blkno_t n)
{
	struct bmapc_memb lb, *b;

	lb.bcm_blkno=n;
	b = SPLAY_FIND(bmap_cache, &fcoo->fcoo_bmapc, &lb);
	if (b)
		atomic_inc(&b->bcm_opcnt);

	return (b);
}

static inline struct bmapc_memb *
bmap_lookup(struct fidc_membh *f, sl_blkno_t n)
{
	int locked = reqlock(&f->fcmh_lock);
	struct bmapc_memb *b = bmap_lookup_locked(f->fcmh_fcoo, n);

	ureqlock(&f->fcmh_lock, locked);
	return (b);
}

static inline void
bmap_op_done(struct bmapc_memb *b)
{
	atomic_dec(&b->bcm_opcnt);
}

#endif /* _BMAP_H_ */
