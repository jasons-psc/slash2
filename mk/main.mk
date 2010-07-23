# $Id$

INCLUDES+=		-I${SLASH_BASE}/include
INCLUDES+=		-I${SLASH_BASE}

DEFINES+=		-DAPP_STRERROR=slstrerror
#DEFINES+=		-DAUTHBUF

SRC_PATH+=		${ZFS_BASE}
ifneq ($(realpath ${SLASH_BASE}),${CURDIR})
SRC_PATH+=		${SLASH_BASE}
endif

SLASH_MODULES+=		cli ion mds

-include ${SLASH_BASE}/mk/local.mk

include ${MAINMK}
