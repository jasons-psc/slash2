/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2007-2013, Pittsburgh Supercomputing Center (PSC).
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
 * Interface for controlling live operation of a mount_slash instance.
 */

#include <sys/param.h>
#include <sys/socket.h>

#include "pfl/cdefs.h"
#include "pfl/fs.h"
#include "pfl/str.h"
#include "psc_rpc/rpc.h"
#include "psc_rpc/rsx.h"
#include "psc_util/ctl.h"
#include "psc_util/ctlsvr.h"
#include "psc_util/net.h"

#include "bmap.h"
#include "bmap_cli.h"
#include "ctl.h"
#include "ctl_cli.h"
#include "ctlsvr.h"
#include "ctlsvr_cli.h"
#include "fidc_cli.h"
#include "mount_slash.h"
#include "rpc_cli.h"
#include "slashrpc.h"
#include "slerr.h"

struct psc_lockedlist	 psc_odtables;

psc_atomic32_t		 msctl_id = PSC_ATOMIC32_INIT(0);
struct psc_lockedlist	 msctl_replsts = PLL_INIT(&msctl_replsts,
    struct msctl_replstq, mrsq_lentry);

#define REPLRQ_BMAPNO_ALL (-1)

int
msctl_getcreds(int s, struct pscfs_creds *pcrp)
{
	uid_t uid;
	gid_t gid;
	int rc;

	rc = pfl_socket_getpeercred(s, &uid, &gid);
	pcrp->pcr_uid = uid;
	pcrp->pcr_gid = gid;
	pcrp->pcr_ngid = 1;
	return (rc);
}

int
msctl_getclientctx(__unusedx int s, struct pscfs_clientctx *pfcc)
{
	pfcc->pfcc_pid = -1;
	return (0);
}

int
msctlrep_replrq(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct slashrpc_cservice *csvc = NULL;
	struct pscrpc_request *rq = NULL;
	struct msctlmsg_replrq *mrq = m;
	struct pscfs_clientctx pfcc;
	struct srm_replrq_rep *mp;
	struct srm_replrq_req *mq;
	struct slash_fidgen fg;
	struct pscfs_creds pcr;
	struct fidc_membh *f;
	uint32_t n;
	int rc;

	if (mrq->mrq_nios < 1 ||
	    mrq->mrq_nios >= nitems(mrq->mrq_iosv))
		return (psc_ctlsenderr(fd, mh,
		    "replication request: %s", slstrerror(EINVAL)));

	rc = msctl_getcreds(fd, &pcr);
	if (rc)
		return (psc_ctlsenderr(fd, mh,
		    SLPRI_FID": unable to obtain credentials: %s",
		    mrq->mrq_fid, slstrerror(rc)));
	rc = msctl_getclientctx(fd, &pfcc);
	if (rc)
		return (psc_ctlsenderr(fd, mh,
		    SLPRI_FID": unable to obtain client context: %s",
		    mrq->mrq_fid, slstrerror(rc)));

	rc = fidc_lookup_load_inode(mrq->mrq_fid, &f, &pfcc);
	if (rc)
		return (psc_ctlsenderr(fd, mh, SLPRI_FID": %s",
		    mrq->mrq_fid, slstrerror(rc)));

	FCMH_LOCK(f);
	if (!S_ISREG(f->fcmh_sstb.sst_mode) &&
	    !S_ISDIR(f->fcmh_sstb.sst_mode))
		rc = ENOTSUP;
	else
		rc = fcmh_checkcreds(f, &pcr, W_OK);
	fg = f->fcmh_fg;
	fcmh_op_done(f);

	if (rc)
		return (psc_ctlsenderr(fd, mh, SLPRI_FID": %s",
		    mrq->mrq_fid, slstrerror(rc)));

	MSL_RMC_NEWREQ_PFCC(&pfcc, f, csvc,
	    mh->mh_type == MSCMT_ADDREPLRQ ?
	    SRMT_REPL_ADDRQ : SRMT_REPL_DELRQ, rq, mq, mp, rc);
	if (rc) {
		rc = psc_ctlsenderr(fd, mh, SLPRI_FID": %s",
		    mrq->mrq_fid, slstrerror(rc));
		goto out;
	}

	/* parse I/O systems specified */
	for (n = 0; n < mrq->mrq_nios; n++, mq->nrepls++)
		if ((mq->repls[n].bs_id =
		    libsl_str2id(mrq->mrq_iosv[n])) == IOS_ID_ANY) {
			rc = psc_ctlsenderr(fd, mh,
			    "%s: unknown I/O system", mrq->mrq_iosv[n]);
			goto out;
		}
	memcpy(&mq->fg, &fg, sizeof(mq->fg));
	mq->bmapno = mrq->mrq_bmapno;

	rc = SL_RSX_WAITREP(csvc, rq, mp);
	if (rc == 0)
		rc = mp->rc;
	if (rc)
		rc = psc_ctlsenderr(fd, mh, SLPRI_FID": %s",
		    mrq->mrq_fid, slstrerror(rc));
	else
		rc = 1;

 out:
	if (rq)
		pscrpc_req_finished(rq);
	if (csvc)
		sl_csvc_decref(csvc);
	return (rc);
}

