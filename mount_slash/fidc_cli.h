/* $Id$ */
/*
 * %PSCGPL_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2007-2013, Pittsburgh Supercomputing Center (PSC).
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

#ifndef _FIDC_CLI_H_
#define _FIDC_CLI_H_

#include "pfl/list.h"
#include "psc_util/lock.h"

#include "sltypes.h"
#include "fidcache.h"
#include "dircache.h"

struct pscfs_clientctx;

struct fidc_membh;

struct cli_finfo {
	int			 nrepls;
	sl_replica_t		 reptbl[SL_MAX_REPLICAS];
};

struct fcmh_cli_info {
	struct sl_resm		*fci_resm;
	struct timeval		 fci_age;
	union {
		struct cli_finfo	f;
		struct dircache_info	d;
	} ford;
#define fci_nrepls	ford.f.nrepls
#define fci_reptbl	ford.f.reptbl
#define fci_dci		ford.d
	struct psclist_head	 fci_lentry;	/* all fcmhs with dirty attributes */
	struct timespec		 fci_etime;	/* attr expire time */
};

static __inline struct fcmh_cli_info *
fcmh_2_fci(struct fidc_membh *f)
{
	return (fcmh_get_pri(f));
}

static __inline struct fidc_membh *
fci_2_fcmh(struct fcmh_cli_info *fci)
{
	struct fidc_membh *fcmh;

	psc_assert(fci);
	fcmh = (void *)fci;
	return (fcmh - 1);
}

#define fcmh_2_dci(f)		(&fcmh_2_fci(f)->fci_dci)

/* Client-specific fcmh_flags */
#define FCMH_CLI_HAVEREPLTBL	(_FCMH_FLGSHFT << 0)	/* file replica table present */
#define FCMH_CLI_FETCHREPLTBL	(_FCMH_FLGSHFT << 1)	/* file replica table loading */
#define FCMH_CLI_INITDCI	(_FCMH_FLGSHFT << 2)	/* dircache initialized */
#define FCMH_CLI_TRUNC		(_FCMH_FLGSHFT << 3)	/* truncate in progress */
#define FCMH_CLI_DIRTY_ATTRS	(_FCMH_FLGSHFT << 4)	/* has dirty attributes */
#define FCMH_CLI_DIRTY_QUEUE	(_FCMH_FLGSHFT << 5)	/* on dirty queue */

void	slc_fcmh_initdci(struct fidc_membh *);

int	fcmh_checkcreds(struct fidc_membh *, const struct pscfs_creds *, int);

#define fidc_lookup_load_inode(fid, fcmhp, pfcc)			\
	_fidc_lookup_load_inode(PFL_CALLERINFOSS(SLSS_FCMH), (fid),	\
	    (fcmhp), (pfcc))

/**
 * fidc_lookup_load_inode - Create the inode if it doesn't exist,
 *	loading its attributes from the MDS.
 */
static __inline int
_fidc_lookup_load_inode(const struct pfl_callerinfo *pci, slfid_t fid,
    struct fidc_membh **fcmhp, struct pscfs_clientctx *pfcc)
{
	struct slash_fidgen fg = { fid, FGEN_ANY };

	return (_fidc_lookup(pci, &fg, FIDC_LOOKUP_CREATE |
	    FIDC_LOOKUP_LOAD, NULL, 0, fcmhp, pfcc));
}

#endif /* _FIDC_CLI_H_ */
