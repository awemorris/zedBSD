include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk

# GLX test (WS069 p004): an X window drawn with OpenGL through GLX (egltest's scene; --gl3: an OpenGL 3.0 context, WS068 p013; --gl31: 3.1, p031; --gl32: 3.2, p033; --gl33: 3.3).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
GLXTEST_SOURCES := userland/x11/glxtest/main.c userland/x11/glxtest/gl3.c userland/x11/glxtest/gl31.c userland/x11/glxtest/gl32.c userland/x11/glxtest/gl33.c userland/tests/egltest/scene.c userland/x11/libX11/xlib.c
$(eval $(call USERLAND_PACKAGE,glxtest,GLX test,x86_64,y,basic,$(GLXTEST_SOURCES),y,x11-applications,X11/glxtest,desktop/libGL desktop/xserver,0755,program,,,bin))
