# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

WRITE_SOURCES := \
	userland/base/write/main.c

$(eval $(call USERLAND_PACKAGE,write,write,*,y,basic,$(WRITE_SOURCES),y,base,))
