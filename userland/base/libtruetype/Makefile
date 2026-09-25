include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Independent TrueType reader and glyph rasterizer; no FreeType code.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
LIBTRUETYPE_SOURCES := userland/base/libtruetype/face.c \
	userland/base/libtruetype/cmap.c userland/base/libtruetype/outline.c \
	userland/base/libtruetype/render.c userland/base/libtruetype/glyph.c

LIBTRUETYPE_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libtruetype.so=$(BUILD)/dynamic/libtruetype.so)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libtruetype,TrueType glyphs,amd64,y,library,$(LIBTRUETYPE_SOURCES),y,base,base/libtruetype,,,shared-library,$(LIBTRUETYPE_IMAGE_DATA),,lib))
