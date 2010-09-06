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

#include "psc_ds/listcache.h"
#include "psc_ds/vbitmap.h"
#include "psc_rpc/rpc.h"
#include "psc_util/atomic.h"
#include "psc_util/lock.h"
#include "psc_util/pthrutil.h"

#include "sltypes.h"
#include "bmap_iod.h"
#include "buffer.h"
#include "fidc_iod.h"
#include "slerr.h"
#include "slvr.h"

struct psc_listcache lruSlvrs;   /* LRU list of clean slivers which may be reaped */
struct psc_listcache crcqSlvrs;  /* Slivers ready to be crc'd and have their
				    crc's shipped to the mds. */

__static SPLAY_GENERATE(biod_slvrtree, slvr_ref, slvr_tentry, slvr_cmp);

__static void
slvr_lru_requeue(struct slvr_ref *s, int tail)
{
	/*
	 * Locking convention: it is legal to request for a list lock while
	 * holding the sliver lock.  On the other hand, when you already hold
	 * the list lock, you should drop the list lock first before asking
	 * for the sliver lock or you should use trylock().
	 */
	LIST_CACHE_LOCK(&lruSlvrs);
	if (tail)
		lc_move2tail(&lruSlvrs, s);
	else
		lc_move2head(&lruSlvrs, s);
	LIST_CACHE_ULOCK(&lruSlvrs);
}

/**
 * slvr_do_crc - Given a sliver reference, Take the CRC of the respective
 *   data and attach the ref to an srm_bmap_crcup structure.
 * @s: the sliver reference.
 * Notes:  Don't hold the lock while taking the CRC.
 * Returns: errno on failure, 0 on success, -1 on not applicable.
 */
