# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

DIFF_SOURCES := \
	userland/base/diff/main.c \
	userland/base/diff/lines.c \
	userland/base/diff/tree.c

$(eval $(call USERLAND_PACKAGE,diff,diff,*,y,basic,$(DIFF_SOURCES),y,base,))
