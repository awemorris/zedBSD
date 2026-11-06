# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

BASE64_SOURCES := \
	userland/base/base64/main.c

$(eval $(call USERLAND_PACKAGE,base64,base64,*,y,basic,$(BASE64_SOURCES),y,base,))