int
slvr_do_crc(struct slvr_ref *s)
{
	psc_crc64_t crc;

	/*
	 * SLVR_FAULTING implies that we're bringing this data buffer
	 *   in from the filesystem.
	 *
	 * SLVR_CRCDIRTY means that DATARDY has been set and that
	 *   a write dirtied the buffer and invalidated the CRC.
	 */
	psc_assert(s->slvr_flags & SLVR_PINNED &&
		   (s->slvr_flags & SLVR_FAULTING ||
		    s->slvr_flags & SLVR_CRCDIRTY));

	if (s->slvr_flags & SLVR_FAULTING) {
		if (!s->slvr_pndgreads && !(s->slvr_flags & SLVR_REPLDST)) {
			/*
			 * Small RMW workaround.
			 *  XXX needs to be rectified, the CRC should
			 *    be taken here.
			 */
			psc_assert(s->slvr_pndgwrts);
			return (-1);
		}

		psc_assert(!(s->slvr_flags & SLVR_DATARDY));

		/*
		 * This thread holds faulting status so all others are
		 *  waiting on us which means that exclusive access to
		 *  slvr contents is ours until we set SLVR_DATARDY.
		 *
		 * XXX For now we assume that all blocks are being
		 *  processed, otherwise there's no guarantee that the
		 *  entire slvr was read.
		 */
		if (!(s->slvr_flags & SLVR_REPLDST))
			psc_assert(!psc_vbitmap_nfree(s->slvr_slab->slb_inuse));
		psc_assert(slvr_2_biodi_wire(s));

		if ((slvr_2_crcbits(s) & BMAP_SLVR_DATA) &&
		    (slvr_2_crcbits(s) & BMAP_SLVR_CRC)) {
			psc_assert(!s->slvr_crc_soff);

			psc_crc64_calc(&crc, slvr_2_buf(s, 0),
			       SLVR_CRCLEN(s));

			if (crc != slvr_2_crc(s)) {
				DEBUG_SLVR(PLL_ERROR, s, "crc failed want=%"
				   PRIx64" got=%"PRIx64 " len=%u",
				   slvr_2_crc(s), crc, SLVR_CRCLEN(s));

				DEBUG_BMAP(PLL_ERROR, slvr_2_bmap(s),
				   "slvrnum=%hu", s->slvr_num);

				/* Shouln't need a lock, !SLVR_DATADY
				 */
				s->slvr_crc_eoff = 0;

				return (SLERR_BADCRC);
			} else
				s->slvr_crc_eoff = 0;
		} else
			return (0);

	} else if (s->slvr_flags & SLVR_CRCDIRTY) {
		uint32_t soff, eoff;

		SLVR_LOCK(s);
		DEBUG_SLVR(PLL_NOTIFY, s, "len=%u soff=%u loff=%u",
		   SLVR_CRCLEN(s), s->slvr_crc_soff, s->slvr_crc_loff);

		psc_assert(s->slvr_crc_eoff &&
			   (s->slvr_crc_eoff <= SLASH_BMAP_CRCSIZE));

		if (!s->slvr_crc_loff ||
		    s->slvr_crc_soff != s->slvr_crc_loff) {
			/* Detect non-sequential write pattern into the
			 *   slvr.
			 */
			PSC_CRC64_INIT(&s->slvr_crc);
			s->slvr_crc_soff = 0;
			s->slvr_crc_loff = 0;
		}
		/* Copy values in preparation for lock release.
		 */
		soff = s->slvr_crc_soff;
		eoff = s->slvr_crc_eoff;

		SLVR_ULOCK(s);

#ifdef ADLERCRC32
		//XXX not a running crc?  double check for correctness
		s->slvr_crc = adler32(s->slvr_crc, slvr_2_buf(s, 0) + soff,
			      (int)(eoff - soff));
		crc = s->slvr_crc;
#else
		psc_crc64_add(&s->slvr_crc,
			      (unsigned char *)(slvr_2_buf(s, 0) + soff),
			      (int)(eoff - soff));
		crc = s->slvr_crc;
		PSC_CRC32_FIN(&crc);
#endif

		DEBUG_SLVR(PLL_NOTIFY, s, "crc=%"PRIx64 " len=%u soff=%u",
			   crc, SLVR_CRCLEN(s), s->slvr_crc_soff);

		DEBUG_BMAP(PLL_NOTIFY, slvr_2_bmap(s),
			   "slvrnum=%hu", s->slvr_num);

		SLVR_LOCK(s);
		/* loff is only set here.
		 */
		s->slvr_crc_loff = eoff;

		if (!s->slvr_pndgwrts && !s->slvr_compwrts)
			s->slvr_flags &= ~SLVR_CRCDIRTY;

		if (slvr_2_biodi_wire(s)) {
			slvr_2_crc(s) = crc;
			slvr_2_crcbits(s) |= (BMAP_SLVR_DATA|BMAP_SLVR_CRC);
		}
		SLVR_ULOCK(s);
	} else
		psc_fatal("FAULTING or CRCDIRTY is not set");

	return (-1);
}

void
slvr_clear_inuse(struct slvr_ref *s, int sblk, uint32_t size)
{
	int locked, nblks;

	/* XXX trim startoff from size?? */
	nblks = howmany(size, SLASH_SLVR_BLKSZ);
	locked = SLVR_RLOCK(s);
	psc_vbitmap_unsetrange(s->slvr_slab->slb_inuse, sblk, nblks);
	SLVR_URLOCK(s, locked);
}

