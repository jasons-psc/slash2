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

#include "pfl/export.h"
#include "pfl/fs.h"
#include "pfl/hashtbl.h"
#include "pfl/lockedlist.h"
#include "pfl/rpclog.h"
#include "pfl/rsx.h"
#include "pfl/tree.h"
#include "pfl/treeutil.h"
#include "psc_util/alloc.h"
#include "psc_util/atomic.h"
#include "psc_util/ctlsvr.h"
#include "psc_util/log.h"
#include "psc_util/odtable.h"

#include "bmap.h"
#include "bmap_mds.h"
#include "cache_params.h"
#include "fidc_mds.h"
#include "fidcache.h"
#include "inode.h"
#include "journal_mds.h"
#include "mdscoh.h"
#include "mdsio.h"
#include "mdslog.h"
#include "odtable_mds.h"
#include "repl_mds.h"
#include "rpc_mds.h"
#include "slashd.h"
#include "slashrpc.h"
#include "slerr.h"
#include "up_sched_res.h"
#include "worker.h"

#include "zfs-fuse/zfs_slashlib.h"

struct odtable		*mdsBmapAssignTable;

sqlite3			*slm_dbh;
struct pfl_mutex	 slm_dbh_mut = PSC_MUTEX_INIT;
struct psc_hashtbl	 slm_sth_hashtbl;

int
mds_bmap_exists(struct fidc_membh *f, sl_bmapno_t n)
{
	sl_bmapno_t nb;
	int locked;

	locked = FCMH_RLOCK(f);
#if 0
	if ((rc = mds_stat_refresh_locked(f)))
		return (rc);
#endif

	nb = fcmh_nvalidbmaps(f);

	/* XXX just read the bmap and check for valid state */

	psclog_debug("f+g="SLPRI_FG" nb=%u fsz=%"PSCPRIdOFFT,
	    SLPRI_FG_ARGS(&f->fcmh_fg), nb, fcmh_2_fsz(f));

	FCMH_URLOCK(f, locked);
	return (n < nb);
}

int64_t
slm_bmap_calc_repltraffic(struct bmapc_memb *b)
{
	struct fidc_membh *f;
	int i, locked[2];
	int64_t amt = 0;
	off_t bsiz, sz;

	locked[1] = 0; /* gcc */

	f = b->bcm_fcmh;
	locked[0] = FCMH_RLOCK(f);
	if (locked[0] == PSLRV_WASLOCKED)
		locked[1] = BMAP_RLOCK(b);
	else
		BMAP_LOCK(b);
	for (i = 0; i < SLASH_SLVRS_PER_BMAP; i++) {
		if (b->bcm_crcstates[i] & BMAP_SLVR_DATA) {
			/*
			 * If this is the last bmap, tally only the
			 * portion of data that exists.  This is needed
			 * to fill big network pipes when dealing with
			 * lots of small files.
			 */
			bsiz = fcmh_2_fsz(f) % SLASH_BMAP_SIZE;
			if (bsiz == 0 && fcmh_2_fsz(f))
				bsiz = SLASH_BMAP_SIZE;
			if (b->bcm_bmapno == fcmh_nvalidbmaps(f) - 1 &&
			    i == bsiz / SLASH_SLVR_SIZE) {
				sz = bsiz % SLASH_SLVR_SIZE;
				if (sz == 0 && bsiz)
					sz = SLASH_SLVR_SIZE;
				amt += sz;
				break;
			}
			amt += SLASH_SLVR_SIZE;
		}
	}
	if (locked[0] == PSLRV_WASLOCKED)
		BMAP_URLOCK(b, locked[1]);
	else
		BMAP_ULOCK(b);
	FCMH_URLOCK(f, locked[0]);
	return (amt);
}

/**
 * mds_bmap_directio_locked - Called when a new read or write lease is
 *	added to the bmap.  Maintains the DIRECTIO status of the bmap
 *	based on the numbers of readers and writers present.
 * @b: the bmap
 * @rw: read / write op
 * @np: value-result target ION network + process ID.
 * Note: the new bml has yet to be added.
 */
__static int
mds_bmap_directio_locked(struct bmapc_memb *b, enum rw rw, int want_dio,
    lnet_process_id_t *np)
{
	struct bmap_mds_info *bmi = bmap_2_bmi(b);
	struct bmap_mds_lease *bml = NULL, *tmp;
	int rc = 0, force_dio = 0, check_leases = 0;

	BMAP_LOCK_ENSURE(b);

	if (b->bcm_flags & BMAP_DIO)
		return (0);

	/*
	 * We enter into the DIO mode in three cases:
	 *
	 *  (1) Our caller wants a DIO lease
	 *  (2) There is already a write lease out there
	 *  (3) We want to a write lease when there are read leases out there.
	 *
	 * In addition, even if the current lease request does not trigger a
	 * DIO by itself, it has to wait if there is a DIO downgrade already
	 * in progress.
	 */
	if (!want_dio && (b->bcm_flags & BMAP_DIOCB))
		want_dio = 1;

	if (want_dio || bmi->bmi_writers || (rw == SL_WRITE && bmi->bmi_readers))
		check_leases = 1;

	if (check_leases) {
		PLL_FOREACH(bml, &bmi->bmi_leases) {
			tmp = bml;
			do {
				/*
				 * A client can have more than one lease in flight
				 * even though it really uses one at any time.
				 */
				if (bml->bml_cli_nidpid.nid == np->nid &&
				    bml->bml_cli_nidpid.pid == np->pid)
					goto next;

				force_dio = 1;

				BML_LOCK(bml);
				if (bml->bml_flags & BML_DIO) {
					BML_ULOCK(bml);
					goto next;
				}

				rc = -SLERR_BMAP_DIOWAIT;
				if (!(bml->bml_flags & BML_DIOCB)) {
					bml->bml_flags |= BML_DIOCB;
					b->bcm_flags |= BMAP_DIOCB;
					BML_ULOCK(bml);
					mdscoh_req(bml);
				} else
					BML_ULOCK(bml);
 next:
				bml = bml->bml_chain;
			} while (tmp != bml);
		}
	}
	if (!rc && (want_dio || force_dio)) {
		OPSTAT_INCR(SLM_OPST_BMAP_DIO_SET);
		b->bcm_flags |= BMAP_DIO;
		b->bcm_flags &= ~BMAP_DIOCB;
	}
	return (rc);
}

__static int
mds_bmap_ios_restart(struct bmap_mds_lease *bml)
{
	struct sl_resm *resm = libsl_ios2resm(bml->bml_ios);
	struct resm_mds_info *rmmi;
	int rc = 0;

	rmmi = resm2rmmi(resm);
	atomic_inc(&rmmi->rmmi_refcnt);

	psc_assert(bml->bml_bmi->bmi_assign);
	bml->bml_bmi->bmi_wr_ion = rmmi;
	bmap_op_start_type(bml_2_bmap(bml), BMAP_OPCNT_IONASSIGN);

	if (mds_bmap_timeotbl_mdsi(bml, BTE_REATTACH) == BMAPSEQ_ANY)
		rc = 1;

	bml->bml_bmi->bmi_seq = bml->bml_seq;

	DEBUG_BMAP(PLL_INFO, bml_2_bmap(bml), "res(%s) seq=%"PRIx64,
	    resm->resm_res->res_name, bml->bml_seq);

	return (rc);
}

int
mds_sliod_alive(void *arg)
{
	struct sl_mds_iosinfo *si = arg;
	int ok = 0;

	if (si->si_lastcomm.tv_sec) {
		struct timespec a, b;

		clock_gettime(CLOCK_MONOTONIC, &a);
		b = si->si_lastcomm;
		b.tv_sec += CSVC_PING_INTV * 2;

		if (timespeccmp(&a, &b, <))
			ok = 1;
	}

	return (ok);
}

/**
 * mds_try_sliodresm - Given an I/O resource, iterate through its
 *	members looking for one which is suitable for assignment.
 * @res: the resource
 * @prev_ios:  list of previously assigned resources (used for
 *	reassigment requests by the client).
 * @nprev_ios: size of the list.
 * Notes:  Because of logical I/O systems like CNOS, we use
 *	'resm->resm_res_id' instead of 'res->res_id' since the former
 *	points at the real resm's identifier not the logical identifier.
 */
__static int
mds_try_sliodresm(struct sl_resm *resm)
{
	struct slashrpc_cservice *csvc = NULL;
	struct sl_mds_iosinfo *si;
	int ok = 0;

	psclog_info("trying res(%s)", resm->resm_res->res_name);

	/*
	 * Access the resm's res pointer to get around resources which
	 * are marked RES_ISCLUSTER().  resm_res always points back to
	 * the member's native resource and not to a logical resource
	 * like a CNOS.
	 */
	si = res2iosinfo(resm->resm_res);
	if (si->si_flags & SIF_DISABLE_BIA) {
		psclog_diag("res=%s skipped due to SIF_DISABLE_BIA",
		    resm->resm_res->res_name);
		return (0);
	}

	csvc = slm_geticsvc_nb(resm, NULL);
	if (!csvc) {
		/* This sliod hasn't established a connection to us. */
		psclog_info("res=%s skipped due to NULL csvc",
		    resm->resm_res->res_name);
		return (0);
	}

	ok = mds_sliod_alive(si);
	if (!ok)
		psclog_notice("res=%s skipped due to lastcomm",
		    resm->resm_res->res_name);

	if (csvc)
		sl_csvc_decref(csvc);

	return (ok);
}

/**
 * mds_resm_select - Choose an I/O resource member for write bmap lease
 *	assignment.
 * @b: The bmap which is being leased.
 * @pios: The preferred I/O resource specified by the client.
 * @to_skip: IONs to skip
 * @nskip: # IONS in @to_skip.
 * Notes:  This call accounts for the existence of existing replicas.
 *	When found, mds_resm_select() must choose a replica which is
 *	marked as BREPLST_VALID.
 */
