/* $Id$ */

/*
 * Control interface for querying and modifying
 * parameters of a running daemon instance.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "psc_ds/hash.h"
#include "psc_ds/list.h"
#include "psc_ds/listcache.h"
#include "psc_ds/pool.h"
#include "psc_ds/stree.h"
#include "psc_util/atomic.h"
#include "psc_util/cdefs.h"
#include "psc_util/ctl.h"
#include "psc_util/ctlsvr.h"
#include "psc_util/fmtstr.h"
#include "psc_util/iostats.h"
#include "psc_util/log.h"
#include "psc_util/mlist.h"
#include "psc_util/strlcpy.h"
#include "psc_util/thread.h"

#define Q 15	/* listen() queue */

/*
 * psc_ctlmsg_sendv - send a control message back to client.
 * @fd: client socket descriptor.
 * @mh: already filled-out control message header.
 * @m: control message contents.
 */
int
psc_ctlmsg_sendv(int fd, const struct psc_ctlmsghdr *mh, const void *m)
{
	struct iovec iov[2];
	size_t tsiz;
	ssize_t n;

	iov[0].iov_base = (void *)mh;
	iov[0].iov_len = sizeof(*mh);

	iov[1].iov_base = (void *)m;
	iov[1].iov_len = mh->mh_size;

	n = writev(fd, iov, NENTRIES(iov));
	if (n == -1) {
		if (errno == EPIPE) {
			psc_ctlthr(pscthr_get())->pc_st_ndrop++;
			sched_yield();
			return (0);
		}
		psc_fatal("write");
	}
	tsiz = sizeof(*mh) + mh->mh_size;
	if ((size_t)n != tsiz)
		psc_warn("short write");
	psc_ctlthr(pscthr_get())->pc_st_nsent++;
	sched_yield();
	return (1);
}

/*
 * psc_ctlmsg_send - send a control message back to client.
 * @fd: client socket descriptor.
 * @type: type of message.
 * @siz: size of message.
 * @m: control message contents.
 * Notes: a control message header will be constructed and
 *	written to the client preceding the message contents.
 */
int
psc_ctlmsg_send(int fd, int type, size_t siz, const void *m)
{
	struct psc_ctlmsghdr mh;
	struct iovec iov[2];
	size_t tsiz;
	ssize_t n;

	memset(&mh, 0, sizeof(mh));
	mh.mh_type = type;
	mh.mh_size = siz;

	iov[0].iov_base = &mh;
	iov[0].iov_len = sizeof(mh);

	iov[1].iov_base = (void *)m;
	iov[1].iov_len = siz;

	n = writev(fd, iov, NENTRIES(iov));
	if (n == -1) {
		if (errno == EPIPE) {
			psc_ctlthr(pscthr_get())->pc_st_ndrop++;
			sched_yield();
			return (0);
		}
		psc_fatal("write");
	}
	tsiz = sizeof(mh) + siz;
	if ((size_t)n != tsiz)
		psc_warn("short write");
	psc_ctlthr(pscthr_get())->pc_st_nsent++;
	sched_yield();
	return (1);
}

/*
 * psc_ctlsenderr - send an error to client over control interface.
 * @fd: client socket descriptor.
 * @fmt: printf(3) format of error message.
 */
int
psc_ctlsenderr(int fd, struct psc_ctlmsghdr *mh, const char *fmt, ...)
{
	struct psc_ctlmsg_error pce;
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(pce.pce_errmsg, sizeof(pce.pce_errmsg), fmt, ap); /* XXX */
	va_end(ap);

	mh->mh_type = PCMT_ERROR;
	mh->mh_size = sizeof(pce);
	return (psc_ctlmsg_sendv(fd, mh, &pce));
}

/*
 * psc_ctlthr_stat - export control thread stats.
 * @thr: thread begin queried.
 * @pcst: thread stats control message to be filled in.
 */
void
psc_ctlthr_stat(struct psc_thread *thr, struct psc_ctlmsg_stats *pcst)
{
	pcst->pcst_nclients = psc_ctlthr(thr)->pc_st_nclients;
	pcst->pcst_nsent    = psc_ctlthr(thr)->pc_st_nsent;
	pcst->pcst_nrecv    = psc_ctlthr(thr)->pc_st_nrecv;
}

/*
 * psc_ctlmsg_stats_send - send a response to a "getstats" inquiry.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @m: control message to be filled in and sent out.
 * @thr: thread begin queried.
 */
__static int
psc_ctlmsg_stats_send(int fd, struct psc_ctlmsghdr *mh, void *m,
    struct psc_thread *thr)
{
	struct psc_ctlmsg_stats *pcst = m;

