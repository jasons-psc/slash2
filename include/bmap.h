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
 * The bmap (block map) interface divides the space of a file in a SLASH
 * network into manageable units.  bmaps are ordered sequentially from
 * the beginning of the file space and are the fundamental elements of
 * I/O node file region ownership and in replication management.
 *
 * bmaps store information such as the state on each replicated instance
 * and are themselves subdivided into slivers which track cyclic redundancy
 * checksums for integrity and such.
 */

#ifndef _BMAP_H_
#define _BMAP_H_

#include <inttypes.h>
#include <time.h>

#include "pfl/tree.h"
#include "pfl/list.h"
#include "psc_util/atomic.h"
#include "psc_util/crc.h"
#include "psc_util/lock.h"

#include "cache_params.h"
#include "fid.h"
#include "slsubsys.h"
#include "sltypes.h"

struct fidc_membh;
struct srt_bmapdesc;

/**
 * bmap_core_state - Basic information needed by all nodes.
 * @bcs_crcstates: bits describing the state of each sliver.
 * @bcs_repls: bitmap used for tracking the replication status of this
 *	bmap.
 *
 * This structure must be 64-bit aligned and padded.
 */
struct bmap_core_state {
	uint8_t			bcs_crcstates[SLASH_CRCS_PER_BMAP];
	uint8_t			bcs_repls[SL_REPLICA_NBYTES];
};

#define BMAP_SEQLOG_FACTOR	100

/**
 * bmap_extra_state - Additional fields needed by MDS.
 * @bes_crcs: the CRC table, one 8-byte CRC per sliver.
 * @bes_gen: current generation number.
 * @bes_replpol: replication policy.
 *
 * This structure must be 64-bit aligned and padded.
 */
struct bmap_extra_state {
	uint64_t		bes_crcs[SLASH_CRCS_PER_BMAP];
	sl_bmapgen_t		bes_gen;
	uint32_t		bes_replpol;
};

/**
 * bmap_ondisk - Bmap over-wire/on-disk structure.  This structure maps
 *	the persistent state of the bmap within the inode's metafile.
 *	This structure is followed by a 64-bit CRC on disk.
 */
struct bmap_ondisk {
	struct bmap_core_state	bod_corestate;
	struct bmap_extra_state	bod_extrastate;
#define bod_repls	bod_corestate.bcs_repls
#define bod_crcstates	bod_corestate.bcs_crcstates
#define bod_crcs	bod_extrastate.bes_crcs
#define bod_replpol	bod_extrastate.bes_replpol
#define bod_gen		bod_extrastate.bes_gen
};

/**
 * bmap - Central structure for block map caching used in all SLASH2
 *	service contexts (mds, ios, client).  The pool for this
 *	structure and its private area for each service is initialized
 *	in bmap_cache_init().
 *
 * bmap sits in the middle of the GFC stratum.
 * XXX some of these elements may need to be moved into the bcm_info_pri
 *     area (as part of new structures?) so save space on the mds.
 */
struct bmap {
	sl_bmapno_t		 bcm_bmapno;	/* bmap index number */
	uint32_t		 bcm_flags;	/* see BMAP_* below */
	struct fidc_membh	*bcm_fcmh;	/* pointer to fid info */
	psc_atomic32_t		 bcm_opcnt;	/* pending opcnt (# refs) */
	psc_spinlock_t		 bcm_lock;
	SPLAY_ENTRY(bmap)	 bcm_tentry;	/* bmap_cache splay tree entry */
	struct psc_listentry	 bcm_lentry;	/* free pool */
	pthread_t		 bcm_owner;	/* temporary processor */

	/*
	 * This must start on a 64-bit boundary, and must lay at the end
	 * of this structure as the bmap_{mds,iod}_info begin with the
	 * next segment of the bmap_ondisk, which must lay contiguous in
	 * memory for I/O over the network and with ZFS.
	 */
	struct bmap_core_state	 bcm_corestate __attribute__((aligned(8)));

#define bcm_crcstates	bcm_corestate.bcs_crcstates
#define bcm_repls	bcm_corestate.bcs_repls
};

