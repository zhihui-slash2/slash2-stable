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

#include <sys/types.h>
#include <sys/syscall.h>

#include <err.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pfl/alloc.h"
#include "pfl/atomic.h"
#include "pfl/cdefs.h"
#include "pfl/lock.h"
#include "pfl/log.h"
#include "pfl/pfl.h"
#include "pfl/thread.h"
#include "pfl/time.h"

psc_spinlock_t				  psc_umask_lock = SPINLOCK_INIT;
__threadx const struct pfl_callerinfo	*_pfl_callerinfo;
__threadx int				 _pfl_callerinfo_lvl;
__static void				*_pfl_tls[PFL_TLSIDX_MAX];
struct timespec				  pfl_uptime;
pid_t					  pfl_pid;

pid_t
pfl_getsysthrid(void)
{
#if defined(SYS_thread_selfid)
	return (syscall(SYS_thread_selfid));
#elif defined(SYS_gettid)
	return (syscall(SYS_gettid));
#elif defined(SYS_getthrid)
	return (syscall(SYS_getthrid));
#elif defined(SYS_thr_self)
	return (syscall(SYS_thr_self));
#elif defined(HAVE_LIBPTHREAD)
	return (pthread_self());
#else
	return (getpid());
#endif
}

struct pfl_atexit_func {
	void			(*aef_func)(void);
	struct psc_listentry	  aef_lentry;
};
struct psc_lockedlist pfl_atexit_funcs = PLL_INIT(&pfl_atexit_funcs,
    struct pfl_atexit_func, aef_lentry);

void
pfl_atexit(void (*f)(void))
{
	struct pfl_atexit_func *aef;

	aef = PSCALLOC(sizeof(*aef));
	aef->aef_func = f;
	INIT_PSC_LISTENTRY(&aef->aef_lentry);
	pll_add(&pfl_atexit_funcs, aef);
	atexit(f);
}

__static void
pfl_run_atexit(void)
{
	struct pfl_atexit_func *aef;

	aef = PSCALLOC(sizeof(*aef));
	PLL_FOREACH(aef, &pfl_atexit_funcs)
		aef->aef_func();
}

void
pfl_abort(void)
{
	pfl_run_atexit();
	abort();
}

void
pfl_dump_stack(void)
{
	char buf[BUFSIZ];

	snprintf(buf, sizeof(buf),
	    "{ pstack %d 2>/dev/null || gstack %d 2>/dev/null; "
	    "  pstack %d 2>/dev/null || gstack %d 2>/dev/null; } | "
	    "{ tools/filter-pstack - 2>/dev/null; cat -; }",
	    pfl_getsysthrid(), pfl_getsysthrid(), getpid(), getpid());
	if (system(buf) == -1)
		warn("%s", buf);
}

void
pfl_dump_stack1(int sig)
{
	static psc_spinlock_t lock = SPINLOCK_INIT_NOLOG;

//	if (!trylock(&lock)) {
//		write(STDERR_FILENO, );
//		_exit(1);
//	}

	spinlock(&lock);
	fflush(stderr);
	printf("\n\n");
	if (sig)
		printf("signal %d received, ", sig);
	printf("attempting to generate stack trace...\n");
	pfl_dump_stack();
	pfl_abort();
}

void
pfl_init(void)
{
	static psc_atomic32_t init = PSC_ATOMIC32_INIT(0);
	struct sigaction sa;
	char *p;

	if (psc_atomic32_xchg(&init, 1))
		errx(1, "pfl_init: already initialized");

	pfl_pid = getpid();

#ifdef HAVE_LIBPTHREAD
	pscthrs_init();
#endif
	psc_log_init();
#ifdef HAVE_LIBPTHREAD
	void psc_memnode_init(void);

	psc_memnode_init();
#endif

	psc_pagesize = sysconf(_SC_PAGESIZE);
	if (psc_pagesize == -1)
		psc_fatal("sysconf");

	psc_memallocs_init();

	psc_subsys_register(PSS_DEF, "def");
	psc_subsys_register(PSS_TMP, "tmp");
	psc_subsys_register(PSS_MEM, "mem");
	psc_subsys_register(PSS_LNET, "lnet");
	psc_subsys_register(PSS_RPC, "rpc");

	p = getenv("PSC_DUMPSTACK");
	if (p && strcmp(p, "0") &&
	    signal(SIGSEGV, pfl_dump_stack1) == SIG_ERR)
		psc_fatal("signal");
	p = getenv("PSC_FORCE_DUMPSTACK");
	if (p && strcmp(p, "0"))
		atexit(pfl_dump_stack);

	p = getenv("PSC_TIMEOUT");
	if (p) {
		struct itimerval it;
		long l;

		memset(&it, 0, sizeof(it));
		l = strtol(p, NULL, 10);
		it.it_value.tv_sec += l;
		if (setitimer(ITIMER_REAL, &it, NULL) == -1)
			psclog_error("setitimer");
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	if (sigaction(SIGPIPE, &sa, NULL) == -1)
		psc_fatal("sigaction");

	_PFL_GETTIMESPEC(CLOCK_MONOTONIC, &pfl_uptime);
}
