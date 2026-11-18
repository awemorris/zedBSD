# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

BLKID_SOURCES := \
	userland/base/blkid/main.c

$(eval $(call USERLAND_PACKAGE,blkid,blkid,*,y,basic,$(BLKID_SOURCES),y,base,,,0755,program,,,sbin))
