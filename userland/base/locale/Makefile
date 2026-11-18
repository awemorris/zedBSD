# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LOCALE_SOURCES := \
	userland/base/locale/main.c

$(eval $(call USERLAND_PACKAGE,locale,locale,*,y,basic,$(LOCALE_SOURCES),y,base,))