__static struct sl_resm *
mds_resm_select(struct bmapc_memb *b, sl_ios_id_t pios,
    sl_ios_id_t *to_skip, int nskip)
{
	int i, j, skip, off, val, nr, repls = 0;
	struct slash_inode_od *ino = fcmh_2_ino(b->bcm_fcmh);
	struct slash_inode_extras_od *inox = NULL;
	struct psc_dynarray a = DYNARRAY_INIT;
	struct sl_resm *resm = NULL;
	sl_ios_id_t ios;

	FCMH_LOCK(b->bcm_fcmh);
	nr = fcmh_2_nrepls(b->bcm_fcmh);
	FCMH_ULOCK(b->bcm_fcmh);

	if (nr > SL_DEF_REPLICAS) {
		mds_inox_ensure_loaded(fcmh_2_inoh(b->bcm_fcmh));
		inox = fcmh_2_inox(b->bcm_fcmh);
	}

	for (i = 0, off = 0; i < nr; i++, off += SL_BITS_PER_REPLICA) {
		val = SL_REPL_GET_BMAP_IOS_STAT(b->bcm_repls, off);
		if (val != BREPLST_INVALID)
			/* Determine if there are any active replicas. */
			repls++;

		if (val != BREPLST_VALID)
			continue;

		ios = (i < SL_DEF_REPLICAS) ? ino->ino_repls[i].bs_id :
		    inox->inox_repls[i - SL_DEF_REPLICAS].bs_id;

		psc_dynarray_add(&a, libsl_ios2resm(ios));
	}

	if (nskip) {
		if (repls != 1) {
			DEBUG_FCMH(PLL_WARN, b->bcm_fcmh,
			    "invalid reassign req");
			DEBUG_BMAP(PLL_WARN, b,
			    "invalid reassign req (repls=%d)", repls);
			goto out;
		}

		/*
		 * Make sure the client had the resource ID which
		 * corresponds to that in the fcmh + bmap.
		 */
		resm = psc_dynarray_getpos(&a, 0);
		for (i = 0, ios = IOS_ID_ANY; i < nskip; i++)
			if (resm->resm_res_id == to_skip[i]) {
				ios = resm->resm_res_id;
				break;
			}

		if (ios == IOS_ID_ANY) {
			DEBUG_FCMH(PLL_WARN, b->bcm_fcmh,
			    "invalid reassign req (res=%x)",
			    resm->resm_res_id);
			DEBUG_BMAP(PLL_WARN, b,
			    "invalid reassign req (res=%x)",
			    resm->resm_res_id);
			goto out;
		}
		psc_dynarray_reset(&a);
		repls = 0;
	}

	if (repls && !psc_dynarray_len(&a)) {
		DEBUG_BMAPOD(PLL_ERROR, b, "no replicas marked valid we "
		    "can use; repls=%d nskip=%d", repls, nskip);
		return (NULL);
	}

	slcfg_get_ioslist(pios, &a, 0);

	DYNARRAY_FOREACH(resm, i, &a) {
		for (j = 0, skip = 0; j < nskip; j++)
			if (resm->resm_res->res_id == to_skip[j]) {
				skip = 1;
				psclog_notice("res=%s skipped due being a "
				    "prev_ios",
				    resm->resm_res->res_name);
				break;
			}

		if (!skip && mds_try_sliodresm(resm))
			break;
	}
 out:
	psc_dynarray_free(&a);
	return (resm);
}

__static int
mds_bmap_add_repl(struct bmapc_memb *b, struct bmap_ios_assign *bia)
{
	struct slmds_jent_bmap_assign *sjba;
	struct slmds_jent_assign_rep *logentry;
	struct slash_inode_handle *ih;
	struct fidc_membh *f;
	uint32_t nrepls;
	int iosidx;

	f = b->bcm_fcmh;
	ih = fcmh_2_inoh(f);
	nrepls = ih->inoh_ino.ino_nrepls;

	psc_assert(b->bcm_flags & BMAP_IONASSIGN);

	FCMH_WAIT_BUSY(f);
	iosidx = mds_repl_ios_lookup_add(current_vfsid, ih,
	    bia->bia_ios);

	if (iosidx < 0)
		psc_fatalx("ios_lookup_add %d: %s", bia->bia_ios,
		    slstrerror(iosidx));

	BMAP_WAIT_BUSY(b);

	if (mds_repl_inv_except(b, iosidx)) {
		DEBUG_BMAP(PLL_ERROR, b, "mds_repl_inv_except() failed");
		BMAP_UNBUSY(b);
		FCMH_UNBUSY(f);
		return (-1);
	}
	mds_reserve_slot(1);
	logentry = pjournal_get_buf(mdsJournal, sizeof(*logentry));

	logentry->sjar_flags = SLJ_ASSIGN_REP_NONE;
	if (nrepls != ih->inoh_ino.ino_nrepls) {
		mdslogfill_ino_repls(f, &logentry->sjar_ino);
		logentry->sjar_flags |= SLJ_ASSIGN_REP_INO;
	}

	mdslogfill_bmap_repls(b, &logentry->sjar_rep);

	BMAP_UNBUSY(b);
	FCMH_UNBUSY(f);

	logentry->sjar_flags |= SLJ_ASSIGN_REP_REP;

	sjba = &logentry->sjar_bmap;
	sjba->sjba_lastcli.nid = bia->bia_lastcli.nid;
	sjba->sjba_lastcli.pid = bia->bia_lastcli.pid;
	sjba->sjba_ios = bia->bia_ios;
	sjba->sjba_fid = bia->bia_fid;
	sjba->sjba_seq = bia->bia_seq;
	sjba->sjba_bmapno = bia->bia_bmapno;
	sjba->sjba_start = bia->bia_start;
	sjba->sjba_flags = bia->bia_flags;
	logentry->sjar_flags |= SLJ_ASSIGN_REP_BMAP;
	logentry->sjar_elem = bmap_2_bmi(b)->bmi_assign->odtr_elem;

	pjournal_add_entry(mdsJournal, 0, MDS_LOG_BMAP_ASSIGN, 0,
	    logentry, sizeof(*logentry));
	pjournal_put_buf(mdsJournal, logentry);
	mds_unreserve_slot(1);

	return (0);
}

/**
 * mds_bmap_ios_assign - Bind a bmap to an ION for writing.  The process
 *	involves a round-robin'ing of an I/O system's nodes and
 *	attaching a resm_mds_info to the bmap, used for establishing
 *	connection to the ION.
 * @bml: the bmap lease
 * @pios: the preferred I/O system
 */
__static int
mds_bmap_ios_assign(struct bmap_mds_lease *bml, sl_ios_id_t pios)
{
	struct bmapc_memb *b = bml_2_bmap(bml);
	struct bmap_mds_info *bmi = bmap_2_bmi(b);
	struct resm_mds_info *rmmi;
	struct bmap_ios_assign bia;
	struct sl_resm *resm;

	psc_assert(!bmi->bmi_wr_ion);
	psc_assert(!bmi->bmi_assign);
	psc_assert(psc_atomic32_read(&b->bcm_opcnt) > 0);

	BMAP_LOCK(b);
	psc_assert(b->bcm_flags & BMAP_IONASSIGN);
	BMAP_ULOCK(b);

	resm = mds_resm_select(b, pios, NULL, 0);
	if (!resm) {
		BMAP_SETATTR(b, BMAP_MDS_NOION);
		bml->bml_flags |= BML_ASSFAIL;

		psclog_warnx("unable to contact ION %#x for lease", pios);

		return (-SLERR_ION_OFFLINE);
	}

	bmi->bmi_wr_ion = rmmi = resm2rmmi(resm);
	atomic_inc(&rmmi->rmmi_refcnt);

	DEBUG_BMAP(PLL_INFO, b, "online res(%s)",
	    resm->resm_res->res_name);

	/*
	 * An ION has been assigned to the bmap, mark it in the odtable
	 * so that the assignment may be restored on reboot.
	 */
	memset(&bia, 0, sizeof(bia));
	bia.bia_ios = bml->bml_ios = rmmi2resm(rmmi)->resm_res_id;
	bia.bia_lastcli = bml->bml_cli_nidpid;
	bia.bia_fid = fcmh_2_fid(b->bcm_fcmh);
	bia.bia_seq = bmi->bmi_seq = mds_bmap_timeotbl_mdsi(bml, BTE_ADD);
	bia.bia_bmapno = b->bcm_bmapno;
	bia.bia_start = time(NULL);
	bia.bia_flags = (b->bcm_flags & BMAP_DIO) ? BIAF_DIO : 0;

	bmi->bmi_assign = mds_odtable_putitem(mdsBmapAssignTable, &bia,
	    sizeof(bia));

	if (!bmi->bmi_assign) {
		BMAP_SETATTR(b, BMAP_MDS_NOION);
		bml->bml_flags |= BML_ASSFAIL;

		DEBUG_BMAP(PLL_ERROR, b, "failed odtable_putitem()");
		// XXX fix me - dont leak the journal buf!
		return (-SLERR_XACT_FAIL);
	}
	BMAP_CLEARATTR(b, BMAP_MDS_NOION);

	/*
	 * Signify that a ION has been assigned to this bmap.  This
	 * opcnt ref will stay in place until the bmap has been released
	 * by the last client or has been timed out.
	 */
	bmap_op_start_type(b, BMAP_OPCNT_IONASSIGN);

	if (mds_bmap_add_repl(b, &bia))
		return (-1); // errno

	bml->bml_seq = bia.bia_seq;

	DEBUG_FCMH(PLL_INFO, b->bcm_fcmh, "bmap assign, elem=%zd",
	    bmi->bmi_assign->odtr_elem);
	DEBUG_BMAP(PLL_INFO, b, "using res(%s) "
	    "rmmi(%p) bia(%p)", resm->resm_res->res_name,
	    bmi->bmi_wr_ion, bmi->bmi_assign);

	return (0);
}

__static int
mds_bmap_ios_update(struct bmap_mds_lease *bml)
{
	struct bmapc_memb *b = bml_2_bmap(bml);
	struct bmap_mds_info *bmi = bmap_2_bmi(b);
	struct bmap_ios_assign bia;
	int rc, dio;

	BMAP_LOCK(b);
	psc_assert(b->bcm_flags & BMAP_IONASSIGN);
	dio = (b->bcm_flags & BMAP_DIO);
	BMAP_ULOCK(b);

	rc = mds_odtable_getitem(mdsBmapAssignTable,
	    bmi->bmi_assign, &bia, sizeof(bia));
	if (rc) {
		DEBUG_BMAP(PLL_ERROR, b, "odtable_getitem() failed");
		return (rc); // negative errno
	}
	if (bia.bia_fid != fcmh_2_fid(b->bcm_fcmh)) {
		/* XXX release bia? */
		DEBUG_BMAP(PLL_ERROR, b, "different fid="SLPRI_FID,
		   bia.bia_fid);
		return (-1); // errno
	}

	psc_assert(bia.bia_seq == bmi->bmi_seq);
	bia.bia_start = time(NULL);
	bia.bia_seq = bmi->bmi_seq = mds_bmap_timeotbl_mdsi(bml,
	    BTE_ADD);
	bia.bia_lastcli = bml->bml_cli_nidpid;
	bia.bia_flags = dio ? BIAF_DIO : 0;

	mds_odtable_replaceitem(mdsBmapAssignTable,
	    bmi->bmi_assign, &bia, sizeof(bia));

	bml->bml_ios = bia.bia_ios;
	bml->bml_seq = bia.bia_seq;

	if (mds_bmap_add_repl(b, &bia))
		return (-1); // errno

	DEBUG_FCMH(PLL_INFO, b->bcm_fcmh, "bmap update, elem=%zd",
	    bmi->bmi_assign->odtr_elem);

	return (0);
}

