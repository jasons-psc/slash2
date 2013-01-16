/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
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

/*
 * This file contains definitions for operations on slivers.  Slivers
 * are 1MB sections of bmaps.
 */

#define PSC_SUBSYS SLISS_SLVR
#include "subsys_iod.h"

#include "psc_ds/listcache.h"
#include "psc_ds/treeutil.h"
#include "psc_ds/vbitmap.h"
#include "psc_rpc/rpc.h"
#include "psc_rpc/rsx.h"
#include "psc_util/atomic.h"
#include "psc_util/ctlsvr.h"
#include "psc_util/lock.h"
#include "psc_util/pthrutil.h"

#include "bmap_iod.h"
#include "buffer.h"
#include "fidc_iod.h"
#include "rpc_iod.h"
#include "slerr.h"
#include "sltypes.h"
#include "slvr.h"

struct psc_poolmaster	 slvr_poolmaster;
struct psc_poolmaster	 sli_aiocbr_poolmaster;
struct psc_poolmaster	 sli_iocb_poolmaster;

struct psc_poolmgr	*slvr_pool;
struct psc_poolmgr	*sli_aiocbr_pool;
struct psc_poolmgr	*sli_iocb_pool;

struct psc_listcache	 sli_iocb_pndg;

psc_atomic64_t		 sli_aio_id = PSC_ATOMIC64_INIT(0);

struct psc_listcache	 lruSlvrs;   /* LRU list of clean slivers which may be reaped */
struct psc_listcache	 crcqSlvrs;  /* Slivers ready to be CRC'd and have their
				      * CRCs shipped to the MDS. */

__static SPLAY_GENERATE(biod_slvrtree, slvr_ref, slvr_tentry, slvr_cmp);

__static void
slvr_lru_requeue(struct slvr_ref *s, int tail)
{
	/*
	 * Locking convention: it is legal to request for a list lock
	 * while holding the sliver lock.  On the other hand, when you
	 * already hold the list lock, you should drop the list lock
	 * first before asking for the sliver lock or you should use
	 * trylock().
	 */
	LIST_CACHE_LOCK(&lruSlvrs);
	if (tail)
		lc_move2tail(&lruSlvrs, s);
	else
		lc_move2head(&lruSlvrs, s);
	LIST_CACHE_ULOCK(&lruSlvrs);
}

/**
 * slvr_do_crc - Given a sliver reference, Take the CRC of the
 *	respective data and attach the ref to an srm_bmap_crcup
	structure.
 * @s: the sliver reference.
 * Notes:  Don't hold the lock while taking the CRC.
 * Returns: errno on failure, 0 on success, -1 on not applicable.
 */
