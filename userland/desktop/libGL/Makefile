include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# OpenGL (WS069 p004, p005; WS178): the OpenGL ES translation to Vulkan (libGLESv2's sources), the
# fixed-function OpenGL 1.x over it, and OpenGL 3.0's calls that OpenGL ES 3.0 does not have (WS068 p013).
# GL only: GLX is libGLX.so, xserver's library (userland/desktop/xserver/libGLX/), which gives GL the
# current context's version through the private interface of zgl-glx.h.  X programs link -lGL -lGLX.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# libGLESv2's GLSL compiler (WS068 p015-p019), built in with the rest of libGLESv2's sources.
LIBGL_GLSL_SOURCES := userland/desktop/libglesv2/glsl/arena.c userland/desktop/libglesv2/glsl/lex.c \
	userland/desktop/libglesv2/glsl/preprocess.c userland/desktop/libglesv2/glsl/parse.c userland/desktop/libglesv2/glsl/types.c \
	userland/desktop/libglesv2/glsl/builtins.c userland/desktop/libglesv2/glsl/check.c userland/desktop/libglesv2/glsl/fold.c \
	userland/desktop/libglesv2/glsl/glsl.c userland/desktop/libglesv2/glsl/module.c userland/desktop/libglesv2/glsl/emit.c \
	userland/desktop/libglesv2/glsl/emit-builtin.c userland/desktop/libglesv2/glsl/link.c
LIBGL_SOURCES := userland/desktop/libGL/context.c userland/desktop/libGL/fixed.c userland/desktop/libGL/immediate.c userland/desktop/libGL/gl3.c userland/desktop/libGL/gl33.c userland/desktop/libGL/gl40.c userland/desktop/libGL/gl43.c userland/desktop/libGL/gl45.c userland/desktop/libGL/debug.c \
	userland/desktop/libglesv2/gles.c userland/desktop/libglesv2/buffer.c userland/desktop/libglesv2/texture.c \
	userland/desktop/libglesv2/spirv.c userland/desktop/libglesv2/program.c userland/desktop/libglesv2/draw.c \
	userland/desktop/libglesv2/framebuffer.c userland/desktop/libglesv2/format.c userland/desktop/libglesv2/etc2.c userland/desktop/libglesv2/pixels.c userland/desktop/libglesv2/query.c userland/desktop/libglesv2/feedback.c \
	userland/desktop/libglesv2/compute.c $(LIBGL_GLSL_SOURCES)

# The public SONAME follows the standard spelling.
LIBGL_IMAGE_DATA := $(if $(TOPLEVEL_BUILD),/lib/libGL.so=$(BUILD)/dynamic/libGL.so)
$(eval $(call USERLAND_PACKAGE,libgl,libGL,amd64,y,library,$(LIBGL_SOURCES),y,desktop,desktop/libGL,desktop/libegl,,shared-library,$(LIBGL_IMAGE_DATA),,lib))
