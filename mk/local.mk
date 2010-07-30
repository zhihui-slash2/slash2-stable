# $Id$

MKDEP=		env CC="${CC}" ${ROOTDIR}/tools/unwrapcc ${ROOTDIR}/tools/mkdep

# Disappointingly, recent versions of gcc hide
# standard headers in places other than /usr/include.
LIBC_INCLUDES+=	$$(if ${CC} -v 2>&1 | grep -q gcc; then ${CC} -print-search-dirs | \
		    grep install | awk '{print "-I" $$2 "include"}' | sed 's/:/ -I/'; fi)
LINT=		splint +posixlib
NOTEMPTY=	${ROOTDIR}/tools/notempty
SCONS=		scons
PKG_CONFIG=	pkg-config
LIBGCRYPT_CONFIG=libgcrypt-config
MPICC=		mpicc
ECHORUN=	${ROOTDIR}/tools/echorun.sh
GENTYPES=	PERL5LIB=${PERL5LIB}:${CROOTDIR}/tools/lib ${CROOTDIR}/tools/gentypes.pl
HDRCLEAN=	${ROOTDIR}/tools/hdrclean.pl
LIBDEP=		${ROOTDIR}/tools/libdep.pl
MDPROC=		${ROOTDIR}/tools/mdproc.pl
MINVER=		${ROOTDIR}/tools/minver.pl
MYECHO=		${ROOTDIR}/tools/myecho.pl
PCPP=		PERL5LIB=${PERL5LIB}:${CROOTDIR}/tools/lib ${CROOTDIR}/tools/pcpp.pl
PICKLEGEN=	${ROOTDIR}/tools/pickle-gen.sh

MAKEFLAGS+=	--no-print-directory

LFLAGS+=	-t $$(if ${MINVER} $$(lex -V | sed 's![a-z /]*!!g') 2.5.5; then echo --nounput; fi)
YFLAGS+=	-d

CFLAGS+=	-Wall -W

DEBUG?=		1
ifeq (${DEBUG},1)
CFLAGS+=	-g
#CFLAGS+=	-ggdb3
#DEFINES+=	-DDEBUG
else
CFLAGS+=	-Wunused -Wuninitialized -O2
#CFLAGS+=	-Wshadow
endif

DEFINES+=	-D_REENTRANT -DYY_NO_UNPUT -DYY_NO_INPUT -DYYERROR_VERBOSE

KERNEL_BASE=	/usr/src/kernels/linux

FUSE_CFLAGS=	$$(PKG_CONFIG_PATH=${FUSE_BASE} ${PKG_CONFIG} --cflags fuse | ${EXTRACT_CFLAGS})
FUSE_DEFINES=	$$(PKG_CONFIG_PATH=${FUSE_BASE} ${PKG_CONFIG} --cflags fuse | ${EXTRACT_DEFINES})
FUSE_INCLUDES=	$$(PKG_CONFIG_PATH=${FUSE_BASE} ${PKG_CONFIG} --cflags fuse | ${EXTRACT_INCLUDES})
FUSE_LIBS=	$$(PKG_CONFIG_PATH=${FUSE_BASE} ${PKG_CONFIG} --libs fuse)
FUSE_VERSION=	$$(PKG_CONFIG_PATH=${FUSE_BASE} ${PKG_CONFIG} --modversion fuse | sed 's/\([0-9]\)*\.\([0-9]*\).*/\1\2/')

GCRYPT_CFLAGS=	$$(${LIBGCRYPT_CONFIG} --cflags | ${EXTRACT_CFLAGS})
GCRYPT_DEFINES=	$$(${LIBGCRYPT_CONFIG} --cflags | ${EXTRACT_DEFINES})
GCRYPT_INCLUDES=$$(${LIBGCRYPT_CONFIG} --cflags | ${EXTRACT_INCLUDES})
GCRYPT_LIBS=	$$(${LIBGCRYPT_CONFIG} --libs)

ZFS_LIBS=	-L${ZFS_BASE}/zfs-fuse					\
		-L${ZFS_BASE}/lib/libavl				\
		-L${ZFS_BASE}/lib/libnvpair				\
		-L${ZFS_BASE}/lib/libsolkerncompat			\
		-L${ZFS_BASE}/lib/libumem				\
		-L${ZFS_BASE}/lib/libzfscommon				\
		-L${ZFS_BASE}/lib/libzpool				\
		-lzfs-fuse -lzpool-kernel -lzfscommon-kernel		\
		-lnvpair-kernel -lavl -lumem -lsolkerncompat -ldl

LIBL=		-ll
LIBZ=		-lz
THREAD_LIBS=	-pthread
LIBCURSES=	-lncurses
LIBAIO=		-laio

OSTYPE:=	$(shell uname)

# global file-specific settings
psc_fsutil_libs_psc_util_crc_c_CFLAGS=		-O2 -g0
psc_fsutil_libs_psc_util_parity_c_CFLAGS=	-O2 -g0