int
msctlrep_getreplst(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct slashrpc_cservice *csvc = NULL;
	struct pscrpc_request *rq = NULL;
	struct srm_replst_master_req *mq;
	struct srm_replst_master_rep *mp;
	struct msctlmsg_replrq *mrq = m;
	struct fidc_membh *f = NULL;
	struct pscfs_clientctx pfcc;
	struct msctl_replstq mrsq;
	struct slash_fidgen fg;
	struct pscfs_creds pcr;
	int added = 0, rc;

	if (mrq->mrq_fid == FID_ANY) {
		fg.fg_fid = FID_ANY;
		fg.fg_gen = FGEN_ANY;
		goto issue;
	}

	rc = msctl_getcreds(fd, &pcr);
	if (rc)
		return (psc_ctlsenderr(fd, mh,
		    SLPRI_FID": unable to obtain credentials: %s",
		    mrq->mrq_fid, slstrerror(rc)));
	rc = msctl_getclientctx(fd, &pfcc);
	if (rc)
		return (psc_ctlsenderr(fd, mh,
		    SLPRI_FID": unable to obtain client context: %s",
		    mrq->mrq_fid, slstrerror(rc)));

	rc = fidc_lookup_load_inode(mrq->mrq_fid, &f, &pfcc);
	if (rc)
		return (psc_ctlsenderr(fd, mh, SLPRI_FID": %s",
		    mrq->mrq_fid, slstrerror(rc)));

	FCMH_LOCK(f);
	if (!S_ISREG(f->fcmh_sstb.sst_mode) &&
	    !S_ISDIR(f->fcmh_sstb.sst_mode))
		rc = ENOTSUP;
	else
		rc = fcmh_checkcreds(f, &pcr, R_OK);
	fg = f->fcmh_fg;
	fcmh_op_done(f);

	if (rc)
		return (psc_ctlsenderr(fd, mh, SLPRI_FID": %s",
		    mrq->mrq_fid, slstrerror(rc)));

 issue:
	MSL_RMC_NEWREQ_PFCC(&pfcc, f, csvc, SRMT_REPL_GETST, rq, mq, mp,
	    rc);
	if (rc) {
		rc = psc_ctlsenderr(fd, mh, SLPRI_FID": %s",
		    fg.fg_fid, slstrerror(rc));
		goto out;
	}

	mq->fg = fg;
	mq->id = psc_atomic32_inc_getnew(&msctl_id);

	memset(&mrsq, 0, sizeof(mrsq));
	INIT_PSC_LISTENTRY(&mrsq.mrsq_lentry);
	INIT_SPINLOCK(&mrsq.mrsq_lock);
	psc_waitq_init(&mrsq.mrsq_waitq);
	spinlock(&mrsq.mrsq_lock);
	mrsq.mrsq_id = mq->id;
	mrsq.mrsq_fd = fd;
	mrsq.mrsq_fid = mrq->mrq_fid;
	mrsq.mrsq_mh = mh;

	pll_add(&msctl_replsts, &mrsq);
	added = 1;

	rc = SL_RSX_WAITREP(csvc, rq, mp);
	if (rc == 0)
		rc = mp->rc;
	if (rc) {
		rc = psc_ctlsenderr(fd, mh, SLPRI_FID": %s",
		    fg.fg_fid, slstrerror(rc));
		goto out;
	}

	while (mrsq.mrsq_rc == 0) {
		psc_waitq_wait(&mrsq.mrsq_waitq, &mrsq.mrsq_lock);
		spinlock(&mrsq.mrsq_lock);
	}

	freelock(&mrsq.mrsq_lock);
	PLL_LOCK(&msctl_replsts);
	spinlock(&mrsq.mrsq_lock);
	while (mrsq.mrsq_refcnt) {
		PLL_ULOCK(&msctl_replsts);
		psc_waitq_wait(&mrsq.mrsq_waitq, &mrsq.mrsq_lock);
		PLL_LOCK(&msctl_replsts);
		spinlock(&mrsq.mrsq_lock);
	}
	pll_remove(&msctl_replsts, &mrsq);
	PLL_ULOCK(&msctl_replsts);
	added = 0;

	rc = 1;
	if (mrsq.mrsq_rc && mrsq.mrsq_rc != EOF)
		rc = psc_ctlsenderr(fd, mh, SLPRI_FID": %s",
		    fg.fg_fid, slstrerror(mrsq.mrsq_rc));

 out:
	if (added) {
		pll_remove(&msctl_replsts, &mrsq);
		psc_waitq_destroy(&mrsq.mrsq_waitq);
	}
	if (rq)
		pscrpc_req_finished(rq);
	if (csvc)
		sl_csvc_decref(csvc);
	return (rc);
}

