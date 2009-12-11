/* $Id$ */

#ifndef _PFL_PTHRUTIL_H_
#define _PFL_PTHRUTIL_H_

#include <pthread.h>

void	psc_pthread_mutex_init(pthread_mutex_t *);
void	psc_pthread_mutex_lock(pthread_mutex_t *);
void	psc_pthread_mutex_unlock(pthread_mutex_t *);
int	psc_pthread_mutex_reqlock(pthread_mutex_t *);
void	psc_pthread_mutex_ureqlock(pthread_mutex_t *, int);

#define psc_pthread_mutex_ensure_locked(mutex)				\
	psc_assert(pthread_mutex_trylock(mutex) == EDEADLK)

#endif /* _PFL_PTHRUTIL_H_ */
