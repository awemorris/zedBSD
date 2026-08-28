# zedBSD X11 server package
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
XZED_SOURCES := userland/X11/xzed/main.c userland/X11/xzed/input.c \
	userland/X11/xzed/input-posix.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,Xzed,Xzed,*,n,basic,$(XZED_SOURCES),y,x11-servers,,))
