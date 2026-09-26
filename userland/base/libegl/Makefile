include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# EGL 1.5 over Vulkan (WS068): Wayland and display-direct window surfaces, OpenGL ES contexts.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
LIBEGL_SOURCES := userland/base/libegl/egl.c userland/base/libegl/vulkan.c

# The public SONAME follows the standard spelling.
LIBEGL_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libEGL.so=$(BUILD)/dynamic/libEGL.so)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libegl,EGL,amd64,y,library,$(LIBEGL_SOURCES),y,base,base/libegl,base/libvulkan base/libwayland base/libwayland-egl,,shared-library,$(LIBEGL_IMAGE_DATA),,lib))
