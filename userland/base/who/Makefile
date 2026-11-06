# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

WHO_SOURCES := \
	userland/base/who/main.c

$(eval $(call USERLAND_PACKAGE,who,who,*,y,basic,$(WHO_SOURCES),y,base,))
