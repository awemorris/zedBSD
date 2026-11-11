# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

TR_SOURCES := \
	userland/base/tr/main.c

$(eval $(call USERLAND_PACKAGE,tr,tr,*,y,basic,$(TR_SOURCES),y,base,))
