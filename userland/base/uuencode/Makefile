# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

UUENCODE_SOURCES := \
	userland/base/uuencode/main.c \
	userland/base/common/uucodec.c

$(eval $(call USERLAND_PACKAGE,uuencode,uuencode,*,y,basic,$(UUENCODE_SOURCES),y,base,))
