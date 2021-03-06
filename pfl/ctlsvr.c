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

/*
 * Control interface for querying and modifying parameters of a running
 * daemon instance.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/resource.h>

#include <errno.h>
#include <fnmatch.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "pfl/atomic.h"
#include "pfl/cdefs.h"
#include "pfl/ctl.h"
#include "pfl/ctlsvr.h"
#include "pfl/explist.h"
#include "pfl/fault.h"
#include "pfl/fmtstr.h"
#include "pfl/hashtbl.h"
#include "pfl/opstats.h"
#include "pfl/journal.h"
#include "pfl/list.h"
#include "pfl/listcache.h"
#include "pfl/lock.h"
#include "pfl/log.h"
#include "pfl/mlist.h"
#include "pfl/net.h"
#include "pfl/pfl.h"
#include "pfl/pool.h"
#include "pfl/random.h"
#include "pfl/rlimit.h"
#include "pfl/rpc_intrfc.h"
#include "pfl/str.h"
#include "pfl/stree.h"
#include "pfl/thread.h"
#include "pfl/umask.h"
#include "pfl/waitq.h"

#define QLEN 15	/* listen(2) queue */

struct psc_dynarray	psc_ctl_clifds = DYNARRAY_INIT;
psc_spinlock_t		psc_ctl_clifds_lock = SPINLOCK_INIT;
struct psc_waitq	psc_ctl_clifds_waitq = PSC_WAITQ_INIT;

struct pfl_mutex	pfl_ctl_mutex = PSC_MUTEX_INIT;

__weak size_t
psc_multiwaitcond_nwaiters(__unusedx struct psc_multiwaitcond *m)
{
	psc_fatalx("multiwait support not compiled in");
}

__weak int
psc_ctlrep_getfault(int fd, struct psc_ctlmsghdr *mh,
    __unusedx void *c)
{
	return (psc_ctlsenderr(fd, mh, "fault support not compiled in"));
}

/*
 * Send a control message back to client.
 * @fd: client socket descriptor.
 * @mh: already filled-out control message header.
 * @m: control message contents.
 */
int
psc_ctlmsg_sendv(int fd, const struct psc_ctlmsghdr *mh, const void *m)
{
	struct iovec iov[2];
	struct msghdr msg;
	size_t tsiz;
	ssize_t n;

	iov[0].iov_base = (void *)mh;
	iov[0].iov_len = sizeof(*mh);

	iov[1].iov_base = (void *)m;
	iov[1].iov_len = mh->mh_size;

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = iov;
	msg.msg_iovlen = nitems(iov);

	psc_mutex_lock(&pfl_ctl_mutex);
	n = sendmsg(fd, &msg, PFL_MSG_NOSIGNAL);
	psc_mutex_unlock(&pfl_ctl_mutex);

	if (n == -1) {
		if (errno == EPIPE || errno == ECONNRESET) {
			psc_ctlthr(pscthr_get())->pct_stat.ndrop++;
			pscthr_yield();
			return (0);
		}
		psc_fatal("sendmsg");
	}
	tsiz = sizeof(*mh) + mh->mh_size;
	if ((size_t)n != tsiz)
		psclog_warn("short sendmsg");
	psc_ctlthr(pscthr_get())->pct_stat.nsent++;
	pscthr_yield();
	return (1);
}

/*
 * Send a control message back to client.
 * @fd: client socket descriptor.
 * @id: client-provided passback identifier.
 * @type: type of message.
 * @siz: size of message.
 * @m: control message contents.
 * Notes: a control message header will be constructed and
 *	written to the client preceding the message contents.
 */
int
psc_ctlmsg_send(int fd, int id, int type, size_t siz, const void *m)
{
	struct psc_ctlmsghdr mh;

	memset(&mh, 0, sizeof(mh));
	mh.mh_id = id;
	mh.mh_type = type;
	mh.mh_size = siz;
	return (psc_ctlmsg_sendv(fd, &mh, m));
}

int
psc_ctlsenderrv(int fd, const struct psc_ctlmsghdr *mhp,
    const char *fmt, va_list ap)
{
	struct psc_ctlmsg_error pce;
	struct psc_ctlmsghdr mh;

	/* XXX */
	vsnprintf(pce.pce_errmsg, sizeof(pce.pce_errmsg), fmt, ap);

	mh.mh_id = mhp->mh_id;
	mh.mh_type = PCMT_ERROR;
	mh.mh_size = sizeof(pce);
	return (psc_ctlmsg_sendv(fd, &mh, &pce));
}

/*
 * Send an error to client over control interface.
 * @fd: client socket descriptor.
 * @mh: message header to use.
 * @fmt: printf(3) format of error message.
 */
int
psc_ctlsenderr(int fd, const struct psc_ctlmsghdr *mhp, const char *fmt,
    ...)
{
	va_list ap;
	int rc;

	va_start(ap, fmt);
	rc = psc_ctlsenderrv(fd, mhp, fmt, ap);
	va_end(ap);
	return (rc);
}

/*
 * Export control thread stats.
 * @thr: thread begin queried.
 * @pcst: thread stats control message to be filled in.
 */
void
psc_ctlthr_get(struct psc_thread *thr, struct psc_ctlmsg_thread *pcst)
{
	pcst->pcst_nsent	= psc_ctlthr(thr)->pct_stat.nsent;
	pcst->pcst_nrecv	= psc_ctlthr(thr)->pct_stat.nrecv;
	pcst->pcst_ndrop	= psc_ctlthr(thr)->pct_stat.ndrop;
}

/*
 * Export control thread stats.
 * @thr: thread begin queried.
 * @pcst: thread stats control message to be filled in.
 */
void
psc_ctlacthr_get(struct psc_thread *thr, struct psc_ctlmsg_thread *pcst)
{
	pcst->pcst_nclients	= psc_ctlacthr(thr)->pcat_stat.nclients;
}

/*
 * Send a reply to a "GETTHREAD" inquiry.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @m: control message to be filled in and sent out.
 * @thr: thread begin queried.
 */
__static int
psc_ctlmsg_thread_send(int fd, struct psc_ctlmsghdr *mh, void *m,
    struct psc_thread *thr)
{
	struct psc_ctlmsg_thread *pcst = m;

	snprintf(pcst->pcst_thrname, sizeof(pcst->pcst_thrname),
	    "%s", thr->pscthr_name);
	pcst->pcst_thrid = thr->pscthr_thrid;
	pcst->pcst_thrtype = thr->pscthr_type;
	pcst->pcst_flags = thr->pscthr_flags;
	if (thr->pscthr_type >= 0 &&
	    thr->pscthr_type < psc_ctl_nthrgets &&
	    psc_ctl_thrgets[thr->pscthr_type])
		psc_ctl_thrgets[thr->pscthr_type](thr, pcst);
	return (psc_ctlmsg_sendv(fd, mh, pcst));
}

/*
 * Respond to a "GETTHREAD" inquiry.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @m: control message to examine and reuse.
 */
int
psc_ctlrep_getthread(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct psc_ctlmsg_thread *pcst = m;

	return (psc_ctl_applythrop(fd, mh, m, pcst->pcst_thrname,
	    psc_ctlmsg_thread_send));
}

/*
 * Send a response to a "GETSUBSYS" inquiry.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 */
int
psc_ctlrep_getsubsys(int fd, struct psc_ctlmsghdr *mh,
    __unusedx void *m)
{
	struct psc_ctlmsg_subsys *pcss;
	size_t siz;
	int n, rc;

	rc = 1;
	siz = PCSS_NAME_MAX * psc_dynarray_len(&psc_subsystems);
	pcss = PSCALLOC(siz);
	for (n = 0; n < psc_dynarray_len(&psc_subsystems); n++)
		if (snprintf(&pcss->pcss_names[n * PCSS_NAME_MAX],
		    PCSS_NAME_MAX, "%s", psc_subsys_name(n)) == -1) {
			psclog_warn("snprintf");
			rc = psc_ctlsenderr(fd, mh,
			    "unable to retrieve subsystems");
			goto done;
		}
	mh->mh_size = siz;
	rc = psc_ctlmsg_sendv(fd, mh, pcss);
 done:
	/* reset because we used our own buffer */
	mh->mh_size = 0;
	PSCFREE(pcss);
	return (rc);
}

__weak int
psc_ctlrep_getlnetif(int fd, struct psc_ctlmsghdr *mh,
    __unusedx void *m)
{
	return (psc_ctlsenderr(fd, mh, "get lnet interface: %s",
	    strerror(ENOTSUP)));
}

#ifndef PFL_RPC
int
psc_ctlrep_getrpcrq(int fd, struct psc_ctlmsghdr *mh,
    __unusedx void *m)
{
	return (psc_ctlsenderr(fd, mh, "get rpcrq: %s",
	    strerror(ENOTSUP)));
}

int
psc_ctlrep_getrpcsvc(int fd, struct psc_ctlmsghdr *mh,
    __unusedx void *m)
{
	return (psc_ctlsenderr(fd, mh, "get rpcsvc: %s",
	    strerror(ENOTSUP)));
}
#endif

/*
 * Send a reply to a "GETLOGLEVEL" inquiry.
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

	siz = sizeof(*pcl) + sizeof(*pcl->pcl_levels) *
	    psc_dynarray_len(&psc_subsystems);
	pcl = PSCALLOC(siz);
	snprintf(pcl->pcl_thrname, sizeof(pcl->pcl_thrname),
	    "%s", thr->pscthr_name);
	memcpy(pcl->pcl_levels, thr->pscthr_loglevels,
	    psc_dynarray_len(&psc_subsystems) *
	    sizeof(*pcl->pcl_levels));
	mh->mh_size = siz;
	rc = psc_ctlmsg_sendv(fd, mh, pcl);
	/* reset because we used our own buffer */
	mh->mh_size = 0;
	PSCFREE(pcl);
	return (rc);
}

