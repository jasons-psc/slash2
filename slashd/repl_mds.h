/* $Id$ */

#ifndef _SL_MDS_REPL_H_
#define _SL_MDS_REPL_H_

#include "psc_ds/tree.h"

#include "fidc_mds.h"

struct mds_resm_info;

struct sl_replrq {
	struct slash_inode_handle	*rrq_inoh;
	psc_spinlock_t			 rrq_lock;
	struct psc_waitq		 rrq_waitq;
	int				 rrq_flags;
	psc_atomic32_t			 rrq_refcnt;
	union {
		struct psclist_head	 rrqu_lentry;
		SPLAY_ENTRY(sl_replrq)	 rrqu_tentry;
	} rrq_u;
#define rrq_tentry rrq_u.rrqu_tentry
#define rrq_lentry rrq_u.rrqu_lentry
};

/* replication request flags */
#define REPLRQF_BUSY	(1 << 0)
#define REPLRQF_DIE	(1 << 1)
#define REPLRQF_REQUEUE	(1 << 2)	/* replrq has been requeued, reprocess */

int replrq_cmp(const void *, const void *);

SPLAY_HEAD(replrqtree, sl_replrq);
SPLAY_PROTOTYPE(replrqtree, sl_replrq, rrq_tentry, replrq_cmp);

uint64_t sl_get_repls_inum(void);

struct sl_replrq *
	mds_repl_findrq(struct slash_fidgen *, int *);
int	mds_repl_accessrq(struct sl_replrq *);
void	mds_repl_reset_scheduled(sl_ios_id_t);
int	mds_repl_addrq(struct slash_fidgen *, sl_blkno_t, sl_replica_t *, int);
void	mds_repl_bmap_rel(struct bmapc_memb *);
int	mds_repl_delrq(struct slash_fidgen *, sl_blkno_t, sl_replica_t *, int);
void	mds_repl_init(void);
int	mds_repl_inv_except_locked(struct bmapc_memb *, sl_ios_id_t);
int	mds_repl_loadino(struct slash_fidgen *, struct fidc_membh **);
int	mds_repl_nodes_getbusy(struct mds_resm_info *, struct mds_resm_info *);
int	mds_repl_nodes_setbusy(struct mds_resm_info *, struct mds_resm_info *, int);
void	mds_repl_tryrmqfile(struct sl_replrq *);
void	mds_repl_unrefrq(struct sl_replrq *);
int	_mds_repl_ios_lookup(struct slash_inode_handle *, sl_ios_id_t, int);

#define mds_repl_ios_lookup_add(ih, ios)	_mds_repl_ios_lookup((ih), (ios), 1)
#define mds_repl_ios_lookup(ih, ios)		_mds_repl_ios_lookup((ih), (ios), 0)

#define REPLRQ_INO(rrq)		(&(rrq)->rrq_inoh->inoh_ino)
#define REPLRQ_INOX(rrq)	(rrq)->rrq_inoh->inoh_extras
#define REPLRQ_NREPLS(rrq)	REPLRQ_INO(rrq)->ino_nrepls
#define REPLRQ_FG(rrq)		(&REPLRQ_INO(rrq)->ino_fg)
#define REPLRQ_FID(rrq)		REPLRQ_FG(rrq)->fg_fid
#define REPLRQ_FCMH(rrq)	(rrq)->rrq_inoh->inoh_fcmh
#define REPLRQ_NBMAPS(rrq)	fcmh_2_nbmaps(REPLRQ_FCMH(rrq))

#define REPLRQ_GETREPL(rrq, n)	((n) < INO_DEF_NREPLS ?			\
				    REPLRQ_INO(rrq)->ino_repls[n] :	\
				    REPLRQ_INOX(rrq)->inox_repls[(n) - 1])

extern struct psc_poolmgr	*replrq_pool;

extern struct replrqtree	 replrq_tree;
extern psc_spinlock_t		 replrq_tree_lock;

extern struct vbitmap		*repl_busytable;
extern psc_spinlock_t		 repl_busytable_lock;

#endif /* _SL_MDS_REPL_H_ */
