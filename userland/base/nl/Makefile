# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

NL_SOURCES := \
	userland/base/nl/main.c

$(eval $(call USERLAND_PACKAGE,nl,nl,*,y,basic,$(NL_SOURCES),y,base,))
