# $Id$

INCLUDES+=		-I${SLASH_BASE}/include
INCLUDES+=		-I${SLASH_BASE}

DEFINES+=		-DSL_STK_VERSION=$$(svn info | awk '{ if ($$0 ~ /^Revision: /) print $$2 }')
DEFINES+=		-DAPP_STRERROR=slstrerror
SRCS+=			${SLASH_BASE}/share/slerr.c

SRC_PATH+=		${SLASH_BASE}/include
SRC_PATH+=		${SLASH_BASE}/mount_slash
SRC_PATH+=		${SLASH_BASE}/share
SRC_PATH+=		${SLASH_BASE}/slashd
SRC_PATH+=		${SLASH_BASE}/sliod

SLASH_MODULES?=		cli ion mds

-include ${SLASH_BASE}/mk/local.mk

include ${MAINMK}
