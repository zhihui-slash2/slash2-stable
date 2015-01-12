/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2006-2014, Pittsburgh Supercomputing Center (PSC).
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
 * Definitions for the daemon live control interface.
 */

#ifndef _PFL_CTLSVR_H_
#define _PFL_CTLSVR_H_

#include <ctype.h>
#include <string.h>

#include "pfl/str.h"
#include "pfl/atomic.h"
#include "pfl/thread.h"

struct psc_ctlmsghdr;
struct psc_ctlmsg_param;
struct psc_ctlmsg_thread;
struct psc_ctlparam_node;

#define PSC_CTL_FOREACH_THREAD(thr, thrname, threads)				\
	psclist_for_each_entry((thr), (threads), pscthr_lentry)			\
		if (strncmp((thr)->pscthr_name, (thrname),			\
		    strlen(thrname)) == 0 ||					\
		    strcmp((thrname), PCTHRNAME_EVERYONE) == 0)

/* XXX use PSCTHR_MKCAST */
#define psc_ctlthr(thr)		((struct psc_ctlthr *)(thr)->pscthr_private)
#define psc_ctlacthr(thr)	((struct psc_ctlacthr *)(thr)->pscthr_private)

#ifdef PFL_RPC
#  define PFL_CTLOPS_GETRPCSVC \
	{ psc_ctlrep_getrpcsvc,		sizeof(struct psc_ctlmsg_rpcsvc) },
#else
#  define PFL_CTLOPS_GETRPCSVC
#endif

/* default control operations shared by all controllable daemons */
#define PSC_CTLDEFOPS								\
	{ NULL,				0 },					\
	{ psc_ctlrep_getfault,		sizeof(struct psc_ctlmsg_fault) },	\
	{ psc_ctlrep_gethashtable,	sizeof(struct psc_ctlmsg_hashtable) },	\
	{ psc_ctlrep_getiostats,	sizeof(struct psc_ctlmsg_iostats) },	\
	{ psc_ctlrep_getjournal,	sizeof(struct psc_ctlmsg_journal) },	\
	{ psc_ctlrep_getlc,		sizeof(struct psc_ctlmsg_lc) },		\
	{ psc_ctlrep_getlni,		sizeof(struct psc_ctlmsg_lni) },	\
	{ psc_ctlrep_getloglevel,	sizeof(struct psc_ctlmsg_loglevel) },	\
	{ psc_ctlrep_getmeter,		sizeof(struct psc_ctlmsg_meter) },	\
	{ psc_ctlrep_getmlist,		sizeof(struct psc_ctlmsg_mlist) },	\
	{ psc_ctlrep_getodtable,	sizeof(struct psc_ctlmsg_odtable) },	\
	{ psc_ctlrep_param,		sizeof(struct psc_ctlmsg_param) },	\
	{ psc_ctlrep_getpool,		sizeof(struct psc_ctlmsg_pool) },	\
	PFL_CTLOPS_GETRPCSVC							\
	{ psc_ctlrep_getsubsys,		0 },					\
	{ psc_ctlrep_getthread,		sizeof(struct psc_ctlmsg_thread) },	\
	{ psc_ctlrep_param,		sizeof(struct psc_ctlmsg_param) }

struct psc_ctlacthr {
	int			 pcat_sock;
	struct {
		int nclients;
	}			 pcat_stat;
};

struct psc_ctlthr {
	int			 pct_sockfd;
	int			 pct_nops;
	const struct psc_ctlop	*pct_ct;
	struct {
		int nsent;
		int nrecv;
		int ndrop;
	}			 pct_stat;
};

struct psc_ctlop {
	int	(*pc_op)(int, struct psc_ctlmsghdr *, void *);
	size_t	  pc_siz;
};

struct pfl_opstat {
	const char		*pos_name;
	psc_atomic64_t		 pos_value;
};

#define OPSTATS_MAX		256

#define	OPSTAT_ADD(op, n)						\
	do {								\
		static int _opst_idx = -1;				\
									\
		if (_opst_idx == -1)					\
			_opst_idx = pfl_opstats_lookup(op);		\
		psc_atomic64_add(&pflctl_opstats[_opst_idx].pos_value,	\
		    (n));						\
	} while (0)

#define	OPSTAT_INCR(op)		OPSTAT_ADD((op), 1)

/* XXX race: should use cmpxchg */
#define	OPSTAT_SET_MAX(op, value)					\
	do {								\
		static int _opst_idx = -1;				\
		struct pfl_opstat *_pos;				\
									\
		if (_opst_idx == -1)					\
			_opst_idx = pfl_opstats_lookup(op);		\
		_pos = &pflctl_opstats[_opst_idx];			\
		if ((value) > psc_atomic64_read(&_pos->pos_value))	\
			psc_atomic64_set(&_pos->pos_value, (value));	\
	} while (0)

int	psc_ctlsenderr(int, const struct psc_ctlmsghdr *, const char *, ...);

int	psc_ctlmsg_sendv(int, const struct psc_ctlmsghdr *, const void *);
int	psc_ctlmsg_send(int, int, int, size_t, const void *);

