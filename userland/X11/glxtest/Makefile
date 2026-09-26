include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk

# GLX test (WS069 p004): an X window drawn with OpenGL through GLX (egltest's scene).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
GLXTEST_SOURCES := userland/X11/glxtest/main.c userland/base/egltest/scene.c userland/X11/libX11/xlib.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,glxtest,GLX test,amd64,y,basic,$(GLXTEST_SOURCES),y,x11-applications,X11/glxtest,X11/libGL,0755,program,,,bin))
