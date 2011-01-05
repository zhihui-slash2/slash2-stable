/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2006-2010, Pittsburgh Supercomputing Center (PSC).
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

/*
 * Locked lists are thread-safe linked lists.
 */

#ifndef _PFL_LOCKEDLIST_H_
#define _PFL_LOCKEDLIST_H_

#include <sys/types.h>

#include "psc_ds/list.h"
#include "psc_util/lock.h"
#include "psc_util/log.h"

struct psc_lockedlist {
	struct psclist_head	 pll_listhd;		/* this must be first */
	int			 pll_nitems;		/* # items on list */
	int			 pll_flags;		/* see PLLF_* below */
	ptrdiff_t		 pll_offset;		/* offset of the sub-structure linkage */
	union {
		psc_spinlock_t	 pllu_lock;
		psc_spinlock_t	*pllu_lockp;
	} pll_u;
#define pll_lockp	pll_u.pllu_lockp
#define pll_lock	pll_u.pllu_lock
};

#define PLLF_EXTLOCK		(1 << 0)		/* lock is external */
#define PLLF_NOLOG		(1 << 1)		/* don't log locks/unlocks */
#define _PLLF_FLSHFT		(1 << 2)

#define PLL_GETLOCK(pll)	((pll)->pll_flags & PLLF_EXTLOCK ?	\
				 (pll)->pll_lockp : &(pll)->pll_lock)

#define PLL_LOCK(pll)		spinlock(PLL_GETLOCK(pll))
#define PLL_ULOCK(pll)		freelock(PLL_GETLOCK(pll))
#define PLL_TRYLOCK(pll)	trylock(PLL_GETLOCK(pll))
#define PLL_RLOCK(pll)		reqlock(PLL_GETLOCK(pll))
#define PLL_TRYRLOCK(pll, lk)	tryreqlock(PLL_GETLOCK(pll), (lk))
#define PLL_URLOCK(pll, lk)	ureqlock(PLL_GETLOCK(pll), (lk))
#define PLL_ENSURE_LOCKED(pll)	LOCK_ENSURE(PLL_GETLOCK(pll))
#define PLL_HASLOCK(pll)	psc_spin_haslock(PLL_GETLOCK(pll))

#define PLL_FOREACH(p, pll)						\
	psclist_for_each_entry2((p), &(pll)->pll_listhd, (pll)->pll_offset)

#define PLL_FOREACH_SAFE(p, t, pll)					\
	psclist_for_each_entry2_safe((p), (t), &(pll)->pll_listhd,	\
	    (pll)->pll_offset)

#define PLL_FOREACH_BACKWARDS(p, pll)					\
	psclist_for_each_entry2_backwards((p), &(pll)->pll_listhd,	\
	    (pll)->pll_offset)

#define PLL_INIT(pll, type, member)					\
	{ PSCLIST_HEAD_INIT((pll)->pll_listhd), 0, 0,			\
	  offsetof(type, member), { SPINLOCK_INIT } }

#define PLL_INIT_NOLOG(pll, type, member)				\
	{ PSCLIST_HEAD_INIT((pll)->pll_listhd), 0, PLLF_NOLOG,		\
	  offsetof(type, member), { SPINLOCK_INIT_NOLOG } }

#define pll_init(pll, type, member, lock)				\
	_pll_init((pll), offsetof(type, member), (lock))

#define pll_empty(pll)		(pll_nitems(pll) == 0)

#define PLLBF_HEAD		0
#define PLLBF_TAIL		(1 << 1)
#define PLLBF_PEEK		(1 << 2)

#define pll_addstack(pll, p)	_pll_add((pll), (p), PLLBF_HEAD)
#define pll_addqueue(pll, p)	_pll_add((pll), (p), PLLBF_TAIL)
#define pll_addhead(pll, p)	_pll_add((pll), (p), PLLBF_HEAD)
#define pll_addtail(pll, p)	_pll_add((pll), (p), PLLBF_TAIL)
#define pll_add(pll, p)		_pll_add((pll), (p), PLLBF_TAIL)

#define pll_gethead(pll)	_pll_get((pll), PLLBF_HEAD)
#define pll_gettail(pll)	_pll_get((pll), PLLBF_TAIL)
#define pll_getstack(pll)	_pll_get((pll), PLLBF_HEAD)
#define pll_getqueue(pll)	_pll_get((pll), PLLBF_HEAD)
#define pll_get(pll)		_pll_get((pll), PLLBF_HEAD)
#define pll_peekhead(pll)	_pll_get((pll), PLLBF_HEAD | PLLBF_PEEK)
#define pll_peektail(pll)	_pll_get((pll), PLLBF_TAIL | PLLBF_PEEK)

void  _pll_add(struct psc_lockedlist *, void *, int);
void   pll_add_sorted(struct psc_lockedlist *, void *, int (*)(const
	    void *, const void *));
int    pll_conjoint(struct psc_lockedlist *, void *);
void *_pll_get(struct psc_lockedlist *, int);
void  _pll_init(struct psc_lockedlist *, int, psc_spinlock_t *);
void   pll_remove(struct psc_lockedlist *, void *);
void   pll_sort(struct psc_lockedlist *, void (*)(void *, size_t,
	    size_t, int (*)(const void *, const void *)), int (*)(const void *,
	    const void *));

static __inline struct psc_listentry *
_pll_obj2entry(struct psc_lockedlist *pll, void *p)
{
	psc_assert(p);
	return ((void *)((char *)p + pll->pll_offset));
}

static __inline void *
_pll_entry2obj(struct psc_lockedlist *pll, struct psc_listentry *e)
{
	psc_assert(e);
	return ((char *)e - pll->pll_offset);
}

static __inline int
pll_nitems(struct psc_lockedlist *pll)
{
	int n, locked;

	locked = PLL_RLOCK(pll);
	n = pll->pll_nitems;
	PLL_URLOCK(pll, locked);
	return (n);
}

static __inline int
psc_listhd_empty_locked(psc_spinlock_t *lk, struct psclist_head *hd)
{
	int locked, empty;

	locked = reqlock(lk);
	empty = psc_listhd_empty(hd);
	ureqlock(lk, locked);
	return (empty);
}

#endif /* _PFL_LOCKEDLIST_H_ */
