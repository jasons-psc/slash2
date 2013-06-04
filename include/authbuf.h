/* $Id$ */
/*
 * %PSCGPL_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2008-2013, Pittsburgh Supercomputing Center (PSC).
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
 * authbuf - routines for managing, signing, and checking the signatures
 * messages sent between hosts in a SLASH network for integrity with a
 * secret key.
 */

#ifndef _SL_AUTHBUF_H_
#define _SL_AUTHBUF_H_

#include "psc_util/atomic.h"

struct stat;
struct pscrpc_request;

#define AUTHBUF_KEYSIZE	1024

int	authbuf_check(struct pscrpc_request *, int);
void	authbuf_sign(struct pscrpc_request *, int);

void	authbuf_checkkey(const char *, struct stat *);
void	authbuf_checkkeyfile(void);
void	authbuf_createkeyfile(void);
void	authbuf_readkeyfile(void);

extern psc_atomic64_t	authbuf_nonce;
extern unsigned char	authbuf_key[AUTHBUF_KEYSIZE];
extern int		authbuf_alglen;

#endif /* _SL_AUTHBUF_H_ */
