/* $Id$ */

#include "mount_slash.h"

#include "psc_rpc/rpc.h"
#include "psc_util/cdefs.h"
#include "psc_ds/list.h"

#define SLASH_SVR_PID 54321

typedef int (*rpcsvc_connect_t)(lnet_nid_t, int, u64, u32);

struct rpcsvc {
	struct pscrpc_import	*svc_import;
	lnet_nid_t		 svc_default_server_id;
	struct psclist_head	 svc_old_imports;
	psc_spinlock_t		 svc_lock;
	int			 svc_failed;
	int			 svc_initialized;
	rpcsvc_connect_t	 svc_connect;
};

struct rpcsvc *rpcsvcs[NRPCSVC];
struct pscrpc_nbreqset *ioNbReqSet;

int
rpc_io_interpret_set(struct pscrpc_request_set *set, void *arg,
    int status)
{
        struct pscrpc_request *req;
        int rc = 0;

        /*
         * zestrpc_set_wait() already does this for us but it
         *  doesn't abort.
         */
        psclist_for_each_entry(req, &set->set_requests, rq_set_chain) {
                LASSERT(req->rq_phase == ZRQ_PHASE_COMPLETE);
                if (req->rq_status != 0) {
                        /* sanity check */
                        psc_assert(status);
                        rc = req->rq_status;
                }
        }
        if (rc)
                psc_fatalx("Some I/O reqs could not be completed");
        return (rc);
}

/**
 * rpc_nbcallback - async op completion callback
 */
int
rpc_nbcallback(struct pscrpc_request *req,
    struct pscrpc_async_args *cb_args)
{
#if 0
        psc_stream_buffer_t *zsb;

        /*
         * Catch bad status here, we can't proceed if a
         *  nb buffer did not send properly.
         */
        if (req->rq_status)
                zfatalx("I/O req could not be completed");

        zsb = cb_args->pointer_arg[ZSB_CB_POINTER_SLOT];
        zest_assert(zsb);
        zest_assert(zsb->zsb_zcf);
	zlist_del(zsb->zsb_ent);
        return (zest_buffer_free(zsb));
#endif
return 0;
}

int
rpc_connect(lnet_nid_t server, int ptl, u64 magic, u32 version)
{
	lnet_process_id_t server_id = { server, 0 };
	struct slashrpc_connect_req *mq;
	struct slashrpc_connect_rep *mp;
	struct pscrpc_request *rq;
	struct pscrpc_import *imp;
	lnet_process_id_t id;
        int size, rc;

	if (LNetGetId(1, &id))
                psc_fatalx("LNetGetId");

	imp = rpcsvcs[ptl]->svc_import;
	imp->imp_connection = pscrpc_get_connection(server_id, id.nid, NULL);
	imp->imp_connection->c_peer.pid = SLASH_SVR_PID;

	size = sizeof(*mq);
	rq = pscrpc_prep_req(imp, version, SRMT_CONNECT, 1, &size, NULL);
	if (rq == NULL)
                return (-ENOMEM);

	mq = psc_msg_buf(rq->rq_reqmsg, 0, sizeof(*mq));
        if (mq == NULL) {
		/* XXX leak req */
                psc_warnx("psc_msg_buf");
		return -1;
        }

	mq->version = version;
	mq->magic = magic;

	/* Setup buffer for response. */
	size = sizeof(*mp);
        rq->rq_replen = psc_msg_size(1, &size);

	rc = pscrpc_queue_wait(rq);
	if (rc) {
		errno = -rc;
		psc_fatal("connect failed");
		return (rc);
	}

	/* Save server PID from reply callback and mark initialized.  */
	imp->imp_connection->c_peer.pid = rq->rq_peer.pid;
	imp->imp_state = PSC_IMP_FULL;
	return (0);
}

struct rpcsvc *
rpc_svc_create(__unusedx lnet_nid_t server, u32 rqptl, u32 rpptl,
    rpcsvc_connect_t fconn)
{
	struct rpcsvc *svc;

	svc = PSCALLOC(sizeof(*svc));

	INIT_PSCLIST_HEAD(&svc->svc_old_imports);
	LOCK_INIT(&svc->svc_lock);

	svc->svc_failed = 0;
	svc->svc_initialized = 0;

	if ((svc->svc_import = new_import()) == NULL)
		psc_fatalx("new_import");

	svc->svc_import->imp_client =
	    PSCALLOC(sizeof(*svc->svc_import->imp_client));
	svc->svc_import->imp_client->cli_request_portal = rqptl;
	svc->svc_import->imp_client->cli_reply_portal = rpptl;

	svc->svc_import->imp_max_retries = 2;

	svc->svc_connect = fconn;
	return (svc);
}

int
rpc_svc_init(void)
{
	struct rpcsvc *svc;
	lnet_nid_t nid;
	char *snid;

	/* Setup client MDS service */
	snid = getenv("SLASH_SERVER_NID");
	if (snid == NULL)
		psc_fatalx("SLASH_RPC_SERVER_NID not set");
	nid = libcfs_str2nid(snid);
	if (nid == LNET_NID_ANY)
		psc_fatalx("invalid SLASH_SERVER_NID: %s", snid);

	svc = rpc_svc_create(nid, RPCMDS_REQ_PORTAL,
	    RPCMDS_REP_PORTAL, rpc_connect);

	rpcsvcs[RPCSVC_MDS] = svc;
	if (rpc_connect(nid, RPCSVC_MDS, SMDS_CONNECT_MAGIC, SMDS_VERSION))
		psc_error("rpc_mds_connect %s", snid);

	/* Setup client I/O service */
	svc = rpc_svc_create(nid, RPCIO_REQ_PORTAL,
	    RPCIO_REP_PORTAL, rpc_connect);

	rpcsvcs[RPCSVC_IO] = svc;
	if (rpc_connect(nid, RPCSVC_IO, SIO_CONNECT_MAGIC, SIO_VERSION))
		psc_error("rpc_io_connect %s", snid);

	/* Initialize manager for single-block, non-blocking requests */
	ioNbReqSet = nbreqset_init(rpc_io_interpret_set, rpc_nbcallback);
	if (ioNbReqSet == NULL)
		psc_fatal("nbreqset_init");
	return (0);
}

