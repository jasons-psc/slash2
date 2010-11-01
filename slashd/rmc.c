/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2007-2010, Pittsburgh Supercomputing Center (PSC).
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

/*
 * Routines for handling RPC requests for MDS from CLIENT.
 */

#define PSC_SUBSYS PSS_RPC

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/statvfs.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <unistd.h>

#include "pfl/fs.h"
#include "pfl/str.h"
#include "psc_rpc/export.h"
#include "psc_rpc/rpc.h"
#include "psc_rpc/rpclog.h"
#include "psc_rpc/rsx.h"
#include "psc_rpc/service.h"
#include "psc_util/lock.h"

#include "authbuf.h"
#include "bmap_mds.h"
#include "fidc_mds.h"
#include "fidcache.h"
#include "mdsio.h"
#include "mkfn.h"
#include "pathnames.h"
#include "repl_mds.h"
#include "rpc_mds.h"
#include "slashd.h"
#include "slashrpc.h"
#include "slerr.h"
#include "slutil.h"
#include "up_sched_res.h"

uint64_t next_slash_id;
static psc_spinlock_t slash_id_lock = SPINLOCK_INIT;

uint64_t
slm_get_curr_slashid(void)
{
	uint64_t slid;

	spinlock(&slash_id_lock);
	slid = next_slash_id;
	freelock(&slash_id_lock);
	return (slid);
}

void
slm_set_curr_slashid(uint64_t slfid)
{
	spinlock(&slash_id_lock);
	next_slash_id = slfid;
	freelock(&slash_id_lock);
}

/*
 * slm_get_next_slashid - Return the next SLASH FID to use.  Note that from ZFS
 *     point of view, it is perfectly okay that we use the same SLASH FID to
 *     refer to different files/directories.  However, doing so can confuse
 *     our clients (think identity theft).  So we must make sure that we never
 *     reuse a SLASH FID, even after a crash.
 */
uint64_t
slm_get_next_slashid(void)
{
	uint64_t slid;

	spinlock(&slash_id_lock);
	slid = next_slash_id++;
	if (next_slash_id >= (UINT64_C(1) << SLASH_ID_FID_BITS))
		next_slash_id = SLFID_MIN;
	freelock(&slash_id_lock);

	return (slid | ((uint64_t)nodeResm->resm_site->site_id <<
	    SLASH_ID_FID_BITS));
}

int
slm_rmc_handle_connect(struct pscrpc_request *rq)
{
	struct pscrpc_export *e = rq->rq_export;
	struct srm_connect_req *mq;
	struct srm_generic_rep *mp;

	SL_RSX_ALLOCREP(rq, mq, mp);
	if (mq->magic != SRMC_MAGIC || mq->version != SRMC_VERSION)
		mp->rc = EINVAL;
	psc_assert(e->exp_private == NULL);
	mexpc_get(e);
	return (0);
}

int
slm_rmc_handle_ping(struct pscrpc_request *rq)
{
	struct srm_generic_rep *mp;
	struct srm_ping_req *mq;

	SL_RSX_ALLOCREP(rq, mq, mp);
	return (0);
}

int
slm_rmc_handle_getattr(struct pscrpc_request *rq)
{
	const struct srm_getattr_req *mq;
	struct srm_getattr_rep *mp;
	struct fidc_membh *fcmh;

	SL_RSX_ALLOCREP(rq, mq, mp);
	mp->rc = slm_fcmh_get(&mq->fg, &fcmh);
	if (mp->rc)
		goto out;

	FCMH_LOCK(fcmh);
	mp->attr = fcmh->fcmh_sstb;
	FCMH_ULOCK(fcmh);
 out:
	if (fcmh)
		fcmh_op_done_type(fcmh, FCMH_OPCNT_LOOKUP_FIDC);
	return (0);
}

static void
slm_rmc_bmapdesc_setup(struct bmapc_memb *bmap,
    struct srt_bmapdesc *sbd, enum rw rw)
{
	sbd->sbd_fg = bmap->bcm_fcmh->fcmh_fg;
	sbd->sbd_bmapno = bmap->bcm_bmapno;
	if (bmap->bcm_flags & BMAP_DIO)
		sbd->sbd_flags |= SRM_LEASEBMAPF_DIRECTIO;

	if (rw == SL_WRITE) {
		struct bmap_mds_info *bmdsi = bmap_2_bmdsi(bmap);

		psc_assert(bmdsi->bmdsi_wr_ion);
		sbd->sbd_ion_nid = bmdsi->bmdsi_wr_ion->rmmi_resm->resm_nid;
		sbd->sbd_ios_id = bmdsi->bmdsi_wr_ion->rmmi_resm->resm_res->res_id;
	} else {
		sbd->sbd_ion_nid = LNET_NID_ANY;
		sbd->sbd_ios_id = IOS_ID_ANY;
	}
}

