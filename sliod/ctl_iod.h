/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2006-2010, Pittsburgh Supercomputing Center (PSC).
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

/*
 * Interface for controlling live operation of a sliod instance.
 */

#include "psc_rpc/rpc.h"

#include "fid.h"
#include "slconfig.h"
#include "sltypes.h"

struct slictlmsg_replwkst {
	struct slash_fidgen	srws_fg;
	char			srws_peer_addr[RESM_ADDRBUF_SZ];
	sl_bmapno_t		srws_bmapno;
	uint32_t		srws_data_tot;
	uint32_t		srws_data_cur;
	/* XXX #inflight slivers? */
};

/* sliricthr thread stat aliases */
#define pcst_nwrite		pcst_u32_1

/* sliod message types */
#define SLICMT_GET_REPLWKST	NPCMT
#define SLICMT_GETCONNS		(NPCMT + 1)
#define SLICMT_GETFILES		(NPCMT + 2)

/* sliod control commands */
#define SICC_EXIT		0
#define SICC_RECONFIG		1