__static int
slvr_fsio(struct slvr_ref *s, int sblk, uint32_t size, enum rw rw)
{
	int	i;
	ssize_t	rc;
	int	nblks;
	int	save_errno;
	uint64_t *v8;

	nblks = (size + SLASH_SLVR_BLKSZ - 1) / SLASH_SLVR_BLKSZ;

	psc_assert(s->slvr_flags & SLVR_PINNED);
	psc_assert(rw == SL_READ || rw == SL_WRITE);

	if (rw == SL_READ) {
		psc_assert(s->slvr_flags & SLVR_FAULTING);
		errno = 0;
		rc = pread(slvr_2_fd(s), slvr_2_buf(s, sblk), size,
			   slvr_2_fileoff(s, sblk));
		save_errno = errno;

		/* XXX this is a bit of a hack.  Here we'll check crc's
		 *  only when nblks == an entire sliver.  Only RMW will
		 *  have their checks bypassed.  This should probably be
		 *  handled more cleanly, like checking for RMW and then
		 *  grabbing the crc table, we use the 1MB buffer in
		 *  either case.
		 */

		/* XXX do the right thing when EOF is reached..
		 */
		if (rc > 0 && nblks == SLASH_BLKS_PER_SLVR) {
			int crc_rc;

			s->slvr_crc_soff = 0;
			s->slvr_crc_eoff = rc;

			crc_rc = slvr_do_crc(s);
			if (crc_rc == SLERR_BADCRC)
				DEBUG_SLVR(PLL_ERROR, s,
					   "bad crc blks=%d off=%"PRIx64,
					   nblks, slvr_2_fileoff(s, sblk));
		}

	} else {
		/* Denote that this block(s) have been synced to the
		 *  filesystem.
		 * Should this check and set of the block bits be
		 *  done for read also?  Probably not because the fs
		 *  is only read once and that's protected by the
		 *  FAULT bit.  Also, we need to know which blocks
		 *  to mark as dirty after an RPC.
		 */
		SLVR_LOCK(s);
		for (i = 0; i < nblks; i++) {
			//psc_assert(psc_vbitmap_get(s->slvr_slab->slb_inuse,
			//	       sblk + i));
			psc_vbitmap_unset(s->slvr_slab->slb_inuse, sblk + i);
		}
		errno = 0;
		rc = pwrite(slvr_2_fd(s), slvr_2_buf(s, sblk), size,
			    slvr_2_fileoff(s, sblk));
		SLVR_ULOCK(s);

		save_errno = errno;
	}

	if (rc < 0)
		DEBUG_SLVR(PLL_ERROR, s, "failed (rc=%zd, size=%u) "
			   "%s blks=%d off=%"PRIx64" errno=%d",
			   rc, size, (rw == SL_WRITE ? "SL_WRITE" : "SL_READ"),
			   nblks, slvr_2_fileoff(s, sblk), save_errno);

	else if ((uint32_t)rc != size)
		DEBUG_SLVR(PLL_NOTICE, s, "short io (rc=%zd, size=%u) "
			   "%s blks=%d off=%"PRIu64" errno=%d",
			   rc, size, (rw == SL_WRITE ? "SL_WRITE" : "SL_READ"),
			   nblks, slvr_2_fileoff(s, sblk), save_errno);
	else {
		v8 = slvr_2_buf(s, sblk);
		DEBUG_SLVR(PLL_INFO, s, "ok %s size=%u off=%"PRIu64" rc=%zd nblks=%d "
			   " v8(%"PRIx64")", (rw == SL_WRITE ? "SL_WRITE" : "SL_READ"),
			   size, slvr_2_fileoff(s, sblk), rc, nblks, *v8);
		rc = 0;
	}

	//psc_vbitmap_printbin1(s->slvr_slab->slb_inuse);

	return ((rc < 0) ? (int)-save_errno : (int)0);
}

/**
 * slvr_fsbytes_get - read in the blocks which have their respective bits set
 *   in slab bitmap, trying to coalesce where possible.
 * @s: the sliver.
 */
int
slvr_fsbytes_rio(struct slvr_ref *s)
{
	int	i;
	int	rc;
	int	blk;
	int	nblks;

	psc_trace("psc_vbitmap_nfree() = %d",
		  psc_vbitmap_nfree(s->slvr_slab->slb_inuse));

	if (!(s->slvr_flags & SLVR_DATARDY))
		psc_assert(s->slvr_flags & SLVR_FAULTING);

	psc_assert(s->slvr_flags & SLVR_PINNED);

	rc = 0;
	blk = 0; /* gcc */
	for (i = 0, nblks = 0; i < SLASH_BLKS_PER_SLVR; i++) {
		if (psc_vbitmap_get(s->slvr_slab->slb_inuse, i)) {
			if (nblks == 0)
				blk = i;

			nblks++;
			continue;
		}
		if (nblks) {
			rc = slvr_fsio(s, blk, nblks * SLASH_SLVR_BLKSZ,
				       SL_READ);
			if (rc)
				goto out;

			/* reset nblks so we won't do it again later */
			nblks = 0;
		}
	}

	if (nblks)
		rc = slvr_fsio(s, blk, nblks * SLASH_SLVR_BLKSZ, SL_READ);

 out:
	if (rc) {
		/* There was a problem, unblock any waiters and tell them
		 *   the bad news.
		 */
		SLVR_LOCK(s);
		s->slvr_flags |= SLVR_DATAERR;
		DEBUG_SLVR(PLL_ERROR, s, "slvr_fsio() error, rc=%d", rc);
		SLVR_WAKEUP(s);
		SLVR_ULOCK(s);
	}

	return (rc);
}

