/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2007-2015, Pittsburgh Supercomputing Center (PSC).
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
 * Debug/logging routines.
 */

#include <sys/param.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <paths.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "pfl/alloc.h"
#include "pfl/cdefs.h"
#include "pfl/fmtstr.h"
#include "pfl/hashtbl.h"
#include "pfl/log.h"
#include "pfl/pfl.h"
#include "pfl/str.h"
#include "pfl/thread.h"
#include "pfl/time.h"

#ifndef PSC_LOG_FMT
#define PSC_LOG_FMT "[%s:%06u %n:%I:%T %B %F %l] "
#endif

const char			*psc_logfmt = PSC_LOG_FMT;
__static int			 psc_loglevel = PLL_NOTICE;
__static struct psclog_data	*psc_logdata;
char				 psclog_eol[8] = "\n";	/* overrideable with ncurses EOL */
int				*pfl_syslog;
FILE				*pflog_ttyfp;

struct psc_dynarray		_pfl_logpoints = DYNARRAY_INIT_NOLOG;
struct psc_hashtbl		_pfl_logpoints_hashtbl;

int pfl_syslog_map[] = {
/* fatal */	LOG_EMERG,
/* error */	LOG_ERR,
/* warn */	LOG_WARNING,
/* notice */	LOG_NOTICE,
/* info */	LOG_INFO
};

int
psc_log_setfn(const char *p, const char *mode)
{
	static int logger_pid = -1;
	char *lp, fn[PATH_MAX];
	struct timeval tv;
	int rc;

	PFL_GETTIMEVAL(&tv);
	(void)FMTSTR(fn, sizeof(fn), p,
		FMTSTRCASE('t', "d", tv.tv_sec)
	);
	if (freopen(fn, mode, stderr) == NULL)
		return (errno);

	lp = getenv("PSC_LOG_FILE_LINK");
	if (lp) {
		if (unlink(lp) == -1 && errno != ENOENT)
			warn("unlink %s", lp);
		if (link(fn, lp) == -1)
			warn("link %s", lp);
	}

#if 0
	struct statvfs sfb;

	if (fstatvfs(fileno(stderr), &sfb) == -1)
		warn("statvfs stderr");
	else {
		if (nfs)
			warn("warning: error log file over NFS");
	}
#endif

	lp = getenv("PFL_SYSLOG_PIPE");
	if (lp) {
		/* cleanup old */
		if (logger_pid != -1)
			kill(logger_pid, SIGINT);

		/* launch new */
		logger_pid = fork();
		switch (logger_pid) {
		case -1:
			warn("fork");
			break;
		case 0: {
			char cmdbuf[LINE_MAX];

			rc = snprintf(cmdbuf, sizeof(cmdbuf),
			    "tail -f %s | %s", fn, lp);
			if (rc < 0 || rc > (int)sizeof(cmdbuf))
				errx(1, "snprintf");
			exit(system(cmdbuf));
		    }
		default:
			rc = waitpid(logger_pid, NULL, 0);
			break;
		}
	}

	return (0);
}

void
psc_log_init(void)
{
	char *p;

	p = getenv("PSC_LOG_FILE");
	if (p && psc_log_setfn(p, "w"))
		warn("%s", p);

	p = getenv("PSC_LOG_FORMAT");
	if (p)
		psc_logfmt = p;

	p = getenv("PSC_LOG_LEVEL");
	if (p) {
		psc_loglevel = psc_loglevel_fromstr(p);
		if (psc_loglevel == PNLOGLEVELS)
			errx(1, "invalid PSC_LOG_LEVEL: %s", p);
	}

	_psc_hashtbl_init(&_pfl_logpoints_hashtbl, PHTF_STRP |
	    PHTF_NOLOG, offsetof(struct pfl_logpoint, plogpt_key),
	    sizeof(struct pfl_logpoint), 3067, NULL, "logpoints");

	if (!isatty(fileno(stderr)))
		pflog_ttyfp = fopen(_PATH_TTY, "w");
}

int
psc_log_getlevel_global(void)
{
	return (psc_loglevel);
}

void
psc_log_setlevel_global(int newlevel)
{
	if (newlevel >= PNLOGLEVELS || newlevel < 0)
		errx(1, "log level out of bounds (%d)", newlevel);
	psc_loglevel = newlevel;
}

#ifndef HAVE_LIBPTHREAD

int
psc_log_getlevel(int subsys)
{
	return (psc_log_getlevel_ss(subsys));
}

void
psc_log_setlevel(int subsys, int newlevel)
{
	psc_log_setlevel_ss(subsys, newlevel);
}

#endif

#ifndef HAVE_MPI
/**
 * MPI_Comm_rank - Dummy overrideable MPI rank retriever.
 */
int
MPI_Comm_rank(__unusedx int comm, int *rank)
{
	*rank = -1;
	return (0);
}
#endif

const char *
pflog_get_fsctx_uprog_stub(__unusedx struct psc_thread *thr)
{
	return (NULL);
}

uid_t
pflog_get_fsctx_uid_stub(__unusedx struct psc_thread *thr)
{
	return (-1);
}