int
msctlhnd_set_newreplpol(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct msctlmsg_newreplpol *mfnrp = m;
	struct slashrpc_cservice *csvc = NULL;
	struct srm_set_newreplpol_req *mq;
	struct srm_set_newreplpol_rep *mp;
	struct pscrpc_request *rq = NULL;
	struct pscfs_clientctx pfcc;
	struct slash_fidgen fg;
	struct pscfs_creds pcr;
	struct fidc_membh *f;
	int rc;

	rc = msctl_getcreds(fd, &pcr);
	if (rc)
		return (psc_ctlsenderr(fd, mh,
		    SLPRI_FID": unable to obtain credentials: %s",
		    mfnrp->mfnrp_fid, slstrerror(rc)));
	rc = msctl_getclientctx(fd, &pfcc);
	if (rc)
		return (psc_ctlsenderr(fd, mh,
		    SLPRI_FID": unable to obtain client context: %s",
		    mfnrp->mfnrp_fid, slstrerror(rc)));

	rc = fidc_lookup_load_inode(mfnrp->mfnrp_fid, &f, &pfcc);
	if (rc)
		return (psc_ctlsenderr(fd, mh, SLPRI_FID": %s",
		    mfnrp->mfnrp_fid, slstrerror(rc)));

	FCMH_LOCK(f);
	if (!S_ISREG(f->fcmh_sstb.sst_mode) &&
	    !S_ISDIR(f->fcmh_sstb.sst_mode))
		rc = ENOTSUP;
	else
		rc = fcmh_checkcreds(f, &pcr, W_OK);
	fg = f->fcmh_fg;
	fcmh_op_done(f);

	if (rc)
		return (psc_ctlsenderr(fd, mh, SLPRI_FID": %s",
		    mfnrp->mfnrp_fid, slstrerror(rc)));

	MSL_RMC_NEWREQ_PFCC(&pfcc, f, csvc, SRMT_SET_NEWREPLPOL, rq, mq,
	    mp, rc);
	if (rc) {
		rc = psc_ctlsenderr(fd, mh, SLPRI_FID": %s",
		    mfnrp->mfnrp_fid, slstrerror(rc));
		goto out;
	}
	mq->pol = mfnrp->mfnrp_pol;
	mq->fg = fg;
	rc = SL_RSX_WAITREP(csvc, rq, mp);
	if (rc == 0)
		rc = mp->rc;
	if (rc)
		rc = psc_ctlsenderr(fd, mh, "%s: %s",
		    mfnrp->mfnrp_fid, slstrerror(rc));

 out:
	if (rq)
		pscrpc_req_finished(rq);
	if (csvc)
		sl_csvc_decref(csvc);
	return (rc);
}

