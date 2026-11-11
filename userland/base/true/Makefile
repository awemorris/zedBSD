# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

TRUE_SOURCES := \
	userland/base/true/main.c

$(eval $(call USERLAND_PACKAGE,true,true,*,y,basic,$(TRUE_SOURCES),y,base,))
