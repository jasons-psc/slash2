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
 * Interface for controlling live operation of a sliod instance.
 */

#include "pfl/cdefs.h"
#include "pfl/str.h"
#include "psc_ds/lockedlist.h"
#include "psc_util/ctl.h"
#include "psc_util/ctlsvr.h"

#include "ctl.h"
#include "ctl_iod.h"
#include "ctlsvr.h"
#include "repl_iod.h"
#include "sliod.h"

struct psc_lockedlist psc_mlists;
struct psc_lockedlist psc_odtables;

int
slictlrep_getreplwkst(int fd, struct psc_ctlmsghdr *mh, void *m)
{
	struct slictlmsg_replwkst *srws = m;
	struct sli_repl_workrq *w;
	struct sl_resm *resm;
	int rc;

	rc = 1;
	PLL_LOCK(&sli_replwkq_active);
	PLL_FOREACH(w, &sli_replwkq_active) {
		resm = libsl_nid2resm(w->srw_nid);

		srws->srws_fg = w->srw_fg;
		srws->srws_bmapno = w->srw_bmapno;
		srws->srws_data_tot = SLASH_SLVR_SIZE * w->srw_nslvr_tot;
		srws->srws_data_cur = SLASH_SLVR_SIZE * w->srw_nslvr_cur;
		strlcpy(srws->srws_peer_addr, resm->resm_addrbuf,
		    sizeof(srws->srws_peer_addr));

		rc = psc_ctlmsg_sendv(fd, mh, srws);
		if (!rc)
			break;
	}
	PLL_ULOCK(&sli_replwkq_active);
	return (rc);
}

/*
 * slictlcmd_exit - handle an EXIT command to terminate execution.
 */
__dead int
slictlcmd_exit(__unusedx int fd, __unusedx struct psc_ctlmsghdr *mh,
    __unusedx void *m)
{
	exit(0);
}

struct psc_ctlop slictlops[] = {
	PSC_CTLDEFOPS,
	{ slictlrep_getreplwkst,	sizeof(struct slictlmsg_replwkst ) },
	{ slctlrep_getconns,		sizeof(struct slctlmsg_conn ) },
	{ slctlrep_getfcmh,		sizeof(struct slctlmsg_fcmh ) }
};

psc_ctl_thrget_t psc_ctl_thrgets[] = {
/* BMAPRLS	*/ NULL,
/* CONN		*/ NULL,
/* CTL		*/ psc_ctlthr_get,
/* CTLAC	*/ psc_ctlacthr_get,
/* LNETAC	*/ NULL,
/* REPLFIN	*/ NULL,
/* REPLPND	*/ NULL,
/* REPLREAP	*/ NULL,
/* RIC		*/ NULL,
/* RII		*/ NULL,
/* RIM		*/ NULL,
/* SLVR_CRC	*/ NULL,
/* TINTV	*/ NULL,
/* TIOS		*/ NULL,
/* USKLNDPL	*/ NULL
};

psc_ctl_cmd_t psc_ctl_cmds[] = {
	slictlcmd_exit
};

PFLCTL_SVR_DEFS;

void
slictlthr_main(const char *fn)
{
//	psc_ctlparam_register("faults", psc_ctlparam_faults);
	psc_ctlparam_register("log.file", psc_ctlparam_log_file);
	psc_ctlparam_register("log.format", psc_ctlparam_log_format);
	psc_ctlparam_register("log.level", psc_ctlparam_log_level);
	psc_ctlparam_register("pause", psc_ctlparam_pause);
	psc_ctlparam_register("pool", psc_ctlparam_pool);
	psc_ctlparam_register("rlim.nofile", psc_ctlparam_rlim_nofile);
	psc_ctlparam_register("run", psc_ctlparam_run);

	psc_ctlthr_main(fn, slictlops, nitems(slictlops), SLITHRT_CTLAC);
}