	if (thr->pscthr_type >= psc_ctl_ngetstats ||
	    psc_ctl_getstats[thr->pscthr_type] == NULL)
		return (1);
	snprintf(pcst->pcst_thrname, sizeof(pcst->pcst_thrname),
	    "%s", thr->pscthr_name);
	pcst->pcst_thrtype = thr->pscthr_type;
	psc_ctl_getstats[thr->pscthr_type](thr, pcst);
	return (psc_ctlmsg_sendv(fd, mh, pcst));
}

/*
 * psc_ctlrep_getstats - send a response to a "getstats" inquiry.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @m: control message to examine and reuse.
 */
int
psc_ctlrep_getstats(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct psc_ctlmsg_stats *pcst = m;

	return (psc_ctl_applythrop(fd, mh, m, pcst->pcst_thrname,
	    psc_ctlmsg_stats_send));
}

/*
 * psc_ctlrep_getsubsys - send a response to a "getsubsys" inquiry.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 */
int
psc_ctlrep_getsubsys(int fd, struct psc_ctlmsghdr *mh, __unusedx void *m)
{
	struct psc_ctlmsg_subsys *pcss;
	size_t siz;
	int n, rc;

	rc = 1;
	siz = PCSS_NAME_MAX * psc_nsubsys;
	pcss = PSCALLOC(siz);
	for (n = 0; n < psc_nsubsys; n++)
		if (snprintf(&pcss->pcss_names[n * PCSS_NAME_MAX],
		    PCSS_NAME_MAX, "%s", psc_subsys_name(n)) == -1) {
			psc_warn("snprintf");
			psc_ctlsenderr(fd, mh,
			    "unable to retrieve subsystems");
			goto done;
		}
	mh->mh_size = siz;
	rc = psc_ctlmsg_sendv(fd, mh, pcss);
 done:
	mh->mh_size = 0;	/* reset because we used our own buffer */
	PSCFREE(pcss);
	return (rc);
}

/*
 * psc_ctlmsg_getloglevel_send - send a response to a "getloglevel" inquiry.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @thr: thread begin queried.
 */
__static int
psc_ctlmsg_loglevel_send(int fd, struct psc_ctlmsghdr *mh, void *m,
    struct psc_thread *thr)
{
	struct psc_ctlmsg_loglevel *pcl = m;
	size_t siz;
	int rc;

	siz = sizeof(*pcl) + sizeof(*pcl->pcl_levels) * psc_nsubsys;
	pcl = PSCALLOC(siz);
	snprintf(pcl->pcl_thrname, sizeof(pcl->pcl_thrname),
	    "%s", thr->pscthr_name);
	memcpy(pcl->pcl_levels, thr->pscthr_loglevels, psc_nsubsys *
	    sizeof(*pcl->pcl_levels));
	mh->mh_size = siz;
	rc = psc_ctlmsg_sendv(fd, mh, pcl);
	mh->mh_size = 0;	/* reset because we used our own buffer */
	PSCFREE(pcl);
	return (rc);
}

/*
 * psc_ctlmsg_getloglevel_send - send a response to a "getloglevel" inquiry.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @m: control message to examine.
 */
int
psc_ctlrep_getloglevel(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct psc_ctlmsg_loglevel *pcl = m;

	return (psc_ctl_applythrop(fd, mh, m, pcl->pcl_thrname,
	    psc_ctlmsg_loglevel_send));
}

/*
 * psc_ctlrep_gethashtable - respond to a "gethashtable" inquiry.
 *	This computes bucket usage statistics of a hash table and
 *	sends the results back to the client.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @m: control message to be filled in and sent out.
 */
int
psc_ctlrep_gethashtable(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct psc_ctlmsg_hashtable *pcht = m;
	struct hash_table *ht;
	char name[HTNAME_MAX];
	int rc, found, all;

	rc = 1;
	found = 0;
	snprintf(name, sizeof(name), "%s", pcht->pcht_name); /* XXX */
	all = (strcmp(name, PCHT_NAME_ALL) == 0);
	spinlock(&hashTablesListLock);
	psclist_for_each_entry(ht, &hashTablesList, htable_entry) {
		if (all ||
		    strncmp(name, ht->htable_name, strlen(name)) == 0) {
			found = 1;

			snprintf(pcht->pcht_name, sizeof(pcht->pcht_name),
			    "%s", ht->htable_name);
			hash_table_stats(ht, &pcht->pcht_totalbucks,
			    &pcht->pcht_usedbucks, &pcht->pcht_nents,
			    &pcht->pcht_maxbucklen);
			rc = psc_ctlmsg_sendv(fd, mh, pcht);
			if (!rc)
				break;

			/* Terminate on exact match. */
			if (strlen(ht->htable_name) == strlen(name))
				break;
		}
	}
	freelock(&hashTablesListLock);
	if (rc && !found && !all)
		rc = psc_ctlsenderr(fd, mh, "unknown hash table: %s", name);
	return (rc);
}

