/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2006-2011, Pittsburgh Supercomputing Center (PSC).
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
#include "psc_util/log.h"

#include "bmap_mds.h"
#include "fidc_mds.h"
#include "mdsio.h"
#include "repl_mds.h"
#include "slerr.h"

/**
 * mds_bmap_initnew - Called when a read request offset exceeds the
 *	bounds of the file causing a new bmap to be created.
 * Notes:  Bmap creation race conditions are prevented because the bmap
 *	handle already exists at this time with
 *	bmapi_mode == BMAP_INIT.
 *
 *	This causes other threads to block on the waitq until
 *	read/creation has completed.
 * More Notes:  this bmap is not written to disk until a client actually
 *	writes something to it.
 */
__static void
mds_bmap_initnew(struct bmapc_memb *b)
{
	struct bmap_ondisk *bod = bmap_2_ondisk(b);
	struct fidc_membh *fcmh = b->bcm_fcmh;
	uint32_t pol;
	int i;

	for (i = 0; i < SLASH_CRCS_PER_BMAP; i++)
		bod->bod_crcs[i] = BMAP_NULL_CRC;

	INOH_LOCK(fcmh_2_inoh(fcmh));
	pol = fcmh_2_ino(fcmh)->ino_replpol;
	INOH_ULOCK(fcmh_2_inoh(fcmh));

	BHREPL_POLICY_SET(b, pol);
}

void
mds_bmap_ensure_valid(struct bmapc_memb *b)
{
	int rc, retifset[NBREPLST];

	brepls_init(retifset, 0);
	retifset[BREPLST_VALID] = 1;
	retifset[BREPLST_GARBAGE] = 1;
	retifset[BREPLST_GARBAGE_SCHED] = 1;
	retifset[BREPLST_TRUNCPNDG] = 1;
	retifset[BREPLST_TRUNCPNDG_SCHED] = 1;
	rc = mds_repl_bmap_walk_all(b, NULL, retifset,
	    REPL_WALKF_SCIRCUIT);
	if (!rc)
		psc_fatal("bmap has no valid replicas");
}

/**
 * mds_bmap_read - Retrieve a bmap from the ondisk inode file.
 * @bcm: bmap.
 * Returns zero on success, negative errno code on failure.
 */
int
mds_bmap_read(struct bmapc_memb *bcm, __unusedx enum rw rw)
{
	struct fidc_membh *f = bcm->bcm_fcmh;
	int rc;

	rc = mdsio_bmap_read(bcm);
	/*
	 * Check for a NULL CRC if we had a good read.  NULL CRC can happen
	 *    when bmaps are gaps that have not been written yet.   Note
	 *    that a short read is tolerated as long as the bmap is zeroed.
	 */
	if (!rc || rc == SLERR_SHORTIO) {
		if (bmap_2_ondiskcrc(bcm) == 0 &&
		    pfl_memchk(bmap_2_ondisk(bcm), 0, BMAP_OD_SZ)) {
			DEBUG_BMAPOD(PLL_INFO, bcm, "");
			mds_bmap_initnew(bcm);
			DEBUG_BMAPOD(PLL_INFO, bcm, "");
			return (0);
		}
	}

	/*
	 * At this point, the short I/O is an error since the bmap isn't
	 *    zeros.
	 */
	if (rc) {
		DEBUG_FCMH(PLL_ERROR, f, "mdsio_bmap_read: "
		    "bmapno=%u, rc=%d", bcm->bcm_bmapno, rc);
		return (-EIO);
	}

	mds_bmap_ensure_valid(bcm);

	DEBUG_BMAPOD(PLL_INFO, bcm, "");
	return (0);
}

void
mds_bmap_init(struct bmapc_memb *bcm)
{
	struct bmap_mds_info *bmi;

	bmi = bmap_2_bmdsi(bcm);
	pll_init(&bmi->bmdsi_leases, struct bmap_mds_lease,
	    bml_bmdsi_lentry, &bcm->bcm_lock);
	bmi->bmdsi_xid = 0;
	psc_rwlock_init(&bmi->bmdsi_rwlock);
}

void
mds_bmap_destroy(struct bmapc_memb *bcm)
{
	struct bmap_mds_info *bmdsi = bmap_2_bmdsi(bcm);

	psc_assert(bmdsi->bmdsi_writers == 0);
	psc_assert(bmdsi->bmdsi_readers == 0);
	psc_assert(bmdsi->bmdsi_assign == NULL);
	psc_assert(pll_empty(&bmdsi->bmdsi_leases));
}

void
mds_bmap_calc_crc(struct bmapc_memb *bmap)
{
	int locked;

	locked = BMAPOD_REQWRLOCK(bmap_2_bmdsi(bmap));
	psc_crc64_calc(&bmap_2_ondiskcrc(bmap), bmap_2_ondisk(bmap),
	    BMAP_OD_CRCSZ);
	BMAPOD_UREQLOCK(bmap_2_bmdsi(bmap), locked);
}

