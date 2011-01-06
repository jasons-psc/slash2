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

#ifndef _SL_JOURNAL_H_
#define _SL_JOURNAL_H_

#include "inode.h"
#include "slashrpc.h"

#define SLJ_MDS_JNENTS		(128 * 1024)	/* 131072 */
#define SLJ_MDS_RA		1024		/* SLJ_MDS_JNENTS % SLJ_MDS_RA == 0 */
#define SLJ_MDS_NCRCS		MAX_BMAP_INODE_PAIRS

#define SLJ_MDS_PJET_VOID	0
#define SLJ_MDS_PJET_INUM	1
#define SLJ_MDS_PJET_BMAP	2
#define SLJ_MDS_PJET_INODE	3

/**
 * slmds_jent_crc - Used to log CRC updates which come from IONs.
 * @sjc_fid: file ID.
 * @sjc_bmapno: which bmap region.
 * @sjc_ion: the ion who sent the request.
 * @sjc_crc: array of slots and crcs.
 * Notes: this is presumed to be the most common entry in the journal.
 */
struct slmds_jent_crc {
	/*
	 * We can't use ZFS ID here because the create operation may not
	 * make it to the disk.  When we redo the creation, we will get
	 * a different ZFS ID.
	 */
	uint64_t		sjc_fid;
	sl_bmapno_t		sjc_bmapno;
	sl_ios_id_t		sjc_ion;		/* Track the ION which did the I/O */
	int32_t			sjc_ncrcs;
	uint32_t		sjc_utimgen;
	uint64_t		sjc_fsize;
	struct srm_bmap_crcwire	sjc_crc[SLJ_MDS_NCRCS];
} __packed;

#define slion_jent_crc slmds_jent_crc

/**
 * slmds_jent_repgen - Log changes to the replication state of a bmap
 *	which occur upon processing a new write for a replicated bmap.
 * @sjp_fid: what file.
 * @sjp_bmapno: which bmap region.
 * @sjp_bgen: the new bmap generation.
 * @sjp_ino: the slash2 inode
 * @sjp_inox: the slash2 inode extras
 * @sjp_reptbl: the bmap's entire replica bitmap.
 */
struct slmds_jent_repgen {
	slfid_t				sjp_fid;
	sl_bmapno_t			sjp_bmapno;
	sl_bmapgen_t			sjp_bgen;
	struct slash_inode_od		sjp_ino;
	struct slash_inode_extras_od	sjp_inox;
	uint8_t				sjp_reptbl[SL_REPLICA_NBYTES];
} __packed;

/**
 * slmds_jent_ino_addrepl - Add a new replica IOS to the inode or the
 *	inode extras.
 * @sjir_fid: what file.
 * @sjir_ios: the IOS being added.
 * @sjir_pos: the slot or position the replica IOS is to be added to.
 * @sjir_nrepls: the number of replicas after this update.
 */
struct slmds_jent_ino_addrepl {
	slfid_t			sjir_fid;
	sl_ios_id_t		sjir_ios;
	uint32_t		sjir_pos;
	uint32_t		sjir_nrepls;
} __packed;


struct slmds_jent_bmapseq {
	uint64_t		sjbsq_high_wm;
	uint64_t		sjbsq_low_wm;
} __packed;

#define SJ_NAMESPACE_MAGIC	UINT64_C(0xaa5a5aaa43211234)

#define	SLJ_NAMES_MAX		358

#define SJ_NAMESPACE_RECLAIM	0x01

/*
 * For easy seek within a system log file, each entry has a fixed length
 * of 512 bytes (going 1024 allows us to support longer names to make some
 * POSIX tests happy).  But when we send log entries over the network, we
 * condense them (especially the names) to save network bandwidth.
 */
struct slmds_jent_namespace {
	uint64_t		sjnm_magic;			/* debugging */
	uint32_t		sjnm_op;			/* operation type (i.e., enum namespace_operation) */
	int16_t			sjnm_reclen;
	int16_t			sjnm_flag;			/* need garbage collection */

	uint64_t		sjnm_parent_fid;		/* parent dir FID */
	uint64_t		sjnm_target_fid;

	uint64_t		sjnm_target_gen;		/* reclaim only */
	uint64_t		sjnm_new_parent_fid;		/* rename only  */

	uint32_t		sjnm_mask;			/* attribute mask */

	uint32_t		sjnm_mode;			/* file permission */
	int32_t			sjnm_uid;			/* user ID of owner */
	int32_t			sjnm_gid;			/* group ID of owner */
	uint64_t		sjnm_atime;			/* time of last access */
	uint64_t		sjnm_atime_ns;
	uint64_t		sjnm_mtime;			/* time of last modification */
	uint64_t		sjnm_mtime_ns;
	uint64_t		sjnm_ctime;			/* time of last status change */
	uint64_t		sjnm_ctime_ns;

	uint64_t		sjnm_size;			/* total size, in bytes */

	char			sjnm_name[SLJ_NAMES_MAX + 2];	/* one or two names */

} __packed;

/*
 * List all of the journaling structures here so that the maximum
 *  size can be obtained.
 */
struct slmds_jents {
	union {
		struct slmds_jent_repgen	sjr;
		struct slmds_jent_crc		sjc;
		struct slmds_jent_ino_addrepl	sjia;
		struct slmds_jent_bmapseq	sjsq;
		struct slmds_jent_namespace	sjnm;
	} slmds_jent_types;
};

/*
 * The combined size of the standard header of each log entry (i.e.
 * struct psc_journal_enthdr) and its data, if any, must occupy less
 * than this size.
 */
#define	SLJ_MDS_ENTSIZE		512

#endif /* _SL_JOURNAL_H_ */
