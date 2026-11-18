# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

EXPAND_SOURCES := \
	userland/base/expand/main.c \
	userland/base/expand/tabs.c

$(eval $(call USERLAND_PACKAGE,expand,expand,*,y,basic,$(EXPAND_SOURCES),y,base,))
