# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

XARGS_SOURCES := \
	userland/base/xargs/main.c

$(eval $(call USERLAND_PACKAGE,xargs,xargs,*,y,basic,$(XARGS_SOURCES),y,base,))
