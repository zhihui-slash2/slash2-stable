# $Id$

ROOTDIR=.
include ${ROOTDIR}/Makefile.path

-include local.mk

SUBDIRS+=	${PFL_BASE}
SUBDIRS+=	tools
SUBDIRS+=	apps
SUBDIRS+=	fio
SUBDIRS+=	${_SLASH_BASE}
SUBDIRS+=	${_ZEST_BASE}

_SLASH_BASE=	${SLASH_BASE}

ifneq ($(wildcard ${ZEST_BASE}),)
_ZEST_BASE=	${ZEST_BASE}
endif

include ${MAINMK}
