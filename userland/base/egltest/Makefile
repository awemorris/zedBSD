include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# EGL and OpenGL ES test (WS068): clears or draws into a Wayland window or the screen and presents it.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
EGLTEST_SOURCES := userland/base/egltest/main.c userland/base/egltest/scene.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,egltest,EGL test,amd64,y,basic,$(EGLTEST_SOURCES),y,base,base/egltest,base/libegl base/libglesv2 base/libwayland-egl base/libwayland,0755,program,,,bin))
