# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

MORE_SOURCES := \
	userland/base/more/main.c

$(eval $(call USERLAND_PACKAGE,more,more,*,y,basic,$(MORE_SOURCES),y,base,))
