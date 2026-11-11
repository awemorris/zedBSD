# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

TOP_SOURCES := \
	userland/base/top/main.c

$(eval $(call USERLAND_PACKAGE,top,top,*,y,basic,$(TOP_SOURCES),y,base,))
