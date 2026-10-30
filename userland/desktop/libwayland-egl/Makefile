include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The Wayland EGL window (WS068): the standard small library EGL's Wayland platform uses.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
LIBWAYLAND_EGL_SOURCES := userland/desktop/libwayland-egl/wayland-egl.c

# The library, and the .pc file for a program built on the system.
LIBWAYLAND_EGL_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libwayland-egl.so=$(BUILD)/dynamic/libwayland-egl.so \
	/usr/lib/pkgconfig/wayland-egl.pc=userland/desktop/libwayland/pkgconfig/wayland-egl.pc)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libwayland-egl,libwayland-egl,amd64,y,library,$(LIBWAYLAND_EGL_SOURCES),y,desktop,desktop/libwayland-egl,,,shared-library,$(LIBWAYLAND_EGL_IMAGE_DATA),,lib))
