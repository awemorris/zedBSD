include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ZEDBSD_M4_SOURCES := userland/base/m4/main.c userland/base/m4/engine.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,m4,m4,*,y,basic,$(ZEDBSD_M4_SOURCES),y,base,,))
