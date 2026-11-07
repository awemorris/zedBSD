# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LIBWAYLAND_EGL_SOURCES := userland/desktop/libwayland-egl/wayland-egl.c

LIBWAYLAND_EGL_IMAGE_DATA := $(if $(TOPLEVEL_BUILD),/lib/libwayland-egl.so=$(BUILD)/dynamic/libwayland-egl.so \
	/usr/lib/pkgconfig/wayland-egl.pc=userland/desktop/libwayland/pkgconfig/wayland-egl.pc)

$(eval $(call USERLAND_PACKAGE,libwayland-egl,libwayland-egl,x86_64,y,library,$(LIBWAYLAND_EGL_SOURCES),y,desktop,desktop/libwayland-egl,,,shared-library,$(LIBWAYLAND_EGL_IMAGE_DATA),,lib))
