/* $Id$ */

#include <sys/param.h>

#include <err.h>
#include <stdio.h>
#include <unistd.h>

#include "pfl/cdefs.h"

/* start includes */
#include "bmap.h"
#include "buffer.h"
#include "cache_params.h"
#include "cfd.h"
#include "creds.h"
#include "fdbuf.h"
#include "fid.h"
#include "fidcache.h"
#include "inode.h"
#include "inodeh.h"
#include "jflush.h"
#include "mkfn.h"
#include "pathnames.h"
#include "slashexport.h"
#include "slashrpc.h"
#include "slconfig.h"
#include "slerr.h"
#include "sljournal.h"
#include "sltypes.h"
#include "mount_slash/bmpc.h"
#include "mount_slash/cli_bmap.h"
#include "mount_slash/ctl_cli.h"
#include "mount_slash/ctlsvr_cli.h"
#include "mount_slash/fidc_cli.h"
#include "mount_slash/fuse_listener.h"
#include "mount_slash/mount_slash.h"
#include "mount_slash/msl_fuse.h"
#include "msctl/msctl.h"
#include "slashd/ctl_mds.h"
#include "slashd/fidc_mds.h"
#include "slashd/mds_bmap.h"
#include "slashd/mdscoh.h"
#include "slashd/mdsexpc.h"
#include "slashd/mdsio_zfs.h"
#include "slashd/mdslog.h"
#include "slashd/repl_mds.h"
#include "slashd/rpc_mds.h"
#include "slashd/slashd.h"
#include "sliod/ctl_iod.h"
#include "sliod/fidc_iod.h"
#include "sliod/iod_bmap.h"
#include "sliod/repl_iod.h"
#include "sliod/rpc_iod.h"
#include "sliod/sliod.h"
#include "sliod/slvr.h"
/* end includes */

const char *progname;

