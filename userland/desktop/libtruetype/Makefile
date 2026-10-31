include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk
include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Independent TrueType reader and glyph rasterizer; no FreeType code.  Every platform builds it since
# ws079-p007 (libpdf draws text with it).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

LIBTRUETYPE_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libtruetype.so=$(BUILD)/dynamic/libtruetype.so)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libtruetype,libtruetype,*,y,library,$(LIBTRUETYPE_SOURCES),y,desktop,desktop/libtruetype,,,shared-library,$(LIBTRUETYPE_IMAGE_DATA),,lib))
