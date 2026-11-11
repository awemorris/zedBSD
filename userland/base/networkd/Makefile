# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

NETWORKD_SOURCES := \
	userland/base/networkd/main.c \
	userland/base/networkd/request-frame.c \
	userland/base/networkd/confirmed.c \
	userland/base/networkd/wifi-child.c \
	userland/base/networkd/managed-wlan.c \
	userland/base/networkd/sleep-state.c \
	userland/base/networkd/managed-lan.c \
	userland/base/networkd/lan-configure.c \
	userland/base/networkd/ipv6.c \
	userland/base/networkd/resolver6.c \
	userland/base/networkd/slaac.c \
	userland/base/common/sha256.c \
	userland/base/net/netconf.c \
	userland/base/net/protocol.c \
	userland/base/net/wifi-conf.c \
	userland/base/net/wifi-store.c

$(eval $(call USERLAND_PACKAGE,networkd,networkd,*,y,network,$(NETWORKD_SOURCES),n,base,base/net,,0755,program,,,sbin))
