include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# zedBSD userland package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ZEDBSD_GREP_SOURCES := userland/base/grep/main.c userland/base/grep/search.c
$(eval $(call USERLAND_PACKAGE,grep,grep,*,y,basic,$(ZEDBSD_GREP_SOURCES),y,base,,))
