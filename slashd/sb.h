/* $Id$ */

#include "psc_util/journal.h"

struct slash_sb_mem {
	int			sbm_inum;	/* next inum to assign */
	struct psc_journal	sbm_pj;
};
