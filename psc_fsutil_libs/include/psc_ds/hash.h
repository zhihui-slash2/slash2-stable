/* $Id: zestHash.h 2189 2007-11-07 22:18:18Z yanovich $ */

#ifndef HAVE_ZEST_HASH_INC
#define HAVE_ZEST_HASH_INC

#include <string.h>

#include "ds/list.h"
#include "ds/lock.h"
#include "psc_types.h"

#define LOCK_BUCKET(b)  spinlock(&((b)->hbucket_lock))
#define ULOCK_BUCKET(b) freelock(&(b)->hbucket_lock)
// Not sure if GET_BUCKET is correct?
#define GET_BUCKET(t,i) &(t)->htable_buckets[(i) % (t)->htable_size]

#define HTNAME_MAX 30

struct hash_bucket {
	struct psclist_head	  hbucket_list;	/* Entry list head */
	zest_spinlock_t		  hbucket_lock;	/* Spinlock for this bucket */
};

struct hash_table {
	char			  htable_name[HTNAME_MAX];
	struct psclist_head	  htable_entry;

	int			  htable_size;
	int			  htable_strlen_max;
	psc_spinlock_t		  htable_lock;
	struct hash_bucket	 *htable_buckets;
	int			(*htcompare)(const void *, const void *);
};

#define HASH_BUCKET_SZ sizeof(struct hash_bucket)

struct hash_entry {
	struct psclist_head	  hentry_list;	/* Entry list pointers */
	u64			 *hentry_id;	/* Pointer to the hash element id */
	void			 *private;	/* pointer to private data */
};

/**
 * hash_entry - get the struct for this entry
 * @ptr:	the &struct hash_entry pointer.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the hash_entry struct within the struct.
 */
#define hash_entry(ptr, type, member) \
	((type *)((char *)(ptr)-(unsigned long)(&((type *)0)->member)))

/*
 * String Hash Defines
 */
#define SGET_BUCKET(t,i) &(t)->htable_buckets[str_hash((i)) % (t)->htable_size]

struct hash_entry_str {
	struct psclist_head	  hentry_str_list; /* Entry list pointers */
	const char		 *hentry_str_id; /* Pointer to the hash element str */
	void			 *private;	/* pointer to private data */
};

/*
 * "This well-known hash function
 *    was used in P.J. Weinberger's C compiler
 *    (cf. Compilers: Principles, Techniques,
 *    and Tools, by Aho, Sethi & Ullman,
 *    Addison-Wesley, 1988, p. 436)."
 */
static inline int
str_hash(const char *s)
{
	const char *p;
	unsigned h = 0, g;

	if ( s == NULL )
		return -1;

	for (p = s; *p != '\0'; p++) {
		h = (h << 4) + (*p);
		if ((g = h & 0xf0000000)) {
			h = h ^ (g >> 24);
			h = h ^ g;
		}
	}
#ifdef LCKTEST
	return 63;
#else
	return (h);
#endif
}

/*<<<<<<<<<<  The below prototypes are auto-generated by fillproto */

/* $Id: pscHash.h 2189 2007-11-07 22:18:18Z yanovich $ */
extern
struct psclist_head hashTablesList;

/* no comments found for this variable */
extern
psc_spinlock_t hashTablesListLock;

/**
 * init_hash_table	-   initialize a hash table
 * @hash_table:	pointer to the hash table array
 * @size:       size of the hash table
 */
void
init_hash_table(struct hash_table *t, int size, const char *fmt, ...);

/**
 * init_hash_entry	-   initialize a hash entry
 * @hash_entry: pointer to the hash_entry which will be initialized
 * @id: pointer to the array of hash_entry pointers
 * @private:    application data to be stored in the hash
 */
void
init_hash_entry(struct hash_entry *hentry, u64 *id, void *private);

/**
 * get_hash_entry	-   locate an address in the hash table
 * @t:		hash table pointer
 * @id:		identifier used to get hash bucket
 */
struct hash_entry *
get_hash_entry(const struct hash_table *h, u64 id, const void *comp);

/**
 * del_hash_entry	-   remove an entry in the hash table
 * @t:		pointer to hash table
 * @id:		identifier used to get hash bucket
 *
 * Returns 0 on success, -1 if entry was not found.
 */
int
del_hash_entry(struct hash_table *h, u64 id);

/**
 * add_hash_entry	-   add an entry in the hash table
 * @t:				pointer to the hash table
 * @entry:		pointer to entry to be added
 */
void
add_hash_entry(struct hash_table *t, struct hash_entry *e);

/**
 * init_hash_entry_str	-   initialize a string hash entry
 * @hentry: 	pointer to the hash_entry which will be initialized
 * @id: 			pointer to the array of hash_entry pointers
 * @private:  application data to be stored in the hash
 */
void
init_hash_entry_str(struct hash_entry_str *hentry, const char *id,
    void *private);

/**
 * get_hash_entry_str	-   locate an address in the hash table
 * @h:		pointer to the hash table
 * @id:		identifier used to get hash bucket
 */
struct hash_entry_str *
get_hash_entry_str(struct hash_table *h, const char *id);

/**
 * del_hash_entry_str	-   remove an entry in the hash table
 * @t:		pointer to the hash table
 * @size:	the match string
 */
int
del_hash_entry_str(struct hash_table *h, char *id);

/**
 * add_hash_entry_str	-  add an entry in the hash table
 * @t:				pointer to the hash table
 * @entry:		pointer to entry to be added
 */
void
add_hash_entry_str(struct hash_table *t, struct hash_entry_str *e);

/**
 * hash_table_stats - query a hash table for its bucket usage stats.
 * @t: pointer to the hash table.
 * @totalbucks: value-result pointer to # of buckets available.
 * @usedbucks: value-result pointer to # of buckets in use.
 * @nents: value-result pointer to # items in hash table.
 * @maxbucklen: value-result pointer to maximum bucket length.
 */
void
hash_table_stats(struct hash_table *t, int *totalbucks, int *usedbucks,
    int *nents, int *maxbucklen);

/**
 * hash_table_printstats - print hash table bucket usage stats.
 * @t: pointer to the hash table.
 */
void
hash_table_printstats(struct hash_table *t);

/*<<<<<<<<<<   This is end of the auto-generated output from fillproto. */

#endif /* HAVE_PSC_HASH_INC */
