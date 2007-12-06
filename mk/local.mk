# $Id$

# Disappointingly, recent versions of gcc hide
# standard headers in places other than /usr/include.
MKDEP= `type -t mkdep >/dev/null 2>&1 && echo mkdep || echo makedepend -f.depend` \
    $$(if ${CC} -v 2>&1 | grep -q gcc; then ${CC} -print-search-dirs | \
    grep install | awk '{print "-I" $$2 "include"}' | sed 's/:/ -I/'; fi)
LINT=		splint +posixlib
CTAGS=		ctags

DEFINES+=	-D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64
INCLUDES+=	-I${ROOTDIR}/psc_fsutil_libs/include

THREAD_LIBS?=	-lpthread
LNET_BASE?=	${ROOTDIR}/lnet-lite
LNET_LIBS?=	-L${LNET_BASE}/lib -lzlnet -lzcfs -lsocknal
PFL_LIBS?=	-L${ROOTDIR}/psc_fsutil_libs -lpscutil