/*
 * psc_ctlrep_getlc - send a response to a "getlc" inquiry.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @pclc: control message to examine and reuse.
 */
int
psc_ctlrep_getlc(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct psc_ctlmsg_lc *pclc = m;
	struct psc_listcache *lc;
	char name[LC_NAME_MAX];
	int rc, found, all;

	rc = 1;
	found = 0;
	strlcpy(name, pclc->pclc_name, sizeof(name));
	all = (strcmp(name, PCLC_NAME_ALL) == 0);
	PLL_LOCK(&pscListCaches);
	psclist_for_each_entry(lc,
	    &pscListCaches.pll_listhd, lc_index_lentry) {
		if (all || strncmp(lc->lc_name,
		    name, strlen(name)) == 0) {
			found = 1;

			LIST_CACHE_LOCK(lc);
			strlcpy(pclc->pclc_name, lc->lc_name,
			    sizeof(pclc->pclc_name));
			pclc->pclc_size = lc->lc_size;
			pclc->pclc_nseen = lc->lc_nseen;
			pclc->pclc_flags = lc->lc_flags;
			pclc->pclc_nw_want = atomic_read(
			    &lc->lc_wq_want.wq_nwaitors);
			pclc->pclc_nw_empty = atomic_read(
			    &lc->lc_wq_empty.wq_nwaitors);
			LIST_CACHE_ULOCK(lc);
			rc = psc_ctlmsg_sendv(fd, mh, pclc);
			if (!rc)
				break;

			/* Terminate on exact match. */
			if (strlen(lc->lc_name) == strlen(name))
				break;
		}
	}
	PLL_ULOCK(&pscListCaches);
	if (rc && !found && !all)
		rc = psc_ctlsenderr(fd, mh, "unknown listcache: %s", name);
	return (rc);
}

/*
 * psc_ctlrep_getpool - send a response to a "getpool" inquiry.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @pcpl: control message to examine and reuse.
 */
int
psc_ctlrep_getpool(int fd, struct psc_ctlmsghdr *mh, void *msg)
{
	struct psc_ctlmsg_pool *pcpl = msg;
	struct psc_poolmgr *m;
	char name[LC_NAME_MAX];
	int rc, locked, found, all;

	rc = 1;
	found = 0;
	strlcpy(name, pcpl->pcpl_name, sizeof(name));
	all = (strcmp(name, PCPL_NAME_ALL) == 0);
	PLL_LOCK(&psc_pools);
	psclist_for_each_entry(m, &psc_pools.pll_listhd, ppm_all_lentry) {
		if (all || strncmp(m->ppm_lc.lc_name, name,
		    strlen(name)) == 0) {
			found = 1;

			locked = POOL_RLOCK(m);
			strlcpy(pcpl->pcpl_name, m->ppm_lc.lc_name,
			    sizeof(pcpl->pcpl_name));
			pcpl->pcpl_min = m->ppm_min;
			pcpl->pcpl_max = m->ppm_max;
			pcpl->pcpl_total = m->ppm_total;
			pcpl->pcpl_flags = m->ppm_flags;
			pcpl->pcpl_free = lc_sz(&m->ppm_lc);
			POOL_ULOCK(m, locked);
			rc = psc_ctlmsg_sendv(fd, mh, pcpl);
			if (!rc)
				break;

			/* Terminate on exact match. */
			if (strlen(m->ppm_lc.lc_name) == strlen(name))
				break;
		}
	}
	PLL_ULOCK(&psc_pools);
	if (rc && !found && !all)
		rc = psc_ctlsenderr(fd, mh, "unknown pool: %s", name);
	return (rc);
}

#define MAX_LEVELS 8

int
psc_ctlmsg_param_send(int fd, const struct psc_ctlmsghdr *mh,
    struct psc_ctlmsg_param *pcp, const char *thrname,
    char **levels, int nlevels, const char *value)
{
	char *s, othrname[PSC_THRNAME_MAX];
	const char *p, *end;
	int rc, lvl;

	/*
	 * Save original request threadname and copy actual in
	 * for this message.  These will differ in cases such as
	 * "all" or "ziothr" against "ziothr9".
	 */
	snprintf(othrname, sizeof(othrname), "%s", pcp->pcp_thrname);
	snprintf(pcp->pcp_thrname, sizeof(pcp->pcp_thrname), "%s", thrname);

	/* Concatenate each levels[] element together with dots (`.'). */
	s = pcp->pcp_field;
	end = s + sizeof(pcp->pcp_field) - 1;
	for (lvl = 0; s < end && lvl < nlevels; lvl++) {
		for (p = levels[lvl]; s < end && *p; s++, p++)
			*s = *p;
		if (s < end && lvl < nlevels - 1)
			*s++ = '.';
	}
	*s = '\0';

	snprintf(pcp->pcp_value, sizeof(pcp->pcp_value), "%s", value);
	rc = psc_ctlmsg_sendv(fd, mh, pcp);

	/* Restore original threadname value for additional processing. */
	snprintf(pcp->pcp_thrname, sizeof(pcp->pcp_thrname), "%s", othrname);
	return (rc);
}

