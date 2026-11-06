# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

FALSE_SOURCES := \
	userland/base/false/main.c

$(eval $(call USERLAND_PACKAGE,false,false,*,y,basic,$(FALSE_SOURCES),y,base,))