int
msctlhnd_set_bmapreplpol(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct msctlmsg_bmapreplpol *mfbrp = m;
	struct slashrpc_cservice *csvc = NULL;
	struct srm_set_bmapreplpol_req *mq;
	struct srm_set_bmapreplpol_rep *mp;
	struct pscrpc_request *rq = NULL;
	struct pscfs_clientctx pfcc;
	struct slash_fidgen fg;
	struct pscfs_creds pcr;
	struct fidc_membh *f;
	int rc;

	rc = msctl_getcreds(fd, &pcr);
	if (rc)
		return (psc_ctlsenderr(fd, mh,
		    SLPRI_FID": unable to obtain credentials: %s",
		    mfbrp->mfbrp_fid, slstrerror(rc)));
	rc = msctl_getclientctx(fd, &pfcc);
	if (rc)
		return (psc_ctlsenderr(fd, mh,
		    SLPRI_FID": unable to obtain client context: %s",
		    mfbrp->mfbrp_fid, slstrerror(rc)));

	rc = fidc_lookup_load_inode(mfbrp->mfbrp_fid, &f, &pfcc);
	if (rc)
		return (psc_ctlsenderr(fd, mh, SLPRI_FID": %s",
		    mfbrp->mfbrp_fid, slstrerror(rc)));

	FCMH_LOCK(f);
	if (!S_ISREG(f->fcmh_sstb.sst_mode))
		rc = ENOTSUP;
	else
		rc = fcmh_checkcreds(f, &pcr, W_OK);
	fg = f->fcmh_fg;
	fcmh_op_done(f);

	if (rc)
		return (psc_ctlsenderr(fd, mh, SLPRI_FID": %s",
		    mfbrp->mfbrp_fid, slstrerror(rc)));

	MSL_RMC_NEWREQ_PFCC(&pfcc, f, csvc, SRMT_SET_BMAPREPLPOL, rq,
	    mq, mp, rc);
	if (rc) {
		rc = psc_ctlsenderr(fd, mh, SLPRI_FID": %s",
		    mfbrp->mfbrp_fid, slstrerror(rc));
		goto out;
	}
	mq->pol = mfbrp->mfbrp_pol;
	mq->bmapno = mfbrp->mfbrp_bmapno;
	mq->nbmaps = mfbrp->mfbrp_nbmaps;
	mq->fg = fg;
	rc = SL_RSX_WAITREP(csvc, rq, mp);
	if (rc == 0)
		rc = mp->rc;
	if (rc)
		rc = psc_ctlsenderr(fd, mh, SLPRI_FID": %s",
		    mfbrp->mfbrp_fid, slstrerror(rc));

 out:
	if (rq)
		pscrpc_req_finished(rq);
	if (csvc)
		sl_csvc_decref(csvc);
	return (rc);
}

void
msctlparam_mountpoint_get(char buf[PCP_VALUE_MAX])
{
	strlcpy(buf, mountpoint, PCP_VALUE_MAX);
}

void
msctlparam_prefios_get(char buf[PCP_VALUE_MAX])
{
	struct sl_resource *r;

	r = libsl_id2res(prefIOS);
	strlcpy(buf, r->res_name, PCP_VALUE_MAX);
}

int
msctlparam_prefios_set(const char *val)
{
	struct sl_resource *r;

	r = libsl_str2res(val);
	if (r == NULL)
		return (-1);
	prefIOS = r->res_id;
	return (0);
}

void
msctlparam_offlinenretries_get(char buf[PCP_VALUE_MAX])
{
	snprintf(buf, PCP_VALUE_MAX, "%d",
	    psc_atomic32_read(&max_nretries));
}

