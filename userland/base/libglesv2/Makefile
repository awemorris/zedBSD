include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# OpenGL ES over EGL and Vulkan (WS068).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
LIBGLESV2_SOURCES := userland/base/libglesv2/gles.c

# The public SONAME follows the standard spelling.
LIBGLESV2_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libGLESv2.so=$(BUILD)/dynamic/libGLESv2.so)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libglesv2,OpenGL ES,amd64,y,library,$(LIBGLESV2_SOURCES),y,base,base/libglesv2,base/libegl,,shared-library,$(LIBGLESV2_IMAGE_DATA),,lib))