/**
 * mds_bmap_dupls_find - Find the first lease of a given client based on
 *	its {nid, pid} pair.  Also walk the chain of duplicate leases to
 *	count the number of read and write leases.  Note that only the
 *	first lease of a client is linked on the bmi->bmi_leases
 *	list, the rest is linked on a private chain and tagged with
 *	BML_CHAIN flag.
 */
static __inline struct bmap_mds_lease *
mds_bmap_dupls_find(struct bmap_mds_info *bmi, lnet_process_id_t *cnp,
    int *wlease, int *rlease)
{
	struct bmap_mds_lease *tmp, *bml = NULL;

	*rlease = 0;
	*wlease = 0;

	PLL_FOREACH(tmp, &bmi->bmi_leases) {
		if (tmp->bml_cli_nidpid.nid != cnp->nid ||
		    tmp->bml_cli_nidpid.pid != cnp->pid)
			continue;
		/* Only one lease per client is allowed on the list. */
		psc_assert(!bml);
		bml = tmp;
	}

	if (!bml)
		return (NULL);

	tmp = bml;
	do {
		/* All dup leases should be chained off the first bml. */
		if (tmp->bml_flags & BML_READ)
			(*rlease)++;
		else
			(*wlease)++;

		DEBUG_BMAP(PLL_INFO, bmi_2_bmap(bmi), "bml=%p tmp=%p "
		    "(wlease=%d rlease=%d) (nwtrs=%d nrdrs=%d)",
		    bml, tmp, *wlease, *rlease,
		    bmi->bmi_writers, bmi->bmi_readers);

		tmp = tmp->bml_chain;
	} while (tmp != bml);

	return (bml);
}

/**
 * mds_bmap_bml_chwrmode - Attempt to upgrade a client-granted bmap
 *	lease from READ-only to READ+WRITE.
 * @bml: bmap lease.
 * @prefios: client's preferred I/O system ID.
 */
int
mds_bmap_bml_chwrmode(struct bmap_mds_lease *bml, sl_ios_id_t prefios)
{
	int rc, wlease, rlease;
	struct bmap_mds_info *bmi;
	struct bmapc_memb *b;

	bmi = bml->bml_bmi;
	b = bmi_2_bmap(bmi);

	bmap_wait_locked(b, b->bcm_flags & BMAP_IONASSIGN);
	BMAP_SETATTR(b, BMAP_IONASSIGN);

	DEBUG_BMAP(PLL_INFO, b, "bml=%p bmi_writers=%d bmi_readers=%d",
	    bml, bmi->bmi_writers, bmi->bmi_readers);

	if (bml->bml_flags & BML_WRITE) {
		rc = -PFLERR_ALREADY;
		goto out;
	}

	rc = mds_bmap_directio_locked(b, SL_WRITE, 0,
	    &bml->bml_cli_nidpid);
	if (rc)
		goto out;

	BMAP_ULOCK(b);

	if (bmi->bmi_wr_ion)
		rc = mds_bmap_ios_update(bml);
	else
		rc = mds_bmap_ios_assign(bml, prefios);

	BMAP_LOCK(b);

	DEBUG_BMAP(PLL_INFO, b, "bml=%p rc=%d "
	    "bmi_writers=%d bmi_readers=%d",
	    bml, rc, bmi->bmi_writers, bmi->bmi_readers);

	if (rc) {
		bml->bml_flags |= BML_ASSFAIL;
		goto out;
	}
	psc_assert(bmi->bmi_wr_ion);

	mds_bmap_dupls_find(bmi, &bml->bml_cli_nidpid, &wlease,
	    &rlease);

	/* Account for the read lease which is to be converted. */
	psc_assert(rlease);
	if (!wlease) {
		/*
		 * Only bump bmi_writers if no other write lease is
		 * still leased to this client.
		 */
		bmi->bmi_writers++;
		bmi->bmi_readers--;
	}
	bml->bml_flags &= ~BML_READ;
	bml->bml_flags |= BML_WRITE;
	OPSTAT_INCR(SLM_OPST_BMAP_CHWRMODE_DONE);

  out:
	BMAP_CLEARATTR(b, BMAP_IONASSIGN);
	bmap_wake_locked(b);
	return (rc);
}

/**
 * mds_bmap_getbml_locked - Obtain the lease handle for a bmap denoted
 *	by the specified issued sequence number.
 * @b: locked bmap.
 * @cli_nid: client network ID.
 * @cli_pid: client network process ID.
 * @seq: lease sequence.
 */
struct bmap_mds_lease *
mds_bmap_getbml_locked(struct bmapc_memb *b, uint64_t seq, uint64_t nid,
    uint32_t pid)
{
	struct bmap_mds_lease *bml, *bml1, *bml2;
	struct bmap_mds_info *bmi;

	bml1 = NULL;
	BMAP_LOCK_ENSURE(b);

	bmi = bmap_2_bmi(b);
	PLL_FOREACH(bml, &bmi->bmi_leases) {
		if (bml->bml_cli_nidpid.nid != nid ||
		    bml->bml_cli_nidpid.pid != pid)
			continue;

		bml2 = bml;
		do {
			if (bml2->bml_seq == seq) {
				/*
				 * A lease won't go away with bmap lock
				 * taken.
				 */
				BML_LOCK(bml2);
				if (!(bml2->bml_flags & BML_FREEING)) {
					bml1 = bml2;
					bml1->bml_refcnt++;
				}
				BML_ULOCK(bml2);
				goto out;
			}

			bml2 = bml2->bml_chain;
		} while (bml != bml2);
	}
 out:
	return (bml1);
}

/**
 * mds_bmap_bml_add - Add a read or write reference to the bmap's tree
 *	and refcnts.  This also calls into the directio_[check|set]
 *	calls depending on the number of read and/or write clients of
 *	this bmap.
 * @bml: bmap lease.
 * @rw: read/write access for bmap.
 * @prefios: client preferred I/O system.
 */
__static int
mds_bmap_bml_add(struct bmap_mds_lease *bml, enum rw rw,
    sl_ios_id_t prefios)
{
	struct bmap_mds_info *bmi = bml->bml_bmi;
	struct bmapc_memb *b = bmi_2_bmap(bmi);
	struct bmap_mds_lease *obml;
	int rlease, wlease, rc = 0;

	psc_assert(bml->bml_cli_nidpid.nid &&
		   bml->bml_cli_nidpid.pid &&
		   bml->bml_cli_nidpid.nid != LNET_NID_ANY &&
		   bml->bml_cli_nidpid.pid != LNET_PID_ANY);

	BMAP_LOCK(b);
	/* Wait for BMAP_IONASSIGN to be removed before proceeding. */
	bmap_wait_locked(b, (b->bcm_flags & BMAP_IONASSIGN));
	bmap_op_start_type(b, BMAP_OPCNT_LEASE);

	rc = mds_bmap_directio_locked(b, rw, bml->bml_flags & BML_DIO,
		&bml->bml_cli_nidpid);
	if (rc && !(bml->bml_flags & BML_RECOVER))
		/* 'rc != 0' means that we're waiting on an async cb
		 *    completion.
		 */
		goto out;

	obml = mds_bmap_dupls_find(bmi, &bml->bml_cli_nidpid, &wlease,
	    &rlease);

	DEBUG_BMAP(PLL_INFO, b, "bml=%p obml=%p (wlease=%d rlease=%d) "
	    "(nwtrs=%d nrdrs=%d)", bml, obml, wlease, rlease,
	    bmi->bmi_writers, bmi->bmi_readers);

	if (obml) {
		struct bmap_mds_lease *tmp = obml;

		bml->bml_flags |= BML_CHAIN;
		/* Add ourselves to the end. */
		while (tmp->bml_chain != obml)
			tmp = tmp->bml_chain;

		tmp->bml_chain = bml;
		bml->bml_chain = obml;
		psc_assert(psclist_disjoint(&bml->bml_bmi_lentry));
	} else {
		/* First on the list. */
		bml->bml_chain = bml;
		pll_addtail(&bmi->bmi_leases, bml);
	}

	bml->bml_flags |= BML_BMI;

	if (rw == SL_WRITE) {
		/* Drop the lock prior to doing disk and possibly network
		 *    I/O.
		 */
		b->bcm_flags |= BMAP_IONASSIGN;

		/* For any given chain of leases, the bmi_[readers|writers]
		 *    value may only be 1rd or 1wr.  In the case where 2
		 *    wtrs are present, the value is 1wr.  Mixed readers and
		 *    wtrs == 1wtr.  1-N rdrs, 1rd.
		 * Only increment writers if this is the first
		 *    write lease from the respective client.
		 */
		if (!wlease) {
			/* This is the first write from the client. */
			bmi->bmi_writers++;

			if (rlease)
				/*
				 * Remove the read cnt, it has been
				 * superseded by the write.
				 */
				bmi->bmi_readers--;
		}

		if (bml->bml_flags & BML_RECOVER) {
			psc_assert(bmi->bmi_writers == 1);
			psc_assert(!bmi->bmi_readers);
			psc_assert(!bmi->bmi_wr_ion);
			psc_assert(bml->bml_ios &&
			    bml->bml_ios != IOS_ID_ANY);
			BMAP_ULOCK(b);
			rc = mds_bmap_ios_restart(bml);
			bml->bml_flags &= ~BML_RECOVER;

		} else if (!wlease && bmi->bmi_writers == 1) {
			/* No duplicate lease detected and this client
			 * is the first writer.
			 */
			psc_assert(!bmi->bmi_wr_ion);
			BMAP_ULOCK(b);
			rc = mds_bmap_ios_assign(bml, prefios);

		} else {
			/* Possible duplicate and/or multiple writer. */
			psc_assert(bmi->bmi_wr_ion);
			BMAP_ULOCK(b);
			rc = mds_bmap_ios_update(bml);
		}

		BMAP_LOCK(b);
		b->bcm_flags &= ~BMAP_IONASSIGN;

	} else { //rw == SL_READ
		bml->bml_seq = mds_bmap_timeotbl_getnextseq();

		if (!wlease && !rlease)
			bmi->bmi_readers++;
		mds_bmap_timeotbl_mdsi(bml, BTE_ADD);
	}

 out:
	DEBUG_BMAP(rc && rc != -SLERR_BMAP_DIOWAIT ? PLL_WARN : PLL_INFO,
	    b, "bml_add (mion=%p) bml=%p (seq=%"PRId64") (rw=%d) "
	    "(nwtrs=%d nrdrs=%d) (rc=%d)",
	    bmi->bmi_wr_ion, bml, bml->bml_seq, rw,
	    bmi->bmi_writers, bmi->bmi_readers, rc);

	bmap_wake_locked(b);
	BMAP_ULOCK(b);

	/*
	 * On error, the caller will issue mds_bmap_bml_release() which
	 * deals with the gory details of freeing a fullly, or
	 * partially, instantiated bml.  Therefore, BMAP_OPCNT_LEASE will
	 * not be removed in the case of an error.
	 */
	return (rc);
}