/**
 * mdsio_bmap_crc_update - Handle CRC updates for one bmap by pushing
 *	the updates to ZFS and then log it.
 */
int
mds_bmap_crc_update(struct bmapc_memb *bmap, struct
    srm_bmap_crcup *crcup)
{
	struct bmap_mds_info *bmdsi = bmap_2_bmdsi(bmap);
	struct sl_mds_crc_log crclog;
	uint32_t utimgen, i;
	int extend = 0;

	psc_assert(bmap->bcm_flags & BMAP_MDS_CRC_UP);

	FCMH_LOCK(bmap->bcm_fcmh);
	if (crcup->fsize > (uint64_t)fcmh_2_fsz(bmap->bcm_fcmh))
		extend = 1;
	mds_fcmh_increase_fsz(bmap->bcm_fcmh, crcup->fsize);
	utimgen = bmap->bcm_fcmh->fcmh_sstb.sst_utimgen;
	FCMH_ULOCK(bmap->bcm_fcmh);

	if (utimgen < crcup->utimgen)
		DEBUG_FCMH(PLL_ERROR, bmap->bcm_fcmh,
		   "utimgen %d < crcup->utimgen %d",
		   utimgen, crcup->utimgen);

	crcup->extend = extend;
	crclog.scl_bmap = bmap;
	crclog.scl_crcup = crcup;

	BMAPOD_WRLOCK(bmdsi);
	for (i = 0; i < crcup->nups; i++) {
		bmap_2_crcs(bmap, crcup->crcs[i].slot) =
		    crcup->crcs[i].crc;

		bmap->bcm_crcstates[crcup->crcs[i].slot] =
		    BMAP_SLVR_DATA | BMAP_SLVR_CRC;

		DEBUG_BMAP(PLL_INFO, bmap, "slot(%d) crc(%"PSCPRIxCRC64")",
		    crcup->crcs[i].slot, crcup->crcs[i].crc);
	}
	mds_bmap_calc_crc(bmap);
	return (mdsio_bmap_write(bmap, utimgen == crcup->utimgen,
	    mds_bmap_crc_log, &crclog));
}

/**
 * mds_bmap_repl_update - We update bmap replication status in two cases:
 *	(1) An MDS issues a write lease to a client.
 *	(2) An MDS performs a replicate request.
 */
int
mds_bmap_repl_update(struct bmapc_memb *bmap, int log)
{
	int logchg;

	BMAPOD_REQRDLOCK(bmap_2_bmdsi(bmap));
	BMDSI_LOGCHG_CHECK(bmap, logchg);
	if (!logchg) {
		BMAPOD_READ_DONE(bmap, 0);
		return (0);
	}
	BMAPOD_REQWRLOCK(bmap_2_bmdsi(bmap));
	mds_bmap_calc_crc(bmap);
	return (mdsio_bmap_write(bmap, 0,
	    log ? mds_bmap_repl_log : NULL, bmap));
}

#if PFL_DEBUG > 0
void
dump_bmap_flags(uint32_t flags)
{
	int seq = 0;

	_dump_bmap_flags(&flags, &seq);
	PFL_PRFLAG(BMAP_MDS_CRC_UP, &flags, &seq);
	PFL_PRFLAG(BMAP_MDS_CRCWRT, &flags, &seq);
	PFL_PRFLAG(BMAP_MDS_NOION, &flags, &seq);
	PFL_PRFLAG(BMAP_MDS_LOGCHG, &flags, &seq);
	PFL_PRFLAG(BMAP_MDS_DIO, &flags, &seq);
	PFL_PRFLAG(BMAP_MDS_SEQWRAP, &flags, &seq);
	if (flags)
		printf(" unknown: %#x", flags);
	printf("\n");
}

void
dump_bml_flags(uint32_t flags)
{
	int seq = 0;

	PFL_PRFLAG(BML_READ, &flags, &seq);
	PFL_PRFLAG(BML_WRITE, &flags, &seq);
	PFL_PRFLAG(BML_CDIO, &flags, &seq);
	PFL_PRFLAG(BML_COHRLS, &flags, &seq);
	PFL_PRFLAG(BML_COHDIO, &flags, &seq);
	PFL_PRFLAG(BML_EXP, &flags, &seq);
	PFL_PRFLAG(BML_TIMEOQ, &flags, &seq);
	PFL_PRFLAG(BML_BMDSI, &flags, &seq);
	PFL_PRFLAG(BML_COH, &flags, &seq);
	PFL_PRFLAG(BML_RECOVER, &flags, &seq);
	PFL_PRFLAG(BML_CHAIN, &flags, &seq);
	PFL_PRFLAG(BML_UPGRADE, &flags, &seq);
	PFL_PRFLAG(BML_EXPFAIL, &flags, &seq);
	PFL_PRFLAG(BML_FREEING, &flags, &seq);
	PFL_PRFLAG(BML_ASSFAIL, &flags, &seq);
	printf("\n");
}
#endif

struct bmap_ops bmap_ops = {
	mds_bmap_init,
	mds_bmap_read,
	NULL,
	mds_bmap_destroy
};
