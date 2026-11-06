# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

RMDIR_SOURCES := \
	userland/base/rmdir/main.c

$(eval $(call USERLAND_PACKAGE,rmdir,rmdir,*,y,basic,$(RMDIR_SOURCES),y,base,))
