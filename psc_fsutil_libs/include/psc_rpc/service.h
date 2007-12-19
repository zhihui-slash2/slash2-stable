/* $Id$ */

#ifndef _PFL_SERVICE_H_
#define _PFL_SERVICE_H_

#include "psc_rpc/rpc.h"
#include "psc_util/thread.h"
#include "psc_ds/list.h"

typedef int (*pscrpc_service_func_t)(struct pscrpc_request *);

struct pscrpc_svc_handle { 
	struct psclist_head    svh_chain;
	struct psc_thread     *svh_threads;
	struct pscrpc_service *svh_service;
	pscrpc_service_func_t  svh_handler;
	int svh_nthreads;
	int svh_nbufs;
	int svh_bufsz;
	int svh_reqsz;
	int svh_repsz;
	int svh_req_portal;
	int svh_rep_portal;
	int svh_type;
	char svh_svc_name[32];
};

typedef struct pscrpc_svc_handle pscrpc_svc_handle_t;


#endif /* _PFL_SERVICE_H_ */