/**
 *
 */
int
slvr_fsbytes_wio(struct slvr_ref *s, uint32_t size, uint32_t sblk)
{
	DEBUG_SLVR(PLL_INFO, s, "sblk=%u size=%u", sblk, size);

	return (slvr_fsio(s, sblk, size, SL_WRITE));
}

void
slvr_repl_prep(struct slvr_ref *s, int src_or_dst)
{
	psc_assert((src_or_dst == SLVR_REPLDST) ||
		   (src_or_dst == SLVR_REPLSRC));

	SLVR_LOCK(s);
	psc_assert(!(s->slvr_flags & SLVR_REPLDST) &&
		   !(s->slvr_flags & SLVR_REPLSRC));

	if (src_or_dst == SLVR_REPLSRC)
		psc_assert(s->slvr_pndgreads > 0);
	else
		psc_assert(s->slvr_pndgwrts > 0);

	s->slvr_flags |= src_or_dst;

	DEBUG_SLVR(PLL_INFO, s, "replica_%s", (src_or_dst == SLVR_REPLSRC) ?
		   "src" : "dst");

	SLVR_ULOCK(s);
}

void
slvr_slab_prep(struct slvr_ref *s, enum rw rw)
{
	struct sl_buffer *tmp=NULL;

	SLVR_LOCK(s);
 restart:
	/* slvr_lookup() must pin all slvrs to avoid racing with
	 *   the reaper.
	 */
	psc_assert(s->slvr_flags & SLVR_PINNED);

	if (rw == SL_WRITE)
		psc_assert(s->slvr_pndgwrts > 0);
	else
		psc_assert(s->slvr_pndgreads > 0);

 newbuf:
	if (s->slvr_flags & SLVR_NEW) {
		if (!tmp) {
			/* Drop the lock before potentially blocking
			 *   in the pool reaper.  To do this we
			 *   must first allocate to a tmp pointer.
			 */
		getbuf:
			SLVR_ULOCK(s);

			tmp = psc_pool_get(slBufsPool);
			sl_buffer_fresh_assertions(tmp);
			sl_buffer_clear(tmp, tmp->slb_blksz * tmp->slb_nblks);
			SLVR_LOCK(s);
			goto newbuf;

		} else
			psc_assert(tmp);

		psc_assert(psclist_disjoint(&s->slvr_lentry));
		s->slvr_flags &= ~SLVR_NEW;
		s->slvr_slab = tmp;
		tmp = NULL;
		/* Until the slab is added to the sliver, the sliver is private
		 *  to the bmap's biod_slvrtree.
		 */
		s->slvr_flags |= SLVR_LRU;
		/* note: lc_addtail() will grab the list lock itself */
		lc_addtail(&lruSlvrs, s);

	} else if ((s->slvr_flags & SLVR_LRU) && !s->slvr_slab) {
		psc_assert(!(s->slvr_flags & SLVR_DATARDY));
		if (!tmp)
			goto getbuf;
		else {
			s->slvr_slab = tmp;
			tmp = NULL;
		}

	} else if (s->slvr_flags & SLVR_SLBFREEING) {
		DEBUG_SLVR(PLL_INFO, s, "caught slbfreeing");
		SLVR_WAIT(s, (s->slvr_flags & SLVR_SLBFREEING));
		goto restart;
	}

	DEBUG_SLVR(PLL_INFO, s, "should have slab");
	psc_assert(s->slvr_slab);
	SLVR_ULOCK(s);

	if (tmp)
		psc_pool_return(slBufsPool, tmp);
}

/**
 * slvr_io_prep - prepare a sliver for an incoming io.  This may entail
 *   faulting 32k aligned regions in from the underlying fs.
 * @s: the sliver
 * @off: offset into the slvr (not bmap or file object)
 * @len: len relative to the slvr
 * @rw:  read or write op
 */
