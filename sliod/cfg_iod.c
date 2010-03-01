/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2006-2010, Pittsburgh Supercomputing Center (PSC).
 *
 * Permission to use, copy, and modify this software and its documentation
 * without fee for personal use or non-commercial use within your organization
 * is hereby granted, provided that the above copyright notice is preserved in
 * all copies and that the copyright and this permission notice appear in
 * supporting documentation.  Permission to redistribute this software to other
 * organizations or individuals is not permitted without the written permission
 * of the Pittsburgh Supercomputing Center.  PSC makes no representations about
 * the suitability of this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 * -----------------------------------------------------------------------------
 * %PSC_END_COPYRIGHT%
 */

#include "pfl/cdefs.h"
#include "psc_util/alloc.h"

#include "slconfig.h"
#include "sliod.h"

void
slcfg_init_res(__unusedx struct sl_resource *res)
{
}

void
slcfg_init_resm(struct sl_resm *resm)
{
	struct resm_iod_info *rmii;

	rmii = resm->resm_pri = PSCALLOC(sizeof(*rmii));
	LOCK_INIT(&rmii->rmii_lock);
	psc_waitq_init(&rmii->rmii_waitq);
}

void
slcfg_init_site(__unusedx struct sl_site *site)
{
}