/*
 * Respond to a "GETLOGLEVEL" inquiry.
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
 * Respond to a "GETHASHTABLE" inquiry.  This computes bucket usage
 * statistics of a hash table and sends the results back to the client.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @m: control message to be filled in and sent out.
 */
int
psc_ctlrep_gethashtable(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct psc_ctlmsg_hashtable *pcht = m;
	struct psc_hashtbl *pht;
	char name[PSC_HTNAME_MAX];
	int rc, found, all;

	rc = 1;
	found = 0;
	strlcpy(name, pcht->pcht_name, sizeof(name));
	all = (name[0] == '\0');

	PLL_LOCK(&psc_hashtbls);
	PLL_FOREACH(pht, &psc_hashtbls) {
		if (all ||
		    strncmp(name, pht->pht_name, strlen(name)) == 0) {
			found = 1;

			snprintf(pcht->pcht_name,
			    sizeof(pcht->pcht_name), "%s",
			    pht->pht_name);
			psc_hashtbl_getstats(pht,
			    &pcht->pcht_totalbucks,
			    &pcht->pcht_usedbucks, &pcht->pcht_nents,
			    &pcht->pcht_maxbucklen);
			rc = psc_ctlmsg_sendv(fd, mh, pcht);
			if (!rc)
				break;

			/* Terminate on exact match. */
			if (strcmp(pht->pht_name, name) == 0)
				break;
		}
	}
	PLL_ULOCK(&psc_hashtbls);

	if (rc && !found && !all)
		rc = psc_ctlsenderr(fd, mh, "unknown hash table: %s",
		    name);
	return (rc);
}

/*
 * Respond to a "GETLISTCACHE" inquiry.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @m: control message to examine and reuse.
 */
int
psc_ctlrep_getlistcache(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct psc_ctlmsg_listcache *pclc = m;
	struct psc_listcache *lc;
	char name[PEXL_NAME_MAX];
	int rc, found, all;

	rc = 1;
	found = 0;
	strlcpy(name, pclc->pclc_name, sizeof(name));
	all = (strcmp(name, PCLC_NAME_ALL) == 0 || name[0] == '\0');
	PLL_LOCK(&psc_listcaches);
	PLL_FOREACH(lc, &psc_listcaches) {
		if (all || strncmp(lc->plc_name,
		    name, strlen(name)) == 0) {
			found = 1;

			LIST_CACHE_LOCK(lc);
			strlcpy(pclc->pclc_name, lc->plc_name,
			    sizeof(pclc->pclc_name));
			pclc->pclc_size = lc->plc_nitems;
			pclc->pclc_nseen = psc_atomic64_read(
			    &lc->plc_nseen->opst_lifetime);
			pclc->pclc_flags = lc->plc_flags;
			pclc->pclc_nw_want = psc_waitq_nwaiters(
			    &lc->plc_wq_want);
			pclc->pclc_nw_empty = psc_waitq_nwaiters(
			    &lc->plc_wq_empty);
			LIST_CACHE_ULOCK(lc);
			rc = psc_ctlmsg_sendv(fd, mh, pclc);
			if (!rc)
				break;

			/* Terminate on exact match. */
			if (strcmp(lc->plc_name, name) == 0)
				break;
		}
	}
	PLL_ULOCK(&psc_listcaches);
	if (rc && !found && !all)
		rc = psc_ctlsenderr(fd, mh, "unknown listcache: %s",
		    name);
	return (rc);
}

/*
 * Send a response to a "GETPOOL" inquiry.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @msg: control message to examine and reuse.
 */
int
psc_ctlrep_getpool(int fd, struct psc_ctlmsghdr *mh, void *msg)
{
	struct psc_ctlmsg_pool *pcpl = msg;
	struct psc_poolmgr *m;
	char name[PEXL_NAME_MAX];
	int rc, found, all;

	rc = 1;
	found = 0;
	strlcpy(name, pcpl->pcpl_name, sizeof(name));
	all = (strcmp(name, PCPL_NAME_ALL) == 0 || name[0] == '\0');
	PLL_LOCK(&psc_pools);
	PLL_FOREACH(m, &psc_pools) {
		if (all || strncmp(m->ppm_name, name,
		    strlen(name)) == 0) {
			found = 1;

			POOL_LOCK(m);
			strlcpy(pcpl->pcpl_name, m->ppm_name,
			    sizeof(pcpl->pcpl_name));
			pcpl->pcpl_min = m->ppm_min;
			pcpl->pcpl_max = m->ppm_max;
			pcpl->pcpl_total = m->ppm_total;
			pcpl->pcpl_flags = m->ppm_flags;
			pcpl->pcpl_thres = m->ppm_thres;
			pcpl->pcpl_nseen = psc_atomic64_read(
			    &m->ppm_nseen->opst_lifetime);
			pcpl->pcpl_ngrow = psc_atomic64_read(
			    &m->ppm_opst_grows->opst_lifetime);
			pcpl->pcpl_nshrink = psc_atomic64_read(
			    &m->ppm_opst_shrinks->opst_lifetime);
			if (POOL_IS_MLIST(m)) {
				pcpl->pcpl_free = psc_mlist_size(
				    &m->ppm_ml);
				pcpl->pcpl_nw_empty =
				    psc_multiwaitcond_nwaiters(
					&m->ppm_ml.pml_mwcond_empty);
			} else {
				pcpl->pcpl_free = lc_nitems(&m->ppm_lc);
				pcpl->pcpl_nw_want = psc_waitq_nwaiters(
				    &m->ppm_lc.plc_wq_want);
				pcpl->pcpl_nw_empty =
				    psc_waitq_nwaiters(
					&m->ppm_lc.plc_wq_empty);
			}
			POOL_ULOCK(m);
			rc = psc_ctlmsg_sendv(fd, mh, pcpl);
			if (!rc)
				break;

			/* Terminate on exact match. */
			if (strcmp(m->ppm_name, name) == 0)
				break;
		}
	}
	PLL_ULOCK(&psc_pools);
	if (rc && !found && !all)
		rc = psc_ctlsenderr(fd, mh, "unknown pool: %s", name);
	return (rc);
}

/* Maximum depth of parameter node, e.g. [thr.]foo.bar.glarch=3 */
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
	 * "all" or "mythr" against "mythr9".
	 */
	snprintf(othrname, sizeof(othrname), "%s", pcp->pcp_thrname);
	snprintf(pcp->pcp_thrname, sizeof(pcp->pcp_thrname), "%s",
	    thrname);

	/*
	 * Concatenate each levels[] element together with dots (`.').
	 */
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

	/*
	 * Restore original threadname value for additional processing.
	 */
	snprintf(pcp->pcp_thrname, sizeof(pcp->pcp_thrname), "%s",
	    othrname);
	return (rc);
}

int
psc_ctlparam_log_level(int fd, struct psc_ctlmsghdr *mh,
    struct psc_ctlmsg_param *pcp, char **levels, int nlevels,
    __unusedx struct psc_ctlparam_node *pcn)
{
	int found, rc, set, loglevel, subsys, start_ss, end_ss;
	struct psc_thread *thr;

	if (nlevels > 3)
		return (psc_ctlsenderr(fd, mh, "invalid field"));

	levels[0] = "log";
	levels[1] = "level";

	loglevel = 0; /* gcc */

	set = (mh->mh_type == PCMT_SETPARAM);

	if (set) {
		loglevel = psc_loglevel_fromstr(pcp->pcp_value);
		if (loglevel == PNLOGLEVELS)
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
		end_ss = psc_dynarray_len(&psc_subsystems);
		subsys = PSS_ALL;
	}

	if (set && strcmp(pcp->pcp_thrname, PCTHRNAME_EVERYONE) == 0)
		psc_log_setlevel(subsys, loglevel);
		/* XXX optimize: bail */

	rc = 1;
	found = 0;
	PLL_LOCK(&psc_threads);
	PSC_CTL_FOREACH_THREAD(thr, pcp->pcp_thrname,
	    &psc_threads.pll_listhd) {
		found = 1;

		for (subsys = start_ss; subsys < end_ss; subsys++) {
			if (set)
				thr->pscthr_loglevels[subsys] =
				    loglevel;
			else {
				levels[2] = (char *)psc_subsys_name(
				    subsys);
				rc = psc_ctlmsg_param_send(fd, mh, pcp,
				    thr->pscthr_name, levels, 3,
				    psc_loglevel_getname(thr->
				    pscthr_loglevels[subsys]));
				if (!rc)
					goto done;
			}
		}
	}

 done:
	PLL_ULOCK(&psc_threads);
	if (!found)
		return (psc_ctlsenderr(fd, mh, "invalid thread: %s",
		    pcp->pcp_thrname));
	return (rc);
}

int
psc_ctlparam_log_file(int fd, struct psc_ctlmsghdr *mh,
    struct psc_ctlmsg_param *pcp, char **levels, int nlevels,
    __unusedx struct psc_ctlparam_node *pcn)
{
	int rc = 1, set;

	if (nlevels > 2)
		return (psc_ctlsenderr(fd, mh, "invalid field"));

	if (strcmp(pcp->pcp_thrname, PCTHRNAME_EVERYONE) != 0) {
		if (nlevels == 2)
			rc = psc_ctlsenderr(fd, mh, "log.file: "
			    "not thread specific");
		return (rc);
	}

	levels[0] = "log";
	levels[1] = "file";

	set = (mh->mh_type == PCMT_SETPARAM);

	if (set) {
		if (pcp->pcp_flags & PCPF_SUB)
			return (psc_ctlsenderr(fd,
			    mh, "invalid operation"));
		if (freopen(pcp->pcp_value,
		    pcp->pcp_flags & PCPF_ADD ?
		    "a" : "w", stderr) == NULL)
			rc = psc_ctlsenderr(fd, mh, "log.file: %s",
			    strerror(errno));
	} else
		rc = psc_ctlsenderr(fd, mh,
		    "log.file: write-only field");
	return (rc);
}

