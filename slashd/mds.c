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

#include "psc_ds/lockedlist.h"
#include "psc_ds/tree.h"
#include "psc_ds/treeutil.h"
#include "psc_rpc/export.h"
#include "psc_rpc/rsx.h"
#include "psc_util/alloc.h"
#include "psc_util/atomic.h"
#include "psc_util/log.h"
#include "psc_util/odtable.h"

#include "bmap.h"
#include "bmap_mds.h"
#include "cache_params.h"
#include "fidc_mds.h"
#include "fidcache.h"
#include "inode.h"
#include "mdscoh.h"
#include "mdsio.h"
#include "mdslog.h"
#include "repl_mds.h"
#include "rpc_mds.h"
#include "slashd.h"
#include "slashrpc.h"
#include "slerr.h"

struct odtable				*mdsBmapAssignTable;
uint64_t				 mdsBmapSequenceNo;
const struct bmap_ondisk		 null_bmap_od;
const struct slash_inode_od		 null_inode_od;
const struct slash_inode_extras_od	 null_inox_od;

__static void
mds_inode_od_initnew(struct slash_inode_handle *i)
{
	i->inoh_flags = INOH_INO_NEW | INOH_INO_DIRTY;

	/* For now this is a fixed size. */
	i->inoh_ino.ino_bsz = SLASH_BMAP_SIZE;
	i->inoh_ino.ino_version = INO_VERSION;
	i->inoh_ino.ino_flags = 0;
	i->inoh_ino.ino_nrepls = 0;
}

int
mds_inode_read(struct slash_inode_handle *i)
{
	uint64_t crc;
	int rc=0, locked;

	locked = reqlock(&i->inoh_lock);
	psc_assert(i->inoh_flags & INOH_INO_NOTLOADED);

	rc = mdsio_inode_read(i);

	if (rc == SLERR_SHORTIO && i->inoh_ino.ino_crc == 0 &&
	    memcmp(&i->inoh_ino, &null_inode_od, INO_OD_CRCSZ) == 0) {
		DEBUG_INOH(PLL_INFO, i, "detected a new inode");
		mds_inode_od_initnew(i);
		rc = 0;

	} else if (rc) {
		DEBUG_INOH(PLL_WARN, i, "mdsio_inode_read: %d", rc);

	} else {
		psc_crc64_calc(&crc, &i->inoh_ino, INO_OD_CRCSZ);
		if (crc == i->inoh_ino.ino_crc) {
			i->inoh_flags &= ~INOH_INO_NOTLOADED;
			DEBUG_INOH(PLL_INFO, i, "successfully loaded inode od");
		} else {
			DEBUG_INOH(PLL_WARN, i,
				   "CRC failed want=%"PRIx64", got=%"PRIx64,
				   i->inoh_ino.ino_crc, crc);
			rc = EIO;
		}
	}
	ureqlock(&i->inoh_lock, locked);
	return (rc);
}

int
mds_inox_load_locked(struct slash_inode_handle *ih)
{
	uint64_t crc;
	int rc;

	INOH_LOCK_ENSURE(ih);

	psc_assert(!(ih->inoh_flags & INOH_HAVE_EXTRAS));

	psc_assert(ih->inoh_extras == NULL);
	ih->inoh_extras = PSCALLOC(sizeof(*ih->inoh_extras));

	rc = mdsio_inode_extras_read(ih);
	if (rc == SLERR_SHORTIO && ih->inoh_extras->inox_crc == 0 &&
	    memcmp(&ih->inoh_extras, &null_inox_od, INOX_OD_CRCSZ) == 0) {
		rc = 0;
	} else if (rc) {
		DEBUG_INOH(PLL_WARN, ih, "mdsio_inode_extras_read: %d", rc);
	} else {
		psc_crc64_calc(&crc, ih->inoh_extras, INOX_OD_CRCSZ);
		if (crc == ih->inoh_extras->inox_crc)
			ih->inoh_flags |= INOH_HAVE_EXTRAS;
		else {
			psc_errorx("inox CRC fail; disk %"PSCPRIxCRC64
			    " mem %"PSCPRIxCRC64,
			    ih->inoh_extras->inox_crc, crc);
			rc = EIO;
		}
	}
	if (rc) {
		PSCFREE(ih->inoh_extras);
		ih->inoh_extras = NULL;
		ih->inoh_flags &= ~INOH_HAVE_EXTRAS;
	}
	return (rc);
}

int
mds_inox_ensure_loaded(struct slash_inode_handle *ih)
{
	int locked, rc = 0;

	locked = INOH_RLOCK(ih);
	if (ATTR_NOTSET(ih->inoh_flags, INOH_HAVE_EXTRAS))
		rc = mds_inox_load_locked(ih);
	INOH_URLOCK(ih, locked);
	return (rc);
}

int
mds_bmap_exists(struct fidc_membh *f, sl_bmapno_t n)
{
	sl_bmapno_t lblk;
	int locked;
	//int rc;

	locked = FCMH_RLOCK(f);
#if 0
	if ((rc = mds_stat_refresh_locked(f)))
		return (rc);
#endif

	lblk = fcmh_2_nbmaps(f);

	psc_trace("fid="SLPRI_FG" lblk=%u fsz=%"PSCPRIdOFFT,
	    SLPRI_FG_ARGS(&f->fcmh_fg), lblk, fcmh_2_fsz(f));

	FCMH_URLOCK(f, locked);
	return (n < lblk);
}

/**
 * mds_bmap_directio - Called when a new read or write lease is added
 *    to the bmap.  Maintains the DIRECTIO status of the bmap based on
 *    the numbers of readers and writers present.
 * @b:   the bmap
 * @@rw: read / write op
 * Note: the new bml has yet to be added.
 */
