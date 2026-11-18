# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../../package.mk

KEILAND_LIBGLX_SOURCES := userland/desktop/xserver/libGLX/glx.c userland/x11/libX11/xlib.c
KEILAND_LIBGLX_IMAGE_DATA := $(if $(TOPLEVEL_BUILD),/lib/libGLX.so=$(BUILD)/dynamic/libGLX.so)

$(eval $(call USERLAND_PACKAGE,libglx,libGLX,x86_64,y,library,$(KEILAND_LIBGLX_SOURCES),y,desktop,desktop/xserver/libGLX,desktop/libGL desktop/libegl,,shared-library,$(KEILAND_LIBGLX_IMAGE_DATA),,lib))
