/* $Id$ */
/*
 * %PSCGPL_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2009-2013, Pittsburgh Supercomputing Center (PSC).
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

#include <string.h>
#include <stdlib.h>

#include "pfl/cdefs.h"

#include "slerr.h"

char *pfl_errstrs[] = {
/*  0 */ "Bad message",
/*  1 */ "Key has expired",
/*  2 */ "No connection to peer",
/*  3 */ "Operation already in progress",
/*  4 */ "Operation not supported",
	  NULL
};

char *slash_errstrs[] = {
/*  0 */ "Specified replica(s) already exist",
/*  1 */ "Specified replica(s) do not exist",
/*  2 */ "",
/*  3 */ "",
/*  4 */ "Invalid bmap",
/*  5 */ "Bmap direct I/O must wait",
/*  6 */ "Uninitialized bmap",
/*  7 */ "Unknown resource",
/*  8 */ "Unknown I/O system",
/*  9 */ "Unknown I/O node",
/* 10 */ "I/O node connection could not be established",
/* 11 */ "Unable to remove last replica",
/* 12 */ "Transaction could not be started",
/* 13 */ "Short I/O",
/* 14 */ "Authorization buffer has bad magic",
/* 15 */ "Authorization buffer has a bad src/dst peer",
/* 16 */ "Authorization buffer has a bad hash",
/* 17 */ "Authorization buffer not provided",
/* 18 */ "User account does not exist",
/* 19 */ "Bad checksum",
/* 20 */ "File generation old",
/* 21 */ "File generation invalid",
/* 22 */ "CONNECT protocol message has not been transmitted",
/* 23 */ "Bmap is awaiting partial truncation resolution",
/* 24 */ "Bmap has started partial truncation resolution",
/* 25 */ "Asynchronous I/O would block",
/* 26 */ "Reimport failed because target is newer",
/* 27 */ "Import additional replica registration failed because source and target differ",
/* 28 */ "",
/* 29 */ "",
/* 30 */ "Peer resource is of wrong type",
/* 31 */ "Activity already in progress",
/* 32 */ "",
/* 33 */ "",
/* 34 */ "CRC absent",
/* 35 */ "Bad message",
/* 36 */ "Key expired",
	 NULL
};

char *
slstrerror(int error)
{
	error = abs(error);

	if (error >= _PFLERR_START &&
	    error < _PFLERR_START + nitems(pfl_errstrs))
		return (pfl_errstrs[error - _PFLERR_START]);

	if (error >= _SLERR_START &&
	    error < _SLERR_START + nitems(slash_errstrs))
		/* XXX ensure strerror(error) == unknown) */
		return (slash_errstrs[error - _SLERR_START]);
	return (strerror(error));
}