static void
mds_bmap_bml_del_locked(struct bmap_mds_lease *bml)
{
	struct bmap_mds_info *bmi = bml->bml_bmi;
	struct bmap_mds_lease *obml, *tail;
	int rlease = 0, wlease = 0;

	BMAP_LOCK_ENSURE(bmi_2_bmap(bmi));
	BML_LOCK_ENSURE(bml);

	obml = mds_bmap_dupls_find(bmi, &bml->bml_cli_nidpid, &wlease,
	    &rlease);

	/*
	 * obml must be not NULL because at least the lease being freed
	 * must be present in the list.  Therefore lease cnt must be
	 * positive.   Also note that the find() function returns the
	 * head of the chain of duplicate leases.
	 */
	psc_assert(obml);
	psc_assert((wlease + rlease) > 0);
	psc_assert(!(obml->bml_flags & BML_CHAIN));
	psc_assert(psclist_conjoint(&obml->bml_bmi_lentry,
	    psc_lentry_hd(&obml->bml_bmi_lentry)));

	/* Find the bml's preceeding entry. */
	tail = obml;
	while (tail->bml_chain != bml)
		tail = tail->bml_chain;
	psc_assert(tail->bml_chain == bml);

	/* Manage the bml list and bml_bmds_lentry. */
	if (bml->bml_flags & BML_CHAIN) {
		psc_assert(psclist_disjoint(&bml->bml_bmi_lentry));
		psc_assert((wlease + rlease) > 1);
		tail->bml_chain = bml->bml_chain;

	} else {
		psc_assert(obml == bml);
		psc_assert(!(bml->bml_flags & BML_CHAIN));
		pll_remove(&bmi->bmi_leases, bml);

		if ((wlease + rlease) > 1) {
			psc_assert(bml->bml_chain->bml_flags & BML_CHAIN);
			psc_assert(psclist_disjoint(
			    &bml->bml_chain->bml_bmi_lentry));

			bml->bml_chain->bml_flags &= ~BML_CHAIN;
			pll_addtail(&bmi->bmi_leases, bml->bml_chain);

			tail->bml_chain = bml->bml_chain;
		} else
			psc_assert(bml == bml->bml_chain);
	}

	if (bml->bml_flags & BML_WRITE) {
		if (wlease == 1) {
			psc_assert(bmi->bmi_writers > 0);
			bmi->bmi_writers--;

			DEBUG_BMAP(PLL_INFO, bmi_2_bmap(bmi),
			   "bml=%p bmi_writers=%d bmi_readers=%d",
			   bml, bmi->bmi_writers, bmi->bmi_readers);

			if (rlease)
				bmi->bmi_readers++;
		}
	} else {
		psc_assert(bml->bml_flags & BML_READ);
		if (!wlease && (rlease == 1)) {
			psc_assert(bmi->bmi_readers > 0);
			bmi->bmi_readers--;
		}
	}
}

/**
 * mds_bmap_bml_release - Remove a bmap lease from the MDS.  This can be
 *	called from the bmap_timeo thread, from a client bmap_release
 *	RPC, or from the nbreqset cb context.
 * @bml: bmap lease.
 * Notes:  the bml must be removed from the timeotbl in all cases.
 *    otherwise we determine list removals on a case by case basis.
 */
int
mds_bmap_bml_release(struct bmap_mds_lease *bml)
{
	struct bmapc_memb *b = bml_2_bmap(bml);
	struct bmap_mds_info *bmi = bml->bml_bmi;
	struct slmds_jent_assign_rep *logentry;
	struct odtable_receipt *odtr = NULL;
	struct fidc_membh *f = b->bcm_fcmh;
	uint64_t key;
	size_t elem;
	int rc = 0;

	/* On the last release, BML_FREEING must be set. */
	psc_assert(psc_atomic32_read(&b->bcm_opcnt) > 0);

	DEBUG_BMAP(PLL_INFO, b, "bml=%p fl=%d seq=%"PRId64, bml,
		   bml->bml_flags, bml->bml_seq);

	/*
	 * BMAP_IONASSIGN acts as a barrier for operations which may
	 * modify bmi_wr_ion.  Since ops associated with
	 * BMAP_IONASSIGN do disk and net I/O, the spinlock is dropped.
	 *
	 * XXX actually, the bcm_lock is not dropped until the very end.
	 * If this becomes problematic we should investigate more.
	 * ATM the BMAP_IONASSIGN is not relied upon
	 */
	(void)BMAP_RLOCK(b);
	bmap_wait_locked(b, (b->bcm_flags & BMAP_IONASSIGN));
	b->bcm_flags |= BMAP_IONASSIGN;

	BML_LOCK(bml);
	if (bml->bml_refcnt > 1 || !(bml->bml_flags & BML_FREEING)) {
		psc_assert(bml->bml_refcnt > 0);
		bml->bml_refcnt--;
		BML_ULOCK(bml);
		b->bcm_flags &= ~BMAP_IONASSIGN;
		bmap_wake_locked(b);
		BMAP_ULOCK(b);
		return (0);
	}

	/*
	 * While holding the last reference to the lease, take the lease
	 * off the timeout list to avoid a race with the timeout thread.
	 */
	if (bml->bml_flags & BML_TIMEOQ) {
		BML_ULOCK(bml);
		mds_bmap_timeotbl_mdsi(bml, BTE_DEL);
		BML_LOCK(bml);
	}

	/*
	 * If I am called by the timeout thread, then the refcnt is
	 * zero.
	 */
	psc_assert(bml->bml_refcnt <= 1);
	if (!(bml->bml_flags & BML_BMI)) {
		BML_ULOCK(bml);
		goto out;
	}

	mds_bmap_bml_del_locked(bml);
	bml->bml_flags &= ~BML_BMI;

	BML_ULOCK(bml);

	if ((b->bcm_flags & (BMAP_DIO | BMAP_DIOCB)) &&
	    (!bmi->bmi_writers ||
	     (bmi->bmi_writers == 1 && !bmi->bmi_readers))) {
		/* Remove the directio flag if possible. */
		b->bcm_flags &= ~(BMAP_DIO | BMAP_DIOCB);
		OPSTAT_INCR(SLM_OPST_BMAP_DIO_CLR);
	}

	/*
	 * Only release the odtable entry if the key matches.  If a
	 * match is found then verify the sequence number matches.
	 */
	if ((bml->bml_flags & BML_WRITE) && !bmi->bmi_writers) {
		int retifset[NBREPLST];

		if (b->bcm_flags & BMAP_MDS_NOION) {
			psc_assert(!bmi->bmi_assign);
			psc_assert(!bmi->bmi_wr_ion);
			goto out;
		}

		/*
		 * Bml's which have failed ion assignment shouldn't be
		 * relevant to any odtable entry.
		 */
		if (bml->bml_flags & BML_ASSFAIL)
			goto out;

		if (!(bml->bml_flags & BML_RECOVERFAIL)) {
			struct bmap_ios_assign bia;

			rc = mds_odtable_getitem(mdsBmapAssignTable,
			    bmi->bmi_assign, &bia, sizeof(bia));
			psc_assert(!rc && bia.bia_seq == bmi->bmi_seq);
			psc_assert(bia.bia_bmapno == b->bcm_bmapno);
			/* End sanity checks. */
			odtr = bmi->bmi_assign;
			bmi->bmi_assign = NULL;
		} else {
			psc_assert(!bmi->bmi_assign);
		}
		atomic_dec(&bmi->bmi_wr_ion->rmmi_refcnt);
		bmi->bmi_wr_ion = NULL;

		/*
		 * Check if any replication work is ready and queue it
		 * up.
		 */
		brepls_init(retifset, 0);
		retifset[BREPLST_REPL_QUEUED] = 1;
		retifset[BREPLST_TRUNCPNDG] = 1;
		if (mds_repl_bmap_walk_all(b, NULL, retifset,
		    REPL_WALKF_SCIRCUIT)) {
			struct slm_update_data *upd;
			int retifset[NBREPLST];

			if (!FCMH_HAS_BUSY(f))
				BMAP_ULOCK(b);
			FCMH_WAIT_BUSY(f);
			BMAP_WAIT_BUSY(b);
			BMAPOD_RDLOCK(bmi);

			brepls_init(retifset, 0);
			retifset[BREPLST_REPL_QUEUED] = 1;
			retifset[BREPLST_TRUNCPNDG] = 1;

			upd = &bmi->bmi_upd;
			UPD_WAIT(upd);
			if (fcmh_2_nrepls(f) > SL_DEF_REPLICAS)
				mds_inox_ensure_loaded(fcmh_2_inoh(f));
			if (mds_repl_bmap_walk_all(b, NULL, retifset,
			    REPL_WALKF_SCIRCUIT))
				upsch_enqueue(upd);
			UPD_UNBUSY(upd);
			BMAPOD_ULOCK(bmi);
			BMAP_UNBUSY(b);
			FCMH_UNBUSY(f);

			BMAP_LOCK(b);
		}
	}

 out:
	b->bcm_flags &= ~BMAP_IONASSIGN;
	bmap_wake_locked(b);
	bmap_op_done_type(b, BMAP_OPCNT_LEASE);

	if (odtr) {
		key = odtr->odtr_key;
		elem = odtr->odtr_elem;
		rc = mds_odtable_freeitem(mdsBmapAssignTable, odtr);
		DEBUG_BMAP(PLL_DIAG, b, "odtable remove seq=%"PRId64" "
		    "key=%#"PRIx64" rc=%d", bml->bml_seq, key, rc);
		bmap_op_done_type(b, BMAP_OPCNT_IONASSIGN);

		mds_reserve_slot(1);
		logentry = pjournal_get_buf(mdsJournal,
		    sizeof(*logentry));
		logentry->sjar_elem = elem;
		logentry->sjar_flags = SLJ_ASSIGN_REP_FREE;
		pjournal_add_entry(mdsJournal, 0, MDS_LOG_BMAP_ASSIGN,
		    0, logentry, sizeof(*logentry));
		pjournal_put_buf(mdsJournal, logentry);
		mds_unreserve_slot(1);
	}

	psc_pool_return(bmapMdsLeasePool, bml);

	return (rc);
}

