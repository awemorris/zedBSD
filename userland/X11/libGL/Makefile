include $(dir $(lastword $(MAKEFILE_LIST)))../../base/package.mk

# OpenGL with GLX for Xzed (WS069 p004, p005): the OpenGL ES translation to Vulkan (libGLESv2's sources),
# the fixed-function OpenGL 1.x over it, and GLX, which draws into an EGL pbuffer and puts each frame into
# the X window; libX11 is built in (X programs build it into themselves too).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
LIBGL_SOURCES := userland/X11/libGL/glx.c userland/X11/libGL/fixed.c userland/X11/libGL/immediate.c \
	userland/X11/libX11/xlib.c \
	userland/base/libglesv2/gles.c userland/base/libglesv2/buffer.c userland/base/libglesv2/texture.c \
	userland/base/libglesv2/spirv.c userland/base/libglesv2/program.c userland/base/libglesv2/draw.c \
	userland/base/libglesv2/framebuffer.c

# The public SONAME follows the standard spelling.
LIBGL_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libGL.so=$(BUILD)/dynamic/libGL.so)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libgl,OpenGL and GLX,amd64,y,library,$(LIBGL_SOURCES),y,x11-applications,X11/libGL,base/libegl,,shared-library,$(LIBGL_IMAGE_DATA),,lib))
