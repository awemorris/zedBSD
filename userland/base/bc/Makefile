include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ZEDBSD_BC_SOURCES := userland/base/bc/main.c userland/base/bc/number.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,bc,bc,*,y,basic,$(ZEDBSD_BC_SOURCES),y,base,,))
