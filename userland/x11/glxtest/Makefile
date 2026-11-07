# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk

GLXTEST_SOURCES := userland/x11/glxtest/main.c userland/x11/glxtest/gl3.c userland/x11/glxtest/gl31.c userland/x11/glxtest/gl32.c userland/x11/glxtest/gl33.c userland/tests/egltest/scene.c userland/x11/libX11/xlib.c
$(eval $(call USERLAND_PACKAGE,glxtest,GLX test,x86_64,y,basic,$(GLXTEST_SOURCES),y,x11-applications,X11/glxtest,desktop/libGL desktop/xserver,0755,program,,,bin))