int
slvr_io_prep(struct slvr_ref *s, uint32_t off, uint32_t len, enum rw rw)
{
	int		i;
	int		rc;
	int		blks;
	int		unaligned[2] = {-1, -1};

	SLVR_LOCK(s);
	psc_assert(s->slvr_flags & SLVR_PINNED);
	/*
	 * Common courtesy requires us to wait for another threads' work
	 *   FIRST. Otherwise, we could bail out prematurely when the
	 *   data is ready without considering the range we want to write.
	 *
	 * Note we have taken our read or write references, so the sliver
	 *   won't be freed from under us.
	 */
	if (s->slvr_flags & SLVR_FAULTING) {
		psc_assert(!(s->slvr_flags & SLVR_DATARDY));
		SLVR_WAIT(s, !(s->slvr_flags & (SLVR_DATARDY|SLVR_DATAERR)));
		psc_assert((s->slvr_flags & (SLVR_DATARDY|SLVR_DATAERR)));
	}

	DEBUG_SLVR(((s->slvr_flags & SLVR_DATAERR) ? PLL_ERROR : PLL_INFO), s,
		   "slvrno=%hu off=%u len=%u rw=%d",
		   s->slvr_num, off, len, rw);

	if (s->slvr_flags & SLVR_DATAERR) {
		rc = -1;
		goto out;

	} else if (s->slvr_flags & SLVR_DATARDY) {
		if (rw == SL_READ)
			goto out;
	} else {
		/* Importing data into the sliver is now our responsibility,
		 *  other IO into this region will block until SLVR_FAULTING
		 *  is released.
		 */
		s->slvr_flags |= SLVR_FAULTING;
		if (rw == SL_READ) {
			psc_vbitmap_setall(s->slvr_slab->slb_inuse);
			goto do_read;
		}
	}

	psc_assert(rw != SL_READ);
	/* Setting of this flag here is mainly for informative purposes.
	 *   It may be unset in do_crc so we set it again in wio_done.
	 * Replication sink buffers do not need a crc new crc to be taken.
	 */
	if (!(s->slvr_flags & SLVR_REPLDST))
		s->slvr_flags |= SLVR_CRCDIRTY;

	if (!off && len == SLASH_SLVR_SIZE) {
		/* Full sliver write, no need to read blocks from disk.
		 *  All blocks will be dirtied by the incoming network IO.
		 */
		psc_vbitmap_setall(s->slvr_slab->slb_inuse);
		goto out;
	}
	/*
	 * Prepare the sliver for a read-modify-write.  Mark the blocks
	 * that need to be read as 1 so that they can be faulted in by
	 * slvr_fsbytes_io().  We can have at most two unaligned writes.
	 */
	if (off) {
		blks = (off / SLASH_SLVR_BLKSZ);
		if (off & SLASH_SLVR_BLKMASK)
			unaligned[0] = blks;

		for (i=0; i <= blks; i++)
			psc_vbitmap_set(s->slvr_slab->slb_inuse, i);
	}
	if ((off + len) < SLASH_SLVR_SIZE) {
		blks = (off + len) / SLASH_SLVR_BLKSZ;
		if ((off + len) & SLASH_SLVR_BLKMASK)
			unaligned[1] = blks;

		for (i = blks; i < SLASH_BLKS_PER_SLVR; i++)
			psc_vbitmap_set(s->slvr_slab->slb_inuse, i);
	}

	//psc_vbitmap_printbin1(s->slvr_slab->slb_inuse);
	psc_info("psc_vbitmap_nfree()=%d",
		 psc_vbitmap_nfree(s->slvr_slab->slb_inuse));
	/* We must have found some work to do.
	 */
	psc_assert(psc_vbitmap_nfree(s->slvr_slab->slb_inuse) <
		   (int)SLASH_BLKS_PER_SLVR);

	if (s->slvr_flags & SLVR_DATARDY)
		goto invert;

 do_read:
	SLVR_ULOCK(s);
	/* Execute read to fault in needed blocks after dropping
	 *   the lock.  All should be protected by the FAULTING bit.
	 */
	if ((rc = slvr_fsbytes_rio(s)))
		return (rc);

	if (rw == SL_READ) {
		SLVR_LOCK(s);
		psc_assert(!(s->slvr_flags & SLVR_DATARDY));

		s->slvr_flags |= SLVR_DATARDY;
		s->slvr_flags &= ~SLVR_FAULTING;

		psc_vbitmap_invert(s->slvr_slab->slb_inuse);
		//psc_vbitmap_printbin1(s->slvr_slab->slb_inuse);
		DEBUG_SLVR(PLL_INFO, s, "FAULTING -> DATARDY");
		SLVR_WAKEUP(s);
		SLVR_ULOCK(s);

		return (0);

	} else {
		/* Above, the bits were set for the RMW blocks, now
		 *  that they have been read, invert the bitmap so that
		 *  it properly represents the blocks to be dirtied by
		 *  the rpc.
		 */
		SLVR_LOCK(s);
	invert:
		psc_vbitmap_invert(s->slvr_slab->slb_inuse);
		if (unaligned[0] >= 0)
			psc_vbitmap_set(s->slvr_slab->slb_inuse, unaligned[0]);

		if (unaligned[1] >= 0)
			psc_vbitmap_set(s->slvr_slab->slb_inuse, unaligned[1]);
		//psc_vbitmap_printbin1(s->slvr_slab->slb_inuse);
	out:
		SLVR_ULOCK(s);
	}

	return (0);
}