int
slvr_do_crc(struct slvr_ref *s)
{
	uint64_t crc;

	SLVR_LOCK(s);
	psc_assert((s->slvr_flags & SLVR_PINNED) &&
		   (s->slvr_flags & SLVR_FAULTING ||
		    s->slvr_flags & SLVR_CRCDIRTY));

	if (s->slvr_flags & SLVR_FAULTING) {
		if (slvr_2_crcbits(s) & BMAP_SLVR_CRCABSENT) {
			SLVR_ULOCK(s);
			return (SLERR_CRCABSENT);
		}
		/*
		 * SLVR_FAULTING implies that we're bringing this data
		 * buffer in from the filesystem.
		 */
		if (!s->slvr_pndgreads && !(s->slvr_flags & SLVR_REPLDST)) {
			/*
			 * Small RMW workaround.
			 *  XXX needs to be rectified, the CRC should
			 *    be taken here.
			 */
			psc_assert(s->slvr_pndgwrts);
			SLVR_ULOCK(s);
			return (-1);
		}

		/*
		 * This thread holds faulting status so all others are
		 *  waiting on us which means that exclusive access to
		 *  slvr contents is ours until we set SLVR_DATARDY.
		 */
		psc_assert(!(s->slvr_flags & SLVR_DATARDY));

		if (!(s->slvr_flags & SLVR_REPLDST))
			/*
			 *  For now we assume that all blocks are being
			 *  processed, otherwise there's no guarantee
			 *  that the entire slvr was read.
			 */
			psc_assert(!psc_vbitmap_nfree(s->slvr_slab->slb_inuse));

		if ((slvr_2_crcbits(s) & BMAP_SLVR_DATA) &&
		    (slvr_2_crcbits(s) & BMAP_SLVR_CRC)) {
			SLVR_ULOCK(s);

			psc_crc64_calc(&crc, slvr_2_buf(s, 0),
			    SLASH_SLVR_SIZE);

			if (crc != slvr_2_crc(s)) {
				DEBUG_SLVR(PLL_ERROR, s, "CRC failed "
				    "want=%"PSCPRIxCRC64" "
				    "got=%"PSCPRIxCRC64, slvr_2_crc(s), crc);

				DEBUG_BMAP(PLL_ERROR, slvr_2_bmap(s),
				   "CRC failed slvrnum=%hu", s->slvr_num);

				return (SLERR_BADCRC);
			}

		} else {
			SLVR_ULOCK(s);
			return (0);
		}

	} else if (s->slvr_flags & SLVR_CRCDIRTY) {
		/*
		 * SLVR_CRCDIRTY means that DATARDY has been set and
		 * that a write dirtied the buffer and invalidated the
		 * CRC.
		 */
		DEBUG_SLVR(PLL_DIAG, s, "crc");
		PSC_CRC64_INIT(&s->slvr_crc);
		SLVR_ULOCK(s);

#ifdef ADLERCRC32
		// XXX not a running CRC?  double check for correctness
		s->slvr_crc = adler32(s->slvr_crc, slvr_2_buf(s, 0) +
		    soff, (int)(eoff - soff));
		crc = s->slvr_crc;
#else
		psc_crc64_add(&s->slvr_crc, slvr_2_buf(s, 0),
		    SLASH_SLVR_SIZE);

		crc = s->slvr_crc;
		PSC_CRC64_FIN(&crc);
#endif

		DEBUG_SLVR(PLL_DIAG, s, "crc=%"PSCPRIxCRC64, crc);

		SLVR_LOCK(s);
		if (!s->slvr_pndgwrts && !s->slvr_compwrts)
			s->slvr_flags &= ~SLVR_CRCDIRTY;
		slvr_2_crc(s) = crc;
		slvr_2_crcbits(s) |= BMAP_SLVR_DATA | BMAP_SLVR_CRC;
		SLVR_ULOCK(s);
	} else {
		psc_fatal("FAULTING or CRCDIRTY is not set");
	}

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

__static struct sli_aiocb_reply *
sli_aio_aiocbr_new(void)
{
	struct sli_aiocb_reply *a;

	a = psc_pool_get(sli_aiocbr_pool);
	memset(a, 0, sizeof(*a));

	INIT_SPINLOCK(&a->aiocbr_lock);
	INIT_PSC_LISTENTRY(&a->aiocbr_lentry);

	return (a);
}

void
sli_aio_aiocbr_release(struct sli_aiocb_reply *a)
{
	psc_assert(psclist_disjoint(&a->aiocbr_lentry));

	psc_pool_return(sli_aiocbr_pool, a);
}

__static int
slvr_aio_chkslvrs(const struct sli_aiocb_reply *a)
{
	struct slvr_ref *s;
	int i, rc = 0;

	for (i = 0; i < a->aiocbr_nslvrs; i++) {
		s = a->aiocbr_slvrs[i];
		SLVR_LOCK(s);
		psc_assert(s->slvr_flags &
		    (SLVR_DATARDY | SLVR_DATAERR));
		if (s->slvr_flags & SLVR_DATAERR)
			rc = s->slvr_err;
		SLVR_ULOCK(s);
		if (rc)
			return (rc);
	}
	return (rc);
}

__static void
slvr_aio_replreply(struct sli_aiocb_reply *a)
{
	struct pscrpc_request *rq = NULL;
	struct srm_repl_read_req *mq;
	struct srm_repl_read_rep *mp;
	struct slvr_ref *s = NULL;
	int rc;

	psc_assert(a->aiocbr_nslvrs == 1);

	if (!a->aiocbr_csvc)
		goto out;

	if (SL_RSX_NEWREQ(a->aiocbr_csvc, SRMT_REPL_READAIO, rq, mq,
	    mp))
		goto out;

	OPSTAT_INCR(SLI_OPST_REPL_READAIO);

	s = a->aiocbr_slvrs[0];

	mq->rc = slvr_aio_chkslvrs(a);
	mq->fg = slvr_2_fcmh(s)->fcmh_fg;
	mq->len = a->aiocbr_len;
	mq->bmapno = slvr_2_bmap(s)->bcm_bmapno;
	mq->slvrno = s->slvr_num;
	if (mq->rc)
		pscrpc_msg_add_flags(rq->rq_reqmsg, MSG_ABORT_BULK);
	else
		mq->rc = rsx_bulkclient(rq, BULK_GET_SOURCE,
		    SRII_BULK_PORTAL, a->aiocbr_iovs, a->aiocbr_niov);

	rc = SL_RSX_WAITREP(a->aiocbr_csvc, rq, mp);
	pscrpc_req_finished(rq);

	if (rc)
		DEBUG_SLVR(PLL_ERROR, s, "rc=%d", rc);

 out:
	if (s)
		slvr_rio_done(s);

	if (a->aiocbr_csvc)
		sl_csvc_decref(a->aiocbr_csvc);

	sli_aio_aiocbr_release(a);
}

__static void
slvr_aio_reply(struct sli_aiocb_reply *a)
{
	struct pscrpc_request *rq = NULL;
	struct srm_io_req *mq;
	struct srm_io_rep *mp;
	int rc, i;

	OPSTAT_INCR(SLI_OPST_SLVR_AIO_REPLY);

	if (!a->aiocbr_csvc)
		goto out;

	rc = SL_RSX_NEWREQ(a->aiocbr_csvc, a->aiocbr_rw == SL_WRITE ?
	    SRMT_WRITE : SRMT_READ, rq, mq, mp);
	if (rc)
		goto out;

	mq->rc = slvr_aio_chkslvrs(a);

	memcpy(&mq->sbd, &a->aiocbr_sbd, sizeof(mq->sbd));
	mq->id = a->aiocbr_id;
	mq->size = a->aiocbr_len;
	mq->offset = a->aiocbr_off;
	if (a->aiocbr_rw == SL_WRITE)
		/* Notify the client that he may resubmit the write.
		 */
		mq->op = SRMIOP_WR;
	else {
		mq->op = SRMIOP_RD;
		if (mq->rc)
			pscrpc_msg_add_flags(rq->rq_reqmsg,
			    MSG_ABORT_BULK);
		else
			mq->rc = rsx_bulkclient(rq, BULK_GET_SOURCE,
			    SRCI_BULK_PORTAL, a->aiocbr_iovs,
			    a->aiocbr_niov);
	}

	rc = SL_RSX_WAITREP(a->aiocbr_csvc, rq, mp);
	pscrpc_req_finished(rq);

 out:
	sl_csvc_decref(a->aiocbr_csvc);

	if (a->aiocbr_rw == SL_READ) {
		for (i = 0; i < a->aiocbr_nslvrs; i++)
			slvr_rio_done(a->aiocbr_slvrs[i]);
	} else {
		struct slvr_ref *s;
		for (i = 0; i < a->aiocbr_nslvrs; i++) {
			s = a->aiocbr_slvrs[i];

			SLVR_LOCK(s);
			psc_assert(s->slvr_pndgwrts > 0);
			s->slvr_flags &= ~SLVR_RDMODWR;
			slvr_try_crcsched_locked(s);
			SLVR_ULOCK(s);
		}
	}

	sli_aio_aiocbr_release(a);
}

__static void
slvr_aio_tryreply(struct sli_aiocb_reply *a)
{
	struct slvr_ref *s;
	int i, ready, replsrc = 0;

 retry:
	spinlock(&a->aiocbr_lock);
	while (!(a->aiocbr_flags & SLI_AIOCBSF_READY)) {
		freelock(&a->aiocbr_lock);
		sched_yield();
		goto retry;
	}

	for (ready = 0, i = 0; i < a->aiocbr_nslvrs; i++) {
		s = a->aiocbr_slvrs[i];
		SLVR_LOCK(s);

		if (a->aiocbr_flags & SLI_AIOCBSF_REPL) {
			psc_assert(a->aiocbr_nslvrs == 1);
			replsrc = 1;
		}

		/*
		 * FixMe: What if the sliver get reused for another purpose?
		 */
		if (s->slvr_flags & (SLVR_DATARDY | SLVR_DATAERR))
			ready++;

		else if (s->slvr_flags & SLVR_AIOWAIT) {
			/* One of our slvrs is still waiting on aio
			 *    completion.  Add this reply to that slvr.
			 */
			a->aiocbr_slvratt = s;
			pll_add(&s->slvr_pndgaios, a);
			SLVR_ULOCK(s);
			break;
		}
		SLVR_ULOCK(s);
	}
	freelock(&a->aiocbr_lock);

	if (ready == a->aiocbr_nslvrs)
		replsrc ? slvr_aio_replreply(a) : slvr_aio_reply(a);
}

/**
 * slvr_aio_process - Given a slvr, scan the list of pndg aio's for those
 *   ready for completion.
 */
void
slvr_aio_process(struct slvr_ref *s)
{
	struct sli_aiocb_reply *a;

	SLVR_LOCK(s);
	psc_assert(s->slvr_flags & (SLVR_DATARDY | SLVR_DATAERR));
	psc_assert(!(s->slvr_flags & SLVR_AIOWAIT));
	SLVR_ULOCK(s);

	while ((a = pll_get(&s->slvr_pndgaios)))
		slvr_aio_tryreply(a);
}

__static void
slvr_iocb_release(struct sli_iocb *iocb)
{
	OPSTAT_INCR(SLI_OPST_IOCB_FREE);
	psc_pool_return(sli_iocb_pool, iocb);
}

__static void
slvr_fsaio_done(struct sli_iocb *iocb)
{
	struct slvr_ref *s;
	int rc;

	s = iocb->iocb_slvr;
	rc = iocb->iocb_rc;

	SLVR_LOCK(s);
	psc_assert(iocb == s->slvr_iocb);
	psc_assert(s->slvr_flags & SLVR_FAULTING);
	psc_assert(s->slvr_flags & SLVR_AIOWAIT);
	psc_assert(!(s->slvr_flags & (SLVR_DATARDY | SLVR_DATAERR)));
	if (s->slvr_flags & SLVR_RDMODWR)
		psc_assert(s->slvr_pndgwrts > 0);
	else
		psc_assert(s->slvr_pndgreads > 0);

	/* Prevent additions from new requests. */
	s->slvr_flags &= ~(SLVR_AIOWAIT | SLVR_FAULTING);

	slvr_iocb_release(s->slvr_iocb);
	s->slvr_iocb = NULL;

	if (rc) {
		/*
		 * There was a problem; unblock any waiters and
		 * tell them the bad news.
		 */
		s->slvr_flags |= SLVR_DATAERR;
		DEBUG_SLVR(PLL_ERROR, s, "error, rc=%d", rc);
		s->slvr_err = rc;
	} else {
		s->slvr_flags |= SLVR_DATARDY;
		DEBUG_SLVR(PLL_INFO, s, "FAULTING -> DATARDY");
	}
	psc_vbitmap_invert(s->slvr_slab->slb_inuse);
	SLVR_WAKEUP(s);
	SLVR_ULOCK(s);

	slvr_aio_process(s);
}

__static struct sli_iocb *
sli_aio_iocb_new(struct slvr_ref *s)
{
	struct sli_iocb *iocb;

	OPSTAT_INCR(SLI_OPST_IOCB_GET);
	iocb = psc_pool_get(sli_iocb_pool);
	memset(iocb, 0, sizeof(*iocb));
	INIT_LISTENTRY(&iocb->iocb_lentry);
	iocb->iocb_slvr = s;
	iocb->iocb_cbf = slvr_fsaio_done;

	return (iocb);
}

void
sli_aio_replreply_setup(struct sli_aiocb_reply *a,
    struct pscrpc_request *rq, struct slvr_ref *s, struct iovec *iov)
{
	//const struct srm_repl_read_req *mq;
	struct srm_repl_read_rep *mp;

	mp = pscrpc_msg_buf(rq->rq_repmsg, 0, sizeof(*mp));
	mp->id = a->aiocbr_id = psc_atomic64_inc_getnew(&sli_aio_id);

	memcpy(a->aiocbr_iovs, iov, sizeof(*iov));
	a->aiocbr_slvrs[0] = s;
	a->aiocbr_nslvrs = a->aiocbr_niov = 1;
	a->aiocbr_len = iov->iov_len;
	a->aiocbr_off = 0;

	/* Ref taken here must persist until reply is attempted.
	 */
	a->aiocbr_csvc = sli_geticsvcx(libsl_try_nid2resm(
	    rq->rq_peer.nid), rq->rq_export);

	spinlock(&a->aiocbr_lock);
	a->aiocbr_flags |= SLI_AIOCBSF_REPL | SLI_AIOCBSF_READY;
	freelock(&a->aiocbr_lock);
}

void
sli_aio_reply_setup(struct sli_aiocb_reply *a, struct pscrpc_request *rq,
    uint32_t len, uint32_t off, struct slvr_ref **slvrs, int nslvrs,
    struct iovec *iovs, int niovs, enum rw rw)
{
	struct srm_io_req *mq;
	struct srm_io_rep *mp;
	int i;

	spinlock(&a->aiocbr_lock);
	a->aiocbr_nslvrs = nslvrs;
	for (i = 0; i < nslvrs; i++) {
		a->aiocbr_slvrs[i] = slvrs[i];

		if (rw == SL_WRITE)
			psc_assert(slvrs[i]->slvr_pndgwrts > 0);
		else
			psc_assert(slvrs[i]->slvr_pndgreads > 0);
	}
	freelock(&a->aiocbr_lock);

	psc_assert(niovs == a->aiocbr_nslvrs);

	mq = pscrpc_msg_buf(rq->rq_reqmsg, 0, sizeof(*mq));
	memcpy(&a->aiocbr_sbd, &mq->sbd, sizeof(mq->sbd));

	mp = pscrpc_msg_buf(rq->rq_repmsg, 0, sizeof(*mp));
	mp->id = a->aiocbr_id = psc_atomic64_inc_getnew(&sli_aio_id);

	memcpy(a->aiocbr_iovs, iovs, niovs * sizeof(*iovs));

	a->aiocbr_niov = niovs;
	a->aiocbr_len = len;
	a->aiocbr_off = off;
	a->aiocbr_rw = rw;
	a->aiocbr_csvc = sli_getclcsvc(rq->rq_export);

	spinlock(&a->aiocbr_lock);
	a->aiocbr_flags |= SLI_AIOCBSF_READY;
	freelock(&a->aiocbr_lock);
}

int
sli_aio_register(struct slvr_ref *s, struct sli_aiocb_reply **aiocbrp,
    int issue)
{
	struct sli_aiocb_reply *a;
	struct sli_iocb *iocb;
	struct aiocb *aio;
	int error = SLERR_AIOWAIT;

	a = *aiocbrp;

	if (!a)
		a = *aiocbrp = sli_aio_aiocbr_new();

	DEBUG_SLVR(PLL_DIAG, s, "issue=%d *aiocbrp=%p", issue,
	    *aiocbrp);

	if (!issue)
		/* Not called from slvr_fsio(). */
		goto out;

	iocb = sli_aio_iocb_new(s);

	SLVR_LOCK(s);
	psc_assert(!(s->slvr_flags & SLVR_DATARDY));
	psc_assert(s->slvr_flags & SLVR_AIOWAIT);
	psc_assert(!s->slvr_iocb);

	s->slvr_iocb = iocb;
	SLVR_ULOCK(s);

	aio = &iocb->iocb_aiocb;
	aio->aio_fildes = slvr_2_fd(s);

	/* Read the entire sliver. */
	aio->aio_offset = slvr_2_fileoff(s, 0);
	aio->aio_buf = slvr_2_buf(s, 0);
	aio->aio_nbytes = SLASH_SLVR_SIZE;

	aio->aio_sigevent.sigev_notify = SIGEV_SIGNAL;
	aio->aio_sigevent.sigev_signo = SIGIO;
	aio->aio_sigevent.sigev_value.sival_ptr = (void *)aio;

	lc_add(&sli_iocb_pndg, iocb);
	error = aio_read(aio);
	if (error == 0) {
		error = SLERR_AIOWAIT;
		psclog_info("aio_read: fd=%d iocb=%p sliver=%p",
		    aio->aio_fildes, iocb, s);
	} else {
		psclog_warn("aio_read: fd=%d iocb=%p sliver=%p error=%d",
		    aio->aio_fildes, iocb, s, error);
		lc_remove(&sli_iocb_pndg, iocb);
		slvr_iocb_release(iocb);
	}
 out:
	return (-error);
}

__static ssize_t
slvr_fsio(struct slvr_ref *s, int sblk, uint32_t size, enum rw rw,
    struct sli_aiocb_reply **aiocbr)
{
	int i, nblks, save_errno = 0;
	uint64_t *v8;
	ssize_t	rc;
	size_t off;

	psc_assert(rw == SL_READ || rw == SL_WRITE);

	nblks = (size + SLASH_SLVR_BLKSZ - 1) / SLASH_SLVR_BLKSZ;
	psc_assert(sblk + nblks <= SLASH_BLKS_PER_SLVR);

	SLVR_LOCK(s);
	psc_assert(s->slvr_flags & SLVR_PINNED);
	off = slvr_2_fileoff(s, sblk);

	if (rw == SL_READ) {
		OPSTAT_INCR(SLI_OPST_FSIO_READ);
		psc_assert(s->slvr_flags & SLVR_FAULTING);
		errno = 0;
		if (globalConfig.gconf_async_io) {
			s->slvr_flags |= SLVR_AIOWAIT;
			SLVR_WAKEUP(s);
			SLVR_ULOCK(s);

			return (sli_aio_register(s, aiocbr, 1));
		}
		SLVR_ULOCK(s);

		rc = pread(slvr_2_fd(s), slvr_2_buf(s, sblk), size,
		    off);

		if (OPSTAT_CURR(SLI_OPST_DEBUG) == 1) {
			rc = -1;
			errno = EBADF;
		}

		if (rc == -1) {
			save_errno = errno;
			OPSTAT_INCR(SLI_OPST_FSIO_READ_FAIL);
		}

		/*
		 * XXX this is a bit of a hack.  Here we'll check CRCs
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

			crc_rc = slvr_do_crc(s);
			if (crc_rc == SLERR_BADCRC)
				DEBUG_SLVR(PLL_ERROR, s,
				    "bad crc blks=%d off=%zu",
				    nblks, off);
		}
	} else {
		OPSTAT_INCR(SLI_OPST_FSIO_WRITE);
		for (i = 0; i < nblks; i++)
			psc_vbitmap_unset(s->slvr_slab->slb_inuse,
			    sblk + i);

		errno = 0;
		SLVR_ULOCK(s);

		rc = pwrite(slvr_2_fd(s), slvr_2_buf(s, sblk), size,
		    off);
		if (rc == -1) {
			save_errno = errno;
			OPSTAT_INCR(SLI_OPST_FSIO_WRITE_FAIL);
		}
	}

	if (rc < 0)
		DEBUG_SLVR(PLL_ERROR, s, "failed (rc=%zd, size=%u) "
		    "%s blks=%d off=%zu errno=%d",
		    rc, size, (rw == SL_WRITE ? "SL_WRITE" : "SL_READ"),
		    nblks, off, save_errno);

	else if ((uint32_t)rc != size) {
		DEBUG_SLVR(off + size > slvr_2_fcmh(s)->
		    fcmh_sstb.sst_size ? PLL_DIAG : PLL_NOTICE, s,
		    "short I/O (rc=%zd, size=%u) "
		    "%s blks=%d off=%zu errno=%d",
		    rc, size, (rw == SL_WRITE ? "SL_WRITE" : "SL_READ"),
		    nblks, off, save_errno);
	} else {
		v8 = slvr_2_buf(s, sblk);
		DEBUG_SLVR(PLL_INFO, s, "ok %s size=%u off=%zu"
		    " rc=%zd nblks=%d v8(%"PRIx64")",
		    (rw == SL_WRITE ? "SL_WRITE" : "SL_READ"),
		    size, off, rc, nblks, *v8);
		rc = 0;
	}

	//psc_vbitmap_printbin1(s->slvr_slab->slb_inuse);

	return (rc < 0 ? -save_errno : 0);
}

/**
 * slvr_fsbytes_get - Read in the blocks which have their respective
 *	bits set in slab bitmap, trying to coalesce where possible.
 * @s: the sliver.
 */
ssize_t
slvr_fsbytes_rio(struct slvr_ref *s, struct sli_aiocb_reply **aiocbr)
{
	int i, blk, nblks;
	ssize_t rc;

	psclog_debug("psc_vbitmap_nfree() = %d",
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
			    SL_READ, aiocbr);
			if (rc)
				goto out;

			/* reset nblks so we won't do it again later */
			nblks = 0;
		}
	}

	if (nblks)
		rc = slvr_fsio(s, blk, nblks * SLASH_SLVR_BLKSZ,
		    SL_READ, aiocbr);
 out:
	if (rc == -SLERR_AIOWAIT)
		return (rc);

	else if (rc) {
		/*
		 * There was a problem; unblock any waiters and tell
		 * them the bad news.
		 */
		SLVR_LOCK(s);
		s->slvr_flags |= SLVR_DATAERR;
		s->slvr_flags &= ~SLVR_FAULTING;
		DEBUG_SLVR(PLL_ERROR, s, "slvr_fsio() error, rc=%zd", rc);
		SLVR_WAKEUP(s);
		SLVR_ULOCK(s);
	}

	return (rc);
}

ssize_t
slvr_fsbytes_wio(struct slvr_ref *s, uint32_t size, uint32_t sblk)
{
	DEBUG_SLVR(PLL_INFO, s, "sblk=%u size=%u", sblk, size);

	return (slvr_fsio(s, sblk, size, SL_WRITE, NULL));
}

void
slvr_repl_prep(struct slvr_ref *s, int src_or_dst)
{
	SLVR_LOCK(s);

	if (src_or_dst & SLVR_REPLDST) {
		psc_assert(s->slvr_pndgwrts > 0);

		if (s->slvr_flags & SLVR_DATARDY) {
			/* The slvr is about to be overwritten by this
			 *    replication request. For sanity's sake, wait
			 *    for pending io competion and set 'faulting'
			 *    before proceeding.
			 */
			DEBUG_SLVR(PLL_WARN, s,
				   "mds requested repldst of active slvr");
			SLVR_WAIT(s, ((s->slvr_pndgwrts > 1) ||
				      s->slvr_pndgreads));
			s->slvr_flags &= ~SLVR_DATARDY;
		}

		s->slvr_flags |= (SLVR_FAULTING | src_or_dst);

	} else {
		psc_assert(s->slvr_pndgreads > 0);
	}

	DEBUG_SLVR(PLL_INFO, s, "replica_%s",
	    (src_or_dst & SLVR_REPLDST) ? "dst" : "src");

	SLVR_ULOCK(s);
}

void
slvr_slab_prep(struct slvr_ref *s, enum rw rw)
{
	struct sl_buffer *tmp = NULL;

	//XXX may have to lock bmap instead..
	SLVR_LOCK(s);

 restart:
	/*
	 * slvr_lookup() must pin all slvrs to avoid racing with the
	 * reaper.
	 */
	psc_assert(s->slvr_flags & SLVR_PINNED);

	if (rw == SL_WRITE)
		psc_assert(s->slvr_pndgwrts > 0);
	else
		psc_assert(s->slvr_pndgreads > 0);

 newbuf:
	if (s->slvr_flags & SLVR_NEW) {
		if (!tmp) {
			/*
			 * Drop the lock before potentially blocking in
			 * the pool reaper.  To do this we must first
			 * allocate to a tmp pointer.
			 */

 getbuf:
			SLVR_ULOCK(s);

			tmp = psc_pool_get(sl_bufs_pool);
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
		/*
		 * Until the slab is added to the sliver, the sliver is
		 * private to the bmap's biod_slvrtree.
		 */
		s->slvr_flags |= SLVR_LRU;
		/* note: lc_addtail() will grab the list lock itself */
		lc_addtail(&lruSlvrs, s);

	} else if ((s->slvr_flags & SLVR_LRU) && !s->slvr_slab) {
		psc_assert(!(s->slvr_flags & SLVR_DATARDY));
		if (!tmp)
			goto getbuf;
		s->slvr_slab = tmp;
		tmp = NULL;

	} else if (s->slvr_flags & SLVR_SLBFREEING) {
		DEBUG_SLVR(PLL_INFO, s, "caught slbfreeing");
		SLVR_WAIT(s, (s->slvr_flags & SLVR_SLBFREEING));
		goto restart;
	}

	DEBUG_SLVR(PLL_INFO, s, "should have slab");
	psc_assert(s->slvr_slab);
	SLVR_ULOCK(s);

	if (tmp)
		psc_pool_return(sl_bufs_pool, tmp);
}

/**
 * slvr_io_prep - Prepare a sliver for an incoming I/O.  This may entail
 *   faulting 32k aligned regions in from the underlying fs.
 * @s: the sliver
 * @off: offset into the slvr (not bmap or file object)
 * @len: len relative to the slvr
 * @rw: read or write op
 */
ssize_t
slvr_io_prep(struct slvr_ref *s, uint32_t off, uint32_t len, enum rw rw,
    struct sli_aiocb_reply **aiocbr)
{
	int i, blks, unaligned[2] = { -1, -1 };
	ssize_t rc = 0;

	SLVR_LOCK(s);
	psc_assert(s->slvr_flags & SLVR_PINNED);

	/*
	 * Note we have taken our read or write references, so the
	 * sliver won't be freed from under us.
	 */
	if (s->slvr_flags & SLVR_FAULTING &&
	    !(s->slvr_flags & SLVR_REPLDST)) {
		/*
		 * Common courtesy requires us to wait for another
		 * thread's work FIRST.  Otherwise, we could bail out
		 * prematurely when the data is ready without
		 * considering the range we want to write.
		 */
		psc_assert(!(s->slvr_flags & SLVR_DATARDY));

		SLVR_WAIT(s, !(s->slvr_flags &
		    (SLVR_DATARDY | SLVR_DATAERR | SLVR_AIOWAIT)));

		psc_assert((s->slvr_flags &
		    (SLVR_DATARDY | SLVR_DATAERR | SLVR_AIOWAIT)));

		if (s->slvr_flags & SLVR_AIOWAIT) {
			SLVR_ULOCK(s);
			psc_assert(globalConfig.gconf_async_io);

			return (sli_aio_register(s, aiocbr, 0));
		}
	}

	DEBUG_SLVR(s->slvr_flags & SLVR_DATAERR ?
	    PLL_ERROR : PLL_INFO, s,
	    "slvrno=%hu off=%u len=%u rw=%d",
	    s->slvr_num, off, len, rw);

	if (s->slvr_flags & SLVR_DATAERR) {
		rc = -1;
		goto out;

	} else if (s->slvr_flags & SLVR_DATARDY) {
		if (rw == SL_READ)
			goto out;

	} else if (!(s->slvr_flags & SLVR_REPLDST)) {
		/*
		 * Importing data into the sliver is now our
		 * responsibility, other I/O into this region will block
		 * until SLVR_FAULTING is released.
		 */
		s->slvr_flags |= SLVR_FAULTING;
		if (rw == SL_READ) {
			psc_vbitmap_setall(s->slvr_slab->slb_inuse);
			goto do_read;
		}

	} else if (s->slvr_flags & SLVR_REPLDST) {
		/*
		 * The sliver is going to be used for replication.
		 * Ensure proper setup has occurred.
		 */
		psc_assert(!off);
		psc_assert(!(s->slvr_flags & SLVR_DATARDY));
		psc_assert(s->slvr_flags & SLVR_FAULTING);
		psc_assert(s->slvr_pndgreads == 0 &&
		    s->slvr_pndgwrts == 1);

		blks = len / SLASH_SLVR_BLKSZ +
		    (len & SLASH_SLVR_BLKMASK) ? 1 : 0;

		psc_vbitmap_setrange(s->slvr_slab->slb_inuse, 0, blks);
		SLVR_ULOCK(s);

		return (0);
	}

	psc_assert(rw != SL_READ);

	if (!off && len == SLASH_SLVR_SIZE) {
		/* Full sliver write, no need to read blocks from disk.
		 *  All blocks will be dirtied by the incoming network IO.
		 */
		psc_vbitmap_setall(s->slvr_slab->slb_inuse);
		goto out;
	}

	/* FixMe: Check the underlying file size to avoid useless RMW */
	OPSTAT_INCR(SLI_OPST_IO_PREP_RMW);

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

		/* XXX use psc_vbitmap_setrange() */
		for (i = blks; i < SLASH_BLKS_PER_SLVR; i++)
			psc_vbitmap_set(s->slvr_slab->slb_inuse, i);
	}

	//psc_vbitmap_printbin1(s->slvr_slab->slb_inuse);
	psclog_info("psc_vbitmap_nfree()=%d",
	    psc_vbitmap_nfree(s->slvr_slab->slb_inuse));

	/* We must have found some work to do. */
	psc_assert(psc_vbitmap_nfree(s->slvr_slab->slb_inuse) <
		   SLASH_BLKS_PER_SLVR);

	if (s->slvr_flags & SLVR_DATARDY)
		goto invert;

	s->slvr_flags |= SLVR_RDMODWR;

 do_read:
	SLVR_ULOCK(s);
	/* Execute read to fault in needed blocks after dropping
	 *   the lock.  All should be protected by the FAULTING bit.
	 */
	if ((rc = slvr_fsbytes_rio(s, aiocbr)))
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
	}

	/*
	 * Above, the bits were set for the RMW blocks, now that they
	 * have been read, invert the bitmap so that it properly
	 * represents the blocks to be dirtied by the RPC.
	 */
	SLVR_LOCK(s);

 invert:
	psc_vbitmap_invert(s->slvr_slab->slb_inuse);
	if (unaligned[0] >= 0)
		psc_vbitmap_set(s->slvr_slab->slb_inuse, unaligned[0]);

	if (unaligned[1] >= 0)
		psc_vbitmap_set(s->slvr_slab->slb_inuse, unaligned[1]);

//	psc_vbitmap_printbin1(s->slvr_slab->slb_inuse);

 out:
	if (!rc && s->slvr_flags & SLVR_FAULTING) {
		s->slvr_flags |= SLVR_DATARDY;
		s->slvr_flags &= ~SLVR_FAULTING;
		DEBUG_SLVR(PLL_INFO, s, "FAULTING -> DATARDY");
		SLVR_WAKEUP(s);
	}
	SLVR_ULOCK(s);
	return (rc);
}

