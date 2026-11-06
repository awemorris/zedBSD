# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

IFCONFIG_SOURCES := \
	userland/base/ifconfig/main.c

$(eval $(call USERLAND_PACKAGE,ifconfig,ifconfig,*,y,network,$(IFCONFIG_SOURCES),y,base,,,0755,program,,,sbin))
