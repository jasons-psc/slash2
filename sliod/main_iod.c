/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2006-2013, Pittsburgh Supercomputing Center (PSC).
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

#include <sys/param.h>
#include <sys/statvfs.h>
#include <sys/mount.h>
#include <sys/wait.h>

#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <gcrypt.h>

#include "pfl/pfl.h"
#include "pfl/str.h"
#include "pfl/listcache.h"
#include "psc_util/alloc.h"
#include "psc_util/ctlsvr.h"
#include "psc_util/fault.h"
#include "psc_util/random.h"
#include "psc_util/thread.h"
#include "psc_util/timerthr.h"
#include "psc_util/usklndthr.h"
#include "psc_util/iostats.h"

#include "authbuf.h"
#include "bmap_iod.h"
#include "buffer.h"
#include "fidc_iod.h"
#include "fidcache.h"
#include "mkfn.h"
#include "pathnames.h"
#include "repl_iod.h"
#include "rpc_iod.h"
#include "slconfig.h"
#include "slerr.h"
#include "sliod.h"
#include "slsubsys.h"
#include "slutil.h"
#include "slvr.h"

GCRY_THREAD_OPTION_PTHREAD_IMPL;

struct srt_statfs	 sli_ssfb;
psc_spinlock_t		 sli_ssfb_lock = SPINLOCK_INIT;
struct psc_thread	*sliconnthr;

uint32_t		 sys_upnonce;
int			 allow_root_uid = 1;
const char		*progname;

/* XXX put these in an array */
struct psc_iostats	sliod_wr_1b_stat;
struct psc_iostats	sliod_wr_1k_stat;
struct psc_iostats	sliod_wr_4k_stat;
struct psc_iostats	sliod_wr_16k_stat;
struct psc_iostats	sliod_wr_64k_stat;
struct psc_iostats	sliod_wr_128k_stat;
struct psc_iostats	sliod_wr_512k_stat;
struct psc_iostats	sliod_wr_1m_stat;

struct psc_iostats	sliod_rd_1b_stat;
struct psc_iostats	sliod_rd_1k_stat;
struct psc_iostats	sliod_rd_4k_stat;
struct psc_iostats	sliod_rd_16k_stat;
struct psc_iostats	sliod_rd_64k_stat;
struct psc_iostats	sliod_rd_128k_stat;
struct psc_iostats	sliod_rd_512k_stat;
struct psc_iostats	sliod_rd_1m_stat;

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

void
slistatfsthr_main(__unusedx struct psc_thread *thr)
{
	struct statvfs sfb;
	int rc;

	while (pscthr_run()) {
#ifdef HAVE_STATFS_FSTYPE
		struct statfs b;

		rc = statfs(globalConfig.gconf_fsroot, &b);
		if (rc == -1)
			psclog_error("statfs %s",
			    globalConfig.gconf_fsroot);
		statfs_2_statvfs(&b, &sfb);
#else
		rc = statvfs(globalConfig.gconf_fsroot, &sfb);
		if (rc == -1)
			psclog_error("statvfs %s",
			    globalConfig.gconf_fsroot);
#endif

		if (rc == 0) {
			spinlock(&sli_ssfb_lock);
			sl_externalize_statfs(&sfb, &sli_ssfb);
#ifdef HAVE_STATFS_FSTYPE
			strlcpy(sli_ssfb.sf_type, b.f_fstypename,
			    sizeof(sli_ssfb.sf_type));
#endif
			freelock(&sli_ssfb_lock);
		}
		sleep(5);
	}
}

int
slirmiconnthr_upcall(__unusedx void *arg)
{
	int rc;

	if (nodeResm->resm_res->res_selftest[0] == '\0')
		return (0);
	rc = system(nodeResm->resm_res->res_selftest);
	return (WEXITSTATUS(rc));
}

__dead void
usage(void)
{
	fprintf(stderr,
	    "usage: %s [-V] [-D datadir] [-f cfgfile] [-S socket] [mds-resource]\n",
	    progname);
	exit(1);
}