void
slvr_rio_done(struct slvr_ref *s)
{
	SLVR_LOCK(s);

	s->slvr_pndgreads--;
	if (slvr_lru_tryunpin_locked(s)) {
		slvr_lru_requeue(s, 1);
		DEBUG_SLVR(PLL_DEBUG, s, "unpinned");
	} else
		DEBUG_SLVR(PLL_DEBUG, s, "ops still pending or dirty");

	if (s->slvr_flags & SLVR_REPLSRC) {
		psc_assert((s->slvr_flags & SLVR_REPLDST) == 0);
		s->slvr_flags &= ~SLVR_REPLSRC;
	}

	SLVR_ULOCK(s);
}

__static void
slvr_schedule_crc_locked(struct slvr_ref *s)
{
	psc_assert(s->slvr_flags & SLVR_PINNED);
	psc_assert(s->slvr_flags & SLVR_CRCDIRTY);
	psc_assert(s->slvr_flags & SLVR_LRU);

	slvr_2_biod(s)->biod_crcdrty_slvrs++;

	DEBUG_SLVR(PLL_INFO, s, "crc sched (ndirty slvrs=%u)",
		   slvr_2_biod(s)->biod_crcdrty_slvrs);

	s->slvr_flags &= ~SLVR_LRU;

	lc_remove(&lruSlvrs, s);
	lc_addqueue(&crcqSlvrs, s);
}

/**
 * slvr_wio_done - called after a write rpc has completed.  The sliver may
 *    be FAULTING which is handled separately from DATARDY.  If FAULTING,
 *    this thread must wake up sleepers on the bmap waitq.
 * Notes: conforming with standard lock ordering, this routine drops
 *    the sliver lock prior to performing list operations.
 */