pid_t
pflog_get_fsctx_pid_stub(__unusedx struct psc_thread *thr)
{
	return (-1);
}

const char	*(*pflog_get_fsctx_uprog)(struct psc_thread *) =
		    pflog_get_fsctx_uprog_stub;
pid_t		 (*pflog_get_fsctx_pid)(struct psc_thread *) =
		    pflog_get_fsctx_pid_stub;
uid_t		 (*pflog_get_fsctx_uid)(struct psc_thread *) =
		    pflog_get_fsctx_uid_stub;

struct psclog_data *
psclog_getdata(void)
{
	struct psclog_data *d;
	char *p;

	d = pfl_tls_get(PFL_TLSIDX_LOGDATA, sizeof(*d));
	if (d->pld_thrid == 0) {
		/* XXX use psc_get_hostname() */
		if (gethostname(d->pld_hostname,
		    sizeof(d->pld_hostname)) == -1)
			err(1, "gethostname");
		strlcpy(d->pld_hostshort, d->pld_hostname,
		    sizeof(d->pld_hostshort));
		if ((p = strchr(d->pld_hostshort, '.')) != NULL)
			*p = '\0';
		/* XXX try to read this if the pscthr is available */
		d->pld_thrid = pfl_getsysthrid();
		snprintf(d->pld_nothrname, sizeof(d->pld_nothrname),
		    "<%"PSCPRI_PTHRT">", pthread_self());

#ifdef HAVE_CNOS
		d->pld_rank = cnos_get_rank();
#else
		MPI_Comm_rank(1, &d->pld_rank); /* 1=MPI_COMM_WORLD */
#endif
	}
	return (d);
}

const char *
pfl_fmtlogdate(const struct timeval *tv, const char **s)
{
	char fmtbuf[LINE_MAX], *bufp;
	const char *end, *start;
	struct tm tm;
	time_t sec;

	start = *s + 1;
	if (*start != '<')
		errx(1, "invalid log prefix format: %s", start);
	for (end = start++;
	    *end && *end != '>' && end - start < LINE_MAX; end++)
		;
	if (*end != '>')
		errx(1, "invalid log prefix format: %s", end);

	memcpy(fmtbuf, start, end - start);
	fmtbuf[end - start] = '\0';

	bufp = pfl_tls_get(PFL_TLSIDX_LOGDATEBUF, LINE_MAX);

	sec = tv->tv_sec;
	localtime_r(&sec, &tm);
	strftime(bufp, LINE_MAX, fmtbuf, &tm);

	*s = end;
	return (bufp);
}

__weak const char *
pfl_strerror(int rc)
{
	return (strerror(rc));
}

void
_psclogv(const struct pfl_callerinfo *pci, int level, int options,
    const char *fmt, va_list ap)
{
	char *p, buf[BUFSIZ];
	extern const char *__progname;
	struct psc_thread *thr;
	struct psclog_data *d;
	struct timeval tv;
	const char *thrname;
	int rc, save_errno;
	pid_t thrid;
	size_t len;

	save_errno = errno;

	d = psclog_getdata();
	if (d->pld_flags & PLDF_INLOG) {
//		write(); ?
		// also place line, file, etc
		vfprintf(stderr, fmt, ap); /* XXX syslog, etc */

		if (level == PLL_FATAL)
			abort();
		goto out;
	}
	d->pld_flags |= PLDF_INLOG;

	thr = pscthr_get_canfail();
	if (thr) {
		thrid = thr->pscthr_thrid;
		thrname = thr->pscthr_name;
	} else {
		thrid = d->pld_thrid;
		thrname = d->pld_nothrname;
	}

	gettimeofday(&tv, NULL);
	(void)FMTSTR(buf, sizeof(buf), psc_logfmt,
		FMTSTRCASE('B', "s", pfl_basename(pci->pci_filename))
		FMTSTRCASE('D', "s", pfl_fmtlogdate(&tv, &_t))
		FMTSTRCASE('F', "s", pci->pci_func)
		FMTSTRCASE('f', "s", pci->pci_filename)
		FMTSTRCASE('H', "s", d->pld_hostname)
		FMTSTRCASE('h', "s", d->pld_hostshort)
		FMTSTRCASE('I', PSCPRI_PTHRT, pthread_self())
		FMTSTRCASE('i', "d", thrid)
		FMTSTRCASE('L', "d", level)
		FMTSTRCASE('l', "d", pci->pci_lineno)
		FMTSTRCASE('N', "s", __progname)
		FMTSTRCASE('X', "s", pflog_get_fsctx_uprog(thr))
		FMTSTRCASE('n', "s", thrname)
		FMTSTRCASE('P', "d", pflog_get_fsctx_pid(thr))
		FMTSTRCASE('r', "d", d->pld_rank)
//		FMTSTRCASE('S', "s", call stack)
		FMTSTRCASE('s', "lu", tv.tv_sec)
		FMTSTRCASE('T', "s", psc_subsys_name(pci->pci_subsys))
		FMTSTRCASE('t', "d", pci->pci_subsys)
		FMTSTRCASE('U', "d", pflog_get_fsctx_uid(thr))
		FMTSTRCASE('u', "lu", tv.tv_usec)
	);

	len = strlen(buf);
	rc = vsnprintf(buf + len, sizeof(buf) - len, fmt, ap);
	if (rc != -1)
		len = strlen(buf);
	/* trim newline if present, since we add our own */
	if (len && buf[len - 1] == '\n')
		buf[--len] = '\0';
	if (options & PLO_ERRNO)
		snprintf(buf + len, sizeof(buf) - len,
		    ": %s", pfl_strerror(save_errno));

	PSCLOG_LOCK();

	/* XXX consider using fprintf_unlocked() for speed */
	fprintf(stderr, "%s%s", buf, psclog_eol);
	fflush(stderr);

	if (pfl_syslog && pfl_syslog[pci->pci_subsys] &&
	    level >= 0 && level < (int)nitems(pfl_syslog_map))
		syslog(pfl_syslog_map[level], "%s", buf);

	if (level <= PLL_WARN && pflog_ttyfp)
		fprintf(pflog_ttyfp, "%s\n", buf);

	if (level == PLL_FATAL) {
		p = getenv("PSC_DUMPSTACK");
		if (p && strcmp(p, "0"))
			pfl_dump_stack();
		d->pld_flags &= ~PLDF_INLOG;
		pfl_abort();
	}

	PSCLOG_UNLOCK();

	d->pld_flags &= ~PLDF_INLOG;

 out:
	/*
	 * Restore in case app needs it after our printf()'s may have
	 * modified it.
	 */
	errno = save_errno;
}

