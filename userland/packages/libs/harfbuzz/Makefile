# zedBSD userland package: HarfBuzz (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# HarfBuzz is MIT-licensed ("Old MIT") and stays outside the source tree:
# only this Makefile and the recorded archive identity live here.  It turns
# text into positioned glyphs for Pango and the GTK port (WS115), and later
# for Qt (WS116).  Provenance is in plan/ws034/package-inventory.md and
# plan/ws115/port-contract.md.
ZEDBSD_HARFBUZZ_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_HARFBUZZ_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_HARFBUZZ_MAKEFILE)))
include $(ZEDBSD_HARFBUZZ_DIR)/../../../base/package.mk
include $(ZEDBSD_HARFBUZZ_DIR)/../../external.mk

# 14.5.0 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; they match the digest GitHub gives the release
# asset (WS034 p001).
ZEDBSD_EXT_harfbuzz_VERSION := 14.5.0
ZEDBSD_EXT_harfbuzz_ROOT := harfbuzz-14.5.0
ZEDBSD_EXT_harfbuzz_ARCHIVE := harfbuzz-14.5.0.tar.xz
ZEDBSD_EXT_harfbuzz_URL := https://github.com/harfbuzz/harfbuzz/releases/download/14.5.0/harfbuzz-14.5.0.tar.xz
ZEDBSD_EXT_harfbuzz_SIZE := 20262956
ZEDBSD_EXT_harfbuzz_SHA256 := b7132e148358a45185c9feafd049dbaf243649d3c44414b3534d9c95d18592b9
ZEDBSD_EXT_harfbuzz_PATCH_LEVEL := zedbsd0
ZEDBSD_EXT_harfbuzz_PATCHES :=

$(eval $(call ZEDBSD_EXTERNAL_SOURCE,harfbuzz))

# ---------------------------------------------------------------- build

# Meson, through the common rule.  HarfBuzz is C++ written to need nothing
# of the C++ library at run time, and upstream links it with the C linker
# (with_libstdcxx stays off), so no C++ runtime is recorded in what it needs.
# It still includes a few of the C++ library's headers (<new>, <atomic>,
# <type_traits> and the like), which come from the LLVM C++ runtime package
# (devel/libcxx); -nostdinc++ keeps the compiler from looking anywhere else.
#
# GLib and GObject are on because Pango uses harfbuzz-gobject; FreeType is on
# for hb-ft; harfbuzz-subset is required by GTK.  Off, and why:
#   cairo, chafa          Cairo is not built yet, and the utilities that use
#                         them are not built either
#   icu                   GLib supplies the Unicode functions
#   raster, vector, gpu   libraries new in 14.x that neither Pango nor GTK 4.18 uses
#   png, zlib             used only by the raster and vector libraries
#   tests, introspection, docs, utilities  as for GLib
ZEDBSD_HARFBUZZ_VIEW := $(ZEDBSD_EXTERNAL_WORKROOT)/harfbuzz/view
ZEDBSD_EXT_harfbuzz_MAKEFILE := $(ZEDBSD_HARFBUZZ_MAKEFILE)
ZEDBSD_EXT_harfbuzz_DEPENDS := glib freetype libcxx
ZEDBSD_EXT_harfbuzz_HOST_TOOLS :=
ZEDBSD_EXT_harfbuzz_MESON_OPTIONS := \
	-Dglib=enabled -Dgobject=enabled -Dfreetype=enabled -Dsubset=enabled \
	-Dcairo=disabled -Dchafa=disabled -Dicu=disabled \
	-Draster=disabled -Dvector=disabled -Dgpu=disabled -Dgpu_demo=disabled \
	-Dpng=disabled -Dzlib=disabled \
	-Dtests=disabled -Dintrospection=disabled -Ddocs=disabled \
	-Dutilities=disabled -Dbenchmark=disabled \
	'-Dcpp_args=-nostdinc++ -isystem $(ZEDBSD_HARFBUZZ_VIEW)/usr/include/c++/v1'

$(eval $(call ZEDBSD_EXTERNAL_MESON,harfbuzz))

ZEDBSD_HARFBUZZ_STAGED := $(ZEDBSD_EXT_harfbuzz_STAGEDIR)/.zedbsd-staged
ZEDBSD_HARFBUZZ_CHECKED := $(ZEDBSD_EXT_harfbuzz_STAGEDIR)/.zedbsd-checked
ZEDBSD_HARFBUZZ_LIBDIR := $(ZEDBSD_EXT_harfbuzz_STAGEDIR)/usr/lib
ZEDBSD_HARFBUZZ_LICENSE := $(ZEDBSD_EXT_harfbuzz_SRCDIR)/COPYING

