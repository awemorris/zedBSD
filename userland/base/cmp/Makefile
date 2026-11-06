# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

CMP_SOURCES := \
	userland/base/cmp/main.c

$(eval $(call USERLAND_PACKAGE,cmp,cmp,*,y,basic,$(CMP_SOURCES),y,base,))
