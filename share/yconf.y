/* $Id$ */
/*
 * %PSC_START_COPYRIGHT%
 * -----------------------------------------------------------------------------
 * Copyright (c) 2007-2011, Pittsburgh Supercomputing Center (PSC).
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

/*
 * Definitions for the yacc parsing of the SLASH2 configuration file.
 */

%{
#define YYSTYPE char *

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <ctype.h>
#include <err.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <netdb.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "libcfs/kp30.h"
#include "lnet/lib-lnet.h"

#include "pfl/str.h"
#include "psc_rpc/rpc.h"
#include "psc_util/alloc.h"
#include "psc_util/bitflag.h"
#include "psc_util/log.h"

#include "fid.h"
#include "slconfig.h"
#include "slerr.h"

enum slconf_sym_type {
	SL_TYPE_BOOL,
	SL_TYPE_FLOAT,
	SL_TYPE_HEXU64,
	SL_TYPE_INT,
	SL_TYPE_SIZET,
	SL_TYPE_STR,
	SL_TYPE_STRP
};

enum slconf_sym_struct {
	SL_STRUCT_VAR,
	SL_STRUCT_RES,
	SL_STRUCT_SITE
};

typedef uint32_t (*cfg_sym_handler_t)(const char *);

struct slconf_symbol {
	char			*c_name;
	enum slconf_sym_struct	 c_struct;
	enum slconf_sym_type	 c_type;
	uint64_t		 c_max;
	int			 c_offset;
	cfg_sym_handler_t	 c_handler;
};

struct cfg_file {
	char			 cf_fn[PATH_MAX];
	struct psclist_head	 cf_lentry;
};

int		 lnet_match_networks(char **, char *, uint32_t *, char **, int);

void		 slcfg_add_include(const char *);
uint32_t	 slcfg_str2restype(const char *);
void		 slcfg_store_tok_val(const char *, char *);

int		 yylex(void);
int		 yyparse(void);

/*
 * Define a table macro for each structure type filled in by the config
 */
#define TABENT_VAR(name, type, max, field, handler)				\
	{ name, SL_STRUCT_VAR, type, max, offsetof(struct sl_gconf, field), handler }

#define TABENT_SITE(name, type, max, field, handler)				\
	{ name, SL_STRUCT_SITE, type, max, offsetof(struct sl_site, field), handler }

#define TABENT_RES(name, type, max, field, handler)				\
	{ name, SL_STRUCT_RES, type, max, offsetof(struct sl_resource, field), handler }

struct slconf_symbol sym_table[] = {
	TABENT_VAR("allow_exec",	SL_TYPE_STR,	BUFSIZ,		gconf_allowexe,	NULL),
	TABENT_VAR("fs_root",		SL_TYPE_STR,	PATH_MAX,	gconf_fsroot,	NULL),
	TABENT_VAR("journal",		SL_TYPE_STR,	PATH_MAX,	gconf_journal,	NULL),
	TABENT_VAR("net",		SL_TYPE_STR,	NAME_MAX,	gconf_lnets,	NULL),
	TABENT_VAR("nets",		SL_TYPE_STR,	NAME_MAX,	gconf_lnets,	NULL),
	TABENT_VAR("port",		SL_TYPE_INT,	0,		gconf_port,	NULL),
	TABENT_VAR("pref_ios",		SL_TYPE_STR,	RES_NAME_MAX,	gconf_prefios,	NULL),
	TABENT_VAR("pref_mds",		SL_TYPE_STR,	RES_NAME_MAX,	gconf_prefmds,	NULL),
	TABENT_VAR("routes",		SL_TYPE_STR,	NAME_MAX,	gconf_routes,	NULL),
	TABENT_VAR("zpool_cache",	SL_TYPE_STR,	PATH_MAX,	gconf_zpcachefn,NULL),
	TABENT_VAR("zpool_name",	SL_TYPE_STR,	NAME_MAX,	gconf_zpname,	NULL),

	TABENT_SITE("site_desc",	SL_TYPE_STRP,	0,		site_desc,	NULL),
	TABENT_SITE("site_id",		SL_TYPE_INT,	SITE_MAXID,	site_id,	NULL),

	TABENT_RES("desc",		SL_TYPE_STRP,	0,		res_desc,	NULL),
	TABENT_RES("fsroot",		SL_TYPE_STR,	PATH_MAX,	res_fsroot,	NULL),
	TABENT_RES("id",		SL_TYPE_INT,	RES_MAXID,	res_id,		NULL),
	TABENT_RES("jrnldev",		SL_TYPE_STR,	PATH_MAX,	res_jrnldev,	NULL),
	TABENT_RES("type",		SL_TYPE_INT,	0,		res_type,	slcfg_str2restype),
	{ NULL, 0, 0, 0, 0, NULL }
};

struct sl_gconf		   globalConfig;
struct sl_resm		  *nodeResm;

int			   cfg_nid_counter = -1;
int			   cfg_errors;
int			   cfg_lineno;
char			   cfg_filename[PATH_MAX];
struct psclist_head	   cfg_files = PSCLIST_HEAD_INIT(cfg_files);
struct ifaddrs		  *cfg_ifaddrs;
PSCLIST_HEAD(cfg_lnetif_pairs);

static struct sl_site	  *currentSite;
static struct sl_resource *currentRes;
static struct sl_resm	  *currentResm;
%}

