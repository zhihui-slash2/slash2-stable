/* $Id$ */

#include "psc_ds/list.h"
#include "psc_ds/listcache.h"
#include "psc_util/lock.h"

__static psc_spinlock_t pscListCachesLock = LOCK_INITIALIZER;
struct psc_lockedlist pscListCaches =
    PLL_INITIALIZER(&pscListCaches, struct psc_listcache,
    lc_index_lentry, &pscListCachesLock);
