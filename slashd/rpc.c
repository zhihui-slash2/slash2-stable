/* $Id$ */

#include <stdio.h>

#include "psc_ds/tree.h"
#include "psc_rpc/rpc.h"

#include "rpc.h"
#include "slashrpc.h"

struct sexptree sexptree;
psc_spinlock_t sexptreelock = LOCK_INITIALIZER;

SPLAY_GENERATE(sexptree, slashrpc_export, entry, sexpcmp);

/*
 * slashrpc_export_get - access our application-specific variables associated
 *	with an LNET connection.
 * @exp: RPC export of peer.
 */
struct slashrpc_export *
slashrpc_export_get(struct pscrpc_export *exp)
{
	spinlock(&exp->exp_lock);
	if (exp->exp_private == NULL) {
		exp->exp_private = PSCALLOC(sizeof(struct slashrpc_export));
		exp->exp_destroycb = slashrpc_export_destroy;

		spinlock(&sexptreelock);
		if (SPLAY_INSERT(sexptree, &sexptree, exp->exp_private))
			psc_fatalx("export already registered");
		freelock(&sexptreelock);
	}
	freelock(&exp->exp_lock);
	return (exp->exp_private);
}

void
slashrpc_export_destroy(void *data)
{
	struct slashrpc_export *sexp = data;
	struct cfdent *c, *next;

	for (c = SPLAY_MIN(cfdtree, &sexp->cfdtree); c; c = next) {
		next = SPLAY_NEXT(cfdtree, &sexp->cfdtree, c);
		SPLAY_REMOVE(cfdtree, &sexp->cfdtree, c);
		free(c);
	}
	spinlock(&sexptreelock);
	SPLAY_REMOVE(sexptree, &sexptree, sexp);
	freelock(&sexptreelock);
	free(sexp);
}

int
sexpcmp(const void *a, const void *b)
{
	const struct slashrpc_export *sa = a, *sb = b;
	const lnet_process_id_t *pa = &sa->exp->exp_connection->c_peer;
	const lnet_process_id_t *pb = &sb->exp->exp_connection->c_peer;

	if (pa->nid < pb->nid)
		return (-1);
	else if (pa->nid > pb->nid)
		return (1);

	if (pa->pid < pb->pid)
		return (-1);
	else if (pa->pid > pb->pid)
		return (1);

	return (0);
}