%start config

%token BOOL
%token FLOATVAL
%token GLOBPATH
%token HEXNUM
%token NAME
%token NUM
%token PATHNAME
%token SIZEVAL

%token INCLUDE
%token RESOURCE_PROFILE
%token RESOURCE_TYPE
%token SET
%token SITE_PROFILE

%token IPADDR
%token LNETNAME
%token NODES
%token PEERS
%token QUOTEDS

%%

config		: vars includes site_profiles
		;

vars		: /* nothing */
		| var vars
		;

var		: SET statement
		;

includes	: /* nothing */
		| include includes
		;

include		: INCLUDE QUOTEDS {
			glob_t gl;
			size_t i;
			int rc;

			rc = glob($2, GLOB_BRACE, NULL, &gl);
			if (rc)
				yywarn("%s: could not glob", $2);
			else {
				for (i = 0; i < (size_t)gl.gl_pathc; i++)
					slcfg_add_include($2);
				globfree(&gl);
			}
			PSCFREE($2);
		}
		;

site_profiles	: site_profile
		| site_profile site_profiles
		;

site_profile	: site_prof_start site_defs '}' {
			if (libsl_siteid2site(currentSite->site_id))
				yyerror("site %s ID %d already assigned to %s",
				    currentSite->site_name, currentSite->site_id,
				    libsl_siteid2site(currentSite->site_id)->site_name);

			pll_add(&globalConfig.gconf_sites, currentSite);
		}
		;

site_prof_start	: SITE_PROFILE '@' NAME '{' {
			struct sl_site *s;

			PLL_FOREACH(s, &globalConfig.gconf_sites)
				if (strcasecmp(s->site_name, $3) == 0)
					yyerror("duplicate site name: %s", $3);

			currentSite = PSCALLOC(sizeof(*currentSite) +
			    cfg_site_pri_sz);
			psc_dynarray_init(&currentSite->site_resources);
			INIT_PSC_LISTENTRY(&currentSite->site_lentry);
			if (strlcpy(currentSite->site_name, $3,
			    sizeof(currentSite->site_name)) >=
			    sizeof(currentSite->site_name))
				psc_fatalx("site %s: name too long", $3);
			slcfg_init_site(currentSite);
			PSCFREE($3);
		}
		;

site_defs	: statements site_resources
		;

site_resources	: site_resource
		| site_resources site_resource
		;

