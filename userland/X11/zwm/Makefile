# zedBSD minimal X11 window manager
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk

$(eval $(call ZEDBSD_USERLAND_PACKAGE,zwm,zwm,*,n,basic,userland/X11/zwm/main.c userland/X11/libX11/xlib.c,y,x11-applications,,))
ZWM_BACKGROUNDS := userland/X11/zwm/backgrounds/640x480.ppm \
	userland/X11/zwm/backgrounds/800x600.ppm \
	userland/X11/zwm/backgrounds/1024x768.ppm
ZEDBSD_PACKAGE_INPUTS += $(ZWM_BACKGROUNDS)
ZEDBSD_PACKAGE_FILES += $(if $(filter zwm,$(ZEDBSD_USER_PROGRAMS)),\
	--file /usr/share/zwm/backgrounds/640x480.ppm=userland/X11/zwm/backgrounds/640x480.ppm \
	--file /usr/share/zwm/backgrounds/800x600.ppm=userland/X11/zwm/backgrounds/800x600.ppm \
	--file /usr/share/zwm/backgrounds/1024x768.ppm=userland/X11/zwm/backgrounds/1024x768.ppm)
