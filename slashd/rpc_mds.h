/* $Id$ */
/*
 * %PSCGPL_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2006-2012, Pittsburgh Supercomputing Center (PSC).
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

#ifndef _RPC_MDS_H_
#define _RPC_MDS_H_

#include "psc_util/multiwait.h"

#include "slconn.h"
#include "slashrpc.h"

struct pscrpc_request;
struct pscrpc_export;

#define SLM_RMM_NTHREADS		8
#define SLM_RMM_NBUFS			1024
#define SLM_RMM_BUFSZ			640
#define SLM_RMM_REPSZ			512
#define SLM_RMM_SVCNAME			"slmrmm"

#define SLM_RMI_NTHREADS		8
#define SLM_RMI_NBUFS			1024
#define SLM_RMI_BUFSZ			768
#define SLM_RMI_REPSZ			1320
#define SLM_RMI_SVCNAME			"slmrmi"

#define SLM_RMC_NTHREADS		32
#define SLM_RMC_NBUFS			1024
#define SLM_RMC_BUFSZ			648
#define SLM_RMC_REPSZ			1024
#define SLM_RMC_SVCNAME			"slmrmc"

enum slm_fwd_op {
	SLM_FORWARD_CREATE,
	SLM_FORWARD_MKDIR,
	SLM_FORWARD_RMDIR,
	SLM_FORWARD_UNLINK,
	SLM_FORWARD_RENAME,
	SLM_FORWARD_SYMLINK,
	SLM_FORWARD_SETATTR
};

/*
 * The number of update or reclaim records saved in the same log file.
 * Each log record is identified by its transaction ID (xid), which is
 * always increasing, but not necessary contiguous.
 *
 * Increasing these values should help logging performance because we
 * can then sync less often.  However, the size of any individual log
 * file must be less than LNET_MTU so it can always be transmitted in
 * a single RPC bulk.
 */
#define SLM_UPDATE_BATCH		2048			/* namespace updates */
#define SLM_RECLAIM_BATCH		2048			/* garbage reclamation */

struct slm_exp_cli {
	struct slashrpc_cservice	*mexpc_csvc;		/* must be first field */
	uint32_t			 mexpc_stkvers;		/* must be second field */
};

void	slm_rpc_initsvc(void);

int	slm_rmc_handle_lookup(struct pscrpc_request *);

int	slm_rmc_handler(struct pscrpc_request *);
int	slm_rmi_handler(struct pscrpc_request *);
int	slm_rmm_handler(struct pscrpc_request *);
int	slm_rmm_forward_namespace(int, struct slash_fidgen *,
	    struct slash_fidgen *, char *, char *, uint32_t,
	    const struct slash_creds *, struct srt_stat *, int32_t);

int	slm_mkdir(int, struct srm_mkdir_req *, struct srm_mkdir_rep *, int,
	    struct fidc_membh **);
int	slm_symlink(struct pscrpc_request *, struct srm_symlink_req *,
	    struct srm_symlink_rep *, int);

/* aliases for connection management */
#define slm_getmcsvc(resm, exp, fl, mw)					\
	sl_csvc_get(&(resm)->resm_csvc, (fl), (exp),			\
	    &(resm)->resm_nids, SRMM_REQ_PORTAL, SRMM_REP_PORTAL,	\
	    SRMM_MAGIC, SRMM_VERSION, SLCONNT_MDS, (mw))

#define slm_geticsvc(resm, exp, fl, mw)					\
	sl_csvc_get(&(resm)->resm_csvc, (fl), (exp),			\
	    &(resm)->resm_nids, SRIM_REQ_PORTAL, SRIM_REP_PORTAL,	\
	    SRIM_MAGIC,	SRIM_VERSION, SLCONNT_IOD, (mw))

#define slm_getclcsvc(x)	_slm_getclcsvc(PFL_CALLERINFO(), (x))

#define slm_getmcsvcx(m, x)	slm_getmcsvc((m), (x), 0, NULL)
#define slm_getmcsvcf(m, fl)	slm_getmcsvc((m), NULL, (fl), NULL)
#define slm_getmcsvc_wait(m)	slm_getmcsvc((m), NULL, 0, NULL)

#define slm_geticsvcx(m, x)	slm_geticsvc((m), (x), 0, NULL)
#define slm_geticsvcf(m, fl)	slm_geticsvc((m), NULL, (fl), NULL)
#define slm_geticsvc_nb(m, mw)	slm_geticsvc((m), NULL, CSVCF_NONBLOCK, (mw))

#define _pfl_callerinfo pci
static __inline struct slashrpc_cservice *
_slm_getclcsvc(const struct pfl_callerinfo *pci,
    struct pscrpc_export *exp)
{
	struct slm_exp_cli *mexpc;

	mexpc = sl_exp_getpri_cli(exp);
	return (sl_csvc_get(&mexpc->mexpc_csvc, 0, exp, NULL,
	    SRCM_REQ_PORTAL, SRCM_REP_PORTAL, SRCM_MAGIC, SRCM_VERSION,
	    SLCONNT_CLI, NULL));
}
#undef _pfl_callerinfo

#endif /* _RPC_MDS_H_ */
