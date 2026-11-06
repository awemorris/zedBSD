# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

AT_SOURCES := \
	userland/base/at/main.c

$(eval $(call USERLAND_PACKAGE,at,at,*,y,basic,$(AT_SOURCES),y,base,))
