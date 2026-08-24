include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

ZEDBSD_ED_SOURCES := userland/base/ed/buf.c userland/base/ed/glbl.c \
	userland/base/ed/io.c userland/base/ed/main.c userland/base/ed/re.c \
	userland/base/ed/sub.c userland/base/ed/undo.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,ed,ed,*,y,basic,$(ZEDBSD_ED_SOURCES),y,base,,))