__static int
mds_bmap_directio(struct bmapc_memb *b, enum rw rw, lnet_process_id_t *np)
{
	struct bmap_mds_info *bmdsi = bmap_2_bmdsi(b);
	struct bmap_mds_lease *bml;
	int rc=0;

	BMAP_LOCK_ENSURE(b);
	psc_assert(rw == SL_WRITE || rw == SL_READ);

	if (b->bcm_flags & BMAP_DIO) {
		psc_assert(bmdsi->bmdsi_wr_ion);
		goto out;
	}
	if (b->bcm_flags & BMAP_DIORQ) {
		psc_assert(bmdsi->bmdsi_wr_ion);
		/* In the process of waiting for an async rpc to complete.
		 */
		rc = SLERR_BMAP_DIOWAIT;
		goto out;
	}

	if (bmdsi->bmdsi_writers) {
		/* A second writer or a reader wants access.  Ensure only
		 *    one lease is present.
		 */
		bml = pll_peektail(&bmdsi->bmdsi_leases);
		psc_assert(bml->bml_flags & BML_WRITE);
		psc_assert(!(b->bcm_flags & BMAP_DIORQ));
		if (bml->bml_flags & BML_CDIO) {
			b->bcm_flags |= BMAP_DIO;
			goto out;

		} else if (bml->bml_cli_nidpid.nid == np->nid &&
			   bml->bml_cli_nidpid.pid == np->pid) {
			/* Lease belongs to the client making the request.
			 */
			DEBUG_BMAP(PLL_NOTIFY, b, "dup lease");

		} else {
			b->bcm_flags |= BMAP_DIORQ;
			bml->bml_flags |= BML_COHDIO;
			DEBUG_BMAP(PLL_WARN, b, "set BMAP_DIORQ, issuing cb");
			/* Use a non-blocking revocation.  The client will
			 *   retry periodically until the bmap has either
			 *   reached DIO state or been timed out.
			 */
			mdscoh_req(bml, MDSCOH_NONBLOCK);
			rc = SLERR_BMAP_DIOWAIT;
			goto out;
		}

	} else if (rw == SL_WRITE && bmdsi->bmdsi_readers) {
		struct bmap_mds_lease *tmp;
		int set_dio=0;

		/* Writer being added amidst one or more readers.  Issue
		 *   courtesy callbacks to the readers.
		 */
		PLL_FOREACH(bml, &bmdsi->bmdsi_leases) {
			tmp = bml;
			do {
				psc_assert(!(bml->bml_flags & BML_WRITE));
				psc_assert(bml->bml_flags & BML_READ);

				if (bml->bml_cli_nidpid.nid != np->nid) {
					set_dio = 1;
					if (!(bml->bml_flags & BML_CDIO))
						(int)mdscoh_req(bml,
							MDSCOH_NONBLOCK);
				}
				tmp = tmp->bml_chain;
			} while (tmp != bml);
		}
		if (set_dio)
			b->bcm_flags |= BMAP_DIO;
	}
 out:
	return (rc);
}

__static int
mds_bmap_ion_restart(struct bmap_mds_lease *bml)
{
	struct sl_resm *resm = libsl_nid2resm(bml->bml_ion_nid);
	struct slashrpc_cservice *csvc;
	struct resm_mds_info *rmmi;

	csvc = slm_geticsvc(resm);
	if (csvc == NULL)
		/*
		 * This can happen if the MDS finds bmap leases in
		 * the odtable and we didn't start the I/O server.
		 */
		return (-SLERR_ION_OFFLINE);

	rmmi = resm->resm_pri;

	atomic_inc(&rmmi->rmmi_refcnt);
	sl_csvc_decref(csvc);

	bml->bml_bmdsi->bmdsi_wr_ion = rmmi;
	bmap_op_start_type(bml_2_bmap(bml), BMAP_OPCNT_IONASSIGN);

	mds_bmap_timeotbl_mdsi(bml, BTE_ADD|BTE_REATTACH);

	bml->bml_bmdsi->bmdsi_seq = bml->bml_seq;

	DEBUG_BMAP(PLL_DEBUG, bml_2_bmap(bml), "res(%s) ion(%s)",
	    resm->resm_res->res_name, resm->resm_addrbuf);

	return (0);
}

/**
 * mds_bmap_ion_assign - Bind a bmap to an ION for writing.  The process
 *    involves a round-robin'ing of an I/O system's nodes and attaching a
 *    a resm_mds_info to the bmap, used for establishing connection to the ION.
 * @bml: the bmap lease
 * @pios: the preferred I/O system
 */
__static int
mds_bmap_ion_assign(struct bmap_mds_lease *bml, sl_ios_id_t pios)
{
	struct bmapc_memb *bmap = bml_2_bmap(bml);
	struct bmap_mds_info *bmdsi = bmap_2_bmdsi(bmap);
	struct sl_resource *res = libsl_id2res(pios);
	struct slashrpc_cservice *csvc;
	struct resprof_mds_info *rpmi;
	struct resm_mds_info *rmmi;
	struct bmap_ion_assign bia;
	struct sl_resm *resm;
	int nb, j, len;

	psc_assert(bmap->bcm_flags & BMAP_IONASSIGN);
	psc_assert(!bmdsi->bmdsi_wr_ion);
	psc_assert(psc_atomic32_read(&bmap->bcm_opcnt) > 0);

	if (!res) {
		psc_warnx("Failed to find pios %d", pios);
		return (-SLERR_ION_UNKNOWN);
	}
	rpmi = res->res_pri;
	len = psc_dynarray_len(&res->res_members);

	/*
	 * Try a connection to each member in the resource.  If none
	 * are immediately available, try again in a block manner before
	 * returning offline status.
	 */
	for (nb = 1; nb > 0; nb--)
		for (j = 0; j < len; j++) {
			spinlock(&rpmi->rpmi_lock);
			resm = psc_dynarray_getpos(&res->res_members,
			    slm_get_rpmi_idx(res));
			freelock(&rpmi->rpmi_lock);

			psc_trace("trying res(%s) ion(%s)",
			    res->res_name, resm->resm_addrbuf);

			if (nb)
				csvc = slm_geticsvc_nb(resm);
			else
				csvc = slm_geticsvc(resm);

			if (csvc)
				goto online;
		}

	return (-SLERR_ION_OFFLINE);

 online:
	bmdsi->bmdsi_wr_ion = rmmi = resm->resm_pri;
	atomic_inc(&rmmi->rmmi_refcnt);
	sl_csvc_decref(csvc); /* XXX this is really dumb */

	DEBUG_BMAP(PLL_TRACE, bmap, "online res(%s) ion(%s)",
	    res->res_name, resm->resm_addrbuf);

	/*
	 * An ION has been assigned to the bmap, mark it in the odtable
	 *   so that the assignment may be restored on reboot.
	 */
	memset(&bia, 0, sizeof(bia));
	bia.bia_ion_nid = bml->bml_ion_nid = rmmi->rmmi_resm->resm_nid;
	bia.bia_lastcli = bml->bml_cli_nidpid;

	bia.bia_ios = rmmi->rmmi_resm->resm_res->res_id;
	bia.bia_fid = fcmh_2_fid(bmap->bcm_fcmh);
	bia.bia_bmapno = bmap->bcm_bmapno;
	bia.bia_start = time(NULL);
	bia.bia_flags = (bmap->bcm_flags & BMAP_DIO) ? BIAF_DIO : 0;
	bmdsi->bmdsi_seq = bia.bia_seq = mds_bmap_timeotbl_mdsi(bml, BTE_ADD);

	psc_assert(!bmdsi->bmdsi_assign);
	bmdsi->bmdsi_assign = odtable_putitem(mdsBmapAssignTable, &bia,
	    sizeof(bia));
	if (!bmdsi->bmdsi_assign) {
		DEBUG_BMAP(PLL_ERROR, bmap, "failed odtable_putitem()");
		return (-SLERR_XACT_FAIL);
	}

	/*
	 * Signify that a ION has been assigned to this bmap.  This
	 *   opcnt ref will stay in place until the bmap has been released
	 *   by the last client or has been timed out.
	 */
	bmap_op_start_type(bmap, BMAP_OPCNT_IONASSIGN);

	mds_repl_inv_except(bmap, bia.bia_ios);

	bml->bml_seq = bia.bia_seq;

	DEBUG_FCMH(PLL_INFO, bmap->bcm_fcmh, "bmap assignment");
	DEBUG_BMAP(PLL_INFO, bmap, "using res(%s) ion(%s) "
		   "rmmi(%p) bia(%p)", res->res_name, resm->resm_addrbuf,
		   bmdsi->bmdsi_wr_ion, bmdsi->bmdsi_assign);

	return (0);
}