void
slvr_wio_done(struct slvr_ref *s, uint32_t off, uint32_t len)
{
	SLVR_LOCK(s);
	psc_assert(s->slvr_flags & SLVR_PINNED);
	psc_assert(s->slvr_pndgwrts > 0);

	if (s->slvr_flags & SLVR_REPLDST) {
		/* This was a replication dest slvr.  Adjust the slvr flags
		 *    so that the slvr may be freed on demand.
		 */
		DEBUG_SLVR(PLL_INFO, s, "replication complete");

		psc_assert(s->slvr_pndgwrts == 1);
		psc_assert(s->slvr_flags & SLVR_PINNED);
		psc_assert(s->slvr_flags & SLVR_FAULTING);
		psc_assert(!(s->slvr_flags & SLVR_REPLSRC));
		psc_assert(!(s->slvr_flags & SLVR_CRCDIRTY));
		s->slvr_pndgwrts--;
		s->slvr_flags &= ~(SLVR_PINNED|SLVR_FAULTING|SLVR_REPLDST);

		SLVR_ULOCK(s);

		slvr_lru_requeue(s, 0);
		return;
	}

	s->slvr_flags |= SLVR_CRCDIRTY;
	/* Manage the description of the dirty crc area. If the slvr's checksum
	 *   is not being processed then soff and len may be adjusted.
	 * If soff doesn't align with loff then the slvr will be
	 *   crc'd from offset 0.
	 */
	s->slvr_crc_soff = off;

	if ((off + len) > s->slvr_crc_eoff)
		s->slvr_crc_eoff =  off + len;

	if (off != s->slvr_crc_loff)
		s->slvr_crc_loff = 0;

	psc_assert(s->slvr_crc_eoff <= SLASH_BMAP_CRCSIZE);

	if (s->slvr_flags & SLVR_FAULTING) {
		/* This sliver was being paged-in over the network.
		 */
		psc_assert(!(s->slvr_flags & SLVR_DATARDY));
		psc_assert(!(s->slvr_flags & SLVR_REPLDST));

		s->slvr_flags |= SLVR_DATARDY;
		s->slvr_flags &= ~SLVR_FAULTING;

		DEBUG_SLVR(PLL_INFO, s, "FAULTING -> DATARDY");
		/* Other threads may be waiting for DATARDY to either
		 *   read or write to this sliver.  At this point it's
		 *   safe to wake them up.
		 * Note: when iterating over the lru list for
		 *   reclaiming, slvrs with pending writes must be
		 *   skipped.
		 */
		SLVR_WAKEUP(s);

	} else if (s->slvr_flags & SLVR_DATARDY) {

		DEBUG_SLVR(PLL_INFO, s, "%s", "datardy");

		if ((s->slvr_flags & SLVR_LRU) && s->slvr_pndgwrts > 1)
			slvr_lru_requeue(s, 1);
	} else
		DEBUG_SLVR(PLL_FATAL, s, "invalid state");

	/* If there are no more pending writes, schedule a CRC op.
	 *   Increment slvr_compwrts to prevent a crc op from being skipped
	 *   which can happen due to the release of the slvr lock being
	 *   released prior to the crc of the buffer.
	 */
	s->slvr_pndgwrts--;
	s->slvr_compwrts++;

	if (!s->slvr_pndgwrts && (s->slvr_flags & SLVR_LRU))
		slvr_schedule_crc_locked(s);

	SLVR_ULOCK(s);
}

/*
 * Lookup or create a sliver reference, ignoring one that is being freed.
 */
struct slvr_ref *
slvr_lookup(uint32_t num, struct bmap_iod_info *b, enum rw rw)
{
	struct slvr_ref *s, ts;

	ts.slvr_num = num;
 retry:
	spinlock(&b->biod_lock);

	s = SPLAY_FIND(biod_slvrtree, &b->biod_slvrs, &ts);
	/* Note, slvr lock and biod lock are the same.
	 */
	if (s && (s->slvr_flags & SLVR_FREEING)) {
		freelock(&b->biod_lock);
		goto retry;

	} else if (!s) {
		s = PSCALLOC(sizeof(*s));

		s->slvr_num = num;
		s->slvr_flags = SLVR_NEW | SLVR_SPLAYTREE;
		s->slvr_pri = b;
		s->slvr_slab = NULL;
		INIT_PSCLIST_ENTRY(&s->slvr_lentry);

		SPLAY_INSERT(biod_slvrtree, &b->biod_slvrs, s);
		bmap_op_start_type(bii_2_bmap(b), BMAP_OPCNT_SLVR);
	}

	s->slvr_flags |= SLVR_PINNED;

	if (rw == SL_WRITE)
		s->slvr_pndgwrts++;
	else if (rw == SL_READ)
		s->slvr_pndgreads++;
	else
		abort();

	freelock(&b->biod_lock);

	return (s);
}

