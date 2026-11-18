# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

ICONV_SOURCES := \
	userland/base/iconv/main.c

$(eval $(call USERLAND_PACKAGE,iconv,iconv,*,y,basic,$(ICONV_SOURCES),y,base,))
