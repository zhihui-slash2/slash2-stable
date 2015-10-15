/* $Id$ */
/*
 * %PSCGPL_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2006-2014, Pittsburgh Supercomputing Center (PSC).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed WITHOUT ANY WARRANTY; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License contained in the file
 * `COPYING-GPL' at the top of this distribution or at
 * https://www.gnu.org/licenses/gpl-2.0.html for more details.
 *
 * Pittsburgh Supercomputing Center	phone: 412.268.4960  fax: 412.268.5832
 * 300 S. Craig Street			e-mail: remarks@psc.edu
 * Pittsburgh, PA 15213			web: http://www.psc.edu/
 * -----------------------------------------------------------------------------
 * %PSC_END_COPYRIGHT%
 */

#ifndef _SLASH_CREDS_H_
#define _SLASH_CREDS_H_

#include <stdint.h>

struct passwd;

struct pscfs_creds;
struct pscfs_clientctx;

struct fidc_membh;
struct srt_stat;

#define SLASH_UID	"_slash"

struct slash_creds {
	uint32_t	scr_uid;
	uint32_t	scr_gid;
};

void	sl_drop_privs(int);
void	sl_getuserpwent(struct passwd **);
int	sl_fcmh_checkacls(struct fidc_membh *,
	    const struct pscfs_clientctx *, const struct pscfs_creds *,
	    int);

int	checkcreds(const struct srt_stat *, const struct pscfs_creds *,
	    int);

#endif /* _SLASH_CREDS_H_ */
