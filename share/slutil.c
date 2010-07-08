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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "pfl/fcntl.h"
#include "pfl/stat.h"
#include "pfl/types.h"
#include "psc_util/log.h"

#include "creds.h"
#include "pathnames.h"
#include "sltypes.h"
#include "slutil.h"

const char *sl_datadir = SL_PATH_DATADIR;

void
dump_statbuf(int level, const struct stat *stb)
{
	DEBUG_STATBUF(level, stb, "");
}

void
print_flag(const char *str, int *seq)
{
	printf("%s%s", *seq ? "|" : "", str);
	*seq = 1;
}

#define PR_O_FLAG(fl, val, seq)						\
	do {								\
		if ((val) & (fl)) {					\
			print_flag(#fl, (seq));				\
			(val) &= ~(fl);					\
		}							\
	} while (0)

void
dump_fflags(int fflags)
{
	int seq = 0;

	PR_O_FLAG(O_WRONLY, fflags, &seq);
	PR_O_FLAG(O_RDWR, fflags, &seq);
	if ((fflags & O_ACCMODE) == O_RDONLY)
		print_flag("O_RDONLY", &seq);

	PR_O_FLAG(O_CREAT, fflags, &seq);
	PR_O_FLAG(O_EXCL, fflags, &seq);
	PR_O_FLAG(O_TRUNC, fflags, &seq);
	PR_O_FLAG(O_APPEND, fflags, &seq);
	PR_O_FLAG(O_NONBLOCK, fflags, &seq);
	PR_O_FLAG(O_SYNC, fflags, &seq);
	PR_O_FLAG(O_NOCTTY, fflags, &seq);
	PR_O_FLAG(O_NOFOLLOW, fflags, &seq);

	PR_O_FLAG(O_DSYNC, fflags, &seq);
	PR_O_FLAG(O_RSYNC, fflags, &seq);
	PR_O_FLAG(O_ASYNC, fflags, &seq);
	PR_O_FLAG(O_DIRECTORY, fflags, &seq);
	PR_O_FLAG(O_EXLOCK, fflags, &seq);
	PR_O_FLAG(O_SHLOCK, fflags, &seq);

	PR_O_FLAG(O_DIRECT, fflags, &seq);
	PR_O_FLAG(O_CLOEXEC, fflags, &seq);
	PR_O_FLAG(O_SYMLINK, fflags, &seq);
	PR_O_FLAG(O_NOATIME, fflags, &seq);
	PR_O_FLAG(O_LARGEFILE, fflags, &seq);

	if (fflags) {
		print_flag("", &seq);
		printf("%x", fflags);
	}
	printf("\n");
}

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
 *	- sst_gen
 *	- sst_ptruncgen
 */
void
sl_externalize_stat(const struct stat *stb, struct srt_stat *sstb)
{
	sstb->sst_dev		= stb->st_dev;
	sstb->sst_ino		= stb->st_ino;
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
	stb->st_ino		= sstb->sst_ino;
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
sl_externalize_statfs(const struct statvfs *sfb, struct srt_statfs *ssfb)
{
	ssfb->sf_bsize		= sfb->f_bsize;
	ssfb->sf_frsize		= sfb->f_frsize;
	ssfb->sf_blocks		= sfb->f_blocks;
	ssfb->sf_bfree		= sfb->f_bfree;
	ssfb->sf_bavail		= sfb->f_bavail;
	ssfb->sf_files		= sfb->f_files;
	ssfb->sf_ffree		= sfb->f_ffree;
	ssfb->sf_favail		= sfb->f_favail;
	ssfb->sf_fsid		= sfb->f_fsid;
	ssfb->sf_flag		= sfb->f_flag;
	ssfb->sf_namemax	= sfb->f_namemax;
}

void
sl_internalize_statfs(const struct srt_statfs *ssfb, struct statvfs *sfb)
{
	sfb->f_bsize		= ssfb->sf_bsize;
	sfb->f_frsize		= ssfb->sf_frsize;
	sfb->f_blocks		= ssfb->sf_blocks;
	sfb->f_bfree		= ssfb->sf_bfree;
	sfb->f_bavail		= ssfb->sf_bavail;
	sfb->f_files		= ssfb->sf_files;
	sfb->f_ffree		= ssfb->sf_ffree;
	sfb->f_favail		= ssfb->sf_favail;
	sfb->f_fsid		= ssfb->sf_fsid;
	sfb->f_flag		= ssfb->sf_flag;
	sfb->f_namemax		= ssfb->sf_namemax;
}

/**
 * checkcreds - Perform a classic UNIX permission access check.
 * @sstb: ownership info.
 * @cr: credentials of access.
 * @xmode: type of access.
 * Returns zero on success, errno code on failure.
 */
int
checkcreds(const struct srt_stat *sstb, const struct slash_creds *cr,
    int xmode)
{
	if (cr->uid == 0)
		return (0);
	if (sstb->sst_uid == cr->uid) {
		if (((xmode & R_OK) && (sstb->sst_mode & S_IRUSR) == 0) ||
		    ((xmode & W_OK) && (sstb->sst_mode & S_IWUSR) == 0) ||
		    ((xmode & X_OK) && (sstb->sst_mode & S_IXUSR) == 0))
			return (EACCES);
		return (0);
	}
	/* XXX check process supplementary group list */
	if (sstb->sst_gid == cr->gid) {
		if (((xmode & R_OK) && (sstb->sst_mode & S_IRGRP) == 0) ||
		    ((xmode & W_OK) && (sstb->sst_mode & S_IWGRP) == 0) ||
		    ((xmode & X_OK) && (sstb->sst_mode & S_IXGRP) == 0))
			return (EACCES);
		return (0);
	}
	if (((xmode & R_OK) && (sstb->sst_mode & S_IROTH) == 0) ||
	    ((xmode & W_OK) && (sstb->sst_mode & S_IWOTH) == 0) ||
	    ((xmode & X_OK) && (sstb->sst_mode & S_IXOTH) == 0))
		return (EACCES);
	return (0);
}
