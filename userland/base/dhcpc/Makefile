# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

DHCPC_SOURCES := \
	userland/base/dhcpc/main.c \
	userland/base/dhcpc/inet6.c \
	userland/base/net/dhcp6.c

$(eval $(call USERLAND_PACKAGE,dhcpc,dhcpc,*,y,network,$(DHCPC_SOURCES),y,base,,,0755,program,,,sbin))
