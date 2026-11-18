# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

SWAPON_SOURCES := \
	userland/base/swapon/main.c

$(eval $(call USERLAND_PACKAGE,swapon,swapon,*,y,basic,$(SWAPON_SOURCES),y,base,,,0755,program,,,sbin))
