include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ZEDBSD_ED_SOURCES := userland/base/ed/main.c userland/base/ed/buffer.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,ed,ed,*,y,basic,$(ZEDBSD_ED_SOURCES),y,base,,))
