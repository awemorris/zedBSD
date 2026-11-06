# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

WIFI_SOURCES := \
	userland/base/wifi/main.c

$(eval $(call USERLAND_PACKAGE,wifi,wifi,*,y,network,$(WIFI_SOURCES),y,base,,,0755,program,,,sbin))