__static void
slvr_remove(struct slvr_ref *s)
{
	struct bmap_iod_info	*b;
	int                      locked;

	DEBUG_SLVR(PLL_DEBUG, s, "freeing slvr");
	/* Slvr should be detached from any listheads.
	 */
	psc_assert(psclist_disjoint(&s->slvr_lentry));

	b = slvr_2_biod(s);
	locked = reqlock(&b->biod_lock);
	SPLAY_REMOVE(biod_slvrtree, &b->biod_slvrs, s);
	ureqlock(&b->biod_lock, locked);

	bmap_op_done_type(bii_2_bmap(b), BMAP_OPCNT_SLVR);

	PSCFREE(s);
}

/*
 * The reclaim function for slBufsPool.  Note that our
 *   caller psc_pool_get() ensures that we are called exclusviely.
 */
int
slvr_buffer_reap(struct psc_poolmgr *m)
{
	int			 i;
	int			 n;
	int                      locked;
	struct psc_dynarray	 a;
	struct slvr_ref		*s;
	struct slvr_ref		*dummy;

	n = 0;
	psc_dynarray_init(&a);
	LIST_CACHE_LOCK(&lruSlvrs);
	LIST_CACHE_FOREACH_SAFE(s, dummy, &lruSlvrs) {
		DEBUG_SLVR(PLL_INFO, s, "considering for reap, nwaiters=%d",
			   atomic_read(&m->ppm_nwaiters));

		/* We are reaping, so it is fine to back off on some
		 *   slivers.  We have to use a reqlock here because
		 *   slivers do not have private spinlocks, instead
		 *   they use the lock of the biod.  So if this thread
		 *   tries to free a slvr from the same biod trylock
		 *   will abort.
		 */
		if (!SLVR_TRYREQLOCK(s, &locked))
			continue;

		/* Look for slvrs which can be freed, slvr_lru_freeable()
		 *   returning true means that no slab is attached.
		 */
		if (slvr_lru_freeable(s)) {
			psc_dynarray_add(&a, s);
			s->slvr_flags |= SLVR_FREEING;
			lc_remove(&lruSlvrs, s);
			goto next;
		}

		if (slvr_lru_slab_freeable(s)) {
			/* At this point we know that the slab can be
			 *   reclaimed, however the slvr itself may
			 *   have to stay.
			 */
			psc_dynarray_add(&a, s);
			s->slvr_flags |= SLVR_SLBFREEING;
			n++;
		}
	next:
		SLVR_URLOCK(s, locked);
		if (n >= atomic_read(&m->ppm_nwaiters))
			break;
	}
	LIST_CACHE_ULOCK(&lruSlvrs);

	for (i = 0; i < psc_dynarray_len(&a); i++) {
		s = psc_dynarray_getpos(&a, i);

		locked = SLVR_RLOCK(s);

		if (s->slvr_flags & SLVR_SLBFREEING) {
			struct sl_buffer *tmp=s->slvr_slab;

			psc_assert(!(s->slvr_flags & SLVR_FREEING));
			psc_assert(s->slvr_slab);

			s->slvr_flags &= ~(SLVR_SLBFREEING|SLVR_DATARDY);

			DEBUG_SLVR(PLL_DEBUG, s, "freeing slvr slab=%p",
				   s->slvr_slab);
			s->slvr_slab = NULL;
			SLVR_WAKEUP(s);
			SLVR_URLOCK(s, locked);

			psc_pool_return(m, tmp);

		} else if (s->slvr_flags & SLVR_FREEING) {

			psc_assert(!(s->slvr_flags & SLVR_SLBFREEING));
			psc_assert(!(s->slvr_flags & SLVR_PINNED));
			psc_assert(!s->slvr_slab);
			if (s->slvr_flags & SLVR_SPLAYTREE) {
				s->slvr_flags &= ~SLVR_SPLAYTREE;
				SLVR_URLOCK(s, locked);
				slvr_remove(s);
			} else
				SLVR_URLOCK(s, locked);
		}
	}
	psc_dynarray_free(&a);

	return (n);
}

void
slvr_cache_init(void)
{
	lc_reginit(&lruSlvrs,  struct slvr_ref, slvr_lentry, "lruSlvrs");
	lc_reginit(&crcqSlvrs,  struct slvr_ref, slvr_lentry, "crcqSlvrs");

	sl_buffer_cache_init();

	slvr_worker_init();
}