/**
 * slm_rmc_handle_bmap_chwrmode - Handle a BMAPCHWRMODE request to
 *	upgrade a client bmap lease from READ-only to READ+WRITE.
 * @rq: RPC request.
 */
int
slm_rmc_handle_bmap_chwrmode(struct pscrpc_request *rq)
{
	const struct srm_bmap_chwrmode_req *mq;
	struct srm_bmap_chwrmode_rep *mp;
	struct fidc_membh *f = NULL;
	struct bmapc_memb *b = NULL;
	struct bmap_mds_info *bmdsi;
	struct bmap_mds_lease *bml;

	SL_RSX_ALLOCREP(rq, mq, mp);
	mp->rc = slm_fcmh_get(&mq->sbd.sbd_fg, &f);
	if (mp->rc)
		goto out;
	mp->rc = bmap_lookup(f, mq->sbd.sbd_bmapno, &b);
	if (mp->rc)
		goto out;

	bmdsi = bmap_2_bmdsi(b);

	BMAP_LOCK(b);
	bml = mds_bmap_getbml(b, rq->rq_conn->c_peer.nid,
	    rq->rq_conn->c_peer.pid, mq->sbd.sbd_seq);
	if (bml == NULL) {
		mp->rc = EINVAL;
		goto out;
	}

	mp->rc = mds_bmap_bml_chwrmode(bml, mq->prefios);
	if (mp->rc == EALREADY)
		mp->rc = 0;
	else if (mp->rc)
		goto out;

	mp->sbd = mq->sbd;
	mp->sbd.sbd_seq = bml->bml_seq;
	mp->sbd.sbd_key = bmdsi->bmdsi_assign->odtr_key;

	psc_assert(bmdsi->bmdsi_wr_ion);
	mp->sbd.sbd_ion_nid = bmdsi->bmdsi_wr_ion->rmmi_resm->resm_nid;
	mp->sbd.sbd_ios_id = bmdsi->bmdsi_wr_ion->rmmi_resm->resm_res->res_id;

 out:
	if (b)
		bmap_op_done_type(b, BMAP_OPCNT_LOOKUP);
	if (f)
		fcmh_op_done_type(f, FCMH_OPCNT_LOOKUP_FIDC);
	return (0);
}

int
slm_rmc_handle_getbmap(struct pscrpc_request *rq)
{
	int rc = 0;
	const struct srm_leasebmap_req *mq;
	struct srm_leasebmap_rep *mp;
	struct fidc_membh *fcmh;
	struct bmapc_memb *bmap=NULL;
	struct bmap_mds_info *bmdsi;

	SL_RSX_ALLOCREP(rq, mq, mp);
	mp->rc = slm_fcmh_get(&mq->fg, &fcmh);
	if (mp->rc)
		return (mp->rc);
	mp->flags = mq->flags;

	if (mq->rw != SL_READ && mq->rw != SL_WRITE)
		return ((mp->rc = EINVAL));

	bmap = NULL;
	mp->rc = mds_bmap_load_cli(fcmh, mq->bmapno, mq->flags, mq->rw,
	   mq->prefios, &mp->sbd, rq->rq_export, &bmap);
	if (mp->rc)
		return (mp->rc == SLERR_BMAP_DIOWAIT ? 0 : mp->rc);

	bmdsi = bmap_2_bmdsi(bmap);

	if (mq->flags & SRM_LEASEBMAPF_DIRECTIO)
		mp->sbd.sbd_flags |= SRM_LEASEBMAPF_DIRECTIO;

	slm_rmc_bmapdesc_setup(bmap, &mp->sbd, mq->rw);

	memcpy(&mp->bcs, &bmap->bcm_corestate, sizeof(mp->bcs));

	if (mp->flags & SRM_LEASEBMAPF_GETREPLTBL) {
		struct slash_inode_handle *ih;

		ih = fcmh_2_inoh(fcmh);
		mp->nrepls = ih->inoh_ino.ino_nrepls;
		memcpy(&mp->reptbl[0], &ih->inoh_ino.ino_repls,
		    sizeof(ih->inoh_ino.ino_repls));

		if (mp->nrepls > SL_DEF_REPLICAS) {
			rc = mds_inox_ensure_loaded(ih);
			if (!rc)
				memcpy(&mp->reptbl[SL_DEF_REPLICAS],
				    &ih->inoh_extras->inox_repls,
				    sizeof(ih->inoh_extras->inox_repls));
		}
	}

	fcmh_op_done_type(fcmh, FCMH_OPCNT_LOOKUP_FIDC);

	return (rc);
}

