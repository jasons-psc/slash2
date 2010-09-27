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

#define PSC_SUBSYS SLSS_BMAP
#include "slsubsys.h"

#include "pfl/cdefs.h"
#include "psc_ds/tree.h"
#include "psc_ds/treeutil.h"
#include "psc_util/alloc.h"
#include "psc_util/lock.h"

#include "bmap.h"
#include "bmapdesc.h"
#include "fidcache.h"
#include "inode.h"
#include "slashrpc.h"

__static SPLAY_GENERATE(bmap_cache, bmapc_memb, bcm_tentry, bmap_cmp);

struct psc_poolmaster	 bmap_poolmaster;
struct psc_poolmgr	*bmap_pool;

/**
 * bmap_cmp - comparator for bmapc_membs in the splay cache.
 * @a: a bmapc_memb
 * @b: another bmapc_memb
 */
int
bmap_cmp(const void *x, const void *y)
{
	const struct bmapc_memb *a = x, *b = y;

	return (CMP(a->bcm_bmapno, b->bcm_bmapno));
}

void
bmap_orphan(struct bmapc_memb *b)
{
	struct fidc_membh *f = b->bcm_fcmh;
	int locked;

	locked = BMAP_RLOCK(b);

	DEBUG_BMAP(PLL_INFO, b, "orphan");
	psc_assert(!(b->bcm_flags & BMAP_ORPHAN));
	b->bcm_flags |= BMAP_ORPHAN;

	FCMH_RLOCK(f);
	PSC_SPLAY_XREMOVE(bmap_cache, &f->fcmh_bmaptree, b);
	fcmh_op_done_type(f, FCMH_OPCNT_BMAP);

	BMAP_URLOCK(b, locked);
}

void
bmap_remove(struct bmapc_memb *b)
{
	struct fidc_membh *f = b->bcm_fcmh;

	BMAP_RLOCK(b);

	DEBUG_BMAP(PLL_INFO, b, "removing");

	psc_assert(b->bcm_flags & BMAP_CLOSING);
	psc_assert(!(b->bcm_flags & BMAP_DIRTY));
	psc_assert(!atomic_read(&b->bcm_opcnt));

	if (!(b->bcm_flags & BMAP_ORPHAN)) {
		BMAP_ULOCK(b);

		FCMH_RLOCK(f);
		PSC_SPLAY_XREMOVE(bmap_cache, &f->fcmh_bmaptree, b);
		fcmh_op_done_type(f, FCMH_OPCNT_BMAP);

		psc_pool_return(bmap_pool, b);
	} else
		BMAP_ULOCK(b);

}

void
_bmap_op_done(struct bmapc_memb *b, const char *fn, const char *func,
    int line, const char *fmt, ...)
{
	va_list ap;

	BMAP_RLOCK(b);
	psc_atomic32_dec(&b->bcm_opcnt);
	psc_assert(psc_atomic32_read(&b->bcm_opcnt) >= 0);

	va_start(ap, fmt);
	psclogv(fn, func, line, SLSS_BMAP, PLL_DEBUG, 0, fmt, ap);
	va_end(ap);

	if (!psc_atomic32_read(&b->bcm_opcnt)) {
		b->bcm_flags |= BMAP_CLOSING;
		BMAP_ULOCK(b);

		if (bmap_ops.bmo_final_cleanupf)
			bmap_ops.bmo_final_cleanupf(b);

		bmap_remove(b);
		return;
	}
	bcm_wake_locked(b);
	BMAP_ULOCK(b);
}

__static struct bmapc_memb *
bmap_lookup_cache_locked(struct fidc_membh *f, sl_bmapno_t n)
{
	struct bmapc_memb lb, *b;

 restart:
	lb.bcm_bmapno = n;
	b = SPLAY_FIND(bmap_cache, &f->fcmh_bmaptree, &lb);
	if (b) {
		BMAP_LOCK(b);
		if (b->bcm_flags & BMAP_CLOSING) {
			/*
			 * This bmap is going away; wait for
			 * it so we can reload it back.
			 */
			BMAP_ULOCK(b);
			fcmh_wait_nocond_locked(f);
			goto restart;
		}
		bmap_op_start_type(b, BMAP_OPCNT_LOOKUP);
	}
	return (b);
}

/**
 * bmap_getf - Get the specified bmap.
 * @f: fcmh.
 * @n: bmap number.
 * @rw: access mode.
 * @flags: retrieval parameters.
 * @bp: value-result bmap pointer.
 * Notes: returns the bmap referenced via bcm_opcnt.
 */
int
bmap_getf(struct fidc_membh *f, sl_bmapno_t n, enum rw rw, int flags,
    struct bmapc_memb **bp)
{
	int rc = 0, do_load = 0, locked, bmaprw = 0;
	struct bmapc_memb *b;

	*bp = NULL;

	if (rw)
		bmaprw = ((rw == SL_WRITE) ? BMAP_WR : BMAP_RD);

	locked = FCMH_RLOCK(f);
	b = bmap_lookup_cache_locked(f, n);
	if (b == NULL) {
		if ((flags & BMAPGETF_LOAD) == 0) {
			ureqlock(&f->fcmh_lock, locked);
			return (ENOENT);
		}
		b = psc_pool_get(bmap_pool);
		memset(b, 0, bmap_pool->ppm_master->pms_entsize);
		INIT_PSC_LISTENTRY(&b->bcm_lentry);
		INIT_SPINLOCK(&b->bcm_lock);

		atomic_set(&b->bcm_opcnt, 0);
		b->bcm_fcmh = f;
		b->bcm_bmapno = n;

		/*
		 * Signify that the bmap is newly initialized and therefore
		 *  may not contain certain structures.
		 */
		b->bcm_flags = BMAP_INIT | bmaprw;

		bmap_op_start_type(b, BMAP_OPCNT_LOOKUP);
		/* Perform app-specific substructure initialization. */
		bmap_ops.bmo_init_privatef(b);

		/* Add to the fcmh's bmap cache */
		SPLAY_INSERT(bmap_cache, &f->fcmh_bmaptree, b);
		fcmh_op_start_type(f, FCMH_OPCNT_BMAP);
		do_load = 1;
	}
	FCMH_URLOCK(f, locked);