void
slvr_rio_done(struct slvr_ref *s)
{
	SLVR_LOCK(s);

	s->slvr_pndgreads--;
	if (slvr_lru_tryunpin_locked(s)) {
		slvr_lru_requeue(s, 1);
		DEBUG_SLVR(PLL_INFO, s, "decref, unpinned");
	} else
		DEBUG_SLVR(PLL_INFO, s, "decref, ops still pending or dirty");

	SLVR_ULOCK(s);
}

__static void
slvr_schedule_crc_locked(struct slvr_ref *s)
{
	psc_assert(s->slvr_flags & SLVR_PINNED);
	psc_assert(s->slvr_flags & SLVR_CRCDIRTY);
	psc_assert(s->slvr_flags & SLVR_LRU);

	if (!s->slvr_dirty_cnt) {
		psc_atomic32_inc(&slvr_2_bii(s)->bii_crcdrty_slvrs);
		s->slvr_dirty_cnt++;
	}

	DEBUG_SLVR(PLL_INFO, s, "crc sched (ndirty slvrs=%u)",
	    psc_atomic32_read(&slvr_2_bii(s)->bii_crcdrty_slvrs));

	s->slvr_flags &= ~SLVR_LRU;

	lc_remove(&lruSlvrs, s);
	lc_addqueue(&crcqSlvrs, s);
}