lnet_lite_ulnds_socklnd_conn_c_CFLAGS=		-DPSC_SUBSYS=PSS_LNET -Wno-shadow
lnet_lite_ulnds_socklnd_handlers_c_CFLAGS=	-DPSC_SUBSYS=PSS_LNET -Wno-shadow
lnet_lite_ulnds_socklnd_poll_c_CFLAGS=		-DPSC_SUBSYS=PSS_LNET -Wno-shadow
lnet_lite_ulnds_socklnd_usocklnd_c_CFLAGS=	-DPSC_SUBSYS=PSS_LNET -Wno-shadow
lnet_lite_ulnds_socklnd_usocklnd_cb_c_CFLAGS=	-DPSC_SUBSYS=PSS_LNET -Wno-shadow

lnet_lite_libcfs_debug_c_CFLAGS=		-DPSC_SUBSYS=PSS_LNET -Wno-shadow
lnet_lite_libcfs_nidstrings_c_CFLAGS=		-DPSC_SUBSYS=PSS_LNET -Wno-shadow
lnet_lite_libcfs_user_lock_c_CFLAGS=		-DPSC_SUBSYS=PSS_LNET -Wno-shadow
lnet_lite_libcfs_user_prim_c_CFLAGS=		-DPSC_SUBSYS=PSS_LNET -Wno-shadow
lnet_lite_libcfs_user_tcpip_c_CFLAGS=		-DPSC_SUBSYS=PSS_LNET -Wno-shadow

lnet_lite_lnet_acceptor_c_CFLAGS=		-DPSC_SUBSYS=PSS_LNET -Wno-shadow
lnet_lite_lnet_api_errno_c_CFLAGS=		-DPSC_SUBSYS=PSS_LNET -Wno-shadow
lnet_lite_lnet_api_ni_c_CFLAGS=			-DPSC_SUBSYS=PSS_LNET -Wno-shadow
lnet_lite_lnet_config_c_CFLAGS=			-DPSC_SUBSYS=PSS_LNET -Wno-shadow
lnet_lite_lnet_lib_eq_c_CFLAGS=			-DPSC_SUBSYS=PSS_LNET -Wno-shadow
lnet_lite_lnet_lib_md_c_CFLAGS=			-DPSC_SUBSYS=PSS_LNET -Wno-shadow
lnet_lite_lnet_lib_me_c_CFLAGS=			-DPSC_SUBSYS=PSS_LNET -Wno-shadow
lnet_lite_lnet_lib_move_c_CFLAGS=		-DPSC_SUBSYS=PSS_LNET -Wno-shadow
lnet_lite_lnet_lib_msg_c_CFLAGS=		-DPSC_SUBSYS=PSS_LNET -Wno-shadow
lnet_lite_lnet_lo_c_CFLAGS=			-DPSC_SUBSYS=PSS_LNET -Wno-shadow
lnet_lite_lnet_peer_c_CFLAGS=			-DPSC_SUBSYS=PSS_LNET -Wno-shadow
lnet_lite_lnet_router_c_CFLAGS=			-DPSC_SUBSYS=PSS_LNET -Wno-shadow
lnet_lite_lnet_router_proc_c_CFLAGS=		-DPSC_SUBSYS=PSS_LNET -Wno-shadow

# system-specific settings/overrides
ifneq ($(wildcard /opt/sgi),)
  # on altix
  NUMA_DEFINES=					-DHAVE_NUMA
  NUMA_LIBS=					-lcpuset -lbitmask -lnuma
  LIBL=						-lfl

  slash_nara_mount_slash_obj_lconf_c_PCPP_FLAGS=	-x
  slash_nara_slashd_obj_lconf_c_PCPP_FLAGS=		-x
  slash_nara_sliod_obj_lconf_c_PCPP_FLAGS=		-x
  slash_nara_tests_config_obj_lconf_c_PCPP_FLAGS=	-x

  slash_nara_mount_slash_obj_yconf_c_PCPP_FLAGS=	-x
  slash_nara_slashd_obj_yconf_c_PCPP_FLAGS=		-x
  slash_nara_sliod_obj_yconf_c_PCPP_FLAGS=		-x
  slash_nara_tests_config_obj_yconf_c_PCPP_FLAGS=	-x

  zest_zestFormat_obj_zestLexConfig_c_PCPP_FLAGS=	-x
  zest_zestiond_obj_zestLexConfig_c_PCPP_FLAGS=		-x
  zest_tests_config_obj_zestLexConfig_c_PCPP_FLAGS=	-x
endif

ifneq ($(wildcard /opt/xt-os),)
  # on XT3
  QKCC=						qk-gcc
  LIBL=						-lfl
  DEFINES+=					-DHAVE_CNOS
endif

ifeq (${OSTYPE},Linux)
  LIBRT=					-lrt
  DEFINES+=					-D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE
  DEFINES+=					-D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE
endif

ifeq (${OSTYPE},Darwin)
  DEFINES+=					-D_DARWIN_C_SOURCE -D_DARWIN_FEATURE_64_BIT_INODE
endif

ifeq (${OSTYPE},OpenBSD)
  DEFINES+=					-D_BSD_SOURCE
endif

include ${ROOTDIR}/mk/pickle.mk
