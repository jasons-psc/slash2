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

#include <sys/param.h>

#include <err.h>
#include <stdio.h>
#include <unistd.h>

#include "pfl/cdefs.h"
#include "psc_util/ctl.h"

/* start includes */
#include "authbuf.h"
#include "bmap.h"
#include "bmapdesc.h"
#include "buffer.h"
#include "cache_params.h"
#include "creds.h"
#include "ctl.h"
#include "ctlcli.h"
#include "ctlsvr.h"
#include "fid.h"
#include "fidcache.h"
#include "inode.h"
#include "inodeh.h"
#include "mkfn.h"
#include "pathnames.h"
#include "slashrpc.h"
#include "slconfig.h"
#include "slconn.h"
#include "slerr.h"
#include "sljournal.h"
#include "slsubsys.h"
#include "sltypes.h"
#include "slutil.h"
#include "mount_slash/bmap_cli.h"
#include "mount_slash/bmpc.h"
#include "mount_slash/ctl_cli.h"
#include "mount_slash/ctlsvr_cli.h"
#include "mount_slash/dircache.h"
#include "mount_slash/fidc_cli.h"
#include "mount_slash/mount_slash.h"
#include "mount_slash/rpc_cli.h"
#include "msctl/msctl.h"
#include "slashd/bmap_mds.h"
#include "slashd/ctl_mds.h"
#include "slashd/fidc_mds.h"
#include "slashd/mdscoh.h"
#include "slashd/mdsio.h"
#include "slashd/mdslog.h"
#include "slashd/namespace.h"
#include "slashd/repl_mds.h"
#include "slashd/rpc_mds.h"
#include "slashd/slashd.h"
#include "slashd/subsys_mds.h"
#include "slashd/up_sched_res.h"
#include "sliod/bmap_iod.h"
#include "sliod/ctl_iod.h"
#include "sliod/fidc_iod.h"
#include "sliod/repl_iod.h"
#include "sliod/rpc_iod.h"
#include "sliod/sliod.h"
#include "sliod/slvr.h"
#include "sliod/subsys_iod.h"
/* end includes */

struct bmap_ondisk bmapod;
char buf[1024 * 1024];
const char *progname;

void
pr(const char *name, uint64_t value, int hex)
{
	static int i;
	int n;

	if (i++ % 2) {
		n = printf("%s ", name);
		while (n++ <= 50)
			putchar('-');
		if (n < 53)
			printf("> ");
		if (hex)
			printf("%"PRIx64"\n", value);
		else
			printf("%"PRIu64"\n", value);
	} else {
		if (hex)
			printf("%-52s %"PRIx64"\n", name, value);
		else
			printf("%-52s %"PRIu64"\n", name, value);
	}
}

__dead void
usage(void)
{
	fprintf(stderr, "usage: %s\n", progname);
	exit(1);
}

