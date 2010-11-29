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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "pfl/pfl.h"
#include "pfl/cdefs.h"
#include "psc_util/ctl.h"
#include "psc_util/ctlcli.h"
#include "psc_util/log.h"

#include "ctl.h"
#include "ctlcli.h"
#include "pathnames.h"

#include "slashd/ctl_mds.h"

void
slmrmcthr_pr(const struct psc_ctlmsg_thread *pcst)
{
	printf(" #open %8d #close %8d #stat %8d",
	    pcst->pcst_nopen, pcst->pcst_nclose, pcst->pcst_nstat);
}

void
slmrmmthr_pr(const struct psc_ctlmsg_thread *pcst)
{
	printf(" #open %8d", pcst->pcst_nopen);
}

void
packshow_conns(__unusedx const char *thr)
{
	psc_ctlmsg_push(SLMCMT_GETCONNS, sizeof(struct slctlmsg_conn));
}

void
packshow_fcmhs(__unusedx const char *thr)
{
	struct slctlmsg_fcmh *scf;

	scf = psc_ctlmsg_push(SLMCMT_GETFCMH, sizeof(struct slctlmsg_fcmh));
	scf->scf_fg.fg_fid = FID_ANY;
}

struct psc_ctlshow_ent psc_ctlshow_tab[] = {
	{ "connections",	packshow_conns },
	{ "fcmhs",		packshow_fcmhs },
	{ "loglevels",		psc_ctl_packshow_loglevels },
	{ "odtables",		psc_ctl_packshow_odtables },
	{ "rpcsvcs",		psc_ctl_packshow_rpcsvcs },
	{ "threads",		psc_ctl_packshow_threads },

	/* aliases */
	{ "conns",		packshow_conns },
	{ "fidcache",		packshow_fcmhs },
	{ "files",		packshow_fcmhs },
	{ "stats",		psc_ctl_packshow_threads },
	{ "thrstats",		psc_ctl_packshow_threads }
};

struct psc_ctlmsg_prfmt psc_ctlmsg_prfmts[] = {
	PSC_CTLMSG_PRFMT_DEFS,
	{ sl_conn_prhdr,	sl_conn_prdat,		sizeof(struct slctlmsg_conn),		NULL },
	{ sl_fcmh_prhdr,	sl_fcmh_prdat,		sizeof(struct slctlmsg_fcmh),		NULL }
};

psc_ctl_prthr_t psc_ctl_prthrs[] = {
/* BMAPTIMEO	*/ NULL,
/* COH		*/ NULL,
/* CTL		*/ psc_ctlthr_pr,
/* CTLAC	*/ psc_ctlacthr_pr,
/* CURSOR	*/ NULL,
/* FSSYNC	*/ NULL,
/* JNAMESPACE	*/ NULL,
/* JRECLAIM	*/ NULL,
/* JRNL		*/ NULL,
/* LNETAC	*/ NULL,
/* RCM		*/ NULL,
/* RMC		*/ slmrmcthr_pr,
/* RMI		*/ NULL,
/* RMM		*/ slmrmmthr_pr,
/* TINTV	*/ NULL,
/* TIOS		*/ NULL,
/* UPSCHED	*/ NULL,
/* USKLNDPL	*/ NULL
};

struct psc_ctlcmd_req psc_ctlcmd_reqs[] = {
	{ "exit",	SMCC_EXIT },
	{ "reconfig",	SMCC_RECONFIG }
};

PFLCTL_CLI_DEFS;

const char *progname;

__dead void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [-HI] [-c cmd] [-h table] [-i iostat] [-L listspec]\n"
	    "\t[-m meter] [-P pool] [-p param[=value]] [-S socket] [-s value]\n",
	    progname);
	exit(1);
}

struct psc_ctlopt opts[] = {
	{ 'c', PCOF_FUNC, psc_ctlparse_cmd },
	{ 'H', PCOF_FLAG, &psc_ctl_noheader },
	{ 'h', PCOF_FUNC, psc_ctlparse_hashtable },
	{ 'I', PCOF_FLAG, &psc_ctl_inhuman },
	{ 'i', PCOF_FUNC, psc_ctlparse_iostats },
	{ 'L', PCOF_FUNC, psc_ctlparse_lc },
	{ 'm', PCOF_FUNC, psc_ctlparse_meter },
	{ 'P', PCOF_FUNC, psc_ctlparse_pool },
	{ 'p', PCOF_FUNC, psc_ctlparse_param },
	{ 's', PCOF_FUNC, psc_ctlparse_show }
};

int
main(int argc, char *argv[])
{
	pfl_init();
	progname = argv[0];
	psc_ctlcli_main(SL_PATH_SLMCTLSOCK, argc, argv, opts,
	    nitems(opts));
	exit(0);
}
