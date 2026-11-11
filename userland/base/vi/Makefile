# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

ZEDBSD_VI_SOURCES := userland/base/vi/main.c userland/base/vi/command.c \
	userland/base/vi/ex.c userland/base/vi/search.c \
	userland/base/vi/screen.c userland/base/vi/terminal.c \
	userland/base/vi/buffer.c userland/base/vi/undo.c \
	userland/base/vi/file.c userland/base/common/terminfo.c

$(eval $(call USERLAND_PACKAGE,vi,vi,*,y,basic,$(ZEDBSD_VI_SOURCES),y,base,base/vi,base/terminfo))
