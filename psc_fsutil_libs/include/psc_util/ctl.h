/* $Id$ */

/*
 * Control interface for querying and modifying
 * parameters of a running daemon instance.
 */

#ifndef _PFL_CTL_H_
#define _PFL_CTL_H_

#include <sys/types.h>

#include "psc_ds/hash.h"
#include "psc_ds/listcache.h"
#include "psc_util/iostats.h"
#include "psc_util/meter.h"
#include "psc_util/mlist.h"
#include "psc_util/thread.h"

#define PCTHRNAME_EVERYONE	"everyone"

#define PCE_ERRMSG_MAX		50

struct psc_ctlmsg_error {
	char			pce_errmsg[PCE_ERRMSG_MAX];
};

#define PCSS_NAME_MAX 16	/* multiple of wordsize */

struct psc_ctlmsg_subsys {
	char			pcss_names[0];
};

struct psc_ctlmsg_loglevel {
	char			pcl_thrname[PSC_THRNAME_MAX];
	int32_t			pcl_levels[0];
};

struct psc_ctlmsg_lc {
	char			pclc_name[LC_NAME_MAX];
	uint64_t		pclc_size;	/* #items on list */
	uint64_t		pclc_nseen;	/* max #items list can attain */
	int32_t			pclc_flags;
	int32_t			pclc_nw_want;	/* #waitors waking for a want */
	int32_t			pclc_nw_empty;	/* #waitors waking on empty */
};

#define PCLC_NAME_ALL		"all"

struct psc_ctlmsg_stats {
	char			pcst_thrname[PSC_THRNAME_MAX];
	int32_t			pcst_thrtype;
	uint32_t		pcst_u32_1;
	uint32_t		pcst_u32_2;
	uint32_t		pcst_u32_3;
	uint32_t		pcst_u32_4;
};

#define pcst_nclients	pcst_u32_1
#define pcst_nsent	pcst_u32_2
#define pcst_nrecv	pcst_u32_3
#define pcst_ndrop	pcst_u32_4

struct psc_ctlmsg_hashtable {
	char			pcht_name[HTNAME_MAX];
	int32_t			pcht_totalbucks;
	int32_t			pcht_usedbucks;
	int32_t			pcht_nents;
	int32_t			pcht_maxbucklen;
};

#define PCHT_NAME_ALL		"all"

#define PCP_FIELD_MAX		48
#define PCP_VALUE_MAX		48

struct psc_ctlmsg_param {
	char			pcp_thrname[PSC_THRNAME_MAX];
	char			pcp_field[PCP_FIELD_MAX];
	char			pcp_value[PCP_VALUE_MAX];
	int32_t			pcp_flags;
};

#define PCPF_ADD	(1 << 0)
#define PCPF_SUB	(1 << 1)

struct psc_ctlmsg_iostats {
	struct iostats		pci_ist;
};

#define PCI_NAME_ALL		"all"

struct psc_ctlmsg_meter {
	struct psc_meter	pcm_mtr;
};

#define PCM_NAME_ALL		"all"

struct psc_ctlmsg_pool {
	char			pcpl_name[LC_NAME_MAX];
	int32_t			pcpl_min;
	int32_t			pcpl_max;
	int32_t			pcpl_total;
	int32_t			pcpl_free;
	int32_t			pcpl_flags;
};

#define PCPL_NAME_ALL		"all"

struct psc_ctlmsg_mlist {
	char			 pcml_name[PML_NAME_MAX];
	uint64_t		 pcml_nseen;
	uint32_t		 pcml_size;
	uint32_t		 pcml_waitors;
};

#define PCML_NAME_ALL		"all"

struct psc_ctlmsg_cmd {
	int32_t			 pcc_opcode;
};

/* Control message types. */
#define PCMT_ERROR		0
#define PCMT_GETLOGLEVEL	1
#define PCMT_GETLC		2
#define PCMT_GETSTATS		3
#define PCMT_GETSUBSYS		4
#define PCMT_GETHASHTABLE	5
#define PCMT_GETPARAM		6
#define PCMT_SETPARAM		7
#define PCMT_GETIOSTATS		8
#define PCMT_GETMETER		9
#define PCMT_GETPOOL		10
#define ZCMT_GETMLIST		11
#define ZCMT_CMD		12
#define NPCMT			13

/*
 * Control message header.
 * This structure precedes each actual message.
 */
struct psc_ctlmsghdr {
	int			mh_type;
	int			mh_id;
	size_t			mh_size;
	unsigned char		mh_data[0];
};

#endif /* _PFL_CTL_H_ */