int	psc_ctlrep_getfault(int, struct psc_ctlmsghdr *, void *);
int	psc_ctlrep_gethashtable(int, struct psc_ctlmsghdr *, void *);
int	psc_ctlrep_getiostats(int, struct psc_ctlmsghdr *, void *);
int	psc_ctlrep_getjournal(int, struct psc_ctlmsghdr *, void *);
int	psc_ctlrep_getlc(int, struct psc_ctlmsghdr *, void *);
int	psc_ctlrep_getlni(int, struct psc_ctlmsghdr *, void *);
int	psc_ctlrep_getloglevel(int, struct psc_ctlmsghdr *, void *);
int	psc_ctlrep_getmeter(int, struct psc_ctlmsghdr *, void *);
int	psc_ctlrep_getmlist(int, struct psc_ctlmsghdr *, void *);
int	psc_ctlrep_getodtable(int, struct psc_ctlmsghdr *, void *);
int	psc_ctlrep_getpool(int, struct psc_ctlmsghdr *, void *);
int	psc_ctlrep_getrpcsvc(int, struct psc_ctlmsghdr *, void *);
int	psc_ctlrep_getsubsys(int, struct psc_ctlmsghdr *, void *);
int	psc_ctlrep_getthread(int, struct psc_ctlmsghdr *, void *);
int	psc_ctlrep_param(int, struct psc_ctlmsghdr *, void *);

void	psc_ctlthr_get(struct psc_thread *, struct psc_ctlmsg_thread *);
void	psc_ctlacthr_get(struct psc_thread *, struct psc_ctlmsg_thread *);

int	psc_ctlparam_log_file(int, struct psc_ctlmsghdr *,
		struct psc_ctlmsg_param *, char **, int, struct psc_ctlparam_node *);
int	psc_ctlparam_log_format(int, struct psc_ctlmsghdr *,
		struct psc_ctlmsg_param *, char **, int, struct psc_ctlparam_node *);
int	psc_ctlparam_log_level(int, struct psc_ctlmsghdr *,
		struct psc_ctlmsg_param *, char **, int, struct psc_ctlparam_node *);
int	psc_ctlparam_log_points(int, struct psc_ctlmsghdr *,
		struct psc_ctlmsg_param *, char **, int, struct psc_ctlparam_node *);
int	psc_ctlparam_pool(int, struct psc_ctlmsghdr *,
		struct psc_ctlmsg_param *, char **, int, struct psc_ctlparam_node *);
int	psc_ctlparam_rlim(int, struct psc_ctlmsghdr *,
		struct psc_ctlmsg_param *, char **, int, struct psc_ctlparam_node *);
int	psc_ctlparam_rusage(int, struct psc_ctlmsghdr *,
		struct psc_ctlmsg_param *, char **, int, struct psc_ctlparam_node *);
int	psc_ctlparam_run(int, struct psc_ctlmsghdr *,
		struct psc_ctlmsg_param *, char **, int, struct psc_ctlparam_node *);
int	psc_ctlparam_opstats(int, struct psc_ctlmsghdr *,
		struct psc_ctlmsg_param *, char **, int, struct psc_ctlparam_node *);
int	psc_ctlparam_pause(int, struct psc_ctlmsghdr *,
		struct psc_ctlmsg_param *, char **, int, struct psc_ctlparam_node *);
int	psc_ctlparam_faults(int, struct psc_ctlmsghdr *,
		struct psc_ctlmsg_param *, char **, int, struct psc_ctlparam_node *);

enum pflctl_paramt {
	PFLCTL_PARAMT_NONE,
	PFLCTL_PARAMT_ATOMIC32,
	PFLCTL_PARAMT_INT,
	PFLCTL_PARAMT_STR,
	PFLCTL_PARAMT_UINT64
};

#define PFLCTL_PARAMF_RDWR	(1 << 0)

struct psc_ctlparam_node *
	psc_ctlparam_register(const char *, int (*)(int, struct psc_ctlmsghdr *,
		struct psc_ctlmsg_param *, char **, int, struct psc_ctlparam_node *));
void	psc_ctlparam_register_simple(const char *, void (*)(char *), int (*)(const char *));
void	psc_ctlparam_register_var(const char *, enum pflctl_paramt, int, void *);

int	psc_ctlmsg_param_send(int, const struct psc_ctlmsghdr *,
		struct psc_ctlmsg_param *, const char *, char **, int, const char *);

void	psc_ctlthr_main(const char *, const struct psc_ctlop *, int, int);
int	psc_ctl_applythrop(int, struct psc_ctlmsghdr *, void *, const char *,
		int (*)(int, struct psc_ctlmsghdr *, void *, struct psc_thread *));

int	pfl_opstats_lookup(const char *);

typedef void (*psc_ctl_thrget_t)(struct psc_thread *, struct psc_ctlmsg_thread *);

extern psc_ctl_thrget_t psc_ctl_thrgets[];
extern int psc_ctl_nthrgets;

extern struct pfl_opstat pflctl_opstats[];

#define PFLCTL_SVR_DEFS							\
int psc_ctl_nthrgets = nitems(psc_ctl_thrgets);				\

#endif /* _PFL_CTLSVR_H_ */
