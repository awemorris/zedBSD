# zedBSD userland package: Pango (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# Pango is LGPL-2.0-or-later (its COPYING is the Library GPL, version 2) and
# stays outside the source tree: only this Makefile and the recorded archive
# identity live here.  It lays out and draws the text of the GTK port
# (WS115).  Provenance and the choice of 1.56 (the series GTK 4.18 is made
# with) are in plan/ws115/port-contract.md.
PANGO_MAKEFILE := $(lastword $(MAKEFILE_LIST))
PANGO_DIR := $(patsubst %/,%,$(dir $(PANGO_MAKEFILE)))
include $(PANGO_DIR)/../../../base/package.mk
include $(PANGO_DIR)/../../external.mk

# 1.56.4 is the last release of the 1.56 series.  Size and digest are the
# upstream bytes as measured; they match the digest GNOME publishes.
EXT_pango_VERSION := 1.56.4
EXT_pango_ROOT := pango-1.56.4
EXT_pango_ARCHIVE := pango-1.56.4.tar.xz
EXT_pango_URL := https://download.gnome.org/sources/pango/1.56/pango-1.56.4.tar.xz
EXT_pango_SIZE := 1883988
EXT_pango_SHA256 := 17065e2fcc5f5a5bdbffc884c956bfc7c451a96e8c4fb2f8ad837c6413cb5a01
EXT_pango_PATCH_LEVEL := zedbsd2
EXT_pango_PATCHES := \
	$(PANGO_DIR)/patches/0001-keep-unused-but-set-global-a-warning.patch

$(eval $(call EXTERNAL_SOURCE,pango))

# ---------------------------------------------------------------- build

# Meson, through the common rule.  The fontconfig and FreeType backend
# (pangoft2) and the Cairo renderer (pangocairo) are what GTK uses on
# Wayland.  Xft, libthai, sysprof, introspection, the documentation, the
# tests and the examples are off.  HarfBuzz is in the dependency closure, and
# with it the C++ runtime package whose headers HarfBuzz compiled against.
EXT_pango_MAKEFILE := $(PANGO_MAKEFILE)
EXT_pango_DEPENDS := glib fribidi harfbuzz fontconfig freetype cairo
EXT_pango_HOST_TOOLS :=
EXT_pango_MESON_OPTIONS := \
	-Dfontconfig=enabled -Dfreetype=enabled -Dcairo=enabled \
	-Dxft=disabled -Dlibthai=disabled -Dsysprof=disabled \
	-Dintrospection=disabled -Ddocumentation=false -Dgtk_doc=false \
	-Dman-pages=false -Dbuild-testsuite=false -Dbuild-examples=false

$(eval $(call EXTERNAL_MESON,pango))

PANGO_STAGED := $(EXT_pango_STAGEDIR)/.zedbsd-staged
PANGO_CHECKED := $(EXT_pango_STAGEDIR)/.zedbsd-checked
PANGO_LIBDIR := $(EXT_pango_STAGEDIR)/usr/lib
PANGO_LICENSE := $(EXT_pango_SRCDIR)/COPYING

# The libraries, each under its SONAME, with the libraries it needs.  The
# list is the dependency contract.  The file name carries upstream's
# version (0.5600.4 for 1.56.4).
PANGO_LIBRARIES := pango pangoft2 pangocairo
PANGO_SONAME_pango := libpango-1.0.so.0
PANGO_NEEDED_pango := libglib-2.0.so.0 libgobject-2.0.so.0 libgio-2.0.so.0 \
	libfribidi.so.0 libharfbuzz.so.0 libc.so
PANGO_SONAME_pangoft2 := libpangoft2-1.0.so.0
PANGO_NEEDED_pangoft2 := libpango-1.0.so.0 libglib-2.0.so.0 libgobject-2.0.so.0 \
	libgio-2.0.so.0 libharfbuzz.so.0 libfontconfig.so.1 libfreetype.so.6 libc.so
PANGO_SONAME_pangocairo := libpangocairo-1.0.so.0
PANGO_NEEDED_pangocairo := libpango-1.0.so.0 libpangoft2-1.0.so.0 libglib-2.0.so.0 \
	libgobject-2.0.so.0 libharfbuzz.so.0 libfontconfig.so.1 libcairo.so.2 libc.so
PANGO_REALNAME = $(PANGO_LIBDIR)/$(PANGO_SONAME_$(1)).5600.4

# Checks every library against its contract once the stage exists.
$(PANGO_CHECKED): $(PANGO_STAGED)
	@set -e; $(foreach library,$(PANGO_LIBRARIES),\
		$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname $(PANGO_SONAME_$(library)) \
		$(foreach needed,$(PANGO_NEEDED_$(library)),--needed $(needed)) \
		'$(call PANGO_REALNAME,$(library))';)
	@touch '$@'

$(PANGO_LICENSE): $(EXT_pango_SRCSTAMP)
	@test -f '$@'

.PHONY: pango
pango: $(PANGO_CHECKED)
	@:

# ---------------------------------------------------------------- packaging

# Each library goes in under its SONAME.  The headers and the .pc files are
# the development files below; pango-view and the other tools are not
# installed.
$(eval $(call USERLAND_PACKAGE,pango,Text layout library,*,n,package,,y,\
	packages/libs,libs/pango,libs/glib libs/fribidi libs/harfbuzz libs/fontconfig libs/freetype libs/cairo))

PACKAGE_INPUTS += $(if $(filter pango,$(USER_PROGRAMS)),\
	$(PANGO_CHECKED) $(PANGO_LICENSE))
PACKAGE_FILES += $(if $(filter pango,$(USER_PROGRAMS)),\
	$(foreach library,$(PANGO_LIBRARIES),\
		--file /usr/lib/$(PANGO_SONAME_$(library))=$(call PANGO_REALNAME,$(library))) \
	--file /usr/share/licenses/pango/COPYING=$(PANGO_LICENSE))

# The development files: the headers, the .pc files and the unversioned names
# of the libraries (external.mk).
$(eval $(call EXTERNAL_DEVELOPMENT,pango,$(EXT_pango_STAGEDIR),\
	$(addprefix lib/pkgconfig/,pango.pc pangocairo.pc pangofc.pc pangoft2.pc pangoot.pc),\
	include/pango-1.0,\
	$(foreach library,$(PANGO_LIBRARIES),$(PANGO_SONAME_$(library)))))
