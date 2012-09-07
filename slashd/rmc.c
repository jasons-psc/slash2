/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2007-2012, Pittsburgh Supercomputing Center (PSC).
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
#include "journal_mds.h"
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

#include "zfs-fuse/zfs_slashlib.h"

uint64_t		slm_next_fid = UINT64_MAX;
psc_spinlock_t		slm_fid_lock = SPINLOCK_INIT;

extern struct psc_hashtbl rootHtable;

void *
slm_rmc_search_roots(char *name)
{
	void *p;

	p = psc_hashtbl_search(&rootHtable, NULL, NULL, name);
	return (p);
}

slfid_t
slm_get_curr_slashfid(void)
{
	slfid_t fid;

	spinlock(&slm_fid_lock);
	fid = slm_next_fid;
	freelock(&slm_fid_lock);
	return (fid);
}

void
slm_set_curr_slashfid(slfid_t slfid)
{
	spinlock(&slm_fid_lock);
	slm_next_fid = slfid;
	freelock(&slm_fid_lock);
}

/**
 * slm_get_next_slashfid - Return the next SLASH FID to use.  Note that
 *	from ZFS point of view, it is perfectly okay that we use the
 *	same SLASH FID to refer to different files/directories.
 *	However, doing so can confuse our clients (think identity
 *	theft).  So we must make sure that we never reuse a SLASH FID,
 *	even after a crash.
 */
int
slm_get_next_slashfid(slfid_t *fidp)
{
	uint64_t fid;

	spinlock(&slm_fid_lock);
	/*
	 * This should never happen.  If it does, we crash to let the
	 * sys admin know.  He could fix this if there are still room in
	 * the cycle bits.  We have to let sys admin know, otherwise,
	 * he/she does not know how to bump the cycle bits.
	 */
	if (FID_GET_INUM(slm_next_fid) >= FID_MAX_INUM) {
		psclog_warnx("Max FID "SLPRI_FID" reached, manual "
		    "intervention needed", slm_next_fid);
		freelock(&slm_fid_lock);
		return (ENOSPC);
	}
	fid = slm_next_fid++;
	freelock(&slm_fid_lock);

	psclog_info("next FID "SLPRI_FID, fid);
	* fidp = fid;
	return (0);
}

int
slm_rmc_handle_ping(struct pscrpc_request *rq)
{
	const struct srm_ping_req *mq;
	struct srm_ping_rep *mp;

	SL_RSX_ALLOCREP(rq, mq, mp);
	return (0);
}

int
slm_rmc_handle_getattr(struct pscrpc_request *rq)
{
	const struct srm_getattr_req *mq;
	struct srm_getattr_rep *mp;
	struct fidc_membh *f;

	SL_RSX_ALLOCREP(rq, mq, mp);
	mp->rc = -slm_fcmh_get(&mq->fg, &f);
	if (mp->rc)
		goto out;

	FCMH_LOCK(f);
	mp->attr = f->fcmh_sstb;

 out:
	if (f)
		fcmh_op_done(f);
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
		struct bmap_mds_info *bmi = bmap_2_bmi(bmap);

		psc_assert(bmi->bmdsi_wr_ion);
		sbd->sbd_ios =
		    bmi->bmdsi_wr_ion->rmmi_resm->resm_res->res_id;
	} else
		sbd->sbd_ios = IOS_ID_ANY;
}

/**
 * slm_rmc_handle_bmap_chwrmode - Handle a BMAPCHWRMODE request to
 *	upgrade a client bmap lease from READ-only to READ+WRITE.
 * @rq: RPC request.
 */
int
slm_rmc_handle_bmap_chwrmode(struct pscrpc_request *rq)
{
	struct srm_bmap_chwrmode_req *mq;
	struct srm_bmap_chwrmode_rep *mp;
	struct fidc_membh *f = NULL;
	struct bmapc_memb *b = NULL;
	struct bmap_mds_lease *bml;
	struct bmap_mds_info *bmi;

	SL_RSX_ALLOCREP(rq, mq, mp);
	mp->rc = -slm_fcmh_get(&mq->sbd.sbd_fg, &f);
	if (mp->rc)
		goto out;
	mp->rc = bmap_lookup(f, mq->sbd.sbd_bmapno, &b);
	if (mp->rc)
		goto out;

	bmi = bmap_2_bmi(b);

	BMAP_LOCK(b);
	bml = mds_bmap_getbml_locked(b, mq->sbd.sbd_seq,
	    mq->sbd.sbd_nid, mq->sbd.sbd_pid);

	if (bml == NULL) {
		mp->rc = -EINVAL;
		goto out;
	}

	mp->rc = mds_bmap_bml_chwrmode(bml, mq->prefios[0]);
	if (mp->rc == -EALREADY)
		mp->rc = 0;
	else if (mp->rc)
		goto out;

	mp->sbd = mq->sbd;
	mp->sbd.sbd_seq = bml->bml_seq;
	mp->sbd.sbd_key = bmi->bmdsi_assign->odtr_key;

	psc_assert(bmi->bmdsi_wr_ion);
	mp->sbd.sbd_ios = bmi->bmdsi_wr_ion->rmmi_resm->resm_res_id;

 out:
	if (bml)
		mds_bmap_bml_release(bml);
	if (b)
		bmap_op_done(b);
	if (f)
		fcmh_op_done(f);
	return (0);
}

int
slm_rmc_handle_extendbmapls(struct pscrpc_request *rq)
{
	struct srm_leasebmapext_req *mq;
	struct srm_leasebmapext_rep *mp;
	struct fidc_membh *f;

	SL_RSX_ALLOCREP(rq, mq, mp);

	mp->rc = -slm_fcmh_get(&mq->sbd.sbd_fg, &f);
	if (mp->rc)
		return (0);

	mp->rc = mds_lease_renew(f, &mq->sbd, &mp->sbd, rq->rq_export);
	fcmh_op_done(f);
	return (0);
}

