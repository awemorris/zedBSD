# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

TABS_SOURCES := \
	userland/base/tabs/main.c \
	userland/base/common/terminfo.c

$(eval $(call USERLAND_PACKAGE,tabs,tabs,*,y,basic,$(TABS_SOURCES),y,base,base/terminfo))
