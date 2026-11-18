# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

TSORT_SOURCES := \
	userland/base/tsort/main.c

$(eval $(call USERLAND_PACKAGE,tsort,tsort,*,y,basic,$(TSORT_SOURCES),y,base,))
