/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2006-2013, Pittsburgh Supercomputing Center (PSC).
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

#ifndef _SLASHD_H_
#define _SLASHD_H_

#include "pfl/dynarray.h"
#include "pfl/vbitmap.h"
#include "psc_rpc/rpc.h"
#include "psc_rpc/service.h"
#include "psc_util/meter.h"
#include "psc_util/multiwait.h"

#include "inode.h"
#include "namespace.h"
#include "slashrpc.h"
#include "slconfig.h"
#include "sltypes.h"

struct odtable;

struct fidc_membh;
struct slm_sth;
struct srt_stat;

/* MDS thread types. */
enum {
	SLMTHRT_BMAPTIMEO,	/* bmap timeout thread */
	SLMTHRT_COH,		/* coherency thread */
	SLMTHRT_CONN,		/* peer resource connection monitor */
	SLMTHRT_CTL,		/* control processor */
	SLMTHRT_CTLAC,		/* control acceptor */
	SLMTHRT_CURSOR,		/* cursor update thread */
	SLMTHRT_JNAMESPACE,	/* namespace propagating thread */
	SLMTHRT_JRECLAIM,	/* garbage reclamation thread */
	SLMTHRT_JRNL,		/* journal distill thread */
	SLMTHRT_LNETAC,		/* lustre net accept thr */
	SLMTHRT_NBRQ,		/* non-blocking RPC reply handler */
	SLMTHRT_RCM,		/* CLI <- MDS msg issuer */
	SLMTHRT_RMC,		/* MDS <- CLI msg svc handler */
	SLMTHRT_RMI,		/* MDS <- I/O msg svc handler */
	SLMTHRT_RMM,		/* MDS <- MDS msg svc handler */
	SLMTHRT_TIOS,		/* I/O stats updater */
	SLMTHRT_UPSCHED,	/* update scheduler for site resources */
	SLMTHRT_USKLNDPL,	/* userland socket lustre net dev poll thr */
	SLMTHRT_WORKER,		/* miscellaneous work */
	SLMTHRT_ZFS_KSTAT	/* ZFS stats */
};

enum {
	SLM_OPST_BMAP_CHWRMODE,
	SLM_OPST_BMAP_CHWRMODE_DONE,
	SLM_OPST_BMAP_DIO_CLR,
	SLM_OPST_BMAP_DIO_SET,
	SLM_OPST_BMAP_RELEASE,
	SLM_OPST_COHERENT_CB,
	SLM_OPST_COHERENT_REQ,
	SLM_OPST_CREATE,
	SLM_OPST_ODTABLE_EXTEND,
	SLM_OPST_ODTABLE_FREE,
	SLM_OPST_ODTABLE_FULL,
	SLM_OPST_ODTABLE_REPLACE,
	SLM_OPST_EXTEND_BMAP_LEASE,
	SLM_OPST_GETATTR,
	SLM_OPST_GETXATTR,
	SLM_OPST_GET_BMAP_LEASE_READ,
	SLM_OPST_GET_BMAP_LEASE_WRITE,
	SLM_OPST_LINK,
	SLM_OPST_LOOKUP,
	SLM_OPST_LOGFILE_CREATE,
	SLM_OPST_LOGFILE_REMOVE,
	SLM_OPST_MKDIR,
	SLM_OPST_MKNOD,
	SLM_OPST_READDIR,
	SLM_OPST_READLINK,
	SLM_OPST_REASSIGN_BMAP_LEASE,
	SLM_OPST_RENAME,
	SLM_OPST_REPL_SCHEDWK,
	SLM_OPST_SETATTR,
	SLM_OPST_SETXATTR,
	SLM_OPST_STATFS,
	SLM_OPST_SYMLINK,
	SLM_OPST_UNLINK
};

struct slmrmc_thread {
	struct pscrpc_thread	  smrct_prt;
};

struct slmrcm_thread {
	char			 *srcm_page;
	int			  srcm_page_bitpos;
};

struct slmrmi_thread {
	struct pscrpc_thread	  smrit_prt;
};

struct slmrmm_thread {
	struct pscrpc_thread	  smrmt_prt;
};

