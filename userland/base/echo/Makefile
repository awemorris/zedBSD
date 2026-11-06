# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

ZEDBSD_ECHO_SOURCES := userland/base/echo/main.c userland/base/sh/printf.c \
	userland/base/common/builtin-standalone.c
$(eval $(call USERLAND_PACKAGE,echo,echo,*,y,basic,$(ZEDBSD_ECHO_SOURCES),y,base,,))