__static int
mds_bmap_ion_update(struct bmap_mds_lease *bml)
{
	struct bmapc_memb *b = bml_2_bmap(bml);
	struct bmap_mds_info *bmdsi = bmap_2_bmdsi(b);
	struct bmap_ion_assign *bia;

	psc_assert(b->bcm_flags & BMAP_IONASSIGN);

	bia = odtable_getitem(mdsBmapAssignTable, bmdsi->bmdsi_assign);
	if (!bia) {
		DEBUG_BMAP(PLL_WARN, b, "odtable_getitem() failed");
		return (-1);
	}

	if (bml->bml_cli_nidpid.nid != bia->bia_lastcli.nid ||
	    bml->bml_cli_nidpid.pid != bia->bia_lastcli.pid)
	    psc_assert(b->bcm_flags & BMAP_DIO);

	psc_assert(bia->bia_seq == bmdsi->bmdsi_seq);
	bia->bia_start = time(NULL);
	bia->bia_seq = bmdsi->bmdsi_seq = mds_bmap_timeotbl_mdsi(bml, BTE_ADD);
	bia->bia_lastcli = bml->bml_cli_nidpid;
	bia->bia_flags = BIAF_DIO;

	bmdsi->bmdsi_assign = odtable_replaceitem(mdsBmapAssignTable,
	    bmdsi->bmdsi_assign, bia, sizeof(*bia));
	psc_assert(bmdsi->bmdsi_assign);

	bml->bml_ion_nid = bia->bia_ion_nid;
	bml->bml_seq = bia->bia_seq;

	return (0);
}

/**
 * mds_bmap_dupls_find - Find the first lease of a given client based on its
 *     {nid, pid} pair.  Also walk the chain of duplicate leases to count the
 *     number of read and write leases. Note that only the first lease of a
 *     client is linked on the bmdsi->bmdsi_leases list, the rest is linked
 *     on a private chain and tagged with BML_CHAIN flag.
 */
