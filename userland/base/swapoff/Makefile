# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

SWAPOFF_SOURCES := \
	userland/base/swapoff/main.c

$(eval $(call USERLAND_PACKAGE,swapoff,swapoff,*,y,basic,$(SWAPOFF_SOURCES),y,base,,,0755,program,,,sbin))
