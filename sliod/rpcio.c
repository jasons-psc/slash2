/* $Id$ */

#define _XOPEN_SOURCE 500

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/fsuid.h>
#include <sys/vfs.h>

#include <unistd.h>
#include <errno.h>

#include "psc_rpc/rpc.h"
#include "psc_rpc/rpclog.h"
#include "psc_rpc/rsx.h"
#include "psc_rpc/service.h"
#include "psc_util/strlcpy.h"

#include "fid.h"
#include "rpc.h"
#include "../slashd/cfd.h"
#include "sliod.h"
#include "slashrpc.h"

#define SRIC_NTHREADS	8
#define SRIC_NBUFS	1024
#define SRIC_BUFSZ	256
#define SRIC_REPSZ	256
#define SRIC_SVCNAME	"slricthr"

int
cfd2fid_cache(slash_fid_t *fidp, struct pscrpc_export *exp, u64 cfd)
{
	struct srm_getfid_req *mq;
	struct srm_getfid_rep *mp;
	struct pscrpc_request *rq;
	struct cfdent *c;
	int rc;

	/* Check in cfdtree. */
	if (cfd2fid(fidp, exp, cfd) == 0)
		return (0);

	/* Not there, contact slashd and populate it. */
	if ((rc = rsx_newreq(rim_imp, SRMI_VERSION,
	    SRMT_GETFID, sizeof(*mq), sizeof(*mp), &rq, &mq)) != 0)
		return (rc);
	mq->pid = exp->exp_connection->c_peer.pid;
	mq->nid = exp->exp_connection->c_peer.nid;
	mq->cfd = cfd;
	if ((rc = rsx_waitrep(rq, sizeof(*mp), &mp)) == 0)
		if ((c = cfdinsert(cfd, exp, fidp)) != NULL)
			*fidp = c->fid;
	pscrpc_req_finished(rq);
	return (rc);				/* XXX preserve errno */
}

int
slric_connect(struct pscrpc_request *rq)
{
	struct srm_connect_req *mq;
	struct srm_generic_rep *mp;

	RSX_ALLOCREP(rq, mq, mp);
	if (mq->magic != SRCI_MAGIC || mq->version != SRCI_VERSION)
		mp->rc = -EINVAL;
	return (0);
}

int
slric_read(struct pscrpc_request *rq)
{
	struct pscrpc_bulk_desc *desc;
	struct srm_read_req *mq;
	struct srm_read_rep *mp;
	struct iovec iov;
	char fn[PATH_MAX];
	slash_fid_t fid;
	ssize_t nbytes;
	void *buf;
	int fd;

	RSX_ALLOCREP(rq, mq, mp);
#define MAX_BUFSIZ (1024 * 1024)
	if (mq->size <= 0 || mq->size > MAX_BUFSIZ) {
		mp->rc = -EINVAL;
		return (0);
	}
	if (cfd2fid_cache(&fid, rq->rq_export, mq->cfd)) {
		mp->rc = -errno;
		return (0);
	}
	fid_makepath(&fid, fn);
	if ((fd = open(fn, O_RDONLY)) == -1) {
		mp->rc = -errno;
		return (0);
	}
	buf = PSCALLOC(mq->size);
	nbytes = pread(fd, buf, mq->size, mq->offset);
	if (nbytes == -1) {
		mp->rc = -errno;
		close(fd);
		goto done;
	}
	close(fd);
	mp->size = nbytes;
	if (nbytes == 0)
		goto done;

	iov.iov_base = buf;
	iov.iov_len = mq->size;
	mp->rc = rsx_bulkclient(rq, &desc, BULK_GET_SOURCE,
	    SRCI_BULK_PORTAL, &iov, 1);
	if (desc)
		pscrpc_free_bulk(desc);
 done:
	free(buf);
	return (0);
}

int
slric_write(struct pscrpc_request *rq)
{
	struct pscrpc_bulk_desc *desc;
	struct srm_write_req *mq;
	struct srm_write_rep *mp;
	struct iovec iov;
	char fn[PATH_MAX];
	slash_fid_t fid;
	ssize_t nbytes;
	void *buf;
	int fd;

	RSX_ALLOCREP(rq, mq, mp);
	if (cfd2fid_cache(&fid, rq->rq_export, mq->cfd)) {
		mp->rc = -errno;
		return (0);
	}
	fid_makepath(&fid, fn);
	if (mq->size <= 0 || mq->size > MAX_BUFSIZ) {
		mp->rc = -EINVAL;
		return (0);
	}
	buf = PSCALLOC(mq->size);
	iov.iov_base = buf;
	iov.iov_len = mq->size;
	if ((mp->rc = rsx_bulkserver(rq, &desc, BULK_GET_SINK,
	    SRCI_BULK_PORTAL, &iov, 1)) == 0) {
//	mq->size / pscPageSize,
		if ((fd = open(fn, O_WRONLY)) == -1)
			mp->rc = -errno;
		else {
			nbytes = pwrite(fd, buf, mq->size, mq->offset);
			if (nbytes == -1)
				mp->rc = -errno;
			else
				mq->size = nbytes;
			close(fd);
		}
	}
	if (desc)
		pscrpc_free_bulk(desc);
	free(buf);
	return (0);
}

int
slric_svc_handler(struct pscrpc_request *rq)
{
	int rc = 0;

	switch (rq->rq_reqmsg->opc) {
	case SRMT_CONNECT:
		rc = slric_connect(rq);
		break;
	case SRMT_READ:
		rc = slric_read(rq);
		break;
	case SRMT_WRITE:
		rc = slric_write(rq);
		break;
	default:
		psc_errorx("Unexpected opcode %d", rq->rq_reqmsg->opc);
		rq->rq_status = -ENOSYS;
		rc = pscrpc_error(rq);
		goto done;
	}
	target_send_reply_msg(rq, rc, 0);

 done:
	return (rc);
}

/**
 * slric_init - start up the I/O threads via pscrpc_thread_spawn()
 */
void
slric_init(void)
{
	pscrpc_svc_handle_t *svh = PSCALLOC(sizeof(*svh));

	svh->svh_nbufs      = SRIC_NBUFS;
	svh->svh_bufsz      = SRIC_BUFSZ;
	svh->svh_reqsz      = SRIC_BUFSZ;
	svh->svh_repsz      = SRIC_REPSZ;
	svh->svh_req_portal = SRCI_REQ_PORTAL;
	svh->svh_rep_portal = SRCI_REP_PORTAL;
	svh->svh_type       = SLIOTHRT_RIC;
	svh->svh_nthreads   = SRIC_NTHREADS;
	svh->svh_handler    = slric_svc_handler;

	strlcpy(svh->svh_svc_name, SRIC_SVCNAME, sizeof(svh->svh_svc_name));

	pscrpc_thread_spawn(svh, struct slash_ricthr);
}
