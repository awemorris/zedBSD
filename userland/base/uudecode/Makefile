# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

UUDECODE_SOURCES := \
	userland/base/uudecode/main.c \
	userland/base/common/uucodec.c

$(eval $(call USERLAND_PACKAGE,uudecode,uudecode,*,y,basic,$(UUDECODE_SOURCES),y,base,))
