/* $Id$ */

#include <sys/types.h>

#include "cfd.h"

struct cfdtree;
struct pscrpc_export;

/* SERVER_CLIENT */
enum slash_exp_types {
	MDS_ION_EXP = (1<<0),
	MDS_CLI_EXP = (1<<1),
	MDS_MDS_EXP = (1<<2),
	ION_CLI_EXP = (1<<3),
	ION_MDS_EXP = (1<<4),
	ION_ION_EXP = (1<<5),
	CLI_MDS_EXP = (1<<6),
	EXP_CLOSING = (1<<7)
};

struct slashrpc_export {
	uint64_t		 sexp_conn_gen;
	uint64_t		 sexp_nextcfd;
	struct cfdtree		 sexp_cfdtree;
	int			 sexp_type;
	void			*sexp_data;
	struct pscrpc_export	*sexp_export;
};

struct slashrpc_export *
slashrpc_export_get(struct pscrpc_export *);

#if SEXPTREE
int sexpcmp(const void *a, const void *b);

extern struct sexptree sexptree;
extern psc_spinlock_t sexptreelock;
#endif
