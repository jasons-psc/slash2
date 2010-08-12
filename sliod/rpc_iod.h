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

#ifndef _RPC_IOD_H_
#define _RPC_IOD_H_

#include <sys/types.h>

#include "slconfig.h"
#include "slconn.h"

struct sli_repl_workrq;

#define SLI_RIM_NTHREADS	8
#define SLI_RIM_NBUFS		1024
#define SLI_RIM_BUFSZ		256
#define SLI_RIM_REPSZ		256
#define SLI_RIM_SVCNAME		"slirim"

#define SLI_RIC_NTHREADS	32
#define SLI_RIC_NBUFS		1024
#define SLI_RIC_BUFSZ		640
#define SLI_RIC_REPSZ		256
#define SLI_RIC_SVCNAME		"sliric"

#define SLI_RII_NTHREADS	8
#define SLI_RII_NBUFS		1024
#define SLI_RII_BUFSZ		256
#define SLI_RII_REPSZ		256
#define SLI_RII_SVCNAME		"slirii"

/* aliases for connection management */
#define sli_geticsvc(resm)							\
	sl_csvc_get(&(resm)->resm_csvc, 0, NULL, (resm)->resm_nid,		\
	    SRII_REQ_PORTAL, SRII_REP_PORTAL, SRII_MAGIC, SRII_VERSION,		\
	    &resm2rmii(resm)->rmii_lock, &resm2rmii(resm)->rmii_waitq, SLCONNT_IOD)

#define sli_getmcsvc(resm)							\
	sl_csvc_get(&(resm)->resm_csvc, 0, NULL, (resm)->resm_nid,		\
	    SRMI_REQ_PORTAL, SRMI_REP_PORTAL, SRMI_MAGIC, SRMI_VERSION,		\
	    &resm2rmii(resm)->rmii_lock, &resm2rmii(resm)->rmii_waitq, SLCONNT_MDS)

#define sli_ric_handle_read(rq)		sli_ric_handle_io((rq), SL_READ)
#define sli_ric_handle_write(rq)	sli_ric_handle_io((rq), SL_WRITE)

void	sli_rpc_initsvc(void);

int	sli_rim_handler(struct pscrpc_request *);
int	sli_ric_handler(struct pscrpc_request *);
int	sli_rii_handler(struct pscrpc_request *);

int	sli_rmi_getimp(struct slashrpc_cservice **);
int	sli_rmi_setmds(const char *);

int	sli_rmi_issue_repl_schedwk(struct sli_repl_workrq *);

int	sli_rii_issue_repl_read(struct pscrpc_import *, int, int, struct sli_repl_workrq *);

#endif /* _RPC_IOD_H_ */
