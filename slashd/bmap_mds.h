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

#ifndef _SLASHD_BMAP_MDS_H_
#define _SLASHD_BMAP_MDS_H_

#include <sys/time.h>

#include "psc_ds/lockedlist.h"
#include "psc_rpc/rpc.h"
#include "psc_util/odtable.h"
#include "psc_util/pthrutil.h"

#include "bmap.h"
#include "mdslog.h"
#include "slashd.h"
#include "inode.h"

struct srt_bmapdesc;

/*
 * bmap_mds_info - the bmap_get_pri() data structure for the slash2 mds.
 *   bmap_mds_info holds all bmap specific context for the mds which
 *   includes the journal handle, ref counts for client readers and writers
 *   a point to our ION, a tree of our client's exports, a pointer to the
 *   on-disk structure, a receipt for the odtable, and a reqset for issuing
 *   callbacks (XXX is that really needed?).
 * Notes: both read and write clients are stored to bmdsi_exports, the ref
 *   counts are used to determine the number of both and hence the caching
 *   mode used at the clients.   bmdsi_wr_ion is a shortcut pointer used
 *   only when the bmap has client writers - all writers (and readers) are
 *   directed to this ion once a client has invoked write mode on the bmap.
 */
struct bmap_mds_info {
	/*
	 * This structure must start with the continuation of
	 * bmap_ondisk from where bmapc_memb left off so an entire
	 * bmap_ondisk will be laid contiguously in memory for I/O over
	 * the network and with ZFS.
	 */
	struct bmap_extra_state	 bmdsi_extrastate;
	uint64_t		 bmdsi_ondiskcrc;

	struct resm_mds_info	*bmdsi_wr_ion;		/* pointer to write ION */
	struct psc_lockedlist	 bmdsi_leases;		/* tracked bmap leases */
	struct odtable_receipt	*bmdsi_assign;
	uint64_t		 bmdsi_seq;		/* Largest write bml seq # */
	uint32_t		 bmdsi_xid;		/* last op recv'd from ION */
	int32_t			 bmdsi_writers;
	int32_t			 bmdsi_readers;
	struct psc_pthread_rwlock bmdsi_rwlock;
};

/* MDS-specific bcm_flags */
#define BMAP_MDS_CRC_UP		(_BMAP_FLSHFT << 0)	/* CRC update in progress */
#define BMAP_MDS_CRCWRT		(_BMAP_FLSHFT << 1)
#define BMAP_MDS_NOION		(_BMAP_FLSHFT << 2)
#define BMAP_MDS_LOGCHG		(_BMAP_FLSHFT << 3)	/* in-mem change made, needs logged */
#define BMAP_MDS_DIO		(_BMAP_FLSHFT << 4)	/* direct I/O enabled */
#define BMAP_MDS_SEQWRAP	(_BMAP_FLSHFT << 5)	/* sequence number wrapped */

#define bmap_2_bmdsi(b)		((struct bmap_mds_info *)bmap_get_pri(b))
#define bmap_2_bmi(b)		((struct bmap_mds_info *)bmap_get_pri(b))
#define bmap_2_bmdsassign(b)	bmap_2_bmdsi(b)->bmdsi_assign
#define bmap_2_xstate(b)	(&bmap_2_bmi(b)->bmdsi_extrastate)
#define bmap_2_bgen(b)		bmap_2_xstate(b)->bes_gen
#define bmap_2_replpol(b)	bmap_2_xstate(b)->bes_repl_policy
#define bmap_2_repl(b, i)	fcmh_2_repl((b)->bcm_fcmh, (i))
#define bmap_2_ondisk(b)	((struct bmap_ondisk *)(&(b)->bcm_corestate))
#define bmap_2_ondiskcrc(b)	bmap_2_bmi(b)->bmdsi_ondiskcrc
#define bmap_2_crcs(b, n)	bmap_2_xstate(b)->bes_crcs[n]

#define BMAPOD_RDLOCK(bmdsi)	psc_pthread_rwlock_rdlock(&(bmdsi)->bmdsi_rwlock)
#define BMAPOD_RDREQLOCK(bmdsi)	psc_pthread_rwlock_rdreqlock(&(bmdsi)->bmdsi_rwlock)
#define BMAPOD_WRLOCK(bmdsi)	psc_pthread_rwlock_wrlock(&(bmdsi)->bmdsi_rwlock)
#define BMAPOD_ULOCK(bmdsi)	psc_pthread_rwlock_unlock(&(bmdsi)->bmdsi_rwlock)