site_resource	: resource_start resource_def '}' {
			struct sl_resource *r;
			int j, nmds = 0;

			if (strcmp(currentRes->res_name, "") == 0)
				yyerror("resource ID %d @%s has no name",
				    currentSite->site_name);

			if (currentRes->res_type == SLREST_NONE)
				yyerror("resource %s has no type specified",
				    currentRes->res_name);

			if (psc_dynarray_len(&currentRes->res_members) == 0)
				yywarn("resource %s has no members",
				    currentRes->res_name);

			currentRes->res_id = sl_global_id_build(
			    currentSite->site_id, currentRes->res_id);

			/* resource name & ID must be unique within a site */
			DYNARRAY_FOREACH(r, j, &currentSite->site_resources) {
				if (currentRes->res_id == r->res_id)
					yyerror("resource %s ID %d "
					    "already assigned to %s",
					    currentRes->res_name,
					    currentRes->res_id,
					    r->res_name);
				if (strcasecmp(currentRes->res_name,
				    r->res_name) == 0)
					yyerror("duplicate resource name %s@%s",
					    currentRes->res_name,
					    currentSite->site_name);
			}

			psc_dynarray_add(&currentSite->site_resources, currentRes);

			DYNARRAY_FOREACH(r, j, &currentSite->site_resources)
				if (r->res_type == SLREST_MDS &&
				    ++nmds > 1)
					yyerror("site %s has more than "
					    "one metadata server",
					    currentSite->site_name);

			slcfg_init_res(currentRes);
		}
		;

resource_start	: RESOURCE_PROFILE NAME '{' {
			struct sl_resource *r;
			int j, rc;

			currentRes = PSCALLOC(sizeof(*currentRes) +
			    cfg_res_pri_sz);
			currentRes->res_site = currentSite;
			psc_dynarray_init(&currentRes->res_peers);
			psc_dynarray_init(&currentRes->res_members);
			rc = snprintf(currentRes->res_name,
			    sizeof(currentRes->res_name), "%s@%s",
			    $2, currentSite->site_name);
			if (rc == -1)
				psc_fatal("resource %s@%s",
				    $2, currentSite->site_name);
			if (rc >= (int)sizeof(currentRes->res_name))
				psc_fatalx("resource %s@%s: name too long",
				    $2, currentSite->site_name);

			DYNARRAY_FOREACH(r, j, &currentSite->site_resources)
				if (strcasecmp(r->res_name,
				    currentRes->res_name) == 0)
					yyerror("duplicate resource name: %s",
					    r->res_name);

			PSCFREE($2);
		}
		;

resource_def	: statements
		;

peerlist	: PEERS '=' peers ';'
		;

peers		: peer
		| peer ',' peers
		;

peer		: NAME '@' NAME {
			char *p;

			if (asprintf(&p, "%s@%s", $1, $3) == -1)
				psc_fatal("asprintf");
			psc_dynarray_add(&currentRes->res_peers, p);
			PSCFREE($1);
			PSCFREE($3);
		}
		;

nodeslist	: NODES '=' nodes ';'
		;

nodes		: node			{ cfg_nid_counter++; }
		| node nodesep nodes
		;

nodesep		: ','			{ cfg_nid_counter++; }
		;

node		: nodeaddr
		| nodeaddr '|' node
		;

nodeaddr	: IPADDR '@' LNETNAME	{ slcfg_resm_addaddr($1, $3); PSCFREE($3); }
		| IPADDR		{ slcfg_resm_addaddr($1, NULL); }
		| NAME '@' LNETNAME	{ slcfg_resm_addaddr($1, $3); PSCFREE($3); }
		| NAME			{ slcfg_resm_addaddr($1, NULL); }
		;

statements	: /* nothing */
		| statement statements
		;

statement	: restype_stmt
		| bool_stmt
		| float_stmt
		| glob_stmt
		| hexnum_stmt
		| lnetname_stmt
		| nodeslist
		| num_stmt
		| path_stmt
		| peerlist
		| quoteds_stmt
		| size_stmt
		;

restype_stmt	: NAME '=' RESOURCE_TYPE ';' {
			psclog_debug("found restype statement: "
			    "tok '%s' val '%s'", $1, $3);
			slcfg_store_tok_val($1, $3);
			PSCFREE($1);
			PSCFREE($3);
		}
		;

path_stmt	: NAME '=' PATHNAME ';' {
			psclog_debug("found path statement: tok "
			    "'%s' val '%s'", $1, $3);
			slcfg_store_tok_val($1, $3);
			PSCFREE($1);
			PSCFREE($3);
		}
		;

glob_stmt	: NAME '=' GLOBPATH ';' {
			psclog_debug("found glob statement: tok "
			    "'%s' Val '%s'", $1, $3);
			slcfg_store_tok_val($1, $3);
			PSCFREE($1);
			PSCFREE($3);
		}
		;

