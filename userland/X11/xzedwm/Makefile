# zedBSD minimal X11 window manager
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,xzedwm,xzedwm,*,n,basic,userland/X11/xzedwm/main.c userland/X11/libX11/xlib.c,y,X11,,))
XZEDWM_BACKGROUNDS := userland/X11/xzedwm/background/640x480.ppm \
	userland/X11/xzedwm/background/800x600.ppm \
	userland/X11/xzedwm/background/1024x768.ppm
ZEDBSD_PACKAGE_INPUTS += $(XZEDWM_BACKGROUNDS)
ZEDBSD_PACKAGE_FILES += $(if $(filter xzedwm,$(ZEDBSD_USER_PROGRAMS)),\
	--file /usr/share/xzedwm/background/640x480.ppm=userland/X11/xzedwm/background/640x480.ppm \
	--file /usr/share/xzedwm/background/800x600.ppm=userland/X11/xzedwm/background/800x600.ppm \
	--file /usr/share/xzedwm/background/1024x768.ppm=userland/X11/xzedwm/background/1024x768.ppm)
