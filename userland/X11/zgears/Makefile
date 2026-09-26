include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk

# Gears (WS069 p005): three lit gears turning, drawn with OpenGL 1.x's fixed function through GLX.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ZGEARS_SOURCES := userland/X11/zgears/main.c userland/X11/libX11/xlib.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,zgears,Gears (OpenGL),amd64,y,basic,$(ZGEARS_SOURCES),y,x11-applications,X11/zgears,X11/libGL,0755,program,,,bin))
