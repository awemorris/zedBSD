# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

STTY_SOURCES := \
	userland/base/stty/main.c

$(eval $(call USERLAND_PACKAGE,stty,stty,*,y,basic,$(STTY_SOURCES),y,base,))
