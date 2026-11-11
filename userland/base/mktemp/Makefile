# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

MKTEMP_SOURCES := \
	userland/base/mktemp/main.c

$(eval $(call USERLAND_PACKAGE,mktemp,mktemp,*,y,basic,$(MKTEMP_SOURCES),y,base,))
