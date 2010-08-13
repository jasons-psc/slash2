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

#ifndef _SLASHD_MDSIO_H_
#define _SLASHD_MDSIO_H_

#include <sys/types.h>

#include <stdint.h>

#include "fid.h"
#include "sltypes.h"

struct statvfs;

struct bmapc_memb;
struct fidc_membh;
struct slash_creds;
struct slash_inode_handle;
struct srt_stat;

typedef uint64_t mdsio_fid_t;

/* callback to get a SLASH2 ID */
typedef slfid_t (*sl_getslfid_cb_t)(void);

/* callback to log writes to bmap */
typedef void (*sl_log_write_t)(void *, uint64_t);

/* callback to log updates to namespace */
typedef void (*sl_log_update_t)(int, uint64_t, uint64_t, uint64_t,
    const struct srt_stat *, int, const char *, const char *);

void	mds_namespace_log(int, uint64_t, uint64_t, uint64_t,
	    const struct srt_stat *, int, const char *, const char *);

/* predefined mdsio layer "fids" */
#define MDSIO_FID_ROOT	3

/* opencreatef() flags */
#define MDSIO_OPENCRF_NOLINK	(1 << 0)	/* do not create links in FID namespace */

#define mdsio_opencreate(pino, crp, fflags, mode, fn, mfp, sstb,	\
	    mdsio_datap, logfunc, getslfid)				\
	mdsio_opencreatef((pino), (crp), (fflags), 0, (mode), (fn),	\
	    (mfp), (sstb), (mdsio_datap), (logfunc), (getslfid))

/* high-level interface */
int	mdsio_apply_fcmh_size(struct fidc_membh *, size_t);
int	mdsio_bmap_read(struct bmapc_memb *);
int	mdsio_bmap_write(struct bmapc_memb *);
int	mdsio_inode_extras_read(struct slash_inode_handle *);
int	mdsio_inode_extras_write(struct slash_inode_handle *);
int	mdsio_inode_read(struct slash_inode_handle *);
int	mdsio_inode_write(struct slash_inode_handle *);

struct mdsio_ops {
	/* control interface */
	int	(*mio_init)(void);
	void	(*mio_exit)(void);

	/* low-level file system interface */
	int	(*mio_access)(mdsio_fid_t, int, const struct slash_creds *);
	int	(*mio_getattr)(mdsio_fid_t, const struct slash_creds *, struct srt_stat *);
	int	(*mio_link)(mdsio_fid_t, mdsio_fid_t, const char *, const struct slash_creds *, struct srt_stat *, sl_log_update_t);
	int	(*mio_lookup)(mdsio_fid_t, const char *, mdsio_fid_t *, const struct slash_creds *, struct srt_stat *);
	int	(*mio_lookup_slfid)(slfid_t, const struct slash_creds *, struct srt_stat *, mdsio_fid_t *);
	int	(*mio_mkdir)(mdsio_fid_t, const char *, mode_t, const struct slash_creds *, struct srt_stat *, mdsio_fid_t *, sl_log_update_t, sl_getslfid_cb_t);
	int	(*mio_opencreatef)(mdsio_fid_t, const struct slash_creds *, int, int, mode_t, const char *, mdsio_fid_t *, struct srt_stat *, void *, sl_log_update_t, sl_getslfid_cb_t);
	int	(*mio_opendir)(mdsio_fid_t, const struct slash_creds *, struct slash_fidgen *, void *);
	int	(*mio_read)(const struct slash_creds *, void *, size_t, size_t *, off_t, void *);
	int	(*mio_readdir)(const struct slash_creds *, size_t, off_t, void *, size_t *, size_t *, void *, int, void *);
	int	(*mio_readlink)(mdsio_fid_t, char *, const struct slash_creds *);
	int	(*mio_release)(const struct slash_creds *, void *);
	int	(*mio_rename)(mdsio_fid_t, const char *, mdsio_fid_t, const char *, const struct slash_creds *, sl_log_update_t);
	int	(*mio_rmdir)(mdsio_fid_t, const char *, const struct slash_creds *, sl_log_update_t);
	int	(*mio_setattr)(mdsio_fid_t, const struct srt_stat *, int, const struct slash_creds *, struct srt_stat *, void *, sl_log_update_t);
	int	(*mio_statfs)(struct statvfs *);
	int	(*mio_symlink)(const char *, mdsio_fid_t, const char *, const struct slash_creds *, struct srt_stat *, mdsio_fid_t *, sl_getslfid_cb_t, sl_log_update_t);
	int	(*mio_unlink)(mdsio_fid_t, const char *, const struct slash_creds *, sl_log_update_t);
	int	(*mio_write)(const struct slash_creds *, const void *, size_t, size_t *, off_t, int, void *, sl_log_write_t, void *);

