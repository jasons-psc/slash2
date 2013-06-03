/* $Id$ */
/*
 * %PSCGPL_START_COPYRIGHT%
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

#define PSC_SUBSYS SLSS_FCMH
#include "slsubsys.h"

#include "pfl/cdefs.h"

#include "fidc_mds.h"
#include "inode.h"
#include "journal_mds.h"
#include "mdsio.h"
#include "mdslog.h"
#include "slashd.h"
#include "slerr.h"

#include "zfs-fuse/zfs_slashlib.h"

__static void
mds_inode_od_initnew(struct slash_inode_handle *ih)
{
	ih->inoh_flags = INOH_INO_NEW;

	/* For now this is a fixed size. */
	ih->inoh_ino.ino_bsz = SLASH_BMAP_SIZE;
	ih->inoh_ino.ino_version = INO_VERSION;
}

int
mds_inode_read(struct slash_inode_handle *ih)
{
	uint64_t crc, od_crc = 0;
	struct fidc_membh *f;
	struct iovec iovs[2];
	int rc, locked, vfsid;
	uint16_t vers;
	size_t nb;

	f = ih->inoh_fcmh;
	if (mdsio_fid_to_vfsid(fcmh_2_fid(f), &vfsid) < 0)
		return (EINVAL);

	locked = INOH_RLOCK(ih); /* XXX bad on slow archiver */
	psc_assert(ih->inoh_flags & INOH_INO_NOTLOADED);

	memset(&ih->inoh_ino, 0, sizeof(ih->inoh_ino));

	iovs[0].iov_base = &ih->inoh_ino;
	iovs[0].iov_len = sizeof(ih->inoh_ino);
	iovs[1].iov_base = &od_crc;
	iovs[1].iov_len = sizeof(od_crc);

	rc = mdsio_preadv(vfsid, &rootcreds, iovs, nitems(iovs), &nb, 0,
	    inoh_2_mdsio_data(ih));

	if (rc == 0 && nb != sizeof(ih->inoh_ino) + sizeof(od_crc))
		rc = SLERR_SHORTIO;

	if (rc == SLERR_SHORTIO && od_crc == 0 &&
	    pfl_memchk(&ih->inoh_ino, 0, sizeof(ih->inoh_ino))) {
		if (!mds_inode_update_interrupted(vfsid, ih, &rc)) {
			DEBUG_INOH(PLL_INFO, ih, "detected a new inode");
			mds_inode_od_initnew(ih);
			rc = 0;
		}
	} else if (rc && rc != SLERR_SHORTIO) {
		DEBUG_INOH(PLL_ERROR, ih, "inode read error %d", rc);
	} else {
		psc_crc64_calc(&crc, &ih->inoh_ino, sizeof(ih->inoh_ino));
		if (crc != od_crc) {
			vers = ih->inoh_ino.ino_version;
			memset(&ih->inoh_ino, 0, sizeof(ih->inoh_ino));

			if (mds_inode_update_interrupted(vfsid, ih, &rc))
				;
			else if (vers && vers < INO_VERSION)
				rc = mds_inode_update(vfsid, ih, vers);
			else if (rc == SLERR_SHORTIO)
				DEBUG_INOH(PLL_INFO, ih,
				    "short read I/O (%zd vs %zd)",
				    nb, sizeof(ih->inoh_ino) +
				    sizeof(od_crc));
			else {
				rc = SLERR_BADCRC;
				DEBUG_INOH(PLL_WARN, ih, "CRC failed "
				    "want=%"PSCPRIxCRC64", got=%"PSCPRIxCRC64,
				    od_crc, crc);
			}
		}
		if (rc == 0) {
			ih->inoh_flags &= ~INOH_INO_NOTLOADED;
			DEBUG_INOH(PLL_INFO, ih, "successfully loaded inode od");
		}
	}
	INOH_URLOCK(ih, locked);
	return (rc);
}

