# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

HEAD_SOURCES := \
	userland/base/head/main.c

$(eval $(call USERLAND_PACKAGE,head,head,*,y,basic,$(HEAD_SOURCES),y,base,))