int
psc_ctlparam_log_format(int fd, struct psc_ctlmsghdr *mh,
    struct psc_ctlmsg_param *pcp, char **levels, int nlevels,
    __unusedx struct psc_ctlparam_node *pcn)
{
	int rc = 1, set;

	if (nlevels > 2)
		return (psc_ctlsenderr(fd, mh, "invalid field"));

	if (strcmp(pcp->pcp_thrname, PCTHRNAME_EVERYONE) != 0) {
		if (nlevels == 2)
			rc = psc_ctlsenderr(fd, mh, "log.format: "
			    "not thread specific");
		return (rc);
	}

	levels[0] = "log";
	levels[1] = "format";

	set = (mh->mh_type == PCMT_SETPARAM);

	if (set) {
		/* XXX race */
		static char logbuf[LINE_MAX];

		if (nlevels != 2)
			return (psc_ctlsenderr(fd, mh, "log.format: "
			    "invalid level of specification"));

		if (pcp->pcp_flags & (PCPF_ADD | PCPF_SUB))
			return (psc_ctlsenderr(fd, mh,
			    "invalid operation"));

		strlcpy(logbuf, pcp->pcp_value, sizeof(logbuf));
		psc_logfmt = logbuf;
	} else
		rc = psc_ctlmsg_param_send(fd, mh, pcp,
		    PCTHRNAME_EVERYONE, levels, 2, psc_logfmt);
	return (rc);
}

int
psc_ctlparam_log_points(int fd, struct psc_ctlmsghdr *mh,
    struct psc_ctlmsg_param *pcp, char **levels, int nlevels,
    __unusedx struct psc_ctlparam_node *pcn)
{
	struct pfl_logpoint *pt;
	int line, n, rc = 0;
	char *sln, *endp;
	long l;

	if (nlevels > 2)
		return (psc_ctlsenderr(fd, mh, "invalid field"));

	if (strcmp(pcp->pcp_thrname, PCTHRNAME_EVERYONE) != 0) {
		if (nlevels == 2)
			rc = psc_ctlsenderr(fd, mh, "log.points: "
			    "not thread specific");
		return (rc);
	}

	levels[0] = "log";
	levels[1] = "points";

	if (mh->mh_type == PCMT_SETPARAM) {
		sln = strchr(pcp->pcp_value, ':');
		if (sln == NULL)
			return (psc_ctlsenderr(fd, mh,
			    "invalid point format"));

		*sln++ = '\0';
		l = strtol(sln, &endp, 10);
		if (l < 1 || l > INT_MAX ||
		    *endp || endp == sln)
			return (psc_ctlsenderr(fd, mh,
			    "invalid point format"));
		line = l;

		if (pcp->pcp_flags & PCPF_SUB) {
			pt = _pfl_get_logpointid(pcp->pcp_value, line,
			    0);
			if (pt)
				psc_dynarray_setpos(&_pfl_logpoints,
				    pt->plogpt_idx, NULL);
			else
				return (psc_ctlsenderr(fd, mh,
				    "no such log point"));
		} else if (pcp->pcp_flags & PCPF_ADD) {
			pt = _pfl_get_logpointid(pcp->pcp_value, line,
			    1);
			psc_dynarray_setpos(&_pfl_logpoints,
			    pt->plogpt_idx, pt);
		} else
			return (psc_ctlsenderr(fd, mh,
			    "invalid operation"));
	} else {
		DYNARRAY_FOREACH(pt, n, &_pfl_logpoints) {
			if (pt) {
				rc = psc_ctlmsg_param_send(fd, mh, pcp,
				    PCTHRNAME_EVERYONE, levels, 2,
				    pt->plogpt_key);
				if (!rc)
					break;
			}
		}
	}
	return (rc);
}


struct psc_ctl_rlim {
	char	*pcr_name;
	int	 pcr_id;
} psc_ctl_rlimtab [] = {
	{ "cpu",	RLIMIT_CPU },
	{ "csize",	RLIMIT_CORE },
	{ "dsize",	RLIMIT_DATA },
	{ "fsize",	RLIMIT_FSIZE },
#ifdef RLIMIT_LOCKS
	{ "locks",	RLIMIT_LOCKS },
#endif
#ifdef RLIMIT_NPROC
	{ "maxproc",	RLIMIT_NPROC },
#endif
#ifdef RLIMIT_AS
	{ "mem",	RLIMIT_AS },
#endif
#ifdef RLIMIT_MEMLOCK
	{ "mlock",	RLIMIT_MEMLOCK },
#endif
#ifdef RLIMIT_MSGQUEUE
	{ "msgqueue",	RLIMIT_MSGQUEUE },
#endif
#ifdef RLIMIT_NICE
	{ "nice",	RLIMIT_NICE },
#endif
	{ "nofile",	RLIMIT_NOFILE },
#ifdef RLIMIT_RSS
	{ "rss",	RLIMIT_RSS },
#endif
#ifdef RLIMIT_RTPRIO
	{ "rtprio",	RLIMIT_RTPRIO },
#endif
#ifdef RLIMIT_RTTIME
	{ "rttime",	RLIMIT_RTTIME },
#endif
#ifdef RLIMIT_SIGPENDING
	{ "sigpndg",	RLIMIT_SIGPENDING },
#endif
	{ "stksize",	RLIMIT_STACK }
};

int
psc_ctlparam_rlim(int fd, struct psc_ctlmsghdr *mh,
    struct psc_ctlmsg_param *pcp, char **levels, int nlevels,
    __unusedx struct psc_ctlparam_node *pcn)
{
	struct psc_ctl_rlim *pcr = NULL;
	char buf[32], *endp;
	int rc, set, i;
	long val = 0;
	rlim_t n;

	if (nlevels > 2)
		return (psc_ctlsenderr(fd, mh, "invalid field"));

	if (strcmp(pcp->pcp_thrname, PCTHRNAME_EVERYONE) != 0)
		return (psc_ctlsenderr(fd, mh, "rlim: not thread "
		    "specific"));

	rc = 1;
	levels[0] = "rlim";

	if (nlevels == 2) {
		for (pcr = psc_ctl_rlimtab, i = 0;
		    i < (int)nitems(psc_ctl_rlimtab); i++, pcr++)
			if (strcmp(levels[1], pcr->pcr_name) == 0)
				break;
		if (i == nitems(psc_ctl_rlimtab))
			return (psc_ctlsenderr(fd, mh,
			    "invalid rlim field: %s", levels[1]));
	}

	set = (mh->mh_type == PCMT_SETPARAM);

	if (set) {
		if (nlevels != 2)
			return (psc_ctlsenderr(fd, mh,
			    "invalid operation"));
		endp = NULL;
		val = strtol(pcp->pcp_value, &endp, 10);
		if (val <= 0 || val > 10 * 1024 ||
		    endp == pcp->pcp_value || *endp != '\0')
			return (psc_ctlsenderr(fd, mh,
			    "invalid rlim.%s value: %s",
			    pcr->pcr_name, pcp->pcp_value));

		if (pcp->pcp_flags & (PCPF_ADD | PCPF_SUB)) {
			if (psc_getrlimit(pcr->pcr_id, &n,
			    NULL) == -1) {
				int error = errno;

				psclog_error("getrlimit");
				return (psc_ctlsenderr(fd, mh,
				    "getrlimit: %s", strerror(error)));
			}
			if (pcp->pcp_flags & PCPF_ADD)
				val += n;
			else if (pcp->pcp_flags & PCPF_SUB)
				val = n - val;
		}
	}

	for (pcr = psc_ctl_rlimtab, i = 0;
	    i < (int)nitems(psc_ctl_rlimtab); i++, pcr++) {
		if (nlevels < 2 ||
		    strcmp(levels[1], pcr->pcr_name) == 0) {
			if (set) {
				if (psc_setrlimit(pcr->pcr_id, val,
				    val) == -1)
					return (psc_ctlsenderr(fd, mh,
					    "setrlimit: %s",
					    strerror(errno)));
			} else {
				levels[1] = pcr->pcr_name;
				if (psc_getrlimit(pcr->pcr_id, &n,
				    NULL) == -1) {
					psclog_error("getrlimit");
					return (psc_ctlsenderr(fd, mh,
					    "getrlimit: %s",
					    strerror(errno)));
				}
				if (n == RLIM_INFINITY)
					snprintf(buf, sizeof(buf),
					    "-1");
				else
					snprintf(buf, sizeof(buf),
					    "%"PRId64, n);
				rc = psc_ctlmsg_param_send(fd, mh, pcp,
				    PCTHRNAME_EVERYONE, levels, 2, buf);
			}
			if (nlevels == 2)
				break;
		}
	}
	return (rc);
}

#define RUST_TIMEVAL	0
#define RUST_LONG	1