void slvr_slb_free_locked(struct slvr_ref *, struct psc_poolmgr *);

void
slvr_try_crcsched_locked(struct slvr_ref *s)
{
	if ((s->slvr_flags & SLVR_LRU) && s->slvr_pndgwrts > 1)
		slvr_lru_requeue(s, 1);

	/*
	 * If there are no more pending writes, schedule a CRC op.
	 * Increment slvr_compwrts to prevent a CRC op from being
	 * skipped which can happen due to the release of the slvr lock
	 * being released prior to the CRC of the buffer.
	 */
	s->slvr_pndgwrts--;
	s->slvr_compwrts++;

	DEBUG_SLVR(PLL_DEBUG, s, "decref");

	if (!s->slvr_pndgwrts && (s->slvr_flags & SLVR_LRU)) {
		if (s->slvr_flags & SLVR_CRCDIRTY)
			slvr_schedule_crc_locked(s);
		else
			slvr_lru_tryunpin_locked(s);
	}
}

int
slvr_lru_tryunpin_locked(struct slvr_ref *s)
{
	SLVR_LOCK_ENSURE(s);
	psc_assert(s->slvr_slab);
	if (s->slvr_pndgwrts || s->slvr_pndgreads ||
	    s->slvr_flags & SLVR_CRCDIRTY || s->slvr_flags & SLVR_CRCING)
		return (0);

	psc_assert(s->slvr_flags & SLVR_LRU);
	psc_assert(s->slvr_flags & SLVR_PINNED);

	psc_assert(s->slvr_flags & (SLVR_DATARDY | SLVR_DATAERR));
	psc_assert(!(s->slvr_flags & (SLVR_NEW | SLVR_GETSLAB)));

	if ((s->slvr_flags & SLVR_REPLDST) == 0)
		psc_assert(!(s->slvr_flags & SLVR_FAULTING));

	s->slvr_flags &= ~SLVR_PINNED;

	if (s->slvr_flags & (SLVR_DATAERR | SLVR_REPLFAIL)) {
		s->slvr_flags |= SLVR_SLBFREEING;
		slvr_slb_free_locked(s, sl_bufs_pool);
	}

	return (1);
}

