/* $Id$ */

#ifndef __PFL_BITFLAG_H__
#define __PFL_BITFLAG_H__

#include "psc_util/assert.h"
#include "psc_util/cdefs.h"
#include "psc_util/lock.h"

#define BIT_STRICT		(1 << 0)
#define BIT_ABORT		(1 << 1)

/*
 * bitflag_sorc - check and/or set flags on a variable.
 * @f: flags variable to perform operations on.
 * @lck: optional spinlock.
 * @checkon: values to ensure are enabled.
 * @checkoff: values to ensure are disabled.
 * @turnon: values to enable.
 * @turnoff: values to disable.
 * @flags: settings which dictate operation of this routine.
 * Notes: returns -1 on failure, 0 on success.
 */
static __inline int
bitflag_sorc(int *f, psc_spinlock_t *lck, int checkon, int checkoff,
    int turnon, int turnoff, int flags)
{
	int strict, locked;

	strict = ATTR_ISSET(flags, BIT_STRICT);

	if (lck)
		locked = reqlock(lck);

	/* check on bits */
	if (checkon &&
	    (!ATTR_HASANY(*f, checkon) ||
	     (strict && !ATTR_HASALL(*f, checkon))))
		goto error;

	/* check off bits */
	if (checkoff &&
	    (ATTR_HASALL(*f, checkoff) ||
	     (strict && ATTR_HASANY(*f, checkoff))))
		goto error;

	/* strict setting mandates turn bits be in negated state */
	if (strict &&
	    ((turnon && ATTR_HASANY(*f, turnon)) ||
	     (turnoff && ATTR_HASANY(~(*f), turnoff))))
		goto error;

	/* set on bits */
	if (turnon)
		*f |= turnon;

	/* unset off bits */
	if (turnoff)
		*f &= ~turnoff;

	if (lck)
		ureqlock(lck, locked);
	return (1);
 error:
	if (lck)
		ureqlock(lck, locked);
	psc_assert((flags & BIT_ABORT) == 0);
	return (0);
}

#endif /* __PFL_BITFLAG_H__ */