int
psc_ctlparam_log_level(int fd, struct psc_ctlmsghdr *mh,
    struct psc_ctlmsg_param *pcp, char **levels, int nlevels)
{
	int rc, set, loglevel, subsys, start_ss, end_ss;
	struct psc_thread *thr;

	if (nlevels > 3)
		return (psc_ctlsenderr(fd, mh, "invalid field"));

	levels[0] = "log";
	levels[1] = "level";

	loglevel = 0; /* gcc */

	set = (mh->mh_type == PCMT_SETPARAM);

	if (set) {
		loglevel = psc_loglevel_getid(pcp->pcp_value);
		if (loglevel == -1)
			return (psc_ctlsenderr(fd, mh,
			    "invalid log.level value: %s",
			    pcp->pcp_value));
		if (pcp->pcp_flags & (PCPF_ADD | PCPF_SUB))
			return (psc_ctlsenderr(fd, mh,
			    "invalid operation"));
	}

	if (nlevels == 3) {
		/* Subsys specified, use it. */
		subsys = psc_subsys_id(levels[2]);
		if (subsys == -1)
			return (psc_ctlsenderr(fd, mh,
			    "invalid log.level subsystem: %s",
			    levels[2]));
		start_ss = subsys;
		end_ss = subsys + 1;
	} else {
		/* No subsys specified, use all. */
		start_ss = 0;
		end_ss = psc_nsubsys;
	}

	rc = 1;
	PLL_LOCK(&psc_threads);
	PSC_CTL_FOREACH_THREAD(thr, pcp->pcp_thrname, &psc_threads.pll_listhd)
		for (subsys = start_ss; subsys < end_ss; subsys++) {
			if (set)
				thr->pscthr_loglevels[subsys] = loglevel;
			else {
				levels[2] = (char *)psc_subsys_name(subsys);
				rc = psc_ctlmsg_param_send(fd, mh, pcp,
				    thr->pscthr_name, levels, 3,
				    psc_loglevel_getname(thr->
				    pscthr_loglevels[subsys]));
				if (!rc)
					goto done;
			}
		}
 done:
	PLL_ULOCK(&psc_threads);
	return (rc);
}

int
psc_ctlparam_pool_handle(int fd, struct psc_ctlmsghdr *mh,
    struct psc_ctlmsg_param *pcp, char **levels, int nlevels,
    struct psc_poolmgr *m, int val)
{
	char nbuf[20];
	int set;

	if (nlevels > 4 || (nlevels == 3 &&
	    (strcmp(levels[2], "min") != 0 &&
	     strcmp(levels[2], "max") != 0 &&
	     strcmp(levels[2], "total") != 0)))
		return (psc_ctlsenderr(fd, mh, "invalid field"));

	levels[0] = "pool";
	levels[1] = m->ppm_lc.lc_name;

	set = (mh->mh_type == PCMT_SETPARAM);

	if (nlevels < 3 || strcmp(levels[2], "min") == 0) {
		if (nlevels == 3 && set) {
			if (pcp->pcp_flags & PCPF_ADD)
				m->ppm_min += val;
			else if (pcp->pcp_flags & PCPF_SUB)
				m->ppm_min -= val;
			else
				m->ppm_min = val;
			psc_pool_resize(m);
		} else {
			levels[2] = "min";
			snprintf(nbuf, sizeof(nbuf), "%d", m->ppm_min);
			if (!psc_ctlmsg_param_send(fd, mh, pcp,
			    PCTHRNAME_EVERYONE, levels, 3, nbuf))
				return (0);
		}
	}
	if (nlevels < 3 || strcmp(levels[2], "max") == 0) {
		if (nlevels == 3 && set) {
			if (pcp->pcp_flags & PCPF_ADD)
				m->ppm_max += val;
			else if (pcp->pcp_flags & PCPF_SUB)
				m->ppm_max -= val;
			else
				m->ppm_max = val;
			psc_pool_resize(m);
		} else {
			levels[2] = "max";
			snprintf(nbuf, sizeof(nbuf), "%d", m->ppm_max);
			if (!psc_ctlmsg_param_send(fd, mh, pcp,
			    PCTHRNAME_EVERYONE, levels, 3, nbuf))
				return (0);
		}
	}
	if (nlevels < 3 || strcmp(levels[2], "total") == 0) {
		if (nlevels == 3 && set) {
			if (pcp->pcp_flags & PCPF_ADD)
				psc_pool_grow(m, val);
			else if (pcp->pcp_flags & PCPF_SUB)
				psc_pool_shrink(m, val);
			else
				psc_pool_settotal(m, val);
		} else {
			levels[2] = "total";
			snprintf(nbuf, sizeof(nbuf), "%d", m->ppm_total);
			if (!psc_ctlmsg_param_send(fd, mh, pcp,
			    PCTHRNAME_EVERYONE, levels, 3, nbuf))
				return (0);
		}
	}
	return (1);
}

