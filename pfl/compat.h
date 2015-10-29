/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2006-2015, Pittsburgh Supercomputing Center (PSC).
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

#ifndef _PFL_COMPAT_H_
#define _PFL_COMPAT_H_

#include <sys/param.h>

#include <errno.h>

#define _PFLERR_START	500

#if defined(ELAST) && ELAST >= _PFLERR_START
#  error system error codes into application space, need to adjust and recompile
#endif

#ifndef ECOMM
#  define ECOMM		EPROTO
#endif

#ifdef HAVE_QSORT_R_THUNK
#  define pfl_qsort_r(base, nel, wid, cmpf, arg)			\
	qsort_r((base), (nel), (wid), (arg), (cmpf))
#else
#  define pfl_qsort_r(base, nel, wid, cmpf, arg)			\
	qsort_r((base), (nel), (wid), (cmpf), (arg))
#endif

#ifndef HAVE_QSORT_R
void qsort_r(void *, size_t, size_t,
    int (*)(const void *, const void *, void *), void *);
#endif

#endif /* _PFL_COMPAT_H_ */