int
slm_rmc_handle_link(struct pscrpc_request *rq)
{
	struct fidc_membh *p, *c;
	struct srm_link_req *mq;
	struct srm_link_rep *mp;

	p = c = NULL;
	SL_RSX_ALLOCREP(rq, mq, mp);
	mp->rc = slm_fcmh_get(&mq->fg, &c);
	if (mp->rc)
		goto out;
	mp->rc = slm_fcmh_get(&mq->pfg, &p);
	if (mp->rc)
		goto out;

	mq->name[sizeof(mq->name) - 1] = '\0';
	mds_reserve_slot();
	mp->rc = mdsio_link(fcmh_2_mdsio_fid(c), fcmh_2_mdsio_fid(p),
	    mq->name, &mq->creds, &mp->attr, mds_namespace_log);
	mds_unreserve_slot();
 out:
	if (c)
		fcmh_op_done_type(c, FCMH_OPCNT_LOOKUP_FIDC);
	if (p)
		fcmh_op_done_type(p, FCMH_OPCNT_LOOKUP_FIDC);
	return (0);
}

int
slm_rmc_handle_lookup(struct pscrpc_request *rq)
{
	struct srm_lookup_req *mq;
	struct srm_lookup_rep *mp;
	struct fidc_membh *p;

	SL_RSX_ALLOCREP(rq, mq, mp);
	mp->rc = slm_fcmh_get(&mq->pfg, &p);
	if (mp->rc)
		goto out;

	mq->name[sizeof(mq->name) - 1] = '\0';
	if (fcmh_2_mdsio_fid(p) == SLFID_ROOT &&
	    strncmp(mq->name, SL_PATH_PREFIX,
	     strlen(SL_PATH_PREFIX)) == 0) {
		mp->rc = EINVAL;
		goto out;
	}
	mp->rc = mdsio_lookup(fcmh_2_mdsio_fid(p),
	    mq->name, NULL, &rootcreds, &mp->attr);

 out:
	if (p)
		fcmh_op_done_type(p, FCMH_OPCNT_LOOKUP_FIDC);
	return (0);
}

int
slm_rmc_handle_mkdir(struct pscrpc_request *rq)
{
	struct srm_mkdir_req *mq;
	struct srm_mkdir_rep *mp;
	struct fidc_membh *fcmh;

	SL_RSX_ALLOCREP(rq, mq, mp);
	mp->rc = slm_fcmh_get(&mq->pfg, &fcmh);
	if (mp->rc)
		goto out;

	mq->name[sizeof(mq->name) - 1] = '\0';
	mds_reserve_slot();
	mp->rc = mdsio_mkdir(fcmh_2_mdsio_fid(fcmh), mq->name, mq->mode,
	    &mq->creds, &mp->attr, NULL, mds_namespace_log,
	    slm_get_next_slashid);
	mds_unreserve_slot();
 out:
	if (fcmh)
		fcmh_op_done_type(fcmh, FCMH_OPCNT_LOOKUP_FIDC);
	return (0);
}

int
slm_rmc_handle_create(struct pscrpc_request *rq)
{
	struct fidc_membh *p, *fcmh;
	struct srm_create_rep *mp;
	struct srm_create_req *mq;
	struct bmapc_memb *bmap;
	void *mdsio_data;

	p = NULL;

	SL_RSX_ALLOCREP(rq, mq, mp);
	if (mq->flags & SRM_LEASEBMAPF_GETREPLTBL) {
		mp->rc = EINVAL;
		goto out;
	}

	/* Lookup the parent directory in the cache so that the
	 *   slash2 ino can be translated into the inode for the
	 *   underlying fs.
	 */
	mp->rc = slm_fcmh_get(&mq->pfg, &p);
	if (mp->rc)
		goto out;

	mq->name[sizeof(mq->name) - 1] = '\0';

//	DEBUG_FCMH(PLL_WARN, p, "create op start for %s", mq->name);

	mds_reserve_slot();
	mp->rc = mdsio_opencreate(fcmh_2_mdsio_fid(p), &mq->creds,
	    O_CREAT | O_EXCL | O_RDWR, mq->mode, mq->name, NULL,
	    &mp->attr, &mdsio_data, mds_namespace_log,
	    slm_get_next_slashid);
	mds_unreserve_slot();

	if (mp->rc)
		goto out;

//	DEBUG_FCMH(PLL_WARN, p, "create op done for %s", mq->name);
	/* XXX enter this into the fcmh cache instead of doing it again
	 *   This release may be the sanest thing actually, unless EXCL is
	 *   used.
	 */
	if (mp->rc == 0)
		mdsio_release(&rootcreds, mdsio_data);

	mp->rc2 = slm_fcmh_get(&mp->attr.sst_fg, &fcmh);
	if (mp->rc2)
		goto out;
//	DEBUG_FCMH(PLL_WARN, p, "release op done for %s", mq->name);

	mp->flags = mq->flags;

	bmap = NULL;
	mp->rc2 = mds_bmap_load_cli(fcmh, 0, mp->flags, SL_WRITE,
			    mq->prefios, &mp->sbd, rq->rq_export, &bmap);
	if (mp->rc2) {
		fcmh_op_done_type(fcmh, FCMH_OPCNT_LOOKUP_FIDC);
		goto out;
	}
	slm_rmc_bmapdesc_setup(bmap, &mp->sbd, SL_WRITE);

	fcmh_op_done_type(fcmh, FCMH_OPCNT_LOOKUP_FIDC);
 out:
	if (p)
		fcmh_op_done_type(p, FCMH_OPCNT_LOOKUP_FIDC);
	return (0);
}