PSCTHR_MKCAST(slmrcmthr, slmrcm_thread, SLMTHRT_RCM)
PSCTHR_MKCAST(slmrmcthr, slmrmc_thread, SLMTHRT_RMC)
PSCTHR_MKCAST(slmrmithr, slmrmi_thread, SLMTHRT_RMI)
PSCTHR_MKCAST(slmrmmthr, slmrmm_thread, SLMTHRT_RMM)

struct site_mds_info {
};

static __inline struct site_mds_info *
site2smi(struct sl_site *site)
{
	return (site_get_pri(site));
}

/* per-MDS eventually consistent namespace stats */
struct sl_mds_nsstats {
	psc_atomic32_t		  ns_stats[NS_NDIRS][NS_NOPS + 1][NS_NSUMS];
};

#define _SLM_NSSTATS_ADJ(adj, peerinfo, dir, op, sum)			\
	do {								\
		psc_atomic32_##adj(&(peerinfo)->sp_stats.		\
		    ns_stats[dir][op][sum]);				\
		psc_atomic32_##adj(&(peerinfo)->sp_stats.		\
		    ns_stats[dir][NS_NOPS][sum]);			\
									\
		psc_atomic32_##adj(&slm_nsstats_aggr.			\
		    ns_stats[dir][op][sum]);				\
		psc_atomic32_##adj(&slm_nsstats_aggr.			\
		    ns_stats[dir][NS_NOPS][sum]);			\
	} while (0)

#define SLM_NSSTATS_INCR(peerinfo, dir, op, sum)			\
	_SLM_NSSTATS_ADJ(inc, (peerinfo), (dir), (op), (sum))
#define SLM_NSSTATS_DECR(peerinfo, dir, op, sum)			\
	_SLM_NSSTATS_ADJ(dec, (peerinfo), (dir), (op), (sum))

/*
 * This structure tracks the progress of namespace log application on a
 * MDS.  We allow one pending request per MDS until it responds or
 * timeouts.
 */
struct sl_mds_peerinfo {
	int			  sp_flags;
	struct psc_meter	  sp_batchmeter;
#define sp_batchno sp_batchmeter.pm_cur
	uint64_t		  sp_xid;

	int			  sp_fails;		/* the number of successive RPC failures */
	int			  sp_skips;		/* the number of times to skip */

	int			  sp_send_count;	/* # of updates in the batch */
	uint64_t		  sp_send_seqno;	/* next log sequence number to send */

	uint64_t		  sp_recv_seqno;	/* last received log sequence number */

	struct sl_mds_nsstats	  sp_stats;
};

#define	SPF_NEED_JRNL_INIT	(1 << 0)		/* journal fields need initialized */

#define res2mdsinfo(res)	((struct sl_mds_peerinfo *)res2rpmi(res)->rpmi_info)

/*
 * This structure tracks the progress of garbage collection on each I/O
 * server.
 */
struct sl_mds_iosinfo {
	int			  si_flags;
	pthread_t		  si_owner;
	struct timespec		  si_lastcomm;		/* last communication (PING) to track soft conn reset */
	uint64_t		  si_xid;		/* garbage reclaim transaction group identifier */
	struct psc_meter	  si_batchmeter;
#define si_batchno si_batchmeter.pm_cur
	struct srt_statfs	  si_ssfb;
};

#define	SIF_NEED_JRNL_INIT	(1 << 0)		/* journal fields need initialized */
#define SIF_DISABLE_BIA		(1 << 1)		/* disable bmap lease assignments */
#define SIF_DISABLE_GC		(1 << 2)		/* disable garbage collection temporarily */
#define SIF_BUSY		(1 << 3)
#define SIF_UPSCH_PAGING	(1 << 4)

#define res2iosinfo(res)	((struct sl_mds_iosinfo *)res2rpmi(res)->rpmi_info)

/* MDS-specific data for struct sl_resource */
struct resprof_mds_info {
	int			  rpmi_cnt;		/* IOS round-robin assigner */
	struct pfl_mutex	  rpmi_mutex;
	struct psc_dynarray	  rpmi_upschq;		/* updates queue */
	struct psc_waitq	  rpmi_waitq;

	/* sl_mds_peerinfo for peer MDS or sl_mds_iosinfo for IOS */
	void			 *rpmi_info;
};

