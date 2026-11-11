# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

READLINK_SOURCES := \
	userland/base/readlink/main.c

$(eval $(call USERLAND_PACKAGE,readlink,readlink,*,y,basic,$(READLINK_SOURCES),y,base,))
