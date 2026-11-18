# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

FILE_SOURCES := \
	userland/base/file/main.c

$(eval $(call USERLAND_PACKAGE,file,file,*,y,basic,$(FILE_SOURCES),y,base,))
