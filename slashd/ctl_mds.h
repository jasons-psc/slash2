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

/*
 * Interface for controlling live operation of a slashd instance.
 */

/* slrmcthr stats */
#define pcst_nopen		pcst_u32_1
#define pcst_nstat		pcst_u32_2
#define pcst_nclose		pcst_u32_3

/* sliod message types */
#define SLMCMT_GETCONNS		NPCMT
#define SLMCMT_GETFCMH		(NPCMT + 1)

/* slashd control commands */
#define SMCC_EXIT		0
#define SMCC_RECONFIG		1
