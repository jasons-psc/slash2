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

#ifndef _FIDC_CLI_H_
#define _FIDC_CLI_H_

#include "psc_ds/list.h"
#include "psc_util/lock.h"

#include "sltypes.h"
#include "fidcache.h"
#include "dircache.h"

#define MSLFS_ENTRY_TIMEO	8.0
#define MSLFS_ATTR_TIMEO	8.0

struct fidc_membh;

struct cli_finfo {
	int			 nrepls;
	sl_replica_t		 reptbl[SL_MAX_REPLICAS];
};

struct fcmh_cli_info {
	struct timeval		 fci_age;
	union {
		struct cli_finfo	f;
		struct dircache_info	d;
	} ford;
#define fci_nrepls	ford.f.nrepls
#define fci_reptbl	ford.f.reptbl
#define fci_dci		ford.d
};

#define fcmh_2_fci(f)		((struct fcmh_cli_info *)fcmh_get_pri(f))

/* Client-specific fcmh_flags */
#define FCMH_CLI_HAVEREPLTBL	(_FCMH_FLGSHFT << 0)	/* file replica table present */
#define FCMH_CLI_FETCHREPLTBL	(_FCMH_FLGSHFT << 1)	/* file replica table loading */
#define FCMH_CLI_APPENDWR	(_FCMH_FLGSHFT << 2)	/* file opened with O_APPEND */
#define FCMH_CLI_INITDCI	(_FCMH_FLGSHFT << 3)	/* dircache initialized */
#define FCMH_CLI_TRUNC		(_FCMH_FLGSHFT << 4)	/* truncate in progress */

void	fcmh_setlocalsize(struct fidc_membh *, uint64_t);
void	slc_fcmh_initdci(struct fidc_membh *);

#define fidc_lookup_load_inode(fid, fcmhp)				\
	_fidc_lookup_load_inode(PFL_CALLERINFOSS(SLSS_FCMH), (fid),	\
	    (fcmhp))

/**
 * fidc_lookup_load_inode - Create the inode if it doesn't exist, loading
 *	its attributes from the MDS.
 */
static __inline int
_fidc_lookup_load_inode(const struct pfl_callerinfo *pci, slfid_t fid,
    struct fidc_membh **fcmhp)
{
	struct slash_fidgen fg = { fid, FGEN_ANY };

	return (_fidc_lookup(pci, &fg, FIDC_LOOKUP_CREATE | FIDC_LOOKUP_LOAD,
	    NULL, 0, fcmhp));
}

#endif /* _FIDC_CLI_H_ */