#define RPMI_LOCK(rpmi)		psc_mutex_lock(&(rpmi)->rpmi_mutex)
#define RPMI_RLOCK(rpmi)	psc_mutex_reqlock(&(rpmi)->rpmi_mutex)
#define RPMI_ULOCK(rpmi)	psc_mutex_unlock(&(rpmi)->rpmi_mutex)
#define RPMI_URLOCK(rpmi, lkd)	psc_mutex_ureqlock(&(rpmi)->rpmi_mutex, (lkd))

static __inline struct resprof_mds_info *
res2rpmi(struct sl_resource *res)
{
	return (resprof_get_pri(res));
}

/* MDS-specific data for struct sl_resm */
struct resm_mds_info {
	int			 rmmi_busyid;
	struct sl_resm		*rmmi_resm;
	atomic_t		 rmmi_refcnt;		/* #CLIs using this ion */
};

static __inline struct resm_mds_info *
resm2rmmi(struct sl_resm *resm)
{
	return (resm_get_pri(resm));
}

#define resm2rpmi(resm)		res2rpmi((resm)->resm_res)

struct slm_wkdata_wr_brepl {
	struct bmapc_memb	*b;

	/* only used during REPLAY */
	struct slash_fidgen	 fg;
	sl_bmapno_t		 bno;
};

struct slm_wkdata_ptrunc {
	struct fidc_membh	*f;
};

struct slm_wkdata_upsch_purge {
	slfid_t			 fid;
	struct bmap		*b;
};

struct slm_wkdata_upsch_cb {
	struct slashrpc_cservice	*csvc;
	struct sl_resm			*src_resm;
	struct sl_resm			*dst_resm;
	struct bmap			*b;
	int				 rc;
	int				 off;
	int64_t				 amt;
	int				 undowr;
};

#define SLM_NWORKER_THREADS	4

int		 mds_handle_rls_bmap(struct pscrpc_request *, int);
int		 mds_lease_renew(struct fidc_membh *, struct srt_bmapdesc *,
			struct srt_bmapdesc *, struct pscrpc_export *);
int		 mds_lease_reassign(struct fidc_membh *,
			struct srt_bmapdesc *, sl_ios_id_t, sl_ios_id_t *,
			int, struct srt_bmapdesc *, struct pscrpc_export *);

int		 mds_sliod_alive(void *);

void		 slm_iosv_setbusy(sl_replica_t *, int);

__dead void	 slmctlthr_main(const char *);
void		 slmbmaptimeothr_spawn(void);
void		 slmrcmthr_main(struct psc_thread *);
void		 slmtimerthr_spawn(void);

slfid_t		 slm_get_curr_slashfid(void);
void		 slm_set_curr_slashfid(slfid_t);
int		 slm_get_next_slashfid(slfid_t *);

int		 slm_ptrunc_prepare(void *);
void		 slm_ptrunc_apply(struct slm_wkdata_ptrunc *);
int		 slm_ptrunc_wake_clients(void *);
void		 slm_setattr_core(struct fidc_membh *, struct srt_stat *, int);

void		 slm_upsch_init(void);
void		 slmupschedthr_spawn(void);

void		 psc_scan_filesystems(void);

#define dbdo(cb, arg, fmt, ...)	_dbdo(PFL_CALLERINFO(), (cb), (arg), (fmt), ## __VA_ARGS__)
void		 _dbdo(const struct pfl_callerinfo *,
			int (*)(struct slm_sth *, void *), void *,
			const char *, ...);

extern struct slash_creds	 rootcreds;
extern struct odtable		*mdsBmapAssignTable;
extern struct odtable		*slm_repl_odt;
extern struct odtable		*slm_ptrunc_odt;
extern struct sl_mds_nsstats	 slm_nsstats_aggr;	/* aggregate namespace stats */
extern struct sl_mds_peerinfo	*localinfo;

extern struct psc_thread	*slmconnthr;

static __inline int
slm_get_rpmi_idx(struct sl_resource *res)
{
	struct resprof_mds_info *rpmi;
	int locked, n;

	rpmi = res2rpmi(res);
	locked = RPMI_RLOCK(rpmi);
	if (rpmi->rpmi_cnt >= psc_dynarray_len(&res->res_members))
		rpmi->rpmi_cnt = 0;
	n = rpmi->rpmi_cnt++;
	RPMI_URLOCK(rpmi, locked);
	return (n);
}

#endif /* _SLASHD_H_ */
