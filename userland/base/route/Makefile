# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

ROUTE_SOURCES := \
	userland/base/route/main.c

$(eval $(call USERLAND_PACKAGE,route,route,*,y,network,$(ROUTE_SOURCES),y,base,,,0755,program,,,sbin))