bool_stmt	: NAME '=' BOOL ';' {
			psclog_debug("found bool statement: "
			    "tok '%s' val '%s'", $1, $3);
			slcfg_store_tok_val($1, $3);
			PSCFREE($1);
			PSCFREE($3);
		}
		;

size_stmt	: NAME '=' SIZEVAL ';' {
			psclog_debug("found sizeval statement: "
			    "tok '%s' val '%s'", $1, $3);
			slcfg_store_tok_val($1, $3);
			PSCFREE($1);
			PSCFREE($3);
		}
		;

num_stmt	: NAME '=' NUM ';' {
			psclog_debug("found num statement: "
			    "tok '%s' val '%s'", $1, $3);
			slcfg_store_tok_val($1, $3);
			PSCFREE($1);
			PSCFREE($3);
		}
		;

float_stmt	: NAME '=' FLOATVAL ';' {
			psclog_debug("found float statement: "
			    "tok '%s' val '%s'", $1, $3);
			slcfg_store_tok_val($1, $3);
			PSCFREE($1);
			PSCFREE($3);
		}
		;

hexnum_stmt	: NAME '=' HEXNUM ';' {
			psclog_debug("found hexnum statement: "
			    "tok '%s' val '%s'", $1, $3);
			slcfg_store_tok_val($1, $3);
			PSCFREE($1);
			PSCFREE($3);
		}
		;

quoteds_stmt	: NAME '=' QUOTEDS ';' {
			psclog_debug("found quoted string statement: "
			    "tok '%s' val '%s'", $1, $3);
			slcfg_store_tok_val($1, $3);
			PSCFREE($1);
			PSCFREE($3);
			/* XXX: don't free, just copy the pointer */
		}
		;

lnetname_stmt	: NAME '=' LNETNAME ';' {
			psclog_debug("found lnetname string statement: "
			    "tok '%s' val '%s'", $1, $3);
			slcfg_store_tok_val($1, $3);
			PSCFREE($1);
			PSCFREE($3);
		}
		;

%%

void
slcfg_add_lnet(uint32_t lnet, char *ifn)
{
	char buf[PSCRPC_NIDSTR_SIZE], ibuf[PSCRPC_NIDSTR_SIZE], *p, *tnam, *ifv[1];
	struct lnetif_pair *i, lp;
	union pfl_sockaddr sa;
	in_addr_t ip;

	if (!pflnet_getifaddr(cfg_ifaddrs, ifn, &sa))
		return;

	ifv[0] = ifn;
	ip = ntohl(sa.sin.sin_addr.s_addr);
	if (lnet_match_networks(&tnam, globalConfig.gconf_lnets,
	    &ip, ifv, 1) == 0)
		return;

	p = strchr(tnam, '(');
	if (p)
		*p = '\0';
	pscrpc_net2str(lnet, buf);
	if (strcmp(tnam, buf))
		return;

	memset(&lp, 0, sizeof(lp));
	INIT_PSC_LISTENTRY(&lp.lentry);
	strlcpy(lp.ifn, ifn, sizeof(lp.ifn));
	lp.net = lnet;

	/*
	 * Ensure mutual exclusion of this interface and Lustre network,
	 * ignoring any interface aliases.
	 */
	psclist_for_each_entry(i, &cfg_lnetif_pairs, lentry) {
		if (strcmp(lp.ifn, i->ifn) == 0 && i->net != lp.net) {
			pscrpc_net2str(i->net, ibuf);
			psc_fatalx("network/interface pair "
			    "%s:%s conflicts with %s:%s",
			    buf, lp.ifn, ibuf, i->ifn);
		}

		/* if the same, don't process more */
		if (i->net == lp.net) {
			if (i->flags & LPF_NOACCEPTOR)
				i->flags |= LPF_SKIP;
			else
				return;
		}
	}

	i = PSCALLOC(sizeof(*i));
	memcpy(i, &lp, sizeof(*i));
	psclist_add(&i->lentry, &cfg_lnetif_pairs);
#ifdef _SLASH_MDS
	if (currentRes->res_type != SLREST_MDS)
		i->flags |= LPF_NOACCEPTOR;
#else
	if (currentRes->res_type == SLREST_MDS)
		i->flags |= LPF_NOACCEPTOR;
#endif
}

