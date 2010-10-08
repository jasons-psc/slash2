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

#include <pthread.h>
#include <stdio.h>

#include "pfl/cdefs.h"
#include "psc_ds/list.h"
#include "psc_ds/listcache.h"
#include "psc_util/atomic.h"
#include "psc_util/pool.h"

#include "bmap.h"
#include "buffer.h"
#include "cache_params.h"
#include "fid.h"
#include "fidcache.h"
#include "slutil.h"

int			(*fidcReapCb)(struct fidc_membh *);
struct psc_poolmaster	  fidcPoolMaster;
struct psc_poolmgr	 *fidcPool;
struct psc_listcache	  fidcBusyList;		/* active in use */
struct psc_listcache	  fidcIdleList;			/* identity untouched, but reapable */
struct psc_hashtbl	  fidcHtable;

#define	fcmh_get()	psc_pool_get(fidcPool)
#define	fcmh_put(f)	psc_pool_return(fidcPool, (f))

/**
 * fcmh_dtor - Destructor for FID cache member handles.
 * @r: fcmh being destroyed.
 */
void
fcmh_destroy(struct fidc_membh *f)
{
	psc_assert(SPLAY_EMPTY(&f->fcmh_bmaptree));
	psc_assert(!psc_waitq_nwaiters(&f->fcmh_waitq));
	psc_assert(f->fcmh_refcnt == 0);
	psc_assert(psc_hashent_disjoint(&fidcHtable, f));

	/* slc_fcmh_dtor(), slm_fcmh_dtor(), sli_fcmh_dtor() */
	if (sl_fcmh_ops.sfop_dtor) {
		if (f->fcmh_flags & FCMH_CTOR_FAILED)
			DEBUG_FCMH(PLL_WARN, f, "bypassing dtor() call");
		else
			sl_fcmh_ops.sfop_dtor(f);
	}

	f->fcmh_flags = FCMH_CAC_FREE;
	fcmh_put(f);
}

/**
 * fcmh_setattr - Update the high-level app stat(2)-like attribute buffer
 *	for a FID cache member.
 * @fcmh: FID cache member to update.
 * @sstb: incoming stat attributes.
 * @flags: behavioral flags.
 * Notes:
 *     (1) if SAVELOCAL has been specified, save local field values:
 *		(o) file size
 *		(o) mtime
 *     (2) This function should only be used by a client.
 */
void
fcmh_setattr(struct fidc_membh *fcmh, struct srt_stat *sstb, int flags)
{
	if (!(flags & FCMH_SETATTRF_HAVELOCK))
		FCMH_LOCK(fcmh);

	if (fcmh_2_gen(fcmh) == FGEN_ANY)
	    fcmh_2_gen(fcmh) = sstb->sst_gen;

	if (fcmh_2_fid(fcmh) != SLFID_ROOT &&
	    fcmh_2_gen(fcmh) > sstb->sst_gen) {
		DEBUG_FCMH(PLL_WARN, fcmh, "attempt to set attr from a "
		    "stale generation number!");
		goto out;
	}

	/*
	 * If we don't have stat attributes, how can we save
	 * our local updates?
	 */
	if ((fcmh->fcmh_flags & FCMH_HAVE_ATTRS) == 0)
		flags &= ~FCMH_SETATTRF_SAVELOCAL;

	psc_assert(sstb->sst_gen != FGEN_ANY);
	psc_assert(fcmh->fcmh_fg.fg_fid == sstb->sst_fid);

	/*
	 * If generation numbers match, take the highest of the values.
	 * Otherwise, disregard local values and blindly accept what the
	 * MDS tells us.
	 */
	if (flags & FCMH_SETATTRF_SAVELOCAL) {
		if (fcmh_2_ptruncgen(fcmh) == sstb->sst_ptruncgen &&
		    fcmh_2_gen(fcmh) == sstb->sst_gen &&
		    fcmh_2_fsz(fcmh) > sstb->sst_size)
			sstb->sst_size = fcmh_2_fsz(fcmh);
		if (fcmh_2_utimgen(fcmh) == sstb->sst_utimgen)
			sstb->sst_mtim = fcmh->fcmh_sstb.sst_mtim;
	}

	fcmh->fcmh_sstb = *sstb;
	fcmh->fcmh_flags |= FCMH_HAVE_ATTRS;
	fcmh->fcmh_flags &= ~FCMH_GETTING_ATTRS;

	if (sl_fcmh_ops.sfop_postsetattr)
		sl_fcmh_ops.sfop_postsetattr(fcmh);

	DEBUG_FCMH(PLL_DEBUG, fcmh, "attr set");

 out:
	if (!(flags & FCMH_SETATTRF_HAVELOCK))
		FCMH_ULOCK(fcmh);
}