int
mds_inode_write(int vfsid, struct slash_inode_handle *ih, void *logf, void *arg)
{
	int rc, wasbusy, waslocked;
	struct fidc_membh *f;
	struct iovec iovs[2];
	uint64_t crc;
	size_t nb;

	INOH_LOCK_ENSURE(ih);

	f = ih->inoh_fcmh;
	wasbusy = FCMH_REQ_BUSY(f, &waslocked);

	psc_crc64_calc(&crc, &ih->inoh_ino, sizeof(ih->inoh_ino));

	iovs[0].iov_base = &ih->inoh_ino;
	iovs[0].iov_len = sizeof(ih->inoh_ino);
	iovs[1].iov_base = &crc;
	iovs[1].iov_len = sizeof(crc);

	INOH_ULOCK(ih);

	if (logf)
		mds_reserve_slot(1);
	rc = mdsio_pwritev(vfsid, &rootcreds, iovs, nitems(iovs), &nb, 0, 0,
	    inoh_2_mdsio_data(ih), logf, arg);
	if (logf)
		mds_unreserve_slot(1);

	INOH_LOCK(ih);

	if (rc == 0 && nb != sizeof(ih->inoh_ino) + sizeof(crc))
		rc = SLERR_SHORTIO;

	if (rc)
		DEBUG_INOH(PLL_ERROR, ih,
		    "mdsio_pwritev: error (resid=%zd nb=%zd rc=%d)",
		    sizeof(ih->inoh_ino) + sizeof(crc), nb, rc);
	else {
		DEBUG_INOH(PLL_INFO, ih, "wrote inode, "
		    "flags=%x size=%"PRIu64" data=%p",
		    ih->inoh_flags, inoh_2_fsz(ih),
		    inoh_2_mdsio_data(ih));
		if (ih->inoh_flags & INOH_INO_NEW)
			ih->inoh_flags &= ~INOH_INO_NEW;
	}
	FCMH_UREQ_BUSY(f, wasbusy, waslocked);
	return (rc);
}

int
mds_inox_write(int vfsid, struct slash_inode_handle *ih, void *logf,
    void *arg)
{
	int rc, wasbusy, waslocked;
	struct fidc_membh *f;
	struct iovec iovs[2];
	uint64_t crc;
	size_t nb;

	INOH_LOCK_ENSURE(ih);

	psc_assert(ih->inoh_extras);

	f = ih->inoh_fcmh;
	wasbusy = FCMH_REQ_BUSY(f, &waslocked);

	psc_crc64_calc(&crc, ih->inoh_extras, INOX_SZ);

	iovs[0].iov_base = ih->inoh_extras;
	iovs[0].iov_len = INOX_SZ;
	iovs[1].iov_base = &crc;
	iovs[1].iov_len = sizeof(crc);

	INOH_ULOCK(ih);

	if (logf)
		mds_reserve_slot(1);
	rc = mdsio_pwritev(vfsid, &rootcreds, iovs, nitems(iovs), &nb,
	    SL_EXTRAS_START_OFF, 0, inoh_2_mdsio_data(ih), logf, arg);
	if (logf)
		mds_unreserve_slot(1);

	INOH_LOCK(ih);

	if (rc == 0 && nb != INOX_SZ + sizeof(crc))
		rc = SLERR_SHORTIO;
	if (rc)
		DEBUG_INOH(PLL_ERROR, ih, "mdsio_pwritev: error (rc=%d)",
		    rc);

	FCMH_UREQ_BUSY(f, wasbusy, waslocked);
	return (rc);
}