static inline struct bmap_mds_lease *
mds_bmap_dupls_find(struct bmap_mds_info *bmdsi, lnet_process_id_t *cnp,
	    int *wlease, int *rlease)
{
	struct bmap_mds_lease *tmp, *bml=NULL;

	*rlease=0;
	*wlease=0;

	PLL_FOREACH(tmp, &bmdsi->bmdsi_leases) {
		if (tmp->bml_cli_nidpid.nid != cnp->nid ||
		    tmp->bml_cli_nidpid.pid != cnp->pid)
			continue;
		/* Only one lease per client is allowed on the list.
		 */
		psc_assert(!bml);
		bml = tmp;
	}

	if (!bml)
		return (NULL);

	tmp = bml;
	do {
		/* All dup leases should be chained off the first bml.
		 */
		if (tmp->bml_flags & BML_READ)
			(*rlease)++;
		else
			(*wlease)++;

		DEBUG_BMAP(PLL_INFO, bmi_2_bmap(bmdsi), "bml=%p tmp=%p "
			   "(wlease=%d rlease=%d) (nwtrs=%d nrdrs=%d)",
			   bml, tmp, *wlease, *rlease,
			   bmdsi->bmdsi_writers, bmdsi->bmdsi_readers);

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
	struct bmap_mds_info *bmdsi;
	struct bmapc_memb *b;
	int rc = 0, wlease, rlease;

	bmdsi = bml->bml_bmdsi;
	b = bmi_2_bmap(bmdsi);

	bcm_wait_locked(b, b->bcm_flags & BMAP_IONASSIGN);

	DEBUG_BMAP(PLL_INFO, b, "bml=%p bmdsi_writers=%d bmdsi_readers=%d",
		   bml, bmdsi->bmdsi_writers, bmdsi->bmdsi_readers);

	if (bml->bml_flags & BML_WRITE)
		return (EALREADY);

	if ((rc = mds_bmap_directio(b, SL_WRITE, &bml->bml_cli_nidpid)))
		return (rc);

	mds_bmap_dupls_find(bmdsi, &bml->bml_cli_nidpid,
		   &wlease, &rlease);

	/* Account for the read lease which is to be converted.
	 */
	psc_assert(rlease);
	if (!wlease) {
		/* Only bump bmdsi_writers if no other write lease
		 *   is still leased to this client.
		 */
		bmdsi->bmdsi_writers++;
		wlease = -1;

		if (rlease) {
			bmdsi->bmdsi_readers--;
			rlease = -1;
		}
	}

	b->bcm_flags |= BMAP_IONASSIGN;
	bml->bml_flags &= ~BML_READ;
	bml->bml_flags |= BML_UPGRADE | BML_WRITE;

	if (bmdsi->bmdsi_wr_ion) {
		BMAP_ULOCK(b);
		rc = mds_bmap_ion_update(bml);

	} else {
		BMAP_ULOCK(b);
		rc = mds_bmap_ion_assign(bml, prefios);
	}
	psc_assert(bmdsi->bmdsi_wr_ion);

	BMAP_LOCK(b);

	DEBUG_BMAP(PLL_INFO, b, "bml=%p rc=%d bmdsi_writers=%d bmdsi_readers=%d",
		   bml, rc, bmdsi->bmdsi_writers, bmdsi->bmdsi_readers);

	if (rc) {
		b->bcm_flags |= BMAP_MDS_NOION;
		bml->bml_flags |= BML_READ;
		bml->bml_flags &= ~(BML_UPGRADE | BML_WRITE);

		if (wlease < 0) {
			bmdsi->bmdsi_writers--;
			psc_assert(bmdsi->bmdsi_writers >= 0);
		}

		if (rlease < 0)
			/* Restore the reader cnt.
			 */
			bmdsi->bmdsi_readers++;
	}
	b->bcm_flags &= ~BMAP_IONASSIGN;
	bcm_wake_locked(b);
	return (rc);
}

/**
 * mds_bmap_getbml - Obtain the lease handle for a bmap denoted by the
 *	specified issued sequence number.
 * @b: locked bmap.
 * @cli_nid: client network ID.
 * @cli_pid: client network process ID.
 * @seq: lease sequence.
 */
struct bmap_mds_lease *
mds_bmap_getbml(struct bmapc_memb *b, lnet_nid_t cli_nid,
    lnet_pid_t cli_pid, uint64_t seq)
{
	struct bmap_mds_info *bmdsi;
	struct bmap_mds_lease *bml;

	BMAP_LOCK_ENSURE(b);

	bmdsi = bmap_2_bmdsi(b);
	PLL_FOREACH(bml, &bmdsi->bmdsi_leases) {
		if (bml->bml_cli_nidpid.nid == cli_nid &&
		    bml->bml_cli_nidpid.pid == cli_pid) {
			struct bmap_mds_lease *tmp=bml;

			do {
				if (tmp->bml_seq == seq)
					return (tmp);

				tmp = tmp->bml_chain;
			} while (tmp != bml);
		}
	}
	return (NULL);
}

/**
 * mds_bmap_ref_add - Add a read or write reference to the bmap's tree
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
	struct bmap_mds_info *bmdsi = bml->bml_bmdsi;
	struct bmapc_memb *b = bmi_2_bmap(bmdsi);
	struct bmap_mds_lease *obml;
	int rlease, wlease, rc = 0;

	psc_assert(bml->bml_cli_nidpid.nid &&
		   bml->bml_cli_nidpid.pid &&
		   bml->bml_cli_nidpid.nid != LNET_NID_ANY &&
		   bml->bml_cli_nidpid.pid != LNET_PID_ANY);

	BMAP_LOCK(b);
	/* Wait for BMAP_IONASSIGN to be removed before proceeding.
	 */
	bcm_wait_locked(b, (b->bcm_flags & BMAP_IONASSIGN));

	if (!(bml->bml_flags & BML_RECOVER) &&
	    (rc = mds_bmap_directio(b, rw, &bml->bml_cli_nidpid)))
		/* 'rc != 0' means that we're waiting on an async cb
		 *    completion.
		 */
		goto out;

	bmap_op_start_type(b, BMAP_OPCNT_LEASE);

	obml = mds_bmap_dupls_find(bmdsi, &bml->bml_cli_nidpid, &wlease,
		   &rlease);

	DEBUG_BMAP(PLL_INFO, b, "bml=%p obml=%p (wlease=%d rlease=%d)"
		   " (nwtrs=%d nrdrs=%d)",
		   bml, obml, wlease, rlease,
		   bmdsi->bmdsi_writers, bmdsi->bmdsi_readers);

	if (obml) {
		struct bmap_mds_lease *tmp = obml;

		bml->bml_flags |= BML_CHAIN;
		/* Add ourselves to the end.
		 */
		while (tmp->bml_chain != obml)
			tmp = tmp->bml_chain;

		tmp->bml_chain = bml;
		bml->bml_chain = obml;
		psc_assert(psclist_disjoint(&bml->bml_bmdsi_lentry));
	} else {
		/* First on the list.
		 */
		bml->bml_chain = bml;
		pll_addtail(&bmdsi->bmdsi_leases, bml);
	}
	bml->bml_flags |= BML_BMDSI;

	if (rw == SL_WRITE) {
		/* Drop the lock prior to doing disk and possibly network
		 *    I/O.
		 */
		b->bcm_flags |= BMAP_IONASSIGN;
		/* For any given chain of leases, the bmdsi_[readers|writers]
		 *    value may only be 1rd or 1wr.  In the case where 2
		 *    wtrs are present, the value is 1wr.  Mixed readers and
		 *    wtrs == 1wtr.  1-N rdrs, 1rd.
		 * Only increment writers if this is the first
		 *    write lease from the respective client.
		 */
		if (!wlease) {
			/* This is the first write from the client.
			 */
			bmdsi->bmdsi_writers++;

			if (bmdsi->bmdsi_writers > 1) {
				psc_enter_debugger("bmdsi->bmdsi_writers");
			}

			if (rlease)
				/* Remove the read cnt, it has been
				 *   superseded by the write.
				 */
				bmdsi->bmdsi_readers--;
		}

		if (bml->bml_flags & BML_RECOVER) {
			psc_assert(bmdsi->bmdsi_writers == 1);
			psc_assert(!bmdsi->bmdsi_readers);
			psc_assert(!bmdsi->bmdsi_wr_ion);
			psc_assert(bml->bml_ion_nid &&
			   bml->bml_ion_nid != LNET_NID_ANY);
			BMAP_ULOCK(b);
			rc = mds_bmap_ion_restart(bml);

		} else if (!wlease && bmdsi->bmdsi_writers == 1) {
			psc_assert(!bmdsi->bmdsi_wr_ion);
			BMAP_ULOCK(b);
			rc = mds_bmap_ion_assign(bml, prefios);

		} else {
			psc_assert(wlease && bmdsi->bmdsi_wr_ion);
			BMAP_ULOCK(b);
			rc = mds_bmap_ion_update(bml);
		}

		BMAP_LOCK(b);
		if (rc) {
			/*
			 * Keep the lease around if we can't contact the I/O node until
			 * it expires.
			 */
			if ((bml->bml_flags & BML_RECOVER) && (rc == -SLERR_ION_OFFLINE)) {
				rc = 0;
				b->bcm_flags |= BMAP_MDS_NOION;
				goto out;
			}
			DEBUG_BMAP(PLL_INFO, b, "bml=%p rc=%d bmdsi_writers=%d"
			   " bmdsi_readers=%d wlease=%d",
			   bml, rc, bmdsi->bmdsi_writers,
			   bmdsi->bmdsi_readers, wlease);

			if (!wlease) {
				//bmdsi->bmdsi_writers--;
				psc_assert(bmdsi->bmdsi_writers >= 0);
				b->bcm_flags &= ~BMAP_IONASSIGN;
				b->bcm_flags |= BMAP_MDS_NOION;
			}
		}

		b->bcm_flags &= ~BMAP_IONASSIGN;

	} else {
		bml->bml_seq = mds_bmap_timeotbl_getnextseq();

		if (!wlease && !rlease)
			bmdsi->bmdsi_readers++;
		bml->bml_flags |= BML_TIMEOQ;
		mds_bmap_timeotbl_mdsi(bml, BTE_ADD);
	}

 out:
	DEBUG_BMAP(rc ? PLL_WARN : PLL_INFO, b, "bml_add (mion=%p) bml=%p"
	   "(seq=%"PRId64" (rw=%d) (nwtrs=%d nrdrs=%d) (rc=%d)",
	   bmdsi->bmdsi_wr_ion, bml, bml->bml_seq, rw, bmdsi->bmdsi_writers,
	   bmdsi->bmdsi_readers, rc);

	bcm_wake_locked(b);
	BMAP_ULOCK(b);
	/* On error, the caller will issue mds_bmap_bml_release() which
	 *   deals with the gory details of freeing a fullly, or partially,
	 *   instantiated bml.  Therefore, BMAP_OPCNT_LEASE will not be
	 *   removed in the case of an error.
	 */
	return (rc);
}

