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

/*
 * This interface describes the mechanisms and objects for  writing log
 * entries into the activity journal for recovering state when failure
 * occurs.
 */

#ifndef _SL_JFLUSH_
#define _SL_JFLUSH_

#include "psc_util/journal.h"

#include "sljournal.h"

typedef void (*jflush_handler)(void *);
typedef void (*jflush_prepcb)(void *);

struct jflush_item {
	psc_spinlock_t			 jfi_lock;
	struct psc_journal_xidhndl	*jfi_xh;
	struct psclist_head		 jfi_lentry;
	jflush_handler			 jfi_handler;
	jflush_prepcb			 jfi_prepcb;
	void				*jfi_item;		/* pointer to the item to be flushed */
	int				 jfi_state;
};

enum {
	JFI_QUEUED  = (1 << 0),
	JFI_HAVE_XH = (1 << 1),		/* transaction handle present */
	JFI_BUSY    = (1 << 2)
};

static __inline void
jfi_init(struct jflush_item *j, jflush_handler handler,
    jflush_prepcb prepcb, void *item)
{
	LOCK_INIT(&j->jfi_lock);
	INIT_PSCLIST_ENTRY(&j->jfi_lentry);
	j->jfi_handler = handler;
	j->jfi_prepcb = prepcb;
	j->jfi_item = item;
	j->jfi_state = 0;
}

static __inline void
jfi_ensure_empty(struct jflush_item *jfi)
{
	psc_assert(jfi->jfi_xh == NULL);
	psc_assert(psclist_disjoint(&jfi->jfi_lentry));
}

void jfi_prepare(struct jflush_item *, struct psc_journal *);
void jfi_schedule(struct jflush_item *, struct psc_listcache *);

#endif /* _SL_JFLUSH_ */