int
psc_ctlparam_pool(int fd, struct psc_ctlmsghdr *mh,
    struct psc_ctlmsg_param *pcp, char **levels, int nlevels)
{
	struct psc_poolmgr *m;
	int rc, set;
	char *endp;
	long val;

	if (nlevels > 3)
		return (psc_ctlsenderr(fd, mh, "invalid field"));

	if (strcmp(pcp->pcp_thrname, PCTHRNAME_EVERYONE) != 0)
		return (psc_ctlsenderr(fd, mh, "invalid thread field"));

	levels[0] = "pool";

	val = 0; /* gcc */

	set = (mh->mh_type == PCMT_SETPARAM);

	if (set) {
		if (nlevels != 3)
			return (psc_ctlsenderr(fd, mh,
			    "invalid operation"));

		if (strcmp(levels[2], "min") &&
		    strcmp(levels[2], "max") &&
		    strcmp(levels[2], "total"))
			return (psc_ctlsenderr(fd, mh,
			    "invalid pool field: %s", levels[2]));

		endp = NULL;
		val = strtol(pcp->pcp_value, &endp, 10);
		if (val == LONG_MIN || val == LONG_MAX ||
		    val > INT_MAX || val < 0 ||
		    endp == pcp->pcp_value || *endp != '\0')
			return (psc_ctlsenderr(fd, mh,
			    "invalid pool %s value: %s",
			    levels[2], pcp->pcp_value));
	}

	if (nlevels == 1) {
		PLL_LOCK(&psc_pools);
		psclist_for_each_entry(m,
		    &psc_pools.pll_listhd, ppm_all_lentry)
			if (!(rc = psc_ctlparam_pool_handle(fd,
			    mh, pcp, levels, nlevels, m, val)))
				break;
		PLL_ULOCK(&psc_pools);
	} else {
		m = psc_pool_lookup(levels[1]);
		if (m == NULL)
			return (psc_ctlsenderr(fd, mh,
			    "invalid pool: %s", levels[1]));
		rc = psc_ctlparam_pool_handle(fd, mh,
		    pcp, levels, nlevels, m, val);
	}
	return (rc);
}

/* Node in the control parameter tree. */
struct psc_ctlparam_node {
	char	 *pcn_name;
	int	(*pcn_cbf)(int, struct psc_ctlmsghdr *,
			struct psc_ctlmsg_param *, char **, int);
};

/* Stack processing frame. */
struct psc_ctlparam_procframe {
	struct psclist_head	 pcf_lentry;
	struct psc_streenode	*pcf_ptn;
	int			 pcf_level;
	int			 pcf_flags;
	int			 pcf_pos;
};

#define PCFF_USEPOS	(1<<0)

struct psc_streenode psc_ctlparamtree = PSC_STREE_INIT(psc_ctlparamtree);