void
mds_bmap_bml_del_locked(struct bmap_mds_lease *bml)
{
	struct bmap_mds_info *bmdsi=bml->bml_bmdsi;
	struct bmap_mds_lease *obml, *tail;
	int rlease = 0, wlease = 0;

	BMAP_LOCK_ENSURE(bmi_2_bmap(bmdsi));
	BML_LOCK_ENSURE(bml);

	obml = mds_bmap_dupls_find(bmdsi, &bml->bml_cli_nidpid, &wlease,
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
	psc_assert(psclist_conjoint(&obml->bml_bmdsi_lentry,
	    psc_lentry_hd(&obml->bml_bmdsi_lentry)));

	/* Find the bml's preceeding entry.
	 */
	tail = obml;
	while (tail->bml_chain != bml)
		tail = tail->bml_chain;
	psc_assert(tail->bml_chain == bml);

	/* Manage the bml list and bml_bmdsi_lentry.
	 */
	if (bml->bml_flags & BML_CHAIN) {
		psc_assert(psclist_disjoint(&bml->bml_bmdsi_lentry));
		psc_assert((wlease + rlease) > 1);
		tail->bml_chain = bml->bml_chain;

	} else {
		psc_assert(obml == bml);
		psc_assert(!(bml->bml_flags & BML_CHAIN));
		pll_remove(&bmdsi->bmdsi_leases, bml);

		if ((wlease + rlease) > 1) {
			psc_assert(bml->bml_chain->bml_flags & BML_CHAIN);
			psc_assert(psclist_disjoint(&bml->bml_chain->bml_bmdsi_lentry));

			bml->bml_chain->bml_flags &= ~BML_CHAIN;
			pll_addtail(&bmdsi->bmdsi_leases, bml->bml_chain);

			tail->bml_chain = bml->bml_chain;
		} else
			psc_assert(bml == bml->bml_chain);
	}

	if (bml->bml_flags & BML_WRITE) {
		if (wlease == 1) {
			psc_assert(bmdsi->bmdsi_writers > 0);
			bmdsi->bmdsi_writers--;

			DEBUG_BMAP(PLL_INFO, bmi_2_bmap(bmdsi),
			   "bml=%p bmdsi_writers=%d bmdsi_readers=%d",
			   bml, bmdsi->bmdsi_writers, bmdsi->bmdsi_readers);

			if (rlease)
				bmdsi->bmdsi_readers++;
		}
	} else {
		psc_assert(bml->bml_flags & BML_READ);
		if (!wlease && (rlease == 1)) {
			psc_assert(bmdsi->bmdsi_readers > 0);
			bmdsi->bmdsi_readers--;
		}
	}
}

/**
 * mds_bmap_bml_release - Remove a bmap lease from the MDS.  This can be
 *   called from the bmap_timeo thread, from a client bmap_release RPC,
 *   or from the nbreqset cb context.
 * @bml: bmap lease.
 * Notes:  the bml must be removed from the timeotbl in all cases.
 *    otherwise we determine list removals on a case by case basis.
 */
int
mds_bmap_bml_release(struct bmap_mds_lease *bml)
{
	struct bmapc_memb *b = bml_2_bmap(bml);
	struct bmap_mds_info *bmdsi = bml->bml_bmdsi;
	struct odtable_receipt *odtr = NULL;
	int rc = 0, exp = 0, locked;

	psc_assert(psc_atomic32_read(&b->bcm_opcnt) > 0);
	psc_assert(bml->bml_flags & BML_FREEING);

	DEBUG_BMAP(PLL_INFO, b, "bml=%p fl=%d seq=%"PRId64, bml,
		   bml->bml_flags, bml->bml_seq);

	locked = BMAP_RLOCK(b);
	/* BMAP_IONASSIGN acts as a barrier for operations which
	 *   may modify bmdsi_wr_ion.  Since ops associated with
	 *   BMAP_IONASSIGN do disk and net i/o, the spinlock is
	 *   dropped.
	 * XXX actually, the bcm_lock is not dropped until the very end.
	 *   if this becomes problematic we should investigate more.
	 *   ATM the BMAP_IONASSIGN is not relied upon
	 */
	bcm_wait_locked(b, (b->bcm_flags & BMAP_IONASSIGN));
	b->bcm_flags |= BMAP_IONASSIGN;
	if (bml->bml_flags & BML_EXP) {
		exp = 1;
		bml->bml_flags &= ~BML_EXP;
	} else
		psc_assert(psclist_disjoint(&bml->bml_exp_lentry));
	BML_ULOCK(bml);

	if (exp) {
		EXPORT_LOCK(bml->bml_exp);
		mexpc_get(bml->bml_exp);
		psclist_del(&bml->bml_exp_lentry,
		    psc_lentry_hd(&bml->bml_exp_lentry));
		EXPORT_ULOCK(bml->bml_exp);
	}

	BML_LOCK(bml);
	if (bml->bml_flags & BML_COHRLS) {
		/* Called from the mdscoh callback.  Nothing should be left
		 *   except for removing the bml from the bmdsi.
		 */
		psc_assert(!(bml->bml_flags & (BML_COH|BML_EXP|BML_TIMEOQ)));
		psc_assert(psclist_disjoint(&bml->bml_coh_lentry));
		psc_assert(psclist_disjoint(&bml->bml_exp_lentry));
		psc_assert(psclist_disjoint(&bml->bml_timeo_lentry));
		bml->bml_flags &= ~BML_COHRLS;
	}

	if (bml->bml_flags & BML_COH)
		/* Don't wait for any outstanding coherency callbacks
		 *   to complete.  Mark the bml so that the coh thread
		 *   will call this function upon rpc completion.
		 */
		bml->bml_flags |= BML_COHRLS;

	if (bml->bml_flags & BML_TIMEOQ) {
		BML_ULOCK(bml);
		(uint64_t)mds_bmap_timeotbl_mdsi(bml, BTE_DEL);
		BML_LOCK(bml);
	}

	/* If BML_COHRLS was set above then the lease must remain on the
	 *    bmdsi so that directio can be managed properly.
	 */
	if (bml->bml_flags & BML_COHRLS) {
		bml->bml_flags &= ~BML_FREEING;
		BML_ULOCK(bml);
		b->bcm_flags &= ~BMAP_IONASSIGN;
		bcm_wake_locked(b);
		BMAP_URLOCK(b, locked);
		return (-EAGAIN);
	}

	if (!(bml->bml_flags & BML_BMDSI)) {
		BML_ULOCK(bml);
		goto out;
	}

	mds_bmap_bml_del_locked(bml);
	bml->bml_flags &= ~BML_BMDSI;

	BML_ULOCK(bml);

	if ((b->bcm_flags & BMAP_DIO) &&
	    (!bmdsi->bmdsi_writers ||
	     (bmdsi->bmdsi_writers == 1 && !bmdsi->bmdsi_readers)))
		/* Remove the directio flag if possible.
		 */
		b->bcm_flags &= ~BMAP_DIO;

	/* Only release the odtable entry if the key matches.  If a match
	 *   is found then verify the sequence number matches.
	 */
	if ((bml->bml_flags & BML_WRITE) && !bmdsi->bmdsi_writers) {
		if (bml->bml_flags & BML_ASSFAIL) {
			psc_assert(!bmdsi->bmdsi_assign);
			psc_assert(!bmdsi->bmdsi_wr_ion);

		} else {
			/* odtable sanity checks:
			 */
			struct bmap_ion_assign *bia;

			bia = odtable_getitem(mdsBmapAssignTable,
			    bmdsi->bmdsi_assign);
			psc_assert(bia && bia->bia_seq == bmdsi->bmdsi_seq);
			psc_assert(bia->bia_bmapno == b->bcm_bmapno);
			/* End sanity checks.
			 */
			atomic_dec(&bmdsi->bmdsi_wr_ion->rmmi_refcnt);
			odtr = bmdsi->bmdsi_assign;
			bmdsi->bmdsi_assign = NULL;
			bmdsi->bmdsi_wr_ion = NULL;
		}
	}
	/* bmap_op_done_type(b, BMAP_OPCNT_IONASSIGN) may have released
	 *    the lock.
	 */
 out:
	BMAP_RLOCK(b);
	b->bcm_flags &= ~BMAP_IONASSIGN;
	bcm_wake_locked(b);
	bmap_op_done_type(b, BMAP_OPCNT_LEASE);

	if (odtr) {
		rc = odtable_freeitem(mdsBmapAssignTable, odtr);
		DEBUG_BMAP(PLL_NOTIFY, b, "odtable remove seq=%"PRId64" key=%"
		   PRId64" rc=%d", bml->bml_seq, odtr->odtr_key, rc);
		bmap_op_done_type(b, BMAP_OPCNT_IONASSIGN);
	}

	psc_pool_return(bmapMdsLeasePool, bml);

	return (rc);
}

/**
 * mds_handle_rls_bmap - handle SRMT_RELEASEBMAP RPC from a client or an I/O server.
 */
int
mds_handle_rls_bmap(struct pscrpc_request *rq, int sliod)
{
	struct srm_bmap_release_req *mq;
	struct srm_bmap_release_rep *mp;
	struct fidc_membh *f;
	struct bmapc_memb *b;
	struct srm_bmap_id *bid;
	struct bmap_mds_lease *bml;
	struct slash_fidgen fg;
	uint32_t i;

	SL_RSX_ALLOCREP(rq, mq, mp);
	if (mq->nbmaps > MAX_BMAP_RELEASE) {
		mp->rc = EINVAL;
		return (0);
	}

	for (i = 0; i < mq->nbmaps; i++) {
		bid = &mq->bmaps[i];
		if (!sliod) {
			bid->cli_nid = rq->rq_conn->c_peer.nid;
			bid->cli_pid = rq->rq_conn->c_peer.pid;
		}

		fg.fg_fid = bid->fid;
		fg.fg_gen = 0;

		mp->bidrc[i] = slm_fcmh_get(&fg, &f);
		if (mp->bidrc[i]) {
			//XXX
			continue;
		}

		DEBUG_FCMH(PLL_INFO, f, "rls bmap=%u", bid->bmapno);

		mp->bidrc[i] = bmap_lookup(f, bid->bmapno, &b);
		if (mp->bidrc[i])
			goto next;

		BMAP_LOCK(b);

		bml = mds_bmap_getbml(b, bid->cli_nid, bid->cli_pid,
		    bid->seq);

		DEBUG_BMAP((bml ? PLL_INFO : PLL_WARN), b,
			   "release %"PRId64" nid=%"PRId64" pid=%u bml=%p",
			   bid->seq, bid->cli_nid, bid->cli_pid, bml);

		if (bml) {
			psc_assert(bid->seq == bml->bml_seq);
			BML_LOCK(bml);
			if (!(bml->bml_flags & BML_FREEING)) {
				bml->bml_flags |= BML_FREEING;
				BML_ULOCK(bml);
				mp->bidrc[i] = mds_bmap_bml_release(bml);
			} else {
				BML_ULOCK(bml);
				mp->bidrc[i] = 0;
			}
		}
		/* bmap_op_done_type will drop the lock.
		 */
		bmap_op_done_type(b, BMAP_OPCNT_LOOKUP);
 next:
		fcmh_op_done_type(f, FCMH_OPCNT_LOOKUP_FIDC);
	}
	return (0);
}

static __inline struct bmap_mds_lease *
mds_bml_get(void)
{
	struct bmap_mds_lease *bml;

	bml = psc_pool_get(bmapMdsLeasePool);
	memset(bml, 0, sizeof(*bml));
	INIT_PSC_LISTENTRY(&bml->bml_bmdsi_lentry);
	INIT_PSC_LISTENTRY(&bml->bml_timeo_lentry);
	INIT_PSC_LISTENTRY(&bml->bml_exp_lentry);
	INIT_PSC_LISTENTRY(&bml->bml_coh_lentry);
	INIT_SPINLOCK(&bml->bml_lock);
	return (bml);
}

void
mds_bia_odtable_startup_cb(void *data, struct odtable_receipt *odtr)
{
	struct fidc_membh *f = NULL;
	struct bmapc_memb *b = NULL;
	struct bmap_mds_lease *bml;
	struct slash_fidgen fg;
	struct bmap_ion_assign *bia;
	struct sl_resm *resm;
	int rc;

	bia = data;

	resm = libsl_nid2resm(bia->bia_ion_nid);

	psclog_debug("fid=%"PRId64" seq=%"PRId64" res=(%s) ion=(%s) bmapno=%u",
	    bia->bia_fid, bia->bia_seq, resm->resm_res->res_name,
	    libcfs_nid2str(bia->bia_ion_nid), bia->bia_bmapno);

	if (!bia->bia_fid) {
		psc_warnx("found fid #0 in odtable");
		rc = -EINVAL;
		goto out;
	}

#if 0
	/* For the time being replay all bmap leases so that any ION's
	 *   with dirty crc's and size updates may send us those requests
	 *   while the lease is valid.
	 */
	if ((time() - bia->bia_start) >= BMAP_TIMEO_MAX) {
		/* Don't bother with ancient leases.
		 */
		psc_warnx("bia timed out, ignoring: fid=%"PRId64" seq=%"PRId64
			  " res=(%s) ion=(%s) bmapno=%u",
			  bia->bia_fid, bia->bia_seq, resm->resm_res->res_name,
			  libcfs_nid2str(bia->bia_ion_nid), bia->bia_bmapno);
		rc = -ETIMEDOUT;
		goto out;
	}
#endif

	fg.fg_fid = bia->bia_fid;
	fg.fg_gen = FGEN_ANY;

	rc = slm_fcmh_get(&fg, &f);
	if (rc) {
		psc_errorx("fid=%"PRId64" failed to load", fg.fg_fid);
		goto out;
	}

	rc = mds_bmap_load(f, bia->bia_bmapno, &b);
	if (rc) {
		DEBUG_FCMH(PLL_ERROR, f, "failed to load bmap %u (rc=%d)",
			   bia->bia_bmapno, rc);
		goto out;
	}

	bml = mds_bml_get();
	bml->bml_bmdsi = bmap_2_bmdsi(b);
	bml->bml_flags = BML_WRITE | BML_RECOVER;
	bml->bml_seq = bia->bia_seq;
	bml->bml_cli_nidpid = bia->bia_lastcli;
	bml->bml_ion_nid = bia->bia_ion_nid;
	bml->bml_start = bia->bia_start;

	if (bia->bia_flags & BIAF_DIO) {
		bml->bml_flags |= BML_CDIO;
		b->bcm_flags |= BMAP_DIO;
	}

	bmap_2_bmdsi(b)->bmdsi_assign = odtr;

	rc = mds_bmap_bml_add(bml, SL_WRITE, IOS_ID_ANY);
	if (rc) {
		bmap_2_bmdsi(b)->bmdsi_assign = NULL;
		bml->bml_flags |= BML_FREEING;
		mds_bmap_bml_release(bml);
		goto out;
	}

 out:
	if (rc)
		odtable_freeitem(mdsBmapAssignTable, odtr);
	if (f)
		fcmh_op_done_type(f, FCMH_OPCNT_LOOKUP_FIDC);
	if (b)
		bmap_op_done_type(b, BMAP_OPCNT_LOOKUP);
}

/**
 * mds_bmap_crc_write - Process a CRC update request from an ION.
 * @c: the RPC request containing the FID, bmapno, and chunk ID (cid).
 * @ion_nid:  the network ID of the I/O node which sent the request.  It is
 *	compared against the ID stored in the bmdsi.
 */
int
mds_bmap_crc_write(struct srm_bmap_crcup *c, lnet_nid_t ion_nid)
{
	struct bmap_mds_info *bmdsi;
	struct fidc_membh *fcmh;
	struct bmapc_memb *bmap = NULL;
	int rc;

	rc = slm_fcmh_get(&c->fg, &fcmh);
	if (rc) {
		if (rc == ENOENT) {
			psc_warnx("fid="SLPRI_FID" appears to have been deleted",
			    c->fg.fg_fid);
			return (0);
		}
		psc_errorx("fid="SLPRI_FID" slm_fcmh_get() rc=%d",
		    c->fg.fg_fid, rc);
		return (-rc);
	}

	/* Ignore updates from old or invalid generation numbers.
	 */
	if (fcmh_2_gen(fcmh) != c->fg.fg_gen) {
		int x = (fcmh_2_gen(fcmh) > c->fg.fg_gen) ? 1 : 0;

		DEBUG_FCMH(x ? PLL_WARN : PLL_ERROR, fcmh,
		   "mds gen (%"PRIu64") %s than crcup gen (%"PRIu64")",
		   fcmh_2_gen(fcmh), x ? ">" : "<", c->fg.fg_gen);

		rc = -(x ? SLERR_GEN_OLD : SLERR_GEN_INVALID);
		goto out;
	}

	/* BMAP_OP #2
	 */
	rc = bmap_lookup(fcmh, c->blkno, &bmap);
	if (rc) {
		DEBUG_FCMH(PLL_ERROR, fcmh, "failed lookup bmap(%u) rc=%d",
		    c->blkno, rc);
		rc = -EBADF;
		goto out;
	}
	BMAP_LOCK(bmap);

	DEBUG_BMAP(PLL_DEBUG, bmap, "blkno=%u sz=%"PRId64" ion=%s",
	    c->blkno, c->fsize, libcfs_nid2str(ion_nid));

	psc_assert(psc_atomic32_read(&bmap->bcm_opcnt) > 1);

	bmdsi = bmap_2_bmdsi(bmap);
	/* These better check out.
	 */
	psc_assert(bmap->bcm_fcmh == fcmh);
	psc_assert(bmdsi);

	if (!bmdsi->bmdsi_wr_ion ||
	    ion_nid != bmdsi->bmdsi_wr_ion->rmmi_resm->resm_nid) {
		/* Whoops, we recv'd a request from an unexpected nid.
		 */
		rc = -EINVAL;
		BMAP_ULOCK(bmap);
		goto out;

	} else if (bmap->bcm_flags & BMAP_MDS_CRC_UP) {
		/* Ensure that this thread is the only thread updating the
		 *  bmap crc table.  XXX may have to replace this with a waitq
		 */
		rc = -EALREADY;
		BMAP_ULOCK(bmap);

		DEBUG_BMAP(PLL_ERROR, bmap, "EALREADY blkno=%u sz=%"PRId64
			   " ion=%s", c->blkno, c->fsize,
			   libcfs_nid2str(ion_nid));

		DEBUG_FCMH(PLL_ERROR, fcmh, "EALREADY blkno=%u sz=%"PRId64
			   " ion=%s", c->blkno, c->fsize,
			   libcfs_nid2str(ion_nid));
		goto out;

	} else {
		/* Mark that bmap is undergoing crc updates - this is non-
		 *  reentrant so the ION must know better than to send
		 *  multiple requests for the same bmap.
		 */
		bmap->bcm_flags |= BMAP_MDS_CRC_UP;
	}

	/*  mds_repl_inv_except() takes the lock.
	 *  This shouldn't be racy because
	 *   . only one export may be here (ion_nid)
	 *   . the bmap is locked.
	 * Note the lock ordering here BMAP -> INODEH
	 */
	//if ((rc = mds_repl_inv_except(bmap,
	//    resm_2_resid(bmdsi->bmdsi_wr_ion->rmmi_resm)))) {
	//	BMAP_ULOCK(bmap);
	//	goto out;
	//}

	// XXX replace above with something like this.
	//   Verify the active IOS, dont' set it here.
	//if (mds_repl_bmap_walk(bcm, tract, retifset,
	//    REPL_WALKF_MODOTH, &iosidx, 1))

	/* XXX ok if replicas exist, the gen has to be bumped and the
	 *  replication bmap modified.
	 *  Schedule the bmap for writing.
	 */
	BMAP_ULOCK(bmap);
	/* Call the journal and update the in-memory crc's.
	 */
	mds_bmap_crc_update(bmap, c);
 out:
	/* Mark that mds_bmap_crc_write() is done with this bmap
	 *  - it was incref'd in fcmh_bmap_lookup().
	 */
	if (bmap)
		/* BMAP_OP #2, drop lookup ref
		 */
		bmap_op_done_type(bmap, BMAP_OPCNT_LOOKUP);

	fcmh_op_done_type(fcmh, FCMH_OPCNT_LOOKUP_FIDC);
	return (rc);
}

/**
 * mds_bmapod_initnew - Called when a read request offset exceeds the
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
mds_bmapod_initnew(struct bmap_ondisk *bod)
{
	int i;

	for (i = 0; i < SLASH_CRCS_PER_BMAP; i++)
		bod->bod_crcs[i] = BMAP_NULL_CRC;

	psc_crc64_calc(&bod->bod_crc, bod, BMAP_OD_CRCSZ);
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

	/* pread() the bmap from the meta file.
	 */
	rc = mdsio_bmap_read(bcm);

	/*
	 * Check for a NULL CRC if we had a good read.  NULL CRC can happen
	 *    when bmaps are gaps that have not been written yet.   Note
	 *    that a short read is tolerated as long as the bmap is zeroed.
	 */
	if (!rc || rc == SLERR_SHORTIO) {
		if (bmap_2_ondiskcrc(bcm) == 0 &&
		    memcmp(bmap_2_ondisk(bcm), &null_bmap_od,
		    sizeof(null_bmap_od)) == 0) {
			DEBUG_BMAPOD(PLL_INFO, bcm, "");
			mds_bmapod_initnew(bmap_2_ondisk(bcm));
			DEBUG_BMAPOD(PLL_INFO, bcm, "");
			return (0);
		}
	}

	/* At this point, the short I/O is an error since the bmap isn't
	 *    zeros.
	 */
	if (rc) {
		DEBUG_FCMH(PLL_ERROR, f, "mdsio_bmap_read: "
		    "bmapno=%u, rc=%d", bcm->bcm_bmapno, rc);
		rc = -EIO;
		goto out;
	}

	DEBUG_BMAPOD(PLL_INFO, bcm, "");
	return (0);
 out:
	return (rc);
}

