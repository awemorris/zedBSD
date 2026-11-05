# zedBSD userland package: cairo (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# cairo is under LGPL-2.1-only OR MPL-1.1 and stays outside the source tree:
# only this Makefile and the recorded archive identity live here.  It is the
# 2D drawing library GTK draws its software path and its text with, for the
# GTK port (WS115).  Provenance is in plan/ws034/package-inventory.md and
# plan/ws115/port-contract.md.
CAIRO_MAKEFILE := $(lastword $(MAKEFILE_LIST))
CAIRO_DIR := $(patsubst %/,%,$(dir $(CAIRO_MAKEFILE)))
include $(CAIRO_DIR)/../../../base/package.mk
include $(CAIRO_DIR)/../../external.mk

# 1.18.6 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; they match the SHA-256 published beside the
# archive (WS034 p001).
EXT_cairo_VERSION := 1.18.6
EXT_cairo_ROOT := cairo-1.18.6
EXT_cairo_ARCHIVE := cairo-1.18.6.tar.xz
EXT_cairo_URL := https://cairographics.org/releases/cairo-1.18.6.tar.xz
EXT_cairo_SIZE := 32725456
EXT_cairo_SHA256 := 1c767308174337a74694da0f3ec069c271452163a1ef4540964c50c301f157d4
EXT_cairo_PATCH_LEVEL := zedbsd2
EXT_cairo_PATCHES :=

$(eval $(call EXTERNAL_SOURCE,cairo))

# ---------------------------------------------------------------- build

# Meson, through the common rule.  The image surface with PNG, fonts through
# FreeType and fontconfig, cairo-gobject (GTK requires it), and with zlib the
# PDF and PostScript surfaces, which GTK's print-to-file and gtk4-demo use
# (WS115 p002).  Off, and why:
#   xlib, xcb, quartz, dwrite  no such window systems here; GL is gone from
#                         cairo 1.18 altogether
#   lzo, spectre, symbol-lookup, tee  optional extras of the test tools
#   tests, gtk2-utils, gtk_doc
# The SVG surface needs nothing more and stays, as cairo builds it.  The
# alloca patch of WS115 p007 went when the C library gained <alloca.h>.
EXT_cairo_MAKEFILE := $(CAIRO_MAKEFILE)
EXT_cairo_DEPENDS := pixman freetype fontconfig libpng zlib glib
EXT_cairo_HOST_TOOLS :=
EXT_cairo_MESON_OPTIONS := \
	-Dfontconfig=enabled -Dfreetype=enabled -Dpng=enabled -Dglib=enabled \
	-Dzlib=enabled -Ddwrite=disabled -Dquartz=disabled -Dtee=disabled \
	-Dxcb=disabled -Dxlib=disabled -Dxlib-xcb=disabled -Dlzo=disabled \
	-Dspectre=disabled -Dsymbol-lookup=disabled -Dtests=disabled \
	-Dgtk2-utils=disabled -Dgtk_doc=false

$(eval $(call EXTERNAL_MESON,cairo))

CAIRO_STAGED := $(EXT_cairo_STAGEDIR)/.zedbsd-staged
CAIRO_CHECKED := $(EXT_cairo_STAGEDIR)/.zedbsd-checked
CAIRO_LIBDIR := $(EXT_cairo_STAGEDIR)/usr/lib
CAIRO_LICENSES := COPYING COPYING-LGPL-2.1 COPYING-MPL-1.1

# The libraries, each under its SONAME, with the libraries it needs.  The
# list is the dependency contract.  The file name carries upstream's
# version (2.11806.6 for 1.18.6).
CAIRO_LIBRARIES := cairo cairo-gobject cairo-script-interpreter
CAIRO_SONAME_cairo := libcairo.so.2
CAIRO_NEEDED_cairo := libz.so.1 libpng16.so.16 libfontconfig.so.1 libfreetype.so.6 libpixman-1.so.0 libc.so
# cairo-gobject calls nothing in the C library itself, so --as-needed leaves
# libc.so out of it.
CAIRO_SONAME_cairo-gobject := libcairo-gobject.so.2
CAIRO_NEEDED_cairo-gobject := libcairo.so.2 libglib-2.0.so.0 libgobject-2.0.so.0
# The script interpreter comes with zlib; GTK links it to replay recorded
# drawing in its debugging tools.
CAIRO_SONAME_cairo-script-interpreter := libcairo-script-interpreter.so.2
CAIRO_NEEDED_cairo-script-interpreter := libcairo.so.2 libz.so.1 libfontconfig.so.1 libfreetype.so.6 libc.so
CAIRO_REALNAME = $(CAIRO_LIBDIR)/$(CAIRO_SONAME_$(1)).11806.6

# Checks every library against its contract once the stage exists.
$(CAIRO_CHECKED): $(CAIRO_STAGED)
	@set -e; $(foreach library,$(CAIRO_LIBRARIES),\
		$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname $(CAIRO_SONAME_$(library)) \
		$(foreach needed,$(CAIRO_NEEDED_$(library)),--needed $(needed)) \
		'$(call CAIRO_REALNAME,$(library))';)
	@touch '$@'

$(foreach license,$(CAIRO_LICENSES),$(EXT_cairo_SRCDIR)/$(license)): \
	$(EXT_cairo_SRCSTAMP)
	@test -f '$@'

.PHONY: cairo
cairo: $(CAIRO_CHECKED)
	@:

# ---------------------------------------------------------------- packaging

# Each library goes in under its SONAME, with the licence notice and both
# licences it may be taken under.  The headers and the .pc files are the
# development files below.
$(eval $(call USERLAND_PACKAGE,cairo,2D graphics library,*,n,package,,y,\
	packages/libs,libs/cairo,libs/pixman libs/freetype libs/fontconfig libs/libpng libs/zlib libs/glib))

PACKAGE_INPUTS += $(if $(filter cairo,$(USER_PROGRAMS)),\
	$(CAIRO_CHECKED) \
	$(foreach license,$(CAIRO_LICENSES),$(EXT_cairo_SRCDIR)/$(license)))
PACKAGE_FILES += $(if $(filter cairo,$(USER_PROGRAMS)),\
	$(foreach library,$(CAIRO_LIBRARIES),\
		--file /usr/lib/$(CAIRO_SONAME_$(library))=$(call CAIRO_REALNAME,$(library))) \
	$(foreach license,$(CAIRO_LICENSES),\
		--file /usr/share/licenses/cairo/$(license)=$(EXT_cairo_SRCDIR)/$(license)))

# The development files: the headers, the .pc files and the unversioned names
# of the libraries (external.mk).
$(eval $(call EXTERNAL_DEVELOPMENT,cairo,$(EXT_cairo_STAGEDIR),\
	$(addprefix lib/pkgconfig/,cairo.pc cairo-fc.pc cairo-ft.pc cairo-gobject.pc cairo-pdf.pc cairo-png.pc cairo-ps.pc \
	cairo-script.pc cairo-script-interpreter.pc cairo-svg.pc),\
	include/cairo,\
	$(foreach library,$(CAIRO_LIBRARIES),$(CAIRO_SONAME_$(library)))))
