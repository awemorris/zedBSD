# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

OD_SOURCES := \
	userland/base/od/main.c

$(eval $(call USERLAND_PACKAGE,od,od,*,y,basic,$(OD_SOURCES),y,base,))