	/* replay interface */
	int	(*mio_redo_create)(slfid_t, slfid_t, struct srt_stat *, char *);
	int	(*mio_redo_link)(slfid_t, slfid_t, char *);
	int	(*mio_redo_mkdir)(slfid_t, slfid_t, struct srt_stat *, char *);
	int	(*mio_redo_rename)(slfid_t, const char *, slfid_t, const char *);
	int	(*mio_redo_rmdir)(slfid_t, slfid_t, char *);
	int	(*mio_redo_setattr)(slfid_t, struct srt_stat *, uint);
	int	(*mio_redo_symlink)(slfid_t, slfid_t, struct srt_stat *, char *, char *);
	int	(*mio_redo_unlink)(slfid_t, slfid_t, char *);
};

#define mdsio_init		mdsio_ops.mio_init			/* zfs_init() */
#define mdsio_exit		mdsio_ops.mio_exit			/* zfs_exit() */

#define mdsio_access		mdsio_ops.mio_access			/* zfsslash2_access() */
#define mdsio_getattr		mdsio_ops.mio_getattr			/* zfsslash2_getattr() */
#define mdsio_link		mdsio_ops.mio_link			/* zfsslash2_link() */
#define mdsio_lookup		mdsio_ops.mio_lookup			/* zfsslash2_lookup() */
#define mdsio_lookup_slfid	mdsio_ops.mio_lookup_slfid
#define mdsio_mkdir		mdsio_ops.mio_mkdir			/* zfsslash2_mkdir() */
#define mdsio_opencreatef	mdsio_ops.mio_opencreatef		/* zfsslash2_opencreate() */
#define mdsio_opendir		mdsio_ops.mio_opendir
#define mdsio_read		mdsio_ops.mio_read
#define mdsio_readdir		mdsio_ops.mio_readdir			/* zfsslash2_readdir() */
#define mdsio_readlink		mdsio_ops.mio_readlink
#define mdsio_release		mdsio_ops.mio_release
#define mdsio_rename		mdsio_ops.mio_rename
#define mdsio_rmdir		mdsio_ops.mio_rmdir
#define mdsio_setattr		mdsio_ops.mio_setattr
#define mdsio_statfs		mdsio_ops.mio_statfs
#define mdsio_symlink		mdsio_ops.mio_symlink
#define mdsio_unlink		mdsio_ops.mio_unlink
#define mdsio_write		mdsio_ops.mio_write			/* zfsslash2_write() */

#define mdsio_redo_create	mdsio_ops.mio_redo_create		/* zfsslash2_replay_create() */
#define mdsio_redo_link		mdsio_ops.mio_redo_link
#define mdsio_redo_mkdir	mdsio_ops.mio_redo_mkdir
#define mdsio_redo_rename	mdsio_ops.mio_redo_rename
#define mdsio_redo_rmdir	mdsio_ops.mio_redo_rmdir
#define mdsio_redo_setattr	mdsio_ops.mio_redo_setattr
#define mdsio_redo_symlink	mdsio_ops.mio_redo_symlink		/* zfsslash2_replay_symlink() */
#define mdsio_redo_unlink	mdsio_ops.mio_redo_unlink		/* zfsslash2_replay_unlink() */

/* misc API */
uint64_t mdsio_last_synced_txg(void);
int	 mdsio_write_cursor(void *, size_t, void *, sl_log_write_t);

extern struct mdsio_ops mdsio_ops;

#endif
