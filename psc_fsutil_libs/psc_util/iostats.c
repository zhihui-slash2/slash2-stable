/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2006-2009, Pittsburgh Supercomputing Center (PSC).
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

#include <stdarg.h>
#include <stdio.h>

#include "psc_ds/lockedlist.h"
#include "psc_util/iostats.h"
#include "psc_util/log.h"

struct psc_lockedlist	psc_iostats =
    PLL_INITIALIZER(&psc_iostats, struct psc_iostats, ist_lentry);

/*
 * I/O statistic interval lengths:
 *	(o) values are in seconds,
 *	(o) must be even divisors and multipliers of each other, and
 *	(o) must be sorted by increasing lengths (1,2,3) not (3,2,1).
 */
int psc_iostat_intvs[] = {
	1,
	10
};

void
psc_iostats_init(struct psc_iostats *ist, const char *fmt, ...)
{
	va_list ap;
	int rc;

	memset(ist, 0, sizeof(*ist));

	va_start(ap, fmt);
	rc = vsnprintf(ist->ist_name, sizeof(ist->ist_name), fmt, ap);
	va_end(ap);
	if (rc == -1 || rc >= (int)sizeof(ist->ist_name))
		psc_fatal("vsnprintf");

	pll_add(&psc_iostats, ist);
}

void
psc_iostats_rename(struct psc_iostats *ist, const char *fmt, ...)
{
	va_list ap;
	int rc;

	PLL_LOCK(&psc_iostats);

	va_start(ap, fmt);
	rc = vsnprintf(ist->ist_name, sizeof(ist->ist_name), fmt, ap);
	va_end(ap);

	PLL_ULOCK(&psc_iostats);

	if (rc == -1 || rc >= (int)sizeof(ist->ist_name))
		psc_fatal("vsnprintf");
}

void
psc_iostats_intv_add(struct psc_iostats *ist, uint64_t amt)
{
	psc_atomic64_add(&ist->ist_intv[0].istv_len, amt);
	psc_atomic64_add(&ist->ist_len_total, amt);
}