int
rpc_sendmsg(int op, ...)
{
	union {
		struct slashrpc_access_req	*m_access;
		struct slashrpc_chmod_req	*m_chmod;
		struct slashrpc_chown_req	*m_chown;
		struct slashrpc_link_req	*m_link;
		struct slashrpc_mkdir_req	*m_mkdir;
		struct slashrpc_rename_req	*m_rename;
		struct slashrpc_rmdir_req	*m_rmdir;
		struct slashrpc_symlink_req	*m_symlink;
		struct slashrpc_truncate_req	*m_truncate;
		struct slashrpc_unlink_req	*m_unlink;
		struct slashrpc_utimes_req	*m_utimes;
		void				*m;
	} u;
	struct pscrpc_request *rq;
	struct pscrpc_import *imp;
	int rc, msglen;
	va_list ap;

	imp = rpcsvcs[RPCSVC_MDS]->svc_import;

#define SRMT_ALLOC(var)							\
	do {								\
		msglen = sizeof(*(var));				\
		(var) = psc_msg_buf(rq->rq_reqmsg, 0, msglen);	\
	} while (0)

	va_start(ap, op);
	switch (op) {
	case SRMT_ACCESS:
		SRMT_ALLOC(u.m_access);
		snprintf(u.m_access->path, sizeof(u.m_access->path),
		    "%s", va_arg(ap, const char *));
		u.m_access->mask = va_arg(ap, int);
		break;
	case SRMT_CHMOD:
		SRMT_ALLOC(u.m_chmod);
		snprintf(u.m_chmod->path, sizeof(u.m_chmod->path),
		    "%s", va_arg(ap, const char *));
		u.m_chmod->mode = va_arg(ap, mode_t);
		break;
	case SRMT_CHOWN:
		SRMT_ALLOC(u.m_chown);
		snprintf(u.m_chown->path, sizeof(u.m_chown->path),
		    "%s", va_arg(ap, const char *));
		u.m_chown->uid = va_arg(ap, uid_t);
		u.m_chown->gid = va_arg(ap, gid_t);
		break;
	case SRMT_LINK:
		SRMT_ALLOC(u.m_link);
		snprintf(u.m_link->from, sizeof(u.m_link->from),
		    "%s", va_arg(ap, const char *));
		snprintf(u.m_link->to, sizeof(u.m_link->to),
		    "%s", va_arg(ap, const char *));
		break;
	case SRMT_MKDIR:
		SRMT_ALLOC(u.m_mkdir);
		snprintf(u.m_mkdir->path, sizeof(u.m_mkdir->path),
		    "%s", va_arg(ap, const char *));
		u.m_mkdir->mode = va_arg(ap, mode_t);
		break;
	case SRMT_RENAME:
		SRMT_ALLOC(u.m_rename);
		snprintf(u.m_rename->from, sizeof(u.m_rename->from),
		    "%s", va_arg(ap, const char *));
		snprintf(u.m_rename->to, sizeof(u.m_rename->to),
		    "%s", va_arg(ap, const char *));
		break;
	case SRMT_RMDIR:
		SRMT_ALLOC(u.m_rmdir);
		snprintf(u.m_rmdir->path, sizeof(u.m_rmdir->path),
		    "%s", va_arg(ap, const char *));
		break;
	case SRMT_SYMLINK:
		SRMT_ALLOC(u.m_symlink);
		snprintf(u.m_symlink->from, sizeof(u.m_symlink->from),
		    "%s", va_arg(ap, const char *));
		snprintf(u.m_symlink->to, sizeof(u.m_symlink->to),
		    "%s", va_arg(ap, const char *));
		break;
	case SRMT_TRUNCATE:
		SRMT_ALLOC(u.m_truncate);
		snprintf(u.m_truncate->path, sizeof(u.m_truncate->path),
		    "%s", va_arg(ap, const char *));
		u.m_truncate->size = va_arg(ap, size_t);
		break;
	case SRMT_UNLINK:
		SRMT_ALLOC(u.m_unlink);
		snprintf(u.m_unlink->path, sizeof(u.m_unlink->path),
		    "%s", va_arg(ap, const char *));
		break;
	case SRMT_UTIMES:
		SRMT_ALLOC(u.m_utimes);
		snprintf(u.m_utimes->path, sizeof(u.m_utimes->path),
		    "%s", va_arg(ap, const char *));
		memcpy(u.m_utimes->times, va_arg(ap, struct timespec *),
		    sizeof(u.m_utimes->times));
		break;
	default:
		psc_fatalx("unknown op: %d", op);
	}
	va_end(ap);

	/* Create the request and associate it with the import.  */
	rq = pscrpc_prep_req(imp, SMDS_VERSION, op, 1, &msglen, NULL);
	if (rq == NULL)
		return (-ENOMEM);

	/* No reply buffer expected; only return code is needed. */
	msglen = 0;
	rq->rq_replen = psc_msg_size(1, &msglen);

	/* Send the request and block on its completion. */
	rc = pscrpc_queue_wait(rq);
	pscrpc_req_finished(rq);
	return (rc);
}
