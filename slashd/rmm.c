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
 * Routines for handling RPC requests for MDS from MDS.
 */

#define PSC_SUBSYS PSS_RPC

#include <stdio.h>

#include "pfl/str.h"
#include "psc_rpc/rpc.h"
#include "psc_rpc/rpclog.h"
#include "psc_rpc/rsx.h"
#include "psc_rpc/service.h"
#include "psc_util/lock.h"

#include "fid.h"
#include "fidc_mds.h"
#include "rpc_mds.h"
#include "slashd.h"
#include "slashrpc.h"
#include "slconn.h"
#include "slerr.h"
#include "sljournal.h"

int
slm_rmm_apply_update(struct slmds_jent_namespace *jnamespace)
{
	int rc;
	struct sl_mds_peerinfo *localinfo;

	localinfo = res2rpmi(nodeResProf)->rpmi_info;
	rc = mds_redo_namespace(jnamespace);
	if (rc)
		psc_atomic32_inc(&localinfo->sp_stats.ns_stats[NS_DIR_RECV][
		    jnamespace->sjnm_op][NS_SUM_FAIL]);
	else
		psc_atomic32_inc(&localinfo->sp_stats.ns_stats[NS_DIR_RECV][
		    jnamespace->sjnm_op][NS_SUM_SUCC]);
	return (rc);
}

/**
 * slm_rmm_handle_connect - Handle a CONNECT request from another MDS.
 */
int
slm_rmm_handle_connect(struct pscrpc_request *rq)
{
	struct srm_connect_req *mq;
	struct srm_generic_rep *mp;

	SL_RSX_ALLOCREP(rq, mq, mp);
	if (mq->magic != SRMM_MAGIC || mq->version != SRMM_VERSION)
		mp->rc = EINVAL;
	return (0);
}

/**
 * slm_rmm_handle_send_namespace - Handle a SEND_NAMESPACE request from
 *	another MDS.
 */
int
slm_rmm_handle_namespace_update(struct pscrpc_request *rq)
{
	struct slmds_jent_namespace *jnamespace;
	struct srm_send_namespace_req *mq;
	struct pscrpc_bulk_desc *desc;
	struct srm_generic_rep *mp;
	struct sl_mds_peerinfo *p;
	struct sl_resource *res;
	struct sl_site *site;
	struct iovec iov;
	uint64_t crc, seqno;
	int i, count;

	SL_RSX_ALLOCREP(rq, mq, mp);

	count = mq->count;
	seqno = mq->seqno;
	if (count <= 0 || mq->size > LNET_MTU)
		return (EINVAL);

	iov.iov_len = mq->size;
	iov.iov_base = PSCALLOC(mq->size);

	mp->rc = rsx_bulkserver(rq, &desc, BULK_GET_SINK,
	    SRMM_BULK_PORTAL, &iov, 1);
	if (mp->rc)
		goto out;

	if (desc)
		pscrpc_free_bulk(desc);

	psc_crc64_calc(&crc, iov.iov_base, iov.iov_len);
	if (crc != mq->crc) {
		mp->rc = EINVAL;
		goto out;
	}

	/* Search for the peer information by the given site ID. */
	site = libsl_siteid2site(mq->siteid);
	p = NULL;
	if (site)
		SITE_FOREACH_RES(site, res, i)
			if (res->res_type == SLREST_MDS) {
				p = res2rpmi(res)->rpmi_info;
				break;
			}
	if (p == NULL) {
		psc_info("fail to find site ID %d", mq->siteid);
		mp->rc = EINVAL;
		goto out;
	}

	/*
	 * Make sure that the seqno number matches what we expect
	 * (strictly in-order delivery).  If not, reject right away.
	 */
	if (p->sp_recv_seqno > seqno) {
		/*
		 * This is okay; our peer may have just lost patience
		 * with us and decide to resend.
		 */
		psc_notify("seq number %"PRIx64" is less than %"PRIx64,
		    seqno, p->sp_recv_seqno);
		mp->rc = EINVAL;
		goto out;
	}
	if (p->sp_recv_seqno < seqno) {
		psc_notify("seq number %"PRIx64" is greater than %"PRIx64,
		    seqno, p->sp_recv_seqno);
		mp->rc = EINVAL;
		goto out;
	}

	/* iterate through the namespace update buffer and apply updates */
	jnamespace = iov.iov_base;
	for (i = 0; i < count; i++) {
		mp->rc = slm_rmm_apply_update(jnamespace);
		if (mp->rc)
			break;
		jnamespace = PSC_AGP(jnamespace, jnamespace->sjnm_reclen);
	}
	/* Should I ask for a resend if I have trouble applying updates? */
	p->sp_recv_seqno = seqno + count;

 out:
	PSCFREE(iov.iov_base);
	return (mp->rc);
}

/**
 * slm_rmm_handler - Handle a request for MDS from another MDS.
 */
int
slm_rmm_handler(struct pscrpc_request *rq)
{
	int rc;

	rq->rq_status = SL_EXP_REGISTER_RESM(rq->rq_export,
	    slm_getmcsvcx(_resm, rq->rq_export));
	if (rq->rq_status)
		return (pscrpc_error(rq));

	switch (rq->rq_reqmsg->opc) {
	case SRMT_CONNECT:
		rc = slm_rmm_handle_connect(rq);
		break;
	case SRMT_NAMESPACE_UPDATE:
		rc = slm_rmm_handle_namespace_update(rq);
		break;
	default:
		psc_errorx("unexpected opcode %d", rq->rq_reqmsg->opc);
		rq->rq_status = -ENOSYS;
		return (pscrpc_error(rq));
	}
	authbuf_sign(rq, PSCRPC_MSG_REPLY);
	pscrpc_target_send_reply_msg(rq, rc, 0);
	return (rc);
}
