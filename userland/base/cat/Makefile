# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

CAT_SOURCES := \
	userland/base/cat/main.c

$(eval $(call USERLAND_PACKAGE,cat,cat,*,y,basic,$(CAT_SOURCES),y,base,))
