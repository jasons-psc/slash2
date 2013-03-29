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

#include <sys/param.h>

#include <string.h>

#include "mdsio.h"
#include "slashd.h"

#include "pfl/cdefs.h"
#include "pfl/str.h"
#include "pfl/types.h"
#include "psc_ds/lockedlist.h"
#include "psc_util/alloc.h"
#include "psc_util/lock.h"
#include "psc_util/log.h"
#include "psc_util/odtable.h"
#include "psc_util/ctlsvr.h"

#include "odtable_mds.h"

extern int current_vfsid;

struct psc_lockedlist psc_odtables =
    PLL_INIT(&psc_odtables, struct odtable, odt_lentry);

/**
 * odtable_putitem - Store an item into an odtable.
 */
struct odtable_receipt *
mds_odtable_putitem(struct odtable *odt, void *data, size_t len)
{
	struct odtable_receipt *odtr;
	struct odtable_entftr *odtf;
	struct odtable_hdr *h;
	size_t elem, nb;
	uint64_t crc;
	void *p;
	int rc;

	h = odt->odt_hdr;
	psc_assert(len <= h->odth_elemsz);

	spinlock(&odt->odt_lock);
	if (psc_vbitmap_next(odt->odt_bitmap, &elem) <= 0) {
		OPSTAT_INCR(SLM_OPST_ODTABLE_FULL);
		freelock(&odt->odt_lock);
		return (NULL);
	}
	if (elem >= h->odth_nelems) {
		/*
		 * XXX either trust the bitmap or initialize the footer
		 * of new items
		 */
		h->odth_nelems = psc_vbitmap_getsize(odt->odt_bitmap);
		rc = mdsio_write(current_vfsid, &rootcreds, h,
		    sizeof(struct odtable_hdr), &nb, 0, 0,
		    odt->odt_handle, NULL, NULL);
		psc_assert(!rc && nb == sizeof(struct odtable_hdr));

		psclog_warnx("on-disk table now has %zd elements (used to be %zd)",
		    h->odth_nelems, elem);
	}

	freelock(&odt->odt_lock);

	p = PSCALLOC(h->odth_slotsz);
	memcpy(p, data, len);
	if (len < h->odth_elemsz)
		memset(p + len, 0, h->odth_elemsz - len);
	psc_crc64_calc(&crc, p, h->odth_elemsz);

	/*
	 * Overwrite all fields in case we extend the odtable above.
	 * Note that psc_vbitmap_next() already flips the bit under
	 * lock.
	 */
	odtf = p + h->odth_elemsz;
	odtf->odtf_crc = crc;
	odtf->odtf_inuse = ODTBL_INUSE;
	odtf->odtf_slotno = elem;
	odtf->odtf_magic = ODTBL_MAGIC;

	/* Setup and return the receipt. */
	odtr = PSCALLOC(sizeof(*odtr));
	odtr->odtr_elem = elem;
	odtr->odtr_key = crc;

	rc = mdsio_write(current_vfsid, &rootcreds, p, h->odth_slotsz,
	    &nb, h->odth_start + elem * h->odth_slotsz, 0,
	    odt->odt_handle, NULL, NULL);
	psc_assert(!rc && nb == h->odth_slotsz);

	if (h->odth_options & ODTBL_OPT_SYNC)
		mdsio_fsync(current_vfsid, &rootcreds, 0,
		    odt->odt_handle);

	psclog_info("putitem odtr=%p slot=%zd elemcrc=%"PSCPRIxCRC64" "
	    "rc=%d", odtr, odtr->odtr_elem, crc, rc);

	PSCFREE(p);
	return (odtr);
}

/**
 * odtable_getitem - Retrieve an item from an odtable.
 */
int
mds_odtable_getitem(struct odtable *odt,
    const struct odtable_receipt *odtr, void *data, size_t len)
{
	struct odtable_entftr *odtf;
	struct odtable_hdr *h;
	uint64_t crc;
	size_t nb;
	void *p;
	int rc;

	h = odt->odt_hdr;
	psc_assert(len <= h->odth_elemsz);
	psc_assert(odtr->odtr_elem <= h->odth_nelems - 1);

	p = PSCALLOC(h->odth_slotsz);
	rc = mdsio_read(current_vfsid, &rootcreds, p, h->odth_slotsz,
	    &nb, h->odth_start + odtr->odtr_elem * h->odth_slotsz,
	    odt->odt_handle);
	if (nb != h->odth_slotsz && !rc)
		rc = EIO;
	if (rc)
		goto out;

	odtf = p + h->odth_elemsz;
	if (odtable_footercheck(odtf, odtr, 1)) {
		rc = EINVAL;
		goto out;
	}

	if (h->odth_options & ODTBL_OPT_CRC) {
		psc_crc64_calc(&crc, p, h->odth_elemsz);
		if (crc != odtf->odtf_crc) {
			odtf->odtf_inuse = ODTBL_BAD;
			psclog_warnx("slot=%zd CRC fail "
			    "odtfcrc=%"PSCPRIxCRC64" "
			    "elemcrc=%"PSCPRIxCRC64,
			    odtr->odtr_elem, odtf->odtf_crc, crc);
			rc = EINVAL;
			goto out;
		}
	}
	memcpy(data, p, len);

 out:
	PSCFREE(p);
	return (rc);
}

