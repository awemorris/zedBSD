# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

CHMOD_SOURCES := \
	userland/base/chmod/main.c \
	userland/base/chmod/mode.c

$(eval $(call USERLAND_PACKAGE,chmod,chmod,*,y,basic,$(CHMOD_SOURCES),y,base,))