void
slcfg_resm_addaddr(char *addr, const char *lnetname)
{
	static int nidcnt, init;
	char ifn[IFNAMSIZ], *ifv[1], *sp, *tnam;
	struct addrinfo hints, *res, *res0;
	union pfl_sockaddr_ptr sa;
	struct sl_resm *m;
	uint32_t lnet;
	in_addr_t ip;
	int rc;

	if (init == 0) {
		init = 1;
		if (strchr(globalConfig.gconf_lnets, ' ') == NULL)
			strlcat(globalConfig.gconf_lnets, " *.*.*.*",
			    sizeof(globalConfig.gconf_lnets));
	}

	if (lnetname) {
		lnet = libcfs_str2net(lnetname);
		if (lnet == LNET_NIDNET(LNET_NID_ANY)) {
			yyerror("%s: invalid LNET network", lnetname);
			return;
		}
	} else
		lnet = LNET_NIDNET(LNET_NID_ANY);

	/* get numerical addresses */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	rc = getaddrinfo(addr, NULL, &hints, &res0);
	if (rc)
		psc_fatalx("%s: %s", addr, gai_strerror(rc));

	for (res = res0; res; res = res->ai_next) {
		sa.p = res->ai_addr;
		ip = ntohl(sa.s->sin.sin_addr.s_addr);
		pflnet_getifnfordst(cfg_ifaddrs, res->ai_addr, ifn);
		ifv[0] = ifn;
		rc = lnet_match_networks(&tnam,
		    globalConfig.gconf_lnets, &ip, ifv, 1);
		if (lnetname == NULL) {
			if (rc == 0) {
				char addrbuf[256];

				psclog_warnx("address %s does not match "
				    "any LNET networks",
				    inet_ntop(sa.s->sa.sa_family,
				    &sa.s->sin.sin_addr.s_addr, addrbuf,
				    sizeof(addrbuf)));
				continue;
			}
			sp = strchr(tnam, '(');
			if (sp)
				*sp = '\0';
			lnet = libcfs_str2net(tnam);
			if (lnet == LNET_NIDNET(LNET_NID_ANY)) {
				psclog_warnx("%s: invalid LNET network",
				    tnam);
				continue;
			}
		}
		/* XXX else: check letname */
		if (rc)
			slcfg_add_lnet(lnet, ifn);

		if (nidcnt == cfg_nid_counter) {
			lnet_nid_t *nidp;

 addnid:
			nidp = PSCALLOC(sizeof(*nidp));
			*nidp = LNET_MKNID(lnet, ip);
			if (libsl_try_nid2resm(*nidp))
				yyerror("NID already registered");
			psc_dynarray_add(&currentResm->resm_nids, nidp);
			continue;
		}

		currentResm = m = PSCALLOC(sizeof(*m) + cfg_resm_pri_sz);

		rc = snprintf(m->resm_addrbuf, sizeof(m->resm_addrbuf),
		    "%s:%s", currentRes->res_name, addr);
		if (rc >= (int)sizeof(m->resm_addrbuf)) {
			errno = ENAMETOOLONG;
			rc = -1;
		}
		if (rc == -1)
			yyerror("resource member %s address %s: %s",
			    currentRes->res_name, addr, slstrerror(errno));

		psc_dynarray_init(&m->resm_nids);
		m->resm_res = currentRes;
		slcfg_init_resm(m);

		psc_dynarray_add(&currentRes->res_members, m);

		nidcnt = cfg_nid_counter;
		goto addnid;
	}
	freeaddrinfo(res0);
}

uint32_t
slcfg_str2restype(const char *res_type)
{
	if (!strcmp(res_type, "parallel_fs"))
		return (SLREST_PARALLEL_FS);
	if (!strcmp(res_type, "standalone_fs"))
		return (SLREST_STANDALONE_FS);
	if (!strcmp(res_type, "archival_fs"))
		return (SLREST_ARCHIVAL_FS);
	if (!strcmp(res_type, "cluster_noshare_fs"))
		return (SLREST_CLUSTER_NOSHARE_LFS);
	if (!strcmp(res_type, "compute"))
		return (SLREST_COMPUTE);
	if (!strcmp(res_type, "mds"))
		return (SLREST_MDS);
	psc_fatalx("%s: invalid resource type", res_type);
}

