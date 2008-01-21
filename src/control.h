/* $Id$ */

/*
 * Control interface for querying and modifying
 * parameters of a currently running slashd.
 */

#include <sys/param.h>

#include "psc_ds/hash.h"

#include "slash.h"
#include "inode.h"

/* Path to control socket. */
#define _PATH_SLCTLSOCK		"../slashd.sock"

#define SLTHRNAME_EVERYONE	"everyone"

#define SLCTLMSG_ERRMSG_MAX	50

struct slctlmsg_errmsg {
	char			sem_errmsg[SLCTLMSG_ERRMSG_MAX];
};

#define SSS_NAME_MAX 16	/* multiple of wordsize */

struct slctlmsg_subsys {
	char			sss_names[0];
};

struct slctlmsg_loglevel {
	char			sll_threadname[PSC_THRNAME_MAX];
	int			sll_levels[0];
};

struct slctlmsg_lc {
	char			slc_name[LC_NAME_MAX];
	size_t			slc_max;	/* max #items list can attain */
	size_t			slc_size;	/* #items on list */
	size_t			slc_nseen;	/* max #items list can attain */
};

#define SLC_NAME_ALL		"all"

struct slctlmsg_hashtable {
	char			sht_name[HTNAME_MAX];
	int			sht_totalbucks;
	int			sht_usedbucks;
	int			sht_nents;
	int			sht_maxbucklen;
};

#define SHT_NAME_ALL		"all"

struct slctlmsg_mlist {
	char			zm_name[ZML_NAME_MAX];
	u32			zm_size;
	u32			zm_nseen;
	u32			zm_waitors;
};

#define SML_NAME_ALL		"all"

#define SP_FIELD_MAX		30
#define SP_VALUE_MAX		50

struct slctlmsg_param {
	char			sp_thrname[PSC_THRNAME_MAX];
	char			sp_field[ZP_FIELD_MAX];
	char			sp_value[ZP_VALUE_MAX];
};

struct zctlmsg_iostats {
//	struct iostats		zist_ist;
};

#define SIST_NAME_ALL		"all"

/* Slash control message types. */
#define SCMT_ERRMSG		0
#define SCMT_GETINODE		1
#define SCMT_GETLOGLEVEL	2
#define SCMT_GETLIST		3
#define SCMT_GETSTATS		4
#define SCMT_GETDISK		5
#define SCMT_GETHASHTABLE	6
#define SCMT_GETMLIST		7
#define SCMT_GETPARAM		8
#define SCMT_SETPARAM		9
#define SCMT_GETIOSTAT		10
#define SCMT_GETMETER		11

/*
 * Slash control message header.
 * This structure precedes each actual message.
 */
struct slctlmsghdr {
	int			scmh_type;
	int			scmh_id;
	size_t			scmh_size;
	unsigned char		scmh_data[0];
};
