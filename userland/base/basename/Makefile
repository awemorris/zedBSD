# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

BASENAME_SOURCES := \
	userland/base/basename/main.c

$(eval $(call USERLAND_PACKAGE,basename,basename,*,y,basic,$(BASENAME_SOURCES),y,base,))