int
slm_rmc_handle_readdir(struct pscrpc_request *rq)
{
	struct pscrpc_bulk_desc *desc;
	struct srm_readdir_req *mq;
	struct srm_readdir_rep *mp;
	struct fidc_membh *fcmh;
	struct iovec iov[2];
	size_t outsize, nents;
	int niov;

	SL_RSX_ALLOCREP(rq, mq, mp);

	mp->rc = slm_fcmh_get(&mq->fg, &fcmh);
	if (mp->rc)
		goto out2;

	if (mq->size > MAX_READDIR_BUFSIZ ||
	    mq->nstbpref > MAX_READDIR_NENTS) {
		mp->rc = EINVAL;
		goto out2;
	}

	iov[0].iov_base = PSCALLOC(mq->size);
	iov[0].iov_len = mq->size;

	niov = 1;
	if (mq->nstbpref) {
		niov++;
		iov[1].iov_len = mq->nstbpref * sizeof(struct srm_getattr_rep);
		iov[1].iov_base = PSCALLOC(iov[1].iov_len);
	} else {
		iov[1].iov_len = 0;
		iov[1].iov_base = NULL;
	}

	mp->rc = mdsio_readdir(&rootcreds, mq->size, mq->offset,
	       iov[0].iov_base, &outsize, &nents, iov[1].iov_base,
	       mq->nstbpref, fcmh_2_mdsio_data(fcmh));
	mp->size = outsize;
	mp->num = nents;

	psc_info("mdsio_readdir: rc=%d, data=%p", mp->rc,
	    fcmh_2_mdsio_data(fcmh));

	if (mp->rc)
		goto out1;

#if 0
	{
		/* debugging only */
		unsigned int i;
		struct srm_getattr_rep *attr;
		attr = iov[1].iov_base;
		for (i = 0; i < mq->nstbpref; i++, attr++) {
			if (attr->rc || !attr->attr.sst_ino)
				break;
			psc_info("reply: i+g:%"PRIx64"+%"PRIx32", mode=0%o",
				attr->attr.sst_ino, attr->attr.sst_gen,
				attr->attr.sst_mode);
		}
	}
#endif

	mp->rc = rsx_bulkserver(rq, &desc,
	    BULK_PUT_SOURCE, SRMC_BULK_PORTAL, iov, niov);

	if (desc)
		pscrpc_free_bulk(desc);

 out1:
	PSCFREE(iov[0].iov_base);
	PSCFREE(iov[1].iov_base);
 out2:
	if (fcmh)
		fcmh_op_done_type(fcmh, FCMH_OPCNT_LOOKUP_FIDC);
	return (mp->rc);
}

int
slm_rmc_handle_readlink(struct pscrpc_request *rq)
{
	struct pscrpc_bulk_desc *desc;
	struct srm_readlink_req *mq;
	struct srm_readlink_rep *mp;
	struct fidc_membh *fcmh;
	struct iovec iov;
	char buf[PATH_MAX];

	SL_RSX_ALLOCREP(rq, mq, mp);
	mp->rc = slm_fcmh_get(&mq->fg, &fcmh);
	if (mp->rc)
		goto out;

	mp->rc = mdsio_readlink(fcmh_2_mdsio_fid(fcmh), buf, &rootcreds);
	if (mp->rc)
		goto out;

	iov.iov_base = buf;
	iov.iov_len = sizeof(buf);
	mp->rc = rsx_bulkserver(rq, &desc, BULK_PUT_SOURCE,
	    SRMC_BULK_PORTAL, &iov, 1);
	if (desc)
		pscrpc_free_bulk(desc);

 out:
	if (fcmh)
		fcmh_op_done_type(fcmh, FCMH_OPCNT_LOOKUP_FIDC);

	return (mp->rc);
}

int
slm_rmc_handle_rls_bmap(struct pscrpc_request *rq)
{
	return (mds_handle_rls_bmap(rq, 0));
}

