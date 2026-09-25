include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ZEDBSD_ECHO_SOURCES := userland/base/echo/main.c userland/base/sh/printf.c \
	userland/base/common/builtin-standalone.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,echo,echo,*,y,basic,$(ZEDBSD_ECHO_SOURCES),y,base,,))
