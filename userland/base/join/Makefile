# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

JOIN_SOURCES := \
	userland/base/join/main.c

$(eval $(call USERLAND_PACKAGE,join,join,*,y,basic,$(JOIN_SOURCES),y,base,))