int
msctlparam_offlinenretries_set(const char *val)
{
	char *endp;
	long l;

	l = strtol(val, &endp, 10);
	if (l < 1 || l > 1000 ||
	    endp == val || *endp != '\0')
		return (-1);
	psc_atomic32_set(&max_nretries, l);
	return (0);
}

int
slctlmsg_bmap_send(int fd, struct psc_ctlmsghdr *mh,
    struct slctlmsg_bmap *scb, struct bmap *b)
{
	struct bmap_cli_info *bci;
	sl_ios_id_t id;

	bci = bmap_2_bci(b);
	scb->scb_fg = b->bcm_fcmh->fcmh_fg;
	scb->scb_bno = b->bcm_bmapno;
	scb->scb_opcnt = psc_atomic32_read(&b->bcm_opcnt);
	scb->scb_flags = b->bcm_flags;

	id = bci->bci_sbd.sbd_ios;
	strlcpy(scb->scb_resname, id == IOS_ID_ANY ? "<any>" :
	    libsl_id2res(id)->res_name, sizeof(scb->scb_resname));
	return (psc_ctlmsg_sendv(fd, mh, scb));
}

int
msctlmsg_biorq_send(int fd, struct psc_ctlmsghdr *mh,
    struct msctlmsg_biorq *msr, struct bmpc_ioreq *r)
{
	sl_ios_id_t id = r->biorq_last_sliod;

	memset(msr, 0, sizeof(*msr));
	msr->msr_fid = r->biorq_bmap->bcm_fcmh->fcmh_sstb.sst_fid;
	msr->msr_bno = r->biorq_bmap->bcm_bmapno;
	msr->msr_ref = r->biorq_ref;
	msr->msr_off = r->biorq_off;
	msr->msr_len = r->biorq_len;
	msr->msr_flags = r->biorq_flags;
	msr->msr_retries = r->biorq_retries;
	strlcpy(msr->msr_last_sliod, id == IOS_ID_ANY ? "<any>" :
	    libsl_id2res(id)->res_name, sizeof(msr->msr_last_sliod));
	msr->msr_expire.tv_sec = r->biorq_expire.tv_sec;
	msr->msr_expire.tv_nsec = r->biorq_expire.tv_nsec;
	msr->msr_npages = psc_dynarray_len(&r->biorq_pages);
	return (psc_ctlmsg_sendv(fd, mh, msr));
}

int
msctlrep_getbiorq(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct msctlmsg_biorq *msr = m;
	struct bmap_pagecache *bmpc;
	struct psc_lockedlist *pll;
	struct psc_hashbkt *hb;
	struct fidc_membh *f;
	struct bmpc_ioreq *r;
	struct bmap *b;
	int rc = 1;

	PSC_HASHTBL_FOREACH_BUCKET(hb, &fidcHtable) {
		psc_hashbkt_lock(hb);
		PSC_HASHBKT_FOREACH_ENTRY(&fidcHtable, f, hb) {
			FCMH_LOCK(f);
			SPLAY_FOREACH(b, bmap_cache, &f->fcmh_bmaptree) {
				bmpc = bmap_2_bmpc(b);

				pll = &bmpc->bmpc_pndg_biorqs;
				PLL_LOCK(pll);
				PLL_FOREACH(r, pll) {
					BIORQ_LOCK(r);
					rc = msctlmsg_biorq_send(fd, mh,
					    msr, r);
					BIORQ_ULOCK(r);
					if (!rc)
						break;
				}
				if (!rc) {
					PLL_ULOCK(pll);
					break;
				}

				SPLAY_FOREACH(r, bmpc_biorq_tree,
				    &bmpc->bmpc_new_biorqs) {
					BIORQ_LOCK(r);
					rc = msctlmsg_biorq_send(fd, mh,
					    msr, r);
					BIORQ_ULOCK(r);
					if (!rc)
						break;
				}
				BMPC_ULOCK(bmpc);
				if (!rc)
					break;
			}
			FCMH_ULOCK(f);
			if (!rc)
				break;
		}
		psc_hashbkt_unlock(hb);
		if (!rc)
			break;
	}
	return (rc);
}

