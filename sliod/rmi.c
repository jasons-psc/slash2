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
 * Routines for issuing RPC requests to MDS from ION.
 */

#include <sys/types.h>
#include <sys/wait.h>

#include <stdint.h>

#include "pfl/rpc.h"
#include "pfl/rsx.h"

#include "bmap_iod.h"
#include "repl_iod.h"
#include "rpc_iod.h"
#include "slashrpc.h"
#include "slconfig.h"
#include "slconn.h"
#include "slerr.h"
#include "sliod.h"

struct sl_resm *rmi_resm;

int
sli_rmi_getcsvc(struct slashrpc_cservice **csvcp)
{
	int rc;

	*csvcp = sli_getmcsvc(rmi_resm);
	if (*csvcp)
		return (0);

	for (;;) {
		rc = 0;
		CSVC_LOCK(rmi_resm->resm_csvc);
		*csvcp = sli_getmcsvc(rmi_resm);
		if (*csvcp)
			break;
		sl_csvc_waitrel_s(rmi_resm->resm_csvc,
		    CSVC_RECONNECT_INTV);
	}
	CSVC_ULOCK(rmi_resm->resm_csvc);
	return (rc);
}

int
sli_rmi_setmds(const char *name)
{
	struct slashrpc_cservice *csvc;
	struct sl_resource *res;
//	struct sl_resm *old;
	lnet_nid_t nid;

	/* XXX kill any old MDS and purge any bmap updates being held */
//	slconnthr_unwatch(rmi_resm->resm_csvc);
//	old = rmi_resm;
//	sl_csvc_disable(old->resm_csvc);

	nid = libcfs_str2nid(name);
	if (nid == LNET_NID_ANY) {
		res = libsl_str2res(name);
		if (res == NULL)
			return (SLERR_RES_UNKNOWN);
		rmi_resm = psc_dynarray_getpos(&res->res_members, 0);
	} else
		rmi_resm = libsl_nid2resm(nid);

	if (sli_rmi_getcsvc(&csvc))
		psclog_errorx("error connecting to MDS");
	else {
		slconnthr_watch(sliconnthr, csvc, CSVCF_PING, NULL,
		    NULL);
		sl_csvc_decref(csvc);
	}
	return (0);
}

int
sli_rmi_issue_repl_schedwk(struct sli_repl_workrq *w)
{
	struct slashrpc_cservice *csvc = NULL;
	struct pscrpc_request *rq = NULL;
	struct srm_bmap_ptrunc_req *p_mq;
	struct srm_bmap_ptrunc_rep *p_mp;
	struct srm_repl_schedwk_req *mq;
	struct srm_repl_schedwk_rep *mp;
	int rc;

	rc = sli_rmi_getcsvc(&csvc);
	if (rc)
		goto out;

	if (w->srw_op == SLI_REPLWKOP_PTRUNC) {
		rc = SL_RSX_NEWREQ(csvc, SRMT_BMAP_PTRUNC, rq, p_mq,
		    p_mp);
		if (rc)
			goto out;
		p_mq->fg = w->srw_fg;
		p_mq->bmapno = w->srw_bmapno;
		p_mq->bgen = w->srw_bgen;
		p_mq->rc = w->srw_status;
		rc = SL_RSX_WAITREP(csvc, rq, p_mp);
		if (rc == 0)
			rc = p_mp->rc;
	} else {
		rc = SL_RSX_NEWREQ(csvc, SRMT_REPL_SCHEDWK, rq, mq, mp);
		if (rc)
			goto out;
		mq->src_resid = w->srw_src_res->res_id;
		mq->fg = w->srw_fg;
		mq->bmapno = w->srw_bmapno;
		mq->bgen = w->srw_bgen;
		mq->rc = w->srw_status;
		rc = SL_RSX_WAITREP(csvc, rq, mp);
		if (rc == 0)
			rc = mp->rc;
	}
	if (w->srw_status)
		psclog_errorx("sent error rc=%d", w->srw_status);

 out:
	if (rq)
		pscrpc_req_finished(rq);
	if (csvc)
		sl_csvc_decref(csvc);
	return (rc);
}
