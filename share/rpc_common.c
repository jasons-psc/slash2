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

#include <sys/types.h>
#include <sys/statvfs.h>

#include <stdio.h>

#include "psc_ds/list.h"
#include "psc_rpc/export.h"
#include "psc_rpc/rpc.h"
#include "psc_rpc/rsx.h"
#include "psc_util/alloc.h"
#include "psc_util/lock.h"
#include "psc_util/multiwait.h"

#include "slashrpc.h"
#include "slconfig.h"
#include "slconn.h"

struct psc_lockedlist	client_csvcs = PLL_INIT(&client_csvcs,
    struct slashrpc_cservice, csvc_lentry);

/**
 * slrpc_issue_connect - Attempt connection initiation with a peer.
 * @server: NID of server peer.
 * @imp: import (connection structure) to peer.
 * @magic: agreed-upon connection message key.
 * @version: communication protocol version.
 */
__static int
slrpc_issue_connect(lnet_nid_t server, struct pscrpc_import *imp,
    uint64_t magic, uint32_t version, int flags)
{
	lnet_process_id_t prid, server_id = { server, PSCRPC_SVR_PID };
	struct pscrpc_request *rq;
	struct srm_connect_req *mq;
	struct srm_generic_rep *mp;
	int rc;

	pscrpc_getpridforpeer(&prid, &lnet_prids, server);
	if (prid.nid == LNET_NID_ANY)
		return (ENETUNREACH);

	if (imp->imp_connection)
		pscrpc_put_connection(imp->imp_connection);
	imp->imp_connection = pscrpc_get_connection(server_id, prid.nid, NULL);
	imp->imp_connection->c_imp = imp;
	imp->imp_connection->c_peer.pid = PSCRPC_SVR_PID;

	rc = SL_RSX_NEWREQ(imp, version, SRMT_CONNECT, rq, mq, mp);
	if (rc)
		return (rc);
	mq->magic = magic;
	mq->version = version;

	/*
	 * XXX in this case, we should do an async deal and return NULL
	 * for temporary failure.
	 */
	if (flags & CSVCF_NONBLOCK)
		rq->rq_timeout = 3;

	rc = SL_RSX_WAITREP(rq, mp);
	if (rc == 0)
		rc = mp->rc;
	if (rc == 0)
		imp->imp_state = PSCRPC_IMP_FULL;
	pscrpc_req_finished(rq);
	return (rc);
}

__static int
slrpc_issue_ping(struct slashrpc_cservice *csvc, uint32_t version)
{
	struct pscrpc_request *rq;
	struct srm_generic_rep *mp;
	struct srm_ping_req *mq;
	int rc;

	rc = SL_RSX_NEWREQ(csvc->csvc_import, version,
	    SRMT_PING, rq, mq, mp);
	if (rc)
		return (rc);
	rc = SL_RSX_WAITREP(rq, mp);
	if (rc == 0)
		rc = mp->rc;
	pscrpc_req_finished(rq);
	return (rc);
}

__weak void
psc_multiwaitcond_wakeup(__unusedx struct psc_multiwaitcond *arg)
{
	psc_fatalx("unimplemented stub");
}

__weak int
psc_multiwaitcond_waitrel(__unusedx struct psc_multiwaitcond *arg,
    __unusedx pthread_mutex_t *mutex, __unusedx const struct timespec *ts)
{
	psc_fatalx("unimplemented stub");
}

void
sl_csvc_wake(struct slashrpc_cservice *csvc)
{
	sl_csvc_lock_ensure(csvc);
	if (sl_csvc_usemultiwait(csvc))
		psc_multiwaitcond_wakeup(csvc->csvc_waitinfo);
	else
		psc_waitq_wakeall(csvc->csvc_waitinfo);
}

void
_sl_csvc_waitrelv(struct slashrpc_cservice *csvc, long s, long ns)
{
	struct timespec ts;

	ts.tv_sec = s;
	ts.tv_nsec = ns;

	sl_csvc_lock_ensure(csvc);
	if (sl_csvc_usemultiwait(csvc))
		psc_multiwaitcond_waitrel(csvc->csvc_waitinfo,
		    csvc->csvc_mutex, &ts);
	else
		psc_waitq_waitrel(csvc->csvc_waitinfo,
		    csvc->csvc_lock, &ts);
}

