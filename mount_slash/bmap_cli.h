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

#ifndef _SLASH_BMAP_CLI_H_
#define _SLASH_BMAP_CLI_H_

#include "pfl/rpc.h"
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
#define BMAP_CLI_LEASEEXTREQ	(_BMAP_FLSHFT << 0)	/* requesting a lease ext */
#define BMAP_CLI_REASSIGNREQ	(_BMAP_FLSHFT << 1)
#define BMAP_CLI_LEASEFAILED	(_BMAP_FLSHFT << 2)	/* lease request has failed */
#define BMAP_CLI_LEASEEXPIRED	(_BMAP_FLSHFT << 3)	/* lease has expired, new one is needed */

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
