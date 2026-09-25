include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ZEDBSD_SED_SOURCES := userland/base/sed/main.c userland/base/sed/compile.c \
	userland/base/sed/execute.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,sed,sed,*,y,basic,$(ZEDBSD_SED_SOURCES),y,base,,))