int
slm_rmc_handle_rename(struct pscrpc_request *rq)
{
	char from[NAME_MAX+1], to[NAME_MAX+1];
	struct pscrpc_bulk_desc *desc;
	struct fidc_membh *op, *np;
	struct srm_generic_rep *mp;
	struct srm_rename_req *mq;
	struct iovec iov[2];

	op = np = NULL;
	SL_RSX_ALLOCREP(rq, mq, mp);
	if (mq->fromlen <= 1 ||
	    mq->tolen <= 1) {
		mp->rc = -ENOENT;
		return (0);
	}
	if (mq->fromlen > NAME_MAX ||
	    mq->tolen > NAME_MAX) {
		mp->rc = -EINVAL;
		return (0);
	}

	mp->rc = slm_fcmh_get(&mq->opfg, &op);
	if (mp->rc)
		goto out;

	mp->rc = slm_fcmh_get(&mq->npfg, &np);
	if (mp->rc)
		goto out;

	iov[0].iov_base = from;
	iov[0].iov_len = mq->fromlen;
	iov[1].iov_base = to;
	iov[1].iov_len = mq->tolen;

	mp->rc = rsx_bulkserver(rq, &desc, BULK_GET_SINK,
	    SRMC_BULK_PORTAL, iov, 2);
	if (mp->rc)
		goto out;
	pscrpc_free_bulk(desc);

	from[sizeof(from) - 1] = '\0';
	to[sizeof(to) - 1] = '\0';
	/*
	 * Steps for rename (we may have to perform some steps by sending
	 * a request to a remote MDS:
	 *
	 * (1) Remove the exiting target if any.
	 * (2) Create the new target in place.
	 * (3) Remove the old object.
	 *
	 * Whoever performs a step should log before proceed.
	 */

	/* if we get here, op and np must be owned by the current MDS */
	mp->rc = mdsio_rename(fcmh_2_mdsio_fid(op), from,
	    fcmh_2_mdsio_fid(np), to, &rootcreds, mds_namespace_log);

 out:
	if (np)
		fcmh_op_done_type(np, FCMH_OPCNT_LOOKUP_FIDC);
	if (op)
		fcmh_op_done_type(op, FCMH_OPCNT_LOOKUP_FIDC);
	return (mp->rc);
}

__static void
ptrunc_tally_ios(struct bmapc_memb *bcm, int iosidx, int val, void *arg)
{
	struct {
		sl_replica_t	iosv[SL_MAX_REPLICAS];
		int		nios;
	} *ios_list = arg;
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

	ios_id = bmap_2_repl(bcm, iosidx);

	for (i = 0; i < ios_list->nios; i++)
		if (ios_list->iosv[i].bs_id == ios_id)
			return;

	ios_list->iosv[ios_list->nios++].bs_id = ios_id;
}

int
slm_rmc_handle_setattr(struct pscrpc_request *rq)
{
	int tract[NBREPLST], to_set, rc;
	struct up_sched_work_item *wk;
	struct srm_setattr_req *mq;
	struct srm_setattr_rep *mp;
	struct fidc_membh *fcmh;
	struct bmapc_memb *bcm;
	size_t i;

	SL_RSX_ALLOCREP(rq, mq, mp);
	mp->rc = slm_fcmh_get(&mq->attr.sst_fg, &fcmh);
	if (mp->rc)
		goto out;

	to_set = mq->to_set & SL_SETATTRF_CLI_ALL;
	if (to_set & PSCFS_SETATTRF_DATASIZE) {
		if (mq->attr.sst_size == 0) {
			/* full truncate */
			FCMH_LOCK(fcmh);
			fcmh_2_gen(fcmh)++;
			FCMH_ULOCK(fcmh);

			rc = mdsio_fcmh_setattr(fcmh, SL_SETATTRF_GEN);

			/* XXX: queue changelog updates to every IOS replica */
			/* XXX: if file size is already 0, don't bump */
		} else {
			/* partial truncate */
			struct {
				sl_replica_t	iosv[SL_MAX_REPLICAS];
				int		nios;
			} ios_list;

			ios_list.nios = 0;

			to_set |= SL_SETATTRF_PTRUNCGEN;

			/* XXX what do we do with bmap leases that are
			 * concurrently granted?
			 */

			FCMH_LOCK(fcmh);
			fcmh_wait_locked(fcmh,
			    fcmh->fcmh_flags & FMIF_BLOCK_PTRUNC);
			fcmh->fcmh_flags |= FMIF_BLOCK_PTRUNC;
			FCMH_ULOCK(fcmh);

			// bmaps write leases may not be granted
			// for this bmap or any bmap beyond

			brepls_init(tract, -1);
			tract[BREPLST_VALID] = BREPLST_TRUNCPNDG;

			i = mq->attr.sst_size / SLASH_BMAP_SIZE;
			if (mds_bmap_load(fcmh, i, &bcm) == 0) {
				mds_repl_bmap_walk_all(bcm, tract,
				    NULL, 0);
				mds_repl_bmap_rel(bcm);
			}

#if 0
	- synchronously contact an IOS requesting CRC recalculation for sliver and
	  mark BREPLST_VALID on success
	- if BMAP_PERSIST, notify replication queuer
#endif

			brepls_init(tract, -1);
			tract[BREPLST_REPL_SCHED] = BREPLST_GARBAGE;
			tract[BREPLST_REPL_QUEUED] = BREPLST_GARBAGE;
			tract[BREPLST_VALID] = BREPLST_GARBAGE;

			for (i++; i < fcmh_2_nbmaps(fcmh); i++) {
				if (mds_bmap_load(fcmh, i, &bcm))
					continue;
				mds_repl_bmap_walkcb(bcm, tract,
				    NULL, 0, ptrunc_tally_ios,
				    &ios_list);
				mds_repl_bmap_rel(bcm);
			}

			rc = uswi_findoradd(&fcmh->fcmh_fg, &wk);
			uswi_enqueue_sites(wk, ios_list.iosv, ios_list.nios);
			uswi_unref(wk);

			FCMH_LOCK(fcmh);
			fcmh_2_ptruncgen(fcmh)++;
			fcmh->fcmh_flags &= ~FMIF_BLOCK_PTRUNC;
			fcmh_wake_locked(fcmh);
			FCMH_ULOCK(fcmh);
		}
	}
	/*
	 * If the file is open, mdsio_data will be valid and used.
	 * Otherwise, it will be NULL, and we'll use the mdsio_fid.
	 */
	mp->rc = mdsio_setattr(fcmh_2_mdsio_fid(fcmh), &mq->attr,
	    to_set, &rootcreds, &mp->attr, fcmh_2_mdsio_data(fcmh),
	    mds_namespace_log);

	if (!mp->rc) {
		FCMH_LOCK(fcmh);
		fcmh->fcmh_sstb = mp->attr;
		FCMH_ULOCK(fcmh);
	}
 out:
	if (fcmh)
		fcmh_op_done_type(fcmh, FCMH_OPCNT_LOOKUP_FIDC);
	return (0);
}

