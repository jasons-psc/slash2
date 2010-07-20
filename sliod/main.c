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

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <gcrypt.h>

#include "pfl/pfl.h"
#include "pfl/str.h"
#include "psc_ds/listcache.h"
#include "psc_util/alloc.h"
#include "psc_util/ctlsvr.h"
#include "psc_util/thread.h"
#include "psc_util/usklndthr.h"

#include "authbuf.h"
#include "bmap_iod.h"
#include "buffer.h"
#include "fidc_iod.h"
#include "fidcache.h"
#include "pathnames.h"
#include "repl_iod.h"
#include "rpc_iod.h"
#include "slconfig.h"
#include "slerr.h"
#include "sliod.h"
#include "slvr.h"

GCRY_THREAD_OPTION_PTHREAD_IMPL;

int			 allow_root_uid = 1;
const char		*progname;

int
psc_usklndthr_get_type(const char *namefmt)
{
	if (strstr(namefmt, "lnacthr"))
		return (SLITHRT_LNETAC);
	return (SLITHRT_USKLNDPL);
}

void
psc_usklndthr_get_namev(char buf[PSC_THRNAME_MAX], const char *namefmt,
    va_list ap)
{
	size_t n;

	n = strlcpy(buf, "sli", PSC_THRNAME_MAX);
	if (n < PSC_THRNAME_MAX)
		vsnprintf(buf + n, PSC_THRNAME_MAX - n, namefmt, ap);
}

__dead void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [-X] [-D datadir] [-f cfgfile] [-S socket] mds-resource\n",
	    progname);
	exit(1);
}

int
main(int argc, char *argv[])
{
	struct slashrpc_cservice *csvc;
	const char *cfn, *sfn;
	int rc, c;

	/* gcrypt must be initialized very early on */
	gcry_control(GCRYCTL_SET_THREAD_CBS, &gcry_threads_pthread);
	if (!gcry_check_version(GCRYPT_VERSION))
		errx(1, "libgcrypt version mismatch");

	pfl_init();
	progname = argv[0];
	cfn = SL_PATH_CONF;
	sfn = SL_PATH_SLICTLSOCK;
	while ((c = getopt(argc, argv, "D:f:S:X")) != -1)
		switch (c) {
		case 'D':
			sl_datadir = optarg;
			break;
		case 'f':
			cfn = optarg;
			break;
		case 'S':
			sfn = optarg;
			break;
		case 'X':
			allow_root_uid = 1;
			break;
		default:
			usage();
		}
	argc -= optind;
	argv += optind;
	if (argc != 1)
		usage();

	pscthr_init(SLITHRT_CTL, 0, NULL, NULL,
	    sizeof(struct psc_ctlthr), "slictlthr");

	slcfg_parse(cfn);
	authbuf_checkkeyfile();
	authbuf_readkeyfile();

	libsl_init(PSCNET_SERVER, 0);

	sl_drop_privs(allow_root_uid);

	bmap_cache_init(sizeof(struct bmap_iod_info));
	fidc_init(sizeof(struct fcmh_iod_info),
	    FIDC_ION_DEFSZ, FIDC_ION_MAXSZ, NULL);
	bim_init();
	slvr_cache_init();
	sli_repl_init();
	sli_rpc_initsvc();
	slitimerthr_spawn();
	sliod_bmaprlsthr_spawn();
	lc_reginit(&bmapReapQ, struct bmapc_memb, bcm_lentry, "bmapReapQ");

	sli_rmi_setmds(argv[0]);
	rc = sli_rmi_getimp(&csvc);
	if (rc)
		psc_fatalx("MDS server '%s' unavailable: %s",
		    argv[0], slstrerror(rc));
	sl_csvc_decref(csvc);
//	slconnthr_spawn(SLITHRT_CONN, rmi_resm, "sli");

	slictlthr_main(sfn);
	/* NOTREACHED */
}
