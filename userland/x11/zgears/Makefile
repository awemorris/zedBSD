include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk

# Gears (WS069 p005): three lit gears turning, drawn with OpenGL 1.x's fixed function through GLX (links libGL and xserver's libGLX).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ZGEARS_SOURCES := userland/x11/zgears/main.c userland/x11/libX11/xlib.c
$(eval $(call USERLAND_PACKAGE,zgears,Gears (OpenGL),amd64,y,basic,$(ZGEARS_SOURCES),y,x11-applications,X11/zgears,desktop/libGL desktop/xserver,0755,program,,,bin))