void
mds_bmap_init(struct bmapc_memb *bcm)
{
	struct bmap_mds_info *bmdsi;

	bmdsi = bmap_2_bmdsi(bcm);
	pll_init(&bmdsi->bmdsi_leases, struct bmap_mds_lease,
	    bml_bmdsi_lentry, &bcm->bcm_lock);
	bmdsi->bmdsi_xid = 0;
	psc_pthread_rwlock_init(&bmdsi->bmdsi_rwlock);
}

void
mds_bmap_destroy(struct bmapc_memb *bcm)
{
	struct bmap_mds_info *bmdsi = bmap_2_bmdsi(bcm);

	psc_assert(bmdsi->bmdsi_writers == 0);
	psc_assert(bmdsi->bmdsi_readers == 0);
	psc_assert(bmdsi->bmdsi_assign == NULL);
	psc_assert(pll_empty(&bmdsi->bmdsi_leases));
	psc_pthread_rwlock_destroy(&bmdsi->bmdsi_rwlock);
}

/**
 * mds_bmap_loadvalid - Load a bmap if disk I/O is successful and the bmap
 *	has been initialized (i.e. is not all zeroes).
 * @f: fcmh.
 * @bmapno: bmap index number to load.
 * @bp: value-result bmap pointer.
 * NOTE: callers must issue bmap_op_done() if mds_bmap_loadvalid() is
 *     successful.
 */
