/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2009-2015, Pittsburgh Supercomputing Center (PSC).
 *
 * Permission to use, copy, modify, and distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice
 * appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL
 * THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Pittsburgh Supercomputing Center	phone: 412.268.4960  fax: 412.268.5832
 * 300 S. Craig Street			e-mail: remarks@psc.edu
 * Pittsburgh, PA 15213			web: http://www.psc.edu/
 * -----------------------------------------------------------------------------
 * %PSC_END_COPYRIGHT%
 */

#ifndef _PFL_COMPLETION_H_
#define _PFL_COMPLETION_H_

#include "pfl/lock.h"
#include "pfl/waitq.h"

struct psc_compl {
	struct psc_waitq	pc_wq;
	struct psc_spinlock	pc_lock;
	int			pc_counter;
	int			pc_done;
	int			pc_rc;		/* optional "barrier" value */
};

#define PSC_COMPL_INIT		{ PSC_WAITQ_INIT, SPINLOCK_INIT, 0, 0, 1 }

#define psc_compl_ready(pc, rc)	_psc_compl_ready((pc), (rc), 0)
#define psc_compl_one(pc, rc)	_psc_compl_ready((pc), (rc), 1)

#define psc_compl_wait(pc)	 psc_compl_waitrel_s((pc), NULL, 0)

void	 psc_compl_destroy(struct psc_compl *);
void	 psc_compl_init(struct psc_compl *);
void	_psc_compl_ready(struct psc_compl *, int, int);
int	 psc_compl_waitrel_s(struct psc_compl *, struct psc_spinlock *, int);

#endif /* _PFL_COMPLETION_H_ */