#define bmapc_memb bmap

/* shared bmap_flags */
#define BMAP_RD			(1 <<  0)	/* XXX use enum rw */
#define BMAP_WR			(1 <<  1)	/* XXX use enum rw */
#define BMAP_INIT		(1 <<  2)	/* initializing from disk/network */
#define BMAP_DIO		(1 <<  3)	/* direct I/O, no client caching */
#define BMAP_DIOCB		(1 <<  4)
#define BMAP_TOFREE		(1 <<  5)	/* refcnt dropped to zero, removing */
#define BMAP_DIRTY		(1 <<  6)
#define BMAP_TIMEOQ		(1 <<  7)	/* on timeout queue */
#define BMAP_IONASSIGN		(1 <<  8)	/* has been assigned to an ION for writes */
#define BMAP_MDCHNG		(1 <<  9)	/* op mode changing (e.g. READ -> WRITE) */
#define BMAP_WAITERS		(1 << 10)	/* has bcm_fcmh waiters */
#define BMAP_BUSY		(1 << 11)	/* temporary processing lock */
#define BMAP_NEW		(1 << 12)	/* just created */
#define _BMAP_FLSHFT		(1 << 13)

#define bmap_2_fid(b)		fcmh_2_fid((b)->bcm_fcmh)

#define SL_MAX_IOSREASSIGN	16
#define SL_MAX_BMAPFLSH_RETRIES	2048

#define BMAP_LOCK_ENSURE(b)	LOCK_ENSURE(&(b)->bcm_lock)
#define BMAP_HASLOCK(b)		psc_spin_haslock(&(b)->bcm_lock)
#define BMAP_LOCK(b)		spinlock(&(b)->bcm_lock)
#define BMAP_ULOCK(b)		freelock(&(b)->bcm_lock)
#define BMAP_RLOCK(b)		reqlock(&(b)->bcm_lock)
#define BMAP_URLOCK(b, lk)	ureqlock(&(b)->bcm_lock, (lk))
#define BMAP_TRYLOCK(b)		trylock(&(b)->bcm_lock)

#define BMAP_SETATTR(b, fl)	SETATTR_LOCKED(&(b)->bcm_lock, &(b)->bcm_flags, (fl))
#define BMAP_CLEARATTR(b, fl)	CLEARATTR_LOCKED(&(b)->bcm_lock, &(b)->bcm_flags, (fl))

#define _DEBUG_BMAP_FMT		"bmap@%p bno:%u flg:%#x:"		\
				"%s%s%s%s%s%s%s%s%s%s%s%s%s%s "		\
				"fid:"SLPRI_FID" opcnt=%d : "

#define _DEBUG_BMAP_FMTARGS(b)						\
	(b), (b)->bcm_bmapno, (b)->bcm_flags,				\
	(b)->bcm_flags & BMAP_RD	? "R" : "",			\
	(b)->bcm_flags & BMAP_WR	? "W" : "",			\
	(b)->bcm_flags & BMAP_INIT	? "I" : "",			\
	(b)->bcm_flags & BMAP_DIO	? "D" : "",			\
	(b)->bcm_flags & BMAP_DIOCB	? "C" : "",			\
	(b)->bcm_flags & BMAP_TOFREE	? "F" : "",			\
	(b)->bcm_flags & BMAP_DIRTY	? "d" : "",			\
	(b)->bcm_flags & BMAP_TIMEOQ	? "T" : "",			\
	(b)->bcm_flags & BMAP_IONASSIGN	? "A" : "",			\
	(b)->bcm_flags & BMAP_MDCHNG	? "G" : "",			\
	(b)->bcm_flags & BMAP_WAITERS	? "w" : "",			\
	(b)->bcm_flags & BMAP_BUSY	? "B" : "",			\
	(b)->bcm_flags & BMAP_NEW	? "N" : "",			\
	(b)->bcm_flags & ~(_BMAP_FLSHFT - 1) ? "+" : "",		\
	(b)->bcm_fcmh ? fcmh_2_fid((b)->bcm_fcmh) : FID_ANY,		\
	psc_atomic32_read(&(b)->bcm_opcnt)

