# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

PATCH_SOURCES := \
	userland/base/patch/main.c \
	userland/base/patch/parse.c \
	userland/base/patch/apply.c

$(eval $(call USERLAND_PACKAGE,patch,patch,*,y,basic,$(PATCH_SOURCES),y,base,))
