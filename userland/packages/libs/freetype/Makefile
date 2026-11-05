# zedBSD userland package: FreeType (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# FreeType is dual-licensed, FTL OR GPL-2.0-or-later; zedBSD takes it under
# the FreeType License (FTL).  It stays outside the source tree: only this
# Makefile and the recorded archive identity live here.  It rasterizes the
# glyphs that HarfBuzz shapes and fontconfig lists, for the GTK port (WS115)
# and later Qt (WS116).  Provenance is in plan/ws034/package-inventory.md and
# plan/ws115/port-contract.md.
FREETYPE_MAKEFILE := $(lastword $(MAKEFILE_LIST))
FREETYPE_DIR := $(patsubst %/,%,$(dir $(FREETYPE_MAKEFILE)))
include $(FREETYPE_DIR)/../../../base/package.mk
include $(FREETYPE_DIR)/../../external.mk

# 2.14.3 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; the archive's signature was checked (WS034 p001).
# The archive comes from the FreeType project's SourceForge release area,
# where the savannah mirrors redirect too; the bytes are the same.
EXT_freetype_VERSION := 2.14.3
EXT_freetype_ROOT := freetype-2.14.3
EXT_freetype_ARCHIVE := freetype-2.14.3.tar.xz
EXT_freetype_URL := https://downloads.sourceforge.net/project/freetype/freetype2/2.14.3/freetype-2.14.3.tar.xz
EXT_freetype_SIZE := 2670220
EXT_freetype_SHA256 := 36bc4f1cc413335368ee656c42afca65c5a3987e8768cc28cf11ba775e785a5f
EXT_freetype_PATCH_LEVEL := zedbsd0
EXT_freetype_PATCHES :=

$(eval $(call EXTERNAL_SOURCE,freetype))

# ---------------------------------------------------------------- build

# Meson, through the common rule.  HarfBuzz uses FreeType and FreeType can
# use HarfBuzz for its auto-hinter; FreeType is built first and without it,
# which only leaves the auto-hinter's script coverage at its default
# (WS034 inventory §2.5).  PNG is on, because colour emoji fonts such as
# Noto Color Emoji keep their glyphs as PNG images; zlib reads compressed
# WOFF and PCF fonts.  bzip2 and Brotli (WOFF2) are not in the tree.
EXT_freetype_MAKEFILE := $(FREETYPE_MAKEFILE)
EXT_freetype_DEPENDS := zlib libpng
EXT_freetype_HOST_TOOLS :=
EXT_freetype_MESON_OPTIONS := \
	-Dharfbuzz=disabled -Dpng=enabled -Dzlib=system \
	-Dbzip2=disabled -Dbrotli=disabled -Dtests=disabled

$(eval $(call EXTERNAL_MESON,freetype))

FREETYPE_STAGED := $(EXT_freetype_STAGEDIR)/.zedbsd-staged
FREETYPE_CHECKED := $(EXT_freetype_STAGEDIR)/.zedbsd-checked
FREETYPE_SONAME := libfreetype.so.6
FREETYPE_LIBRARY := $(EXT_freetype_STAGEDIR)/usr/lib/libfreetype.so.6.20.6
FREETYPE_LICENSE := $(EXT_freetype_SRCDIR)/docs/FTL.TXT

# Checks the library against its dependency contract once the stage exists.
$(FREETYPE_CHECKED): $(FREETYPE_STAGED)
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname $(FREETYPE_SONAME) \
		--needed libz.so.1 --needed libpng16.so.16 --needed libc.so \
		'$(FREETYPE_LIBRARY)'
	@touch '$@'

$(FREETYPE_LIBRARY): $(FREETYPE_STAGED)
	@test -f '$@'

$(FREETYPE_LICENSE): $(EXT_freetype_SRCSTAMP)
	@test -f '$@'

.PHONY: freetype
freetype: $(FREETYPE_CHECKED)
	@:

# ---------------------------------------------------------------- packaging

# The library goes in under its SONAME, which is the name the loader asks
# for.  The headers and the .pc file are the development files below.
$(eval $(call USERLAND_PACKAGE,freetype,FreeType font rasterizer,*,n,package,,y,\
	packages/libs,libs/freetype,libs/zlib libs/libpng))

PACKAGE_INPUTS += $(if $(filter freetype,$(USER_PROGRAMS)),\
	$(FREETYPE_CHECKED) $(FREETYPE_LIBRARY) $(FREETYPE_LICENSE))
PACKAGE_FILES += $(if $(filter freetype,$(USER_PROGRAMS)),\
	--file /usr/lib/$(FREETYPE_SONAME)=$(FREETYPE_LIBRARY) \
	--file /usr/share/licenses/freetype/FTL.TXT=$(FREETYPE_LICENSE))

# The development files: the headers, the .pc files and the unversioned names
# of the libraries (external.mk).
$(eval $(call EXTERNAL_DEVELOPMENT,freetype,$(EXT_freetype_STAGEDIR),\
	lib/pkgconfig/freetype2.pc,\
	include/freetype2,\
	$(FREETYPE_SONAME)))
