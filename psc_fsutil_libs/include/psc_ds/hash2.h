/* $Id$ */

#ifndef _PFL_HASH2_H_
#define _PFL_HASH2_H_

#include "psc_types.h"
#include "psc_ds/list.h"
#include "psc_ds/lockedlist.h"
#include "psc_util/atomic.h"
#include "psc_util/lock.h"

#define PSC_HASHTBL_LOCK(t)	spinlock(&(t)->pht_lock)
#define PSC_HASHTBL_ULOCK(t)	freelock(&(t)->pht_lock)

#define PSC_HTNAME_MAX 30

struct psc_hashbkt {
	struct psclist_head	  phb_listhd;
	psc_spinlock_t		  phb_lock;
	atomic_t		  phb_nitems;
};

struct psc_hashtbl {
	char			  pht_name[PSC_HTNAME_MAX];
	struct psclist_head	  pht_lentry;
	psc_spinlock_t		  pht_lock;
	int			  pht_flags;	/* see below */
	int			  pht_idoff;	/* offset into item to item ID */
	int			  pht_hentoff;	/* offset into item to item ID */
	int			  pht_nbuckets;
	struct psc_hashbkt	 *pht_buckets;
	int			(*pht_cmp)(const void *, const void *);
};

struct psc_hashent {
	struct psclist_head	  phe_lentry;
};

/* Table flags. */
#define PHTF_STR	(1 << 0)	/* IDs are strings */

/* Lookup flags. */
#define PHLF_DEL	(1 << 0)	/* find and remove item from table */

#define PSC_HASHTBL_FOREACH_BUCKET(b, t)				\
	for ((b) = (t)->pht_buckets;					\
	    (b) - (t)->pht_buckets < (t)->pht_nbuckets;			\
	    (b)++)

#define PSC_HASHBKT_FOREACH_ENTRY(t, p, b)				\
	psclist_for_each_entry2(p, &(b)->phb_listhd, (t)->pht_hentoff)

/**
 * psc_hashtbl_init - initialize a hash table.
 * @flags: modifier flags.
 * @flags: modifier flags.
 * @t: hash table to initialize.
 * @nbuckets: number of buckets to create.
 * @fmt: name of hash for lookups and external control.
 */
#define psc_hashtbl_init(t, flags, type, idmemb, hentmemb, nb, cmp,	\
	    fmt, ...)							\
	_psc_hashtbl_init((t), (flags), offsetof(type, idmemb),		\
	    offsetof(type, hentmemb), (nb), (cmp), (fmt), ## __VA_ARGS__)

/**
 * psc_hashtbl_search - search a hash table for an item by its hash ID.
 * @t: the hash table.
 * @cmp: optional value to compare with to differentiate entries with same ID.
 * @cbf: optional callback routine invoked when the entry is found, executed
 *	while the bucket is locked.
 * The variable argument list should consist of a single argument of either type of:
 *	- uint64_t hash ID value
 *	- const char * string ID
 */
#define psc_hashtbl_search(t, cmp, cbf, ...)				\
	_psc_hashtbl_search((t), 0, (cmp), (cbf), ## __VA_ARGS__)

/**
 * psc_hashtbl_del_item - search a hash table for an item by its hash ID
 *	and remove and return if found.
 * @t: the hash table.
 * @cmp: optional value to compare with to differentiate entries with same ID.
 * The variable argument list should consist of a single argument of either type of:
 *	- uint64_t hash ID value
 *	- const char * string ID
 */
#define psc_hashtbl_del_item(t, cmp, ...)				\
	_psc_hashtbl_search((t), 0, (cmp), (cbf), ## __VA_ARGS__)

struct psc_hashtbl *
	 psc_hashtbl_lookup(const char *);
void	 psc_hashtbl_add_item(const struct psc_hashtbl *, void *);
void	 psc_hashtbl_prstats(const struct psc_hashtbl *);
void	 psc_hashtbl_getstats(const struct psc_hashtbl *, int *, int *, int *, int *);
void	 psc_hashtbl_destroy(struct psc_hashtbl *);
void	 psc_hashtbl_remove(const struct psc_hashtbl *, void *);
void	*_psc_hashtbl_search(const struct psc_hashtbl *, int, const void *,
	    void (*)(void *), ...);
void	_psc_hashtbl_init(struct psc_hashtbl *, int, int, int, int,
	    int (*)(const void *, const void *), const char *, ...);

/**
 * psc_hashbkt_search - search a bucket for an item by its hash ID.
 * @t: the hash table.
 * @b: the bucket to search.
 * @cmp: optional value to compare with to differentiate entries with same ID.
 * @cbf: optional callback routine invoked when the entry is found, executed
 *	while the bucket is locked.
 * The variable argument list should consist of a single argument of either type of:
 *	- uint64_t hash ID value
 *	- const char * string ID
 */
#define	psc_hashbkt_search(t, b, cmp, cbf, ...)				\
	_psc_hashbkt_search((t), (b), 0, (cmp), (cbf), ## __VA_ARGS__)

/**
 * psc_hashtbl_del_item - search a bucket for an item by its hash ID and
 *	remove and return if found.
 * @t: the hash table.
 * @b: the bucket to search.
 * @cmp: optional value to compare with to differentiate entries with same ID.
 * The variable argument list should consist of a single argument of either type of:
 *	- uint64_t hash ID value
 *	- const char * string ID
 */
#define	psc_hashbkt_del_item(t, b, cmp, ...)				\
	_psc_hashbkt_search((t), (b), PHLF_DEL, (cmp), NULL,		\
	    ## __VA_ARGS__)

struct psc_hashbkt *
	 psc_hashbkt_get(const struct psc_hashtbl *, ...);
void	 psc_hashbkt_add_item(const struct psc_hashtbl *,
		struct psc_hashbkt *, void *);
void	*_psc_hashbkt_search(const struct psc_hashtbl *,
		struct psc_hashbkt *, int, const void *,
		void (*)(void *), ...);

#define psc_hashbkt_lock(b)	spinlock(&(b)->phb_lock)
#define psc_hashbkt_unlock(b)	freelock(&(b)->phb_lock)

void	 psc_hashent_init(const struct psc_hashtbl *, void *p);

extern struct psc_lockedlist psc_hashtbls;

#endif /* _PFL_HASH2_H_ */