/**
 * mds_handle_rls_bmap - Handle SRMT_RELEASEBMAP RPC from a client or an
 *	I/O server.
 */
int
mds_handle_rls_bmap(struct pscrpc_request *rq, __unusedx int sliod)
{
	struct srm_bmap_release_req *mq;
	struct srm_bmap_release_rep *mp;
	struct bmap_mds_lease *bml;
	struct srt_bmapdesc *sbd;
	struct slash_fidgen fg;
	struct fidc_membh *f;
	struct bmapc_memb *b;
	uint32_t i;

	SL_RSX_ALLOCREP(rq, mq, mp);
	if (mq->nbmaps > MAX_BMAP_RELEASE) {
		mp->rc = -EINVAL;
		return (0);
	}

	for (i = 0; i < mq->nbmaps; i++) {
		sbd = &mq->sbd[i];

		fg.fg_fid = sbd->sbd_fg.fg_fid;
		fg.fg_gen = 0;

		if (slm_fcmh_get(&fg, &f))
			continue;

		DEBUG_FCMH(PLL_INFO, f, "rls bmap=%u", sbd->sbd_bmapno);

		if (bmap_lookup(f, sbd->sbd_bmapno, &b))
			goto next;

		BMAP_LOCK(b);
		bml = mds_bmap_getbml_locked(b, sbd->sbd_seq,
		    sbd->sbd_nid, sbd->sbd_pid);

		DEBUG_BMAP((bml ? PLL_INFO : PLL_WARN), b,
		    "release %"PRId64" nid=%"PRId64" pid=%u bml=%p",
		    sbd->sbd_seq, sbd->sbd_nid, sbd->sbd_pid, bml);
		if (bml) {
			BML_LOCK(bml);
			bml->bml_flags |= BML_FREEING;
			BML_ULOCK(bml);
			mds_bmap_bml_release(bml);
		}
		bmap_op_done(b);
 next:
		fcmh_op_done(f);
	}
	return (0);
}

static struct bmap_mds_lease *
mds_bml_new(struct bmapc_memb *b, struct pscrpc_export *e, int flags,
    lnet_process_id_t *cnp)
{
	struct bmap_mds_lease *bml;

	bml = psc_pool_get(bmapMdsLeasePool);
	memset(bml, 0, sizeof(*bml));

	INIT_PSC_LISTENTRY(&bml->bml_bmi_lentry);
	INIT_PSC_LISTENTRY(&bml->bml_timeo_lentry);
	INIT_SPINLOCK(&bml->bml_lock);

	bml->bml_exp = e;
	bml->bml_refcnt = 1;
	bml->bml_bmi = bmap_2_bmi(b);
	bml->bml_flags = flags;
	bml->bml_cli_nidpid = *cnp;
	bml->bml_start = time(NULL);
	bml->bml_expire = bml->bml_start + BMAP_TIMEO_MAX;

	return (bml);
}

int
mds_bia_odtable_startup_cb(void *data, struct odtable_receipt *odtr,
    __unusedx void *arg)
{
	struct bmap_ios_assign *bia = data;
	struct fidc_membh *f = NULL;
	struct bmapc_memb *b = NULL;
	struct bmap_mds_lease *bml;
	struct slash_fidgen fg;
	struct sl_resm *resm;
	int rc;

	resm = libsl_ios2resm(bia->bia_ios);

	psclog_debug("fid="SLPRI_FID" seq=%"PRId64" res=(%s) bmapno=%u",
	    bia->bia_fid, bia->bia_seq, resm->resm_res->res_name,
	    bia->bia_bmapno);

	if (!bia->bia_fid) {
		psclog_warnx("found fid #0 in odtable");
		PFL_GOTOERR(out, rc = -EINVAL);
	}

	fg.fg_fid = bia->bia_fid;
	fg.fg_gen = FGEN_ANY;

	/*
	 * Because we don't revoke leases when an unlink comes in,
	 * ENOENT is actually legitimate after a crash.
	 */
	rc = slm_fcmh_get(&fg, &f);
	if (rc) {
		psclog_errorx("failed to load: item=%zd, fid="SLPRI_FID,
		    odtr->odtr_elem, fg.fg_fid);
		PFL_GOTOERR(out, rc);
	}

	rc = bmap_get(f, bia->bia_bmapno, SL_WRITE, &b);
	if (rc) {
		DEBUG_FCMH(PLL_ERROR, f, "failed to load bmap %u (rc=%d)",
		    bia->bia_bmapno, rc);
		PFL_GOTOERR(out, rc);
	}

	bml = mds_bml_new(b, NULL, BML_WRITE | BML_RECOVER,
	    &bia->bia_lastcli);

	bml->bml_seq = bia->bia_seq;
	bml->bml_ios = bia->bia_ios;

	/*
	 * Taking the lease origination time in this manner leaves us
	 * susceptible to gross changes in the system time.
	 */
	bml->bml_start = bia->bia_start;

	/* Grant recovered leases some additional time. */
	bml->bml_expire = time(NULL) + BMAP_RECOVERY_TIMEO_EXT;

	if (bia->bia_flags & BIAF_DIO)
		b->bcm_flags |= BMAP_DIO;

	bmap_2_bmi(b)->bmi_assign = odtr;

	rc = mds_bmap_bml_add(bml, SL_WRITE, IOS_ID_ANY);
	if (rc) {
		bmap_2_bmi(b)->bmi_assign = NULL;
		bml->bml_flags |= (BML_FREEING | BML_RECOVERFAIL);
	}
	mds_bmap_bml_release(bml);

 out:
	if (rc)
		mds_odtable_freeitem(mdsBmapAssignTable, odtr);
	if (b)
		bmap_op_done(b);
	if (f)
		fcmh_op_done(f);
	return (0);
}

/**
 * mds_bmap_crc_write - Process a CRC update request from an ION.
 * @c: the RPC request containing the FID, bmapno, and chunk ID (cid).
 * @iosid:  the IOS ID of the I/O node which sent the request.  It is
 *	compared against the ID stored in the bml
 */
int
mds_bmap_crc_write(struct srm_bmap_crcup *c, sl_ios_id_t iosid,
    const struct srm_bmap_crcwrt_req *mq)
{
	struct sl_resource *res = libsl_id2res(iosid);
	struct bmapc_memb *bmap = NULL;
	struct bmap_mds_info *bmi;
	struct fidc_membh *f;
	int rc, vfsid;

	rc = slfid_to_vfsid(c->fg.fg_fid, &vfsid);
	if (rc)
		return (rc);
	if (vfsid != current_vfsid)
		return (-EINVAL);

	rc = slm_fcmh_get(&c->fg, &f);
	if (rc) {
		if (rc == ENOENT) {
			psclog_warnx("fid="SLPRI_FID" appears to have "
			    "been deleted", c->fg.fg_fid);
			return (0);
		}
		psclog_errorx("fid="SLPRI_FID" slm_fcmh_get() rc=%d",
		    c->fg.fg_fid, rc);
		return (-rc);
	}

	/*
	 * Ignore updates from old or invalid generation numbers.
	 * XXX XXX fcmh is not locked here
	 */
	FCMH_LOCK(f);
	if (fcmh_2_gen(f) != c->fg.fg_gen) {
		int x = (fcmh_2_gen(f) > c->fg.fg_gen) ? 1 : 0;

		DEBUG_FCMH(x ? PLL_INFO : PLL_ERROR, f,
		    "MDS gen (%"PRIu64") %s than crcup gen (%"PRIu64")",
		    fcmh_2_gen(f), x ? ">" : "<", c->fg.fg_gen);

		rc = -(x ? SLERR_GEN_OLD : SLERR_GEN_INVALID);
		FCMH_ULOCK(f);
		goto out;
	}
	FCMH_ULOCK(f);

	/*
	 * BMAP_OP #2
	 * XXX are we sure after restart bmap will be loaded?
	 */
	rc = bmap_lookup(f, c->blkno, &bmap);
	if (rc) {
		DEBUG_FCMH(PLL_ERROR, f, "failed lookup bmap(%u) rc=%d",
		    c->blkno, rc);
		rc = -EBADF;
		goto out;
	}
	BMAP_LOCK(bmap);

	DEBUG_BMAP(PLL_INFO, bmap, "blkno=%u sz=%"PRId64" ios(%s)",
	    c->blkno, c->fsize, res->res_name);

	psc_assert(psc_atomic32_read(&bmap->bcm_opcnt) > 1);

	bmi = bmap_2_bmi(bmap);
	/* These better check out. */
	psc_assert(bmap->bcm_fcmh == f);
	psc_assert(bmi);

	if (!bmi->bmi_wr_ion ||
	    iosid != rmmi2resm(bmi->bmi_wr_ion)->resm_res_id) {
		/* We recv'd a request from an unexpected NID. */
		psclog_errorx("CRCUP for/from invalid NID; "
		    "wr_ion=%s ios=%#x",
		    bmi->bmi_wr_ion ?
		    rmmi2resm(bmi->bmi_wr_ion)->resm_name : "<NONE>",
		    iosid);

		BMAP_ULOCK(bmap);
		PFL_GOTOERR(out, rc = -EINVAL);
	}

	if (bmap->bcm_flags & BMAP_MDS_CRC_UP) {
		/*
		 * Ensure that this thread is the only thread updating
		 * the bmap CRC table.
		 * XXX may have to replace this with a waitq
		 */

		DEBUG_BMAP(PLL_ERROR, bmap,
		    "EALREADY blkno=%u sz=%"PRId64" ios(%s)",
		    c->blkno, c->fsize, res->res_name);

		DEBUG_FCMH(PLL_ERROR, f,
		    "EALREADY blkno=%u sz=%"PRId64" ios(%s)",
		    c->blkno, c->fsize, res->res_name);

		BMAP_ULOCK(bmap);
		PFL_GOTOERR(out, rc = -PFLERR_ALREADY);
	}

	/*
	 * Mark that bmap is undergoing CRC updates - this is
	 * non-reentrant so the ION must know better than to
	 * send multiple requests for the same bmap.
	 */
	bmap->bcm_flags |= BMAP_MDS_CRC_UP;
	BMAP_ULOCK(bmap);

	/* Call the journal and update the in-memory CRCs. */
	rc = mds_bmap_crc_update(bmap, iosid, c);

	if (mq->flags & SRM_BMAPCRCWRT_PTRUNC) {
		struct slash_inode_handle *ih;
		struct slm_wkdata_ptrunc *wk;
		int iosidx, tract[NBREPLST];
		uint32_t bpol;

		ih = fcmh_2_inoh(f);
		iosidx = mds_repl_ios_lookup(vfsid, ih, iosid);
		if (iosidx < 0)
			psclog_errorx("ios not found");
		else {
			BMAPOD_MODIFY_START(bmap);

			brepls_init(tract, -1);
			tract[BREPLST_TRUNCPNDG] = BREPLST_VALID;
			tract[BREPLST_TRUNCPNDG_SCHED] = BREPLST_VALID;
			mds_repl_bmap_apply(bmap, tract, NULL,
			    SL_BITS_PER_REPLICA * iosidx);

			BHREPL_POLICY_GET(bmap, &bpol);

			brepls_init(tract, -1);
			tract[BREPLST_TRUNCPNDG] = bpol == BRPOL_PERSIST ?
			    BREPLST_REPL_QUEUED : BREPLST_GARBAGE;
			mds_repl_bmap_walk(bmap, tract, NULL,
			    REPL_WALKF_MODOTH, &iosidx, 1);

			// XXX write bmap!!!
			BMAPOD_MODIFY_DONE(bmap, 0);

			/*
			 * XXX modify all bmaps after this one and mark
			 * them INVALID since the sliod will have zeroed
			 * those regions.
			 */
			UPD_WAKE(&bmi->bmi_upd);
		}

		FCMH_LOCK(f);
		f->fcmh_flags &= ~FCMH_IN_PTRUNC;
		fcmh_wake_locked(f);
		FCMH_ULOCK(f);

		wk = pfl_workq_getitem(slm_ptrunc_wake_clients,
		    struct slm_wkdata_ptrunc);
		fcmh_op_start_type(f, FCMH_OPCNT_WORKER);
		wk->f = f;
		pfl_workq_putitem(wk);
	}

	if (f->fcmh_sstb.sst_mode & (S_ISGID | S_ISUID)) {
		struct srt_stat sstb;

		FCMH_WAIT_BUSY(f);
		sstb.sst_mode = f->fcmh_sstb.sst_mode &
		    ~(S_ISGID | S_ISUID);
		mds_fcmh_setattr_nolog(vfsid, f, PSCFS_SETATTRF_MODE,
		    &sstb);
		FCMH_UNBUSY(f);
	}

 out:
	/*
	 * Mark that mds_bmap_crc_write() is done with this bmap
	 *  - it was incref'd in fcmh_bmap_lookup().
	 */
	if (bmap)
		/* BMAP_OP #2, drop lookup ref */
		bmap_op_done(bmap);

	fcmh_op_done(f);
	return (rc);
}

