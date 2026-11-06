# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

SPLIT_SOURCES := \
	userland/base/split/main.c

$(eval $(call USERLAND_PACKAGE,split,split,*,y,basic,$(SPLIT_SOURCES),y,base,))
