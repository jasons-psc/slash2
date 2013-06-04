/* $Id$ */
/*
 * %PSCGPL_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2011-2013, Pittsburgh Supercomputing Center (PSC).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed WITHOUT ANY WARRANTY; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License contained in the file
 * `COPYING-GPL' at the top of this distribution or at
 * https://www.gnu.org/licenses/gpl-2.0.html for more details.
 *
 * Pittsburgh Supercomputing Center	phone: 412.268.4960  fax: 412.268.5832
 * 300 S. Craig Street			e-mail: remarks@psc.edu
 * Pittsburgh, PA 15213			web: http://www.psc.edu/
 * -----------------------------------------------------------------------------
 * %PSC_END_COPYRIGHT%
 */

/*
 * odtable: on-disk table for persistent storage of otherwise memory
 * resident data structures.
 *
 * This API is complementary to the vanilla odtable API provided by PFL
 * to write the same format through the ZFS backend so the odtables are
 * contained within the ZFS backend metadata file system.
 */

#ifndef _ODTABLE_MDS_H_
#define _ODTABLE_MDS_H_

void	 mds_odtable_load(struct odtable **, const char *, const char *, ...);
struct odtable_receipt *
	 mds_odtable_putitem(struct odtable *, void *, size_t);
int	 mds_odtable_getitem(struct odtable *, const struct odtable_receipt *, void *, size_t);
int	 mds_odtable_freeitem(struct odtable *, struct odtable_receipt *);
void	 mds_odtable_replaceitem(struct odtable *, struct odtable_receipt *, void *, size_t);
void	 mds_odtable_release(struct odtable *);
void	 mds_odtable_scan(struct odtable *, int (*)(void *, struct odtable_receipt *, void *), void *);

#endif /* _ODTABLE_MDS_H_ */
