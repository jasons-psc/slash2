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
 * SLASH remote procedure call message (SRM) definitions, for issuing
 * operations on and communicating with other hosts in a SLASH network.
 */

#ifndef _SLASHRPC_H_
#define _SLASHRPC_H_

#include "pfl/cdefs.h"

#include "authbuf.h"
#include "bmap.h"
#include "cache_params.h"
#include "creds.h"
#include "fid.h"
#include "sltypes.h"

struct stat;
struct statvfs;

#define _SL_RSX_NEWREQN(imp, version, op, rq, nq, qlens, np, plens,	\
	    mq0)							\
	{								\
		psc_assert((nq) > 1);					\
		psc_assert((np) > 1);					\
		psc_assert((qlens)[(nq) - 1] == 0);			\
		psc_assert((plens)[(np) - 1] == 0);			\
		(qlens)[(nq) - 1] = sizeof(struct srt_authbuf_footer);	\
		(plens)[(np) - 1] = sizeof(struct srt_authbuf_footer);	\
									\
		RSX_NEWREQN((imp), (version), (op), (rq),		\
		    (nq), (qlens), (np), (plens), (mq0));		\
	}

#define SL_RSX_NEWREQN(imp, version, op, rq, nq, qlens, np, plens, mq0)	\
	(_SL_RSX_NEWREQN((imp), (version), (op), (rq), (nq), (qlens),	\
	    (np), (plens), (mq0)))

#define _SL_RSX_NEWREQ(imp, version, op, rq, mq, mp)			\
	{								\
		int _qlens[2] = { sizeof(*(mq)), 0 };			\
		int _plens[2] = { sizeof(*(mp)), 0 };			\
									\
		SL_RSX_NEWREQN((imp), (version), (op), (rq), 2,		\
		    _qlens, 2, _plens, (mq));				\
	}

#define SL_RSX_NEWREQ(imp, version, op, rq, mq, mp)			\
	(_SL_RSX_NEWREQ((imp), (version), (op), (rq), (mq), (mp)))

#define _SL_RSX_WAITREP(rq, mp)						\
	{								\
		int _rc;						\
									\
		authbuf_sign((rq), PSCRPC_MSG_REQUEST);			\
		_rc = RSX_WAITREP((rq), (mp));				\
		if (_rc == 0)						\
			_rc = authbuf_check((rq), PSCRPC_MSG_REPLY);	\
		_rc;							\
	}

#define SL_RSX_WAITREP(rq, mp)						\
	(_SL_RSX_WAITREP((rq), (mp)))

#define SL_RSX_ALLOCREP(rq, mq, mp)					\
	 do {								\
		int _plens[2] = { sizeof(*(mp)),			\
		    sizeof(struct srt_authbuf_footer) };		\
									\
		RSX_ALLOCREPN((rq), (mq), (mp), 2, _plens);		\
		(mp)->rc = authbuf_check((rq), PSCRPC_MSG_REQUEST);	\
		if ((mp)->rc)						\
			return ((mp)->rc);				\
	 } while (0)

/* Slash RPC channel to MDS from client. */
#define SRMC_REQ_PORTAL		10
#define SRMC_REP_PORTAL		11
#define SRMC_BULK_PORTAL	12
#define SRMC_CTL_PORTAL		13

#define SRMC_VERSION		1
#define SRMC_MAGIC		UINT64_C(0xaabbccddeeff0022)

/* Slash RPC channel to MDS from MDS. */
#define SRMM_REQ_PORTAL		15
#define SRMM_REP_PORTAL		16
#define SRMM_BULK_PORTAL	17
#define SRMM_CTL_PORTAL		18

#define SRMM_VERSION		1
#define SRMM_MAGIC		UINT64_C(0xaabbccddeeff0033)

/* Slash RPC channel to MDS from ION. */
#define SRMI_REQ_PORTAL		20
#define SRMI_REP_PORTAL		21
#define SRMI_BULK_PORTAL	22
#define SRMI_CTL_PORTAL		23

#define SRMI_VERSION		1
#define SRMI_MAGIC		UINT64_C(0xaabbccddeeff0044)

/* Slash RPC channel to client from MDS. */
#define SRCM_REQ_PORTAL		25
#define SRCM_REP_PORTAL		26
#define SRCM_BULK_PORTAL	27
#define SRCM_CTL_PORTAL		28

#define SRCM_VERSION		1
#define SRCM_MAGIC		UINT64_C(0xaabbccddeeff0055)