int
mds_bmap_loadvalid(struct fidc_membh *f, sl_bmapno_t bmapno,
    struct bmapc_memb **bp)
{
	struct bmapc_memb *b;
	int n, rc;

	*bp = NULL;

	/* BMAP_OP #3 via lookup
	 */
	rc = mds_bmap_load(f, bmapno, &b);
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
	bmap_op_done_type(b, BMAP_OPCNT_LOOKUP);
	return (SLERR_BMAP_ZERO);
}

int
mds_bmap_load_ion(const struct slash_fidgen *fg, sl_bmapno_t bmapno,
    struct bmapc_memb **bp)
{
	struct fidc_membh *f;
	struct bmapc_memb *b;
	int rc = 0;

	psc_assert(*bp == NULL);

	f = fidc_lookup_fg(fg);
	if (!f)
		return (-ENOENT);

	rc = mds_bmap_load(f, bmapno, &b);
	if (rc == 0)
		*bp = b;

	fcmh_op_done_type(f, FCMH_OPCNT_LOOKUP_FIDC);
	return (rc);
}

/**
 * mds_bmap_load_cli - Routine called to retrieve a bmap, presumably so that
 *	it may be sent to a client.  It first checks for existence in
 *	the cache, if needed, the bmap is retrieved from disk.
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
    struct pscrpc_export *exp, struct bmapc_memb **bmap)
{
	struct bmap_mds_lease *bml;
	struct slm_exp_cli *mexpc;
	struct bmapc_memb *b;
	int rc;

	psc_assert(!*bmap);

	rc = mds_bmap_load(f, bmapno, &b);
	if (rc)
		return (rc);

	bml = mds_bml_get();
	bml->bml_exp = exp;
	bml->bml_bmdsi = bmap_2_bmdsi(b);
	bml->bml_flags = (rw == SL_WRITE ? BML_WRITE : BML_READ);
	bml->bml_cli_nidpid = exp->exp_connection->c_peer;

	if (flags & SRM_LEASEBMAPF_DIRECTIO)
		bml->bml_flags |= BML_CDIO;

	EXPORT_LOCK(exp);
	mexpc = mexpc_get(exp);
	bml->bml_flags |= BML_EXP;
	psclist_add_tail(&bml->bml_exp_lentry, &mexpc->mexpc_bmlhd);
	EXPORT_ULOCK(exp);

	rc = mds_bmap_bml_add(bml, rw, prefios);
	if (rc) {
		if (rc == SLERR_BMAP_DIOWAIT) {
			psclist_del(&bml->bml_exp_lentry, &mexpc->mexpc_bmlhd);
			mds_bml_free(bml);
		} else {
			if (rc == SLERR_ION_OFFLINE)
				bml->bml_flags |= BML_ASSFAIL;
			bml->bml_flags |= BML_FREEING;
			mds_bmap_bml_release(bml);
		}
		goto out;
	}
	*bmap = b;

	sbd->sbd_seq = bml->bml_seq;
	sbd->sbd_key = (rw == SL_WRITE) ?
	    bml->bml_bmdsi->bmdsi_assign->odtr_key : BMAPSEQ_ANY;
 out:
	bmap_op_done_type(b, BMAP_OPCNT_LOOKUP);
	return (rc);
}

void
dump_bmap_flags_mds(uint32_t flags)
{
	int seq = 0;

	dump_bmap_flags_common(&flags, &seq);
	PFL_PRFLAG(BMAP_MDS_CRC_UP, flags, &seq);
	PFL_PRFLAG(BMAP_MDS_CRCWRT, flags, &seq);
	PFL_PRFLAG(BMAP_MDS_NOION, flags, &seq);
	PFL_PRFLAG(BMAP_MDS_LOGCHG, flags, &seq);
	PFL_PRFLAG(BMAP_MDS_DIO, flags, &seq);
	PFL_PRFLAG(BMAP_MDS_SEQWRAP, flags, &seq);
	if (flags)
		printf(" unknown: %#x\n", flags);
	printf("\n");
}

struct bmap_ops bmap_ops = {
	mds_bmap_init,
	mds_bmap_read,
	NULL,
	mds_bmap_destroy
};