#define DEBUG_BMAP(level, b, fmt, ...)					\
	psclogs((level), SLSS_BMAP, _DEBUG_BMAP_FMT fmt,		\
	    _DEBUG_BMAP_FMTARGS(b), ## __VA_ARGS__)

#define _DEBUG_BMAP(pci, level, b, fmt, ...)				\
	_psclog_pci((pci), (level), 0, _DEBUG_BMAP_FMT fmt,		\
	    _DEBUG_BMAP_FMTARGS(b), ## __VA_ARGS__)

#define bmap_wait_locked(b, cond)					\
	do {								\
		BMAP_LOCK_ENSURE(b);					\
		while (cond) {						\
			(b)->bcm_flags |= BMAP_WAITERS;			\
			psc_waitq_wait(&(b)->bcm_fcmh->fcmh_waitq,	\
			    &(b)->bcm_lock);				\
			BMAP_LOCK(b);					\
		}							\
	} while (0)

#define bmap_wake_locked(b)						\
	do {								\
		BMAP_LOCK_ENSURE(b);					\
		if ((b)->bcm_flags & BMAP_WAITERS) {			\
			psc_waitq_wakeall(&(b)->bcm_fcmh->fcmh_waitq);	\
			(b)->bcm_flags &= ~BMAP_WAITERS;		\
		}							\
	} while (0)

#define BMAP_WAIT_BUSY(b)						\
	do {								\
		pthread_t _pthr = pthread_self();			\
									\
		(void)BMAP_RLOCK(b);					\
		bmap_wait_locked((b),					\
		    ((b)->bcm_flags & BMAP_BUSY) &&			\
		    (b)->bcm_owner != _pthr);				\
		(b)->bcm_flags |= BMAP_BUSY;				\
		(b)->bcm_owner = _pthr;					\
		DEBUG_BMAP(PLL_DEBUG, (b), "set BUSY");			\
	} while (0)

#define BMAP_UNBUSY(b)							\
	do {								\
		(void)BMAP_RLOCK(b);					\
		BMAP_BUSY_ENSURE(b);					\
		(b)->bcm_owner = 0;					\
		(b)->bcm_flags &= ~BMAP_BUSY;				\
		DEBUG_BMAP(PLL_DEBUG, (b), "cleared BUSY");		\
		bmap_wake_locked(b);					\
		BMAP_ULOCK(b);						\
	} while (0)

#define BMAP_BUSY_ENSURE(b)						\
	do {								\
		psc_assert((b)->bcm_flags & BMAP_BUSY);			\
		psc_assert((b)->bcm_owner == pthread_self());		\
	} while (0)

// XXX ensure locked???
#define bmap_op_start_type(b, type)					\
	do {								\
		DEBUG_BMAP(PLL_DEBUG, (b),				\
		    "took reference (type=%u)", (type));		\
		psc_atomic32_inc(&(b)->bcm_opcnt);			\
	} while (0)

#define bmap_op_done_type(b, type)					\
	do {								\
		(void)BMAP_RLOCK(b);					\
		DEBUG_BMAP(PLL_DEBUG, (b),				\
		    "drop reference (type=%u)", (type));		\
		psc_assert(psc_atomic32_read(&(b)->bcm_opcnt) > 0);	\
		psc_atomic32_dec(&(b)->bcm_opcnt);			\
		_bmap_op_done(PFL_CALLERINFOSS(SLSS_BMAP), (b),		\
		    _DEBUG_BMAP_FMT "released reference (type=%u)",	\
		    _DEBUG_BMAP_FMTARGS(b), (type));			\
	} while (0)

#define bmap_op_done(b)		bmap_op_done_type((b), BMAP_OPCNT_LOOKUP)

#define bmap_foff(b)		((b)->bcm_bmapno * SLASH_BMAP_SIZE)

/* bmap per-replica states */
#define BREPLST_INVALID		0	/* no data present (zeros) */
#define BREPLST_REPL_SCHED	1	/* replica is being made */
#define BREPLST_REPL_QUEUED	2	/* replica needs to be made */
#define BREPLST_VALID		3	/* replica is active */
#define BREPLST_TRUNCPNDG	4	/* partial truncation in bmap */
#define BREPLST_TRUNCPNDG_SCHED	5	/* ptrunc resolving CRCs recomp */
#define BREPLST_GARBAGE		6	/* marked for reclamation */
#define BREPLST_GARBAGE_SCHED	7	/* being reclaimed */
#define NBREPLST		8

/* CRC of a zeroed sliver */
#define BMAP_NULL_CRC		UINT64_C(0x436f5d7c450ed606)

#define	BMAP_OD_CRCSZ		sizeof(struct bmap_ondisk)
#define	BMAP_OD_SZ		(BMAP_OD_CRCSZ + sizeof(uint64_t))

/* bcs_crcstates flags */
#define BMAP_SLVR_DATA		(1 << 0)	/* Data present, otherwise slvr is hole */
#define BMAP_SLVR_CRC		(1 << 1)	/* Has valid CRC */
#define BMAP_SLVR_CRCABSENT	(1 << 3)
#define _BMAP_SLVR_FLSHFT	(1 << 4)

/*
 * Routines to get and fetch a bmap replica's status.
 * This code assumes NBREPLST is < 256
 */
#define SL_REPL_GET_BMAP_IOS_STAT(data, off)				\
	(SL_REPLICA_MASK &						\
	    (((data)[(off) / NBBY] >> ((off) % NBBY)) |			\
	    ((off) % NBBY + SL_BITS_PER_REPLICA > NBBY ?		\
	     (data)[(off) / NBBY + 1] << (SL_BITS_PER_REPLICA -		\
	      ((off) % NBBY + SL_BITS_PER_REPLICA - NBBY)) : 0)))

#define SL_REPL_SET_BMAP_IOS_STAT(data, off, val)			\
	do {								\
		int _j;							\
									\
		(data)[(off) / NBBY] = ((data)[(off) / NBBY] &		\
		    ~(SL_REPLICA_MASK << ((off) % NBBY))) |		\
		    ((val) << ((off) % NBBY));				\
		_j = (off) % NBBY + SL_BITS_PER_REPLICA - NBBY;		\
		if (_j > 0) {						\
			_j = SL_BITS_PER_REPLICA - _j;			\
			(data)[(off) / NBBY + 1] =			\
			    ((data)[(off) / NBBY + 1] &			\
			    ~(SL_REPLICA_MASK >> _j)) | ((val) >> _j);	\
		}							\
	} while (0)

/* bmap replication policies */
#define BRPOL_ONETIME		0
#define BRPOL_PERSIST		1
#define NBRPOL			2

#define DEBUG_BMAPOD(level, bmap, fmt, ...)				\
	_dump_bmapod(PFL_CALLERINFOSS(SLSS_BMAP), (level), (bmap),	\
	    (fmt), ## __VA_ARGS__)

#define DEBUG_BMAPODV(level, bmap, fmt, ap)				\
	_dump_bmapodv(PFL_CALLERINFOSS(SLSS_BMAP), (level),		\
	    (bmap), (fmt), (ap))

/* bmap_get flags */
#define BMAPGETF_LOAD		(1 << 0)	/* allow loading if not in cache */
#define BMAPGETF_NORETRIEVE	(1 << 1)	/* when loading, do not invoke retrievef */
#define BMAPGETF_NOAUTOINST	(1 << 2)	/* do not autoinstantiate */

int	 bmap_cmp(const void *, const void *);
void	 bmap_cache_init(size_t);
void	 bmap_free_all_locked(struct fidc_membh *);
void	 bmap_biorq_waitempty(struct bmapc_memb *);
void	_bmap_op_done(const struct pfl_callerinfo *,
	    struct bmapc_memb *, const char *, ...);
int	_bmap_get(const struct pfl_callerinfo *, struct fidc_membh *,
	    sl_bmapno_t, enum rw, int, struct bmapc_memb **);
struct bmapc_memb *
	 bmap_lookup_cache(struct fidc_membh *, sl_bmapno_t, int *);

int	 bmapdesc_access_check(struct srt_bmapdesc *, enum rw, sl_ios_id_t);

void	 dump_bmap_repls(uint8_t *);

void	_dump_bmap_flags_common(uint32_t *, int *);

void	_dump_bmapodv(const struct pfl_callerinfo *, int,
	    struct bmapc_memb *, const char *, va_list);
void	_dump_bmapod(const struct pfl_callerinfo *, int,
	    struct bmapc_memb *, const char *, ...);

#define bmap_getf(f, n, rw, fl, bp)	_bmap_get(			\
					    PFL_CALLERINFOSS(SLSS_BMAP),\
					    (f), (n), (rw), (fl), (bp))

#define bmap_lookup(f, n, bp)		bmap_getf((f), (n), 0, 0, (bp))
#define bmap_get(f, n, rw, bp)		bmap_getf((f), (n), (rw),	\
					    BMAPGETF_LOAD, (bp))

enum bmap_opcnt_types {
/*  0 */ BMAP_OPCNT_LOOKUP,		/* bmap_get */
/*  1 */ BMAP_OPCNT_IONASSIGN,
/*  2 */ BMAP_OPCNT_LEASE,
/*  3 */ BMAP_OPCNT_FLUSHQ,
/*  4 */ BMAP_OPCNT_BIORQ,
/*  5 */ BMAP_OPCNT_REPLWK,		/* repl work inside ION */
/*  6 */ BMAP_OPCNT_REAPER,		/* client bmap timeout */
/*  7 */ BMAP_OPCNT_SLVR,
/*  8 */ BMAP_OPCNT_BCRSCHED,
/*  9 */ BMAP_OPCNT_RLSSCHED,
/* 10 */ BMAP_OPCNT_TRUNCWAIT,
/* 11 */ BMAP_OPCNT_READA,
/* 12 */ BMAP_OPCNT_LEASEEXT,
/* 13 */ BMAP_OPCNT_REASSIGN,
/* 14 */ BMAP_OPCNT_UPSCH,		/* peer update scheduler */
/* 15 */ BMAP_OPCNT_WORK		/* generic worker thread */
};

SPLAY_HEAD(bmap_cache, bmapc_memb);
SPLAY_PROTOTYPE(bmap_cache, bmapc_memb, bcm_tentry, bmap_cmp);

struct bmap_ops {
	void	(*bmo_init_privatef)(struct bmapc_memb *);
	int	(*bmo_retrievef)(struct bmapc_memb *, enum rw, int);
	int	(*bmo_mode_chngf)(struct bmapc_memb *, enum rw, int);
	void	(*bmo_final_cleanupf)(struct bmapc_memb *);
};

extern struct bmap_ops sl_bmap_ops;

static __inline void *
bmap_get_pri(struct bmapc_memb *b)
{
	psc_assert(b);
	return (b + 1);
}

static __inline const void *
bmap_get_pri_const(const struct bmapc_memb *b)
{
	psc_assert(b);
	return (b + 1);
}

static __inline void
brepls_init(int *ar, int val)
{
	int i;

	for (i = 0; i < NBREPLST; i++)
		ar[i] = val;
}

static __inline void
brepls_init_idx(int *ar)
{
	int i;

	for (i = 0; i < NBREPLST; i++)
		ar[i] = i;
}

#endif /* _BMAP_H_ */