/* Slash RPC channel to ION from client. */
#define SRIC_REQ_PORTAL		30
#define SRIC_REP_PORTAL		31
#define SRIC_BULK_PORTAL	32
#define SRIC_CTL_PORTAL		33

#define SRIC_VERSION		1
#define SRIC_MAGIC		UINT64_C(0xaabbccddeeff0066)

/* Slash RPC channel to ION from ION. */
#define SRII_REQ_PORTAL		35
#define SRII_REP_PORTAL		36
#define SRII_BULK_PORTAL	37
#define SRII_CTL_PORTAL		38

#define SRII_VERSION		1
#define SRII_MAGIC		UINT64_C(0xaabbccddeeff0077)

/* Slash RPC channel to ION from MDS. */
#define SRIM_REQ_PORTAL		40
#define SRIM_REP_PORTAL		41
#define SRIM_BULK_PORTAL	42
#define SRIM_CTL_PORTAL		43

#define SRIM_VERSION		1
#define SRIM_MAGIC		UINT64_C(0xaabbccddeeff0088)

/* Slash RPC message types. */
enum {
	/* control operations */
	SRMT_CONNECT = 1,
	SRMT_DESTROY,
	SRMT_PING,

	/* namespace operations */
	SRMT_NAMESPACE_UPDATE,		/* send a batch of namespace operation logs */

	/* bmap operations */
	SRMT_BMAPCHWRMODE,
	SRMT_BMAPCRCWRT,
	SRMT_BMAPDIO,
	SRMT_GETBMAP,
	SRMT_GETBMAPMINSEQ,
	SRMT_GETBMAPCRCS,
	SRMT_RELEASEBMAP,

	/* garbage operations */
	SRMT_GARBAGE,

	/* replication operations */
	SRMT_REPL_ADDRQ,
	SRMT_REPL_DELRQ,
	SRMT_REPL_GETST,
	SRMT_REPL_GETST_SLAVE,
	SRMT_REPL_READ,
	SRMT_REPL_SCHEDWK,
	SRMT_SET_BMAPREPLPOL,
	SRMT_SET_NEWREPLPOL,

	/* file system operations */
	SRMT_CHMOD,
	SRMT_CHOWN,
	SRMT_CREATE,
	SRMT_FGETATTR,
	SRMT_FTRUNCATE,
	SRMT_GETATTR,
	SRMT_LINK,
	SRMT_LOCK,
	SRMT_LOOKUP,
	SRMT_MKDIR,
	SRMT_MKNOD,
	SRMT_READ,
	SRMT_READDIR,
	SRMT_READLINK,
	SRMT_RENAME,
	SRMT_RMDIR,
	SRMT_SETATTR,
	SRMT_STATFS,
	SRMT_SYMLINK,
	SRMT_TRUNCATE,
	SRMT_UNLINK,
	SRMT_UTIMES,
	SRMT_WRITE
};

/* ----------------------------- BEGIN MESSAGES ----------------------------- */

/*
 * Note: Member ordering within structures must always follow 64-bit boundaries
 * to preserve compatibility between 32-bit and 64-bit machines.
 */

struct srm_generic_rep {
	uint64_t		data;		/* context overloadable data */
	int32_t			rc;		/* return code, 0 for success or slerrno */
	int32_t			_pad;
} __packed;

/* ---------------------- BEGIN ENCAPSULATED MESSAGES ----------------------- */

/*
 * Note: these messages are contained within other messages and thus must
 * end on 64-bit boundaries.
 */

#define AUTHBUF_REPRLEN		45		/* strlen(base64(SHA256(secret)) + NUL */
#define AUTHBUF_MAGIC		UINT64_C(0x4321432143214321)

struct srt_authbuf_secret {
	uint64_t		sas_magic;
	uint64_t		sas_nonce;
	uint64_t		sas_dst_nid;
	uint64_t		sas_src_nid;
	uint32_t		sas_dst_pid;
	uint32_t		sas_src_pid;
} __packed;

/* this is appended after every RPC message */
struct srt_authbuf_footer {
	struct srt_authbuf_secret saf_secret;
	char			saf_hash[AUTHBUF_REPRLEN];
	char			saf__pad[3];
} __packed;

