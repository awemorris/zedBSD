# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

STAT_SOURCES := \
	userland/base/stat/main.c

$(eval $(call USERLAND_PACKAGE,stat,stat,*,y,basic,$(STAT_SOURCES),y,base,))
