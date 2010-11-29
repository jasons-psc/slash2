/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2009-2010, Pittsburgh Supercomputing Center (PSC).
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
 * The dircache interface caches READDIR bufs after reception from the
 * MDS via RPC for the consequent LOOKUPs on each item by pscfs that
 * follow.
 */

#include <sys/types.h>
#include <sys/time.h>

#include <stdint.h>
#include <string.h>

#include "pfl/fs.h"
#include "pfl/str.h"
#include "psc_ds/dynarray.h"
#include "psc_ds/listcache.h"
#include "psc_util/alloc.h"
#include "psc_util/lock.h"
#include "psc_util/pool.h"

#include "dircache.h"
#include "fidcache.h"
#include "sltypes.h"
#include "slutil.h"

struct psc_poolmaster	 dircache_poolmaster;
struct psc_poolmgr	*dircache_pool;

void
dircache_init(struct dircache_mgr *m, const char *name, size_t maxsz)
{
	m->dcm_maxsz = maxsz;
	m->dcm_alloc = 0;

	INIT_SPINLOCK(&m->dcm_lock);
	lc_reginit(&m->dcm_lc, struct dircache_ents, de_lentry_lc, "%s",
	    name);

#define DE_DEF 64
	psc_poolmaster_init(&dircache_poolmaster, struct dircache_ents,
	    de_lentry_lc, PPMF_AUTO, DE_DEF, DE_DEF, 0, NULL, NULL,
	    NULL, "dircache");
	dircache_pool = psc_poolmaster_getmgr(&dircache_poolmaster);
}

void
dircache_rls_ents(struct dircache_ents *e, int flags)
{
	struct dircache_info *i = e->de_info;
	struct dircache_mgr *m = i->di_dcm;
	int locked;

	if (flags & DCFREEF_EARLY) {
		psc_assert(psclist_disjoint(&e->de_lentry));
		psc_assert(psclist_disjoint(&e->de_lentry_lc));
	} else {
		DEBUG_FCMH(PLL_DEBUG, i->di_fcmh,
		    "rls dircache_ents %p cachesz=%zu", e, m->dcm_alloc);
		psc_assert(e->de_flags & DIRCE_FREEING);
	}

	locked = reqlock(&m->dcm_lock);
	m->dcm_alloc -= e->de_sz;

	if ((flags & DCFREEF_EARLY) == 0) {
		lc_remove(&i->di_dcm->dcm_lc, e);
		pll_remove(&i->di_list, e);
	}

	ureqlock(&m->dcm_lock, locked);

	psc_dynarray_free(&e->de_dents);
	PSCFREE(e->de_base);
	PSCFREE(e->de_desc);
	psc_pool_return(dircache_pool, e);

	if ((flags & DCFREEF_EARLY) == 0) {
		if (flags & DCFREEF_RELEASE)
			fcmh_op_done_type(i->di_fcmh, FCMH_OPCNT_DIRENTBUF);
		else
			fcmh_decref(i->di_fcmh, FCMH_OPCNT_DIRENTBUF);
	}
}

void
dircache_setfreeable_ents(struct dircache_ents *e)
{
	dircache_ent_lock(e);
	e->de_flags |= DIRCE_FREEABLE;

	if (!e->de_remlookup && !(e->de_flags & DIRCE_FREEING)) {
		e->de_flags |= DIRCE_FREEING;
		dircache_ent_ulock(e);
		dircache_rls_ents(e, DCFREEF_RELEASE);
	} else
		dircache_ent_ulock(e);
}

slfid_t
dircache_lookup(struct dircache_info *i, const char *name, int flag)
{
	struct psc_dynarray da = DYNARRAY_INIT;
	struct dircache_desc desc, *d;
	struct pscfs_dirent *dirent;
	struct dircache_ents *e;
	slfid_t ino = FID_ANY;
	int found, pos;

	desc.dd_hash	= psc_str_hashify(name);
	desc.dd_namelen	= strlen(name);
	desc.dd_name	= name;

	/* This lock is equiv to dircache_ent_lock()
	 */
	PLL_LOCK(&i->di_list);
	PLL_FOREACH(e, &i->di_list) {
		/*
		 * The return code for psc_dynarray_bsearch() tells us
		 * the position where our name should be to keep the
		 * the list sorted.  If it is one after the last item,
		 * then we know for sure the item is not there. Otherwise,
		 * we still need one more comparison to be sure.
		 */
		pos = psc_dynarray_bsearch(&e->de_dents,
		    &desc, dirent_cmp);
		if (pos >= psc_dynarray_len(&e->de_dents))
			continue;

		/* find first entry with equiv hash then loop through all */
		for (; pos > 0; pos--) {
			d = psc_dynarray_getpos(&e->de_dents, pos - 1);
			if (d->dd_hash != desc.dd_hash)
				break;
		}
		found = 0;
		for (; pos <= psc_dynarray_len(&e->de_dents); pos++) {
			d = psc_dynarray_getpos(&e->de_dents, pos);
			if (d->dd_hash != desc.dd_hash)
				break;

			/* Map the dirent from the desc's offset. */
			dirent = (void *)(e->de_base + d->dd_offset);

			psc_dbg("ino="SLPRI_FID" off=%"PRId64" nlen=%u "
			    "type=%#o dname=%.*s lookupname=%s off=%d d=%p",
			    dirent->pfd_ino, dirent->pfd_off, dirent->pfd_namelen,
			    dirent->pfd_type, dirent->pfd_namelen, dirent->pfd_name,
			    name, d->dd_offset, d);

			if (d->dd_hash == desc.dd_hash &&
			    d->dd_namelen == desc.dd_namelen &&
			    strncmp(d->dd_name, desc.dd_name, desc.dd_namelen) == 0) {
				if (!(d->dd_flags & DC_LOOKUP)) {
					e->de_remlookup--;
					d->dd_flags |= DC_LOOKUP;
				}

				if (!(d->dd_flags & DC_STALE))
					ino = dirent->pfd_ino;

				if (flag & DC_STALE)
					d->dd_flags |= DC_STALE;

				found = 1;
				break;
			}
		}

		if (!e->de_remlookup && (e->de_flags &
		    (DIRCE_FREEABLE | DIRCE_FREEING)) == DIRCE_FREEABLE) {
			/* If all of the items have been accessed via lookup then
			 *   assume that pscfs has an entry cached for each and free
			 *   the buffer.
			 */
			e->de_flags |= DIRCE_FREEING;
			psc_dynarray_add(&da, e);
		}

		/*
		 * Set DC_STALE on every matching entry; otherwise, we
		 * found our guy, so return.
		 */
		if (found && !(flag & DC_STALE))
			break;
	}
	PLL_ULOCK(&i->di_list);

	DYNARRAY_FOREACH(e, pos, &da)
		dircache_rls_ents(e, 0);
	psc_dynarray_free(&da);

	return (ino);
}

