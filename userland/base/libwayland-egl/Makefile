include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The Wayland EGL window (WS068): the standard small library EGL's Wayland platform uses.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
LIBWAYLAND_EGL_SOURCES := userland/base/libwayland-egl/wayland-egl.c

LIBWAYLAND_EGL_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libwayland-egl.so=$(BUILD)/dynamic/libwayland-egl.so)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libwayland-egl,Wayland EGL window,amd64,y,library,$(LIBWAYLAND_EGL_SOURCES),y,base,base/libwayland-egl,,,shared-library,$(LIBWAYLAND_EGL_IMAGE_DATA),,lib))
