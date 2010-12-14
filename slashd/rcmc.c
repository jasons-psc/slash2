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
 * Routines for issuing RPC requests to CLIENT from MDS.
 */

#define PSC_SUBSYS PSS_RPC

#include <sys/param.h>

#include <dirent.h>

#include "psc_rpc/rpc.h"
#include "psc_rpc/rsx.h"
#include "psc_util/bitflag.h"
#include "psc_util/lock.h"
#include "psc_util/thread.h"

#include "bmap_mds.h"
#include "fid.h"
#include "inodeh.h"
#include "pathnames.h"
#include "repl_mds.h"
#include "rpc_mds.h"
#include "slashd.h"
#include "slashrpc.h"
#include "slerr.h"
#include "up_sched_res.h"

int
slmrmcthr_replst_slave_eof(struct slm_replst_workreq *rsw,
    struct up_sched_work_item *wk)
{
	struct srm_replst_slave_req *mq;
	struct srm_replst_slave_rep *mp;
	struct pscrpc_request *rq;
	int rc;

	rc = SL_RSX_NEWREQ(rsw->rsw_csvc->csvc_import,
	    SRCM_VERSION, SRMT_REPL_GETST_SLAVE, rq, mq, mp);
	if (rc)
		return (rc);

	mq->fg = *USWI_FG(wk);
	mq->id = rsw->rsw_cid;
	mq->rc = EOF;
	rc = SL_RSX_WAITREP(rq, mp);
	pscrpc_req_finished(rq);
	return (rc);
}

int
slmrmcthr_replst_slave_waitrep(struct pscrpc_request *rq, struct up_sched_work_item *wk)
{
	struct srm_replst_slave_req *mq;
	struct srm_replst_slave_rep *mp;
	struct pscrpc_bulk_desc *desc;
	struct slmrcm_thread *srcm;
	struct psc_thread *thr;
	struct iovec iov;
	int rc;

	thr = pscthr_get();
	srcm = slmrcmthr(thr);

	iov.iov_base = srcm->srcm_page;
	iov.iov_len = howmany(srcm->srcm_page_bitpos, NBBY);

	mq = pscrpc_msg_buf(rq->rq_reqmsg, 0, sizeof(*mq));
	mq->len = iov.iov_len;
	mq->nbmaps = srcm->srcm_page_bitpos / (SL_BITS_PER_REPLICA *
	    USWI_INOH(wk)->inoh_ino.ino_nrepls + SL_NBITS_REPLST_BHDR);
	rc = rsx_bulkclient(rq, &desc, BULK_GET_SOURCE,
	    SRCM_BULK_PORTAL, &iov, 1);
	if (rc == 0) {
		rc = SL_RSX_WAITREP(rq, mp);
		if (rc == 0)
			rc = mp->rc;
	}
	pscrpc_req_finished(rq);
	return (rc);
}

int
slmrcmthr_walk_brepls(struct slm_replst_workreq *rsw,
      struct up_sched_work_item *wk, struct bmapc_memb *bcm,
      sl_bmapno_t n, struct pscrpc_request **rqp)
{
	struct srm_replst_slave_req *mq;
	struct srm_replst_slave_rep *mp;
	struct srsm_replst_bhdr bhdr;
	struct bmap_mds_info *bmdsi;
	struct slmrcm_thread *srcm;
	struct psc_thread *thr;
	int nbits, rc;

	thr = pscthr_get();
	srcm = slmrcmthr(thr);
	bmdsi = bmap_2_bmdsi(bcm);

	nbits = USWI_INOH(wk)->inoh_ino.ino_nrepls *
	    SL_BITS_PER_REPLICA + SL_NBITS_REPLST_BHDR;
	if (howmany(srcm->srcm_page_bitpos + nbits,
	    NBBY) > SRM_REPLST_PAGESIZ || *rqp == NULL) {
		if (*rqp) {
			rc = slmrmcthr_replst_slave_waitrep(*rqp, wk);
			*rqp = NULL;
			if (rc)
				return (rc);
		}

		rc = SL_RSX_NEWREQ(rsw->rsw_csvc->csvc_import,
		    SRCM_VERSION, SRMT_REPL_GETST_SLAVE, *rqp, mq, mp);
		if (rc)
			return (rc);
		mq->id = rsw->rsw_cid;
		mq->boff = n;
		mq->fg = *USWI_FG(wk);

		srcm->srcm_page_bitpos = 0;
	}
	memset(&bhdr, 0, sizeof(bhdr));
	bhdr.srsb_replpol = bmap_2_replpol(bcm);
	pfl_bitstr_copy(srcm->srcm_page, srcm->srcm_page_bitpos,
	    &bhdr, 0, SL_NBITS_REPLST_BHDR);
	pfl_bitstr_copy(srcm->srcm_page, srcm->srcm_page_bitpos +
	    SL_NBITS_REPLST_BHDR, bcm->bcm_repls, 0,
	    USWI_INOH(wk)->inoh_ino.ino_nrepls * SL_BITS_PER_REPLICA);
	srcm->srcm_page_bitpos += nbits;
	return (0);
}

/*
 * slm_rcm_issue_getreplst - issue a GETREPLST reply to a CLIENT from MDS.
 */