struct odtable_receipt *
mds_odtable_replaceitem(struct odtable *odt,
    struct odtable_receipt *odtr, void *data, size_t len)
{
	struct odtable_entftr *odtf;
	struct odtable_hdr *h;
	uint64_t crc;
	size_t nb;
	void *p;
	int rc;

	h = odt->odt_hdr;
	psc_assert(len <= h->odth_elemsz);

	p = PSCALLOC(h->odth_slotsz);
	rc = mdsio_read(current_vfsid, &rootcreds, p,
	    h->odth_slotsz, &nb, h->odth_start +
	    odtr->odtr_elem * h->odth_slotsz,
	    odt->odt_handle);

	odtf = p + h->odth_elemsz;
	psc_assert(!odtable_footercheck(odtf, odtr, 1));

	memcpy(p, data, len);
	if (len < h->odth_elemsz)
		memset(p + len, 0, h->odth_elemsz - len);
	psc_crc64_calc(&crc, p, h->odth_elemsz);
	odtr->odtr_key = crc;
	odtf->odtf_crc = crc;

	psclog_info("replaceitem odtr=%p slot=%zd "
	    "elemcrc=%"PSCPRIxCRC64,
	    odtr, odtr->odtr_elem, crc);

	rc = mdsio_write(current_vfsid, &rootcreds, p,
	    h->odth_slotsz, &nb, h->odth_start +
	    odtr->odtr_elem * h->odth_slotsz, 0,
	    odt->odt_handle, NULL, NULL);
	psc_assert(!rc && nb == h->odth_slotsz);

	if (h->odth_options & ODTBL_OPT_SYNC)
		mdsio_fsync(current_vfsid, &rootcreds, 0,
		    odt->odt_handle);

	PSCFREE(p);
	return (odtr);
}

/**
 * odtable_freeitem - Free the odtable slot which corresponds to the
 *	provided receipt.
 * Note: odtr is freed here.
 */
int
mds_odtable_freeitem(struct odtable *odt, struct odtable_receipt *odtr)
{
	struct odtable_entftr *odtf;
	struct odtable_hdr *h;
	size_t nb;
	void *p;
	int rc;

	h = odt->odt_hdr;
	p = PSCALLOC(h->odth_slotsz);
	rc = mdsio_read(current_vfsid, &rootcreds, p, h->odth_slotsz,
	    &nb, h->odth_start + odtr->odtr_elem * h->odth_slotsz,
	    odt->odt_handle);

	odtf = p + h->odth_elemsz;
	psc_assert(!odtable_footercheck(odtf, odtr, 1));

	odtf->odtf_inuse = ODTBL_FREE;
	spinlock(&odt->odt_lock);
	psc_vbitmap_unset(odt->odt_bitmap, odtr->odtr_elem);
	freelock(&odt->odt_lock);

	rc = mdsio_write(current_vfsid, &rootcreds, p, h->odth_slotsz,
	    &nb, h->odth_start + odtr->odtr_elem *
	    h->odth_slotsz, 0, odt->odt_handle, NULL, NULL);
	psc_assert(!rc && nb == h->odth_slotsz);

	if (h->odth_options & ODTBL_OPT_SYNC)
		mdsio_fsync(current_vfsid, &rootcreds, 0,
		    odt->odt_handle);

	psclog_info("freeitem odtr=%p slot=%zd elemcrc=%"PSCPRIxCRC64,
	    odtr, odtr->odtr_elem, odtf->odtf_crc);

	PSCFREE(p);
	PSCFREE(odtr);
	return (rc);
}

