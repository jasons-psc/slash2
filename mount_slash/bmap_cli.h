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

#ifndef _SLASH_CLI_BMAP_H_
#define _SLASH_CLI_BMAP_H_

#include "psc_rpc/rpc.h"
#include "psc_util/lock.h"

#include "bmap.h"
#include "bmpc.h"
#include "inode.h"
#include "slashrpc.h"

/* number of bmap flush threads */
#define NUM_BMAP_FLUSH_THREADS		1

/*
 * bmap_cli_data - private data associated with a bmap used by a SLASH2 client
 */
struct bmap_cli_info {
	struct bmap_pagecache	 bci_bmpc;
	struct srt_bmapdesc	 bci_sbd;		/* open bmap descriptor */
	struct timespec		 bci_xtime;		/* max time */
	struct timespec		 bci_etime;		/* current expire time */
};

/* mount_slash specific bcm_flags */
#define BMAP_CLI_FLUSHPROC	(_BMAP_FLSHFT << 0)	/* proc'd by flush thr */

#define BMAP_CLI_MAX_LEASE	60 /* seconds */
#define BMAP_CLI_TIMEO_INC	5
#define BMAP_CLI_DIOWAIT_SECS	1

#define bmap_2_bci(b)		((struct bmap_cli_info *)bmap_get_pri(b))
#define bmap_2_bci_const(b)	((const struct bmap_cli_info *)bmap_get_pri_const(b))
#define bmap_2_bmpc(b)		(&bmap_2_bci(b)->bci_bmpc)
#define bmap_2_sbd(b)		(&bmap_2_bci(b)->bci_sbd)
#define bmap_2_ion(b)		bmap_2_sbd(b)->sbd_ion_nid

#define BMAP_CLI_BUMP_TIMEO(b)						\
	do {								\
		struct timespec _ctime;					\
									\
		PFL_GETTIMESPEC(&_ctime);				\
		BMAP_LOCK(b);						\
		timespecadd(&_ctime, &msl_bmap_timeo_inc,		\
		    &bmap_2_bci(b)->bci_etime);				\
		if (timespeccmp(&bmap_2_bci(b)->bci_etime,		\
		    &bmap_2_bci(b)->bci_xtime, >))			\
			memcpy(&bmap_2_bci(b)->bci_etime,		\
			    &bmap_2_bci(b)->bci_xtime,			\
			    sizeof(struct timespec));			\
		BMAP_ULOCK(b);						\
	} while (0)

void     msl_bmap_cache_rls(struct bmapc_memb *);

extern struct timespec msl_bmap_max_lease;
extern struct timespec msl_bmap_timeo_inc;

static __inline struct bmapc_memb *
bci_2_bmap(struct bmap_cli_info *bci)
{
	struct bmapc_memb *bcm;

	psc_assert(bci);
	bcm = (void *)bci;
	return (bcm - 1);
}

static __inline int
bmap_cli_timeo_cmp(const void *x, const void *y)
{
	const struct bmap_cli_info * const *pa = x, *a = *pa;
	const struct bmap_cli_info * const *pb = y, *b = *pb;

	if (timespeccmp(&a->bci_etime, &b->bci_etime, <))
		return (-1);

	if (timespeccmp(&a->bci_etime, &b->bci_etime, >))
		return (1);

	return (0);
}

#endif /* _SLASH_CLI_BMAP_H_ */
