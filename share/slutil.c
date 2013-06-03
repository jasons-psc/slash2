/* $Id$ */
/*
 * %PSCGPL_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2009-2013, Pittsburgh Supercomputing Center (PSC).
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
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/mount.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "pfl/fcntl.h"
#include "pfl/fs.h"
#include "pfl/stat.h"
#include "pfl/str.h"
#include "pfl/types.h"
#include "psc_util/log.h"

#include "creds.h"
#include "fid.h"
#include "pathnames.h"
#include "slashrpc.h"
#include "slconfig.h"
#include "sltypes.h"
#include "slutil.h"

const char *sl_datadir = SL_PATH_DATA_DIR;
const int   sl_stkvers = SL_STK_VERSION;

enum rw
fflags_2_rw(int fflags)
{
	if (fflags & (O_WRONLY | O_RDWR))
		return (SL_WRITE);
	return (SL_READ);
}

/**
 * sl_externalize_stat - Prepare a 'struct stat' buffer for high-level
 *	representation, suitable for transmission between systems.
 * @stb: system stat buffer.
 * @sstb: higher-level app stat buffer.
 *
 * Note: the following fields will NOT be filled in as there is no
 * equivalent in the system stat:
 *
 *	- sst_ptruncgen
 *	- sst_utimgen
 *	- sst_fg
 */
void
sl_externalize_stat(const struct stat *stb, struct srt_stat *sstb)
{
//	sstb->sst_fid		= stb->st_ino;
//	sstb->sst_gen		= FGEN_ANY;
	sstb->sst_dev		= stb->st_dev;
	sstb->sst_mode		= stb->st_mode;
	sstb->sst_nlink		= stb->st_nlink;
	sstb->sst_uid		= stb->st_uid;
	sstb->sst_gid		= stb->st_gid;
	sstb->sst_rdev		= stb->st_rdev;
	sstb->sst_size		= stb->st_size;
	sstb->sst_blksize	= stb->st_blksize;
	sstb->sst_blocks	= stb->st_blocks;
	PFL_STB_ATIME_GET(stb, &sstb->sst_atime, &sstb->sst_atime_ns);
	PFL_STB_MTIME_GET(stb, &sstb->sst_mtime, &sstb->sst_mtime_ns);
	PFL_STB_CTIME_GET(stb, &sstb->sst_ctime, &sstb->sst_ctime_ns);
}

void
sl_internalize_stat(const struct srt_stat *sstb, struct stat *stb)
{
	memset(stb, 0, sizeof(*stb));
	stb->st_dev		= sstb->sst_dev;
	stb->st_ino		= sstb->sst_fg.fg_fid;
	stb->st_mode		= sstb->sst_mode;
	stb->st_nlink		= sstb->sst_nlink;
	stb->st_uid		= sstb->sst_uid;
	stb->st_gid		= sstb->sst_gid;
	stb->st_rdev		= sstb->sst_rdev;
	stb->st_size		= sstb->sst_size;
	stb->st_blksize		= sstb->sst_blksize;
	stb->st_blocks		= sstb->sst_blocks;
	PFL_STB_ATIME_SET(sstb->sst_atime, sstb->sst_atime_ns, stb);
	PFL_STB_MTIME_SET(sstb->sst_mtime, sstb->sst_mtime_ns, stb);
	PFL_STB_CTIME_SET(sstb->sst_ctime, sstb->sst_ctime_ns, stb);
}

void
sl_externalize_statfs(const struct statvfs *sfb,
    struct srt_statfs *ssfb)
{
	memset(ssfb, 0, sizeof(*ssfb));
	ssfb->sf_bsize		= sfb->f_frsize;
	ssfb->sf_iosize		= sfb->f_bsize;
	ssfb->sf_blocks		= sfb->f_blocks;
	ssfb->sf_bfree		= sfb->f_bfree;
	ssfb->sf_bavail		= sfb->f_bavail;
	ssfb->sf_files		= sfb->f_files;
	ssfb->sf_ffree		= sfb->f_ffree;
	ssfb->sf_favail		= sfb->f_favail;
}

void
sl_internalize_statfs(const struct srt_statfs *ssfb,
    struct statvfs *sfb)
{
	memset(sfb, 0, sizeof(*sfb));
	sfb->f_bsize		= ssfb->sf_iosize;
	sfb->f_frsize		= ssfb->sf_bsize;
	sfb->f_blocks		= ssfb->sf_blocks;
	sfb->f_bfree		= ssfb->sf_bfree;
	sfb->f_bavail		= ssfb->sf_bavail;
	sfb->f_files		= ssfb->sf_files;
	sfb->f_ffree		= ssfb->sf_ffree;
	sfb->f_favail		= ssfb->sf_favail;
}

#ifdef HAVE_STATFS_FSTYPE
void
statfs_2_statvfs(const struct statfs *sfb, struct statvfs *svfb)
{
	svfb->f_frsize		= sfb->f_bsize;
	svfb->f_bsize		= sfb->f_iosize;
	svfb->f_blocks		= sfb->f_blocks;
	svfb->f_bfree		= sfb->f_bfree;
	svfb->f_bavail		= sfb->f_bavail;
	svfb->f_files		= sfb->f_files;
	svfb->f_ffree		= sfb->f_ffree;
	svfb->f_favail		= sfb->f_files;
}
#endif

#define PERMCHECK(accmode, fmode, mask)						\
	(((((accmode) & R_OK) && ((fmode) & ((mask) & _S_IRUGO)) == 0) ||	\
	  (((accmode) & W_OK) && ((fmode) & ((mask) & _S_IWUGO)) == 0) ||	\
	  (((accmode) & X_OK) && ((fmode) & ((mask) & _S_IXUGO)) == 0)) ? EACCES : 0)

/**
 * checkcreds - Perform a classic UNIX-style permission access check.
 * @sstb: ownership info.
 * @pcrp: credentials of access.
 * @accmode: type of access.
 * Returns zero on success, errno code on failure.
 */
int
checkcreds(const struct srt_stat *sstb, const struct pscfs_creds *pcrp,
    int accmode)
{
	gid_t gid;
	int n;

#if PFL_DEBUG > 0
	psc_assert(!pfl_memchk(sstb, 0, sizeof(*sstb)));
#endif
	/* root can do anything, unless rootsquash is enabled */
	if (!globalConfig.gconf_root_squash && pcrp->pcr_uid == 0)
		return (0);

	if (sstb->sst_uid == pcrp->pcr_uid)
		return (PERMCHECK(accmode, sstb->sst_mode, S_IRWXU));
	for (n = 0; n <= pcrp->pcr_ngid; n++) {
		gid = n == 0 ? pcrp->pcr_gid : pcrp->pcr_gidv[n - 1];
		if (sstb->sst_gid == gid)
			return (PERMCHECK(accmode, sstb->sst_mode,
			    S_IRWXG));
	}
	return (PERMCHECK(accmode, sstb->sst_mode, S_IRWXO));
}