/**
 * mds_bmap_loadvalid - Load a bmap if disk I/O is successful and the
 *	bmap has been initialized (i.e. is not all zeroes).
 * @f: fcmh.
 * @bmapno: bmap index number to load.
 * @bp: value-result bmap pointer.
 * NOTE: callers must issue bmap_op_done() if mds_bmap_loadvalid() is
 *	successful.
 */
int
mds_bmap_loadvalid(struct fidc_membh *f, sl_bmapno_t bmapno,
    struct bmapc_memb **bp)
{
	struct bmapc_memb *b;
	int n, rc;

	*bp = NULL;

	/* BMAP_OP #3 via lookup */
	rc = bmap_get(f, bmapno, SL_WRITE, &b);
	if (rc)
		return (rc);

	BMAP_LOCK(b);
	for (n = 0; n < SLASH_CRCS_PER_BMAP; n++)
		/*
		 * XXX need a bitmap to see which CRCs are
		 * actually uninitialized and not just happen
		 * to be zero.
		 */
		if (b->bcm_crcstates[n]) {
			BMAP_ULOCK(b);
			*bp = b;
			return (0);
		}

	/* BMAP_OP #3, unref if bmap is empty.
	 *    NOTE that our callers must drop this ref.
	 */
	bmap_op_done(b);
	return (SLERR_BMAP_ZERO);
}

int
mds_bmap_load_fg(const struct slash_fidgen *fg, sl_bmapno_t bmapno,
    struct bmapc_memb **bp)
{
	struct fidc_membh *f;
	struct bmapc_memb *b;
	int rc = 0;

	psc_assert(*bp == NULL);

	f = fidc_lookup_fg(fg);
	if (!f)
		return (-ENOENT);

	rc = bmap_get(f, bmapno, SL_WRITE,&b);
	if (rc == 0)
		*bp = b;

	fcmh_op_done(f);
	return (rc);
}

/**
 * mds_bmap_load_cli - Routine called to retrieve a bmap, presumably so
 *	that it may be sent to a client.  It first checks for existence
 *	in the cache otherwise the bmap is retrieved from disk.
 *
 *	mds_bmap_load_cli() also manages the bmap_lease reference
 *	which is used to track the bmaps a particular client knows
 *	about.  mds_bmap_read() is used to retrieve the bmap from disk
 *	or create a new 'blank-slate' bmap if one does not exist.
 *	Finally, a read or write reference is placed on the bmap
 *	depending on the client request.  This is factored in with
 *	existing references to determine whether or not the bmap should
 *	be in DIO mode.
 * @f: the FID cache handle for the inode.
 * @bmapno: bmap index number.
 * @flags: bmap lease flags (BML_*).
 * @rw: read/write access to the bmap.
 * @prefios: client preferred I/O system ID.
 * @sbd: value-result bmap descriptor to pass back to client.
 * @exp: RPC export to client.
 * @bmap: value-result bmap.
 * Note: the bmap is not locked during disk I/O; instead it is marked
 *	with a bit (i.e. INIT) and other threads block on its waitq.
 */
int
mds_bmap_load_cli(struct fidc_membh *f, sl_bmapno_t bmapno, int flags,
    enum rw rw, sl_ios_id_t prefios, struct srt_bmapdesc *sbd,
    struct pscrpc_export *exp, struct bmap_core_state *bcs)
{
	struct slashrpc_cservice *csvc;
	struct bmap_mds_lease *bml;
	struct bmapc_memb *b;
	int rc;

	FCMH_LOCK(f);
	rc = (f->fcmh_flags & FCMH_IN_PTRUNC) &&
	    bmapno >= fcmh_2_fsz(f) / SLASH_BMAP_SIZE;
	FCMH_ULOCK(f);
	if (rc) {
		csvc = slm_getclcsvc(exp);
		FCMH_LOCK(f);
		if (csvc && (f->fcmh_flags & FCMH_IN_PTRUNC)) {
			psc_dynarray_add(&fcmh_2_fmi(f)->fmi_ptrunc_clients,
			    csvc);
			FCMH_ULOCK(f);
			return (SLERR_BMAP_IN_PTRUNC);
		}
		FCMH_ULOCK(f);
	}

	rc = bmap_get(f, bmapno, SL_WRITE, &b);
	if (rc)
		return (rc);

	bml = mds_bml_new(b, exp,
	    ((rw == SL_WRITE ? BML_WRITE : BML_READ) |
	     (flags & SRM_LEASEBMAPF_DIRECTIO ? BML_DIO : 0)),
	    &exp->exp_connection->c_peer);

	rc = mds_bmap_bml_add(bml, rw, prefios);
	if (rc) {
		BML_LOCK(bml);
		bml->bml_flags |= BML_FREEING;
		if (rc == -SLERR_ION_OFFLINE)
			bml->bml_flags |= BML_ASSFAIL;
		BML_ULOCK(bml);
		goto out;
	}
	sbd->sbd_fg = f->fcmh_fg;
	/*
	 * SLASH2 monotonic coherency sequence number assigned to this
	 * lease.
	 */
	sbd->sbd_seq = bml->bml_seq;

	/* Stash the odtable key if this is a write lease. */
	sbd->sbd_key = (rw == SL_WRITE) ?
	    bml->bml_bmi->bmi_assign->odtr_key : BMAPSEQ_ANY;

	/*
	 * Store the nid/pid of the client interface in the bmapdesc to
	 * deal properly deal with IONs on other LNETs.
	 */
	sbd->sbd_nid = exp->exp_connection->c_peer.nid;
	sbd->sbd_pid = exp->exp_connection->c_peer.pid;

	if (rw == SL_WRITE) {
		struct bmap_mds_info *bmi = bmap_2_bmi(b);

		psc_assert(bmi->bmi_wr_ion);
		sbd->sbd_ios = rmmi2resm(bmi->bmi_wr_ion)->resm_res_id;
	} else
		sbd->sbd_ios = IOS_ID_ANY;

	sbd->sbd_bmapno = bmapno;
	if (b->bcm_flags & BMAP_DIO)
		sbd->sbd_flags |= SRM_LEASEBMAPF_DIRECTIO;

	if (bcs)
		memcpy(bcs, &b->bcm_corestate, sizeof(*bcs));
 out:
	mds_bmap_bml_release(bml);
	bmap_op_done(b);
	return (rc);
}

int
mds_lease_reassign(struct fidc_membh *f, struct srt_bmapdesc *sbd_in,
    sl_ios_id_t pios, sl_ios_id_t *prev_ios, int nprev_ios,
    struct srt_bmapdesc *sbd_out, struct pscrpc_export *exp)
{
	struct bmapc_memb *b;
	struct bmap_mds_lease *obml;
	struct bmap_mds_info *bmi;
	struct bmap_ios_assign bia;
	struct sl_resm *resm;
	int rc;

	rc = bmap_get(f, sbd_in->sbd_bmapno, SL_WRITE, &b);
	if (rc)
		return (rc);

	BMAP_LOCK(b);
	obml = mds_bmap_getbml_locked(b, sbd_in->sbd_seq,
	    sbd_in->sbd_nid, sbd_in->sbd_pid);

	if (!obml) {
		PFL_GOTOERR(out2, rc = -ENOENT);

	} else if (!(obml->bml_flags & BML_WRITE)) {
		PFL_GOTOERR(out2, rc = -EINVAL);
	}

	bmap_wait_locked(b, b->bcm_flags & BMAP_IONASSIGN);
	/*
	 * Set BMAP_IONASSIGN before checking the lease counts since
	 * BMAP_IONASSIGN will block further lease additions and
	 * removals
	 *   - including the removal this lease's odtable entry.
	 */
	BMAP_SETATTR(b, BMAP_IONASSIGN);