int
slm_rmc_handle_set_newreplpol(struct pscrpc_request *rq)
{
	struct srm_set_newreplpol_req *mq;
	struct slash_inode_handle *ih;
	struct srm_generic_rep *mp;
	struct fidc_membh *fcmh;

	SL_RSX_ALLOCREP(rq, mq, mp);

	if (mq->pol < 0 || mq->pol >= NBRP) {
		mp->rc = EINVAL;
		return (0);
	}

	mp->rc = slm_fcmh_get(&mq->fg, &fcmh);
	if (mp->rc)
		goto out;

	ih = fcmh_2_inoh(fcmh);
	mp->rc = mds_inox_ensure_loaded(ih);
	if (mp->rc == 0) {
		INOH_LOCK(ih);
		ih->inoh_ino.ino_newbmap_policy = mq->pol;
		ih->inoh_flags |= INOH_EXTRAS_DIRTY;
		INOH_ULOCK(ih);
		mds_inode_sync(ih);
	}

 out:
	if (fcmh)
		fcmh_op_done_type(fcmh, FCMH_OPCNT_LOOKUP_FIDC);

	return (0);
}

int
slm_rmc_handle_set_bmapreplpol(struct pscrpc_request *rq)
{
	struct srm_set_bmapreplpol_req *mq;
	struct bmap_mds_info *bmdsi;
	struct srm_generic_rep *mp;
	struct fcmh_mds_info *fmi;
	struct fidc_membh *fcmh;
	struct bmapc_memb *bcm;

	fmi = NULL;

	SL_RSX_ALLOCREP(rq, mq, mp);

	if (mq->pol < 0 || mq->pol >= NBRP) {
		mp->rc = EINVAL;
		return (0);
	}

	mp->rc = slm_fcmh_get(&mq->fg, &fcmh);
	if (mp->rc)
		goto out;

	fmi = fcmh_2_fmi(fcmh);

	if (!mds_bmap_exists(fcmh, mq->bmapno)) {
		mp->rc = SLERR_BMAP_INVALID;
		goto out;
	}
	mp->rc = mds_bmap_load(fcmh, mq->bmapno, &bcm);
	if (mp->rc)
		goto out;

	bmdsi = bmap_2_bmdsi(bcm);

	BHREPL_POLICY_SET(bcm, mq->pol);

	mds_repl_bmap_rel(bcm);

 out:
	if (fcmh)
		fcmh_op_done_type(fcmh, FCMH_OPCNT_LOOKUP_FIDC);

	return (0);
}