	if (do_load) {
		if ((flags & BMAPGETF_NORETRIEVE) == 0)
			rc = bmap_ops.bmo_retrievef(b, rw);

		BMAP_LOCK(b);
		b->bcm_flags &= ~BMAP_INIT;
		bcm_wake_locked(b);
		if (rc)
			goto out;

	} else {
		/* Wait while BMAP_INIT is set.
		 */
		bcm_wait_locked(b, (b->bcm_flags & BMAP_INIT));

	retry:
		/* Not all lookups are done with the intent of
		 *   changing the bmap mode.  bmap_lookup() does not
		 *   specify a rw value.
		 */
		if (bmaprw && !(bmaprw & b->bcm_flags) &&
		    bmap_ops.bmo_mode_chngf) {
			/* Others wishing to access this bmap in the
			 *   same mode must wait until MDCHNG ops have
			 *   completed.  If the desired mode is present
			 *   then a thread may proceed without blocking
			 *   here so long as it only accesses structures
			 *   which pertain to its mode.
			 */
			if (b->bcm_flags & BMAP_MDCHNG) {
				bcm_wait_locked(b,
					(b->bcm_flags & BMAP_MDCHNG));
				goto retry;

			} else {
				b->bcm_flags |= BMAP_MDCHNG;
				BMAP_ULOCK(b);

				DEBUG_BMAP(PLL_NOTIFY, b,
				   "about to mode change (rw=%d)", rw);

				rc = bmap_ops.bmo_mode_chngf(b, rw);
				BMAP_LOCK(b);
				b->bcm_flags &= ~BMAP_MDCHNG;
				if (!rc)
					b->bcm_flags |= bmaprw;
				bcm_wake_locked(b);
				if (rc)
					goto out;
			}
		}
	}
 out:
	BMAP_ULOCK(b);
	if (rc)
		bmap_op_done_type(b, BMAP_OPCNT_LOOKUP);
	else
		*bp = b;

	return (rc);
}

void
bmap_cache_init(size_t priv_size)
{
	_psc_poolmaster_init(&bmap_poolmaster, sizeof(struct bmapc_memb) +
	    priv_size, offsetof(struct bmapc_memb, bcm_lentry), PPMF_AUTO,
	    64, 64, 0, NULL, NULL, NULL, NULL, "bmap");
	bmap_pool = psc_poolmaster_getmgr(&bmap_poolmaster);
}

void
_debug_bmapod(struct bmapc_memb *bmap, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	DEBUG_BMAPODV(PLL_MAX, bmap, fmt, ap);
	va_end(ap);
}

int
bmapdesc_access_check(struct srt_bmapdesc *sbd, enum rw rw,
    sl_ios_id_t ios_id, lnet_nid_t ion_nid)
{
	if (rw == SL_READ) {
		/* Read requests can get by with looser authentication. */
		if (sbd->sbd_ion_nid != ion_nid &&
		    sbd->sbd_ion_nid != LNET_NID_ANY)
			return (EBADF);
		if (sbd->sbd_ios_id != ios_id &&
		    sbd->sbd_ios_id != IOS_ID_ANY)
			return (EBADF);
	} else if (rw == SL_WRITE) {
		if (sbd->sbd_ion_nid != ion_nid)
			return (EBADF);
		if (sbd->sbd_ios_id != ios_id)
			return (EBADF);
	} else {
		psc_errorx("invalid rw mode: %d", rw);
		return (EBADF);
	}
	return (0);
}

void
_log_debug_bmapodv(const char *file, const char *func, int lineno,
    int level, struct bmapc_memb *bmap, const char *fmt, va_list ap)
{
	char mbuf[LINE_MAX], rbuf[SL_MAX_REPLICAS + 1];
	unsigned char *b = bmap->bcm_repls;
	int off, k, ch[NBREPLST];

	vsnprintf(mbuf, sizeof(mbuf), fmt, ap);

	ch[BREPLST_INVALID] = '-';
	ch[BREPLST_REPL_SCHED] = 's';
	ch[BREPLST_REPL_QUEUED] = 'q';
	ch[BREPLST_VALID] = '+';
	ch[BREPLST_TRUNCPNDG] = 't';
	ch[BREPLST_GARBAGE] = 'g';
	ch[BREPLST_GARBAGE_SCHED] = 'x';
	ch[BREPLST_BADCRC] = 'c';

	for (k = 0, off = 0; k < SL_MAX_REPLICAS;
	    k++, off += SL_BITS_PER_REPLICA)
		rbuf[k] = ch[SL_REPL_GET_BMAP_IOS_STAT(b, off)];
	rbuf[k] = '\0';

	_DEBUG_BMAP(file, func, lineno, level, bmap, "repls=[%s] %s", rbuf, mbuf);
}

void
_log_debug_bmapod(const char *file, const char *func, int lineno,
    int level, struct bmapc_memb *bmap, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	_log_debug_bmapodv(file, func, lineno, level, bmap, fmt, ap);
	va_end(ap);
}
