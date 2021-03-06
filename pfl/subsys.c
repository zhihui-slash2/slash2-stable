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
 * Subsystem definitions.
 */

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <strings.h>
#include <syslog.h>

#include "pfl/alloc.h"
#include "pfl/dynarray.h"
#include "pfl/fmtstr.h"
#include "pfl/log.h"
#include "pfl/subsys.h"

struct psc_subsys {
	const char	*pss_name;
	int		 pss_loglevel;
};

extern int		*pfl_syslog;

struct psc_dynarray	 psc_subsystems = DYNARRAY_INIT_NOLOG;

int
psc_subsys_id(const char *name)
{
	const struct psc_subsys **ss;
	int n, len;

	ss = psc_dynarray_get(&psc_subsystems);
	len = psc_dynarray_len(&psc_subsystems);
	for (n = 0; n < len; n++)
		if (strcasecmp(name, ss[n]->pss_name) == 0)
			return (n);
	return (-1);
}

const char *
psc_subsys_name(int ssid)
{
	const struct psc_subsys *ss;

	if (ssid < 0 || ssid >= psc_dynarray_len(&psc_subsystems))
		return ("<unknown>");
	ss = psc_dynarray_getpos(&psc_subsystems, ssid);
	return (ss->pss_name);
}

void
psc_subsys_register(int ssid, const char *name)
{
	struct psc_subsys *ss;
	char *p, buf[BUFSIZ];
	int nss;

	nss = psc_dynarray_len(&psc_subsystems);
	ss = psc_alloc(sizeof(*ss), PAF_NOLOG);
	ss->pss_name = name;

	snprintf(buf, sizeof(buf), "PSC_LOG_LEVEL_%s", name);
	p = getenv(buf);
	if (p) {
		ss->pss_loglevel = psc_loglevel_fromstr(p);
		if (ss->pss_loglevel == PNLOGLEVELS)
			psc_fatalx("invalid %s value", name);
	} else {
		ss->pss_loglevel = psc_log_getlevel_global();
		if (ssid == PSS_TMP)
			ss->pss_loglevel = PLL_DEBUG;
	}

	snprintf(buf, sizeof(buf), "PSC_SYSLOG_%s", name);
	if (getenv(buf) || getenv("PSC_SYSLOG")) {
		static int init;

		if (!init) {
			extern const char *__progname;
			const char *ident = __progname;

			init = 1;
			p = getenv("PFL_SYSLOG_IDENT"); 
			if (p) {
				static char idbuf[32];

				ident = idbuf;
				(void)FMTSTR(idbuf, sizeof(idbuf), p,
				    FMTSTRCASE('n', "s", __progname)
				);
			}
			openlog(ident, LOG_CONS | LOG_NDELAY | LOG_PID,
			    LOG_DAEMON);
		}

		pfl_syslog = psc_realloc(pfl_syslog,
		    sizeof(*pfl_syslog) * (nss + 1), PAF_NOLOG);
		pfl_syslog[nss] = 1;
	}

	if (ssid != nss)
		psc_fatalx("bad ID %d for subsys %s [want %d], "
		    "check order", ssid, name, nss);
	psc_dynarray_add(&psc_subsystems, ss);
}

int
psc_log_getlevel_ss(int ssid)
{
	const struct psc_subsys *ss;

	if (ssid >= psc_dynarray_len(&psc_subsystems) || ssid < 0) {
		/* don't use psclog to avoid loops */
		warnx("subsystem out of bounds (%d, max %d)", ssid,
		    psc_dynarray_len(&psc_subsystems));
		abort();
	}
	ss = psc_dynarray_getpos(&psc_subsystems, ssid);
	return (ss->pss_loglevel);
}

void
psc_log_setlevel_ss(int ssid, int newlevel)
{
	struct psc_subsys **ss;
	int i, nss;

	if (newlevel >= PNLOGLEVELS || newlevel < 0)
		psc_fatalx("log level out of bounds (%d, max %d)",
		    newlevel, PNLOGLEVELS);

	ss = psc_dynarray_get(&psc_subsystems);
	nss = psc_dynarray_len(&psc_subsystems);

	if (ssid == PSS_ALL)
		for (i = 0; i < nss; i++)
			ss[i]->pss_loglevel = newlevel;
	else if (ssid >= nss || ssid < 0)
		psc_fatalx("subsystem out of bounds (%d, max %d)", ssid,
		    nss);
	else
		ss[ssid]->pss_loglevel = newlevel;
}