/**
 * fidc_reap - Reap some inodes from the idle list.
 */
int
fidc_reap(struct psc_poolmgr *m)
{
#define FCMH_MAX_REAP 8
	struct fidc_membh *f, *tmp, *reap[FCMH_MAX_REAP];
	int i, nreap=0;

	psc_assert(m == fidcPool);

	LIST_CACHE_LOCK(&fidcIdleList);
	LIST_CACHE_FOREACH_SAFE(f, tmp, &fidcIdleList) {
		if (nreap == FCMH_MAX_REAP ||
		    m->ppm_nfree + nreap >=
		    atomic_read(&m->ppm_nwaiters) + 1)
			break;

		/* skip the root right now, no need for locking */
		if (fcmh_2_fid(f) == 1)
			continue;

		if (!FCMH_TRYLOCK(f))
			continue;

		/* skip items in use */
		if (f->fcmh_refcnt)
			goto end;

		psc_assert(f->fcmh_flags & FCMH_CAC_IDLE);

		/* already victimized */
		if (f->fcmh_flags & FCMH_CAC_REAPED)
			goto end;

		/*
		 * Consult the context-specific callback handler before
		 *    freeing.
		 */
		if (!fidcReapCb || fidcReapCb(f)) {
			f->fcmh_flags |= FCMH_CAC_REAPED|FCMH_CAC_TOFREE;
			lc_remove(&fidcIdleList, f);
			reap[nreap] = f;
			nreap++;
		}
 end:
		FCMH_ULOCK(f);
	}
	LIST_CACHE_ULOCK(&fidcIdleList);

	for (i = 0; i < nreap; i++) {
		DEBUG_FCMH(PLL_DEBUG, reap[i], "moving to free list");
		psc_hashent_remove(&fidcHtable, reap[i]);
		fcmh_destroy(reap[i]);
	}
	return (i);
}

/**
 * fidc_lookup_fg - Wrapper for fidc_lookup().  Called when the
 *	generation number is known.
 */
struct fidc_membh *
_fidc_lookup_fg(const struct slash_fidgen *fg, const char *file,
    const char *func, int line)
{
	struct fidc_membh *fcmhp;
	int rc;

	rc = _fidc_lookup(fg, 0, NULL, 0, &fcmhp, file, func, line);
	return (rc == 0 ? fcmhp : NULL);
}

/**
 * fidc_lookup_fid - Wrapper for fidc_lookup().  Called when the
 *	generation number is not known.
 */
struct fidc_membh *
_fidc_lookup_fid(slfid_t f, const char *file, const char *func, int line)
{
	struct slash_fidgen t = { f, FGEN_ANY };
	struct fidc_membh *fcmhp;
	int rc;

	rc = _fidc_lookup(&t, 0, NULL, 0, &fcmhp, file, func, line);
	return (rc == 0 ? fcmhp : NULL);

}

/**
 * _fidc_lookup - Search the FID cache for a member by its FID,
 *	optionally creating it.
 * Notes:  Newly acquired fcmh's are ref'd with FCMH_OPCNT_NEW, reused ones
 *	are ref'd with FCMH_OPCNT_LOOKUP_FIDC.
 */
