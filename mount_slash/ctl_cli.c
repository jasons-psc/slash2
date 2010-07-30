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

/*
 * Interface for controlling live operation of a mount_slash instance.
 */

#include <sys/param.h>
#include <sys/socket.h>

#include "pfl/cdefs.h"
#include "pfl/str.h"
#include "psc_rpc/rpc.h"
#include "psc_rpc/rsx.h"
#include "psc_util/ctl.h"
#include "psc_util/ctlsvr.h"
#include "psc_util/net.h"

#include "ctl.h"
#include "ctl_cli.h"
#include "ctlsvr.h"
#include "ctlsvr_cli.h"
#include "fuse_listener.h"
#include "mount_slash.h"
#include "rpc_cli.h"
#include "slashrpc.h"
#include "slerr.h"

struct psc_lockedlist	 psc_mlists;
struct psc_lockedlist	 psc_odtables;

struct psc_lockedlist	 msctl_replsts = PLL_INITIALIZER(&msctl_replsts,
    struct msctl_replstq, mrsq_lentry);

#define REPLRQ_BMAPNO_ALL (-1)

int
msctl_getcreds(int s, struct slash_creds *crp)
{
	return (pfl_socket_getpeercred(s, &crp->uid, &crp->gid));
}

int
msctlrep_replrq(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct slashrpc_cservice *csvc = NULL;
	struct pscrpc_request *rq = NULL;
	struct msctlmsg_replrq *mrq = m;
	struct srm_generic_rep *mp;
	struct srm_replrq_req *mq;
	struct slash_fidgen fg;
	struct slash_creds cr;
	struct srt_stat sstb;
	uint32_t n;
	int rc;

	if (mrq->mrq_nios < 1 ||
	    mrq->mrq_nios >= nitems(mrq->mrq_iosv))
		return (psc_ctlsenderr(fd, mh,
		    "replication request: %s",
		    slstrerror(EINVAL)));

	rc = msctl_getcreds(fd, &cr);
	if (rc)
		return (psc_ctlsenderr(fd, mh,
		    "unable to obtain credentials: %s",
		    mrq->mrq_fn, slstrerror(rc)));

	/* ensure path exists in the slash fs */
	rc = lookup_pathname_fg(mrq->mrq_fn, &cr, &fg, &sstb);
	if (rc)
		return (psc_ctlsenderr(fd, mh, "%s: %s",
		    mrq->mrq_fn, slstrerror(rc)));

	if (!S_ISREG(sstb.sst_mode))
		return (psc_ctlsenderr(fd, mh, "%s: %s",
		    mrq->mrq_fn, slstrerror(ENOTSUP)));

	rc = checkcreds(&sstb, &cr, W_OK);
	if (rc)
		return (psc_ctlsenderr(fd, mh, "%s: %s",
		    mrq->mrq_fn, slstrerror(rc)));

	rc = slc_rmc_getimp(&csvc);
	if (rc) {
		rc = psc_ctlsenderr(fd, mh, "%s: %s",
		    mrq->mrq_fn, slstrerror(rc));
		goto out;
	}
	rc = SL_RSX_NEWREQ(csvc->csvc_import, SRMC_VERSION,
	    mh->mh_type == MSCMT_ADDREPLRQ ?
	    SRMT_REPL_ADDRQ : SRMT_REPL_DELRQ, rq, mq, mp);
	if (rc) {
		rc = psc_ctlsenderr(fd, mh, "%s: %s",
		    mrq->mrq_fn, slstrerror(rc));
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

	rc = SL_RSX_WAITREP(rq, mp);
	if (rc == 0)
		rc = mp->rc;
	if (rc)
		rc = psc_ctlsenderr(fd, mh, "%s: %s",
		    mrq->mrq_fn, slstrerror(rc));
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
	struct msctl_replstq mrsq;
	struct slash_fidgen fg;
	struct slash_creds cr;
	struct srt_stat sstb;
	int added = 0, rc;
	char *displayfn;

	displayfn = mrq->mrq_fn;
	if (strcmp(mrq->mrq_fn, "") == 0) {
		displayfn = "<all active replications>";
		fg.fg_fid = FID_ANY;
		fg.fg_gen = FIDGEN_ANY;
		goto issue;
	}

	rc = msctl_getcreds(fd, &cr);
	if (rc)
		return (psc_ctlsenderr(fd, mh,
		    "unable to obtain credentials: %s",
		    mrq->mrq_fn, slstrerror(rc)));

	rc = lookup_pathname_fg(mrq->mrq_fn, &cr, &fg, &sstb);
	if (rc)
		return (psc_ctlsenderr(fd, mh, "%s: %s",
		    mrq->mrq_fn, slstrerror(rc)));

	if (!S_ISREG(sstb.sst_mode))
		return (psc_ctlsenderr(fd, mh, "%s: %s",
		    mrq->mrq_fn, slstrerror(ENOTSUP)));

	rc = checkcreds(&sstb, &cr, W_OK);
	if (rc)
		return (psc_ctlsenderr(fd, mh,
		    "%s: %s", mrq->mrq_fn, slstrerror(rc)));

 issue:
	rc = slc_rmc_getimp(&csvc);
	if (rc) {
		rc = psc_ctlsenderr(fd, mh, "%s: %s",
		    displayfn, slstrerror(rc));
		goto out;
	}
	rc = SL_RSX_NEWREQ(csvc->csvc_import, SRMC_VERSION,
	    SRMT_REPL_GETST, rq, mq, mp);
	if (rc) {
		rc = psc_ctlsenderr(fd, mh, "%s: %s",
		    displayfn, slstrerror(rc));
		goto out;
	}

	mq->fg = fg;
	mq->id = fd;

	memset(&mrsq, 0, sizeof(mrsq));
	mrsq.mrsq_fd = fd;
	mrsq.mrsq_fn = mrq->mrq_fn;
	mrsq.mrsq_ctlrc = 1;
	mrsq.mrsq_mh = mh;
	LOCK_INIT(&mrsq.mrsq_lock);
	psc_waitq_init(&mrsq.mrsq_waitq);
	spinlock(&mrsq.mrsq_lock);

	pll_add(&msctl_replsts, &mrsq);
	added = 1;

	rc = SL_RSX_WAITREP(rq, mp);
	if (rc == 0)
		rc = mp->rc;
	if (rc) {
		rc = psc_ctlsenderr(fd, mh, "%s: %s",
		    displayfn, slstrerror(rc));
		goto out;
	}

	while (mrsq.mrsq_ctlrc && mrsq.mrsq_eof == 0) {
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
	rc = mrsq.mrsq_ctlrc;
	pll_remove(&msctl_replsts, &mrsq);
	added = 0;
 out:
	if (added)
		pll_remove(&msctl_replsts, &mrsq);
	if (rq)
		pscrpc_req_finished(rq);
	if (csvc)
		sl_csvc_decref(csvc);
	return (rc);
}

int
msctlhnd_set_newreplpol(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct msctlmsg_fncmd_newreplpol *mfnrp = m;
	struct slashrpc_cservice *csvc = NULL;
	struct srm_set_newreplpol_req *mq;
	struct pscrpc_request *rq = NULL;
	struct srm_generic_rep *mp;
	struct slash_fidgen fg;
	struct slash_creds cr;
	struct srt_stat sstb;
	int rc;

	rc = msctl_getcreds(fd, &cr);
	if (rc)
		return (psc_ctlsenderr(fd, mh,
		    "unable to obtain credentials: %s",
		    mfnrp->mfnrp_fn, slstrerror(rc)));

	/* ensure path exists in the slash fs */
	rc = lookup_pathname_fg(mfnrp->mfnrp_fn, &cr, &fg, &sstb);
	if (rc)
		return (psc_ctlsenderr(fd, mh, "%s: %s",
		    mfnrp->mfnrp_fn, slstrerror(rc)));

	if (!S_ISREG(sstb.sst_mode))
		return (psc_ctlsenderr(fd, mh, "%s: %s",
		    mfnrp->mfnrp_fn, slstrerror(ENOTSUP)));

	rc = checkcreds(&sstb, &cr, W_OK);
	if (rc)
		return (psc_ctlsenderr(fd, mh,
		    "%s: %s", mfnrp->mfnrp_fn, slstrerror(rc)));

	rc = slc_rmc_getimp(&csvc);
	if (rc) {
		rc = psc_ctlsenderr(fd, mh, "%s: %s",
		    mfnrp->mfnrp_fn, slstrerror(rc));
		goto out;
	}
	rc = SL_RSX_NEWREQ(csvc->csvc_import, SRMC_VERSION,
	    SRMT_SET_NEWREPLPOL, rq, mq, mp);
	if (rc) {
		rc = psc_ctlsenderr(fd, mh, "%s: %s",
		    mfnrp->mfnrp_fn, slstrerror(rc));
		goto out;
	}
	mq->pol = mfnrp->mfnrp_pol;
	mq->fg = fg;
	rc = SL_RSX_WAITREP(rq, mp);
	if (rc == 0)
		rc = mp->rc;
	if (rc)
		rc = psc_ctlsenderr(fd, mh, "%s: %s",
		    mfnrp->mfnrp_fn, slstrerror(rc));

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
	struct msctlmsg_fncmd_bmapreplpol *mfbrp = m;
	struct slashrpc_cservice *csvc = NULL;
	struct srm_set_bmapreplpol_req *mq;
	struct pscrpc_request *rq = NULL;
	struct srm_generic_rep *mp;
	struct slash_fidgen fg;
	struct slash_creds cr;
	struct srt_stat sstb;
	int rc;

	rc = msctl_getcreds(fd, &cr);
	if (rc)
		return (psc_ctlsenderr(fd, mh,
		    "unable to obtain credentials: %s",
		    mfbrp->mfbrp_fn, slstrerror(rc)));

	/* ensure path exists in the file system */
	rc = lookup_pathname_fg(mfbrp->mfbrp_fn, &cr, &fg, &sstb);
	if (rc)
		return (psc_ctlsenderr(fd, mh, "%s: %s",
		    mfbrp->mfbrp_fn, slstrerror(rc)));

	if (!S_ISREG(sstb.sst_mode))
		return (psc_ctlsenderr(fd, mh, "%s: %s",
		    mfbrp->mfbrp_fn, slstrerror(ENOTSUP)));

	rc = checkcreds(&sstb, &cr, W_OK);
	if (rc)
		return (psc_ctlsenderr(fd, mh, "%s: %s",
		    mfbrp->mfbrp_fn, slstrerror(rc)));

	rc = slc_rmc_getimp(&csvc);
	if (rc) {
		rc = psc_ctlsenderr(fd, mh, "%s: %s",
		    mfbrp->mfbrp_fn, slstrerror(rc));
		goto out;
	}
	rc = SL_RSX_NEWREQ(csvc->csvc_import, SRMC_VERSION,
	    SRMT_SET_BMAPREPLPOL, rq, mq, mp);
	if (rc) {
		rc = psc_ctlsenderr(fd, mh, "%s: %s",
		    mfbrp->mfbrp_fn, slstrerror(rc));
		goto out;
	}
	mq->pol = mfbrp->mfbrp_pol;
	mq->bmapno = mfbrp->mfbrp_bmapno;
	mq->fg = fg;
	rc = SL_RSX_WAITREP(rq, mp);
	if (rc == 0)
		rc = mp->rc;
	if (rc)
		rc = psc_ctlsenderr(fd, mh, "%s: %s",
		    mfbrp->mfbrp_fn, slstrerror(rc));

 out:
	if (rq)
		pscrpc_req_finished(rq);
	if (csvc)
		sl_csvc_decref(csvc);
	return (rc);
}

struct psc_ctlop msctlops[] = {
	PSC_CTLDEFOPS,
	{ msctlrep_replrq,		sizeof(struct msctlmsg_replrq) },
	{ msctlrep_replrq,		sizeof(struct msctlmsg_replrq) },
	{ msctlrep_getreplst,		sizeof(struct msctlmsg_replst) },
	{ NULL,				0 },
	{ msctlhnd_set_newreplpol,	sizeof(struct msctlmsg_fncmd_newreplpol) },
	{ msctlhnd_set_bmapreplpol,	sizeof(struct msctlmsg_fncmd_bmapreplpol) },
	{ slctlrep_getconns,		sizeof(struct slctlmsg_conn) },
	{ slctlrep_getfiles,		sizeof(struct slctlmsg_file) }
};

void (*psc_ctl_getstats[])(struct psc_thread *, struct psc_ctlmsg_stats *) = {
/* 0 */ psc_ctlthr_stat,
/* 1 */ psc_ctlacthr_stat
};
int psc_ctl_ngetstats = nitems(psc_ctl_getstats);

int
slcctlcmd_exit(__unusedx int fd, __unusedx struct psc_ctlmsghdr *mh,
    __unusedx void *m)
{
	exit(0);
}

int
slcctlcmd_reconfig(__unusedx int fd, __unusedx struct psc_ctlmsghdr *mh,
    __unusedx void *m)
{
	psc_fatalx("not implemented");
	return (0);
}

int (*psc_ctl_cmds[])(int, struct psc_ctlmsghdr *, void *) = {
	slcctlcmd_exit,
	slcctlcmd_reconfig
};
int psc_ctl_ncmds = nitems(psc_ctl_cmds);

void
msctlthr_begin(__unusedx struct psc_thread *thr)
{
	psc_ctlthr_main(ctlsockfn, msctlops, nitems(msctlops), MSTHRT_CTLAC);
}

void
msctlthr_spawn(void)
{
	struct psc_thread *thr;

//	psc_ctlparam_register("faults", psc_ctlparam_faults);
	psc_ctlparam_register("log.file", psc_ctlparam_log_file);
	psc_ctlparam_register("log.format", psc_ctlparam_log_format);
	psc_ctlparam_register("log.level", psc_ctlparam_log_level);
	psc_ctlparam_register("pause", psc_ctlparam_pause);
	psc_ctlparam_register("pool", psc_ctlparam_pool);
	psc_ctlparam_register("rlim.nofile", psc_ctlparam_rlim_nofile);
	psc_ctlparam_register("run", psc_ctlparam_run);

	thr = pscthr_init(MSTHRT_CTL, 0, msctlthr_begin, NULL,
	    sizeof(struct psc_ctlthr), "msctlthr");
	pscthr_setready(thr);
}