struct srt_bmapdesc {
	struct slash_fidgen	sbd_fg;
	uint64_t		sbd_seq;
	uint64_t		sbd_key;
	uint64_t		sbd_ion_nid;	/* owning I/O node if write */
	sl_ios_id_t		sbd_ios_id;
	sl_bmapno_t		sbd_bmapno;
	uint32_t		sbd_flags;
	uint32_t		sbd__pad;
} __packed;

/* Slash RPC transportably safe structures. */
struct srt_stat {
	struct slash_fidgen	sst_fg;		/* file ID + truncate generation */
	uint64_t		sst_dev;	/* ID of device containing file */
	uint32_t		sst_ptruncgen;	/* partial truncate generation */
	uint32_t                sst_utimgen;    /* utimes generation number */
	uint32_t                sst__pad0;
	uint32_t		sst_mode;	/* file type & permissions (e.g., S_IFREG, S_IRWXU) */
	uint64_t		sst_nlink;	/* number of hard links */
	uint32_t		sst_uid;	/* user ID of owner */
	uint32_t		sst_gid;	/* group ID of owner */
	uint64_t		sst_rdev;	/* device ID (if special file) */
	uint64_t		sst_size;	/* total size, in bytes */
	uint64_t		sst_blksize;	/* blocksize for file system I/O */
	uint64_t		sst_blocks;	/* number of 512B blocks allocated */
	struct sl_timespec	sst_atim;	/* time of last access */
	struct sl_timespec	sst_mtim;	/* time of last modification */
	struct sl_timespec	sst_ctim;	/* time of creation */
#define sst_fid		sst_fg.fg_fid
#define sst_gen		sst_fg.fg_gen
#define sst_atime	sst_atim.tv_sec
#define sst_atime_ns	sst_atim.tv_nsec
#define sst_mtime	sst_mtim.tv_sec
#define sst_mtime_ns	sst_mtim.tv_nsec
#define sst_ctime	sst_ctim.tv_sec
#define sst_ctime_ns	sst_ctim.tv_nsec
} __packed;