void
mds_odtable_load(struct odtable **t, const char *fn, const char *fmt, ...)
{
	struct odtable *odt = PSCALLOC(sizeof(struct odtable));
	struct odtable_entftr *odtf;
	struct odtable_receipt odtr;
	struct odtable_hdr *odth;
	mdsio_fid_t mf;
	size_t nb, i;
	int rc, frc;
	va_list ap;
	void *p;

	psc_assert(t);
	*t = NULL;

	INIT_SPINLOCK(&odt->odt_lock);

	rc = mdsio_lookup(current_vfsid,
	    mds_metadir_inum[current_vfsid], fn, &mf, &rootcreds, NULL);
	psc_assert(rc == 0);

	rc = mdsio_opencreate(current_vfsid, mf, &rootcreds, O_RDWR, 0,
	    NULL, NULL, NULL, &odt->odt_handle, NULL, NULL, 0);
	if (rc || !odt->odt_handle)
		psc_fatalx("failed to open odtable %s, rc=%d", fn, rc);

	odth = PSCALLOC(sizeof(*odth));
	rc = mdsio_read(current_vfsid, &rootcreds, odth, sizeof(*odth),
	    &nb, 0, odt->odt_handle);
	odt->odt_hdr = odth;
	psc_assert(rc == 0 && nb == sizeof(*odth));

	psc_assert((odth->odth_magic == ODTBL_MAGIC) &&
		   (odth->odth_version == ODTBL_VERS));

	/*
	 * We used to do mmap() to allow easy indexing.  However, we now
	 * support auto growth of the bitmap.  Plus, ZFS fuse does NOT
	 * like mmap() either.
	 */
	odt->odt_bitmap = psc_vbitmap_newf(odth->odth_nelems,
	    PVBF_AUTO);
	psc_assert(odt->odt_bitmap);

	p = PSCALLOC(odth->odth_slotsz);
	for (i = 0; i < odth->odth_nelems; i++) {
		rc = mdsio_read(current_vfsid, &rootcreds, p,
		    odth->odth_slotsz, &nb,
		    odth->odth_start + i *
		    odth->odth_slotsz, odt->odt_handle);

		odtr.odtr_elem = i;
		odtf = p + odth->odth_elemsz;
		frc = odtable_footercheck(odtf, &odtr, -1);

		/* Sanity checks for debugging. */
		psc_assert(frc != ODTBL_MAGIC_ERR);
		psc_assert(frc != ODTBL_SLOT_ERR);

		if (odtf->odtf_inuse == ODTBL_FREE)
			psc_vbitmap_unset(odt->odt_bitmap, i);

		else if (odtf->odtf_inuse == ODTBL_INUSE) {
			psc_vbitmap_set(odt->odt_bitmap, i);

			if (odth->odth_options & ODTBL_OPT_CRC) {
				uint64_t crc;

				psc_crc64_calc(&crc, p,
				    odth->odth_elemsz);
				if (crc != odtf->odtf_crc) {
					odtf->odtf_inuse = ODTBL_BAD;
					psclog_warnx("slot=%zd CRC fail "
					    "odtfcrc=%"PSCPRIxCRC64" "
					    "elemcrc=%"PSCPRIxCRC64,
					    i, odtf->odtf_crc, crc);
				}
			}
		} else {
			psc_vbitmap_set(odt->odt_bitmap, i);
			psclog_warnx("slot=%zd ignoring, bad inuse value"
			    "inuse=%#"PRIx64,
			    i, odtf->odtf_inuse);
		}
	}

	psclog_info("odtable=%p base=%p has %d/%zd slots available "
	    "elemsz=%zd magic=%#"PRIx64,
	    odt, odt->odt_base, psc_vbitmap_nfree(odt->odt_bitmap),
	    odth->odth_nelems, odth->odth_elemsz, odth->odth_magic);

	INIT_PSC_LISTENTRY(&odt->odt_lentry);

	va_start(ap, fmt);
	vsnprintf(odt->odt_name, sizeof(odt->odt_name), fmt, ap);
	va_end(ap);

	*t = odt;
	pll_add(&psc_odtables, odt);
}

void
mds_odtable_release(struct odtable *odt)
{
	psc_vbitmap_free(odt->odt_bitmap);
	odt->odt_bitmap = NULL;

	PSCFREE(odt->odt_hdr);
	mdsio_fsync(current_vfsid, &rootcreds, 0, odt->odt_handle);
	mdsio_release(current_vfsid, &rootcreds, odt->odt_handle);
	PSCFREE(odt);
}

void
mds_odtable_scan(struct odtable *odt,
    int (*odt_handler)(void *, struct odtable_receipt *, void *),
    void *arg)
{
	struct odtable_receipt *odtr = NULL;
	struct odtable_entftr *odtf;
	struct odtable_hdr *h;
	size_t i, nb;
	void *p;
	int rc;

	h = odt->odt_hdr;
	psc_assert(odt_handler);

	odtr = NULL;
	p = PSCALLOC(h->odth_slotsz);
	for (i = 0; i < h->odth_nelems; i++) {
		if (!odtr)
			odtr = PSCALLOC(sizeof(*odtr));
		if (!psc_vbitmap_get(odt->odt_bitmap, i))
			continue;
		rc = mdsio_read(current_vfsid, &rootcreds, p,
		    h->odth_slotsz, &nb,
		    h->odth_start +
		    i * h->odth_slotsz, odt->odt_handle);
		if (rc)
			psclog_warnx("fail to read slot=%zd, "
			    "skipping (rc=%d)", i, rc);
		odtf = p + h->odth_elemsz;

		odtr->odtr_elem = i;
		odtr->odtr_key = odtf->odtf_key;

		rc = odtable_footercheck(odtf, odtr, 2);
		psc_assert(rc != ODTBL_FREE_ERR);
		if (rc) {
			psclog_warnx("slot=%zd marked bad, "
			    "skipping (rc=%d)", i, rc);
			continue;
		}

		psclog_debug("handing back key=%"PSCPRIxCRC64" "
		    "slot=%zd odtr=%p",
		    odtr->odtr_key, i, odtr);

		rc = odt_handler(p, odtr, arg);
		odtr = NULL;
		if (rc)
			break;
	}
	PSCFREE(p);
	PSCFREE(odtr);
}
