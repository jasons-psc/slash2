/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2006-2013, Pittsburgh Supercomputing Center (PSC).
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

#ifndef _SLASH_BMAP_CLI_H_
#define _SLASH_BMAP_CLI_H_

#include "psc_rpc/rpc.h"
#include "psc_util/lock.h"

#include "bmap.h"
#include "pgcache.h"
#include "slashrpc.h"

/* number of bmap flush threads */
/* XXX I don't think bmap_flush is thread safe, so keep this at '1'
 * - Paul
 */
#define NUM_BMAP_FLUSH_THREADS		1

/**
 * bmap_cli_data - private data associated with a bmap used by a SLASH2 client
 */
struct bmap_cli_info {
	struct bmap_pagecache	 bci_bmpc;
	struct srt_bmapdesc	 bci_sbd;		/* open bmap descriptor */
	struct timespec		 bci_xtime;		/* max time */
	struct timespec		 bci_etime;		/* current expire time */
	int			 bci_error;		/* lease request error */
	int			 bci_nreassigns;	/* number of reassigns */
	sl_ios_id_t		 bci_prev_sliods[SL_MAX_IOSREASSIGN];
	struct psc_listentry	 bci_lentry;		/* bmap flushq */
};

/* mount_slash specific bcm_flags */
#define BMAP_CLI_AVAILABLE	(_BMAP_FLSHFT << 0)	/* not used */
#define BMAP_CLI_BIORQEXPIRE	(_BMAP_FLSHFT << 1)
#define BMAP_CLI_LEASEEXTREQ	(_BMAP_FLSHFT << 2)	/* requesting a lease ext */
#define BMAP_CLI_REASSIGNREQ	(_BMAP_FLSHFT << 3)
#define BMAP_CLI_DIOWR		(_BMAP_FLSHFT << 4)	/* dio for archiver write */
#define BMAP_CLI_LEASEEXPIRED	(_BMAP_FLSHFT << 5)	/* lease has expired, bmap must be released */

#define BMAP_CLI_MAX_LEASE	60 /* seconds */
#define BMAP_CLI_EXTREQSECS	20
#define BMAP_CLI_EXTREQSECSBLOCK (BMAP_CLI_EXTREQSECS/2)
#define BMAP_CLI_TIMEO_INC	1

static __inline struct bmap_cli_info *
bmap_2_bci(struct bmapc_memb *b)
{
	return (bmap_get_pri(b));
}

#define bmap_2_bci_const(b)	((const struct bmap_cli_info *)bmap_get_pri_const(b))

#define bmap_2_bmpc(b)		(&bmap_2_bci(b)->bci_bmpc)

#define bmap_2_sbd(b)		(&bmap_2_bci(b)->bci_sbd)
#define bmap_2_ios(b)		bmap_2_sbd(b)->sbd_ios

void	 msl_bmap_cache_rls(struct bmapc_memb *);
int	 msl_bmap_lease_secs_remaining(struct bmapc_memb *);
int	 msl_bmap_lease_tryext(struct bmapc_memb *, int *, int);
void	 msl_bmap_lease_tryreassign(struct bmapc_memb *);
int	 msl_bmap_lease_secs_remaining(struct bmapc_memb *);

void	 bmap_biorq_expire(struct bmapc_memb *);

extern struct timespec msl_bmap_max_lease;
extern struct timespec msl_bmap_timeo_inc;

static __inline struct bmapc_memb *
bci_2_bmap(struct bmap_cli_info *bci)
{
	struct bmapc_memb *b;

	psc_assert(bci);
	b = (void *)bci;
	return (b - 1);
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

#endif /* _SLASH_BMAP_CLI_H_ */
