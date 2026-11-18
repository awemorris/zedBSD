# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

UNCOMPRESS_SOURCES := \
	userland/base/compress/main.c \
	userland/base/common/lzw.c

$(eval $(call USERLAND_PACKAGE,uncompress,uncompress,*,y,basic,$(UNCOMPRESS_SOURCES),y,base,))