/**
 * slvr_wio_done - Called after a write RPC has completed.  The sliver
 *	may be FAULTING which is handled separately from DATARDY.  If
 *	FAULTING, this thread must wake up sleepers on the bmap waitq.
 * Notes: conforming with standard lock ordering, this routine drops
 *    the sliver lock prior to performing list operations.
 */
void
slvr_wio_done(struct slvr_ref *s)
{
	SLVR_LOCK(s);
	psc_assert(s->slvr_flags & SLVR_PINNED);
	psc_assert(s->slvr_pndgwrts > 0);

	if (s->slvr_flags & SLVR_REPLDST) {
		/*
		 * This was a replication dest slvr.  Adjust the slvr
		 * flags so that the slvr may be freed on demand.
		 */
		if (s->slvr_flags & SLVR_REPLFAIL)
			DEBUG_SLVR(PLL_ERROR, s, "replication failure");
		else
			DEBUG_SLVR(PLL_INFO, s, "replication complete");

		psc_assert(s->slvr_pndgwrts == 1);
		psc_assert(s->slvr_flags & SLVR_PINNED);
		psc_assert(s->slvr_flags & SLVR_FAULTING);
		psc_assert(!(s->slvr_flags & SLVR_CRCDIRTY));
		s->slvr_pndgwrts--;
		s->slvr_flags &= ~(SLVR_PINNED | SLVR_AIOWAIT |
		    SLVR_FAULTING | SLVR_REPLDST);

		if (s->slvr_flags & SLVR_REPLFAIL) {
			/*
			 * Perhaps this should block for any readers?
			 * Technically it should be impossible since
			 * this replica has yet to be registered with
			 * the MDS.
			 */
			s->slvr_flags |= SLVR_SLBFREEING;
			slvr_slb_free_locked(s, sl_bufs_pool);
			s->slvr_flags &= ~SLVR_REPLFAIL;

		} else {
			s->slvr_flags |= SLVR_DATARDY;
			SLVR_WAKEUP(s);
		}

		DEBUG_SLVR(PLL_MAX, s, "decref");

		slvr_lru_requeue(s, 0);
		SLVR_ULOCK(s);

		return;
	}

	PFL_GETTIMESPEC(&s->slvr_ts);

	s->slvr_flags |= SLVR_CRCDIRTY;
	s->slvr_flags &= ~SLVR_RDMODWR;

	if (!(s->slvr_flags & SLVR_DATARDY))
		DEBUG_SLVR(PLL_FATAL, s, "invalid state");

	slvr_try_crcsched_locked(s);
	SLVR_ULOCK(s);
}