	bmi = bmap_2_bmi(b);
	if (bmi->bmi_writers > 1 || bmi->bmi_readers) {
		/*
		 * Other clients have been assigned this sliod.
		 * Therefore the sliod may not be reassigned.
		 */
		PFL_GOTOERR(out1, rc = -EAGAIN);
	}
	psc_assert(bmi->bmi_wr_ion);
	psc_assert(!(b->bcm_flags & BMAP_DIO));
	BMAP_ULOCK(b);

	rc = mds_odtable_getitem(mdsBmapAssignTable,
	    bmi->bmi_assign, &bia, sizeof(bia));
	if (rc) {
		DEBUG_BMAP(PLL_ERROR, b, "odtable_getitem() failed");
		goto out1;
	}
	psc_assert(bia.bia_seq == bmi->bmi_seq);

	resm = mds_resm_select(b, pios, prev_ios, nprev_ios);
	if (!resm)
		PFL_GOTOERR(out1, rc = -SLERR_ION_OFFLINE);

	/*
	 * Deal with the lease renewal and repl_add before modifying the
	 * IOS part of the lease or bmi so that mds_bmap_add_repl()
	 * failure doesn't compromise the existing lease.
	 */
	bia.bia_seq = mds_bmap_timeotbl_mdsi(obml, BTE_ADD);
	bia.bia_lastcli = obml->bml_cli_nidpid;
	bia.bia_ios = resm->resm_res_id;
	bia.bia_start = time(NULL);

	if ((rc = mds_bmap_add_repl(b, &bia)))
		goto out1;

	bmi->bmi_seq = obml->bml_seq = bia.bia_seq;
	obml->bml_ios = resm->resm_res_id;

	mds_odtable_replaceitem(mdsBmapAssignTable,
	    bmi->bmi_assign, &bia, sizeof(bia));

	/* Do some post setup on the modified lease. */
	sbd_out->sbd_seq = obml->bml_seq;
	sbd_out->sbd_bmapno = b->bcm_bmapno;
	sbd_out->sbd_fg = b->bcm_fcmh->fcmh_fg;
	sbd_out->sbd_nid = exp->exp_connection->c_peer.nid;
	sbd_out->sbd_pid = exp->exp_connection->c_peer.pid;
	sbd_out->sbd_key = obml->bml_bmi->bmi_assign->odtr_key;
	sbd_out->sbd_ios = obml->bml_ios;

 out1:
	(void)BMAP_RLOCK(b);
	psc_assert(b->bcm_flags & BMAP_IONASSIGN);
	BMAP_CLEARATTR(b, BMAP_IONASSIGN);
	bmap_wake_locked(b);

 out2:
	if (obml)
		mds_bmap_bml_release(obml);
	DEBUG_BMAP(rc ? PLL_WARN : PLL_INFO, b,
	    "rc=%d renew oseq=%"PRIu64" nseq=%"PRIu64" "
	    "nid=%"PRIu64" pid=%u",
	    rc, sbd_in->sbd_seq, (obml ? obml->bml_seq : 0),
	    exp->exp_connection->c_peer.nid,
	    exp->exp_connection->c_peer.pid);
	bmap_op_done(b);
	return (rc);
}

int
mds_lease_renew(struct fidc_membh *f, struct srt_bmapdesc *sbd_in,
    struct srt_bmapdesc *sbd_out, struct pscrpc_export *exp)
{
	struct bmap_mds_lease *bml = NULL, *obml;
	struct bmapc_memb *b;
	int rc, rw;

	rc = bmap_get(f, sbd_in->sbd_bmapno, SL_WRITE, &b);
	if (rc)
		return (rc);

	/* Lookup the original lease to ensure it actually exists. */
	BMAP_LOCK(b);
	obml = mds_bmap_getbml_locked(b, sbd_in->sbd_seq,
	    sbd_in->sbd_nid, sbd_in->sbd_pid);
	BMAP_ULOCK(b);

	if (!obml) {
		rc = ENOENT;
		goto out;
	}

	rw = (sbd_in->sbd_ios == IOS_ID_ANY) ? BML_READ : BML_WRITE;
	bml = mds_bml_new(b, exp, rw, &exp->exp_connection->c_peer);

	rc = mds_bmap_bml_add(bml, (rw == BML_READ ? SL_READ : SL_WRITE),
	    sbd_in->sbd_ios);
	if (rc) {
		BML_LOCK(bml);
		bml->bml_flags |= BML_FREEING;
		if (rc == -SLERR_ION_OFFLINE)
			bml->bml_flags |= BML_ASSFAIL;
		BML_ULOCK(bml);
		goto out;
	}

	/* Do some post setup on the new lease. */
	sbd_out->sbd_seq = bml->bml_seq;
	sbd_out->sbd_bmapno = b->bcm_bmapno;
	sbd_out->sbd_fg = b->bcm_fcmh->fcmh_fg;
	sbd_out->sbd_nid = exp->exp_connection->c_peer.nid;
	sbd_out->sbd_pid = exp->exp_connection->c_peer.pid;

	if (rw == BML_WRITE) {
		struct bmap_mds_info *bmi = bmap_2_bmi(b);

		psc_assert(bmi->bmi_wr_ion);

		sbd_out->sbd_key = bml->bml_bmi->bmi_assign->odtr_key;
		sbd_out->sbd_ios =
		    rmmi2resm(bmi->bmi_wr_ion)->resm_res_id;
	} else {
		sbd_out->sbd_key = BMAPSEQ_ANY;
		sbd_out->sbd_ios = IOS_ID_ANY;
	}

	BMAP_LOCK(b);
	if (b->bcm_flags & BMAP_DIO)
		sbd_out->sbd_flags |= SRM_LEASEBMAPF_DIRECTIO;
	BMAP_ULOCK(b);

	/*
	 * By this point it should be safe to ignore the error from
	 * mds_bmap_bml_release() since a new lease has already been
	 * issued.
	 */
	BML_LOCK(obml);
	obml->bml_flags |= BML_FREEING;
	BML_ULOCK(obml);

 out:
	if (bml)
		mds_bmap_bml_release(bml);
	if (obml)
		mds_bmap_bml_release(obml);
	DEBUG_BMAP(rc ? PLL_WARN : PLL_INFO, b,
	   "renew oseq=%"PRIu64" nseq=%"PRIu64" nid=%"PRIu64" pid=%u",
	   sbd_in->sbd_seq, bml ? bml->bml_seq : 0,
	   exp->exp_connection->c_peer.nid,
	   exp->exp_connection->c_peer.pid);

	bmap_op_done(b);
	return (rc);
}

void
slm_setattr_core(struct fidc_membh *f, struct srt_stat *sstb,
    int to_set)
{
	int locked, deref = 0, rc = 0;
	struct slm_wkdata_ptrunc *wk;
	struct fcmh_mds_info *fmi;

	if ((to_set & PSCFS_SETATTRF_DATASIZE) && sstb->sst_size) {
		if (f == NULL) {
			rc = slm_fcmh_get(&sstb->sst_fg, &f);
			if (rc) {
				psclog_errorx("unable to retrieve FID "
				    SLPRI_FID": %s",
				    sstb->sst_fid, slstrerror(rc));
				return;
			}

			deref = 1;
		}

		locked = FCMH_RLOCK(f);
		f->fcmh_flags |= FCMH_IN_PTRUNC;
		fmi = fcmh_2_fmi(f);
		fmi->fmi_ptrunc_size = sstb->sst_size;
		FCMH_URLOCK(f, locked);

		wk = pfl_workq_getitem(slm_ptrunc_prepare,
		    struct slm_wkdata_ptrunc);
		wk->f = f;
		fcmh_op_start_type(f, FCMH_OPCNT_WORKER);
		pfl_workq_putitem(wk);

		if (deref)
			fcmh_op_done(f);
	}
}

struct ios_list {
	sl_replica_t	iosv[SL_MAX_REPLICAS];
	int		nios;
};

__static void
ptrunc_tally_ios(struct bmapc_memb *b, int iosidx, int val, void *arg)
{
	struct ios_list *ios_list = arg;
	sl_ios_id_t ios_id;
	int i;

	switch (val) {
	case BREPLST_VALID:
	case BREPLST_REPL_SCHED:
	case BREPLST_REPL_QUEUED:
		break;
	default:
		return;
	}

	ios_id = bmap_2_repl(b, iosidx);

	for (i = 0; i < ios_list->nios; i++)
		if (ios_list->iosv[i].bs_id == ios_id)
			return;

	ios_list->iosv[ios_list->nios++].bs_id = ios_id;
}

int
slm_ptrunc_prepare(void *p)
{
	int to_set, rc, wait = 0;
	struct slm_wkdata_ptrunc *wk = p;
	struct srm_bmap_release_req *mq;
	struct srm_bmap_release_rep *mp;
	struct slashrpc_cservice *csvc;
	struct bmap_mds_lease *bml;
	struct pscrpc_request *rq;
	struct fcmh_mds_info *fmi;
	struct psc_dynarray *da;
	struct fidc_membh *f;
	struct bmapc_memb *b;
	sl_bmapno_t i;

	f = wk->f;
	fmi = fcmh_2_fmi(f);
	da = &fcmh_2_fmi(f)->fmi_ptrunc_clients;

	/*
	 * Wait until any leases for this or any bmap after have been
	 * relinquished.
	 */
	FCMH_LOCK(f);

	/*
	 * XXX bmaps issued in the wild not accounted for in fcmh_fsz
	 * are skipped here.
	 */
	if (fmi->fmi_ptrunc_size >= fcmh_2_fsz(f)) {
		FCMH_WAIT_BUSY(f);
		if (fmi->fmi_ptrunc_size > fcmh_2_fsz(f)) {
			struct srt_stat sstb;

			sstb.sst_size = fmi->fmi_ptrunc_size;
			mds_fcmh_setattr_nolog(current_vfsid, f,
			    PSCFS_SETATTRF_DATASIZE, &sstb);
		}
		f->fcmh_flags &= ~FCMH_IN_PTRUNC;
		FCMH_UNBUSY(f);
		slm_ptrunc_wake_clients(wk);
		return (0);
	}

	i = fmi->fmi_ptrunc_size / SLASH_BMAP_SIZE;
	FCMH_ULOCK(f);
	for (;; i++) {
		if (bmap_getf(f, i, SL_WRITE, BMAPGETF_LOAD |
		    BMAPGETF_NOAUTOINST, &b))
			break;

		BMAP_LOCK(b);
		BMAP_FOREACH_LEASE(b, bml) {
			BMAP_ULOCK(b);

			csvc = slm_getclcsvc(bml->bml_exp);
			if (csvc == NULL)
				continue;
			if (!psc_dynarray_exists(da, csvc) &&
			    SL_RSX_NEWREQ(csvc, SRMT_RELEASEBMAP, rq,
			    mq, mp) == 0) {
				mq->sbd[0].sbd_fg.fg_fid = fcmh_2_fid(f);
				mq->sbd[0].sbd_bmapno = i;
				mq->nbmaps = 1;
				(void)SL_RSX_WAITREP(csvc, rq, mp);
				pscrpc_req_finished(rq);

				FCMH_LOCK(f);
				psc_dynarray_add(da, csvc);
				FCMH_ULOCK(f);
			} else
				sl_csvc_decref(csvc);

			BMAP_LOCK(b);
		}
		if (pll_nitems(&bmap_2_bmi(b)->bmi_leases))
			wait = 1;
		bmap_op_done(b);
	}
	if (wait)
		return (1);

	/* all client leases have been relinquished */
	/* XXX: wait for any CRC updates coming from sliods */

	FCMH_LOCK(f);
	to_set = PSCFS_SETATTRF_DATASIZE | SL_SETATTRF_PTRUNCGEN;
	fcmh_2_ptruncgen(f)++;
	f->fcmh_sstb.sst_size = fmi->fmi_ptrunc_size;

	mds_reserve_slot(1);
	rc = mdsio_setattr(current_vfsid, fcmh_2_mdsio_fid(f),
	    &f->fcmh_sstb, to_set, &rootcreds, &f->fcmh_sstb,
	    fcmh_2_mdsio_data(f), mdslog_namespace);
	mds_unreserve_slot(1);

	if (rc)
		psclog_error("setattr rc=%d", rc);

	FCMH_ULOCK(f);
	slm_ptrunc_apply(wk);
	return (0);
}