void
sl_csvc_lock(struct slashrpc_cservice *csvc)
{
	if (sl_csvc_usemultiwait(csvc))
		psc_pthread_mutex_lock(csvc->csvc_mutex);
	else
		spinlock(csvc->csvc_lock);
}

void
sl_csvc_unlock(struct slashrpc_cservice *csvc)
{
	if (sl_csvc_usemultiwait(csvc))
		psc_pthread_mutex_unlock(csvc->csvc_mutex);
	else
		freelock(csvc->csvc_lock);
}

int
sl_csvc_reqlock(struct slashrpc_cservice *csvc)
{
	if (sl_csvc_usemultiwait(csvc))
		return (psc_pthread_mutex_reqlock(csvc->csvc_mutex));
	return (reqlock(csvc->csvc_lock));
}

void
sl_csvc_ureqlock(struct slashrpc_cservice *csvc, int locked)
{
	if (sl_csvc_usemultiwait(csvc))
		psc_pthread_mutex_ureqlock(csvc->csvc_mutex, locked);
	else
		ureqlock(csvc->csvc_lock, locked);
}

void
sl_csvc_lock_ensure(struct slashrpc_cservice *csvc)
{
	if (sl_csvc_usemultiwait(csvc))
		psc_pthread_mutex_ensure_locked(csvc->csvc_mutex);
	else
		LOCK_ENSURE(csvc->csvc_lock);
}

int
sl_csvc_usemultiwait(struct slashrpc_cservice *csvc)
{
	return (psc_atomic32_read(&csvc->csvc_flags) & CSVCF_USE_MULTIWAIT);
}

/**
 * sl_csvc_useable - Determine service connection useability.
 * @csvc: client service.
 */
int
sl_csvc_useable(struct slashrpc_cservice *csvc)
{
	sl_csvc_lock_ensure(csvc);
	if (csvc->csvc_import->imp_failed ||
	    csvc->csvc_import->imp_invalid)
		return (0);
	return ((psc_atomic32_read(&csvc->csvc_flags) &
	  (CSVCF_CONNECTED | CSVCF_ABANDON)) == CSVCF_CONNECTED);
}

/**
 * sl_csvc_markfree - Mark that a connection will be freed when the last
 *	reference goes away.  This should never be performed on service
 *	connections on resms, only for service connections to clients.
 * @csvc: client service.
 */
void
sl_csvc_markfree(struct slashrpc_cservice *csvc)
{
	int locked;

	locked = sl_csvc_reqlock(csvc);
	psc_atomic32_setmask(&csvc->csvc_flags, CSVCF_ABANDON | CSVCF_WANTFREE);
	psc_atomic32_clearmask(&csvc->csvc_flags, CSVCF_CONNECTED | CSVCF_CONNECTING);
	sl_csvc_ureqlock(csvc, locked);
}

/**
 * sl_csvc_incref - Account for releasing the use of a remote service connection.
 * @csvc: client service.
 */
void
sl_csvc_decref(struct slashrpc_cservice *csvc)
{
	int rc;

	sl_csvc_reqlock(csvc);
	rc = psc_atomic32_dec_getnew(&csvc->csvc_refcnt);
	psc_assert(rc >= 0);
	if (rc == 0) {
		sl_csvc_wake(csvc);
		if (psc_atomic32_read(&csvc->csvc_flags) & CSVCF_WANTFREE) {
			/*
			 * This should only apply to mount_slash clients the MDS
			 * stops communication with.
			 */
			pscrpc_import_put(csvc->csvc_import);
			if (csvc->csvc_ctype == SLCONNT_CLI)
				pll_remove(&client_csvcs, csvc);
			PSCFREE(csvc);
			return;
		}
	}
	sl_csvc_unlock(csvc);
}

/**
 * sl_csvc_incref - Account for starting to use a remote service connection.
 * @csvc: client service.
 */
