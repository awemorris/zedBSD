# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

FMT_SOURCES := \
	userland/base/fmt/main.c

$(eval $(call USERLAND_PACKAGE,fmt,fmt,*,y,basic,$(FMT_SOURCES),y,base,))
