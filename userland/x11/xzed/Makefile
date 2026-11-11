# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

# zedBSD legacy X11 server package

include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk

XZED_SOURCES := userland/x11/xzed/main.c userland/x11/xzed/input.c \
	userland/x11/xzed/input-posix.c
$(eval $(call USERLAND_PACKAGE,Xzed,Xzed,*,n,basic,$(XZED_SOURCES),y,x11-servers,,))