void
sl_csvc_incref(struct slashrpc_cservice *csvc)
{
	sl_csvc_lock_ensure(csvc);
	psc_atomic32_inc(&csvc->csvc_refcnt);
}

/**
 * sl_csvc_disconnect - Perform actual network disconnect to a remote
 *	service.
 * @csvc: client service.
 */
void
sl_csvc_disconnect(struct slashrpc_cservice *csvc)
{
	int locked;

	locked = sl_csvc_reqlock(csvc);
	psc_atomic32_clearmask(&csvc->csvc_flags, CSVCF_CONNECTED);
	sl_csvc_wake(csvc);
	sl_csvc_ureqlock(csvc, locked);

	pscrpc_abort_inflight(csvc->csvc_import);
}

/**
 * sl_csvc_disable - Mark a connection as no longer available.
 * @csvc: client service.
 */
void
sl_csvc_disable(struct slashrpc_cservice *csvc)
{
	int locked;

	locked = sl_csvc_reqlock(csvc);
	psc_atomic32_setmask(&csvc->csvc_flags, CSVCF_ABANDON);
	psc_atomic32_clearmask(&csvc->csvc_flags, CSVCF_CONNECTED | CSVCF_CONNECTING);
	sl_csvc_wake(csvc);
	sl_csvc_ureqlock(csvc, locked);
}

/**
 * sl_csvc_create - Create a new client RPC service.
 * @rqptl: request portal ID.
 * @rpptl: reply portal ID.
 */
__static struct slashrpc_cservice *
sl_csvc_create(uint32_t rqptl, uint32_t rpptl)
{
	struct slashrpc_cservice *csvc;
	struct pscrpc_import *imp;

	csvc = PSCALLOC(sizeof(*csvc));
	INIT_PSC_LISTENTRY(&csvc->csvc_lentry);

	if ((imp = pscrpc_new_import()) == NULL)
		psc_fatalx("pscrpc_new_import");
	csvc->csvc_import = imp;

	imp->imp_client->cli_request_portal = rqptl;
	imp->imp_client->cli_reply_portal = rpptl;
	imp->imp_max_retries = 2;
	imp->imp_igntimeout = 1;
	return (csvc);
}

/**
 * sl_csvc_get - Acquire or create a client RPC service.
 * @csvcp: value-result permanent storage for connection structures.
 * @flags: CSVCF_* flags the connection should take on, only used for
 *	csvc initialization.
 * @exp: RPC peer export.  This or @peernid is required.
 * @peernid: RPC peer network address (NID).  This or @exp is required.
 * @rqptl: request portal ID.
 * @rpptl: reply portal ID.
 * @magic: connection magic bits.
 * @version: version of application protocol.
 * @lockp: point to lock for mutually exclusive access to critical
 *	sections involving this connection structure, whereever @csvcp
 *	is stored.
 * @waitinfo: waitq or multiwait argument to wait/wakeup depending on
 *	connection availability.
 * @ctype: peer type.
 *
 * If we acquire a connection successfully, this function will return
 * the same slashrpc_cservice struct pointer as referred to by its
 * first argument csvcp.  Otherwise, it returns NULL, but the structure
 * is left in the location referred to by csvcp for retry.
 */