int
psc_ctlrep_param(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct psc_ctlparam_procframe *pcf, *npcf;
	struct psc_streenode *ptn, *c, *d;
	struct psc_ctlmsg_param *pcp = m;
	struct psc_ctlparam_node *pcn;
	struct psclist_head stack;
	char *t, *levels[MAX_LEVELS];
	int n, k, nlevels, set;

	pcf = NULL;
	INIT_PSCLIST_HEAD(&stack);

	set = (mh->mh_type == PCMT_SETPARAM);

	for (nlevels = 0, t = pcp->pcp_field;
	    nlevels < MAX_LEVELS &&
	    (levels[nlevels] = t) != NULL; ) {
		if ((t = strchr(levels[nlevels], '.')) != NULL)
			*t++ = '\0';
		if (*levels[nlevels++] == '\0')
			goto invalid;
	}

	if (nlevels == 0 || nlevels >= MAX_LEVELS)
		goto invalid;

	pcf = PSCALLOC(sizeof(*pcf));
	pcf->pcf_ptn = &psc_ctlparamtree;
	psclist_xadd(&pcf->pcf_lentry, &stack);

	while (!psclist_empty(&stack)) {
		pcf = psclist_first_entry(&stack,
		    struct psc_ctlparam_procframe, pcf_lentry);
		psclist_del(&pcf->pcf_lentry);

		n = pcf->pcf_level;
		ptn = pcf->pcf_ptn;
		do {
			k = 0;
			psc_stree_foreach_child(c, ptn) {
				pcn = c->ptn_data;
				if (pcf->pcf_flags & PCFF_USEPOS) {
					if (pcf->pcf_pos == k)
						break;
				} else if (strcmp(pcn->pcn_name,
				    levels[n]) == 0)
					break;
				k++;
			}
			if (c == NULL)
				goto invalid;
			if (psclist_empty(&c->ptn_children)) {
				if (!pcn->pcn_cbf(fd, mh,
				    pcp, levels, nlevels))
					goto shortcircuit;
				break;
			} else if (pcf->pcf_level + 1 >= nlevels) {
				if (set)
					goto invalid;
				k = 0;
				psc_stree_foreach_child(d, c) {
					pcn = d->ptn_data;
					if (psclist_empty(&d->ptn_children)) {
						if (!pcn->pcn_cbf(fd, mh,
						    pcp, levels, nlevels))
							goto shortcircuit;
					} else {
						npcf = PSCALLOC(sizeof(*npcf));
						npcf->pcf_ptn = d;
						npcf->pcf_level = n + 1;
						npcf->pcf_pos = k++;
						npcf->pcf_flags = PCFF_USEPOS;
						psclist_xadd(&npcf->pcf_lentry, &stack);
					}
				}
			}
			ptn = c;
		} while (++n < nlevels);
		PSCFREE(pcf);
	}
	return (1);

 shortcircuit:
	PSCFREE(pcf);
	psclist_for_each_entry_safe(pcf, npcf, &stack, pcf_lentry)
		PSCFREE(pcf);
	return (0);

 invalid:
	PSCFREE(pcf);
	/*
	 * Strictly speaking, this shouldn't be necessary, cause
	 * any frames we added were done out of the integrity of
	 * the paramtree.
	 */
	psclist_for_each_entry_safe(pcf, npcf, &stack, pcf_lentry)
		PSCFREE(pcf);
	while (nlevels-- > 1)
		pcp->pcp_field[strlen(pcp->pcp_field)] = '.';
	return (psc_ctlsenderr(fd, mh, "invalid field%s: %s",
	    set ? "/value" : "", pcp->pcp_field));
}

void
psc_ctlparam_register(const char *oname, int (*cbf)(int, struct psc_ctlmsghdr *,
    struct psc_ctlmsg_param *, char **, int))
{
	struct psc_streenode *ptn, *c;
	struct psc_ctlparam_node *pcn;
	char *subname, *next, *name;

	name = strdup(oname);
	if (name == NULL)
		psc_fatal("strdup");

	ptn = &psc_ctlparamtree;
	for (subname = name; subname != NULL; subname = next) {
		if ((next = strchr(subname, '.')) != NULL)
			*next++ = '\0';
		psc_stree_foreach_child(c, ptn) {
			pcn = c->ptn_data;
			if (strcmp(pcn->pcn_name, subname) == 0)
				break;
		}
		if (c == NULL) {
			pcn = PSCALLOC(sizeof(*pcn));
			pcn->pcn_name = strdup(subname);
			if (pcn->pcn_name == NULL)
				psc_fatal("strdup");
			if (next == NULL)
				pcn->pcn_cbf = cbf;
			c = psc_stree_addchild(ptn, pcn);
		}
		ptn = c;
	}
	PSCFREE(name);
}

/*
 * psc_ctlrep_getiostats - send a response to a "getiostats" inquiry.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @m: iostats control message to be filled in and sent out.
 */
int
psc_ctlrep_getiostats(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct psc_ctlmsg_iostats *pci = m;
	char name[IST_NAME_MAX];
	struct iostats *ist;
	int rc, found, all;

	rc = 1;
	found = 0;
	snprintf(name, sizeof(name), "%s", pci->pci_ist.ist_name);
	all = (strcmp(name, PCI_NAME_ALL) == 0);
	PLL_LOCK(&psc_iostats);
	psclist_for_each_entry(ist,
	    &psc_iostats.pll_listhd, ist_lentry)
		if (all ||
		    strncmp(ist->ist_name, name, strlen(name)) == 0) {
			found = 1;

			pci->pci_ist = *ist;
			rc = psc_ctlmsg_sendv(fd, mh, pci);
			if (!rc)
				break;

			/* Terminate on exact match. */
			if (strlen(ist->ist_name) == strlen(name))
				break;
		}
	PLL_ULOCK(&psc_iostats);
	if (rc && !found && !all)
		rc = psc_ctlsenderr(fd, mh, "unknown iostats: %s", name);
	return (rc);
}

/*
 * zctlrep_getmeter - respond to a "getmeter" inquiry.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @m: control message to examine and reuse.
 */
