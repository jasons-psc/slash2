/* $Id$ */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define FUSE_USE_VERSION 26

#include <fuse.h>

#include "pfl.h"
#include "psc_types.h"
#include "psc_rpc/rpc.h"
#include "psc_rpc/rsx.h"
#include "psc_util/cdefs.h"
#include "psc_util/log.h"
#include "psc_mount/dhfh.h"

#include "mount_slash.h"
#include "slashrpc.h"
#include "offtree.h"

__static void
msl_oftrq_build(struct offtree_req *r, struct bmap_cache_memb *b, 
		off_t off, size_t len)
{
	/* Ensure the offset fits within the range and mask off the
	 *  lower bits to align with the offtree's page size.
	 */
	psc_assert((off + len) <= SLASH_BMAP_SIZE);

	r->oftrq_darray = PSCALLOC(sizeof(struct dynarray));
	r->oftrq_root   = &b->bcm_oftree;
	r->oftrq_memb   = &b->bcm_oftree.oftr_memb;
	r->oftrq_width  = r->oftrq_depth = 0;
	r->oftrq_off    = off & SLASH_BMAP_BLKMASK;
	r->oftrq_nblks  = (off + len) / SLASH_BMAP_BLKSZ + 
		(((off + len) % SLASH_BMAP_BLKSZ) ? 1 : 0);

	DEBUG_OFFTREQ(PLL_TRACE, r, "newly built request");
}

__static void
msl_fcm_get(struct fhent *fh)
{
	struct fcache_memb_handle *f = sl_fidc_get(&fidcFreeList);

	fidcache_memb_init(f);
	/* Incref so that this inode is not immediately 
	 *  considered for reaping.
	 */
	fcmh_incref(fcmh);
	/* Cross-associate the fcmh and fhent structures.
	 */
	fcmh->fcmh_fh = fh->fh;	
	fh->fh_pri    = fcmh;

	return (fcmh);
}

#define BMAP_MAX_GET 63
/**
 * msl_bmap_fetch - perform a blocking 'get' operation to retrieve one or more bmaps from the MDS.
 * @f: pointer to the fid cache structure to which this bmap belongs.
 * @b: the block id to retrieve (block size == SLASH_BMAP_SIZE).
 * @n: the number of bmaps to retrieve (serves as a simple read-ahead mechanism)
 */
__static int 
msl_bmap_fetch(struct fidcache_memb_handle *f, sl_blkno_t b, size_t n) 
{
	struct pscrpc_bulk_desc *desc;
        struct pscrpc_request *rq;
        struct srm_bmap_req *mq;
        struct srm_bmap_rep *mp;
	struct bmap_cache_memb **bmaps;
        struct iovec *iovs;
        int rc=-1, i;

	psc_assert(n < BMAP_MAX_GET);

	/* Build the new rpc request.
	 */
	if ((rc = RSX_NEWREQ(mds_import, SRM_VERSION,
			     SRMT_GETBMAP, rq, mq, mp)) != 0)
		return (rc);

	mq->blkno = b;
	mq->nblks = n;
	mq->fid   = FID_ANY;

	iovs  = PSCALLOC(sizeof(*iov) * n);
	/* Init the bmap handles and setup the iovs.
	 */
	for (i=0; i < n; i++) {
		bmaps[i] = PSCALLOC(sizeof(bmap_cache_memb));
		bmap_cache_memb_init(bmaps[i], f);
		iovs[i].iov_base = &bmaps[i].bcm_bmapi;
		iovs[i].iov_len  = sizeof(struct bmap_info);
	}
	DEBUG_FCMH(PLL_DEBUG, f, "retrieving bmaps (s=%u, n=%zu)", b, n);

	rsx_bulkputsink(rq, &desc, SRM_BULK_PORTAL, iovs, n);
	if ((rc = rsx_waitrep(rq, sizeof(*mp), &mp)) == 0) {
		/* Verify the return.
		 */
		if (!mp->nblks) {
			psc_errorx("MDS returned 0 bmaps");
			rc = -1;
			goto fail;
		}
		if (mp->nblks > n) {
			psc_errorx("MDS returned more bmaps than"
				   " expected! mp->nblks(%zu) > n(%zu)", 
				   mp->nblks, n);
			rc = -1;
			mp->nblks = 0;
			goto fail;
		}
		/* Add the bmaps to the tree.
		 */
		spinlock(&f->fcmh_lock);
		for (i=0; i < mp->nblks ; i++) {
			SPLAY_INSERT(bmap_cache, &f->fcmh_bmap_cache, 
				     bmap_cache_memb);
			atomic_inc(&f->fcmh_bmap_cache_cnt);
		}
		freelock(&f->fcmh_lock);
	} else
		/* Something went wrong, free all bmaps.
		 */
		mp->nblks = 0;
 fail:	
	/* Free any slack.
	 */
	for (i=mp->nblks; i < n; i++)
		PSCFREE(bmaps[i]);

	PSCFREE(iovs);
	return (rc);
}