# The libraries, each under its SONAME, with the libraries it needs.  The
# list is the dependency contract: a change in what upstream links shows up
# here rather than on the target, and a C++ runtime appearing in it would be
# caught.  The file name carries upstream's libtool-style version
# (60000 + 100 * major + 10 * minor + micro).
ZEDBSD_HARFBUZZ_LIBRARIES := harfbuzz harfbuzz-gobject harfbuzz-subset
ZEDBSD_HARFBUZZ_SONAME_harfbuzz := libharfbuzz.so.0
ZEDBSD_HARFBUZZ_NEEDED_harfbuzz := libfreetype.so.6 libglib-2.0.so.0 libc.so
ZEDBSD_HARFBUZZ_SONAME_harfbuzz-gobject := libharfbuzz-gobject.so.0
# harfbuzz-gobject calls nothing in the C library itself, so --as-needed
# leaves libc.so out of it.
ZEDBSD_HARFBUZZ_NEEDED_harfbuzz-gobject := libharfbuzz.so.0 libglib-2.0.so.0 libgobject-2.0.so.0
ZEDBSD_HARFBUZZ_SONAME_harfbuzz-subset := libharfbuzz-subset.so.0
ZEDBSD_HARFBUZZ_NEEDED_harfbuzz-subset := libharfbuzz.so.0 libc.so
ZEDBSD_HARFBUZZ_REALNAME = $(ZEDBSD_HARFBUZZ_LIBDIR)/$(ZEDBSD_HARFBUZZ_SONAME_$(1)).61450.0

# Checks every library against its contract once the stage exists.
$(ZEDBSD_HARFBUZZ_CHECKED): $(ZEDBSD_HARFBUZZ_STAGED)
	@set -e; $(foreach library,$(ZEDBSD_HARFBUZZ_LIBRARIES),\
		$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role shared-library \
		--soname $(ZEDBSD_HARFBUZZ_SONAME_$(library)) \
		$(foreach needed,$(ZEDBSD_HARFBUZZ_NEEDED_$(library)),--needed $(needed)) \
		'$(call ZEDBSD_HARFBUZZ_REALNAME,$(library))';)
	@touch '$@'

$(ZEDBSD_HARFBUZZ_LICENSE): $(ZEDBSD_EXT_harfbuzz_SRCSTAMP)
	@test -f '$@'

.PHONY: harfbuzz
harfbuzz: $(ZEDBSD_HARFBUZZ_CHECKED)
	@:

# ---------------------------------------------------------------- packaging

# Each library goes in under its SONAME, which is the name the loader asks
# for.  The headers and the .pc files are the development files below.  No
# C++ runtime is listed, because the libraries do not need one.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,harfbuzz,HarfBuzz text shaping library,*,n,package,,y,\
	packages/libs,libs/harfbuzz,libs/glib libs/freetype))

ZEDBSD_PACKAGE_INPUTS += $(if $(filter harfbuzz,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_HARFBUZZ_CHECKED) $(ZEDBSD_HARFBUZZ_LICENSE))
ZEDBSD_PACKAGE_FILES += $(if $(filter harfbuzz,$(ZEDBSD_USER_PROGRAMS)),\
	$(foreach library,$(ZEDBSD_HARFBUZZ_LIBRARIES),\
		--file /usr/lib/$(ZEDBSD_HARFBUZZ_SONAME_$(library))=$(call ZEDBSD_HARFBUZZ_REALNAME,$(library))) \
	--file /usr/share/licenses/harfbuzz/COPYING=$(ZEDBSD_HARFBUZZ_LICENSE))

# The development files: the headers, the .pc files and the unversioned names
# of the libraries (external.mk).
$(eval $(call ZEDBSD_EXTERNAL_DEVELOPMENT,harfbuzz,$(ZEDBSD_EXT_harfbuzz_STAGEDIR),\
	$(addprefix lib/pkgconfig/,harfbuzz.pc harfbuzz-gobject.pc harfbuzz-subset.pc),\
	include/harfbuzz,\
	$(foreach library,$(ZEDBSD_HARFBUZZ_LIBRARIES),$(ZEDBSD_HARFBUZZ_SONAME_$(library)))))
