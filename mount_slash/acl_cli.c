/* $Id$ */
/* %PSC_COPYRIGHT% */

#include "pfl/acl.h"

/*
 * Pull POSIX ACLs from an fcmh via RPCs to MDS.
 */
acl_t
slc_acl_get_fcmh(struct pscfs_req *pfr, struct fidc_membh *f)
{
	char name[24], trybuf[16];
	void *buf = NULL;
	size_t retsz = 0;
	acl_t a;

	rc = slc_getxattr(pfr, ACL_EA_ACCESS, trybuf, sizeof(trybuf), f,
	    &retsz);
	if (rc == 0) {
		buf = trybuf;
	} else if (rc == -ERANGE) {
		buf = PSCALLOC(retsz);
		rc = slc_getxattr(pfr, ACL_EA_ACCESS, buf, retsz, f,
		    &retsz);
	}

	a = sl_acl_from_xattr(buf, rc);

 out:
	if (buf != trybuf)
		PSCFREE(buf);
	return (a);
}

#define ACL_SET_PRECEDENCE(level, prec, e, authz)			\
	if ((level) < (prec)) {						\
		(authz) = (e);						\
		(prec) = (level);					\
	}

#define ACL_AUTH(e, mode)

#define FOREACH_GROUP(g, i, pcrp)					\
	for ((i) = 0; (i) <= (pcrp)->pcr_ngid && (((g) = (i) ?		\
	    (pcrp)->pcr_gid : (pcrp)->pcr_gidv[(i) - 1]) || 1); (i)++)

int
sl_checkacls(acl_t a, uid_t owner, gid_t group,
    const struct pscfs_creds *pcrp, int accmode)
{
	int rv = EACCES, rc, prec = 6;
	acl_entry_t e, authz = NULL, mask = NULL;
	acl_tag_t tag;
	uid_t *up;
	gid_t *gp;
	acl_t a;

	wh = ACL_FIRST_ENTRY;
	while ((rc = acl_get_entry(a, wh, &e)) == 1) {
		wh = ACL_NEXT_ENTRY;

		rc = acl_get_tag_type(e, &tag);
		switch (tag) {
		case ACL_USER_OBJ:
			if (owner == pcrp->pcr_uid)
				ACL_SET_PRECEDENCE(1, prec, e, authz);
			break;
		case ACL_USER:
			up = acl_get_qualifier(e);
			if (*up == pcrp->pcr_uid)
				ACL_SET_PRECEDENCE(2, prec, e, authz);
			break;

		case ACL_GROUP_OBJ:
			FOREACH_GROUP(g, i, pcrp)
				if (g == group && ACL_AUTH(e, accmode)) {
					ACL_SET_PRECEDENCE(3, prec, e,
					    authz);
					break;
				}
			break;
		case ACL_GROUP:
			gp = acl_get_qualifier(e);
			FOREACH_GROUP(g, i, pcrp)
				if (g == *gp && ACL_AUTH(e, accmode)) {
					ACL_SET_PRECEDENCE(4, prec, e,
					    authz);
					break;
				}
			break;

		case ACL_OTHER:
			ACL_SET_PRECEDENCE(5, prec, e, authz);
			break;

		case ACL_MASK:
			mask = e;
			break;

		default:
			psclog_error("acl_get_tag_type");
			break;
		}
	}
	if (rc == -1)
		psclog_error("acl_get_entry");
	else if (authz) {
		rv = ACL_AUTH(authz, accmode);
		if (prec != 1 && prec != 5 &&
		    rv == 0 && mask)
			rv = ACL_AUTH(mask, accmode);
	}
	return (rv);
}

int
sl_checkacls(struct fidc_membh *f, const struct pscfs_creds *pcrp,
    int accmode)
{
	int rv;

	a = slc_acl_get_fcmh(f);
	if (a == NULL)
		return (EACCES);
	rv = sl_checkacls(a, f->fcmh_sstb.sst_uid, f->fcmh_sstb.sst_gid,
	    pcrp, accmode);
	acl_free(a);
	return (rv);
}