const struct slctl_res_field slctl_resmds_fields[] = { { NULL, NULL } };
const struct slctl_res_field slctl_resios_fields[] = { { NULL, NULL } };

struct psc_ctlop msctlops[] = {
	PSC_CTLDEFOPS
/* ADDREPLRQ		*/ , { msctlrep_replrq,		sizeof(struct msctlmsg_replrq) }
/* DELREPLRQ		*/ , { msctlrep_replrq,		sizeof(struct msctlmsg_replrq) }
/* GETCONNS		*/ , { slctlrep_getconn,	sizeof(struct slctlmsg_conn) }
/* GETFCMH		*/ , { slctlrep_getfcmh,	sizeof(struct slctlmsg_fcmh) }
/* GETREPLST		*/ , { msctlrep_getreplst,	sizeof(struct msctlmsg_replst) }
/* GETREPLST_SLAVE	*/ , { NULL,			0 }
/* GET_BMAPREPLPOL	*/ , { NULL,			0 }
/* GET_NEWREPLPOL	*/ , { NULL,			0 }
/* SET_BMAPREPLPOL	*/ , { msctlhnd_set_bmapreplpol,sizeof(struct msctlmsg_bmapreplpol) }
/* SET_NEWREPLPOL	*/ , { msctlhnd_set_newreplpol,	sizeof(struct msctlmsg_newreplpol) }
/* GETBMAP		*/ , { slctlrep_getbmap,	sizeof(struct slctlmsg_bmap) }
/* GETBIORQ		*/ , { msctlrep_getbiorq,	sizeof(struct msctlmsg_biorq) }
};

psc_ctl_thrget_t psc_ctl_thrgets[] = {
/* ATTRFLSH	*/ NULL,
/* BMAPFLSH	*/ NULL,
/* BMAPFLSHRLS	*/ NULL,
/* BMAPFLSHRPC	*/ NULL,
/* BMAPLSWATCHER*/ NULL,
/* BMAPREADAHEAD*/ NULL,
/* CONN		*/ NULL,
/* CTL		*/ psc_ctlthr_get,
/* CTLAC	*/ psc_ctlacthr_get,
/* EQPOLL	*/ NULL,
/* FS		*/ NULL,
/* FSMGR	*/ NULL,
/* NBRQ		*/ NULL,
/* RCI		*/ NULL,
/* RCM		*/ NULL,
/* TIOS		*/ NULL,
/* USKLNDPL	*/ NULL
};

PFLCTL_SVR_DEFS;

void
msctlthr_main(__unusedx struct psc_thread *thr)
{
	psc_ctlthr_main(ctlsockfn, msctlops, nitems(msctlops),
	    MSTHRT_CTLAC);
}

void
msctlthr_spawn(void)
{
	struct psc_thread *thr;

	psc_ctlparam_register("faults", psc_ctlparam_faults);
	psc_ctlparam_register("log.file", psc_ctlparam_log_file);
	psc_ctlparam_register("log.format", psc_ctlparam_log_format);
	psc_ctlparam_register("log.level", psc_ctlparam_log_level);
	psc_ctlparam_register("opstats", psc_ctlparam_opstats);
	psc_ctlparam_register("pause", psc_ctlparam_pause);
	psc_ctlparam_register("pool", psc_ctlparam_pool);
	psc_ctlparam_register("rlim", psc_ctlparam_rlim);
	psc_ctlparam_register("run", psc_ctlparam_run);

	psc_ctlparam_register_simple("version", slctlparam_version_get,
	    NULL);

	/* XXX: add max_fs_iosz */
	psc_ctlparam_register_simple("mountpoint",
	    msctlparam_mountpoint_get, NULL);
	psc_ctlparam_register_simple("pref_ios",
	    msctlparam_prefios_get, msctlparam_prefios_set);
	psc_ctlparam_register_simple("offline_nretries",
	    msctlparam_offlinenretries_get,
	    msctlparam_offlinenretries_set);

	thr = pscthr_init(MSTHRT_CTL, 0, msctlthr_main, NULL,
	    sizeof(struct psc_ctlthr), "msctlthr0");
	pscthr_setready(thr);
}