struct psc_ctl_rusage {
	char	*pcru_name;
	int	 pcru_off;
	int	 pcru_fmt;
} psc_ctl_rusagetab [] = {
	{ "utime",	offsetof(struct rusage, ru_utime),	RUST_TIMEVAL },
	{ "stime",	offsetof(struct rusage, ru_stime),	RUST_TIMEVAL },
	{ "maxrss",	offsetof(struct rusage, ru_maxrss),	RUST_LONG },
	{ "ixrss",	offsetof(struct rusage, ru_ixrss),	RUST_LONG },
	{ "idrss",	offsetof(struct rusage, ru_idrss),	RUST_LONG },
	{ "isrss",	offsetof(struct rusage, ru_isrss),	RUST_LONG },
	{ "minflt",	offsetof(struct rusage, ru_minflt),	RUST_LONG },
	{ "majflt",	offsetof(struct rusage, ru_majflt),	RUST_LONG },
	{ "nswap",	offsetof(struct rusage, ru_nswap),	RUST_LONG },
	{ "inblock",	offsetof(struct rusage, ru_inblock),	RUST_LONG },
	{ "oublock",	offsetof(struct rusage, ru_oublock),	RUST_LONG },
	{ "msgsnd",	offsetof(struct rusage, ru_msgsnd),	RUST_LONG },
	{ "msgrcv",	offsetof(struct rusage, ru_msgrcv),	RUST_LONG },
	{ "nsignals",	offsetof(struct rusage, ru_nsignals),	RUST_LONG },
	{ "nvcsw",	offsetof(struct rusage, ru_nvcsw),	RUST_LONG },
	{ "nivcsw",	offsetof(struct rusage, ru_nivcsw),	RUST_LONG }
};

int
psc_ctlparam_rusage(int fd, struct psc_ctlmsghdr *mh,
    struct psc_ctlmsg_param *pcp, char **levels, int nlevels,
    __unusedx struct psc_ctlparam_node *pcn)
{
	struct psc_ctl_rusage *pcru = NULL;
	struct rusage rusage, *rp = &rusage;
	struct timeval *tvp;
	char buf[32];
	int rc, i;
	long *lp;

	if (nlevels > 2)
		return (psc_ctlsenderr(fd, mh, "invalid field"));

	if (strcmp(pcp->pcp_thrname, PCTHRNAME_EVERYONE) != 0)
		return (psc_ctlsenderr(fd, mh, "rusage: not thread "
		    "specific"));

	rc = 1;
	levels[0] = "rusage";

	if (nlevels == 2) {
		for (pcru = psc_ctl_rusagetab, i = 0;
		    i < (int)nitems(psc_ctl_rusagetab); i++, pcru++)
			if (strcmp(levels[1], pcru->pcru_name) == 0)
				break;
		if (i == nitems(psc_ctl_rusagetab))
			return (psc_ctlsenderr(fd, mh,
			    "invalid rusage field: %s", levels[1]));
	}

	if (mh->mh_type == PCMT_SETPARAM)
		return (psc_ctlsenderr(fd, mh,
		    "invalid operation"));

	if (getrusage(RUSAGE_SELF, &rusage) == -1)
		return (psc_ctlsenderr(fd, mh,
		    "getrusage: %s", strerror(errno)));

	for (pcru = psc_ctl_rusagetab, i = 0;
	    i < (int)nitems(psc_ctl_rusagetab); i++, pcru++) {
		if (nlevels < 2 ||
		    strcmp(levels[1], pcru->pcru_name) == 0) {
			levels[1] = pcru->pcru_name;
			switch (pcru->pcru_fmt) {
			case RUST_TIMEVAL:
				tvp = PSC_AGP(rp, pcru->pcru_off);
				snprintf(buf, sizeof(buf),
				    PSCPRI_TIMEVAL,
				    PSCPRI_TIMEVAL_ARGS(tvp));
				break;
			case RUST_LONG:
				lp = PSC_AGP(rp, pcru->pcru_off);
				snprintf(buf, sizeof(buf), "%ld", *lp);
				break;
			}
			rc = psc_ctlmsg_param_send(fd, mh, pcp,
			    PCTHRNAME_EVERYONE, levels, 2, buf);

			if (nlevels == 2)
				break;
		}
	}
	return (rc);
}

int
psc_ctlparam_run(int fd, struct psc_ctlmsghdr *mh,
    struct psc_ctlmsg_param *pcp, char **levels, int nlevels,
    __unusedx struct psc_ctlparam_node *pcn)
{
	int found, rc, set, run;
	struct psc_thread *thr;

	if (nlevels > 1)
		return (psc_ctlsenderr(fd, mh, "invalid field"));

	levels[0] = "run";
	run = 0; /* gcc */

	set = (mh->mh_type == PCMT_SETPARAM);

	if (set) {
		if (pcp->pcp_flags & (PCPF_ADD | PCPF_SUB))
			return (psc_ctlsenderr(fd, mh,
			    "invalid operation"));

		if (strcmp(pcp->pcp_value, "0") == 0)
			run = 0;
		else if (strcmp(pcp->pcp_value, "1") == 0)
			run = 1;
		else
			return (psc_ctlsenderr(fd, mh,
			    "invalid run value: %s",
			    pcp->pcp_field));
	}

	rc = 1;
	found = 0;
	PLL_LOCK(&psc_threads);
	PSC_CTL_FOREACH_THREAD(thr, pcp->pcp_thrname,
	    &psc_threads.pll_listhd) {
		found = 1;

		if (set)
			pscthr_setrun(thr, run);
		else if (!(rc = psc_ctlmsg_param_send(fd, mh, pcp,
		    thr->pscthr_name, levels, 1,
		    thr->pscthr_flags & PTF_RUN ? "1" : "0")))
			break;
	}
	PLL_ULOCK(&psc_threads);
	if (!found)
		return (psc_ctlsenderr(fd, mh, "invalid thread: %s",
		    pcp->pcp_thrname));
	return (rc);
}

/*
 * Handle thread pause state parameter.
 * @fd: control connection file descriptor.
 * @mh: already filled-in control message header.
 * @pcp: parameter control message.
 * @levels: parameter fields.
 * @nlevels: number of fields.
 */
int
psc_ctlparam_pause(int fd, struct psc_ctlmsghdr *mh,
    struct psc_ctlmsg_param *pcp, char **levels, int nlevels,
    __unusedx struct psc_ctlparam_node *pcn)
{
	int found, rc, set, pauseval;
	struct psc_thread *thr;
	char *s;
	long l;

	if (nlevels > 1)
		return (psc_ctlsenderr(fd, mh, "invalid field"));

	levels[0] = "pause";

	pauseval = 0; /* gcc */

	set = (mh->mh_type == PCMT_SETPARAM);

	if (set) {
		if (pcp->pcp_flags & (PCPF_ADD | PCPF_SUB))
			return (psc_ctlsenderr(fd, mh,
			    "invalid operation"));

		s = NULL;
		l = strtol(pcp->pcp_value, &s, 10);
		if (l == LONG_MAX || l == LONG_MIN || *s != '\0' ||
		    s == pcp->pcp_value || l < 0 || l > 1)
			return (psc_ctlsenderr(fd, mh,
			    "invalid pause value: %s",
			    pcp->pcp_field));
		pauseval = (int)l;
	}

	rc = 1;
	found = 0;
	PLL_LOCK(&psc_threads);
	PSC_CTL_FOREACH_THREAD(thr, pcp->pcp_thrname,
	    &psc_threads.pll_listhd) {
		found = 1;

		if (set)
			pscthr_setpause(thr, pauseval);
		else if (!(rc = psc_ctlmsg_param_send(fd, mh, pcp,
		    thr->pscthr_name, levels, 1,
		    (thr->pscthr_flags & PTF_PAUSED) ? "1" : "0")))
			break;
	}
	PLL_ULOCK(&psc_threads);
	if (!found)
		return (psc_ctlsenderr(fd, mh, "invalid thread: %s",
		    pcp->pcp_thrname));
	return (rc);
}

int
psc_ctlparam_pool_handle(int fd, struct psc_ctlmsghdr *mh,
    struct psc_ctlmsg_param *pcp, char **levels, int nlevels,
    struct psc_poolmgr *m, int val)
{
	char nbuf[20];
	int set;

	levels[0] = "pool";
	levels[1] = m->ppm_name;

	set = (mh->mh_type == PCMT_SETPARAM);

	if (nlevels < 3 || strcmp(levels[2], "min") == 0) {
		if (nlevels == 3 && set) {
			/* XXX logic is bogus */
			/* XXX use api/lock  */
			if (pcp->pcp_flags & PCPF_ADD)
				m->ppm_min += val;
			else if (pcp->pcp_flags & PCPF_SUB)
				m->ppm_min -= val;
			else
				m->ppm_min = val;
			if (m->ppm_min < 1)
				m->ppm_min = 1;
			if (m->ppm_max && m->ppm_min > m->ppm_max)
				m->ppm_min = m->ppm_max;
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
			if (m->ppm_max < 1)
				m->ppm_max = 1;
			if (m->ppm_max && m->ppm_max < m->ppm_min)
				m->ppm_max = m->ppm_min;
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
				psc_pool_tryshrink(m, val);
			else
				psc_pool_settotal(m, val);
		} else {
			levels[2] = "total";
			snprintf(nbuf, sizeof(nbuf), "%d",
			    m->ppm_total);
			if (!psc_ctlmsg_param_send(fd, mh, pcp,
			    PCTHRNAME_EVERYONE, levels, 3, nbuf))
				return (0);
		}
	}
	if (nlevels < 3 || strcmp(levels[2], "free") == 0) {
		if (set)
			return (psc_ctlsenderr(fd, mh,
			    "pool.%s.free: read-only", levels[1]));
		else {
			levels[2] = "free";
			if (POOL_IS_MLIST(m))
				snprintf(nbuf, sizeof(nbuf), "%d",
				    psc_mlist_size(&m->ppm_ml));
			else
				snprintf(nbuf, sizeof(nbuf), "%d",
				    lc_nitems(&m->ppm_lc));
			if (!psc_ctlmsg_param_send(fd, mh, pcp,
			    PCTHRNAME_EVERYONE, levels, 3, nbuf))
				return (0);
		}
	}
	if (nlevels < 3 || strcmp(levels[2], "thres") == 0) {
		if (nlevels == 3 && set) {
			if (pcp->pcp_flags & PCPF_ADD)
				m->ppm_thres += val;
			else if (pcp->pcp_flags & PCPF_SUB)
				m->ppm_thres -= val;
			else
				m->ppm_thres = val;
			if (m->ppm_thres < 1)
				m->ppm_thres = 1;
			else if (m->ppm_thres > 99)
				m->ppm_thres = 99;
		} else {
			levels[2] = "thres";
			snprintf(nbuf, sizeof(nbuf), "%d",
			    m->ppm_thres);
			if (!psc_ctlmsg_param_send(fd, mh, pcp,
			    PCTHRNAME_EVERYONE, levels, 3, nbuf))
				return (0);
		}
	}
	if (nlevels == 3 && strcmp(levels[2], "reap") == 0) {
		if (set) {
			if (m->ppm_reclaimcb == NULL)
				return (psc_ctlsenderr(fd, mh,
				    "pool.%s: not reapable",
				    levels[1]));
			if (pcp->pcp_flags & PCPF_SUB)
				return (psc_ctlsenderr(fd, mh,
				    "pool.%s.reap: value cannot be "
				    "negative", levels[1]));

			/* XXX hack */
			psc_atomic32_add(&m->ppm_nwaiters, val);
			psc_pool_reap(m, 0);
			psc_atomic32_sub(&m->ppm_nwaiters, val);
		} else {
			return (psc_ctlsenderr(fd, mh,
			    "pool.%s.reap: write-only field",
			    levels[1]));
		}
	}