#define BMDSI_LOGCHG_SET(b)						\
	do {								\
		int _locked;						\
									\
		_locked = BMAP_RLOCK(b);				\
		(b)->bcm_flags |= BMAP_MDS_LOGCHG;			\
		BMAP_URLOCK((b), _locked);				\
	} while (0)

#define BMDSI_LOGCHG_CLEAR(b)						\
	do {								\
		int _locked;						\
									\
		_locked = BMAP_RLOCK(b);				\
		(b)->bcm_flags &= ~BMAP_MDS_LOGCHG;			\
		BMAP_URLOCK((b), _locked);				\
	} while (0)

#define BMDSI_LOGCHG_CHECK(b, set)					\
	do {								\
		int _locked;						\
									\
		_locked = BMAP_RLOCK(b);				\
		(set) = (b)->bcm_flags & BMAP_MDS_LOGCHG;		\
		BMAP_URLOCK((b), _locked);				\
	} while (0)

#define BMAPOD_MODIFY_START(b)	BMAPOD_WRLOCK(bmap_2_bmdsi(b))
#define BMAPOD_MODIFY_DONE(b)	BMAPOD_ULOCK(bmap_2_bmdsi(b))
#define BMAPOD_READ_START(b)	BMAPOD_RDLOCK(bmap_2_bmdsi(b))
#define BMAPOD_READ_DONE(b)	BMAPOD_ULOCK(bmap_2_bmdsi(b))

#define BHREPL_POLICY_SET(b, pol)					\
	do {								\
		BMAPOD_MODIFY_START(b);					\
		bmap_2_bgen(b) = (pol);					\
		BMDSI_LOGCHG_SET(b);					\
		BMAPOD_MODIFY_DONE(b);					\
	} while (0)

#define BHREPL_POLICY_GET(b, pol)					\
	do {								\
		BMAPOD_READ_START(b);					\
		(pol) = bmap_2_bgen(b);					\
		BMAPOD_READ_DONE(b);					\
	} while (0)

#define BHGEN_INCREMENT(b)						\
	do {								\
		BMAPOD_MODIFY_START(b);					\
		bmap_2_bgen(b)++;					\
		BMDSI_LOGCHG_SET(b);					\
		BMAPOD_MODIFY_DONE(b);					\
	} while (0)

#define BHGEN_GET(b, gen)						\
	do {								\
		BMAPOD_READ_START(b);					\
		(gen) = bmap_2_bgen(b);					\
		BMAPOD_READ_DONE(b);					\
	} while (0)

struct bmap_timeo_entry {
	uint64_t		 bte_maxseq;
	struct psclist_head	 bte_bmaps;
};

struct bmap_timeo_table {
	psc_spinlock_t		 btt_lock;
	/*
	 * High and low water marks of the bmap sequence number. The MDS communicates
	 * the low water mark to an I/O server so that the latter can reject timed
	 * out bmaps.
	 */
	uint64_t		 btt_maxseq;
	uint64_t		 btt_minseq;
	struct bmap_timeo_entry	*btt_entries;
	int			 btt_nentries;
	int			 btt_ready;
};

/* mds_bmap_timeotbl_mdsi (bmap timeout event) ops */
#define BTE_ADD			(1 << 0)
#define BTE_DEL			(1 << 1)
#define BTE_REATTACH		(1 << 2)

#define BMAP_TIMEO_MAX		120	/* Max bmap lease timeout */
#define BMAP_TIMEO_TBL_QUANT	5
#define BMAP_TIMEO_TBL_SZ	(BMAP_TIMEO_MAX / BMAP_TIMEO_TBL_QUANT)
#define BMAP_SEQLOG_FACTOR	100

