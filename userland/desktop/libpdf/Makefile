include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# libpdf: the PDF library of the desktop programs (plan/ws079/design-pdf.md).  ws079-p004: the
# writer (pages of filled vector paths, fill opacity, images, one attached file), the smoothed
# pressure-width outline of a pen stroke, and the reader of the writer's own documents (pages,
# boxes, content hashes, the attached file).  ws079-p006: the page content interpreted into a
# display list (paths, strokes, colours, opacity, Multiply, images through libjpeg-compat, Flate
# through libz-compat) and the CPU rasterizer of the list.  ws079-p014: the update that adds a
# revision to another program's PDF (pages drawn over, kept or added, the attached file).  ws079-p007:
# text (the text operators, embedded TrueType fonts through libtruetype, substitutes for the others),
# axial and radial shadings, cross-reference streams, object streams and the repair of broken
# cross-references.  ws079-p008: embedded Type 1 and CFF fonts (charstrings.c, type1.c, cff.c,
# cffdata.c) and the standard security handler's decryption (crypt.c).  ws079-p015: the CCITT fax
# decoder (ccitt.c) and the user and owner passwords.  ws175: the editor of a page's objects (editor.c),
# the ToUnicode CMap (tounicode.c), the PNG rows taken as they are (intake.c), and the images and edited
# content compressed through libz-compat's deflate.  ws216-p002: the outlines of factors and ranges, the
# writer's states, Flate content and grains (paint.c), the reader's Flate hash and tiling patterns.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
LIBPDF_SOURCES := userland/desktop/libpdf/writer.c userland/desktop/libpdf/update.c userland/desktop/libpdf/clean.c userland/desktop/libpdf/outline.c \
	userland/desktop/libpdf/paint.c \
	userland/desktop/libpdf/object.c userland/desktop/libpdf/reader.c \
	userland/desktop/libpdf/filter.c userland/desktop/libpdf/ccitt.c userland/desktop/libpdf/image.c userland/desktop/libpdf/display.c \
	userland/desktop/libpdf/content.c userland/desktop/libpdf/editor.c userland/desktop/libpdf/tounicode.c userland/desktop/libpdf/intake.c userland/desktop/libpdf/replace.c userland/desktop/libpdf/embed.c userland/desktop/libpdf/subset.c userland/desktop/libpdf/stroke.c userland/desktop/libpdf/raster.c \
	userland/desktop/libpdf/font.c userland/desktop/libpdf/encoding.c \
	userland/desktop/libpdf/shading.c userland/desktop/libpdf/charstrings.c userland/desktop/libpdf/type1.c \
	userland/desktop/libpdf/cff.c userland/desktop/libpdf/cffdata.c userland/desktop/libpdf/crypt.c

LIBPDF_IMAGE_DATA := $(if $(TOPLEVEL_BUILD),/lib/libpdf.so=$(BUILD)/dynamic/libpdf.so)
$(eval $(call USERLAND_PACKAGE,libpdf,libpdf,*,y,library,$(LIBPDF_SOURCES),y,desktop,desktop/libpdf,base/libz-compat desktop/libjpeg-compat desktop/libtruetype,,shared-library,$(LIBPDF_IMAGE_DATA),,lib))