	return (1);
}

#if 0
int
psc_ctlparam_instances(int fd, struct psc_ctlmsghdr *mh,
    struct psc_ctlmsg_param *pcp, char **levels, int nlevels,
    __unusedx struct psc_ctlparam_node *pcn)
{
	struct psc_poolmgr *m;
	int rc, set;
	char *endp;
	long val;

	if (nlevels > 1)
		return (psc_ctlsenderr(fd, mh, "invalid field"));

	if (strcmp(pcp->pcp_thrname, PCTHRNAME_EVERYONE) == 0)
		return (psc_ctlsenderr(fd, mh, "thread not specified"));

	rc = 1;
	levels[0] = "instances";
	val = 0; /* gcc */

	set = (mh->mh_type == PCMT_SETPARAM);
	if (set) {
		endp = NULL;
		val = strtol(pcp->pcp_value, &endp, 10);
		if (val == LONG_MIN || val == LONG_MAX ||
		    val > INT_MAX || val < 0 ||
		    endp == pcp->pcp_value || *endp != '\0')
			return (psc_ctlsenderr(fd, mh,
			    "invalid instances value: %s",
			    pcp->pcp_value));
	}

	if (set) {
		if (pcp->pcp_flags & PCPF_ADD)
			pscthr_init(thrtype, thr_main, NULL,
			    sizeof(struct psc_ctlacthr), "thr%d",
			    p - me->pscthr_name, me->pscthr_name);
		else if (pcp->pcp_flags & PCPF_SUB)
			return (psc_ctlsenderr(fd, mh,
			    "operation not supported"));
		else
			return (psc_ctlsenderr(fd, mh,
			    "operation not supported"));
	} else {
		int ninst = 0;

		PLL_LOCK(&psc_threads);
		PLL_FOREACH(thr, &psc_threads)
			if (strncmp(pcp->pcp_thrname, thr->pscthr_name,
			    strlen(pcp->pcp_thrname)) == 0)
				ninst++;
		PLL_ULOCK(&psc_threads);
		snprintf(nbuf, sizeof(nbuf), "%d", ninst);
		if (!psc_ctlmsg_param_send(fd, mh, pcp,
		    pcp->pcp_thrname, levels, 1, nbuf))
			return (0);
	}

	return (rc);
}
#endif

int
psc_ctlparam_pool(int fd, struct psc_ctlmsghdr *mh,
    struct psc_ctlmsg_param *pcp, char **levels, int nlevels,
    __unusedx struct psc_ctlparam_node *pcn)
{
	struct psc_poolmgr *m;
	int rc, set;
	char *endp;
	long val;

	if (nlevels > 3)
		return (psc_ctlsenderr(fd, mh, "invalid field"));

	if (strcmp(pcp->pcp_thrname, PCTHRNAME_EVERYONE) != 0)
		return (psc_ctlsenderr(fd, mh, "invalid field for %s",
		    pcp->pcp_thrname));

	rc = 1;
	levels[0] = "pool";
	val = 0; /* gcc */

	if (nlevels == 3 &&
	    strcmp(levels[2], "min")   != 0 &&
	    strcmp(levels[2], "max")   != 0 &&
	    strcmp(levels[2], "thres") != 0 &&
	    strcmp(levels[2], "free")  != 0 &&
	    strcmp(levels[2], "reap")  != 0 &&
	    strcmp(levels[2], "total") != 0)
		return (psc_ctlsenderr(fd, mh,
		    "invalid pool field: %s", levels[2]));

	set = (mh->mh_type == PCMT_SETPARAM);

	if (set) {
		if (nlevels != 3)
			return (psc_ctlsenderr(fd, mh,
			    "invalid operation"));

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
		PLL_FOREACH(m, &psc_pools) {
			POOL_LOCK(m);
			rc = psc_ctlparam_pool_handle(fd, mh, pcp,
			    levels, nlevels, m, val);
			POOL_ULOCK(m);
			if (!rc)
				break;
		}
		PLL_ULOCK(&psc_pools);
	} else {
		m = psc_pool_lookup(levels[1]);
		if (m == NULL)
			return (psc_ctlsenderr(fd, mh,
			    "invalid pool: %s", levels[1]));
		rc = psc_ctlparam_pool_handle(fd, mh, pcp, levels,
		    nlevels, m, val);
		POOL_ULOCK(m);
	}
	return (rc);
}

/* Node in the control parameter tree. */
struct psc_ctlparam_node {
	char			 *pcn_name;
	int			(*pcn_cbf)(int, struct psc_ctlmsghdr *,
				    struct psc_ctlmsg_param *, char **,
				    int, struct psc_ctlparam_node *);

	/* only used for SIMPLE ctlparam nodes */
	void			(*pcn_getf)(char [PCP_VALUE_MAX]);
	int			(*pcn_setf)(const char *);

	/* only used for SIMPLE var ctlparam nodes */
	enum pflctl_paramt	  pcn_vtype;
	int			  pcn_flags;
	void			 *pcn_ptr;
};

/* Stack processing frame. */
struct psc_ctlparam_procframe {
	struct psc_listentry	 pcf_lentry;
	struct psc_streenode	*pcf_ptn;	/* parameter tree node */
	int			 pcf_level;
	int			 pcf_flags;
	int			 pcf_pos;
};

/* pcf_flags */
#define PCFF_USEPOS		(1 << 0)

struct psc_streenode psc_ctlparamtree = PSC_STREE_INIT(psc_ctlparamtree);

const char *
psc_ctlparam_fieldname(char *fieldname, int nlevels)
{
	while (nlevels-- > 1)
		fieldname[strlen(fieldname)] = '.';
	return (fieldname);
}

int
psc_ctlrep_param_simple(int fd, struct psc_ctlmsghdr *mh,
    struct psc_ctlmsg_param *pcp, char **levels, int nlevels,
    struct psc_ctlparam_node *pcn)
{
	char val[PCP_VALUE_MAX];

	if (strcmp(pcp->pcp_thrname, PCTHRNAME_EVERYONE) != 0)
		return (psc_ctlsenderr(fd, mh, "simple parameters are "
		    "not thread specific"));

	/*
	 * The last level usually points to NULL. But it can point
	 * to a name that is added automatically. If so, increment
	 * the original nlevels accordingly.
	 */
	if (levels[nlevels])
		nlevels++;

	if (mh->mh_type == PCMT_SETPARAM) {
		if (pcn->pcn_setf) {
			if (pcn->pcn_setf(pcp->pcp_value))
 invalid:
				return (psc_ctlsenderr(fd, mh,
				    "%s: invalid value: %s",
				    psc_ctlparam_fieldname(
				      pcp->pcp_field, nlevels),
				    pcp->pcp_value));
			return (1);
		}
		if (pcn->pcn_flags & PFLCTL_PARAMF_RDWR) {
			char *endp;

			switch (pcn->pcn_vtype) {
			case PFLCTL_PARAMT_ATOMIC32: {
				long l;

				l = strtol(pcp->pcp_value, &endp, 10);
				// min and max check
				if (endp == pcp->pcp_value ||
				    *endp != '\0')
					goto invalid;
				psc_atomic32_set(pcn->pcn_ptr, l);
				break;
			    }
			case PFLCTL_PARAMT_INT: {
				long l;

				l = strtol(pcp->pcp_value, &endp, 10);
				// min and max check
				if (endp == pcp->pcp_value ||
				    *endp != '\0')
					goto invalid;
				*(int *)pcn->pcn_ptr = l;
				break;
			}
			case PFLCTL_PARAMT_STR:
				strlcpy(pcn->pcn_ptr, pcp->pcp_value,
				    PCP_VALUE_MAX);
				break;
			case PFLCTL_PARAMT_UINT64: {
				uint64_t l;

				l = strtoull(pcp->pcp_value, &endp, 10);
				// min and max check
				if (endp == pcp->pcp_value ||
				    *endp != '\0')
					goto invalid;
				*(uint64_t *)pcn->pcn_ptr = l;
				break;
			    }
			default:
				psc_fatalx("internal error");
			}
			return (1);
		}
		return (psc_ctlsenderr(fd, mh, "%s: field is read-only",
		    psc_ctlparam_fieldname(pcp->pcp_field, nlevels)));
	}
	switch (pcn->pcn_vtype) {
	case PFLCTL_PARAMT_NONE:
		pcn->pcn_getf(val);
		break;
	case PFLCTL_PARAMT_ATOMIC32:
		snprintf(val, PCP_VALUE_MAX, "%d",
		    psc_atomic32_read(pcn->pcn_ptr));
		break;
	case PFLCTL_PARAMT_INT:
		snprintf(val, PCP_VALUE_MAX, "%d", *(int *)pcn->pcn_ptr);
		break;
	case PFLCTL_PARAMT_STR:
		snprintf(val, PCP_VALUE_MAX, "%s", (char *)pcn->pcn_ptr);
		break;
	case PFLCTL_PARAMT_UINT64:
		snprintf(val, PCP_VALUE_MAX, "%"PRIu64,
		    *(uint64_t *)pcn->pcn_ptr);
		break;
	}
	return (psc_ctlmsg_param_send(fd, mh, pcp, PCTHRNAME_EVERYONE,
	    levels, nlevels, val));
}

