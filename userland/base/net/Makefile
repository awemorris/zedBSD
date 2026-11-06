# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

NET_SOURCES := \
	userland/base/net/main.c \
	userland/base/net/protocol.c \
	userland/base/net/netconf.c \
	userland/base/net/reconcile.c \
	userland/base/net/wifi-conf.c \
	userland/base/net/wifi-store.c \
	userland/base/libedit/readline.c \
	userland/base/service/rcconf.c \
	userland/base/service/service-config.c

$(eval $(call USERLAND_PACKAGE,net,net,*,y,network,$(NET_SOURCES),n,base,base/net,base/libedit,0755,program,,,sbin))