int
slm_rmc_handle_statfs(struct pscrpc_request *rq)
{
	struct srm_statfs_req *mq;
	struct srm_statfs_rep *mp;
	struct statvfs sfb;

	SL_RSX_ALLOCREP(rq, mq, mp);
	mp->rc = mdsio_statfs(&sfb);
	sl_externalize_statfs(&sfb, &mp->ssfb);
	return (0);
}

int
slm_rmc_handle_symlink(struct pscrpc_request *rq)
{
	struct pscrpc_bulk_desc *desc;
	struct srm_symlink_req *mq;
	struct srm_symlink_rep *mp;
	struct fidc_membh *p;
	struct iovec iov;
	char linkname[PATH_MAX];

	p = NULL;

	SL_RSX_ALLOCREP(rq, mq, mp);
	mq->name[sizeof(mq->name) - 1] = '\0';
	if (mq->linklen == 0) {
		mp->rc = ENOENT;
		goto out;
	}
	if (mq->linklen >= PATH_MAX) {
		mp->rc = ENAMETOOLONG;
		goto out;
	}

	mp->rc = slm_fcmh_get(&mq->pfg, &p);
	if (mp->rc)
		goto out;

	iov.iov_base = linkname;
	iov.iov_len = mq->linklen;
	mp->rc = rsx_bulkserver(rq, &desc, BULK_GET_SINK,
	    SRMC_BULK_PORTAL, &iov, 1);
	if (mp->rc)
		goto out;
	pscrpc_free_bulk(desc);

	linkname[sizeof(linkname) - 1] = '\0';
	mds_reserve_slot();
	mp->rc = mdsio_symlink(linkname, fcmh_2_mdsio_fid(p), mq->name,
	    &mq->creds, &mp->attr, NULL, slm_get_next_slashid,
	    mds_namespace_log);
	mds_unreserve_slot();
 out:
	if (p)
		fcmh_op_done_type(p, FCMH_OPCNT_LOOKUP_FIDC);
	return (mp->rc);
}

int
slm_rmc_handle_unlink(struct pscrpc_request *rq, int isfile)
{
	struct srm_unlink_req *mq;
	struct srm_unlink_rep *mp;
	struct fidc_membh *fcmh;

	SL_RSX_ALLOCREP(rq, mq, mp);
	mp->rc = slm_fcmh_get(&mq->pfg, &fcmh);
	if (mp->rc)
		goto out;

	mq->name[sizeof(mq->name) - 1] = '\0';
	mds_reserve_slot();
	if (isfile)
		mp->rc = mdsio_unlink(fcmh_2_mdsio_fid(fcmh),
		    mq->name, &rootcreds, mds_namespace_log);
	else
		mp->rc = mdsio_rmdir(fcmh_2_mdsio_fid(fcmh),
		    mq->name, &rootcreds, mds_namespace_log);
	mds_unreserve_slot();

	psc_info("mdsio_unlink: parent = "SLPRI_FID", name = %s, rc=%d",
		  mq->pfg.fg_fid, mq->name, mp->rc);
 out:
	if (fcmh)
		fcmh_op_done_type(fcmh, FCMH_OPCNT_LOOKUP_FIDC);

	return (0);
}

int
slm_rmc_handle_addreplrq(struct pscrpc_request *rq)
{
	struct srm_generic_rep *mp;
	struct srm_replrq_req *mq;

	SL_RSX_ALLOCREP(rq, mq, mp);
	mp->rc = mds_repl_addrq(&mq->fg,
	    mq->bmapno, mq->repls, mq->nrepls);
	return (0);
}

int
slm_rmc_handle_delreplrq(struct pscrpc_request *rq)
{
	struct srm_generic_rep *mp;
	struct srm_replrq_req *mq;

	SL_RSX_ALLOCREP(rq, mq, mp);
	mp->rc = mds_repl_delrq(&mq->fg,
	    mq->bmapno, mq->repls, mq->nrepls);
	return (0);
}

int
slm_rmc_handle_getreplst(struct pscrpc_request *rq)
{
	struct srm_replst_master_req *mq;
	struct srm_replst_master_rep *mp;
	struct slm_replst_workreq *rsw;

	SL_RSX_ALLOCREP(rq, mq, mp);

	rsw = PSCALLOC(sizeof(*rsw));
	INIT_PSC_LISTENTRY(&rsw->rsw_lentry);
	rsw->rsw_fg = mq->fg;
	rsw->rsw_cid = mq->id;
	rsw->rsw_csvc = slm_getclcsvc(rq->rq_export);
	lc_add(&slm_replst_workq, rsw);
	return (0);
}

