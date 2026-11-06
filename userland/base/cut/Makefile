# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

CUT_SOURCES := \
	userland/base/cut/main.c

$(eval $(call USERLAND_PACKAGE,cut,cut,*,y,basic,$(CUT_SOURCES),y,base,))
