# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

SORT_SOURCES := \
	userland/base/sort/main.c

$(eval $(call USERLAND_PACKAGE,sort,sort,*,y,basic,$(SORT_SOURCES),y,base,))