struct bmap_mds_lease {
	uint64_t		  bml_seq;
	lnet_nid_t		  bml_ion_nid;
	lnet_process_id_t	  bml_cli_nidpid;
	uint32_t		  bml_flags;
	psc_spinlock_t		  bml_lock;
	time_t			  bml_start;
	struct bmap_mds_info	 *bml_bmdsi;
	struct pscrpc_export	 *bml_exp;
	struct psclist_head	  bml_bmdsi_lentry;
	struct psclist_head	  bml_timeo_lentry;
	struct psclist_head	  bml_exp_lentry;
	struct psclist_head	  bml_coh_lentry;
	struct bmap_mds_lease	 *bml_chain;		/* chain of duplicate leases */
};

/* bml_flags */
#define	BML_READ	(1 <<  0)
#define	BML_WRITE	(1 <<  1)
#define	BML_CDIO	(1 <<  2)
#define	BML_COHRLS	(1 <<  3)
#define	BML_COHDIO	(1 <<  4)
#define	BML_EXP		(1 <<  5)
#define	BML_TIMEOQ	(1 <<  6)
#define	BML_BMDSI	(1 <<  7)
#define	BML_COH		(1 <<  8)
#define	BML_RECOVER	(1 <<  9)
#define	BML_CHAIN	(1 << 10)
#define	BML_UPGRADE	(1 << 11)
#define	BML_EXPFAIL	(1 << 12)

#define bml_2_bmap(bml)		bmi_2_bmap((bml)->bml_bmdsi)

#define BML_LOCK_ENSURE(bml)	LOCK_ENSURE(&(bml)->bml_lock)
#define BML_LOCK(bml)		spinlock(&(bml)->bml_lock)
#define BML_ULOCK(bml)		freelock(&(bml)->bml_lock)

/**
 * bmap_ion_assign - The structure used for tracking the MDS's bmap/ion
 *   assignments.  These structures are stored in a odtable.
 * Note: default odtable entry size is 128 bytes.
 */
struct bmap_ion_assign {
	lnet_nid_t		bia_ion_nid;
	lnet_process_id_t	bia_lastcli;
	sl_ios_id_t		bia_ios;
	slfid_t			bia_fid;
	uint64_t		bia_seq;
	sl_bmapno_t		bia_bmapno;
	time_t			bia_start;
	int			bia_flags;
};

/* bia_flags */
#define BIAF_DIO		(1 << 0)

#define mds_bmap_load(f, n, bp)	bmap_get((f), (n), 0, (bp))

#define mds_bml_free(bml)	psc_pool_return(bmapMdsLeasePool, (bml))

int	 mds_bmap_crc_write(struct srm_bmap_crcup *, lnet_nid_t);
int	 mds_bmap_exists(struct fidc_membh *, sl_bmapno_t);
int	 mds_bmap_load_cli(struct fidc_membh *, sl_bmapno_t, int, enum rw,
	    sl_ios_id_t, struct srt_bmapdesc *, struct pscrpc_export *,
	    struct bmapc_memb **);
int	 mds_bmap_load_ion(const struct slash_fidgen *, sl_bmapno_t,
	    struct bmapc_memb **);
int	 mds_bmap_loadvalid(struct fidc_membh *, sl_bmapno_t,
	    struct bmapc_memb **);
int	 mds_bmap_bml_chwrmode(struct bmap_mds_lease *, sl_ios_id_t);
int	 mds_bmap_bml_release(struct bmap_mds_lease *);
struct bmap_mds_lease *
	mds_bmap_getbml(struct bmapc_memb *, lnet_nid_t, lnet_pid_t, uint64_t);

void	 mds_bmap_setcurseq(uint64_t, uint64_t);
int	 mds_bmap_getcurseq(uint64_t *, uint64_t *);

void	 mds_bmap_timeotbl_init(void);
uint64_t mds_bmap_timeotbl_getnextseq(void);
uint64_t mds_bmap_timeotbl_mdsi(struct bmap_mds_lease *, int);

void	 mds_bia_odtable_startup_cb(void *, struct odtable_receipt *);

extern struct psc_poolmaster	 bmapMdsLeasePoolMaster;
extern struct psc_poolmgr	*bmapMdsLeasePool;

static __inline struct bmapc_memb *
bmi_2_bmap(struct bmap_mds_info *bmi)
{
	struct bmapc_memb *bcm;

	psc_assert(bmi);
	bcm = (void *)bmi;
	return (bcm - 1);
}

#endif /* _SLASHD_BMAP_MDS_H_ */
