/* $Id$ */

/*
 * Subsystem definitions.
 */
#include "psc_util/subsys.h"
#include "psc_ds/dynarray.h"
#include "psc_util/log.h"

#include <stdio.h>
#include <strings.h>

struct dynarray psc_subsystems;

int
psc_subsys_id(const char *name)
{
	const char **ss;
	int n, len;

	ss = dynarray_get(&psc_subsystems);
	len = dynarray_len(&psc_subsystems);
	for (n = 0; n < len; n++)
		if (strcasecmp(name, ss[n]) == 0)
			return (n);
	return (-1);
}

const char *
psc_subsys_name(int id)
{
	const char **ss;
	int len;

	if (id < 0)
		return ("<unknown>");

	ss = dynarray_get(&psc_subsystems);
	len = dynarray_len(&psc_subsystems);

	if (id >= len)
		return ("<unknown>");
	if (ss[id] == NULL)
		return ("<unreg>");
	return (ss[id]);
}

void
psc_subsys_register(int id, const char *name)
{
	const char **ss;

	dynarray_hintlen(&psc_subsystems, id);
	ss = dynarray_get(&psc_subsystems);

	if (ss[id])
		psc_fatalx("subsys slot %d already registered", id);
	ss[id] = name;
}