int
_fidc_lookup(const struct slash_fidgen *fgp, int flags,
    struct srt_stat *sstb, int setattrflags, struct fidc_membh **fcmhp,
    const char *file, const char *func, int line)
{
	struct fidc_membh *tmp, *fcmh, *fcmh_new;
	struct slash_fidgen searchfg = *fgp;
	struct psc_hashbkt *b;
	int rc, try_create = 0;

	psclog(file, func, line, PSC_SUBSYS, PLL_INFO, 0,
	    "fidc_lookup called for fid %"PRIx64, searchfg.fg_fid);

	rc = 0;
	*fcmhp = NULL;
	fcmh_new = NULL; /* gcc */

	/* sanity checks */
#ifdef SLASH_CLIENT
	if (flags & FIDC_LOOKUP_CREATE)
		psc_assert(sstb || (flags & FIDC_LOOKUP_LOAD));
#else
	psc_assert(!(flags & FIDC_LOOKUP_EXCL));
#endif

	if (sstb)
		psc_assert((flags & FIDC_LOOKUP_LOAD) == 0);
	if (flags & FIDC_LOOKUP_LOAD)
		psc_assert(sstb == NULL);

	/* OK.  now check if it is already in the cache */
	b = psc_hashbkt_get(&fidcHtable, &searchfg.fg_fid);
 restart:
	fcmh = NULL;
	psc_hashbkt_lock(b);
	PSC_HASHBKT_FOREACH_ENTRY(&fidcHtable, tmp, b) {
		/*
		 * Note that generation number is only used to track
		 *   truncations.
		 */
		if (searchfg.fg_fid != fcmh_2_fid(tmp))
			continue;
		FCMH_LOCK(tmp);

		/* if the item is being freed, ingore it */
		if (tmp->fcmh_flags & FCMH_CAC_TOFREE) {
			DEBUG_FCMH(PLL_WARN, tmp, "tmp fcmh is deprecated");
			FCMH_ULOCK(tmp);
			continue;
		}
		/* if the item is being inited, take a reference and wait */
		if (tmp->fcmh_flags & FCMH_CAC_INITING) {
			psc_hashbkt_unlock(b);
			tmp->fcmh_flags |= FCMH_CAC_WAITING;
			fcmh_op_start_type(tmp, FCMH_OPCNT_WAIT);
			fcmh_wait_nocond_locked(tmp);
			fcmh_op_done_type(tmp, FCMH_OPCNT_WAIT);
			goto restart;
		}
		fcmh = tmp;
		break;
	}

	/*
	 * If the above lookup is a success, we hold the lock, but
	 * we haven't take a reference yet.  Also, we need to keep
	 * the bucket lock in case we need to insert a new item.
	 */
	if (fcmh) {
		psc_hashbkt_unlock(b);
		/*
		 * Test to see if we jumped here from fidcFreeList.
		 * Note an unlucky thread could find that the fid
		 * does not exist before allocation and exist after
		 * that.
		 */
		if (try_create) {
			fcmh_put(fcmh_new);
			fcmh_new = NULL;
		}

		psc_assert(fgp->fg_fid == fcmh_2_fid(fcmh));
		/* apply provided attributes to the cache */
		if (sstb)
			fcmh_setattr(fcmh, sstb, setattrflags |
			    FCMH_SETATTRF_HAVELOCK);

		/* keep me around after unlocking later */
		fcmh_op_start_type(fcmh, FCMH_OPCNT_LOOKUP_FIDC);

		if (sl_fcmh_ops.sfop_modify)
			sl_fcmh_ops.sfop_modify(fcmh, (void *)fgp);

		FCMH_ULOCK(fcmh);
		*fcmhp = fcmh;
		return (0);
	}
	/* we have failed to find a match in the cache */
	if (flags & FIDC_LOOKUP_CREATE) {
		if (!try_create) {
			/* allocate a new fidc handle */
			psc_hashbkt_unlock(b);
			fcmh_new = fcmh_get();
			try_create = 1;
			goto restart;
		}
	 } else {
		/* FIDC_LOOKUP_CREATE was not specified and the fcmh
		 *  is not present.
		 */
		psc_hashbkt_unlock(b);
		return (ENOENT);
	}

	/* OK, we've got a new fcmh.  No need to lock it since
	 *  it's not yet visible to other threads.
	 */
	fcmh = fcmh_new;

	memset(fcmh, 0, fidcPoolMaster.pms_entsize);
	INIT_PSC_LISTENTRY(&fcmh->fcmh_lentry);
	SPLAY_INIT(&fcmh->fcmh_bmaptree);
	INIT_SPINLOCK(&fcmh->fcmh_lock);
	psc_hashent_init(&fidcHtable, fcmh);
	psc_waitq_init(&fcmh->fcmh_waitq);

	fcmh_op_start_type(fcmh, FCMH_OPCNT_NEW);

	COPYFG(&fcmh->fcmh_fg, fgp);
	DEBUG_FCMH(PLL_DEBUG, fcmh, "new fcmh");

	/*
	 * Add the new item to the hash list, but mark it as INITING.
	 * If we fail to initialize it, we should mark it as TOFREE
	 * and leave it around for the reaper to free it.  Note that
	 * the item is not on any list yet.
	 */
	fcmh->fcmh_flags |= FCMH_CAC_INITING;
	psc_hashbkt_add_item(&fidcHtable, b, fcmh);
	psc_hashbkt_unlock(b);

	if (sstb) {
		FCMH_LOCK(fcmh);
		fcmh_setattr(fcmh, sstb, setattrflags |
		    FCMH_SETATTRF_HAVELOCK);
		rc = sl_fcmh_ops.sfop_ctor(fcmh);
		if (rc)
			fcmh->fcmh_flags |= FCMH_CTOR_FAILED;
		goto out2;
	} else {
		/*
		 * Call service specific constructor slc_fcmh_ctor(),
		 * slm_fcmh_ctor(), and sli_fcmh_ctor() to initialize their
		 * private fields that follow the main fcmh structure. It is
		 * safe to not lock because we don't touch the state, and
		 * other thread should be waiting for us.
		 */
		rc = sl_fcmh_ops.sfop_ctor(fcmh);
		if (rc) {
			fcmh->fcmh_flags |= FCMH_CTOR_FAILED;
			goto out1;
		}
	}

	if (flags & FIDC_LOOKUP_LOAD) {
		psc_assert(sl_fcmh_ops.sfop_getattr);
		rc = sl_fcmh_ops.sfop_getattr(fcmh);   /* slc_fcmh_getattr() */
	}

 out1:
	FCMH_LOCK(fcmh);
 out2:
	fcmh->fcmh_flags &= ~FCMH_CAC_INITING;
	if (fcmh->fcmh_flags & FCMH_CAC_WAITING) {
		fcmh->fcmh_flags &= ~FCMH_CAC_WAITING;
		psc_waitq_wakeall(&fcmh->fcmh_waitq);
	}

	fcmh->fcmh_flags |= FCMH_CAC_IDLE;
	lc_add(&fidcIdleList, fcmh);

	if (rc) {
		FCMH_ULOCK(fcmh);
		fcmh->fcmh_flags |= FCMH_CAC_TOFREE;
		fcmh_op_done_type(fcmh, FCMH_OPCNT_NEW);
	} else {
		*fcmhp = fcmh;
		fcmh_op_start_type(fcmh, FCMH_OPCNT_LOOKUP_FIDC);
		fcmh_op_done_type(fcmh, FCMH_OPCNT_NEW);
	}
	return (rc);
}