int
psc_ctlrep_getmeter(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct psc_ctlmsg_meter *pcm = m;
	char name[PSC_METER_NAME_MAX];
	struct psc_meter *pm;
	int rc, found, all;

	rc = 1;
	found = 0;
	snprintf(name, sizeof(name), "%s", pcm->pcm_mtr.pm_name);
	all = (strcmp(name, PCM_NAME_ALL) == 0);
	spinlock(&pscMetersLock);
	psclist_for_each_entry(pm, &pscMetersList, pm_lentry)
		if (all || strncmp(pm->pm_name, name,
		    strlen(name)) == 0) {
			found = 1;

			pcm->pcm_mtr = *pm; /* XXX atomic */
			rc = psc_ctlmsg_sendv(fd, mh, pcm);
			if (!rc)
				break;

			/* Terminate on exact match. */
			if (strlen(name) == strlen(pm->pm_name))
				break;
		}
	freelock(&pscMetersLock);
	if (rc && !found && !all)
		rc = psc_ctlsenderr(fd, mh, "unknown meter: %s", name);
	return (rc);
}

__weak size_t
multilock_cond_nwaitors(__unusedx struct multilock_cond *m)
{
	return (0);
}

/*
 * psc_ctlrep_getmlist - respond to a "getmlist" inquiry.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @m: control message to examine and reuse.
 */
int
psc_ctlrep_getmlist(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct psc_ctlmsg_mlist *pcml = m;
	char name[PML_NAME_MAX];
	struct psc_mlist *pml;
	int rc, found, all;

	rc = 1;
	found = 0;
	snprintf(name, sizeof(name), "%s", pcml->pcml_name);
	all = (strcmp(name, PCML_NAME_ALL) == 0);
	PLL_LOCK(&psc_mlists);
	psclist_for_each_entry(pml, &psc_mlists.pll_listhd,
	    pml_index_lentry)
		if (all || strncmp(pml->pml_name, name,
		    strlen(name)) == 0) {
			found = 1;

			MLIST_LOCK(pml);
			snprintf(pcml->pcml_name,
			    sizeof(pcml->pcml_name),
			    "%s", pml->pml_name);
			pcml->pcml_size = pml->pml_size;
			pcml->pcml_nseen = pml->pml_nseen;
			pcml->pcml_waitors =
			    multilock_cond_nwaitors(&pml->pml_mlcond_empty);
			MLIST_ULOCK(pml);

			rc = psc_ctlmsg_sendv(fd, mh, pcml);
			if (!rc)
				break;

			/* Terminate on exact match. */
			if (strlen(name) == strlen(pml->pml_name))
				break;
		}
	PLL_ULOCK(&psc_mlists);
	if (rc && !found && !all)
		rc = psc_ctlsenderr(fd, mh, "unknown mlist: %s", name);
	return (rc);
}

/*
 * psc_ctlhnd_cmd - invoke an action from a "command" inquiry.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @m: control message to examine.
 */
int
psc_ctlhnd_cmd(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct psc_ctlmsg_cmd *pcc = m;
	int rc;

	rc = 1;
	if (pcc->pcc_opcode > 0 && pcc->pcc_opcode < psc_ctl_ncmds)
		rc = (*psc_ctl_cmds[pcc->pcc_opcode])(fd, mh, m);
	else
		rc = psc_ctlsenderr(fd, mh,
		    "unknown command: %d", pcc->pcc_opcode);
	return (rc);
}

/*
 * psc_ctl_applythrop - invoke an operation on all applicable threads.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @m: control message to examine.
 * @thrname: name of thread to match on.
 * @cbf: callback to run for matching threads.
 */
int
psc_ctl_applythrop(int fd, struct psc_ctlmsghdr *mh, void *m, const char *thrname,
    int (*cbf)(int, struct psc_ctlmsghdr *, void *, struct psc_thread *))
{
	struct psc_thread *thr;
	int rc;

	rc = 1;
	PLL_LOCK(&psc_threads);
	if (strcasecmp(thrname, PCTHRNAME_EVERYONE) == 0) {
		psclist_for_each_entry(thr,
		    &psc_threads.pll_listhd, pscthr_lentry) {
			rc = cbf(fd, mh, m, thr);
			if (!rc)
				break;
		}
	} else {
		psclist_for_each_entry(thr,
		    &psc_threads.pll_listhd, pscthr_lentry)
			/*
			 * XXX: strncasecmp?
			 * thr1 can't match thr10,
			 * don't partial match numbers.
			 */
			if (strcasecmp(thrname,
			    thr->pscthr_name) == 0) {
				rc = cbf(fd, mh, m, thr);
				break;
			}
		if (thr == NULL)
			rc = psc_ctlsenderr(fd, mh,
			    "unknown thread: %s", thrname);
	}
	PLL_ULOCK(&psc_threads);
	return (rc);
}

