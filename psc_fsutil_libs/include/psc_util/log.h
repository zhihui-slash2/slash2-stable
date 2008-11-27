/* $Id$ */

#include <stdarg.h>

#ifndef PSC_SUBSYS
# define PSC_SUBSYS PSS_OTHER
#endif

#include "psc_util/subsys.h"
#include "psc_util/cdefs.h"

/* Log levels. */
#define PLL_FATAL	0 /* process/thread termination */
#define PLL_ERROR	1 /* recoverable failure */
#define PLL_WARN	2 /* something wrong, require attention */
#define PLL_NOTICE	3 /* something unusual, recommend attention */
#define PLL_INFO	4 /* general information */
#define PLL_DEBUG	5 /* debug messages */
#define PLL_TRACE	6 /* flow */
#define PNLOGLEVELS	7

/* Logging options. */
#define PLO_ERRNO	(1 << 0)	/* strerror(errno) */

#define _psclogck(ss, lvl, flg, fmt, ...)				\
	do {								\
		if (psc_log_getlevel(ss) >= (lvl))			\
			_psclog(__FILE__, __func__, __LINE__, (ss),	\
			    (lvl), (flg), (fmt), ## __VA_ARGS__);	\
	} while (0)

#define psclogvck(ss, lvl, flg, fmt, ap)				\
	do {								\
		if (psc_log_getlevel(ss) >= (lvl))			\
			psclogv(__FILE__, __func__, __LINE__, (ss),	\
			    (lvl), (flg), (fmt), (ap));			\
	} while (0)

#define _psclogft(ss, lvl, flg, fmt, ...)				\
	_psc_fatal(__FILE__, __func__, __LINE__, (ss),			\
	    (lvl), (flg), (fmt), ## __VA_ARGS__ )			\

#define psclogvft(ss, lvl, flg, fmt, ...)				\
	_psc_fatalv(__FILE__, __func__, __LINE__, (ss),			\
	    (lvl), (flg), (fmt), (ap))					\

/* Current/default/active subsystem. */
#define psc_fatal(fmt, ...)		_psclogft(PSC_SUBSYS, PLL_FATAL, PLO_ERRNO, (fmt), ## __VA_ARGS__)
#define psc_fatalx(fmt, ...)		_psclogft(PSC_SUBSYS, PLL_FATAL, 0, (fmt), ## __VA_ARGS__)
#define psc_error(fmt, ...)		_psclogck(PSC_SUBSYS, PLL_ERROR, PLO_ERRNO, (fmt), ## __VA_ARGS__)
#define psc_errorx(fmt, ...)		_psclogck(PSC_SUBSYS, PLL_ERROR, 0, (fmt), ## __VA_ARGS__)
#define psc_warn(fmt, ...)		_psclogck(PSC_SUBSYS, PLL_WARN, PLO_ERRNO, (fmt), ## __VA_ARGS__)
#define psc_warnx(fmt, ...)		_psclogck(PSC_SUBSYS, PLL_WARN, 0, (fmt), ## __VA_ARGS__)
#define psc_notice(fmt, ...)		_psclogck(PSC_SUBSYS, PLL_NOTICE, 0, (fmt), ## __VA_ARGS__)
#define psc_notify(fmt, ...)		_psclogck(PSC_SUBSYS, PLL_NOTICE, 0, (fmt), ## __VA_ARGS__)
#define psc_dbg(fmt, ...)		_psclogck(PSC_SUBSYS, PLL_DEBUG, 0, (fmt), ## __VA_ARGS__)
#define psc_info(fmt, ...)		_psclogck(PSC_SUBSYS, PLL_INFO, 0, (fmt), ## __VA_ARGS__)
#define psc_trace(fmt, ...)		_psclogck(PSC_SUBSYS, PLL_TRACE, 0, (fmt), ## __VA_ARGS__)
#define psc_log(lvl, fmt, ...)		_psclogck(PSC_SUBSYS, (lvl), 0, (fmt), ## __VA_ARGS__)

/* Override/specify subsystem. */
#define psc_fatals(ss, fmt, ...)	_psclogft((ss), PLL_FATAL, PLO_ERRNO, (fmt), ## __VA_ARGS__)
#define psc_fatalxs(ss, fmt, ...)	_psclogft((ss), PLL_FATAL, 0, (fmt), ## __VA_ARGS__)
#define psc_errors(ss, fmt, ...)	_psclogck((ss), PLL_ERROR, PLO_ERRNO, (fmt), ## __VA_ARGS__)
#define psc_errorxs(ss, fmt, ...)	_psclogck((ss), PLL_ERROR, 0, (fmt), ## __VA_ARGS__)
#define psc_warns(ss, fmt, ...)		_psclogck((ss), PLL_WARN, PLO_ERRNO, (fmt), ## __VA_ARGS__)
#define psc_warnxs(ss, fmt, ...)	_psclogck((ss), PLL_WARN, 0, (fmt), ## __VA_ARGS__)
#define psc_notices(ss, fmt, ...)	_psclogck((ss), PLL_NOTICE, 0, (fmt), ## __VA_ARGS__)
#define psc_notifys(ss, fmt, ...)	_psclogck((ss), PLL_NOTICE, 0, (fmt), ## __VA_ARGS__)
#define psc_dbgs(ss, fmt, ...)		_psclogck((ss), PLL_DEBUG, 0, (fmt), ## __VA_ARGS__)
#define psc_infos(ss, fmt, ...)		_psclogck((ss), PLL_INFO, 0, (fmt), ## __VA_ARGS__)
#define psc_traces(ss, fmt, ...)	_psclogck((ss), PLL_TRACE, 0, (fmt), ## __VA_ARGS__)
#define psc_logs(lvl, ss, fmt, ...)	_psclogck((ss), (lvl), 0, (fmt), ## __VA_ARGS__)

/* Variable-argument list versions. */
#define psc_fatalv(fmt, ap)		psclogvft(PSC_SUBSYS, PLL_FATAL, PLO_ERRNO, (fmt), ap)
#define psc_fatalxv(fmt, ap)		psclogvft(PSC_SUBSYS, PLL_FATAL, 0, (fmt), ap)
#define psc_errorv(fmt, ap)		psclogvck(PSC_SUBSYS, PLL_ERROR, PLO_ERRNO, (fmt), ap)
#define psc_errorxv(fmt, ap)		psclogvck(PSC_SUBSYS, PLL_ERROR, 0, (fmt), ap)
#define psc_warnv(fmt, ap)		psclogvck(PSC_SUBSYS, PLL_WARN, PLO_ERRNO, (fmt), ap)
#define psc_warnxv(fmt, ap)		psclogvck(PSC_SUBSYS, PLL_WARN, 0, (fmt), ap)
#define psc_noticev(fmt, ap)		psclogvck(PSC_SUBSYS, PLL_NOTICE, 0, (fmt), ap)
#define psc_notifyv(fmt, ap)		psclogvck(PSC_SUBSYS, PLL_NOTICE, 0, (fmt), ap)
#define psc_dbgv(fmt, ap)		psclogvck(PSC_SUBSYS, PLL_DEBUG, 0, (fmt), ap)
#define psc_infov(fmt, ap)		psclogvck(PSC_SUBSYS, PLL_INFO, 0, (fmt), ap)
#define psc_tracev(fmt, ap)		psclogvck(PSC_SUBSYS, PLL_TRACE, 0, (fmt), ap)
#define psc_logv(lvl, fmt, ap)		psclogvck(PSC_SUBSYS, (lvl), 0, (fmt), ap)

/* Variable-argument list versions with subsystem overriding. */
#define psc_fatalsv(fmt, ap)		psclogvft((ss), PLL_FATAL, PLO_ERRNO, (fmt), (ap))
#define psc_fatalxsv(fmt, ap)		psclogvft((ss), PLL_FATAL, 0, (fmt), (ap))
#define psc_errorsv(fmt, ap)		psclogvck((ss), PLL_ERROR, PLO_ERRNO, (fmt), (ap))
#define psc_errorxsv(fmt, ap)		psclogvck((ss), PLL_ERROR, 0, (fmt), (ap))
#define psc_warnsv(fmt, ap)		psclogvck((ss), PLL_WARN, PLO_ERRNO, (fmt), (ap))
#define psc_warnxsv(fmt, ap)		psclogvck((ss), PLL_WARN, 0, (fmt), (ap))
#define psc_noticesv(fmt, ap)		psclogvck((ss), PLL_NOTICE, 0, (fmt), (ap))
#define psc_notifysv(fmt, ap)		psclogvck((ss), PLL_NOTICE, 0, (fmt), (ap))
#define psc_dbgsv(fmt, ap)		psclogvck((ss), PLL_DEBUG, 0, (fmt), (ap))
#define psc_infosv(fmt, ap)		psclogvck((ss), PLL_INFO, 0, (fmt), (ap))
#define psc_tracesv(fmt, ap)		psclogvck((ss), PLL_TRACE, 0, (fmt), (ap))
#define psc_logsv(lvl, ss, fmt, ap)	psclogvck((ss), (lvl), 0, (fmt), (ap))

#define ENTRY_MARKER			psc_trace("entry_marker")
#define EXIT_MARKER			psc_trace("exit_marker")

#define RETURN_MARKER(v)					\
	do {							\
		psc_trace("exit_marker");			\
		return v;					\
	} while (0)

void	psc_log_init(void);
void	psc_log_setlevel(int, int);
int	psc_log_getlevel(int);
int	psc_log_getlevel_global(void);
int	psc_log_getlevel_ss(int);

const char *
	psc_loglevel_getname(int);
int	psc_loglevel_getid(const char *);

void psclogv(const char *, const char *, int, int, int, int, const char *,
    va_list);

void _psclog(const char *, const char *, int, int, int, int, const char *,
    ...)
    __attribute__((__format__(__printf__, 7, 8)))
    __attribute__((nonnull(7, 7)));

__dead void _psc_fatalv(const char *, const char *, int, int, int, int,
    const char *, va_list);

__dead void _psc_fatal(const char *, const char *, int, int, int, int,
    const char *, ...)
    __attribute__((__format__(__printf__, 7, 8)))
    __attribute__((nonnull(7, 7)));

#ifdef CDEBUG
# undef CDEBUG
#endif
#define CDEBUG(mask, fmt, ...)						\
	do {								\
		switch (mask) {						\
		case D_ERROR:						\
		case D_NETERROR:					\
			psc_errorx((fmt), ## __VA_ARGS__);		\
			break;						\
		case D_WARNING:						\
			psc_warnx((fmt), ## __VA_ARGS__);		\
			break;						\
		case D_NET:						\
		case D_INFO:						\
		case D_CONFIG:						\
			psc_info((fmt), ## __VA_ARGS__);		\
			break;						\
		case D_RPCTRACE:					\
		case D_TRACE:						\
			psc_trace((fmt), ## __VA_ARGS__);		\
			break;						\
		default:						\
			psc_warnx("Unknown lustre mask %d", (mask));	\
			psc_warnx((fmt), ## __VA_ARGS__);		\
			break;						\
		}							\
	} while (0)