struct dircache_ents *
dircache_new_ents(struct dircache_info *i, size_t size)
{
	struct dircache_mgr *m = i->di_dcm;
	struct dircache_ents *e, *tmp;
	struct timeval now;

	PFL_GETTIMEVAL(&now);

	spinlock(&m->dcm_lock);
	m->dcm_alloc += size;

	/* Remove old entries from the top of the list.
	 */
	LIST_CACHE_FOREACH_SAFE(e, tmp, &m->dcm_lc) {
		dircache_ent_lock(e);
		if (timercmp(&now, &e->de_age, >) &&
		    (e->de_flags & DIRCE_FREEABLE) &&
		    !(e->de_flags & DIRCE_FREEING)) {
			e->de_flags |= DIRCE_FREEING;
			dircache_ent_ulock(e);
			dircache_rls_ents(e, DCFREEF_RELEASE);

		} else {
			dircache_ent_ulock(e);
			break;
		}
	}

	/* Clear more space if needed.
	 */
	while ((m->dcm_alloc + size) > m->dcm_maxsz &&
	       !lc_empty(&m->dcm_lc)) {
		e = lc_peekhead(&m->dcm_lc);
		if (!e)
			break;

		dircache_ent_lock(e);
		if ((e->de_flags & DIRCE_FREEABLE) &&
		    !(e->de_flags & DIRCE_FREEING)) {
			e->de_flags |= DIRCE_FREEING;
			dircache_ent_ulock(e);
			dircache_rls_ents(e, DCFREEF_RELEASE);
		} else
			dircache_ent_ulock(e);
	}
	freelock(&m->dcm_lock);

	e = psc_pool_get(dircache_pool);
	memset(e, 0, sizeof(*e));
	INIT_PSC_LISTENTRY(&e->de_lentry);
	INIT_PSC_LISTENTRY(&e->de_lentry_lc);
	psc_dynarray_init(&e->de_dents);
	e->de_sz = size;
	e->de_info = i;
	e->de_base = PSCALLOC(size);
	return (e);
}

/**
 * dircache_reg_ents
 */
void
dircache_reg_ents(struct dircache_ents *e, size_t nents)
{
	struct dircache_info *i = e->de_info;
	struct dircache_mgr  *m = i->di_dcm;
	struct pscfs_dirent *dirent;
	struct dircache_desc *c;
	unsigned char *b;
	off_t off;
	int j;

	psc_assert(e->de_sz);
	psc_assert(e->de_info);
	psc_assert(!e->de_desc);

	PFL_GETTIMEVAL(&e->de_age);
	e->de_age.tv_sec += dirent_timeo;
	e->de_remlookup = nents - 2; /* subtract "." and ".." */
	e->de_flags = 0; /* remain '0' until readdir req has completed */

	c = e->de_desc = PSCALLOC(sizeof(struct dircache_desc) * nents);
	psc_dynarray_ensurelen(&e->de_dents, nents);

	for (j = 0, b = e->de_base, off = 0; j < (int)nents; j++, c++) {
		dirent = (void *)(b + off);

		psc_dbg("ino=%"PRIx64" off=%"PRId64" "
		    "nlen=%u type=%#o "
		    "name=%.*s dirent=%p off=%"PRId64,
		    dirent->pfd_ino, dirent->pfd_off,
		    dirent->pfd_namelen, dirent->pfd_type,
		    dirent->pfd_namelen, dirent->pfd_name, dirent, off);

		c->dd_namelen	= dirent->pfd_namelen;
		c->dd_hash	= psc_strn_hashify(dirent->pfd_name, dirent->pfd_namelen);
		c->dd_flags	= 0;
		c->dd_offset	= off;
		c->dd_name	= dirent->pfd_name;

		psc_dynarray_add(&e->de_dents, c);
		off += PFL_DIRENT_SIZE(dirent->pfd_namelen);
	}

	psc_dynarray_sort(&e->de_dents, qsort, dirent_sort_cmp);
	DYNARRAY_FOREACH(c, j, &e->de_dents)
		psc_dbg("c=%p hash=%#x namelen=%u name=%.*s",
		    c, c->dd_hash, c->dd_namelen, c->dd_namelen, c->dd_name);

	lc_addtail(&m->dcm_lc, e);

	/* New entries are considered to be more accurate so place them
	 *   at the beginning of the list.
	 */
	pll_addhead(&i->di_list, e);

	fcmh_op_start_type(i->di_fcmh, FCMH_OPCNT_DIRENTBUF);
}