int
main(int argc, char *argv[])
{
	const char *cfn, *sfn, *p, *prefmds;
	sigset_t signal_set;
	int rc, c;

	/* gcrypt must be initialized very early on */
	gcry_control(GCRYCTL_SET_THREAD_CBS, &gcry_threads_pthread);
	if (!gcry_check_version(GCRYPT_VERSION))
		errx(1, "libgcrypt version mismatch");

	progname = argv[0];
	pfl_init();
	sl_subsys_register();
	psc_subsys_register(SLISS_SLVR, "slvr");

	psc_fault_register(SLI_FAULT_AIO_FAIL);
	psc_fault_register(SLI_FAULT_CRCUP_FAIL);
	psc_fault_register(SLI_FAULT_FSIO_READ_FAIL);

	sfn = SL_PATH_SLICTLSOCK;
	p = getenv("CTL_SOCK_FILE");
	if (p)
		sfn = p;

	cfn = SL_PATH_CONF;
	p = getenv("CONFIG_FILE");
	if (p)
		cfn = p;

	while ((c = getopt(argc, argv, "D:f:S:VX")) != -1)
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
		case 'V':
			errx(0, "revision is %d", SL_STK_VERSION);
		case 'X':
			allow_root_uid = 1;
			break;
		default:
			usage();
		}
	argc -= optind;
	argv += optind;
	if (argc > 1)
		usage();

	sigemptyset(&signal_set);
	sigaddset(&signal_set, SIGIO);
	sigprocmask(SIG_BLOCK, &signal_set, NULL);

	pscthr_init(SLITHRT_CTL, 0, NULL, NULL,
	    sizeof(struct psc_ctlthr), "slictlthr0");

	sys_upnonce = psc_random32();

	slcfg_parse(cfn);
	authbuf_checkkeyfile();
	authbuf_readkeyfile();

	libsl_init((SLI_RIM_NBUFS + SLI_RIC_NBUFS + SLI_RII_NBUFS) * 2);

	sl_drop_privs(allow_root_uid);

	bmap_cache_init(sizeof(struct bmap_iod_info));
	fidc_init(sizeof(struct fcmh_iod_info), FIDC_ION_DEFSZ);
	bim_init();
	sl_nbrqset = pscrpc_nbreqset_init(NULL, NULL);
	slvr_cache_init();

	psc_iostats_init(&sliod_wr_1b_stat,   "wr: < 1k");
	psc_iostats_init(&sliod_rd_1b_stat,   "rd: < 1k");
	psc_iostats_init(&sliod_wr_1k_stat,   "wr: 1k-4k");
	psc_iostats_init(&sliod_rd_1k_stat,   "rd: 1k-4k");
	psc_iostats_init(&sliod_wr_4k_stat,   "wr: 4k-16k");
	psc_iostats_init(&sliod_rd_4k_stat,   "rd: 4k-16k");
	psc_iostats_init(&sliod_wr_16k_stat,  "wr: 16k-64k");
	psc_iostats_init(&sliod_rd_16k_stat,  "rd: 16k-64k");
	psc_iostats_init(&sliod_wr_64k_stat,  "wr: 64k-128k");
	psc_iostats_init(&sliod_rd_64k_stat,  "rd: 64k-128k");
	psc_iostats_init(&sliod_wr_128k_stat, "wr: 128k-512k");
	psc_iostats_init(&sliod_rd_128k_stat, "rd: 128k-512k");
	psc_iostats_init(&sliod_wr_512k_stat, "wr: 512k-1m");
	psc_iostats_init(&sliod_rd_512k_stat, "rd: 512k-1m");
	psc_iostats_init(&sliod_wr_1m_stat,   "wr: >= 1m");
	psc_iostats_init(&sliod_rd_1m_stat,   "rd: >= 1m");

	psc_poolmaster_init(&bmap_rls_poolmaster, struct bmap_iod_rls,
	    bir_lentry, PPMF_AUTO, 64, 64, 0, NULL, NULL, NULL,
	    "bmaprls");
	bmap_rls_pool = psc_poolmaster_getmgr(&bmap_rls_poolmaster);

	sli_repl_init();
	pscthr_init(SLITHRT_STATFS, 0, slistatfsthr_main, NULL, 0,
	    "slistatfsthr");

	sliconnthr = slconnthr_spawn(SLITHRT_CONN, "sli",
	    slirmiconnthr_upcall, NULL);

	prefmds = globalConfig.gconf_prefmds;
	if (argc)
		prefmds = argv[0];
	rc = sli_rmi_setmds(prefmds);
	if (rc)
		psc_fatalx("invalid MDS %s: %s", argv[0],
		    slstrerror(rc));

	psc_assert(globalConfig.gconf_fsuuid);
	psclog_info("gconf_fsuuid=%"PRIx64, globalConfig.gconf_fsuuid);

	pscrpc_nbreapthr_spawn(sl_nbrqset, SLITHRT_NBRQ, "slinbrqthr");

	sli_rpc_initsvc();
	psc_tiosthr_spawn(SLITHRT_TIOS, "slitiosthr");
	slibmaprlsthr_spawn();

	slictlthr_main(sfn);
	/* NOTREACHED */
}