#define DEBUG_SSTB(level, sstb, fmt, ...)					\
	psc_log((level), "sstb (%p) dev:%"PRIu64" mode:%#o nlink:%"PRIu64" "	\
	    "uid:%u gid:%u rdev:%"PRIu64" "					\
	    "sz:%"PRIu64" blksz:%"PRIu64" blkcnt:%"PRIu64" "			\
	    "atime:%"PRIu64" mtime:%"PRIu64" ctime:%"PRIu64" " fmt,		\
	    (sstb), (sstb)->sst_dev, (sstb)->sst_mode, (sstb)->sst_nlink,	\
	    (sstb)->sst_uid, (sstb)->sst_gid, (sstb)->sst_rdev,			\
	    (sstb)->sst_size, (sstb)->sst_blksize, (sstb)->sst_blocks,		\
	    (sstb)->sst_atime, (sstb)->sst_mtime, (sstb)->sst_ctime, ## __VA_ARGS__)

struct srt_statfs {
	uint64_t		sf_bsize;	/* file system block size */
	uint64_t		sf_frsize;	/* fragment size */
	uint64_t		sf_blocks;	/* size of fs in f_frsize units */
	uint64_t		sf_bfree;	/* # free blocks */
	uint64_t		sf_bavail;	/* # free blocks for non-root */
	uint64_t		sf_files;	/* # inodes */
	uint64_t		sf_ffree;	/* # free inodes */
	uint64_t		sf_favail;	/* # free inodes for non-root */
	uint64_t		sf_fsid;	/* file system ID */
	uint64_t		sf_flag;	/* mount flags */
	uint64_t		sf_namemax;	/* maximum filename length */
} __packed;

/* ------------------------ BEGIN NAMESPACE MESSAGES ------------------------ */

struct srm_send_namespace_req {
	uint64_t		seqno;
	uint64_t		crc;		/* CRC of the bulk data */
	int32_t			size;		/* size of the bulk data to follow */
	int16_t			count;		/* # of entries to follow */
	int16_t			siteid;		/* Site ID for tracking purpose */
} __packed;

struct srm_send_namespace_rep {
	int32_t			rc;
	int32_t			_pad;
} __packed;

/* -------------------------- BEGIN BMAP MESSAGES --------------------------- */

struct srm_leasebmap_req {
	struct slash_fidgen	fg;
	sl_ios_id_t		prefios;	/* client's preferred IOS ID */
	sl_bmapno_t		bmapno;		/* Starting bmap index number */
	int32_t			rw;		/* 'enum rw' value for access */
	uint32_t		flags;		/* see SRM_LEASEBMAPF_* below */
} __packed;

#define SRM_LEASEBMAPF_DIRECTIO	  (1 << 0)	/* client wants direct I/O */
#define SRM_LEASEBMAPF_GETREPLTBL (1 << 1)	/* fetch inode replica table */

struct srm_leasebmap_rep {
	struct srt_bmapdesc	sbd;		/* descriptor for bmap */
	struct bmap_core_state	bcs;
	int32_t			rc;		/* 0 for success or slerrno */
	int32_t			_pad;
	uint32_t		flags;		/* return SRM_LEASEBMAPF_* success */

	/* fetch fcmh repl table if SRM_LEASEBMAPF_GETREPLTBL */
	uint32_t		nrepls;
	sl_replica_t		reptbl[SL_MAX_REPLICAS];

} __packed;

/*
 * ION requesting CRC table from the MDS.
 */
struct srm_getbmap_full_req {
	struct slash_fidgen	fg;
	sl_bmapno_t		bmapno;
	int32_t			rw;
} __packed;

struct srm_getbmap_full_rep {
	struct bmap_ondisk	bod;
	uint64_t		minseq;
	int32_t			rc;
	int32_t			_pad;
} __packed;

struct srm_bmap_chwrmode_req {
	struct srt_bmapdesc	sbd;
	sl_ios_id_t		prefios;	/* preferred I/O system ID (if WRITE) */
	int32_t			_pad;
} __packed;

struct srm_bmap_chwrmode_rep {
	struct srt_bmapdesc	sbd;
	int32_t			rc;
	int32_t			_pad;
} __packed;

struct srm_bmap_dio_req {
	uint64_t		fid;
	uint64_t		seq;
	uint32_t		blkno;
	uint32_t		dio;
	uint32_t		mode;
	int32_t			_pad;
} __packed;

struct srm_bmap_crcwire {
	uint64_t		crc;		/* CRC of the corresponding sliver */
	uint32_t		slot;		/* sliver number in the owning bmap */
	int32_t			_pad;
} __packed;

#define MAX_BMAP_INODE_PAIRS	24		/* ~520 bytes (max) per srm_bmap_crcup */

struct srm_bmap_crcup {
	struct slash_fidgen	fg;
	uint64_t		fsize;		/* largest known size applied in mds_bmap_crc_update() */
	uint32_t		blkno;		/* bmap block number */
	uint32_t		nups;		/* number of CRC updates */
	uint32_t                utimgen;
	uint32_t                _pad;
	struct srm_bmap_crcwire	crcs[0];	/* see above, MAX_BMAP_INODE_PAIRS max */
} __packed;

#define MAX_BMAP_NCRC_UPDATES	64		/* max number of CRC updates in a batch */

struct srm_bmap_crcwrt_req {
	uint64_t		crc;		/* yes, a CRC of the CRC's */
	uint8_t			ncrcs_per_update[MAX_BMAP_NCRC_UPDATES];
	uint32_t		ncrc_updates;
	int32_t			_pad;
} __packed;

struct srm_bmap_crcwrt_rep {
	uint64_t		seq;
	int32_t			crcup_rc[MAX_BMAP_NCRC_UPDATES];
	int32_t			rc;
} __packed;

struct srm_bmap_iod_get {
	uint64_t		fid;
	uint32_t		blkno;
	int32_t			_pad;
} __packed;

struct srm_bmap_id {
	slfid_t			fid;
	uint64_t		key;
	uint64_t		seq;
	uint64_t		cli_nid;
	uint32_t		cli_pid;
	sl_bmapno_t		bmapno;
} __packed;

#define MAX_BMAP_RELEASE 8
struct srm_bmap_release_req {
	struct srm_bmap_id	bmaps[MAX_BMAP_RELEASE];
	uint32_t		nbmaps;
	int32_t			_pad;
} __packed;

struct srm_bmap_release_rep {
	int32_t			bidrc[MAX_BMAP_RELEASE];
	int32_t			rc;
	int32_t			_pad;
} __packed;

struct srm_bmap_minseq_get {
	int32_t			data;
	int32_t			_pad;
} __packed;

/* ------------------------- BEGIN GARBAGE MESSAGES ------------------------- */

struct srm_garbage_req {
	struct slash_fidgen	fg;
	sl_bmapno_t		bmapno;
	sl_bmapgen_t		bgen;
} __packed;

/* ------------------------- BEGIN CONTROL MESSAGES ------------------------- */

struct srm_connect_req {
	uint64_t		magic;
	uint32_t		version;
	int32_t			_pad;
} __packed;

struct srm_ping_req {
	int64_t			data;		/* context overloadable data */
} __packed;

/* ----------------------- BEGIN REPLICATION MESSAGES ----------------------- */

/* for a GETSTATUS about a replication request */
struct srm_replst_master_req {
	struct slash_fidgen	fg;
	sl_replica_t		repls[SL_MAX_REPLICAS];
	int32_t			id;		/* user-provided passback value */
	int32_t			rc;		/* or EOF */
	uint32_t		nbmaps;		/* # of bmaps in file */
	uint32_t		newreplpol;	/* default replication policy */
	uint32_t		nrepls;		/* # of I/O systems in 'repls' */
	int32_t			_pad;
	unsigned char		data[56];	/* slave data here if it fits */
} __packed;

#define srm_replst_master_rep srm_replst_master_req

/*
 * bmap data carrier for a replrq GETSTATUS when data is larger than can
 * fit in srm_replst_master_req.data
 */
struct srm_replst_slave_req {
	struct slash_fidgen	fg;
	int32_t			id;		/* user-provided passback value */
	int32_t			len;		/* of bulk data */
	int32_t			rc;
	uint32_t		nbmaps;		/* # of bmaps in this chunk */
	sl_bmapno_t		boff;		/* offset into inode of first bmap in bulk */
	int32_t			_pad;
/* bulk data is sections of bcs_repls data */
} __packed;

/* per-bmap header submessage, prepended before each bcs_repls content */
struct srsm_replst_bhdr {
	uint8_t			srsb_repl_policy;
} __packed;

#define SL_NBITS_REPLST_BHDR	(8)

#define SRM_REPLST_PAGESIZ	(1024 * 1024)	/* should be network MSS */

#define srm_replst_slave_rep srm_replst_slave_req

struct srm_repl_schedwk_req {
	uint64_t		nid;		/* XXX gross */
	struct slash_fidgen	fg;
	sl_bmapno_t		bmapno;
	sl_bmapgen_t		bgen;
	uint32_t		len;
	int32_t			rc;
} __packed;

struct srm_repl_read_req {
	struct slash_fidgen	fg;
	uint64_t		len;		/* #bytes in this message, to find #slivers */
	sl_bmapno_t		bmapno;
	int32_t			slvrno;
} __packed;

#define srm_repl_read_rep srm_io_rep

struct srm_set_newreplpol_req {
	struct slash_fidgen	fg;
	int32_t			pol;
	int32_t			_pad;
} __packed;

struct srm_set_bmapreplpol_req {
	struct slash_fidgen	fg;
	sl_bmapno_t		bmapno;
	int32_t			pol;
} __packed;

/* ----------------------- BEGIN FILE SYSTEM MESSAGES ----------------------- */

struct srm_create_req {
	struct slash_fidgen	pfg;		/* parent dir's file ID + generation */
	struct slash_creds	creds;		/* st_uid owner for new file */
	char			name[NAME_MAX + 1];
	uint32_t		mode;		/* mode_t permission for new file */

	/* parameters for fetching first bmap */
	sl_ios_id_t		prefios;	/* preferred I/O system ID */
	uint32_t		flags;		/* see SRM_BMAPF_* flags */
	int32_t			_pad;
} __packed;

struct srm_create_rep {
	struct srt_stat		attr;		/* stat(2) buffer of new file attrs */
	int32_t			rc;		/* 0 for success or slerrno */
	int32_t			_pad;

	/* parameters for fetching first bmap */
	uint32_t		rc2;		/* (for LEASEBMAP) 0 or slerrno */
	uint32_t		flags;		/* see SRM_BMAPF_* flags */
	struct srt_bmapdesc	sbd;
} __packed;

struct srm_destroy_req {
} __packed;

struct srm_getattr_req {
	struct slash_fidgen	fg;
} __packed;

struct srm_getattr_rep {
	struct srt_stat		attr;
	int32_t			rc;
	int32_t			_pad;
} __packed;

struct srm_io_req {
	struct srt_bmapdesc	sbd;
	uint32_t		ptruncgen;
	uint32_t                utimgen;
	uint32_t		flags:31;
	uint32_t		op:1;		/* read/write */
	uint32_t		size;
	uint32_t		offset;
	uint32_t                _pad;
/* WRITE data is bulk request. */
} __packed;

/* I/O operations */
#define SRMIOP_RD		0
#define SRMIOP_WR		1

/* I/O flags */
#define SRM_IOF_APPEND		(1 << 0)
#define SRM_IOF_DIO             (1 << 1)

struct srm_io_rep {
	int32_t			rc;
	uint32_t		size;
/* READ data is in bulk reply. */
} __packed;

struct srm_link_req {
	struct slash_creds	creds;		/* st_uid owner for new file */
	struct slash_fidgen	pfg;		/* parent dir */
	struct slash_fidgen	fg;
	char			name[NAME_MAX + 1];
} __packed;

struct srm_link_rep {
	struct srt_stat		attr;
	int32_t			rc;
	int32_t			_pad;
} __packed;

struct srm_lookup_req {
	struct slash_fidgen	pfg;		/* parent dir */
	char			name[NAME_MAX + 1];
} __packed;

struct srm_lookup_rep {
	struct srt_stat		attr;
	int32_t			rc;
	int32_t			_pad;
} __packed;

struct srm_mkdir_req {
	struct slash_creds	creds;		/* st_uid owner for new file */
	struct slash_fidgen	pfg;		/* parent dir */
	char			name[NAME_MAX + 1];
	uint32_t		mode;
	int32_t			_pad;
} __packed;

struct srm_mkdir_rep {
	struct srt_stat		attr;
	int32_t			rc;
	int32_t			_pad;
} __packed;

struct srm_mknod_req {
	struct slash_creds	creds;		/* st_uid owner for new file */
	char			name[NAME_MAX + 1];
	struct slash_fidgen	pfg;		/* parent dir */
	uint32_t		mode;
	uint32_t		rdev;
} __packed;

#define DEF_READDIR_NENTS	100
#define MAX_READDIR_NENTS	1000
#define MAX_READDIR_BUFSIZ	(sizeof(struct srt_stat) * MAX_READDIR_NENTS)

struct srm_readdir_req {
	struct slash_fidgen	fg;
	uint64_t		offset;
	uint64_t		size;
	uint32_t		nstbpref;	/* max prefetched attributes */
	int32_t			_pad;
} __packed;

struct srm_readdir_rep {
	uint64_t		size;
	uint32_t		num;		/* #dirents returned */
	int32_t			_pad;
	int32_t			rc;
/*
 * XXX accompanied by bulk data is (but should not be) in fuse dirent format
 *	and must be 64-bit aligned.
 */
} __packed;

struct srm_readlink_req {
	struct slash_fidgen	fg;
} __packed;

struct srm_readlink_rep {
	int32_t			rc;
	int32_t			_pad;
/* buf is in bulk of size PATH_MAX */
} __packed;

struct srm_rename_req {
	struct slash_fidgen	npfg;		/* new parent dir */
	struct slash_fidgen	opfg;		/* old parent dir */
	uint32_t		fromlen;
	uint32_t		tolen;
/* 'from' and 'to' component names are in bulk data without terminating NULs */
} __packed;

struct srm_replrq_req {
	struct slash_fidgen	fg;
	sl_replica_t		repls[SL_MAX_REPLICAS];
	uint32_t		nrepls;
	sl_bmapno_t		bmapno;		/* bmap to access or -1 for all */
} __packed;

struct srm_setattr_req {
	struct srt_stat		attr;
	int32_t			to_set;		/* see SETATTR_MASKF_* */
	int32_t			_pad;
} __packed;

#define srm_setattr_rep srm_getattr_rep

struct srm_statfs_req {
} __packed;

struct srm_statfs_rep {
	struct srt_statfs	ssfb;
	int32_t			rc;
	int32_t			_pad;
} __packed;

struct srm_symlink_req {
	struct slash_creds	creds;		/* st_uid owner for new file */
	struct slash_fidgen	pfg;		/* parent dir */
	char			name[NAME_MAX + 1];
	uint32_t		linklen;
	int32_t			_pad;
/* link path name is in bulk */
} __packed;

struct srm_symlink_rep {
	struct srt_stat		attr;
	int32_t			rc;
	int32_t			_pad;
} __packed;

struct srm_unlink_req {
	struct slash_fidgen	pfg;		/* parent dir */
	char			name[NAME_MAX + 1];
} __packed;

#define srm_unlink_rep srm_generic_rep

#endif /* _SLASHRPC_H_ */