int
slm_rcm_issue_getreplst(struct slm_replst_workreq *rsw,
    struct up_sched_work_item *wk, int is_eof)
{
	struct srm_replst_master_req *mq;
	struct srm_replst_master_rep *mp;
	struct pscrpc_request *rq;
	int rc;

	rc = SL_RSX_NEWREQ(rsw->rsw_csvc->csvc_import,
	    SRCM_VERSION, SRMT_REPL_GETST, rq, mq, mp);
	if (rc)
		return (rc);
	mq->id = rsw->rsw_cid;
	if (wk) {
		mq->fg = *USWI_FG(wk);
		mq->nbmaps = USWI_NBMAPS(wk);
		mq->nrepls = USWI_NREPLS(wk);
		mq->newreplpol = USWI_INO(wk)->ino_replpol;
		memcpy(mq->repls, USWI_INO(wk)->ino_repls,
		    MIN(mq->nrepls, SL_DEF_REPLICAS) * sizeof(*mq->repls));
		if (mq->nrepls > SL_DEF_REPLICAS)
			memcpy(mq->repls + SL_DEF_REPLICAS,
			    USWI_INOH(wk)->inoh_extras->inox_repls,
			    (USWI_NREPLS(wk) - SL_DEF_REPLICAS) *
			    sizeof(*mq->repls));
	}
	if (is_eof)
		mq->rc = EOF;
	rc = SL_RSX_WAITREP(rq, mp);
	pscrpc_req_finished(rq);
	return (rc);
}

void
slmrcmthr_main(struct psc_thread *thr)
{
	struct up_sched_work_item *wk;
	struct slm_replst_workreq *rsw;
	struct slmrcm_thread *srcm;
	struct pscrpc_request *rq;
	struct fidc_membh *fcmh;
	struct bmapc_memb *bcm;
	sl_bmapno_t n;
	int rc;

	srcm = slmrcmthr(thr);
	while (pscthr_run()) {
		rsw = lc_getwait(&slm_replst_workq);

		srcm->srcm_page_bitpos = SRM_REPLST_PAGESIZ * NBBY;

		rc = 0;
		rq = NULL;
		if (rsw->rsw_fg.fg_fid == FID_ANY) {
			PLL_LOCK(&upsched_listhd);
			PLL_FOREACH(wk, &upsched_listhd) {
				USWI_INCREF(wk, USWI_REFT_LOOKUP);
				if (!uswi_access(wk))
					continue;
				PLL_ULOCK(&upsched_listhd);

				rc = slm_rcm_issue_getreplst(rsw, wk, 0);
				for (n = 0; rc == 0 && n < USWI_NBMAPS(wk); n++) {
					if (mds_bmap_load(wk->uswi_fcmh, n, &bcm))
						continue;
					BMAP_LOCK(bcm);
					rc = slmrcmthr_walk_brepls(rsw, wk, bcm, n, &rq);
					bmap_op_done_type(bcm, BMAP_OPCNT_LOOKUP);
				}
				if (rq) {
					slmrmcthr_replst_slave_waitrep(rq, wk);
					rq = NULL;
				}
				slmrmcthr_replst_slave_eof(rsw, wk);
				PLL_LOCK(&upsched_listhd);
				uswi_unref(wk);
				if (rc)
					break;
			}
			PLL_ULOCK(&upsched_listhd);
		} else if ((wk = uswi_find(&rsw->rsw_fg, NULL)) != NULL) {
			rc = slm_rcm_issue_getreplst(rsw, wk, 0);
			for (n = 0; rc == 0 && n < USWI_NBMAPS(wk); n++) {
				if (mds_bmap_load(wk->uswi_fcmh, n, &bcm))
					continue;
				BMAP_LOCK(bcm);
				rc = slmrcmthr_walk_brepls(rsw, wk, bcm, n, &rq);
				bmap_op_done_type(bcm, BMAP_OPCNT_LOOKUP);
			}
			if (rq)
				slmrmcthr_replst_slave_waitrep(rq, wk);
			slmrmcthr_replst_slave_eof(rsw, wk);
			uswi_unref(wk);
		} else if (mds_repl_loadino(&rsw->rsw_fg, &fcmh) == 0) {
			/*
			 * File is not in cache: load it up to report
			 * replication status, grabbing a dummy uswi to
			 * pass around.
			 */
			wk = psc_pool_get(upsched_pool);
			uswi_init(wk, rsw->rsw_fg.fg_fid);
			wk->uswi_fcmh = fcmh;

			slm_rcm_issue_getreplst(rsw, wk, 0);
			for (n = 0; n < USWI_NBMAPS(wk); n++) {
				if (mds_bmap_load(wk->uswi_fcmh, n, &bcm))
					continue;
				BMAP_LOCK(bcm);
				rc = slmrcmthr_walk_brepls(rsw, wk, bcm, n, &rq);
				bmap_op_done_type(bcm, BMAP_OPCNT_LOOKUP);
				if (rc)
					break;
			}
			if (rq)
				slmrmcthr_replst_slave_waitrep(rq, wk);
			slmrmcthr_replst_slave_eof(rsw, wk);
			fcmh_op_done_type(fcmh, FCMH_OPCNT_LOOKUP_FIDC);
			psc_pool_return(upsched_pool, wk);
		}

		/* signal EOF */
		slm_rcm_issue_getreplst(rsw, NULL, 1);

		sl_csvc_decref(rsw->rsw_csvc);
		PSCFREE(rsw);
	}
}