struct slashrpc_cservice *
sl_csvc_get(struct slashrpc_cservice **csvcp, int flags,
    struct pscrpc_export *exp, lnet_nid_t peernid, uint32_t rqptl,
    uint32_t rpptl, uint64_t magic, uint32_t version,
    void *lockp, void *waitinfo, enum slconn_type ctype)
{
	struct slashrpc_cservice *csvc;
	struct sl_resm *resm;
	int rc = 0, locked;

	if (flags & CSVCF_USE_MULTIWAIT)
		locked = psc_pthread_mutex_reqlock(lockp);
	else
		locked = reqlock(lockp);
	if (exp)
		peernid = exp->exp_connection->c_peer.nid;
	psc_assert(peernid != LNET_NID_ANY);

	csvc = *csvcp;
	if (csvc == NULL) {
		/* ensure that our peer is of the given resource type (REST) */
		switch (ctype) {
		case SLCONNT_CLI:
			break;
		case SLCONNT_IOD:
			resm = libsl_nid2resm(peernid);
			if (resm->resm_res->res_type == SLREST_MDS)
				goto out;
			break;
		case SLCONNT_MDS:
			resm = libsl_nid2resm(peernid);
			if (resm->resm_res->res_type != SLREST_MDS)
				goto out;
			break;
		default:
			psc_fatalx("%d: bad connection type", ctype);
		}

		/* initialize service */
		csvc = *csvcp = sl_csvc_create(rqptl, rpptl);
		psc_atomic32_set(&csvc->csvc_flags, flags);
		csvc->csvc_lockinfo.lm_ptr = lockp;
		csvc->csvc_waitinfo = waitinfo;
		csvc->csvc_ctype = ctype;

		if (ctype == SLCONNT_CLI)
			pll_add(&client_csvcs, csvc);
	}

 restart:
	if (sl_csvc_useable(csvc))
		goto out;

	if (exp) {
		struct pscrpc_connection *c;
		/*
		 * If an export was specified, the peer has already
		 * established a connection to our service, so just
		 * reuse the underhood connection to establish a
		 * connection back to his service.
		 *
		 * The idea is to share the same connection between
		 * an export and an import.  Note we use a local
		 * variable to keep the existing connection intact
		 * until the export connection is assigned to us.
		 */
		c = csvc->csvc_import->imp_connection;

		atomic_inc(&exp->exp_connection->c_refcount);
		csvc->csvc_import->imp_connection = exp->exp_connection;
		csvc->csvc_import->imp_connection->c_imp = csvc->csvc_import;

		if (c)
			pscrpc_put_connection(c);

		psc_atomic32_setmask(&csvc->csvc_flags, CSVCF_CONNECTED);
		csvc->csvc_mtime = time(NULL);

	} else if (psc_atomic32_read(&csvc->csvc_flags) & CSVCF_CONNECTING) {
		if (flags & CSVCF_NONBLOCK) {
			csvc = NULL;
			goto out;
		}

		if (sl_csvc_usemultiwait(csvc)) {
			psc_fatalx("multiwaits not implemented");
//			psc_multiwait_addcond(ml, wakearg);
//			csvc = NULL;
//			goto out;
		} else {
			psc_waitq_wait(csvc->csvc_waitinfo, csvc->csvc_lock);
			sl_csvc_lock(csvc);
		}
		goto restart;
	} else if (csvc->csvc_mtime + CSVC_RECONNECT_INTV < time(NULL)) {
		psc_atomic32_setmask(&csvc->csvc_flags, CSVCF_CONNECTING);
		sl_csvc_unlock(csvc);

		rc = slrpc_issue_connect(peernid,
		    csvc->csvc_import, magic, version, flags);

		sl_csvc_lock(csvc);
		psc_atomic32_clearmask(&csvc->csvc_flags, CSVCF_CONNECTING);
		csvc->csvc_mtime = time(NULL);
		if (rc) {
			csvc->csvc_import->imp_failed = 1;
			csvc->csvc_lasterrno = rc;
			/*
			 * Leave the slashrpc_cservice structure in csvcp intact,
			 * while return NULL to signal that we fail to establish
			 * a connection.
			 */
			csvc = NULL;
			goto out;
		} else
			psc_atomic32_setmask(&csvc->csvc_flags, CSVCF_CONNECTED);
	} else {
		rc = csvc->csvc_lasterrno;
		csvc = NULL;
		goto out;
	}
	if (rc == 0) {
		csvc->csvc_import->imp_failed = 0;
		csvc->csvc_import->imp_invalid = 0;
		sl_csvc_wake(csvc);
	}

 out:
	if (csvc)
		sl_csvc_incref(csvc);
	if (flags & CSVCF_USE_MULTIWAIT)
		psc_pthread_mutex_ureqlock(lockp, locked);
	else
		ureqlock(lockp, locked);
	return (csvc);
}

