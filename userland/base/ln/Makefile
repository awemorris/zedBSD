# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LN_SOURCES := \
	userland/base/ln/main.c

$(eval $(call USERLAND_PACKAGE,ln,ln,*,y,basic,$(LN_SOURCES),y,base,))