/**
 * slvr_lookup - Lookup or create a sliver reference, ignoring one that
 *	is being freed.
 */
struct slvr_ref *
_slvr_lookup(const struct pfl_callerinfo *pci, uint32_t num,
    struct bmap_iod_info *bii, enum rw rw)
{
	struct slvr_ref *s, *tmp = NULL, ts;

	psc_assert(rw == SL_WRITE || rw == SL_READ);

	ts.slvr_num = num;

 retry:
	BII_LOCK(bii);
	s = SPLAY_FIND(biod_slvrtree, &bii->bii_slvrs, &ts);
	if (s) {
		SLVR_LOCK(s);
		if (s->slvr_flags & SLVR_FREEING) {
			SLVR_ULOCK(s);
			/*
			 * Lock is required to free the slvr.
			 * It must be held here to prevent the slvr
			 * from being freed before we release the lock.
			 */
			BII_ULOCK(bii);
			goto retry;

		} else {
			BII_ULOCK(bii);

			s->slvr_flags |= SLVR_PINNED;

			if (rw == SL_WRITE)
				s->slvr_pndgwrts++;
			else
				s->slvr_pndgreads++;
		}
		SLVR_ULOCK(s);

		if (tmp)
			psc_pool_return(slvr_pool, tmp);

	} else {
		if (!tmp) {
			BII_ULOCK(bii);
			tmp = psc_pool_get(slvr_pool);
			goto retry;
		}

		s = tmp;
		memset(s, 0, sizeof(*s));
		s->slvr_num = num;
		s->slvr_flags = SLVR_NEW | SLVR_SPLAYTREE | SLVR_PINNED;
		s->slvr_pri = bii;
		INIT_PSC_LISTENTRY(&s->slvr_lentry);
		INIT_SPINLOCK(&s->slvr_lock);
		pll_init(&s->slvr_pndgaios, struct sli_aiocb_reply,
		    aiocbr_lentry, &s->slvr_lock);

		if (rw == SL_WRITE)
			s->slvr_pndgwrts = 1;
		else
			s->slvr_pndgreads = 1;

		PSC_SPLAY_XINSERT(biod_slvrtree, &bii->bii_slvrs, s);
		bmap_op_start_type(bii_2_bmap(bii), BMAP_OPCNT_SLVR);
		BII_ULOCK(bii);
	}
	DEBUG_SLVR(PLL_DEBUG, s, "incref");
	return (s);
}

