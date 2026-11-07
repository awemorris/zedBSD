# The Linux build of libgif-compat.so; the zedBSD build remains in Makefile.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LIBGLESV2_GLSL_SOURCES := \
	userland/desktop/libglesv2/glsl/arena.c \
	userland/desktop/libglesv2/glsl/lex.c \
	userland/desktop/libglesv2/glsl/preprocess.c \
	userland/desktop/libglesv2/glsl/parse.c \
	userland/desktop/libglesv2/glsl/types.c \
	userland/desktop/libglesv2/glsl/builtins.c \
	userland/desktop/libglesv2/glsl/check.c \
	userland/desktop/libglesv2/glsl/fold.c \
	userland/desktop/libglesv2/glsl/glsl.c \
	userland/desktop/libglesv2/glsl/module.c \
	userland/desktop/libglesv2/glsl/emit.c \
	userland/desktop/libglesv2/glsl/emit-builtin.c \
	userland/desktop/libglesv2/glsl/link.c

LIBGLESV2_SOURCES := \
	userland/desktop/libglesv2/gles.c \
	userland/desktop/libglesv2/buffer.c \
	userland/desktop/libglesv2/texture.c \
	userland/desktop/libglesv2/spirv.c \
	userland/desktop/libglesv2/program.c \
	userland/desktop/libglesv2/draw.c \
	userland/desktop/libglesv2/framebuffer.c \
	userland/desktop/libglesv2/format.c \
	userland/desktop/libglesv2/etc2.c \
	userland/desktop/libglesv2/pixels.c \
	userland/desktop/libglesv2/query.c \
	userland/desktop/libglesv2/feedback.c \
	userland/desktop/libglesv2/compute.c \
	userland/desktop/libglesv2/es31.c \
	$(LIBGLESV2_GLSL_SOURCES)

LIBGLESV2_IMAGE_DATA := $(if $(TOPLEVEL_BUILD),/lib/libGLESv2.so=$(BUILD)/dynamic/libGLESv2.so)

$(eval $(call USERLAND_PACKAGE,libglesv2,libGLESv2,x86_64,y,library,$(LIBGLESV2_SOURCES),y,desktop,desktop/libglesv2,desktop/libegl,,shared-library,$(LIBGLESV2_IMAGE_DATA),,lib))