void
slm_ptrunc_apply(struct slm_wkdata_ptrunc *wk)
{
	int done = 0, tract[NBREPLST];
	struct ios_list ios_list;
	struct fidc_membh *f;
	struct bmapc_memb *b;
	sl_bmapno_t i;

	f = wk->f;

	brepls_init(tract, -1);
	tract[BREPLST_VALID] = BREPLST_TRUNCPNDG;

	ios_list.nios = 0;

	i = fcmh_2_fsz(f) / SLASH_BMAP_SIZE;
	if (fcmh_2_fsz(f) % SLASH_BMAP_SIZE) {
		/*
		 * If a bmap sliver was sliced, we must await for a
		 * sliod to reply with the new CRC.
		 */
		if (bmap_get(f, i, SL_WRITE, &b) == 0) {
			mds_repl_bmap_walkcb(b, tract, NULL, 0,
			    ptrunc_tally_ios, &ios_list);
			mds_bmap_write_repls_rel(b);

			upsch_enqueue(bmap_2_upd(b));
		}
		i++;
	} else {
		done = 1;
	}

	brepls_init(tract, -1);
	tract[BREPLST_REPL_SCHED] = BREPLST_GARBAGE;
	tract[BREPLST_VALID] = BREPLST_GARBAGE;

	for (;; i++) {
		if (bmap_getf(f, i, SL_WRITE, BMAPGETF_LOAD |
		    BMAPGETF_NOAUTOINST, &b))
			break;

		BHGEN_INCREMENT(b);
		mds_repl_bmap_walkcb(b, tract, NULL, 0,
		    ptrunc_tally_ios, &ios_list);
		mds_bmap_write_repls_rel(b);
	}

	if (done) {
		FCMH_LOCK(f);
		f->fcmh_flags &= ~FCMH_IN_PTRUNC;
		fcmh_wake_locked(f);
		FCMH_ULOCK(f);
		slm_ptrunc_wake_clients(wk);
	}

	/* XXX adjust nblks */

	fcmh_op_done_type(f, FCMH_OPCNT_WORKER);
}

int
slm_ptrunc_wake_clients(void *p)
{
	struct slm_wkdata_ptrunc *wk = p;
	struct slashrpc_cservice *csvc;
	struct srm_bmap_wake_req *mq;
	struct srm_bmap_wake_rep *mp;
	struct pscrpc_request *rq;
	struct fcmh_mds_info *fmi;
	struct psc_dynarray *da;
	struct fidc_membh *f;
	int rc;

	f = wk->f;
	fmi = fcmh_2_fmi(f);
	da = &fmi->fmi_ptrunc_clients;
	FCMH_LOCK(f);
	while (psc_dynarray_len(da)) {
		csvc = psc_dynarray_getpos(da, 0);
		psc_dynarray_remove(da, csvc);
		FCMH_ULOCK(f);

		rc = SL_RSX_NEWREQ(csvc, SRMT_BMAP_WAKE, rq, mq, mp);
		if (rc == 0) {
			mq->fg = f->fcmh_fg;
			mq->bmapno = fcmh_2_fsz(f) / SLASH_BMAP_SIZE;
			(void)SL_RSX_WAITREP(csvc, rq, mp);
			pscrpc_req_finished(rq);
		}
		sl_csvc_decref(csvc);

		FCMH_LOCK(f);
	}
	fcmh_op_done_type(f, FCMH_OPCNT_WORKER);
	return (0);
}

void
_dbdo(const struct pfl_callerinfo *pci,
    int (*cb)(struct slm_sth *, void *), void *arg,
    const char *fmt, ...)
{
	char *p, dbuf[LINE_MAX];
	int dbuf_off = 0, rc, n, j;
	struct slm_sth *sth;
	const char *errstr;
	uint64_t key;
	va_list ap;

	key = (uint64_t)fmt;
	sth = psc_hashtbl_search(&slm_sth_hashtbl, NULL, NULL, &key);
	if (sth == NULL) {
		struct psc_hashbkt *hb;

		hb = psc_hashbkt_get(&slm_sth_hashtbl, &key);
		psc_hashbkt_lock(hb);
		sth = psc_hashbkt_search(&slm_sth_hashtbl, hb, NULL,
		    NULL, &key);
		if (sth == NULL) {
			sth = PSCALLOC(sizeof(*sth));
			psc_hashent_init(&slm_sth_hashtbl, sth);
			psc_mutex_init(&sth->sth_mutex);
			sth->sth_fmt = fmt;

			psc_mutex_lock(&slm_dbh_mut);
			rc = sqlite3_prepare_v2(slm_dbh, fmt, -1,
			    &sth->sth_sth, NULL);
			psc_mutex_unlock(&slm_dbh_mut);
			psc_assert(rc == SQLITE_OK);

			psc_hashbkt_add_item(&slm_sth_hashtbl, hb, sth);
		}
		psc_hashbkt_unlock(hb);
	}

	psc_mutex_lock(&sth->sth_mutex);
	n = sqlite3_bind_parameter_count(sth->sth_sth);
	va_start(ap, fmt);
	if (psc_log_getlevel(pci->pci_subsys) >= PLL_DEBUG) {
		strlcpy(dbuf, fmt, sizeof(dbuf));
		dbuf_off = strlen(fmt);
	}
	for (j = 0; j < n; j++) {
		int type = va_arg(ap, int);
		switch (type) {
		case SQLITE_INTEGER64: {
			int64_t arg;

			arg = va_arg(ap, int64_t);
			rc = sqlite3_bind_int64(sth->sth_sth, j + 1,
			    arg);
			if (psc_log_getlevel(pci->pci_subsys) >= PLL_DEBUG)
				dbuf_off += snprintf(dbuf + dbuf_off,
				    sizeof(dbuf) - dbuf_off,
				    "; arg %d: %"PRId64, j + 1, arg);
			break;
		    }
		case SQLITE_INTEGER: {
			int32_t arg;

			arg = va_arg(ap, int32_t);
			rc = sqlite3_bind_int(sth->sth_sth, j + 1, arg);
			if (psc_log_getlevel(pci->pci_subsys) >= PLL_DEBUG)
				dbuf_off += snprintf(dbuf + dbuf_off,
				    sizeof(dbuf) - dbuf_off,
				    "; arg %d: %d", j + 1, arg);
			break;
		    }
		case SQLITE_TEXT:
			p = va_arg(ap, char *);
			rc = sqlite3_bind_text(sth->sth_sth, j + 1, p,
			    strlen(p), SQLITE_STATIC);
			if (psc_log_getlevel(pci->pci_subsys) >= PLL_DEBUG)
				dbuf_off += snprintf(dbuf + dbuf_off,
				    sizeof(dbuf) - dbuf_off,
				    "; arg %d: %s", j + 1, p);
			break;
		default:
			psc_fatalx("type");
		}
		psc_assert(rc == SQLITE_OK);
	}
	psclog_debug("issuing SQL: %s", dbuf);
	va_end(ap);

	psc_mutex_lock(&slm_dbh_mut);
 next:

	rc = sqlite3_step(sth->sth_sth);
	if (rc == SQLITE_ROW) {
		cb(sth, arg);
		psc_mutex_unlock(&slm_dbh_mut);
		goto next;
	} else if (rc != SQLITE_DONE) {
		errstr = sqlite3_errmsg(slm_dbh);
		psclog_errorx("SQL error: rc=%d query=%s; msg=%s", rc,
		    fmt, errstr);
		/* XXX rebuild db? */
		psc_mutex_unlock(&slm_dbh_mut);
	} else
		psc_mutex_unlock(&slm_dbh_mut);
	sqlite3_reset(sth->sth_sth);
	if (n) {
		rc = sqlite3_clear_bindings(sth->sth_sth);
		psc_assert(rc == SQLITE_OK);
	}
	psc_mutex_unlock(&sth->sth_mutex);
}

int
slm_ptrunc_odt_startup_cb(void *data, struct odtable_receipt *odtr,
    __unusedx void *arg)
{
	struct {
		struct slash_fidgen fg;
	} *pt = data;
	struct fidc_membh *f;
//	sl_bmapno_t bno;
	int rc;

	PSCFREE(odtr);

	rc = slm_fcmh_get(&pt->fg, &f);
	if (rc == 0) {
//		bno = howmany(fcmh_2_fsz(f), SLASH_BMAP_SIZE) - 1;
		/* XXX do something */
		fcmh_op_done(f);
	}

//	brepls_init(tract, -1);
//	tract[BREPLST_TRUNCPNDG_SCHED] = BREPLST_TRUNCPNDG;

//	brepls_init(retifset, 0);
//	retifset[BREPLST_TRUNCPNDG_SCHED] = 1;
//	wr = mds_repl_bmap_walk_all(b, tract, retifset, 0);

	return (0);
}
