# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk

$(eval $(call USERLAND_PACKAGE,zwm,zwm,*,n,basic,userland/x11/zwm/main.c userland/x11/libX11/xlib.c,y,x11-applications,,,,,\
	/usr/share/zwm/backgrounds/640x480.ppm=userland/x11/zwm/backgrounds/640x480.ppm \
	/usr/share/zwm/backgrounds/800x600.ppm=userland/x11/zwm/backgrounds/800x600.ppm \
	/usr/share/zwm/backgrounds/1024x768.ppm=userland/x11/zwm/backgrounds/1024x768.ppm))
