# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

LIBPDF_SOURCES := \
	userland/desktop/libpdf/writer.c \
	userland/desktop/libpdf/update.c \
	userland/desktop/libpdf/clean.c \
	userland/desktop/libpdf/outline.c \
	userland/desktop/libpdf/paint.c \
	userland/desktop/libpdf/object.c \
	userland/desktop/libpdf/reader.c \
	userland/desktop/libpdf/filter.c \
	userland/desktop/libpdf/ccitt.c \
	userland/desktop/libpdf/image.c \
	userland/desktop/libpdf/display.c \
	userland/desktop/libpdf/content.c \
	userland/desktop/libpdf/editor.c \
	userland/desktop/libpdf/tounicode.c \
	userland/desktop/libpdf/intake.c \
	userland/desktop/libpdf/replace.c \
	userland/desktop/libpdf/embed.c \
	userland/desktop/libpdf/subset.c \
	userland/desktop/libpdf/stroke.c \
	userland/desktop/libpdf/raster.c \
	userland/desktop/libpdf/font.c \
	userland/desktop/libpdf/encoding.c \
	userland/desktop/libpdf/shading.c \
	userland/desktop/libpdf/charstrings.c \
	userland/desktop/libpdf/type1.c \
	userland/desktop/libpdf/cff.c \
	userland/desktop/libpdf/cffdata.c \
	userland/desktop/libpdf/crypt.c

LIBPDF_IMAGE_DATA := $(if $(TOPLEVEL_BUILD),/lib/libpdf.so=$(BUILD)/dynamic/libpdf.so)

$(eval $(call USERLAND_PACKAGE,libpdf,libpdf,*,y,library,$(LIBPDF_SOURCES),y,desktop,desktop/libpdf,base/libz-compat desktop/libjpeg-compat desktop/libtruetype,,shared-library,$(LIBPDF_IMAGE_DATA),,lib))
