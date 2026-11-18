# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

SYNC_SOURCES := \
	userland/base/sync/main.c

$(eval $(call USERLAND_PACKAGE,sync,sync,*,y,basic,$(SYNC_SOURCES),y,base,))