__static void
slvr_remove(struct slvr_ref *s)
{
	struct bmap_iod_info *bii;

	DEBUG_SLVR(PLL_DEBUG, s, "freeing slvr");

	/* Slvr should be detached from any listheads. */
	psc_assert(psclist_disjoint(&s->slvr_lentry));
	psc_assert(!(s->slvr_flags & SLVR_SPLAYTREE));
	psc_assert(s->slvr_flags & SLVR_FREEING);

	bii = slvr_2_bii(s);

	BII_LOCK(bii);
	PSC_SPLAY_XREMOVE(biod_slvrtree, &bii->bii_slvrs, s);
	bmap_op_done_type(bii_2_bmap(bii), BMAP_OPCNT_SLVR);

	psc_pool_return(slvr_pool, s);
}

void
slvr_slb_free_locked(struct slvr_ref *s, struct psc_poolmgr *m)
{
	struct sl_buffer *tmp = s->slvr_slab;

	SLVR_LOCK_ENSURE(s);
	psc_assert(s->slvr_flags & SLVR_SLBFREEING);
	psc_assert(!(s->slvr_flags & SLVR_FREEING));
	psc_assert(s->slvr_slab);

	s->slvr_flags &= ~(SLVR_SLBFREEING | SLVR_DATARDY | SLVR_DATAERR);

	DEBUG_SLVR(PLL_INFO, s, "freeing slvr slab=%p", s->slvr_slab);
	s->slvr_slab = NULL;
	SLVR_WAKEUP(s);

	psc_pool_return(m, tmp);
}

/**
 * slvr_buffer_reap - The reclaim function for sl_bufs_pool.  Note that
 *	our caller psc_pool_get() ensures that we are called
 *	exclusively.
 */
