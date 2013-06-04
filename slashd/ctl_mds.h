/* $Id$ */
/*
 * %PSCGPL_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2006-2013, Pittsburgh Supercomputing Center (PSC).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed WITHOUT ANY WARRANTY; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License contained in the file
 * `COPYING-GPL' at the top of this distribution or at
 * https://www.gnu.org/licenses/gpl-2.0.html for more details.
 *
 * Pittsburgh Supercomputing Center	phone: 412.268.4960  fax: 412.268.5832
 * 300 S. Craig Street			e-mail: remarks@psc.edu
 * Pittsburgh, PA 15213			web: http://www.psc.edu/
 * -----------------------------------------------------------------------------
 * %PSC_END_COPYRIGHT%
 */

/*
 * Interface for controlling live operation of slashd.
 */

#include "slashrpc.h"

struct slmctlmsg_replpair {
	char			scrp_addrbuf[2][RESM_ADDRBUF_SZ];
	uint64_t		scrp_avail;
	uint64_t		scrp_used;
};

struct slmctlmsg_statfs {
	char			scsf_resname[RES_NAME_MAX];
	int32_t			scsf_flags;
	struct srt_statfs	scsf_ssfb;
};

struct slmctlmsg_bml {
	struct slash_fidgen	scbl_fg;
	uint32_t		scbl_bno;
	uint32_t		scbl_flags;
	uint32_t		scbl_ndups;
	 int32_t		_scbl_pad;

	uint64_t		scbl_seq;
	uint64_t		scbl_key;
	uint64_t		scbl_start;	/* time_t */
	uint64_t		scbl_expire;	/* time_t */
	char			scbl_resname[RES_NAME_MAX];
	char			scbl_client[PSCRPC_NIDSTR_SIZE];
};

/* slrmcthr stats */
#define pcst_nopen		pcst_u32_1
#define pcst_nstat		pcst_u32_2
#define pcst_nclose		pcst_u32_3

/* slashd message types */
#define SLMCMT_GETBMAP		(NPCMT + 0)
#define SLMCMT_GETCONNS		(NPCMT + 1)
#define SLMCMT_GETFCMHS		(NPCMT + 2)
#define SLMCMT_GETREPLPAIRS	(NPCMT + 3)
#define SLMCMT_GETSTATFS	(NPCMT + 4)
#define SLMCMT_STOP		(NPCMT + 5)
#define SLMCMT_GETBML		(NPCMT + 6)