void
_psclog(const struct pfl_callerinfo *pci, int level, int options,
    const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	_psclogv(pci, level, options, fmt, ap);
	va_end(ap);
}

__dead void
_psc_fatal(const struct pfl_callerinfo *pci, int level, int options,
    const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	_psclogv(pci, level, options, fmt, ap);
	va_end(ap);

	errx(1, "should not reach here");
}

__dead void
_psc_fatalv(const struct pfl_callerinfo *pci, int level, int options,
    const char *fmt, va_list ap)
{
	_psclogv(pci, level, options, fmt, ap);
	errx(1, "should not reach here");
}

/* Keep synced with PLL_* constants. */
const char *psc_loglevel_names[] = {
	"fatal",
	"error",
	"warn",
	"notice",
	"info",
	"diag",
	"debug",
	"vdebug",
	"trace"
};

const char *
psc_loglevel_getname(int id)
{
	if (id < 0)
		return ("<unknown>");
	else if (id >= PNLOGLEVELS)
		return ("<unknown>");
	return (psc_loglevel_names[id]);
}

int
psc_loglevel_fromstr(const char *name)
{
	struct {
		const char		*lvl_name;
		int			 lvl_value;
	} altloglevels[] = {
		{ "none",		PLL_FATAL },
		{ "fatals",		PLL_FATAL },
		{ "errors",		PLL_ERROR },
		{ "warning",		PLL_WARN },
		{ "warnings",		PLL_WARN },
		{ "notify",		PLL_NOTICE },
		{ "all",		PLL_TRACE }
	};
	char *endp;
	size_t n;
	long l;

	for (n = 0; n < PNLOGLEVELS; n++)
		if (strcasecmp(name, psc_loglevel_names[n]) == 0)
			return (n);
	for (n = 0; n < nitems(altloglevels); n++)
		if (strcasecmp(name, altloglevels[n].lvl_name) == 0)
			return (altloglevels[n].lvl_value);

	l = strtol(name, &endp, 10);
	if (endp == name || *endp != '\0' || l < 0 || l >= PNLOGLEVELS)
		return (PNLOGLEVELS);
	return (l);
}

struct pfl_logpoint *
_pfl_get_logpointid(const char *fn, int line, int create)
{
	static struct psc_spinlock lock = SPINLOCK_INIT_NOLOG;
	struct psc_hashtbl *t = &_pfl_logpoints_hashtbl;
	struct pfl_logpoint *pt;
	struct psc_hashbkt *b;
	char *key;

	if (asprintf(&key, "%s:%d", pfl_basename(fn), line) == -1)
		err(1, NULL);
	pt = psc_hashtbl_search(t, key);
	if (pt || create == 0)
		goto out;

	b = psc_hashbkt_get(t, key);
	pt = psc_hashbkt_search(t, b, key);
	if (pt == NULL) {
		pt = psc_alloc(sizeof(*pt) + sizeof(struct pfl_hashentry),
		    PAF_NOLOG);
		pt->plogpt_key = key;
		key = NULL;
		psc_hashent_init(t, pt);

		psc_hashbkt_add_item(t, b, pt);

		spinlock(&lock);
		pt->plogpt_idx = psc_dynarray_len(&_pfl_logpoints);
		psc_dynarray_add(&_pfl_logpoints, NULL);
		freelock(&lock);
	}
	psc_hashbkt_put(t, b);

 out:
	free(key);
	return (pt);
}