int
slvr_buffer_reap(struct psc_poolmgr *m)
{
	static struct psc_dynarray a;
	struct slvr_ref *s, *dummy;
	int i, n, locked;

	n = 0;
	psc_dynarray_init(&a);

	LIST_CACHE_LOCK(&lruSlvrs);
	LIST_CACHE_FOREACH_SAFE(s, dummy, &lruSlvrs) {
		DEBUG_SLVR(PLL_INFO, s, "considering for reap, nwaiters=%d",
			   atomic_read(&m->ppm_nwaiters));

		/*
		 * We are reaping, so it is fine to back off on some
		 * slivers.  We have to use a reqlock here because
		 * slivers do not have private spinlocks, instead they
		 * use the lock of the bii.  So if this thread tries to
		 * free a slvr from the same bii trylock will abort.
		 */
		if (!SLVR_TRYRLOCK(s, &locked))
			continue;

		/*
		 * Look for slvrs which can be freed;
		 * slvr_lru_freeable() returning true means that no slab
		 * is attached.
		 */
		if (slvr_lru_freeable(s)) {
			psc_dynarray_add(&a, s);
			s->slvr_flags |= SLVR_FREEING;
			lc_remove(&lruSlvrs, s);
			goto next;
		}

		if (slvr_lru_slab_freeable(s)) {
			/*
			 * At this point we know that the slab can be
			 * reclaimed, however the slvr itself may have
			 * to stay.
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
			slvr_slb_free_locked(s, m);
			SLVR_URLOCK(s, locked);

		} else if (s->slvr_flags & SLVR_FREEING) {
			psc_assert(!(s->slvr_flags & SLVR_SLBFREEING));
			psc_assert(!(s->slvr_flags & SLVR_PINNED));
			psc_assert(!s->slvr_slab);

			if (s->slvr_flags & SLVR_SPLAYTREE) {
				s->slvr_flags &= ~SLVR_SPLAYTREE;
				SLVR_ULOCK(s);
				slvr_remove(s);
			} else
				SLVR_URLOCK(s, locked);
		}
	}
	psc_dynarray_free(&a);

	if (!n || n < atomic_read(&m->ppm_nwaiters))
		psc_waitq_wakeone(&slvrWaitq);

	return (n);
}

void
sliaiothr_main(__unusedx struct psc_thread *thr)
{
	struct sli_iocb *iocb, *next;
	sigset_t signal_set;
	int signo;

	sigemptyset(&signal_set);
	sigaddset(&signal_set, SIGIO);

	for (;;) {

		sigwait(&signal_set, &signo);
		psc_assert(signo == SIGIO);

		LIST_CACHE_LOCK(&sli_iocb_pndg);
		LIST_CACHE_FOREACH_SAFE(iocb, next, &sli_iocb_pndg) {
			iocb->iocb_rc = aio_error(&iocb->iocb_aiocb);
			if (iocb->iocb_rc == EINVAL)
				continue;
			if (iocb->iocb_rc == EINPROGRESS)
				continue;
			psc_assert(iocb->iocb_rc != ECANCELED);
			if (iocb->iocb_rc == 0)
				iocb->iocb_len = aio_return(&iocb->iocb_aiocb);

			if (OPSTAT_CURR(SLI_OPST_DEBUG) == 3)
				iocb->iocb_rc = EIO;

			psclog_info("got signal: iocb=%p", iocb);
			lc_remove(&sli_iocb_pndg, iocb);
			LIST_CACHE_ULOCK(&sli_iocb_pndg);
			iocb->iocb_cbf(iocb);	/* slvr_fsaio_done() */
			LIST_CACHE_LOCK(&sli_iocb_pndg);
		}
		LIST_CACHE_ULOCK(&sli_iocb_pndg);
	}
}

void
slvr_cache_init(void)
{
	psc_poolmaster_init(&slvr_poolmaster,
	    struct slvr_ref, slvr_lentry, PPMF_AUTO, 64, 64, 0,
	    NULL, NULL, NULL, "slvr");
	slvr_pool = psc_poolmaster_getmgr(&slvr_poolmaster);

	lc_reginit(&lruSlvrs, struct slvr_ref, slvr_lentry, "lruslvrs");
	lc_reginit(&crcqSlvrs, struct slvr_ref, slvr_lentry, "crcqslvrs");

	if (globalConfig.gconf_async_io) {

		psc_poolmaster_init(&sli_iocb_poolmaster,
		    struct sli_iocb, iocb_lentry, PPMF_AUTO, 64, 64,
		    1024, NULL, NULL, NULL, "iocb");
		sli_iocb_pool = psc_poolmaster_getmgr(&sli_iocb_poolmaster);

		psc_poolmaster_init(&sli_aiocbr_poolmaster,
		    struct sli_aiocb_reply, aiocbr_lentry, PPMF_AUTO, 64,
		    64, 1024, NULL, NULL, NULL, "aiocbr");
		sli_aiocbr_pool = psc_poolmaster_getmgr(&sli_aiocbr_poolmaster);

		lc_reginit(&sli_iocb_pndg, struct sli_iocb, iocb_lentry,
		    "iocbpndg");

		pscthr_init(SLITHRT_ASYNC_IO, 0, sliaiothr_main, NULL,
		    0, "sliaiothr");
	}

	sl_buffer_cache_init();

	slvr_worker_init();
}

#if PFL_DEBUG > 0
void
dump_sliver(struct slvr_ref *s)
{
	DEBUG_SLVR(PLL_MAX, s, "");
}

void
dump_sliver_flags(int fl)
{
	int seq = 0;

	PFL_PRFLAG(SLVR_NEW, &fl, &seq);
	PFL_PRFLAG(SLVR_SPLAYTREE, &fl, &seq);
	PFL_PRFLAG(SLVR_FAULTING, &fl, &seq);
	PFL_PRFLAG(SLVR_GETSLAB, &fl, &seq);
	PFL_PRFLAG(SLVR_PINNED, &fl, &seq);
	PFL_PRFLAG(SLVR_DATARDY, &fl, &seq);
	PFL_PRFLAG(SLVR_DATAERR, &fl, &seq);
	PFL_PRFLAG(SLVR_LRU, &fl, &seq);
	PFL_PRFLAG(SLVR_CRCDIRTY, &fl, &seq);
	PFL_PRFLAG(SLVR_CRCING, &fl, &seq);
	PFL_PRFLAG(SLVR_FREEING, &fl, &seq);
	PFL_PRFLAG(SLVR_SLBFREEING, &fl, &seq);
	PFL_PRFLAG(SLVR_REPLDST, &fl, &seq);
	PFL_PRFLAG(SLVR_REPLFAIL, &fl, &seq);
	PFL_PRFLAG(SLVR_AIOWAIT, &fl, &seq);
	PFL_PRFLAG(SLVR_RDMODWR, &fl, &seq);
	PFL_PRFLAG(SLVR_REPLWIRE, &fl, &seq);
	if (fl)
		printf(" unknown: %x", fl);
	printf("\n");
}
#endif