__static struct bmap_cache_memb *
msl_bmap_load(struct fcache_memb_handle *f, sl_blkno_t n, int prefetch)
{
	struct bmap_cache_memb t, *b;
	int rc=0;

	t.bcm_bmap_info.bmapi_blkno=n;

	b = SPLAY_FIND(bmap_cache, &f->fcmh_bmap_cache, &t);
	if (!b) {
		/* Retrieve the bmap from the sl_mds.
		 */			
		rc = msl_bmap_fetch(f, t.bcm_bmap_info.bmapi_blkno, 
				    prefetch);
		if (rc)
			return NULL;
	}
	return (SPLAY_FIND(bmap_cache, &f->fcmh_bmap_cache, &t));
}

/**
 * msl_fdreg_cb - (file des registration callback) This is the callback handler issued from fh_register().  Its primary duty is to allocate the fidcache member handle structure and attach it to the file des structure.
 * @fh: the file handle.
 * @op: op type (FD_REG_NEW, FD_REG_EXISTS are valid).
 * @args: array of pointer arguments (not used here).
 */
void
msl_fdreg_cb(struct fhent *fh, int op, __unusedx void *args[])
{
	psc_assert(op == FD_REG_NEW ||
		   op == FD_REG_EXISTS);

	spinlock(&fh->lock);
	if (op == FD_REG_NEW) {
		if (t->fd != FD_REG_INIT) 
			goto exists;

		if (t->fd == FD_REG_INIT) {
			psc_assert(fh->fh_pri == NULL);
			psc_assert(!atomic_read(&fh->refcnt));
 			/* msl_fcm_get() may block for an fcmh, 
			 *  hopefully that doesn't hurt us here since
			 *  the fh is spinlocked.
			 */
			msl_fcm_get(fh);
			t->fd = FD_REG_READY;
		}
		
	} else if (op == FD_REG_EXISTS) {
	exists:
		psc_assert(fh->fd == FD_REG_READY);		
	}
	atomic_inc(&fh->refcnt);
	freelock(&fh->lock);
}

int
msl_read(struct fhent *fh, char *buf, size_t size, off_t off)
{
	struct fcache_memb_handle *f;
	struct offtree_req        *r=NULL;
	struct bmap_cache_memb     t, *b;
	sl_blkno_t                 s, e;
	size_t                     tlen;
	off_t                      roff;
	int i,rc,n=0;

	f = fh->fh_pri;	
	psc_assert(f);
	/* Are these bytes in the cache?
	 *  Get the start and end block regions from the input parameters.
	 */
	s = off / f->fcmh_bmap_sz;
	e = (off + size) / f->fcmh_bmap_sz;
	/* Relativize the length and offset.
	 */
	roff  = off - (s * SLASH_BMAP_SIZE);
	tlen  = MIN((SLASH_BMAP_SIZE - roff), size);
	/* Foreach block range, get its bmap and make a request into its 
	 *  offtree.
	 */
	for (i=0; s < e; s++, i++) {
		/* Load up the bmap, if it's not available then we're out of 
		 *  luck because we have no idea where the data is!
		 */
		b = msl_bmap_load(f, s, (i ? 0 : (e-s)));
		if (!b)
			return -EIO;
		/* Malloc offtree request and pass to the initializer.
		 */
		r = realloc(r, (sizeof(*r)) * i);
		msl_oftrq_build(r[i], b, roff, tlen);

		roff += tlen;
		size -= tlen;		
		tlen  = MIN(SLASH_BMAP_SIZE, size);
	}
}