/**
 * fidc_init - Initialize the FID cache.
 */
void
fidc_init(int privsiz, int nobj,
    int (*fcmh_reap_cb)(struct fidc_membh *))
{
	_psc_poolmaster_init(&fidcPoolMaster,
	    sizeof(struct fidc_membh) + privsiz,
	    offsetof(struct fidc_membh, fcmh_lentry),
	    PPMF_AUTO, nobj, nobj, 0, NULL,
	    NULL, fidc_reap, NULL, "fcmh");
	fidcPool = psc_poolmaster_getmgr(&fidcPoolMaster);

	lc_reginit(&fidcBusyList, struct fidc_membh,
	    fcmh_lentry, "fcmhbusy");
	lc_reginit(&fidcIdleList, struct fidc_membh,
	    fcmh_lentry, "fcmhidle");

	psc_hashtbl_init(&fidcHtable, 0, struct fidc_membh,
	    fcmh_fg, fcmh_hentry, nobj * 2, NULL, "fidc");

	fidcReapCb = fcmh_reap_cb;
}

ssize_t
fcmh_getsize(struct fidc_membh *h)
{
	ssize_t size;
	int locked;

	locked = reqlock(&h->fcmh_lock);
	size = fcmh_2_fsz(h);
	ureqlock(&h->fcmh_lock, locked);
	return (size);
}