/*
 * psc_ctlthr_service - satisfy a client connection.
 * @fd: client socket descriptor.
 *
 * Notes: sched_yield() is not explicity called throughout this routine,
 * which has implications, advantages, and disadvantages.
 *
 * Implications: we run till we finish the client connection and the next
 * accept() puts us back to sleep, if no intervening system calls which
 * run in the meantime relinquish control to other threads.
 *
 * Advantages: it might be nice to block all threads so processing by
 * other threads doesn't happen while control messages which modify
 * operation are being processed.
 *
 * Disadvantages: if we don't go to sleep during processing of client
 * connection, anyone can denial the service quite easily.
 */
__static void
psc_ctlthr_service(int fd, const struct psc_ctlop *ct, int nops)
{
	struct psc_ctlmsghdr mh;
	size_t siz;
	ssize_t n;
	void *m;

	m = NULL;
	siz = 0;
	while ((n = read(fd, &mh, sizeof(mh))) != -1 && n != 0) {
		if (n != sizeof(mh)) {
			psc_notice("short read on psc_ctlmsghdr; read=%zd", n);
			continue;
		}
		if (mh.mh_size > siz) {
			siz = mh.mh_size;
			if ((m = realloc(m, siz)) == NULL)
				psc_fatal("realloc");
		}
		n = read(fd, m, mh.mh_size);
		if (n == -1)
			psc_fatal("read");
		if ((size_t)n != mh.mh_size) {
			psc_warn("short read on psc_ctlmsg contents; "
			    "read=%zu; expected=%zu",
			    n, mh.mh_size);
			break;
		}
		if (mh.mh_type < 0 ||
		    mh.mh_type >= nops ||
		    ct[mh.mh_type].pc_op == NULL) {
			psc_warnx("unrecognized psc_ctlmsghdr type; "
			    "type=%d size=%zu", mh.mh_type, mh.mh_size);
			psc_ctlsenderr(fd, &mh,
			    "unrecognized psc_ctlmsghdr type; "
			    "type=%d size=%zu", mh.mh_type, mh.mh_size);
			continue;
		}
		if (ct[mh.mh_type].pc_siz &&
		    ct[mh.mh_type].pc_siz != mh.mh_size) {
			psc_ctlsenderr(fd, &mh,
			    "invalid ctlmsg size; type=%d, size=%zu",
			    mh.mh_type, mh.mh_size);
			continue;
		}
		psc_ctlthr(pscthr_get())->pc_st_nrecv++;
		if (!ct[mh.mh_type].pc_op(fd, &mh, m))
			break;
	}
	if (n == -1 && errno != EPIPE)
		psc_fatal("read");
	PSCFREE(m);
}

/*
 * psc_ctlthr_main - main control thread client-servicing loop.
 * @ofn: path to control socket.
 * @ct: control operations.
 * @nops: number of operations in @ct table.
 */
__dead void
psc_ctlthr_main(const char *ofn, const struct psc_ctlop *ct, int nops)
{
	struct sockaddr_un sun;
	struct sigaction sa;
	char fn[PATH_MAX];
	mode_t old_umask;
	socklen_t siz;
	int s, fd;

	/* Ignore SIGPIPEs in this thread. */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	if (sigaction(SIGPIPE, &sa, NULL) == -1)
		psc_fatal("sigaction");

	/* Create control socket. */
	if ((s = socket(AF_LOCAL, SOCK_STREAM, 0)) == -1)
		psc_fatal("socket");

	FMTSTR(fn, sizeof(fn), ofn,
		FMTSTRCASE('h', fn, sizeof(fn), "s",
		    psclog_getdata()->pld_hostname)
	);

	bzero(&sun, sizeof(sun));
	sun.sun_family = AF_LOCAL;
	snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", fn);
	if (unlink(fn) == -1)
		if (errno != ENOENT)
			psc_error("unlink %s", fn);

	/* XXX lock this umask call */
	old_umask = umask(S_IXUSR | S_IXGRP | S_IWOTH | S_IROTH | S_IXOTH);
	if (bind(s, (struct sockaddr *)&sun, sizeof(sun)) == -1)
		psc_fatal("bind %s", fn);
	umask(old_umask);

	/* XXX fchmod */
	if (chmod(fn, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
	    S_IROTH | S_IWOTH) == -1) {
		unlink(fn);
		psc_fatal("chmod %s", fn); /* XXX errno */
	}

	/* Serve client connections. */
	if (listen(s, Q) == -1)
		psc_fatal("listen");

	for (;;) {
		siz = sizeof(sun);
		if ((fd = accept(s, (struct sockaddr *)&sun,
		    &siz)) == -1)
			psc_fatal("accept");
		psc_ctlthr(pscthr_get())->pc_st_nclients++;
		psc_ctlthr_service(fd, ct, nops);
		close(fd);
	}
	/* NOTREACHED */
}
