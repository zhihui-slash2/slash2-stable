/* $Id: lock2.c 8259 2009-10-08 03:15:33Z yanovich $ */

#define TEST_LOCK_INCLUDE	"psc_util/mspinlock.h"
#define TEST_LOCK_TYPE		struct psc_mspinlock
#define TEST_LOCK_INITIALIZER	PMSL_INIT
#define TEST_LOCK_ACQUIRE(lk)	psc_mspin_lock(lk)
#define TEST_LOCK_RELEASE(lk)	psc_mspin_unlock(lk)

#include "../lock/lock_template.c"
