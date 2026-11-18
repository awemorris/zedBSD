# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

ZEDBSD_PRINTF_SOURCES := userland/base/printf/main.c userland/base/sh/printf.c \
	userland/base/common/builtin-standalone.c
$(eval $(call USERLAND_PACKAGE,printf,printf,*,y,basic,$(ZEDBSD_PRINTF_SOURCES),y,base,,))
