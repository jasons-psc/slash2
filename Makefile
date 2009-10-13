# $Id$

SLASH_BASE=.
PROJECT_BASE=${SLASH_BASE}
include Makefile.path
include ${SLASHMK}

SUBDIRS+=	mount_slash
SUBDIRS+=	msctl
SUBDIRS+=	slashd
SUBDIRS+=	slctl
SUBDIRS+=	slimmns
SUBDIRS+=	slioctl
SUBDIRS+=	sliod
SUBDIRS+=	slmkjrnl
SUBDIRS+=	${PFL_BASE}/utils/odtable

zbuild:
	@(cd ${ZFS_BASE} && ${SCONS} -c && scons)
	@(cd ${ZFS_BASE} && ${SCONS} slashlib=1 debug=2 -c && \
	    ${SCONS} slashlib=1 debug=2)

prereq rezbuild:
	@(cd ${ZFS_BASE} && ${SCONS} slashlib=1 debug=2)
