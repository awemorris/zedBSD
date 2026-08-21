# zedBSD Unicode VT100 terminal
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
$(eval $(call ZEDBSD_USERLAND_PACKAGE,xzedterm,xzedterm,*,n,basic,userland/X11/xzedterm/main.c userland/X11/libX11/xlib.c,y,X11,,))
ZEDBSD_PACKAGE_INPUTS += userland/X11/xzedterm/app-icon.xpm
ZEDBSD_PACKAGE_FILES += $(if $(filter xzedterm,$(ZEDBSD_USER_PROGRAMS)),--file /usr/share/xzedterm/icons/app-icon.xpm=userland/X11/xzedterm/app-icon.xpm)