void
pr(const char *name, uint64_t value)
{
	static int i;
	int n;

	if (i++ % 2) {
		n = printf("%s ", name);
		while (n++ <= 50)
			putchar('-');
		if (n < 53)
			printf("> ");
		printf("%zu\n", value);
	} else
		printf("%-52s %zu\n", name, value);
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

#define PRTYPE(type)	pr(#type, sizeof(type))
#define PRVAL(val)	pr(#val, (unsigned long)(val))

	/* start structs */
	PRTYPE(cred_t);
	PRTYPE(sl_inum_t);
	PRTYPE(sl_ios_id_t);
	PRTYPE(sl_siteid_t);
	PRTYPE(struct biod_crcup_ref);
	PRTYPE(struct biod_infslvr_tree);
	PRTYPE(struct bmap_cli_info);
	PRTYPE(struct bmap_iod_info);
	PRTYPE(struct bmap_mds_info);
	PRTYPE(struct bmap_pagecache);
	PRTYPE(struct bmap_pagecache_entry);
	PRTYPE(struct bmap_refresh);
	PRTYPE(struct bmapc_memb);
	PRTYPE(struct bmi_assign);
	PRTYPE(struct bmpc_ioreq);
	PRTYPE(struct bmpc_mem_slbs);
	PRTYPE(struct cfdent);
	PRTYPE(struct cfdops);
	PRTYPE(struct cli_imp_ion);
	PRTYPE(struct fidc_iod_info);
	PRTYPE(struct fidc_mds_info);
	PRTYPE(struct fidc_memb);
	PRTYPE(struct fidc_membh);
	PRTYPE(struct fidc_open_obj);
	PRTYPE(struct fidc_private);
	PRTYPE(struct io_server_conn);
	PRTYPE(struct iod_resm_info);
	PRTYPE(struct jflush_item);
	PRTYPE(struct mds_resm_info);
	PRTYPE(struct mds_site_info);
	PRTYPE(struct mexp_cli);
	PRTYPE(struct mexp_ion);
	PRTYPE(struct mexpbcm);
	PRTYPE(struct mexpfcm);
	PRTYPE(struct msbmap_crcrepl_states);
	PRTYPE(struct msctl_replst_cont);
	PRTYPE(struct msctl_replst_slave_cont);
	PRTYPE(struct msctl_replstq);
	PRTYPE(struct msctlmsg_replrq);
	PRTYPE(struct msctlmsg_replst);
	PRTYPE(struct msctlmsg_replst_slave);
	PRTYPE(struct msfs_thread);
	PRTYPE(struct msl_fbr);
	PRTYPE(struct msl_fcoo_data);
	PRTYPE(struct msl_fhent);
	PRTYPE(struct msrcm_thread);
	PRTYPE(struct resprof_cli_info);
	PRTYPE(struct resprof_mds_info);
	PRTYPE(struct sl_buffer);
	PRTYPE(struct sl_buffer_iovref);
	PRTYPE(struct sl_finfo);
	PRTYPE(struct sl_fsops);
	PRTYPE(struct sl_gconf);
	PRTYPE(struct sl_nodeh);
	PRTYPE(struct sl_replrq);
	PRTYPE(struct sl_resm);
	PRTYPE(struct sl_resource);
	PRTYPE(struct sl_site);
	PRTYPE(struct slash_bmap_cli_wire);
	PRTYPE(struct slash_bmap_od);
	PRTYPE(struct slash_creds);
	PRTYPE(struct slash_fidgen);
	PRTYPE(struct slash_inode_extras_od);
	PRTYPE(struct slash_inode_handle);
	PRTYPE(struct slash_inode_od);
	PRTYPE(struct slashrpc_cservice);
	PRTYPE(struct slashrpc_export);
	PRTYPE(struct sli_repl_buf);
	PRTYPE(struct sli_repl_workrq);
	PRTYPE(struct sliric_thread);
	PRTYPE(struct slirii_thread);
	PRTYPE(struct slirim_thread);
	PRTYPE(struct slmds_jent_crc);
	PRTYPE(struct slmds_jent_ino_addrepl);
	PRTYPE(struct slmds_jent_repgen);
	PRTYPE(struct slmds_jents);
	PRTYPE(struct slmiconn_thread);
	PRTYPE(struct slmrcm_thread);
	PRTYPE(struct slmrmc_thread);
	PRTYPE(struct slmrmi_thread);
	PRTYPE(struct slmrmm_thread);
	PRTYPE(struct slmsm_thread);
	PRTYPE(struct slvr_ref);
	PRTYPE(struct srm_access_req);
	PRTYPE(struct srm_bmap_chmode_rep);
	PRTYPE(struct srm_bmap_chmode_req);
	PRTYPE(struct srm_bmap_crcup);
	PRTYPE(struct srm_bmap_crcwire);
	PRTYPE(struct srm_bmap_crcwrt_req);
	PRTYPE(struct srm_bmap_dio_req);
	PRTYPE(struct srm_bmap_iod_get);
	PRTYPE(struct srm_bmap_rep);
	PRTYPE(struct srm_bmap_req);
	PRTYPE(struct srm_bmap_wire_rep);
	PRTYPE(struct srm_bmap_wire_req);
	PRTYPE(struct srm_connect_req);
	PRTYPE(struct srm_create_req);
	PRTYPE(struct srm_destroy_req);
	PRTYPE(struct srm_generic_rep);
	PRTYPE(struct srm_getattr_rep);
	PRTYPE(struct srm_getattr_req);
	PRTYPE(struct srm_io_rep);
	PRTYPE(struct srm_io_req);
	PRTYPE(struct srm_link_rep);
	PRTYPE(struct srm_link_req);
	PRTYPE(struct srm_lookup_rep);
	PRTYPE(struct srm_lookup_req);
	PRTYPE(struct srm_mkdir_rep);
	PRTYPE(struct srm_mkdir_req);
	PRTYPE(struct srm_mknod_req);
	PRTYPE(struct srm_open_rep);
	PRTYPE(struct srm_open_req);
	PRTYPE(struct srm_opendir_req);
	PRTYPE(struct srm_ping_req);
	PRTYPE(struct srm_readdir_rep);
	PRTYPE(struct srm_readdir_req);
	PRTYPE(struct srm_readlink_rep);
	PRTYPE(struct srm_readlink_req);
	PRTYPE(struct srm_release_req);
	PRTYPE(struct srm_releasebmap_req);
	PRTYPE(struct srm_rename_req);
	PRTYPE(struct srm_repl_read_req);
	PRTYPE(struct srm_repl_schedwk_req);
	PRTYPE(struct srm_replrq_req);
	PRTYPE(struct srm_replst_master_req);
	PRTYPE(struct srm_replst_slave_req);
	PRTYPE(struct srm_setattr_req);
	PRTYPE(struct srm_statfs_rep);
	PRTYPE(struct srm_statfs_req);
	PRTYPE(struct srm_symlink_rep);
	PRTYPE(struct srm_symlink_req);
	PRTYPE(struct srm_unlink_req);
	PRTYPE(struct srt_bdb_secret);
	PRTYPE(struct srt_bmapdesc_buf);
	PRTYPE(struct srt_fd_buf);
	PRTYPE(struct srt_fdb_secret);
/* end structs */

	exit(0);
}