struct slconf_symbol *
slcfg_get_symbol(const char *name)
{
	struct slconf_symbol *e;

	psclog_debug("symbol lookup '%s'", name);

	for (e = sym_table; e->c_name; e++)
		if (e->c_name && !strcmp(e->c_name, name))
			return (e);
	yyerror("symbol '%s' was not found", name);
	return (NULL);
}

void
slcfg_store_tok_val(const char *tok, char *val)
{
	struct slconf_symbol *e;
	char *endp;
	void *ptr;

	psclog_debug("val %s tok %s", val, tok);

	e = slcfg_get_symbol(tok);
	if (e == NULL)
		return;

	psclog_debug("sym entry %p, name %s, type %d",
	    e, e->c_name, e->c_type);

	/*
	 * Access the correct structure based on the type stored in the
	 * symtab entry.  The offset of the symbol was obtained with
	 * offsetof().
	 */
	switch (e->c_struct) {
	case SL_STRUCT_VAR:
		ptr = e->c_offset + (char *)&globalConfig;
		break;
	case SL_STRUCT_SITE:
		ptr = e->c_offset + (char *)currentSite;
		break;
	case SL_STRUCT_RES:
		ptr = e->c_offset + (char *)currentRes;
		break;
	default:
		psc_fatalx("invalid structure type %d", e->c_struct);
	}

	psclog_debug("type %d ptr %p", e->c_struct, ptr);

	switch (e->c_type) {
	case SL_TYPE_STR:
		if (strlcpy(ptr, val, e->c_max) >= e->c_max)
			yyerror("field %s value too large", e->c_name);
		psclog_debug("SL_TYPE_STR tok '%s' set to '%s'",
		    e->c_name, (char *)ptr);
		break;

	case SL_TYPE_STRP:
		*(char **)ptr = pfl_strdup(val);
		psclog_debug("SL_TYPE_STRP tok '%s' set to '%s' %p",
		    e->c_name, *(char **)ptr, ptr);
		break;

	case SL_TYPE_HEXU64:
		*(uint64_t *)ptr = strtoull(val, &endp, 16);
		if (endp == val || *endp != '\0')
			yyerror("invalid value");
		if (e->c_max && *(uint64_t *)ptr > e->c_max)
			yyerror("field %s value too large", e->c_name);
		psclog_debug("SL_TYPE_HEXU64 tok '%s' set to '%#"PRIx64"'",
		    e->c_name, *(uint64_t *)ptr);
		break;

	case SL_TYPE_INT:
		if (e->c_handler)
			*(int *)ptr = e->c_handler(val);
		else {
			long l;

			l = strtol(val, &endp, 10);
			if (l >= INT_MAX || l <= INT_MIN ||
			    endp == val || *endp != '\0')
				yyerror("%s: invalid integer", val);
			*(int *)ptr = l;
		}
		if (e->c_max && *(int *)ptr > (int)e->c_max)
			yyerror("field %s value too large", e->c_name);
		psclog_debug("SL_TYPE_INT tok '%s' set to '%d'",
		    e->c_name, *(int *)ptr);
		break;

	case SL_TYPE_BOOL:
		if (!strcasecmp("yes", val) ||
		    !strcasecmp("on", val) ||
		    !strcmp("1", val))
			*(int *)ptr = 1;
		else
			*(int *)ptr = 0;
		psclog_debug("SL_TYPE_BOOL option '%s' %s", e->c_name,
		    *(int *)ptr ? "enabled" : "disabled");
		break;

	case SL_TYPE_FLOAT:
		*(float *)ptr = strtof(val, &endp);
		if (endp == val || *endp != '\0')
			yyerror("invalid value");
		psclog_debug("SL_TYPE_FLOAT tok '%s' %f",
		    e->c_name, *(float *)ptr);
		break;

	case SL_TYPE_SIZET: {
		uint64_t i = 1;
		char *c;
		int j;

		j = strlen(val);
		if (j == 0)
			yyerror("invalid value");
		c = &val[j - 1];

		switch (tolower(*c)) {
		case 'b':
			i = 1;
			break;
		case 'k':
			i = 1024;
			break;
		case 'm':
			i = 1024 * 1024;
			break;
		case 'g':
			i = UINT64_C(1024) * 1024 * 1024;
			break;
		case 't':
			i = UINT64_C(1024) * 1024 * 1024 * 1024;
			break;
		default:
			yyerror("sizeval '%s' has invalid postfix", val);
		}
		psclog_debug("szval = %"PRIu64, i);

		*c = '\0';
		*(uint64_t *)ptr = i * strtoull(val, &endp, 10);

		if (endp == val || *endp != '\0')
			yyerror("invalid value");

		if (e->c_max && *(uint64_t *)ptr > e->c_max)
			yyerror("field %s value too large", e->c_name);

		psclog_debug("SL_TYPE_SIZET tok '%s' set to '%"PRIu64"'",
			e->c_name, *(uint64_t *)ptr);
		break;
	    }

	default:
		yyerror("invalid token '%s' type %d", e->c_name, e->c_type);
	}
}

