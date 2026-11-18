# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

STRINGS_SOURCES := \
	userland/base/strings/main.c

$(eval $(call USERLAND_PACKAGE,strings,strings,*,y,basic,$(STRINGS_SOURCES),y,base,))
