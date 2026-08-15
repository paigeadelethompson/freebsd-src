# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Stormshield
#
# Shared build definitions for the two ifconfig variants used by the libxo
# output consistency test.  They are built in separate subdirectories because
# bsd.progs.mk cannot compile the same sources with different CFLAGS (the
# object files would be shared between the two programs).

.if !target(__<sbin_ifconfig_tests_makefile_inc>__)
__<sbin_ifconfig_tests_makefile_inc>__:	.NOTMAIN

.include <src.opts.mk>
.include <bsd.own.mk>

PROG=	ifconfig

#
# NB: The order here defines the order in which the constructors
#     are called.  This in turn defines the default order in which
#     status is displayed.  It must match sbin/ifconfig/Makefile.
#
SRCS=	../../ifconfig.c \
	../../af_link.c
.if ${MK_INET_SUPPORT} != "no"
SRCS+=	../../af_inet.c
.endif
.if ${MK_INET6_SUPPORT} != "no"
SRCS+=	../../af_inet6.c \
	../../af_nd6.c \
	../../ifstf.c
.endif
SRCS+=	../../ifclone.c \
	../../ifmac.c \
	../../ifmedia.c \
	../../iffib.c \
	../../ifvlan.c \
	../../ifvxlan.c \
	../../ifgre.c \
	../../ifgif.c \
	../../ifipsec.c \
	../../sfp.c
.if ${MK_WIRELESS_SUPPORT} != "no"
SRCS+=	../../ifieee80211.c
.endif
SRCS+=	../../carp.c \
	../../ifgroup.c
.if ${MK_PF} != "no"
SRCS+=	../../ifpfsync.c
.endif
SRCS+=	../../ifbridge.c \
	../../iflagg.c \
	../../ifconfig_output.c
.if ${MK_NETLINK_SUPPORT} != "no"
SRCS+=	../../ifconfig_netlink.c \
	../../ifgeneve.c
.endif

CFLAGS+=	-I${SRCTOP}/lib/libifconfig -I${OBJTOP}/lib/libifconfig
.if ${MK_INET6_SUPPORT} != "no"
CFLAGS+=	-DINET6
.endif
.if ${MK_INET_SUPPORT} != "no"
CFLAGS+=	-DINET
.endif
.if ${MK_JAIL} != "no"
CFLAGS+=	-DJAIL
.endif
.if ${MK_NETLINK_SUPPORT} == "no"
CFLAGS+=	-DWITHOUT_NETLINK
.endif
CFLAGS+=	-Wall -Wmissing-prototypes -Wcast-qual -Wwrite-strings \
	    -Wnested-externs

LIBADD+=	ifconfig m util xo nv
.if ${MK_WIRELESS_SUPPORT} != "no"
LIBADD+=	80211
.endif
.if ${MK_JAIL} != "no"
LIBADD+=	jail
.endif

TESTSDIR=	${TESTSBASE}/sbin/ifconfig/${.CURDIR:T}
BINDIR=		${TESTSDIR}
PACKAGE=	tests
MAN=

# The parent tests directory installs into ${TESTSBASE}/sbin/ifconfig, so the
# per-variant binaries live right next to the libxo test script.  Create the
# install directory since manual "make install" does not run hierarchy(7).
beforeinstall:
	${INSTALL} -d ${DESTDIR}${BINDIR}

.endif