/*
 * fcmh_op_start/done_type(): we only move a cache item to the busy list if we
 * know that the reference being taken is a long one. For short-lived references,
 * we avoid moving the cache item around.  Also, we only move a cache item back
 * to the idle list when the _last_ reference is dropped.
 */
void
fcmh_op_start_type(struct fidc_membh *f, enum fcmh_opcnt_types type)
{
	int locked = FCMH_RLOCK(f);

	psc_assert(f->fcmh_refcnt >= 0);
	f->fcmh_refcnt++;

	DEBUG_FCMH(PLL_NOTIFY, f, "took ref (type=%d)", type);

	/* Only 2 types of references may be long standing, FCMH_OPCNT_OPEN
	 *   and FCMH_OPCNT_BMAP.  Other ref types should not move the fcmh
	 *   to the busy list.
	 */
	if (type == FCMH_OPCNT_OPEN || type == FCMH_OPCNT_BMAP) {
		if (f->fcmh_flags & FCMH_CAC_IDLE) {
			f->fcmh_flags &= ~FCMH_CAC_IDLE;
			f->fcmh_flags |= FCMH_CAC_BUSY;
			lc_remove(&fidcIdleList, f);
			lc_add(&fidcBusyList, f);
		}
	}
	FCMH_URLOCK(f, locked);
}

void
fcmh_op_done_type(struct fidc_membh *f, enum fcmh_opcnt_types type)
{
	FCMH_RLOCK(f);

	psc_assert(f->fcmh_refcnt > 0);

	DEBUG_FCMH(PLL_NOTIFY, (f), "release ref (type=%d)", type);

	f->fcmh_refcnt--;
	if (f->fcmh_refcnt == 0) {
		/*
		 * XXX Should we also free it if FCMH_CAC_TOFREE?
		 */
		if (f->fcmh_flags & FCMH_CAC_BUSY) {
			f->fcmh_flags &= ~FCMH_CAC_BUSY;
			f->fcmh_flags |= FCMH_CAC_IDLE;
			lc_remove(&fidcBusyList, f);
			lc_add(&fidcIdleList, f);
		}
	} else
		fcmh_wake_locked(f);
	FCMH_ULOCK(f);
}

void
dump_fcmh(struct fidc_membh *f)
{
	int locked;

	locked = FCMH_RLOCK(f);
	psclog_max("fcmh@%p f+g:"SLPRI_FG" ref:%d", f,
	    SLPRI_FG_ARGS(&f->fcmh_fg), f->fcmh_refcnt);
	FCMH_URLOCK(f, locked);
}

void
dump_fidcache(void)
{
	struct psc_hashbkt *bkt;
	struct fidc_membh *tmp;

	PSC_HASHTBL_FOREACH_BUCKET(bkt, &fidcHtable) {
		psc_hashbkt_lock(bkt);
		PSC_HASHBKT_FOREACH_ENTRY(&fidcHtable, tmp, bkt)
			dump_fcmh(tmp);
		psc_hashbkt_unlock(bkt);
	}
}

void
dump_fcmh_flags(int flags)
{
	int seq = 0;

	if (flags & FCMH_CAC_FREE)
		pfl_print_flag("FCMH_CAC_FREE", &seq);
	if (flags & FCMH_CAC_IDLE)
		pfl_print_flag("FCMH_CAC_IDLE", &seq);
	if (flags & FCMH_CAC_BUSY)
		pfl_print_flag("FCMH_CAC_DIRTY", &seq);
	if (flags & FCMH_CAC_TOFREE)
		pfl_print_flag("FCMH_CAC_TOFREE", &seq);
	if (flags & FCMH_HAVE_ATTRS)
		pfl_print_flag("FCMH_HAVE_ATTRS", &seq);
	if (flags & FCMH_GETTING_ATTRS)
		pfl_print_flag("FCMH_GETTING_ATTRS", &seq);
	printf("\n");
}