int
mds_inox_load_locked(struct slash_inode_handle *ih)
{
	struct fidc_membh *f;
	struct iovec iovs[2];
	uint64_t crc, od_crc;
	int rc, vfsid;
	size_t nb;

	INOH_LOCK_ENSURE(ih);

	psc_assert(ih->inoh_extras == NULL);

	ih->inoh_extras = PSCALLOC(INOX_SZ);

	iovs[0].iov_base = ih->inoh_extras;
	iovs[0].iov_len = INOX_SZ;
	iovs[1].iov_base = &od_crc;
	iovs[1].iov_len = sizeof(od_crc);

	f = ih->inoh_fcmh;
	mdsio_fid_to_vfsid(fcmh_2_fid(f), &vfsid);
	rc = mdsio_preadv(vfsid, &rootcreds, iovs, nitems(iovs), &nb,
	    SL_EXTRAS_START_OFF, inoh_2_mdsio_data(ih));
	if (rc == 0 && od_crc == 0 &&
	    pfl_memchk(ih->inoh_extras, 0, INOX_SZ)) {
		rc = 0;
	} else if (rc) {
		rc = -abs(rc);
		DEBUG_INOH(PLL_ERROR, ih, "read inox: %d", rc);
	} else if (nb != INOX_SZ + sizeof(od_crc)) {
		rc = -SLERR_SHORTIO;
		DEBUG_INOH(PLL_ERROR, ih, "read inox: %d nb=%zu", rc, nb);
	} else {
		psc_crc64_calc(&crc, ih->inoh_extras, INOX_SZ);
		if (crc != od_crc) {
			psclog_errorx("inox CRC fail (rc=%d) "
			    "disk=%"PSCPRIxCRC64" mem=%"PSCPRIxCRC64,
			    rc, od_crc, crc);
			rc = -SLERR_BADCRC;
		}
	}
	if (rc) {
		PSCFREE(ih->inoh_extras);
		ih->inoh_extras = NULL;
	}
	return (rc);
}

int
mds_inox_ensure_loaded(struct slash_inode_handle *ih)
{
	int locked, rc = 0;

	locked = INOH_RLOCK(ih);
	if (ih->inoh_extras == NULL)
		rc = mds_inox_load_locked(ih);
	INOH_URLOCK(ih, locked);
	return (rc);
}

int
mds_inodes_odsync(int vfsid, struct fidc_membh *f,
    void (*logf)(void *, uint64_t, int))
{
	struct slash_inode_handle *ih = fcmh_2_inoh(f);
	int locked, rc;

	locked = INOH_RLOCK(ih);
	if (ih->inoh_ino.ino_nrepls > SL_DEF_REPLICAS) {
		/*
		 * Don't assume the inox have been loaded.  It's
		 * possible our caller didn't require them (BZ #258).
		 */
		rc = mds_inox_ensure_loaded(ih);
		if (rc) {
			INOH_URLOCK(ih, locked);
			return (rc);
		}
	}

	rc = mds_inode_write(vfsid, ih, logf, f);
	if (rc == 0 && ih->inoh_ino.ino_nrepls > SL_DEF_REPLICAS)
		rc = mds_inox_write(vfsid, ih, NULL, NULL);

	DEBUG_FCMH(PLL_DEBUG, f, "wrote updated ino_repls logf=%p", logf);
	INOH_URLOCK(ih, locked);
	return (rc);
}

char *
_dump_ino(char *buf, size_t siz, const struct slash_inode_od *ino)
{
	char nbuf[LINE_MAX], rbuf[LINE_MAX];
	int nr, j;

	nr = ino->ino_nrepls;
	if (nr < 0)
		nr = 1;
	else if (nr > SL_DEF_REPLICAS)
		nr = SL_DEF_REPLICAS;

	rbuf[0] = '\0';
	for (j = 0; j < nr; j++) {
		if (j)
			strlcat(rbuf, ",", sizeof(rbuf));
		snprintf(nbuf, sizeof(nbuf), "%u",
		    ino->ino_repls[j].bs_id);
		strlcat(rbuf, nbuf, sizeof(rbuf));
	}
	snprintf(buf, siz, "bsz:%u nr:%u nbpol:%u repl:%s",
	    ino->ino_bsz, ino->ino_nrepls, ino->ino_replpol, rbuf);
	return (buf);
}

#if PFL_DEBUG > 0
void
dump_inoh(const struct slash_inode_handle *ih)
{
	DEBUG_INOH(PLL_MAX, ih, "");
}

void
dump_ino(const struct slash_inode_od *ino)
{
	char buf[BUFSIZ];

	fprintf(stderr, "%s", _dump_ino(buf, sizeof(buf), ino));
}
#endif
