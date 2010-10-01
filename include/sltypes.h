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

#ifndef _SL_TYPES_H_
#define _SL_TYPES_H_

#include <sys/types.h>
#include <sys/stat.h>

#include <stdint.h>
#include <pthread.h>

#include "pfl/cdefs.h"

#include "cache_params.h"

typedef uint32_t sl_bmapno_t;			/* file block map index */
typedef uint32_t sl_bmapgen_t;			/* file block map generation */

typedef uint16_t sl_siteid_t;
typedef uint32_t sl_ios_id_t;

typedef uint64_t sl_ino_t;

#define BLKNO_ANY		(~(sl_bmapno_t)0)	/* deprecated */
#define BMAPNO_ANY		((sl_bmapno_t)~0U)

#define IOS_ID_ANY		((sl_ios_id_t)~0U)
#define SITE_ID_ANY		((sl_siteid_t)~0U)

#define BMAPSEQ_ANY		((uint64_t)~0U)

/* breakdown of I/O system ID: # of bits for each part */
#define SL_SITE_BITS		16
#define SL_RES_BITS		16

#define SL_SITE_MASK		0xffff0000
#define SL_RES_MASK		0x0000ffff	/* resource ID mask */

/* I/O flags */
enum rw {
	SL_READ			= 42,
	SL_WRITE		= 43
//	SL_RDWR			= 44
};

/*
 * Defines a storage system which can hold a block or blocks of a file.  A number
 * of these structures are statically allocated within the inode of the file and
 * are fixed for the lifetime of the file.  They apply to snapshots as well as
 * the active file.  Such an arrangement saves us from storing the iosystem id
 * within each block at the cost of limiting the number of iosystems which may
 * manage the blocks of a given file.
 */
typedef struct {
	sl_ios_id_t		bs_id;		/* ID of this block store    */
} __packed sl_replica_t;

/*
 * The default and the maximum number of storage systems that can hold blocks
 * of any given file.
 */
#define SL_DEF_REPLICAS		4
#define SL_MAX_REPLICAS		64

#define SL_INOX_NREPLICAS	(SL_MAX_REPLICAS - SL_DEF_REPLICAS)

typedef uint64_t slfid_t;
typedef uint64_t slfgen_t;

struct srt_dirent {
	uint64_t		ino;
	uint64_t		off;
	uint32_t		namelen;
	uint32_t		type;
	char			name[0];
};
#define fuse_dirent srt_dirent

#define	SL_SETATTRF_METASIZE	(_PSCFS_SETATTRF_LAST << 0)	/* metadata file */
#define SL_SETATTRF_PTRUNCGEN	(_PSCFS_SETATTRF_LAST << 1)	/* partial truncates */
#define SL_SETATTRF_GEN		(_PSCFS_SETATTRF_LAST << 2)	/* full truncate */

#define SL_SETATTRF_CLI_ALL	(SETATTR_MASKF_MODE | SETATTR_MASKF_UID |	\
				 SETATTR_MASKF_GID | SETATTR_MASKF_DATASIZE |	\
				 SETATTR_MASKF_ATIME | SETATTR_MASKF_MTIME |	\
				 SETATTR_MASKF_CTIME)

#define	SLASH2_IGNORE_MTIME	0x80000

struct sl_timespec {
	uint64_t		tv_sec;
	uint64_t		tv_nsec;
};

#endif /* _SL_TYPES_H_ */
