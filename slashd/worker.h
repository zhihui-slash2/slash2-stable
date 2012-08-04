/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2012, Pittsburgh Supercomputing Center (PSC).
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

#ifndef _WORKER_H_
#define _WORKER_H_

#include "psc_ds/list.h"
#include "psc_ds/listcache.h"

struct pfl_workrq {
	int			(*wkrq_cbf)(void *);
	struct psc_listentry	  wkrq_lentry;
};

#define pfl_workq_getitem(cb, type)	_pfl_workq_getitem((cb), sizeof(type))

#define	pfl_workq_lock()		LIST_CACHE_LOCK(&pfl_workq)
#define	pfl_workq_unlock()		LIST_CACHE_LOCK(&pfl_workq)
#define	pfl_workq_waitempty()		psc_waitq_wait(&pfl_workq.plc_wq_want,	\
					    &pfl_workq.plc_lock)

void   pfl_wkthr_spawn(int, int, const char *);
void *_pfl_workq_getitem(int (*)(void *), size_t);
void   pfl_workq_init(size_t);
void   pfl_workq_putitem(void *);

extern struct psc_listcache	pfl_workq;

#endif /* _WORKER_H_ */
