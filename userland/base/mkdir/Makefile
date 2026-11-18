# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

MKDIR_SOURCES := \
	userland/base/mkdir/main.c \
	userland/base/chmod/mode.c

$(eval $(call USERLAND_PACKAGE,mkdir,mkdir,*,y,basic,$(MKDIR_SOURCES),y,base,))