int
psc_ctlrep_param(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct psc_ctlparam_procframe *pcf, *npcf;
	struct psc_streenode *ptn, *c, *d;
	struct psc_ctlmsg_param *pcp = m;
	struct psc_ctlparam_node *pcn;
	struct psclist_head stack;
	char *t, *levels[MAX_LEVELS];
	int n, k, nlevels, set, rc = 1;
	uid_t uid;
	gid_t gid;

	pcf = NULL;
	INIT_PSCLIST_HEAD(&stack);

	pcp->pcp_thrname[sizeof(pcp->pcp_thrname) - 1] = '\0';
	pcp->pcp_field[sizeof(pcp->pcp_field) - 1] = '\0';
	pcp->pcp_value[sizeof(pcp->pcp_value) - 1] = '\0';

	set = (mh->mh_type == PCMT_SETPARAM);

	for (nlevels = 0, t = pcp->pcp_field;
	    nlevels < MAX_LEVELS &&
	    (levels[nlevels] = t) != NULL; ) {
		if ((t = strchr(levels[nlevels], '.')) != NULL)
			*t++ = '\0';
		if (*levels[nlevels++] == '\0')
			return (psc_ctlsenderr(fd, mh,
			    "%s: empty node name",
			    psc_ctlparam_fieldname(pcp->pcp_field,
			    nlevels)));
	}

	if (nlevels == 0)
		return (psc_ctlsenderr(fd, mh,
		    "no parameter field specified"));
	if (nlevels >= MAX_LEVELS)
		return (psc_ctlsenderr(fd, mh,
		    "%s: parameter field exceeds maximum depth",
		    psc_ctlparam_fieldname(pcp->pcp_field,
		    MAX_LEVELS - 1)));

	if (set) {
		rc = pfl_socket_getpeercred(fd, &uid, &gid);
		if (rc == 0 && uid)
			rc = EPERM;
		if (rc)
			return (psc_ctlsenderr(fd, mh, "%s: %s",
			    psc_ctlparam_fieldname(pcp->pcp_field,
			      nlevels), strerror(rc)));
	}

	pcf = PSCALLOC(sizeof(*pcf));
	INIT_PSC_LISTENTRY(&pcf->pcf_lentry);
	pcf->pcf_ptn = &psc_ctlparamtree;
	psclist_add(&pcf->pcf_lentry, &stack);

	/*
	 * Walk down the parameter tree according to the number of levels
	 * specified by the user input. Note that the root of the tree is
	 * at level 0.
	 */
	while (!psc_listhd_empty(&stack)) {
		pcf = psc_listhd_first_obj(&stack,
		    struct psc_ctlparam_procframe, pcf_lentry);
		psclist_del(&pcf->pcf_lentry, &stack);

		n = pcf->pcf_level;
		ptn = pcf->pcf_ptn;
		do {
			if (n == nlevels - 1 &&
			    strcmp(levels[n], "?") == 0) {
				if (n)
					psc_ctlparam_fieldname(
					    pcp->pcp_value,
					    nlevels - 1);
				PSC_STREE_FOREACH_CHILD(c, ptn) {
					pcn = c->ptn_data;
					if (n)
						rc = psc_ctlsenderr(fd,
						    mh, "%s: available "
						    "subnode: %s",
						    pcp->pcp_field,
						    pcn->pcn_name);
					else
						rc = psc_ctlsenderr(fd,
						    mh, "available "
						    "top-level node: %s",
						    pcn->pcn_name);
					if (rc == 0)
						break;
				}
				goto shortcircuit;
			}

			k = 0;
			PSC_STREE_FOREACH_CHILD(c, ptn) {
				pcn = c->ptn_data;
				if (pcf->pcf_flags & PCFF_USEPOS) {
					if (pcf->pcf_pos == k)
						break;
				} else if (n < nlevels &&
				    strcmp(pcn->pcn_name,
				    levels[n]) == 0)
					break;
				k++;
			}
			if (c == NULL)
				goto invalid;

			levels[n] = pcn->pcn_name;

			if (psc_listhd_empty(&c->ptn_children)) {
				rc = pcn->pcn_cbf(fd, mh, pcp, levels,
				    nlevels, pcn);
				if (rc == 0)
					goto shortcircuit;
				break;
			} else if (pcf->pcf_level + 1 >= nlevels) {
				/*
				 * If the paramater tree has more levels
				 * than that is given by the user, then
				 * we end up on a non-leaf node.  We are
				 * going to visit all children of this
				 * node.  Also, we must disallow setting
				 * a value on a non-leaf node.
				 */
				if (set)
					goto invalid;

				k = 0;
				PSC_STREE_FOREACH_CHILD(d, c) {
					pcn = d->ptn_data;
					/*
					 * Avoid stack frame by
					 * processing directly.
					 */
					if (psc_listhd_empty(
					    &d->ptn_children)) {
						/*
						 * Automatically add a
						 * level based on the
						 * name of a child.
						 */
						levels[n + 1] =
						    pcn->pcn_name;
						rc = pcn->pcn_cbf(fd,
						    mh, pcp, levels,
						    n + 1, pcn);
						if (rc == 0)
							goto shortcircuit;
					} else {
						npcf = PSCALLOC(sizeof(*npcf));
						npcf->pcf_ptn = d;
						npcf->pcf_level = n + 1;
						npcf->pcf_pos = k++;
						npcf->pcf_flags = PCFF_USEPOS;
						psclist_add(&npcf->pcf_lentry,
						    &stack);
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
	return (rc);

 invalid:
	PSCFREE(pcf);
	/*
	 * Strictly speaking, this shouldn't be necessary, cause
	 * any frames we added were done out of the integrity of
	 * the paramtree.
	 */
	psclist_for_each_entry_safe(pcf, npcf, &stack, pcf_lentry)
		PSCFREE(pcf);
	psc_ctlparam_fieldname(pcp->pcp_field, nlevels);
	if (set)
		return (psc_ctlsenderr(fd, mh,
		    "%s: not a leaf node", pcp->pcp_field));
	return (psc_ctlsenderr(fd, mh, "%s: invalid field",
	    pcp->pcp_field));
}

int
pfl_ctlparam_node_cmp(const void *a, const void *b)
{
	const struct psc_streenode *x = a, *y = b;
	struct psc_ctlparam_node *c = x->ptn_data, *d = y->ptn_data;

	return (strcmp(c->pcn_name, d->pcn_name));
}

struct psc_ctlparam_node *
psc_ctlparam_register(const char *oname, int (*cbf)(int,
    struct psc_ctlmsghdr *, struct psc_ctlmsg_param *, char **, int,
    struct psc_ctlparam_node *))
{
	struct psc_streenode *ptn, *c;
	struct psc_ctlparam_node *pcn;
	char *subname, *next, *name;

	pcn = NULL; /* gcc */
	name = pfl_strdup(oname);
	ptn = &psc_ctlparamtree;
	for (subname = name; subname; subname = next) {
		next = strchr(subname, '.');
		if (next)
			*next++ = '\0';
		PSC_STREE_FOREACH_CHILD(c, ptn) {
			pcn = c->ptn_data;
			if (strcmp(pcn->pcn_name, subname) == 0)
				break;
		}
		if (c == NULL) {
			pcn = PSCALLOC(sizeof(*pcn));
			pcn->pcn_name = pfl_strdup(subname);
			if (next == NULL)
				pcn->pcn_cbf = cbf;
			c = psc_stree_addchild_sorted(ptn, pcn,
			    pfl_ctlparam_node_cmp,
			    offsetof(struct psc_streenode,
			    ptn_sibling));
		}
		ptn = c;
	}
	PSCFREE(name);
	return (pcn);
}

void
psc_ctlparam_register_simple(const char *name, void (*getf)(char *),
    int (*setf)(const char *))
{
	struct psc_ctlparam_node *pcn;

	pcn = psc_ctlparam_register(name, psc_ctlrep_param_simple);
	pcn->pcn_getf = getf;
	pcn->pcn_setf = setf;
}

void
psc_ctlparam_register_var(const char *name, enum pflctl_paramt type,
    int flags, void *p)
{
	struct psc_ctlparam_node *pcn;

	pcn = psc_ctlparam_register(name, psc_ctlrep_param_simple);
	pcn->pcn_vtype = type;
	pcn->pcn_flags = flags;
	pcn->pcn_ptr = p;
}

/*
 * Handle opstats parameter.
 * @fd: control connection file descriptor.
 * @mh: already filled-in control message header.
 * @pcp: parameter control message.
 * @levels: parameter fields.
 * @nlevels: number of fields.
 */
int
psc_ctlparam_opstats(int fd, struct psc_ctlmsghdr *mh,
    struct psc_ctlmsg_param *pcp, char **levels, int nlevels,
    __unusedx struct psc_ctlparam_node *pcn)
{
	int reset = 0, found = 0, rc = 1, i;
	struct pfl_opstat *opst;
	char buf[32];
	long val;

	if (nlevels > 2)
		return (psc_ctlsenderr(fd, mh, "invalid field"));

	levels[0] = "opstats";

	reset = (mh->mh_type == PCMT_SETPARAM);

	spinlock(&pfl_opstats_lock);
	DYNARRAY_FOREACH(opst, i, &pfl_opstats)
		if (nlevels < 2 ||
		    strcmp(levels[1], opst->opst_name) == 0) {
			found = 1;

			if (reset) {
				errno = 0;
				val = strtol(pcp->pcp_value, NULL, 10);
				if (errno == ERANGE) {
					freelock(&pfl_opstats_lock);
					return (psc_ctlsenderr(fd, mh,
					    "invalid opstat %s value: %s",
					    levels[1], pcp->pcp_value));
				}
				psc_atomic64_set(&opst->opst_lifetime, val);
			} else {
				levels[1] = (char *)opst->opst_name;
				snprintf(buf, sizeof(buf), "%"PRId64,
				    psc_atomic64_read(&opst->opst_lifetime));
				rc = psc_ctlmsg_param_send(fd, mh, pcp,
				    PCTHRNAME_EVERYONE, levels, 2, buf);
			}

			if (nlevels == 2)
				break;
		}
	freelock(&pfl_opstats_lock);
	if (!found && nlevels > 1)
		return (psc_ctlsenderr(fd, mh, "%s: invalid opstat",
		    psc_ctlparam_fieldname(pcp->pcp_field, nlevels)));
	return (rc);
}

/*
 * Respond to a "GETOPSTAT" inquiry.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @m: opstats control message to be filled in and sent out.
 */
int
psc_ctlrep_getopstat(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct psc_ctlmsg_opstat *pcop = m;
	struct pfl_opstat *opst;
	char name[OPST_NAME_MAX];
	int rc, found, all, i;

	rc = 1;
	found = 0;
	strlcpy(name, pcop->pco_name, sizeof(name));
	all = (strcmp(name, "") == 0);
	spinlock(&pfl_opstats_lock);
	DYNARRAY_FOREACH(opst, i, &pfl_opstats)
		if (all || fnmatch(name, opst->opst_name, 0) == 0) {
			found = 1;

			pcop->pco_opst = *opst;
			strlcpy(pcop->pco_name, opst->opst_name,
			    sizeof(pcop->pco_name));
			rc = psc_ctlmsg_sendv(fd, mh, pcop);
			if (!rc)
				break;

			/* Terminate on exact match. */
			if (strcmp(opst->opst_name, name) == 0)
				break;
		}
	freelock(&pfl_opstats_lock);
	if (rc && !found && !all)
		rc = psc_ctlsenderr(fd, mh, "unknown opstats: %s",
		    name);
	return (rc);
}

/*
 * Respond to a "GETMETER" inquiry.
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
	all = (name[0] == '\0');
	PLL_LOCK(&psc_meters);
	PLL_FOREACH(pm, &psc_meters)
		if (all || strncmp(pm->pm_name, name,
		    strlen(name)) == 0) {
			found = 1;

			pcm->pcm_mtr = *pm; /* XXX atomic */
			pcm->pcm_mtr.pm_max = *pm->pm_maxp; /* XXX atomic */
			rc = psc_ctlmsg_sendv(fd, mh, pcm);
			if (!rc)
				break;

			/* Terminate on exact match. */
			if (strcmp(pm->pm_name, name) == 0)
				break;
		}
	PLL_ULOCK(&psc_meters);
	if (rc && !found && !all)
		rc = psc_ctlsenderr(fd, mh, "unknown meter: %s", name);
	return (rc);
}

/*
 * Respond to a "GETMLIST" inquiry.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @m: control message to examine and reuse.
 */
int
psc_ctlrep_getmlist(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct psc_ctlmsg_mlist *pcml = m;
	char name[PEXL_NAME_MAX];
	struct psc_mlist *pml;
	int rc, found, all;

	rc = 1;
	found = 0;
	snprintf(name, sizeof(name), "%s", pcml->pcml_name);
	all = (name[0] == '\0');
	PLL_LOCK(&psc_mlists);
	PLL_FOREACH(pml, &psc_mlists)
		if (all || strncmp(pml->pml_name, name,
		    strlen(name)) == 0) {
			found = 1;

			MLIST_LOCK(pml);
			snprintf(pcml->pcml_name,
			    sizeof(pcml->pcml_name),
			    "%s", pml->pml_name);
			pcml->pcml_size = pml->pml_nitems;
			pcml->pcml_nseen = psc_atomic64_read(
			    &pml->pml_nseen->opst_lifetime);
			pcml->pcml_nwaiters =
			    psc_multiwaitcond_nwaiters(
				&pml->pml_mwcond_empty);
			MLIST_ULOCK(pml);

			rc = psc_ctlmsg_sendv(fd, mh, pcml);
			if (!rc)
				break;

			/* Terminate on exact match. */
			if (strcmp(pml->pml_name, name) == 0)
				break;
		}
	PLL_ULOCK(&psc_mlists);
	if (rc && !found && !all)
		rc = psc_ctlsenderr(fd, mh, "unknown mlist: %s", name);
	return (rc);
}

/*
 * Respond to a "GETODTABLE" control inquiry.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @m: control message to be filled in and sent out.
 */
int
psc_ctlrep_getodtable(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct psc_ctlmsg_odtable *pco = m;
	struct pfl_odt *odt;
	char name[ODT_NAME_MAX];
	int rc, found, all;

	rc = 1;
	found = 0;
	strlcpy(name, pco->pco_name, sizeof(name));
	all = (name[0] == '0');

	PLL_LOCK(&pfl_odtables);
	PLL_FOREACH(odt, &pfl_odtables) {
		if (all || strncmp(name,
		    odt->odt_name, strlen(name)) == 0) {
			found = 1;

			snprintf(pco->pco_name, sizeof(pco->pco_name),
			    "%s", odt->odt_name);
			pco->pco_elemsz = odt->odt_hdr->odth_objsz;
			pco->pco_opts = odt->odt_hdr->odth_options;
			psc_vbitmap_getstats(odt->odt_bitmap,
			    &pco->pco_inuse, &pco->pco_total);
			rc = psc_ctlmsg_sendv(fd, mh, pco);
			if (!rc)
				break;

			/* Terminate on exact match. */
			if (strcmp(odt->odt_name, name) == 0)
				break;
		}
	}
	PLL_ULOCK(&pfl_odtables);

	if (rc && !found && !all)
		rc = psc_ctlsenderr(fd, mh, "unknown odtable: %s",
		    name);
	return (rc);
}

__weak struct psc_lockedlist *
pfl_journals_get(void)
{
	return (NULL);
}

/*
 * Respond to a "GETJOURNAL" control inquiry.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @m: control message to be filled in and sent out.
 */
int
psc_ctlrep_getjournal(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct psc_ctlmsg_journal *pcj = m;
	struct psc_lockedlist *pll;
	struct psc_journal *j;
	int rc;

	rc = 1;

	pll = pfl_journals_get();
	if (pll == NULL)
		return (rc);
	PLL_LOCK(pll);
	PLL_FOREACH(j, pll) {
		PJ_LOCK(j);
		strlcpy(pcj->pcj_name, j->pj_name,
		    sizeof(pcj->pcj_name));
		pcj->pcj_flags		= j->pj_flags;
		pcj->pcj_inuse		= j->pj_inuse;
		pcj->pcj_total		= j->pj_total;
		pcj->pcj_resrv		= j->pj_resrv;
		pcj->pcj_lastxid	= j->pj_lastxid;
		pcj->pcj_commit_txg	= j->pj_commit_txg;
		pcj->pcj_replay_xid	= j->pj_replay_xid;
		pcj->pcj_dstl_xid	= j->pj_distill_xid;
		pcj->pcj_pndg_xids_cnt	= pll_nitems(&j->pj_pendingxids);
		pcj->pcj_dstl_xids_cnt	= pll_nitems(&j->pj_distillxids);
		pcj->pcj_bufs_cnt	= psc_dynarray_len(&j->pj_bufs);
		pcj->pcj_nwaiters	= psc_waitq_nwaiters(&j->pj_waitq);
		pcj->pcj_nextwrite	= j->pj_nextwrite;
		pcj->pcj_wraparound	= j->pj_wraparound;
		PJ_ULOCK(j);

		rc = psc_ctlmsg_sendv(fd, mh, pcj);
		if (!rc)
			break;
	}
	PLL_ULOCK(pll);
	return (rc);
}

/*
 * Invoke an operation on all applicable threads.
 * @fd: client socket descriptor.
 * @mh: already filled-in control message header.
 * @m: control message to examine.
 * @thrname: name of thread to match on.
 * @cbf: callback to run for matching threads.
 */
int
psc_ctl_applythrop(int fd, struct psc_ctlmsghdr *mh, void *m,
    const char *thrname, int (*cbf)(int, struct psc_ctlmsghdr *, void *,
      struct psc_thread *))
{
	struct psc_thread *thr;
	int rc, len, nsz, found;

	rc = 1;
	found = 0;
	PLL_LOCK(&psc_threads);
	if (strcasecmp(thrname, PCTHRNAME_EVERYONE) == 0 ||
	    thrname[0] == '\0') {
		psclist_for_each_entry(thr,
		    &psc_threads.pll_listhd, pscthr_lentry) {
			rc = cbf(fd, mh, m, thr);
			if (!rc)
				break;
		}
	} else {
		len = strlen(thrname);
		psclist_for_each_entry(thr,
		    &psc_threads.pll_listhd, pscthr_lentry) {
			nsz = strcspn(thr->pscthr_name, "0123456789");
			if (len && strncasecmp(thrname,
			    thr->pscthr_name, len) == 0 &&
			    (len <= nsz ||
			     len == (int)strlen(thr->pscthr_name))) {
				found = 1;
				rc = cbf(fd, mh, m, thr);
				if (!rc)
					break;
			}
		}
		if (!found)
			rc = psc_ctlsenderr(fd, mh,
			    "unknown thread: %s", thrname);
	}
	PLL_ULOCK(&psc_threads);
	return (rc);
}

/*
 * Satisfy a client connection.
 * @fd: client socket descriptor.
 * @ct: control operation table.
 * @nops: number of operations in table.
 * @msiz: value-result of buffer size allocated to subsequent message
 *	processing.
 * @pm: value-result buffer for subsequent message processing.
 *
 * Notes: pscthr_yield() is not explicity called throughout this routine,
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
 * Disadvantages: if we sleep during processing of client connection,
 * we deny service to new clients.
 */
__static int
psc_ctlthr_service(int fd, const struct psc_ctlop *ct, int nops,
    size_t *msiz, void *pm)
{
	struct psc_ctlmsghdr mh;
	ssize_t n;
	void *m;

	m = *(void **)pm;

	n = recv(fd, &mh, sizeof(mh), MSG_WAITALL | PFL_MSG_NOSIGNAL);
	if (n == 0)
		return (EOF);
	if (n == -1) {
		if (errno == EPIPE || errno == ECONNRESET)
			return (EOF);
		if (errno == EINTR)
			return (0);
		psc_fatal("recvmsg");
	}

	if (n != sizeof(mh)) {
		psclog_notice("short recv on psc_ctlmsghdr; "
		    "expected=%zd got=%zd", sizeof(mh), n);
		return (0);
	}
	if (mh.mh_size > *msiz) {
		*msiz = mh.mh_size;
		m = *(void **)pm = psc_realloc(m, *msiz, 0);
	}

 again:
	if (mh.mh_size) {
		n = recv(fd, m, mh.mh_size, MSG_WAITALL |
		    PFL_MSG_NOSIGNAL);
		if (n == -1) {
			if (errno == EPIPE || errno == ECONNRESET)
				return (EOF);
			if (errno == EINTR)
				goto again;
			psc_fatal("recv");
		}
		if ((size_t)n != mh.mh_size) {
			psclog_warn("short recv on psc_ctlmsg contents; "
			    "got=%zu; expected=%zu",
			    n, mh.mh_size);
			return (EOF);
		}
	}
	if (mh.mh_type < 0 ||
	    mh.mh_type >= nops ||
	    ct[mh.mh_type].pc_op == NULL) {
		psc_ctlsenderr(fd, &mh,
		    "unrecognized psc_ctlmsghdr type; "
		    "type=%d size=%zu nops=%d", mh.mh_type, mh.mh_size, nops);
		return (0);
	}
	if (ct[mh.mh_type].pc_siz &&
	    ct[mh.mh_type].pc_siz != mh.mh_size) {
		psc_ctlsenderr(fd, &mh,
		    "invalid ctlmsg size; type=%d, size=%zu, want=%zu",
		    mh.mh_type, mh.mh_size, ct[mh.mh_type].pc_siz);
		return (0);
	}
	psc_ctlthr(pscthr_get())->pct_stat.nrecv++;
	if (!ct[mh.mh_type].pc_op(fd, &mh, m))
		return (EOF);
	return (0);
}

/*
 * Control thread connection acceptor.
 * @thr: thread.
 */
void
psc_ctlacthr_main(struct psc_thread *thr)
{
	int s, fd;

	s = psc_ctlacthr(thr)->pcat_sock;
	while (pscthr_run(thr)) {
		fd = accept(s, NULL, NULL);
		if (fd == -1) {
			if (errno == EINTR) {
				usleep(300);
				continue;
			}
			psc_fatal("accept");
		}
		psc_ctlacthr(pscthr_get())->pcat_stat.nclients++;

		spinlock(&psc_ctl_clifds_lock);
		psc_dynarray_add(&psc_ctl_clifds, (void *)(long)fd);
		psc_waitq_wakeall(&psc_ctl_clifds_waitq);
		freelock(&psc_ctl_clifds_lock);
	}
}

void
psc_ctlthr_mainloop(struct psc_thread *thr)
{
	const struct psc_ctlop *ct;
	size_t bufsiz = 0;
	void *buf = NULL;
	uint32_t rnd;
	int s, nops;

	ct = psc_ctlthr(thr)->pct_ct;
	nops = psc_ctlthr(thr)->pct_nops;
	while (pscthr_run(thr)) {
		spinlock(&psc_ctl_clifds_lock);
		if (psc_dynarray_len(&psc_ctl_clifds) == 0) {
			psc_waitq_wait(&psc_ctl_clifds_waitq,
			    &psc_ctl_clifds_lock);
			continue;
		}
		rnd = psc_random32u(psc_dynarray_len(&psc_ctl_clifds));
		s = (int)(long)psc_dynarray_getpos(&psc_ctl_clifds, rnd);
		psc_dynarray_remove(&psc_ctl_clifds, (void *)(long)s);
		freelock(&psc_ctl_clifds_lock);

		if (!psc_ctlthr_service(s, ct, nops, &bufsiz, &buf)) {
			spinlock(&psc_ctl_clifds_lock);
			psc_dynarray_add(&psc_ctl_clifds, (void *)(long)s);
			psc_waitq_wakeall(&psc_ctl_clifds_waitq);
			freelock(&psc_ctl_clifds_lock);
		} else
			close(s);
	}
	PSCFREE(buf);
}

void
psc_ctlthr_spawn_listener(const char *ofn, int acthrtype)
{
	extern const char *__progname;
	static psc_atomic32_t idx;

	struct psc_thread *thr, *me;
	struct sockaddr_un saun;
	mode_t old_umask;
	char *p;
	int s;

	me = pscthr_get();

	p = strstr(me->pscthr_name, "ctlthr");
	if (p == NULL)
		psc_fatalx("'ctlthr' not found in thread name '%s'",
		    me->pscthr_name);

	s = socket(AF_LOCAL, SOCK_STREAM, PF_UNSPEC);
	if (s == -1)
		psc_fatal("socket");

	memset(&saun, 0, sizeof(saun));
	saun.sun_family = AF_LOCAL;
	SOCKADDR_SETLEN(&saun);

	/* perform transliteration for "variables" in file path */
	(void)FMTSTR(saun.sun_path, sizeof(saun.sun_path), ofn,
		FMTSTRCASE('h', "s", psclog_getdata()->pld_hostshort)
		FMTSTRCASE('n', "s", __progname)
	);

	if (unlink(saun.sun_path) == -1 && errno != ENOENT)
		psclog_error("unlink %s", saun.sun_path);

	spinlock(&psc_umask_lock);
	old_umask = umask(S_IXUSR | S_IXGRP | S_IWOTH | S_IROTH |
	    S_IXOTH);
	if (bind(s, (struct sockaddr *)&saun, sizeof(saun)) == -1)
		psc_fatal("bind %s", saun.sun_path);
	umask(old_umask);
	freelock(&psc_umask_lock);

	/* XXX fchmod */
	if (chmod(saun.sun_path, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP |
	    S_IROTH | S_IWOTH) == -1)
		psc_fatal("chmod %s", saun.sun_path); /* XXX errno */

	if (listen(s, QLEN) == -1)
		psc_fatal("listen");

	/*
	 * Spawn a servicing thread to separate processing from acceptor
	 * and to multiplex between clients for fairness.
	 */
	thr = pscthr_init(acthrtype, psc_ctlacthr_main, NULL,
	    sizeof(struct psc_ctlacthr), "%.*sctlacthr%d",
	    p - me->pscthr_name, me->pscthr_name,
	    psc_atomic32_inc_getnew(&idx) - 1);
	psc_ctlacthr(thr)->pcat_sock = s;
	pscthr_setready(thr);
}

/*
 * Main control thread client service loop.
 * @ofn: path to control socket.
 * @ct: control operations.
 * @nops: number of operations in @ct table.
 * @acthrtype: control acceptor thread type.
 */
void
psc_ctlthr_main(const char *ofn, const struct psc_ctlop *ct, int nops,
    int acthrtype)
{
	struct psc_thread *thr, *me;
	const char *p;
	int i;

	me = pscthr_get();

	p = strstr(me->pscthr_name, "ctlthr");
	if (p == NULL)
		psc_fatalx("'ctlthr' not found in control thread name");

	psc_ctlthr_spawn_listener(ofn, acthrtype);

#define PFL_CTL_NTHRS 4
	for (i = 1; i < PFL_CTL_NTHRS; i++) {
		thr = pscthr_init(me->pscthr_type, psc_ctlthr_mainloop,
		    NULL, me->pscthr_privsiz, "%.*sctlthr%d",
		    p - me->pscthr_name, me->pscthr_name, i);
		psc_ctlthr(thr)->pct_ct = ct;
		psc_ctlthr(thr)->pct_nops = nops;
		pscthr_setready(thr);
	}

	psc_ctlthr(me)->pct_ct = ct;
	psc_ctlthr(me)->pct_nops = nops;
	psc_ctlthr_mainloop(me);
}