int
slm_rmc_handler(struct pscrpc_request *rq)
{
	int rc;

	// XXX move
	//mexpc_get(e);
	// here in case the client violates protocol and doesn't CONNECT

	switch (rq->rq_reqmsg->opc) {
	/* bmap messages */
	case SRMT_BMAPCHWRMODE:
		rc = slm_rmc_handle_bmap_chwrmode(rq);
		break;
	case SRMT_GETBMAP:
		rc = slm_rmc_handle_getbmap(rq);
		break;
	case SRMT_RELEASEBMAP:
		rc = slm_rmc_handle_rls_bmap(rq);
		break;

	/* replication messages */
	case SRMT_SET_NEWREPLPOL:
		rc = slm_rmc_handle_set_newreplpol(rq);
		break;
	case SRMT_SET_BMAPREPLPOL:
		rc = slm_rmc_handle_set_bmapreplpol(rq);
		break;
	case SRMT_REPL_ADDRQ:
		rc = slm_rmc_handle_addreplrq(rq);
		break;
	case SRMT_REPL_DELRQ:
		rc = slm_rmc_handle_delreplrq(rq);
		break;
	case SRMT_REPL_GETST:
		rc = slm_rmc_handle_getreplst(rq);
		break;

	/* control messages */
	case SRMT_CONNECT:
		rc = slm_rmc_handle_connect(rq);
		break;
	case SRMT_PING:
		rc = slm_rmc_handle_ping(rq);
		break;

	/* file system messages */
	case SRMT_CREATE:
		rc = slm_rmc_handle_create(rq);
		break;
	case SRMT_GETATTR:
		rc = slm_rmc_handle_getattr(rq);
		break;
	case SRMT_LINK:
		rc = slm_rmc_handle_link(rq);
		break;
	case SRMT_MKDIR:
		rc = slm_rmc_handle_mkdir(rq);
		break;
	case SRMT_LOOKUP:
		rc = slm_rmc_handle_lookup(rq);
		break;
	case SRMT_READDIR:
		rc = slm_rmc_handle_readdir(rq);
		break;
	case SRMT_READLINK:
		rc = slm_rmc_handle_readlink(rq);
		break;
	case SRMT_RENAME:
		rc = slm_rmc_handle_rename(rq);
		break;
	case SRMT_RMDIR:
		rc = slm_rmc_handle_unlink(rq, 0);
		break;
	case SRMT_SETATTR:
		rc = slm_rmc_handle_setattr(rq);
		break;
	case SRMT_STATFS:
		rc = slm_rmc_handle_statfs(rq);
		break;
	case SRMT_SYMLINK:
		rc = slm_rmc_handle_symlink(rq);
		break;
	case SRMT_UNLINK:
		rc = slm_rmc_handle_unlink(rq, 1);
		break;
	default:
		psc_errorx("Unexpected opcode %d", rq->rq_reqmsg->opc);
		rq->rq_status = -ENOSYS;
		return (pscrpc_error(rq));
	}
	authbuf_sign(rq, PSCRPC_MSG_REPLY);
	pscrpc_target_send_reply_msg(rq, rc, 0);
	return (rc);
}

void
mexpc_destroy(struct pscrpc_export *exp)
{
	struct slm_exp_cli *mexpc = exp->exp_private;
	struct bmap_mds_lease *bml, *tmp;

	if (mexpc == NULL)
		return;

	psclist_for_each_entry_safe(bml, tmp, &mexpc->mexpc_bmlhd,
	    bml_exp_lentry) {
		BML_LOCK(bml);
		psc_assert(bml->bml_flags & BML_EXP);
		bml->bml_flags &= ~BML_EXP;
		bml->bml_flags |= BML_EXPFAIL;
		BML_ULOCK(bml);
		psclist_del(&bml->bml_exp_lentry, &mexpc->mexpc_bmlhd);
	}

	if (mexpc->mexpc_csvc) {
		sl_csvc_reqlock(mexpc->mexpc_csvc);
		sl_csvc_markfree(mexpc->mexpc_csvc);
		sl_csvc_decref(mexpc->mexpc_csvc);
	}
	PSCFREE(exp->exp_private);
}

/**
 * mexpc_get - Get pscrpc_export private data specific to CLI.
 * @exp: RPC export of CLI peer.
 */
struct slm_exp_cli *
mexpc_get(struct pscrpc_export *exp)
{
	struct slm_exp_cli *mexpc;
	int locked;

	locked = EXPORT_RLOCK(exp);
	if (exp->exp_private)
		mexpc = exp->exp_private;
	else {
		mexpc = exp->exp_private = PSCALLOC(sizeof(*mexpc));
		INIT_PSCLIST_HEAD(&mexpc->mexpc_bmlhd);
		INIT_SPINLOCK(&mexpc->mexpc_lock);
		psc_waitq_init(&mexpc->mexpc_waitq);
		exp->exp_hldropf = mexpc_destroy;

		/*
		 * This will assign mexpc_csvc and mexpc_destroy() will
		 * drop this reference.
		 */
		slm_getclcsvc(exp);
	}
	EXPORT_URLOCK(exp, locked);
	return (mexpc);
}