void
slcfg_add_include(const char *fn)
{
	struct cfg_file *cf;

	cf = PSCALLOC(sizeof(*cf));
	INIT_PSC_LISTENTRY(&cf->cf_lentry);
	if (strlcpy(cf->cf_fn, fn,
	    sizeof(cf->cf_fn)) >= sizeof(cf->cf_fn)) {
		errno = ENAMETOOLONG;
		psc_fatal("%s", fn);
	}
	psclist_add_tail(&cf->cf_lentry, &cfg_files);
}

void
slcfg_parse(const char *config_file)
{
	extern FILE *yyin;
	struct sl_resource *r, *peer;
	struct cfg_file *cf, *ncf;
	struct sl_site *s;
	int i, j;
	char *p;

	cfg_errors = 0;

	INIT_GCONF(&globalConfig);

	/* interface addresses are used */
	pflnet_getifaddrs(&cfg_ifaddrs);

	slcfg_add_include(config_file);
	psclist_for_each_entry_safe(cf, ncf, &cfg_files, cf_lentry) {
		if (realpath(cf->cf_fn, cfg_filename) == NULL)
			strlcpy(cfg_filename, cf->cf_fn,
			    sizeof(cfg_filename));
		yyin = fopen(cf->cf_fn, "r");
		if (yyin == NULL) {
			cfg_lineno = -1;
			yywarn("%s", strerror(errno));
		} else {
			cfg_lineno = 1;
			yyparse();
			fclose(yyin);
		}

		PSCFREE(cf);
	}
	if (cfg_errors)
		errx(1, "%d error(s) encountered", cfg_errors);

	pflnet_freeifaddrs(cfg_ifaddrs);

	PLL_LOCK(&globalConfig.gconf_sites);
	pll_sort(&globalConfig.gconf_sites, qsort, slcfg_site_cmp);
	PLL_FOREACH(s, &globalConfig.gconf_sites) {
		psc_dynarray_sort(&s->site_resources, qsort,
		    slcfg_res_cmp);
		DYNARRAY_FOREACH(r, j, &s->site_resources) {
			/* Resolve peer names */
			DYNARRAY_FOREACH(p, i, &r->res_peers) {
				peer = libsl_str2res(p);
				if (!peer)
					errx(1, "peer resource %s not "
					    "specified", p);
				PSCFREE(p);
				psc_dynarray_setpos(&r->res_peers, i, peer);

				/*
				 * If cluster no share resource, stick
				 * the resm's in our res_members array.
				 */
				if (r->res_type ==
				    SLREST_CLUSTER_NOSHARE_LFS) {
					psc_assert(psc_dynarray_len(
					    &peer->res_members) == 1);
					psc_assert(peer->res_type ==
					   SLREST_STANDALONE_FS);
					psc_dynarray_add(&r->res_members,
					    psc_dynarray_getpos(
						   &peer->res_members, 0));
				}
			}
		}
	}
	PLL_ULOCK(&globalConfig.gconf_sites);
}

void
yyerror(const char *fmt, ...)
{
	char buf[LINE_MAX];
	va_list ap;

	cfg_errors++;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	warnx("%s:%d: %s", cfg_filename, cfg_lineno, buf);
}

void
yywarn(const char *fmt, ...)
{
	char buf[LINE_MAX];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (cfg_lineno == -1)
		warnx("%s: %s", cfg_filename, buf);
	else
		warnx("%s:%d: %s", cfg_filename, cfg_lineno, buf);
}
