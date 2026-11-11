# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

RM_SOURCES := \
	userland/base/rm/main.c

$(eval $(call USERLAND_PACKAGE,rm,rm,*,y,basic,$(RM_SOURCES),y,base,))
