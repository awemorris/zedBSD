# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk

$(eval $(call USERLAND_PACKAGE,zterm,zterm,*,n,basic,userland/x11/zterm/main.c userland/x11/libX11/xlib.c,y,x11-applications,,,,,\
	/usr/share/zterm/icons/app-icon.xpm=userland/x11/zterm/app-icon.xpm))