int
main(int argc, char *argv[])
{
	uint64_t crc;
	int c;

	progname = argv[0];
	while ((c = getopt(argc, argv, "")) != -1)
		switch (c) {
		default:
			usage();
		}
	argc -= optind;
	if (argc)
		usage();

#define PRTYPE(type)	pr(#type, sizeof(type), 0)
#define PRVAL(val)	pr(#val, (unsigned long)(val), 0)
#define PRVALX(val)	pr(#val, (unsigned long)(val), 1)

	/* start structs */
	printf("structures:\n");
	PRTYPE(mdsio_fid_t);
	PRTYPE(sl_bmapgen_t);
	PRTYPE(sl_bmapno_t);
	PRTYPE(sl_ino_t);
	PRTYPE(sl_ios_id_t);
	PRTYPE(sl_siteid_t);
	PRTYPE(slfgen_t);
	PRTYPE(slfid_t);
	PRTYPE(struct biod_crcup_ref);
	PRTYPE(struct biod_infl_crcs);
	PRTYPE(struct bmap_cli_info);
	PRTYPE(struct bmap_core_state);
	PRTYPE(struct bmap_extra_state);
	PRTYPE(struct bmap_iod_info);
	PRTYPE(struct bmap_iod_minseq);
	PRTYPE(struct bmap_ion_assign);
	PRTYPE(struct bmap_mds_info);
	PRTYPE(struct bmap_mds_lease);
	PRTYPE(struct bmap_ondisk);
	PRTYPE(struct bmap_ops);
	PRTYPE(struct bmap_pagecache);
	PRTYPE(struct bmap_pagecache_entry);
	PRTYPE(struct bmap_timeo_entry);
	PRTYPE(struct bmap_timeo_table);
	PRTYPE(struct bmapc_memb);
	PRTYPE(struct bmpc_ioreq);
	PRTYPE(struct bmpc_mem_slbs);
	PRTYPE(struct cli_finfo);
	PRTYPE(struct dircache_desc);
	PRTYPE(struct dircache_ents);
	PRTYPE(struct dircache_info);
	PRTYPE(struct dircache_mgr);
	PRTYPE(struct fcmh_cli_info);
	PRTYPE(struct fcmh_iod_info);
	PRTYPE(struct fcmh_mds_info);
	PRTYPE(struct fidc_membh);
	PRTYPE(struct mdsio_ops);
	PRTYPE(struct msctl_replstq);
	PRTYPE(struct msctlmsg_fncmd_bmapreplpol);
	PRTYPE(struct msctlmsg_fncmd_newreplpol);
	PRTYPE(struct msctlmsg_replrq);
	PRTYPE(struct msctlmsg_replst);
	PRTYPE(struct msctlmsg_replst_slave);
	PRTYPE(struct msfs_thread);
	PRTYPE(struct msl_fhent);
	PRTYPE(struct msrcm_thread);
	PRTYPE(struct resm_cli_info);
	PRTYPE(struct resm_iod_info);
	PRTYPE(struct resm_mds_info);
	PRTYPE(struct resprof_mds_info);
	PRTYPE(struct site_mds_info);
	PRTYPE(struct site_progress);
	PRTYPE(struct sl_buffer);
	PRTYPE(struct sl_buffer_iovref);
	PRTYPE(struct sl_fcmh_ops);
	PRTYPE(struct sl_gconf);
	PRTYPE(struct sl_mds_crc_log);
	PRTYPE(struct sl_mds_logbuf);
	PRTYPE(struct sl_mds_nsstats);
	PRTYPE(struct sl_mds_peerinfo);
	PRTYPE(struct sl_resm);
	PRTYPE(struct sl_resource);
	PRTYPE(struct sl_site);
	PRTYPE(struct sl_timespec);
	PRTYPE(struct slash_creds);
	PRTYPE(struct slash_fidgen);
	PRTYPE(struct slash_inode_extras_od);
	PRTYPE(struct slash_inode_handle);
	PRTYPE(struct slash_inode_od);
	PRTYPE(struct slash_snapshot);
	PRTYPE(struct slashrpc_cservice);
	PRTYPE(struct slconn_thread);
	PRTYPE(struct slctlmsg_conn);
	PRTYPE(struct slctlmsg_fcmh);
	PRTYPE(struct sli_repl_workrq);
	PRTYPE(struct slictlmsg_replwkst);
	PRTYPE(struct sliric_thread);
	PRTYPE(struct slirii_thread);
	PRTYPE(struct slirim_thread);
	PRTYPE(struct slm_exp_cli);
	PRTYPE(struct slm_replst_workreq);
	PRTYPE(struct slmds_jent_bmapseq);
	PRTYPE(struct slmds_jent_crc);
	PRTYPE(struct slmds_jent_ino_addrepl);
	PRTYPE(struct slmds_jent_namespace);
	PRTYPE(struct slmds_jent_repgen);
	PRTYPE(struct slmds_jents);
	PRTYPE(struct slmjns_thread);
	PRTYPE(struct slmrcm_thread);
	PRTYPE(struct slmrmc_thread);
	PRTYPE(struct slmrmi_thread);
	PRTYPE(struct slmrmm_thread);
	PRTYPE(struct slmupsched_thread);
	PRTYPE(struct slvr_ref);
	PRTYPE(struct srm_bmap_chwrmode_rep);
	PRTYPE(struct srm_bmap_chwrmode_req);
	PRTYPE(struct srm_bmap_crcup);
	PRTYPE(struct srm_bmap_crcwire);
	PRTYPE(struct srm_bmap_crcwrt_rep);
	PRTYPE(struct srm_bmap_crcwrt_req);
	PRTYPE(struct srm_bmap_dio_req);
	PRTYPE(struct srm_bmap_id);
	PRTYPE(struct srm_bmap_iod_get);
	PRTYPE(struct srm_bmap_minseq_get);
	PRTYPE(struct srm_bmap_release_rep);
	PRTYPE(struct srm_bmap_release_req);
	PRTYPE(struct srm_connect_req);
	PRTYPE(struct srm_create_rep);
	PRTYPE(struct srm_create_req);
	PRTYPE(struct srm_destroy_req);
	PRTYPE(struct srm_garbage_req);
	PRTYPE(struct srm_generic_rep);
	PRTYPE(struct srm_getattr2_rep);
	PRTYPE(struct srm_getattr_rep);
	PRTYPE(struct srm_getattr_req);
	PRTYPE(struct srm_getbmap_full_rep);
	PRTYPE(struct srm_getbmap_full_req);
	PRTYPE(struct srm_io_rep);
	PRTYPE(struct srm_io_req);
	PRTYPE(struct srm_leasebmap_rep);
	PRTYPE(struct srm_leasebmap_req);
	PRTYPE(struct srm_link_req);
	PRTYPE(struct srm_lookup_req);
	PRTYPE(struct srm_mkdir_req);
	PRTYPE(struct srm_mknod_req);
	PRTYPE(struct srm_ping_req);
	PRTYPE(struct srm_readdir_rep);
	PRTYPE(struct srm_readdir_req);
	PRTYPE(struct srm_readlink_rep);
	PRTYPE(struct srm_readlink_req);
	PRTYPE(struct srm_rename_req);
	PRTYPE(struct srm_repl_read_req);
	PRTYPE(struct srm_repl_schedwk_req);
	PRTYPE(struct srm_replrq_req);
	PRTYPE(struct srm_replst_master_req);
	PRTYPE(struct srm_replst_slave_req);
	PRTYPE(struct srm_send_namespace_rep);
	PRTYPE(struct srm_send_namespace_req);
	PRTYPE(struct srm_set_bmapreplpol_req);
	PRTYPE(struct srm_set_newreplpol_req);
	PRTYPE(struct srm_setattr_req);
	PRTYPE(struct srm_statfs_rep);
	PRTYPE(struct srm_statfs_req);
	PRTYPE(struct srm_symlink_req);
	PRTYPE(struct srm_unlink_req);
	PRTYPE(struct srsm_replst_bhdr);
	PRTYPE(struct srt_authbuf_footer);
	PRTYPE(struct srt_authbuf_secret);
	PRTYPE(struct srt_bmapdesc);
	PRTYPE(struct srt_stat);
	PRTYPE(struct srt_statfs);
	PRTYPE(struct up_sched_work_item);
	/* end structs */

	/* start constants */
	printf("\nvalues:\n");
	PRVAL(AUTHBUF_KEYSIZE);
	PRVAL(AUTHBUF_REPRLEN);
	PRVAL(BIM_MINAGE);
	PRVAL(BIM_RETRIEVE_SEQ);
	PRVAL(BIOD_CRCUP_MAX_AGE);
	PRVAL(BMAP_CLI_DIOWAIT_SECS);
	PRVAL(BMAP_CLI_MAX_LEASE);
	PRVAL(BMAP_CLI_TIMEO_INC);
	PRVAL(BMAP_SEQLOG_FACTOR);
	PRVAL(BMAP_TIMEO_MAX);
	PRVAL(BMAP_TIMEO_TBL_QUANT);
	PRVAL(BMPC_DEFSLBS);
	PRVAL(BMPC_IOMAXBLKS);
	PRVAL(BMPC_MAXSLBS);
	PRVAL(BMPC_SLB_NBLKS);
	PRVAL(BPHXC);
	PRVAL(BREPLST_BADCRC);
	PRVAL(BREPLST_GARBAGE);
	PRVAL(BREPLST_GARBAGE_SCHED);
	PRVAL(BREPLST_INVALID);
	PRVAL(BREPLST_REPL_QUEUED);
	PRVAL(BREPLST_REPL_SCHED);
	PRVAL(BREPLST_TRUNCPNDG);
	PRVAL(BREPLST_VALID);
	PRVAL(BRP_ONETIME);
	PRVAL(BRP_PERSIST);
	PRVAL(CSVC_RECONNECT_INTV);
	PRVAL(DEF_READDIR_NENTS);
	PRVAL(FCMH_ATTR_TIMEO);
	PRVAL(FCMH_SETATTRF_NONE);
	PRVAL(FIDC_LOOKUP_NONE);
	PRVAL(FID_MAX_PATH);
	PRVAL(FID_PATH_DEPTH);
	PRVAL(FID_PATH_LEN);
	PRVAL(FSID_LEN);
	PRVAL(GCONF_HASHTBL_SZ);
	PRVAL(INTRES_NAME_MAX);
	PRVAL(LNET_NAME_MAX);
	PRVAL(MAX_BMAP_INODE_PAIRS);
	PRVAL(MAX_BMAP_NCRC_UPDATES);
	PRVAL(MAX_BMAP_RELEASE);
	PRVAL(MAX_READDIR_NENTS);
	PRVAL(MDSCOH_BLOCK);
	PRVAL(MDSCOH_NONBLOCK);
	PRVAL(MDSIO_FID_ROOT);
	PRVAL(MDS_LOG_MAX_LOG_BATCH);
	PRVAL(MSCC_EXIT);
	PRVAL(MSCC_RECONFIG);
	PRVAL(MSCMT_ADDREPLRQ);
	PRVAL(MSCMT_DELREPLRQ);
	PRVAL(MSCMT_GETCONNS);
	PRVAL(MSCMT_GETFCMH);
	PRVAL(MSCMT_GETREPLST);
	PRVAL(MSCMT_GETREPLST_SLAVE);
	PRVAL(MSCMT_SET_BMAPREPLPOL);
	PRVAL(MSCMT_SET_NEWREPLPOL);
	PRVAL(NBREPLST);
	PRVAL(NBRP);
	PRVAL(NSLVRCRC_THRS);
	PRVAL(NUM_BMAP_FLUSH_THREADS);
	PRVAL(REPL_MAX_INFLIGHT_SLVRS);
	PRVAL(SICC_EXIT);
	PRVAL(SICC_RECONFIG);
	PRVAL(SITE_NAME_MAX);
	PRVAL(SLASH_BLKS_PER_SLVR);
	PRVAL(SLASH_BMAP_CRCSIZE);
	PRVAL(SLASH_BMAP_SIZE);
	PRVAL(SLASH_BMAP_SIZE);
	PRVAL(SLASH_CRCS_PER_BMAP);
	PRVAL(SLASH_ID_FID_BITS);
	PRVAL(SLASH_ID_FID_BITS);
	PRVAL(SLASH_ID_FLAG_BITS);
	PRVAL(SLASH_ID_FLAG_BITS);
	PRVAL(SLASH_ID_SITE_BITS);
	PRVAL(SLASH_ID_SITE_BITS);
	PRVAL(SLASH_ID_SITE_BITS);
	PRVAL(SLASH_MAXBLKS_PER_REQ);
	PRVAL(SLASH_SLVRS_PER_BMAP);
	PRVAL(SLASH_SLVRS_PER_BMAP);
	PRVAL(SLASH_SLVR_BLKMASK);
	PRVAL(SLASH_SLVR_BLKSZ);
	PRVAL(SLASH_SLVR_BLKSZ);
	PRVAL(SLASH_SLVR_SIZE);
	PRVAL(SLB_MAX);
	PRVAL(SLB_MIN);
	PRVAL(SLB_NBLK);
	PRVAL(SLB_NDEF);
	PRVAL(SLB_RP_TIMEOUT_NSECS);
	PRVAL(SLB_RP_TIMEOUT_SECS);
	PRVAL(SLB_TIMEOUT_NSECS);
	PRVAL(SLB_TIMEOUT_SECS);
	PRVAL(SLCTL_REST_CLI);
	PRVAL(SLFID_MIN);
	PRVAL(SLFID_ROOT);
	PRVAL(SLI_RIC_BUFSZ);
	PRVAL(SLI_RIC_NBUFS);
	PRVAL(SLI_RIC_NTHREADS);
	PRVAL(SLI_RIC_REPSZ);
	PRVAL(SLI_RII_BUFSZ);
	PRVAL(SLI_RII_NBUFS);
	PRVAL(SLI_RII_NTHREADS);
	PRVAL(SLI_RII_REPSZ);
	PRVAL(SLI_RIM_BUFSZ);
	PRVAL(SLI_RIM_NBUFS);
	PRVAL(SLI_RIM_NTHREADS);
	PRVAL(SLI_RIM_REPSZ);
	PRVAL(SLJ_MDS_ENTSIZE);
	PRVAL(SLJ_MDS_PJET_BMAP);
	PRVAL(SLJ_MDS_PJET_INODE);
	PRVAL(SLJ_MDS_PJET_INUM);
	PRVAL(SLJ_MDS_PJET_VOID);
	PRVAL(SLJ_MDS_RA);
	PRVAL(SLJ_NAMES_MAX);
	PRVAL(SLM_NAMESPACE_BATCH);
	PRVAL(SLM_RMC_BUFSZ);
	PRVAL(SLM_RMC_NBUFS);
	PRVAL(SLM_RMC_NTHREADS);
	PRVAL(SLM_RMC_REPSZ);
	PRVAL(SLM_RMI_BUFSZ);
	PRVAL(SLM_RMI_NBUFS);
	PRVAL(SLM_RMI_NTHREADS);
	PRVAL(SLM_RMI_REPSZ);
	PRVAL(SLM_RMM_BUFSZ);
	PRVAL(SLM_RMM_NBUFS);
	PRVAL(SLM_RMM_NTHREADS);
	PRVAL(SLM_RMM_REPSZ);
	PRVAL(SL_BITS_PER_REPLICA);
	PRVAL(SL_DEF_REPLICAS);
	PRVAL(SL_DEF_SNAPSHOTS);
	PRVAL(SL_INFLIGHT_DEC);
	PRVAL(SL_INFLIGHT_INC);
	PRVAL(SL_MAX_GENS_PER_BLK);
	PRVAL(SL_MAX_REPLICAS);
	PRVAL(SL_PEER_MAX);
	PRVAL(SL_RES_BITS);
	PRVAL(SL_SITE_BITS);
	PRVAL(SMCC_EXIT);
	PRVAL(SMCC_RECONFIG);
	PRVAL(SP_FLAG_NONE);
	PRVAL(SRCM_BUFSZ);
	PRVAL(SRCM_BULK_PORTAL);
	PRVAL(SRCM_CTL_PORTAL);
	PRVAL(SRCM_NBUFS);
	PRVAL(SRCM_NTHREADS);
	PRVAL(SRCM_REPSZ);
	PRVAL(SRCM_REP_PORTAL);
	PRVAL(SRCM_REQ_PORTAL);
	PRVAL(SRCM_VERSION);
	PRVAL(SRIC_BULK_PORTAL);
	PRVAL(SRIC_CTL_PORTAL);
	PRVAL(SRIC_REP_PORTAL);
	PRVAL(SRIC_REQ_PORTAL);
	PRVAL(SRIC_VERSION);
	PRVAL(SRII_BULK_PORTAL);
	PRVAL(SRII_CTL_PORTAL);
	PRVAL(SRII_REP_PORTAL);
	PRVAL(SRII_REQ_PORTAL);
	PRVAL(SRII_VERSION);
	PRVAL(SRIM_BULK_PORTAL);
	PRVAL(SRIM_CTL_PORTAL);
	PRVAL(SRIM_REP_PORTAL);
	PRVAL(SRIM_REQ_PORTAL);
	PRVAL(SRIM_VERSION);
	PRVAL(SRMC_BULK_PORTAL);
	PRVAL(SRMC_CTL_PORTAL);
	PRVAL(SRMC_REP_PORTAL);
	PRVAL(SRMC_REQ_PORTAL);
	PRVAL(SRMC_VERSION);
	PRVAL(SRMIOP_RD);
	PRVAL(SRMIOP_WR);
	PRVAL(SRMI_BULK_PORTAL);
	PRVAL(SRMI_CTL_PORTAL);
	PRVAL(SRMI_REP_PORTAL);
	PRVAL(SRMI_REQ_PORTAL);
	PRVAL(SRMI_VERSION);
	PRVAL(SRMM_BULK_PORTAL);
	PRVAL(SRMM_CTL_PORTAL);
	PRVAL(SRMM_REP_PORTAL);
	PRVAL(SRMM_REQ_PORTAL);
	PRVAL(SRMM_VERSION);
	PRVAL(_SLERR_START);
	PRVAL(dirent_timeo);
	/* end constants */

	/* start enums */
	printf("\nenums:\n");
	PRVAL(BMAP_OPCNT_BCRSCHED);
	PRVAL(BMAP_OPCNT_BIORQ);
	PRVAL(BMAP_OPCNT_COHCB);
	PRVAL(BMAP_OPCNT_IONASSIGN);
	PRVAL(BMAP_OPCNT_LEASE);
	PRVAL(BMAP_OPCNT_LOOKUP);
	PRVAL(BMAP_OPCNT_MDSLOG);
	PRVAL(BMAP_OPCNT_REAPER);
	PRVAL(BMAP_OPCNT_REPLWK);
	PRVAL(BMAP_OPCNT_RLSSCHED);
	PRVAL(BMAP_OPCNT_SLVR);
	PRVAL(BMAP_OPCNT_TRUNCWAIT);
	PRVAL(FCMH_OPCNT_BMAP);
	PRVAL(FCMH_OPCNT_DIRENTBUF);
	PRVAL(FCMH_OPCNT_LOOKUP_FIDC);
	PRVAL(FCMH_OPCNT_NEW);
	PRVAL(FCMH_OPCNT_OPEN);
	PRVAL(FCMH_OPCNT_UPSCHED);
	PRVAL(FCMH_OPCNT_WAIT);
	PRVAL(INOH_EXTRAS_DIRTY);
	PRVAL(INOH_HAVE_EXTRAS);
	PRVAL(INOH_INO_DIRTY);
	PRVAL(INOH_INO_NEW);
	PRVAL(INOH_INO_NOTLOADED);
	PRVAL(INOH_INO_SYNCING);
	PRVAL(MSTHRT_BMAPFLSH);
	PRVAL(MSTHRT_BMAPFLSHRLS);
	PRVAL(MSTHRT_BMAPFLSHRPC);
	PRVAL(MSTHRT_CONN);
	PRVAL(MSTHRT_CTL);
	PRVAL(MSTHRT_CTLAC);
	PRVAL(MSTHRT_EQPOLL);
	PRVAL(MSTHRT_FS);
	PRVAL(MSTHRT_FSMGR);
	PRVAL(MSTHRT_LNETAC);
	PRVAL(MSTHRT_RCM);
	PRVAL(MSTHRT_TIOS);
	PRVAL(MSTHRT_USKLNDPL);
	PRVAL(NS_DIR_RECV);
	PRVAL(NS_DIR_SEND);
	PRVAL(NS_NDIRS);
	PRVAL(NS_NOPS);
	PRVAL(NS_NSUMS);
	PRVAL(NS_OP_CREATE);
	PRVAL(NS_OP_LINK);
	PRVAL(NS_OP_MKDIR);
	PRVAL(NS_OP_RENAME);
	PRVAL(NS_OP_RMDIR);
	PRVAL(NS_OP_SETATTR);
	PRVAL(NS_OP_SYMLINK);
	PRVAL(NS_OP_UNLINK);
	PRVAL(NS_SUM_FAIL);
	PRVAL(NS_SUM_PEND);
	PRVAL(NS_SUM_SUCC);
	PRVAL(SLBREF_MAPPED);
	PRVAL(SLBREF_REAP);
	PRVAL(SLB_DIRTY);
	PRVAL(SLB_FREE);
	PRVAL(SLB_FREEING);
	PRVAL(SLB_FRESH);
	PRVAL(SLB_INFLIGHT);
	PRVAL(SLB_INIT);
	PRVAL(SLB_LRU);
	PRVAL(SLB_PINNED);
	PRVAL(SLCONNT_CLI);
	PRVAL(SLCONNT_IOD);
	PRVAL(SLCONNT_MDS);
	PRVAL(SLITHRT_BMAPRLS);
	PRVAL(SLITHRT_CONN);
	PRVAL(SLITHRT_CTL);
	PRVAL(SLITHRT_CTLAC);
	PRVAL(SLITHRT_LNETAC);
	PRVAL(SLITHRT_REPLFIN);
	PRVAL(SLITHRT_REPLPND);
	PRVAL(SLITHRT_REPLREAP);
	PRVAL(SLITHRT_RIC);
	PRVAL(SLITHRT_RII);
	PRVAL(SLITHRT_RIM);
	PRVAL(SLITHRT_SLVR_CRC);
	PRVAL(SLITHRT_TIOS);
	PRVAL(SLITHRT_USKLNDPL);
	PRVAL(SLMTHRT_BMAPTIMEO);
	PRVAL(SLMTHRT_COH);
	PRVAL(SLMTHRT_CTL);
	PRVAL(SLMTHRT_CTLAC);
	PRVAL(SLMTHRT_CURSOR);
	PRVAL(SLMTHRT_FSSYNC);
	PRVAL(SLMTHRT_JNAMESPACE);
	PRVAL(SLMTHRT_JRNL);
	PRVAL(SLMTHRT_LNETAC);
	PRVAL(SLMTHRT_RCM);
	PRVAL(SLMTHRT_RMC);
	PRVAL(SLMTHRT_RMI);
	PRVAL(SLMTHRT_RMM);
	PRVAL(SLMTHRT_TIOS);
	PRVAL(SLMTHRT_UPSCHED);
	PRVAL(SLMTHRT_USKLNDPL);
	PRVAL(SLNCONNT);
	PRVAL(SLREST_ARCHIVAL_FS);
	PRVAL(SLREST_CLUSTER_NOSHARE_FS);
	PRVAL(SLREST_COMPUTE);
	PRVAL(SLREST_MDS);
	PRVAL(SLREST_NONE);
	PRVAL(SLREST_PARALLEL_FS);
	PRVAL(SLREST_STANDALONE_FS);
	PRVAL(SL_READ);
	PRVAL(SL_WRITE);
	PRVAL(SRMT_BMAPCHWRMODE);
	PRVAL(SRMT_BMAPCRCWRT);
	PRVAL(SRMT_BMAPDIO);
	PRVAL(SRMT_CONNECT);
	PRVAL(SRMT_CREATE);
	PRVAL(SRMT_DESTROY);
	PRVAL(SRMT_GARBAGE);
	PRVAL(SRMT_GETATTR);
	PRVAL(SRMT_GETBMAP);
	PRVAL(SRMT_GETBMAPCRCS);
	PRVAL(SRMT_GETBMAPMINSEQ);
	PRVAL(SRMT_LINK);
	PRVAL(SRMT_LOOKUP);
	PRVAL(SRMT_MKDIR);
	PRVAL(SRMT_MKNOD);
	PRVAL(SRMT_NAMESPACE_UPDATE);
	PRVAL(SRMT_PING);
	PRVAL(SRMT_READ);
	PRVAL(SRMT_READDIR);
	PRVAL(SRMT_READLINK);
	PRVAL(SRMT_RELEASEBMAP);
	PRVAL(SRMT_RENAME);
	PRVAL(SRMT_REPL_ADDRQ);
	PRVAL(SRMT_REPL_DELRQ);
	PRVAL(SRMT_REPL_GETST);
	PRVAL(SRMT_REPL_GETST_SLAVE);
	PRVAL(SRMT_REPL_READ);
	PRVAL(SRMT_REPL_SCHEDWK);
	PRVAL(SRMT_RMDIR);
	PRVAL(SRMT_SETATTR);
	PRVAL(SRMT_SET_BMAPREPLPOL);
	PRVAL(SRMT_SET_NEWREPLPOL);
	PRVAL(SRMT_STATFS);
	PRVAL(SRMT_SYMLINK);
	PRVAL(SRMT_UNLINK);
	PRVAL(SRMT_WRITE);
	PRVAL(USWI_REFT_LOOKUP);
	PRVAL(USWI_REFT_SITEUPQ);
	PRVAL(USWI_REFT_TREE);
	/* end enums */

	PRVAL(INOX_OD_SZ);
	PRVAL(INOX_OD_CRCSZ);

	PRVAL(SL_REPLICA_NBYTES);

	PRVALX(FID_ANY);

	PRVAL(sizeof(((struct sl_resm *)NULL)->resm_addrbuf));

	psc_crc64_calc(&crc, buf, sizeof(buf));
	printf("NULL 1MB buf CRC is %"PSCPRIxCRC64"\n", crc);

	psc_crc64_calc(&crc, &bmapod, sizeof(bmapod));
	printf("NULL sl_blkh_t CRC is %#"PRIx64"\n", crc);

	exit(0);
}
