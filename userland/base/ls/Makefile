# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LS_SOURCES := \
	userland/base/ls/main.c

$(eval $(call USERLAND_PACKAGE,ls,ls,*,y,basic,$(LS_SOURCES),y,base,))
