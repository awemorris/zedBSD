include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# EGL 1.5 over Vulkan (WS068): Wayland and display-direct window surfaces, OpenGL ES contexts.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
LIBEGL_SOURCES := userland/desktop/libegl/egl.c userland/desktop/libegl/vulkan.c

# The public SONAME follows the standard spelling.
LIBEGL_IMAGE_DATA := $(if $(TOPLEVEL_BUILD),/lib/libEGL.so=$(BUILD)/dynamic/libEGL.so)
$(eval $(call USERLAND_PACKAGE,libegl,libEGL,amd64,y,library,$(LIBEGL_SOURCES),y,desktop,desktop/libegl,desktop/libvulkan desktop/libwayland desktop/libwayland-egl,,shared-library,$(LIBEGL_IMAGE_DATA),,lib))
