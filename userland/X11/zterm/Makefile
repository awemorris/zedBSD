# zedBSD Unicode VT100 terminal
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,zterm,zterm,*,n,basic,userland/X11/zterm/main.c userland/X11/libX11/xlib.c,y,x11-applications,,))
ZEDBSD_PACKAGE_INPUTS += userland/X11/zterm/app-icon.xpm
ZEDBSD_PACKAGE_FILES += $(if $(filter zterm,$(ZEDBSD_USER_PROGRAMS)),--file /usr/share/zterm/icons/app-icon.xpm=userland/X11/zterm/app-icon.xpm)
