# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

TOUCH_SOURCES := \
	userland/base/touch/main.c

$(eval $(call USERLAND_PACKAGE,touch,touch,*,y,basic,$(TOUCH_SOURCES),y,base,))
