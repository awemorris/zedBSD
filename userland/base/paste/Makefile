# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

PASTE_SOURCES := \
	userland/base/paste/main.c

$(eval $(call USERLAND_PACKAGE,paste,paste,*,y,basic,$(PASTE_SOURCES),y,base,))
