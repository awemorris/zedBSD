# zedBSD userland package: FreeType (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# FreeType is dual-licensed, FTL OR GPL-2.0-or-later; zedBSD takes it under
# the FreeType License (FTL).  It stays outside the source tree: only this
# Makefile and the recorded archive identity live here.  It rasterizes the
# glyphs that HarfBuzz shapes and fontconfig lists, for the GTK port (WS115)
# and later Qt (WS116).  Provenance is in plan/ws034/package-inventory.md and
# plan/ws115/port-contract.md.
ZEDBSD_FREETYPE_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_FREETYPE_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_FREETYPE_MAKEFILE)))
include $(ZEDBSD_FREETYPE_DIR)/../../../base/package.mk
include $(ZEDBSD_FREETYPE_DIR)/../../external.mk

# 2.14.3 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; the archive's signature was checked (WS034 p001).
# The archive comes from the FreeType project's SourceForge release area,
# where the savannah mirrors redirect too; the bytes are the same.
ZEDBSD_EXT_freetype_VERSION := 2.14.3
ZEDBSD_EXT_freetype_ROOT := freetype-2.14.3
ZEDBSD_EXT_freetype_ARCHIVE := freetype-2.14.3.tar.xz
ZEDBSD_EXT_freetype_URL := https://downloads.sourceforge.net/project/freetype/freetype2/2.14.3/freetype-2.14.3.tar.xz
ZEDBSD_EXT_freetype_SIZE := 2670220
ZEDBSD_EXT_freetype_SHA256 := 36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f
ZEDBSD_EXT_freetype_PATCH_LEVEL := zedbsd0
ZEDBSD_EXT_freetype_PATCHES :=

$(eval $(call ZEDBSD_EXTERNAL_SOURCE,freetype))

# ---------------------------------------------------------------- build

# Meson, through the common rule.  HarfBuzz uses FreeType and FreeType can
# use HarfBuzz for its auto-hinter; FreeType is built first and without it,
# which only leaves the auto-hinter's script coverage at its default
# (WS034 inventory §2.5).  PNG is on, because colour emoji fonts such as
# Noto Color Emoji keep their glyphs as PNG images; zlib reads compressed
# WOFF and PCF fonts.  bzip2 and Brotli (WOFF2) are not in the tree.
ZEDBSD_EXT_freetype_MAKEFILE := $(ZEDBSD_FREETYPE_MAKEFILE)
ZEDBSD_EXT_freetype_DEPENDS := zlib libpng
ZEDBSD_EXT_freetype_HOST_TOOLS :=
ZEDBSD_EXT_freetype_MESON_OPTIONS := \
	-Dharfbuzz=disabled -Dpng=enabled -Dzlib=system \
	-Dbzip2=disabled -Dbrotli=disabled -Dtests=disabled

$(eval $(call ZEDBSD_EXTERNAL_MESON,freetype))

ZEDBSD_FREETYPE_STAGED := $(ZEDBSD_EXT_freetype_STAGEDIR)/.zedbsd-staged
ZEDBSD_FREETYPE_CHECKED := $(ZEDBSD_EXT_freetype_STAGEDIR)/.zedbsd-checked
ZEDBSD_FREETYPE_SONAME := libfreetype.so.6
ZEDBSD_FREETYPE_LIBRARY := $(ZEDBSD_EXT_freetype_STAGEDIR)/usr/lib/libfreetype.so.6.20.6
ZEDBSD_FREETYPE_LICENSE := $(ZEDBSD_EXT_freetype_SRCDIR)/docs/FTL.TXT

# Checks the library against its dependency contract once the stage exists.
$(ZEDBSD_FREETYPE_CHECKED): $(ZEDBSD_FREETYPE_STAGED)
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role shared-library \
		--soname $(ZEDBSD_FREETYPE_SONAME) \
		--needed libz.so.1 --needed libpng16.so.16 --needed libc.so \
		'$(ZEDBSD_FREETYPE_LIBRARY)'
	@touch '$@'

$(ZEDBSD_FREETYPE_LIBRARY): $(ZEDBSD_FREETYPE_STAGED)
	@test -f '$@'

$(ZEDBSD_FREETYPE_LICENSE): $(ZEDBSD_EXT_freetype_SRCSTAMP)
	@test -f '$@'

.PHONY: freetype
freetype: $(ZEDBSD_FREETYPE_CHECKED)
	@:

# ---------------------------------------------------------------- packaging

# The library goes in under its SONAME, which is the name the loader asks
# for.  The headers and the .pc file are the development files below.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,freetype,FreeType font rasterizer,*,n,package,,y,\
	packages/libs,libs/freetype,libs/zlib libs/libpng))

ZEDBSD_PACKAGE_INPUTS += $(if $(filter freetype,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_FREETYPE_CHECKED) $(ZEDBSD_FREETYPE_LIBRARY) $(ZEDBSD_FREETYPE_LICENSE))
ZEDBSD_PACKAGE_FILES += $(if $(filter freetype,$(ZEDBSD_USER_PROGRAMS)),\
	--file /usr/lib/$(ZEDBSD_FREETYPE_SONAME)=$(ZEDBSD_FREETYPE_LIBRARY) \
	--file /usr/share/licenses/freetype/FTL.TXT=$(ZEDBSD_FREETYPE_LICENSE))

# The development files: the headers, the .pc files and the unversioned names
# of the libraries (external.mk).
$(eval $(call ZEDBSD_EXTERNAL_DEVELOPMENT,freetype,$(ZEDBSD_EXT_freetype_STAGEDIR),\
	lib/pkgconfig/freetype2.pc,\
	include/freetype2,\
	$(ZEDBSD_FREETYPE_SONAME)))