void
slconnthr_main(struct psc_thread *thr)
{
	struct slashrpc_cservice *csvc;
	struct slconn_thread *sct;
	struct sl_resm *resm;
	int woke, rc;

	sct = thr->pscthr_private;
	resm = sct->sct_resm;
	if (sct->sct_flags & CSVCF_USE_MULTIWAIT)
		psc_pthread_mutex_lock(sct->sct_lockinfo.lm_mutex);
	else
		spinlock(sct->sct_lockinfo.lm_lock);
	do {
		if (sct->sct_flags & CSVCF_USE_MULTIWAIT)
			psc_pthread_mutex_unlock(sct->sct_lockinfo.lm_mutex);
		else
			freelock(sct->sct_lockinfo.lm_lock);

		/* Now just PING for connection lifetime. */
		woke = 0;
		for (;;) {
			csvc = sl_csvc_get(&resm->resm_csvc, sct->sct_flags,
			    NULL, resm->resm_nid, sct->sct_rqptl, sct->sct_rpptl,
			    sct->sct_magic, sct->sct_version,
			    sct->sct_lockinfo.lm_ptr, sct->sct_waitinfo,
			    sct->sct_conntype);

			if (csvc == NULL) {
				time_t mtime;

				sl_csvc_lock(resm->resm_csvc);
				csvc = resm->resm_csvc;
				if (sl_csvc_useable(csvc)) {
					sl_csvc_incref(csvc);
					goto online;
				}
				mtime = csvc->csvc_mtime;
				/*
				 * Allow manual activity to try to
				 * reconnect while we wait.
				 */
				csvc->csvc_mtime = 0;
				/*
				 * Subtract the amount of time someone
				 * manually retried (and failed) instead of
				 * waiting an entire interval after we woke
				 * after our last failed attempt.
				 */
printf("waiting %lu sec\n", CSVC_RECONNECT_INTV - (time(NULL) - mtime));
				sl_csvc_waitrel_s(csvc,
				    CSVC_RECONNECT_INTV - (time(NULL) -
				    mtime));
				continue;
			}

			sl_csvc_lock(csvc);
			if (!sl_csvc_useable(csvc)) {
				sl_csvc_decref(csvc);
				break;
			}

			if (!woke) {
				/*
				 * Wake once whenever the connection
				 * reestablishes.
				 */
				sl_csvc_wake(csvc);
				woke = 1;
			}
 online:
			sl_csvc_unlock(csvc);
			rc = slrpc_issue_ping(csvc, sct->sct_version);
			/* XXX race */
			if (rc) {
				sl_csvc_lock(csvc);
				sl_csvc_disconnect(csvc);
			}
			sl_csvc_decref(csvc);

			if (rc)
				break;

			sl_csvc_lock(csvc);
			sl_csvc_waitrel_s(csvc, 60);
		}

		sl_csvc_lock(csvc);
	} while (pscthr_run() && (psc_atomic32_read(
	    &csvc->csvc_flags) & CSVCF_ABANDON) == 0);
	sl_csvc_decref(csvc);
}

void
slconnthr_spawn(struct sl_resm *resm, uint32_t rqptl, uint32_t rpptl,
    uint64_t magic, uint32_t version, void *lockp, int flags,
    void *waitinfo, enum slconn_type conntype, int thrtype,
    const char *thrnamepre)
{
	struct slconn_thread *sct;
	struct psc_thread *thr;

	thr = pscthr_init(thrtype, 0, slconnthr_main, NULL,
	    sizeof(*sct), "%sconnthr-%s", thrnamepre,
	    resm->resm_res->res_name);
	sct = thr->pscthr_private;
	sct->sct_resm = resm;
	sct->sct_rqptl = rqptl;
	sct->sct_rpptl = rpptl;
	sct->sct_magic = magic;
	sct->sct_version = version;
	sct->sct_lockinfo.lm_ptr = lockp;
	sct->sct_flags = flags;
	sct->sct_waitinfo = waitinfo;
	sct->sct_conntype = conntype;
	pscthr_setready(thr);
}