int
slm_rmc_handle_reassignbmapls(struct pscrpc_request *rq)
{
	struct srm_reassignbmap_req *mq;
	struct srm_reassignbmap_rep *mp;
	struct fidc_membh *f;

	SL_RSX_ALLOCREP(rq, mq, mp);

	mp->rc = -slm_fcmh_get(&mq->sbd.sbd_fg, &f);
	if (mp->rc)
		return (0);

	mp->rc = mds_lease_reassign(f, &mq->sbd, mq->pios,
	    mq->prev_sliods, mq->nreassigns, &mp->sbd, rq->rq_export);

	fcmh_op_done(f);
	return (0);
}

int
slm_rmc_handle_getbmap(struct pscrpc_request *rq)
{
	const struct srm_leasebmap_req *mq;
	struct bmapc_memb *bmap = NULL;
	struct srm_leasebmap_rep *mp;
	struct fidc_membh *f;
	int rc = 0;

	SL_RSX_ALLOCREP(rq, mq, mp);
	if (mq->rw != SL_READ && mq->rw != SL_WRITE) {
		mp->rc = -EINVAL;
		return (0);
	}

	mp->rc = -slm_fcmh_get(&mq->fg, &f);
	if (mp->rc)
		return (0);
	mp->flags = mq->flags;

	mp->rc = mds_bmap_load_cli(f, mq->bmapno, mq->flags, mq->rw,
	    mq->prefios[0], &mp->sbd, rq->rq_export, &bmap);
	if (mp->rc)
		goto out;

	if (mq->flags & SRM_LEASEBMAPF_DIRECTIO)
		mp->sbd.sbd_flags |= SRM_LEASEBMAPF_DIRECTIO;

	slm_rmc_bmapdesc_setup(bmap, &mp->sbd, mq->rw);

	memcpy(&mp->bcs, &bmap->bcm_corestate, sizeof(mp->bcs));

	if (mp->flags & SRM_LEASEBMAPF_GETREPLTBL) {
		struct slash_inode_handle *ih;

		ih = fcmh_2_inoh(f);
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

 out:
	fcmh_op_done(f);
	return (rc ? rc : mp->rc);
}

int
slm_rmc_handle_link(struct pscrpc_request *rq)
{
	struct fidc_membh *p = NULL, *c = NULL;
	struct srm_link_req *mq;
	struct srm_link_rep *mp;
	int vfsid;

	SL_RSX_ALLOCREP(rq, mq, mp);
	if (mdsio_fid_to_vfsid(mq->fg.fg_fid, &vfsid) < 0) {
		mp->rc = -EINVAL;
		goto out;
	}

	mp->rc = -slm_fcmh_get(&mq->fg, &c);
	if (mp->rc)
		goto out;
	mp->rc = -slm_fcmh_get(&mq->pfg, &p);
	if (mp->rc)
		goto out;

	mq->name[sizeof(mq->name) - 1] = '\0';
	mds_reserve_slot(1);
	mp->rc = mdsio_link(vfsid, fcmh_2_mdsio_fid(c),
	    fcmh_2_mdsio_fid(p), mq->name, &rootcreds, &mp->cattr,
	    mdslog_namespace);
	mds_unreserve_slot(1);

	mdsio_fcmh_refreshattr(p, &mp->pattr);

 out:
	if (c)
		fcmh_op_done(c);
	if (p)
		fcmh_op_done(p);
	return (0);
}

int
slm_rmc_handle_lookup(struct pscrpc_request *rq)
{
	struct srm_lookup_req *mq;
	struct srm_lookup_rep *mp;
	struct fidc_membh *p = NULL;
	int vfsid;

	SL_RSX_ALLOCREP(rq, mq, mp);
	if (mdsio_fid_to_vfsid(mq->pfg.fg_fid, &vfsid) < 0) {
		mp->rc = -EINVAL;
		goto out;
	}
	mp->rc = -slm_fcmh_get(&mq->pfg, &p);
	if (mp->rc)
		goto out;

	mq->name[sizeof(mq->name) - 1] = '\0';
	if (fcmh_2_mdsio_fid(p) == SLFID_ROOT &&
	    strcmp(mq->name, SL_RPATH_META_DIR) == 0) {
		mp->rc = -EINVAL;
		goto out;
	}
	mp->rc = mdsio_lookup(vfsid, fcmh_2_mdsio_fid(p), mq->name,
	    NULL, &rootcreds, &mp->attr);
	if (mp->rc)
		goto out;
	if (mq->pfg.fg_fid == SLFID_ROOT) {

		int error;
		uint64_t fid;
		struct rootNames *p;
		mount_info_t *mountinfo;
		struct srt_stat tmpattr;

		p = slm_rmc_search_roots(mq->name);
		if (p) {
			mountinfo = &zfsMount[p->rn_vfsid];
			fid = SLFID_ROOT;
			FID_SET_SITEID(fid, mountinfo->siteid);

			error = mdsio_getattr(p->rn_vfsid,
			    mountinfo->rootid, mountinfo->rootinfo,
			    &rootcreds, &tmpattr);
			if (!error) {
				tmpattr.sst_fg.fg_fid = fid;
				mp->attr = tmpattr;
			} else
				/* better than nothing */
				mp->attr.sst_fg.fg_fid = fid;
		}
	}

 out:
	if (p)
		fcmh_op_done(p);
	return (0);
}

int
slm_mkdir(int vfsid, struct srm_mkdir_req *mq, struct srm_mkdir_rep *mp,
    int opflags, struct fidc_membh **dp)
{
	struct fidc_membh *p = NULL, *c = NULL;
	slfid_t fid = 0;

	mq->name[sizeof(mq->name) - 1] = '\0';
	if (IS_REMOTE_FID(mq->pfg.fg_fid)) {
		struct slash_creds cr;

		cr.scr_uid = mq->sstb.sst_uid;
		cr.scr_gid = mq->sstb.sst_gid;
		/* XXX pass opflags */
		mp->rc = slm_rmm_forward_namespace(SLM_FORWARD_MKDIR,
		    &mq->pfg, NULL, mq->name, NULL, mq->sstb.sst_mode,
		    &cr, &mp->cattr, 0);
		if (mp->rc)
			goto out;
		fid = mp->cattr.sst_fg.fg_fid;
	}

	mp->rc = -slm_fcmh_get(&mq->pfg, &p);
	if (mp->rc)
		goto out;

	mds_reserve_slot(1);
	mp->rc = -mdsio_mkdir(vfsid, fcmh_2_mdsio_fid(p), mq->name,
	    &mq->sstb, 0, opflags, &mp->cattr, NULL, fid ? NULL :
	    mdslog_namespace, fid ? 0 : slm_get_next_slashfid, fid);
	mds_unreserve_slot(1);

 out:
	if (p)
		mdsio_fcmh_refreshattr(p, &mp->pattr);

	/*
	 * Set new subdir's new files' default replication policy from
	 * parent dir.
	 */
	if (mp->rc == 0 && slm_fcmh_get(&mp->cattr.sst_fg, &c) == 0)
		slm_fcmh_endow_nolog(vfsid, p, c);

	if (dp) {
		if (mp->rc == -EEXIST &&
		    mdsio_lookup(vfsid, fcmh_2_mdsio_fid(p), mq->name, NULL,
		    &rootcreds, &mp->cattr) == 0)
			slm_fcmh_get(&mp->cattr.sst_fg, &c);
		*dp = c;
		c = NULL;
	}
	if (p)
		fcmh_op_done(p);
	if (c)
		fcmh_op_done(c);
	return (0);
}

int
slm_rmc_handle_mkdir(struct pscrpc_request *rq)
{
	struct srm_mkdir_req *mq;
	struct srm_mkdir_rep *mp;
	int vfsid;

	SL_RSX_ALLOCREP(rq, mq, mp);
	if (mdsio_fid_to_vfsid(mq->pfg.fg_fid, &vfsid) < 0) {
		mp->rc = -EINVAL;
		return (0);
	}
	return (slm_mkdir(vfsid, mq, mp, 0, NULL));
}

int
slm_rmc_handle_mknod(struct pscrpc_request *rq)
{
	struct fidc_membh *p = NULL;
	struct srm_mknod_req *mq;
	struct srm_mknod_rep *mp;
	int vfsid;

	SL_RSX_ALLOCREP(rq, mq, mp);
	if (mdsio_fid_to_vfsid(mq->pfg.fg_fid, &vfsid) < 0) {
		mp->rc = -EINVAL;
		goto out;
	}

	mp->rc = -slm_fcmh_get(&mq->pfg, &p);
	if (mp->rc)
		goto out;

	mq->name[sizeof(mq->name) - 1] = '\0';
	mds_reserve_slot(1);
	mp->rc = mdsio_mknod(vfsid, fcmh_2_mdsio_fid(p), mq->name,
	    mq->mode, &mq->creds, &mp->cattr, NULL, mdslog_namespace,
	    slm_get_next_slashfid);
	mds_unreserve_slot(1);

	mdsio_fcmh_refreshattr(p, &mp->pattr);
 out:
	if (p)
		fcmh_op_done(p);
	return (0);
}

/**
 * slm_rmc_handle_create - Handle a CREATE from CLI.  As an
 *	optimization, we bundle a write bmap lease in the reply.
 */
int
slm_rmc_handle_create(struct pscrpc_request *rq)
{
	struct fidc_membh *p = NULL, *c;
	struct srm_create_rep *mp;
	struct srm_create_req *mq;
	struct bmapc_memb *bmap;
	void *mdsio_data;
	int vfsid;
	slfid_t fid = 0;

	SL_RSX_ALLOCREP(rq, mq, mp);

	if (mdsio_fid_to_vfsid(mq->pfg.fg_fid, &vfsid) < 0) {
		mp->rc = -EINVAL;
		goto out;
	}

	if (mq->flags & SRM_LEASEBMAPF_GETREPLTBL) {
		mp->rc = -EINVAL;
		goto out;
	}
	mq->name[sizeof(mq->name) - 1] = '\0';

	if (IS_REMOTE_FID(mq->pfg.fg_fid)) {
		mp->rc = slm_rmm_forward_namespace(SLM_FORWARD_CREATE,
		    &mq->pfg, NULL, mq->name, NULL, mq->mode,
		    &mq->creds, &mp->cattr, 0);
		if (mp->rc)
			goto out;
		fid = mp->cattr.sst_fg.fg_fid;
	}

	/* Lookup the parent directory in the cache so that the
	 *   SLASH2 ino can be translated into the inode for the
	 *   underlying fs.
	 */
	mp->rc = -slm_fcmh_get(&mq->pfg, &p);
	if (mp->rc)
		goto out;

	DEBUG_FCMH(PLL_DEBUG, p, "create op start for %s", mq->name);

	mds_reserve_slot(1);
	mp->rc = mdsio_opencreate(vfsid, fcmh_2_mdsio_fid(p), &mq->creds,
	    O_CREAT | O_EXCL | O_RDWR, mq->mode, mq->name, NULL,
	    &mp->cattr, &mdsio_data, fid ? NULL : mdslog_namespace,
	    fid ? 0 : slm_get_next_slashfid, fid);
	mds_unreserve_slot(1);

	if (mp->rc)
		goto out;

	/* Refresh the cached attributes of our parent and pack them
	 *   in the reply.
	 */
	mdsio_fcmh_refreshattr(p, &mp->pattr);

	DEBUG_FCMH(PLL_DEBUG, p, "create op done for %s", mq->name);
	/* XXX enter this into the fcmh cache instead of doing it again
	 *   This release may be the sanest thing actually, unless EXCL is
	 *   used.
	 */
	mdsio_release(vfsid, &rootcreds, mdsio_data);

	DEBUG_FCMH(PLL_DEBUG, p, "mdsio_release() done for %s",
	    mq->name);

	if (fid) {
		mp->rc2 = ENOENT;
		goto out;
	}

	mp->rc = -slm_fcmh_get(&mp->cattr.sst_fg, &c);
	if (mp->rc)
		goto out;

	slm_fcmh_endow_nolog(vfsid, p, c);

	/* obtain lease for first bmap as optimization */
	mp->flags = mq->flags;

	bmap = NULL;
	mp->rc2 = mds_bmap_load_cli(c, 0, mp->flags, SL_WRITE,
	    mq->prefios[0], &mp->sbd, rq->rq_export, &bmap);

	fcmh_op_done(c);

	if (mp->rc2)
		goto out;

	slm_rmc_bmapdesc_setup(bmap, &mp->sbd, SL_WRITE);

 out:
	if (p)
		fcmh_op_done(p);
	return (0);
}

void
slm_rmc_handle_readdir_roots(struct iovec *iov0, struct iovec *iov1,
    size_t nents)
{
	struct srt_stat tmpattr, *attr;
	struct pscfs_dirent *dirent;
	struct rootNames *p;
	mount_info_t *mountinfo;
	size_t i, entsize;
	uint64_t fid;
	int error;

	attr = iov1->iov_base;
	dirent = iov0->iov_base;
	for (i = 0; i < nents; i++) {

		p = slm_rmc_search_roots(dirent->pfd_name);
		if (p) {
			mountinfo = &zfsMount[p->rn_vfsid];
			fid = SLFID_ROOT;
			FID_SET_SITEID(fid, mountinfo->siteid);
			dirent->pfd_ino = fid;

			error = mdsio_getattr(p->rn_vfsid,
			    mountinfo->rootid, mountinfo->rootinfo,
			    &rootcreds, &tmpattr);
			if (!error) {
				tmpattr.sst_fg.fg_fid = fid;
				*attr = tmpattr;
			} else
				/* better than nothing */
				attr->sst_fg.fg_fid = fid;
		}
		attr++;
		entsize = PFL_DIRENT_SIZE(dirent->pfd_namelen);
		dirent = PSC_AGP(dirent, entsize);
	}
}

int
slm_rmc_handle_readdir(struct pscrpc_request *rq)
{
	struct fidc_membh *f = NULL;
	struct srm_readdir_req *mq;
	struct srm_readdir_rep *mp;
	size_t outsize, nents;
	struct iovec iov[2];
	int niov, vfsid;

	iov[0].iov_base = NULL;
	iov[1].iov_base = NULL;

	SL_RSX_ALLOCREP(rq, mq, mp);

	if (mdsio_fid_to_vfsid(mq->fg.fg_fid, &vfsid) < 0) {
		mp->rc = -EINVAL;
		goto out;
	}

	mp->rc = -slm_fcmh_get(&mq->fg, &f);
	if (mp->rc)
		goto out;

	if (mq->size > MAX_READDIR_BUFSIZ ||
	    mq->nstbpref > MAX_READDIR_NENTS) {
		mp->rc = -EINVAL;
		goto out;
	}

	iov[0].iov_base = PSCALLOC(mq->size);
	iov[0].iov_len = mq->size;

	niov = 1;
	if (mq->nstbpref) {
		niov++;
		iov[1].iov_len = mq->nstbpref * sizeof(struct srt_stat);
		iov[1].iov_base = PSCALLOC(iov[1].iov_len);
	} else {
		iov[1].iov_len = 0;
		iov[1].iov_base = NULL;
	}

	/* make sure things are populated under the root before readdir() */
	if (mq->fg.fg_fid == SLFID_ROOT)
		psc_scan_filesystems();

	mp->rc = mdsio_readdir(vfsid, &rootcreds, mq->size, mq->offset,
	    iov[0].iov_base, &outsize, &nents, iov[1].iov_base,
	    mq->nstbpref, fcmh_2_mdsio_data(f));

	psclog_info("mdsio_readdir: rc=%d, data=%p", mp->rc,
	    fcmh_2_mdsio_data(f));
	mp->size = outsize;
	mp->num = nents;

	if (mp->rc)
		goto out;

	/*
	 * If this is the root, we fake part of readdir contents by
	 * return the file system names here.
	 */
	if (mq->fg.fg_fid == SLFID_ROOT)
		slm_rmc_handle_readdir_roots(&iov[0], &iov[1], nents);
#if 0
	{
		/* debugging only */
		unsigned int i;
		struct srt_stat *attr;
		attr = iov[1].iov_base;
		for (i = 0; i < mq->nstbpref; i++, attr++) {
			if (!attr->sst_fg.fg_fid)
				break;
			psclog_info("reply: f+g:"SLPRI_FG", mode=%#o",
				SLPRI_FG_ARGS(&attr->sst_fg),
				attr->sst_mode);
		}
	}
#endif

	if (SRM_READDIR_BUFSZ(mp->size, mp->num, mq->nstbpref) <=
	    sizeof(mp->ents)) {
		size_t sz;

		sz = MIN(mp->num, mq->nstbpref) *
		    sizeof(struct srt_stat);
		memcpy(mp->ents, iov[1].iov_base, sz);
		memcpy(mp->ents + sz, iov[0].iov_base, mp->size);
		pscrpc_msg_add_flags(rq->rq_repmsg, MSG_ABORT_BULK);
	} else {
		mp->rc = rsx_bulkserver(rq, BULK_PUT_SOURCE,
		    SRMC_BULK_PORTAL, iov, niov);
	}

 out:
	PSCFREE(iov[0].iov_base);
	PSCFREE(iov[1].iov_base);
	if (f)
		fcmh_op_done(f);
	return (mp->rc);
}

int
slm_rmc_handle_readlink(struct pscrpc_request *rq)
{
	struct srm_readlink_req *mq;
	struct srm_readlink_rep *mp;
	struct fidc_membh *f = NULL;
	struct iovec iov;
	char buf[SL_PATH_MAX];
	int vfsid;

	SL_RSX_ALLOCREP(rq, mq, mp);
	if (mdsio_fid_to_vfsid(mq->fg.fg_fid, &vfsid) < 0) {
		mp->rc = -EINVAL;
		goto out;
	}

	mp->rc = -slm_fcmh_get(&mq->fg, &f);
	if (mp->rc)
		goto out;

	mp->rc = mdsio_readlink(vfsid, fcmh_2_mdsio_fid(f), buf,
	    &rootcreds);
	if (mp->rc)
		goto out;

	iov.iov_base = buf;
	iov.iov_len = sizeof(buf);
	mp->rc = rsx_bulkserver(rq, BULK_PUT_SOURCE, SRMC_BULK_PORTAL,
	    &iov, 1);

 out:
	if (f)
		fcmh_op_done(f);
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
	char from[SL_NAME_MAX + 1], to[SL_NAME_MAX + 1];
	struct fidc_membh *op = NULL, *np = NULL;
	struct srm_rename_req *mq;
	struct srm_rename_rep *mp;
	struct slash_fidgen chfg;
	struct iovec iov[2];
	int vfsid;

	chfg.fg_fid = FID_ANY;

	SL_RSX_ALLOCREP(rq, mq, mp);
	if (mdsio_fid_to_vfsid(mq->opfg.fg_fid, &vfsid) < 0) {
		mp->rc = -EINVAL;
		return (mp->rc);
	}

	if (mq->fromlen == 0 || mq->tolen == 0 ||
	    mq->fromlen > SL_NAME_MAX ||
	    mq->tolen   > SL_NAME_MAX) {
		mp->rc = -EINVAL;
		return (mp->rc);
	}

	if (FID_GET_SITEID(mq->opfg.fg_fid) !=
	    FID_GET_SITEID(mq->npfg.fg_fid)) {
		mp->rc = -EXDEV;
		return (mp->rc);
	}

	if (mq->fromlen + mq->tolen > SRM_RENAME_NAMEMAX) {
		iov[0].iov_base = from;
		iov[0].iov_len = mq->fromlen;
		iov[1].iov_base = to;
		iov[1].iov_len = mq->tolen;
		mp->rc = rsx_bulkserver(rq, BULK_GET_SINK,
		    SRMC_BULK_PORTAL, iov, 2);
		if (mp->rc)
			return (mp->rc);
	} else {
		memcpy(from, mq->buf, mq->fromlen);
		memcpy(to, mq->buf + mq->fromlen, mq->tolen);
	}

	from[mq->fromlen] = '\0';
	to[mq->tolen]     = '\0';

	if (IS_REMOTE_FID(mq->opfg.fg_fid)) {
		mp->rc = slm_rmm_forward_namespace(SLM_FORWARD_RENAME,
		    &mq->opfg, &mq->npfg, from, to, 0, &rootcreds,
		    &mp->srr_npattr, 0);
		if (mp->rc)
			goto out;
	}

	mp->rc = -slm_fcmh_get(&mq->opfg, &op);
	if (mp->rc)
		goto out;

	if (SAMEFG(&mq->opfg, &mq->npfg)) {
		np = op;
	} else {
		mp->rc = -slm_fcmh_get(&mq->npfg, &np);
		if (mp->rc)
			goto out;
	}

	/* if we get here, op and np must be owned by the current MDS */
	mds_reserve_slot(2);
	mp->rc = mdsio_rename(vfsid, fcmh_2_mdsio_fid(op), from,
	    fcmh_2_mdsio_fid(np), to, &rootcreds, mdslog_namespace,
	    &chfg);
	mds_unreserve_slot(2);

 out:
	if (mp->rc == 0) {
		mdsio_fcmh_refreshattr(op, &mp->srr_opattr);
		if (op != np)
			mdsio_fcmh_refreshattr(np, &mp->srr_npattr);

		if (chfg.fg_fid != FID_ANY) {
			struct fidc_membh *c;

			if (slm_fcmh_get(&chfg, &c) == 0) {
				mdsio_fcmh_refreshattr(c,
				    &mp->srr_cattr);
				fcmh_op_done(c);
			}
		}
	}

	if (np)
		fcmh_op_done(np);
	if (op && op != np)
		fcmh_op_done(op);
	return (0);
}

int
slm_rmc_handle_setattr(struct pscrpc_request *rq)
{
	int to_set, flush, tadj = 0, unbump = 0;
	struct slashrpc_cservice *csvc;
	struct fidc_membh *f = NULL;
	struct srm_setattr_req *mq;
	struct srm_setattr_rep *mp;
	int vfsid;

	SL_RSX_ALLOCREP(rq, mq, mp);
	if (mdsio_fid_to_vfsid(mq->attr.sst_fg.fg_fid, &vfsid) < 0) {
		mp->rc = -EINVAL;
		goto out;
	}

	mp->rc = -slm_fcmh_get(&mq->attr.sst_fg, &f);
	if (mp->rc)
		return (0);

	FCMH_WAIT_BUSY(f);

	flush = mq->to_set & PSCFS_SETATTRF_FLUSH;
	to_set = mq->to_set & SL_SETATTRF_CLI_ALL;

	if (to_set & PSCFS_SETATTRF_DATASIZE) {
#if 0
		if (IS_REMOTE_FID(mq->attr.sst_fg.fg_fid)) {
			mp->rc = -ENOSYS;
			goto out;
		}
#endif
		/* our client should really do this on its own */
		if (!(to_set & PSCFS_SETATTRF_MTIME)) {
			psclog_warn("setattr: missing MTIME flag in RPC request");
			to_set |= PSCFS_SETATTRF_MTIME;
			SL_GETTIMESPEC(&mq->attr.sst_mtim);
		}
		if (mq->attr.sst_size == 0 || !fcmh_2_fsz(f)) {
			/*
			 * Full truncate.  If file size is already zero,
			 * we must still bump the generation since size
			 * updates from the sliod may be pending for
			 * this generation.
			 */
			mq->attr.sst_fg.fg_gen = fcmh_2_gen(f) + 1;
			to_set |= SL_SETATTRF_GEN;
			unbump = 1;
		} else if (!flush) {
mp->rc = -ENOTSUP;
goto out;

			/* partial truncate */
			if (f->fcmh_flags & FCMH_IN_PTRUNC) {
				mp->rc = -SLERR_BMAP_IN_PTRUNC;
				goto out;
			}
			to_set &= ~PSCFS_SETATTRF_DATASIZE;
			tadj |= PSCFS_SETATTRF_DATASIZE;
		}
	}

	if (to_set) {
		if (IS_REMOTE_FID(mq->attr.sst_fg.fg_fid)) {
			mp->rc = slm_rmm_forward_namespace(SLM_FORWARD_SETATTR,
			    &mq->attr.sst_fg, NULL, NULL, NULL, 0, NULL,
			    &mq->attr, to_set);
			if (mp->rc)
				goto out;
		}
		/*
		 * If the file is open, mdsio_data will be valid
		 * and used.  Otherwise, it will be NULL, and
		 * we'll use the mdsio_fid.
		 */
		mp->rc = mds_fcmh_setattr(vfsid, f, to_set, &mq->attr);
	}

	if (mp->rc) {
		if (unbump)
			fcmh_2_gen(f)--;
	} else if (!flush) {
		if (tadj & PSCFS_SETATTRF_DATASIZE) {
			f->fcmh_flags |= FCMH_IN_PTRUNC;

			csvc = slm_getclcsvc(rq->rq_export);
			psc_dynarray_add(&fcmh_2_fmi(f)->
			    fmi_ptrunc_clients, csvc);

			mp->rc = -SLERR_BMAP_PTRUNC_STARTED;
		}

		slm_setattr_core(f, &mq->attr, to_set | tadj);
	}

 out:
	if (f) {
		FCMH_RLOCK(f);
		if (mp->rc == 0 || mp->rc == SLERR_BMAP_PTRUNC_STARTED)
			mp->attr = f->fcmh_sstb;
		FCMH_UNBUSY(f);
		fcmh_op_done(f);
	}
	return (0);
}

int
slm_rmc_handle_set_newreplpol(struct pscrpc_request *rq)
{
	struct srm_set_newreplpol_req *mq;
	struct srm_set_newreplpol_rep *mp;
	struct fidc_membh *f = NULL;
	int vfsid;

	SL_RSX_ALLOCREP(rq, mq, mp);

	if (mq->pol < 0 || mq->pol >= NBRPOL) {
		mp->rc = -EINVAL;
		return (0);
	}
	if (mdsio_fid_to_vfsid(mq->fg.fg_fid, &vfsid) < 0) {
		mp->rc = -EINVAL;
		goto out;
	}
	if (vfsid != current_vfsid) {
		mp->rc = -EINVAL;
		goto out;
	}

	mp->rc = -slm_fcmh_get(&mq->fg, &f);
	if (mp->rc)
		goto out;

	FCMH_LOCK(f);
	fcmh_2_replpol(f) = mq->pol;
	mp->rc = mds_inode_write(vfsid, fcmh_2_inoh(f),
	    mdslog_ino_repls, f);

 out:
	if (f)
		fcmh_op_done(f);
	return (0);
}

int
slm_rmc_handle_set_bmapreplpol(struct pscrpc_request *rq)
{
	struct srm_set_bmapreplpol_req *mq;
	struct srm_set_bmapreplpol_rep *mp;
	struct fidc_membh *f;
	struct bmapc_memb *b;

	SL_RSX_ALLOCREP(rq, mq, mp);

	if (mq->pol < 0 || mq->pol >= NBRPOL) {
		mp->rc = -EINVAL;
		return (0);
	}

	mp->rc = -slm_fcmh_get(&mq->fg, &f);
	if (mp->rc)
		goto out;

	if (!mds_bmap_exists(f, mq->bmapno)) {
		mp->rc = -SLERR_BMAP_INVALID;
		goto out;
	}
	mp->rc = mds_bmap_load(f, mq->bmapno, &b);
	if (mp->rc)
		goto out;

	BHREPL_POLICY_SET(b, mq->pol);

	mds_bmap_write_repls_rel(b);
	/* XXX upd_enqueue */

 out:
	if (f)
		fcmh_op_done(f);
	return (0);
}

int
slm_rmc_handle_statfs(struct pscrpc_request *rq)
{
	int j = 0, single = 0, vfsid;
	struct resprof_mds_info *rpmi;
	struct sl_resource *r, *ri;
	struct srm_statfs_req *mq;
	struct srm_statfs_rep *mp;
	struct sl_mds_iosinfo *si;
	struct statvfs sfb;
	double adj;

	SL_RSX_ALLOCREP(rq, mq, mp);
	if (mdsio_fid_to_vfsid(mq->fid, &vfsid) < 0) {
		mp->rc = -EINVAL;
		return (0);
	}
	mp->rc = mdsio_statfs(vfsid, &sfb);
	sl_externalize_statfs(&sfb, &mp->ssfb);
	r = libsl_id2res(mq->iosid);
	if (r == NULL) {
		mp->rc = -SLERR_RES_UNKNOWN;
		return (0);
	}
	mp->ssfb.sf_bsize = 0;
	mp->ssfb.sf_blocks = 0;
	mp->ssfb.sf_bfree = 0;
	mp->ssfb.sf_bavail = 0;
	if (!RES_ISCLUSTER(r)) {
		ri = r;
		single = 1;
		goto single;
	}
	DYNARRAY_FOREACH(ri, j, &r->res_peers) {
 single:
		rpmi = res2rpmi(r);
		si = res2iosinfo(ri);
		RPMI_LOCK(rpmi);
		if (si->si_ssfb.sf_bsize == 0) {
			RPMI_ULOCK(rpmi);
			continue;
		}
		if (mp->ssfb.sf_bsize == 0)
			mp->ssfb.sf_bsize = si->si_ssfb.sf_bsize;
		adj = mp->ssfb.sf_bsize * 1. / si->si_ssfb.sf_bsize;
		mp->ssfb.sf_blocks	+= adj * si->si_ssfb.sf_blocks;
		mp->ssfb.sf_bfree	+= adj * si->si_ssfb.sf_bfree;
		mp->ssfb.sf_bavail	+= adj * si->si_ssfb.sf_bavail;
		RPMI_ULOCK(rpmi);

		if (single)
			break;
	}
	return (0);
}

int
slm_symlink(struct pscrpc_request *rq, struct srm_symlink_req *mq,
    struct srm_symlink_rep *mp, int ptl)
{
	char linkname[SL_PATH_MAX];
	struct fidc_membh *p = NULL;
	struct slash_creds cr;
	struct iovec iov;
	slfid_t fid = 0;
	int vfsid;

	if (mdsio_fid_to_vfsid(mq->pfg.fg_fid, &vfsid) < 0) {
		mp->rc = -EINVAL;
		return (mp->rc);
	}

	mq->name[sizeof(mq->name) - 1] = '\0';
	if (mq->linklen == 0 || mq->linklen >= SL_PATH_MAX) {
		mp->rc = -EINVAL;
		return (mp->rc);
	}

	iov.iov_base = linkname;
	iov.iov_len = mq->linklen;
	mp->rc = rsx_bulkserver(rq, BULK_GET_SINK, ptl, &iov, 1);
	if (mp->rc)
		return (mp->rc);

	linkname[mq->linklen] = '\0';

	cr.scr_uid = mq->sstb.sst_uid;
	cr.scr_gid = mq->sstb.sst_gid;

	if (IS_REMOTE_FID(mq->pfg.fg_fid)) {
		mp->rc = slm_rmm_forward_namespace(SLM_FORWARD_SYMLINK,
		    &mq->pfg, NULL, mq->name, linkname, 0, &cr,
		    &mp->cattr, 0);
		if (mp->rc)
			goto out;
		fid = mp->cattr.sst_fg.fg_fid;
	}

	mp->rc = -slm_fcmh_get(&mq->pfg, &p);
	if (mp->rc)
		goto out;

	mds_reserve_slot(1);
	mp->rc = mdsio_symlink(vfsid, linkname, fcmh_2_mdsio_fid(p),
	    mq->name, &cr, &mp->cattr, NULL, fid ? NULL :
	    mdslog_namespace, fid ? 0 : slm_get_next_slashfid, fid);
	mds_unreserve_slot(1);

	mdsio_fcmh_refreshattr(p, &mp->pattr);

 out:
	if (p)
		fcmh_op_done(p);
	return (0);
}

int
slm_rmc_handle_symlink(struct pscrpc_request *rq)
{
	struct srm_symlink_req *mq;
	struct srm_symlink_rep *mp;

	SL_RSX_ALLOCREP(rq, mq, mp);
	return (slm_symlink(rq, mq, mp, SRMC_BULK_PORTAL));
}

int
slm_rmc_handle_unlink(struct pscrpc_request *rq, int isfile)
{
	struct slash_fidgen fg, chfg;
	struct fidc_membh *p = NULL;
	struct srm_unlink_req *mq;
	struct srm_unlink_rep *mp;
	int vfsid;

	chfg.fg_fid = FID_ANY;

	SL_RSX_ALLOCREP(rq, mq, mp);
	if (mdsio_fid_to_vfsid(mq->pfid, &vfsid) < 0) {
		mp->rc = -EINVAL;
		goto out;
	}

	fg.fg_fid = mq->pfid;
	fg.fg_gen = FGEN_ANY;
	mq->name[sizeof(mq->name) - 1] = '\0';

	if (IS_REMOTE_FID(mq->pfid)) {
		mp->rc = slm_rmm_forward_namespace(isfile ?
		    SLM_FORWARD_UNLINK : SLM_FORWARD_RMDIR, &fg, NULL,
		    mq->name, NULL, 0, NULL, NULL, 0);
		if (mp->rc)
			goto out;
	}

	mp->rc = -slm_fcmh_get(&fg, &p);
	if (mp->rc)
		goto out;

	mds_reserve_slot(1);
	if (isfile)
		mp->rc = mdsio_unlink(vfsid, fcmh_2_mdsio_fid(p), NULL,
		    mq->name, &rootcreds, mdslog_namespace, &chfg);
	else
		mp->rc = mdsio_rmdir(vfsid, fcmh_2_mdsio_fid(p), NULL,
		    mq->name, &rootcreds, mdslog_namespace);
	mds_unreserve_slot(1);

 out:
	if (mp->rc == 0)
		mdsio_fcmh_refreshattr(p, &mp->pattr);
	if (p)
		fcmh_op_done(p);

	if (chfg.fg_fid != FID_ANY) {
		struct fidc_membh *c;

		if (slm_fcmh_get(&chfg, &c) == 0) {
			mdsio_fcmh_refreshattr(c, &mp->cattr);
			fcmh_op_done(c);
		}
	}

	psclog_info("%s parent="SLPRI_FID" name=%s rc=%d",
	    isfile ? "unlink" : "rmdir", mq->pfid, mq->name, mp->rc);
	return (0);
}

int
slm_rmc_handle_listxattr(struct pscrpc_request *rq)
{
	struct fidc_membh *f = NULL;
	struct srm_listxattr_req *mq;
	struct srm_listxattr_rep *mp;
	size_t outsize;
	struct iovec iov;
	int vfsid;

	iov.iov_base = NULL;
	SL_RSX_ALLOCREP(rq, mq, mp);
	if (mdsio_fid_to_vfsid(mq->fg.fg_fid, &vfsid) < 0) {
		mp->rc = -EINVAL;
		goto out;
	}
	mp->rc = -slm_fcmh_get(&mq->fg, &f);
	if (mp->rc)
		goto out;

	if (mq->size) {
		iov.iov_base = PSCALLOC(mq->size);
		iov.iov_len = mq->size;
	}
	mp->size = 0;

	/* even a list can create the xaddr directory */
	mds_reserve_slot(1);
	mp->rc = mdsio_listxattr(vfsid, &rootcreds,
	    iov.iov_base, mq->size, &outsize, fcmh_2_mdsio_fid(f));
	mds_unreserve_slot(1);
	if (mp->rc) {
		if (mq->size)
			pscrpc_msg_add_flags(rq->rq_repmsg,
			    MSG_ABORT_BULK);
		goto out;
	}

	mp->size = outsize;
	if (mq->size) {
		mp->rc = rsx_bulkserver(rq, BULK_PUT_SOURCE,
		    SRMC_BULK_PORTAL, &iov, 1);
	}

 out:
	if (mq->size)
		PSCFREE(iov.iov_base);
	if (f)
		fcmh_op_done(f);
	return (0);
}

int
slm_rmc_handle_setxattr(struct pscrpc_request *rq)
{
	struct fidc_membh *f = NULL;
	struct srm_setxattr_req *mq;
	struct srm_setxattr_rep *mp;
	char name[SL_NAME_MAX + 1], value[SL_NAME_MAX + 1];
	int vfsid;
	struct iovec iov;

	SL_RSX_ALLOCREP(rq, mq, mp);
	if (mdsio_fid_to_vfsid(mq->fg.fg_fid, &vfsid) < 0) {
		mp->rc = -EINVAL;
		goto out;
	}
	if (mq->namelen  > SL_NAME_MAX ||
	    mq->valuelen > SL_NAME_MAX) {
		mp->rc = -EINVAL;
		goto out;
	}

	mp->rc = -slm_fcmh_get(&mq->fg, &f);
	if (mp->rc)
		goto out;

	memcpy(name, mq->name, mq->namelen);
	name[mq->namelen] = '\0';

	iov.iov_base = value;
	iov.iov_len = mq->valuelen;
	mp->rc = rsx_bulkserver(rq, BULK_GET_SINK, SRMC_BULK_PORTAL,
	    &iov, 1);
	if (mp->rc)
		goto out;

	mds_reserve_slot(1);
	mp->rc = mdsio_setxattr(vfsid, &rootcreds, name, value,
	    mq->valuelen,  fcmh_2_mdsio_fid(f));
	mds_unreserve_slot(1);

 out:
	if (f)
		fcmh_op_done(f);
	return (0);
}

int
slm_rmc_handle_getxattr(struct pscrpc_request *rq)
{
	struct fidc_membh *f = NULL;
	struct srm_getxattr_req *mq;
	struct srm_getxattr_rep *mp;
	char value[SL_NAME_MAX + 1];
	struct iovec iov;
	size_t outsize;
	int vfsid, abort_bulk = 0;

	SL_RSX_ALLOCREP(rq, mq, mp);
	if (mdsio_fid_to_vfsid(mq->fg.fg_fid, &vfsid) < 0) {
		if (mq->size)
			abort_bulk = 1;
		mp->rc = -EINVAL;
		goto out;
	}
	mp->rc = -slm_fcmh_get(&mq->fg, &f);
	if (mp->rc) {
		if (mq->size)
			abort_bulk = 1;
		goto out;
	}

	mp->valuelen = 0;
	mds_reserve_slot(1);
	mp->rc = mdsio_getxattr(vfsid, &rootcreds, mq->name, value,
	    mq->size, &outsize, fcmh_2_mdsio_fid(f));
	mds_unreserve_slot(1);
	if (mp->rc) {
		/* XXX compiler tweak ENOATTR */
		if (mp->rc == 5001)
			mp->rc = 0;
		if (mq->size)
			abort_bulk = 1;
		goto out;
	}
	mp->valuelen = outsize;

	iov.iov_base = value;
	iov.iov_len = outsize;
	if (mq->size) {
		mp->rc = rsx_bulkserver(rq, BULK_PUT_SOURCE,
		    SRMC_BULK_PORTAL, &iov, 1);
	}

 out:
	if (abort_bulk)
		pscrpc_msg_add_flags(rq->rq_repmsg, MSG_ABORT_BULK);
	if (f)
		fcmh_op_done(f);
	return (0);
}

int
slm_rmc_handle_removexattr(struct pscrpc_request *rq)
{
	struct fidc_membh *f = NULL;
	struct srm_removexattr_req *mq;
	struct srm_removexattr_rep *mp;
	int vfsid;

	SL_RSX_ALLOCREP(rq, mq, mp);
	if (mdsio_fid_to_vfsid(mq->fg.fg_fid, &vfsid) < 0) {
		mp->rc = -EINVAL;
		goto out;
	}
	mp->rc = -slm_fcmh_get(&mq->fg, &f);
	if (mp->rc)
		goto out;

	mq->name[sizeof(mq->name) - 1] = '\0';
	mds_reserve_slot(1);
	mp->rc = mdsio_removexattr(vfsid, &rootcreds, mq->name,
	    fcmh_2_mdsio_fid(f));
	mds_unreserve_slot(1);

 out:
	if (f)
		fcmh_op_done(f);
	return (0);
}

int
slm_rmc_handle_addreplrq(struct pscrpc_request *rq)
{
	struct srm_replrq_req *mq;
	struct srm_replrq_rep *mp;

	SL_RSX_ALLOCREP(rq, mq, mp);
	mp->rc = mds_repl_addrq(&mq->fg, mq->bmapno, mq->repls,
	    mq->nrepls);
	return (0);
}

int
slm_rmc_handle_delreplrq(struct pscrpc_request *rq)
{
	struct srm_replrq_req *mq;
	struct srm_replrq_rep *mp;

	SL_RSX_ALLOCREP(rq, mq, mp);
	mp->rc = mds_repl_delrq(&mq->fg, mq->bmapno, mq->repls,
	    mq->nrepls);
	return (0);
}

int
slm_rmc_handle_getreplst(struct pscrpc_request *rq)
{
	const struct srm_replst_master_req *mq;
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
	int rc = 0;

	if (rq->rq_reqmsg->opc != SRMT_CONNECT) {
		EXPORT_LOCK(rq->rq_export);
		if (rq->rq_export->exp_private == NULL)
			rc = -SLERR_NOTCONN;
		EXPORT_ULOCK(rq->rq_export);
		if (rc)
			goto out;
	}

	switch (rq->rq_reqmsg->opc) {
	/* bmap messages */
	case SRMT_BMAPCHWRMODE:
		rc = slm_rmc_handle_bmap_chwrmode(rq);
		break;
	case SRMT_EXTENDBMAPLS:
		rc = slm_rmc_handle_extendbmapls(rq);
		break;
	case SRMT_REASSIGNBMAPLS:
		rc = slm_rmc_handle_reassignbmapls(rq);
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
		rc = slrpc_handle_connect(rq, SRMC_MAGIC, SRMC_VERSION,
		    SLCONNT_CLI);
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
	case SRMT_MKNOD:
		rc = slm_rmc_handle_mknod(rq);
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
	case SRMT_LISTXATTR:
		rc = slm_rmc_handle_listxattr(rq);
		break;
	case SRMT_SETXATTR:
		rc = slm_rmc_handle_setxattr(rq);
		break;
	case SRMT_GETXATTR:
		rc = slm_rmc_handle_getxattr(rq);
		break;
	case SRMT_REMOVEXATTR:
		rc = slm_rmc_handle_removexattr(rq);
		break;
	default:
		psclog_errorx("Unexpected opcode %d",
		    rq->rq_reqmsg->opc);
		rq->rq_status = -ENOSYS;
		return (pscrpc_error(rq));
	}
 out:
	authbuf_sign(rq, PSCRPC_MSG_REPLY);
	pscrpc_target_send_reply_msg(rq, -(abs(rc)), 0);
	return (rc);
}

void
mexpc_allocpri(struct pscrpc_export *exp)
{
	struct slm_exp_cli *mexpc;

	mexpc = exp->exp_private = PSCALLOC(sizeof(*mexpc));
	slm_getclcsvc(exp);
}

struct sl_expcli_ops sl_expcli_ops = {
	mexpc_allocpri,
	NULL
};
