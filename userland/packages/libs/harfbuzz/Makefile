# zedBSD userland package: HarfBuzz (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# HarfBuzz is MIT-licensed ("Old MIT") and stays outside the source tree:
# only this Makefile and the recorded archive identity live here.  It turns
# text into positioned glyphs for Pango and the GTK port (WS115), and later
# for Qt (WS116).  Provenance is in plan/ws034/package-inventory.md and
# plan/ws115/port-contract.md.
HARFBUZZ_MAKEFILE := $(lastword $(MAKEFILE_LIST))
HARFBUZZ_DIR := $(patsubst %/,%,$(dir $(HARFBUZZ_MAKEFILE)))
include $(HARFBUZZ_DIR)/../../../base/package.mk
include $(HARFBUZZ_DIR)/../../external.mk

# 14.5.0 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; they match the digest GitHub gives the release
# asset (WS034 p001).
EXT_harfbuzz_VERSION := 14.5.0
EXT_harfbuzz_ROOT := harfbuzz-14.5.0
EXT_harfbuzz_ARCHIVE := harfbuzz-14.5.0.tar.xz
EXT_harfbuzz_URL := https://github.com/harfbuzz/harfbuzz/releases/download/14.5.0/harfbuzz-14.5.0.tar.xz
EXT_harfbuzz_SIZE := 20262956
EXT_harfbuzz_SHA256 := b7132e148358a45185c9feafd049dbaf243649d3c44414b3534d9c95d18592b9
EXT_harfbuzz_PATCH_LEVEL := zedbsd0
EXT_harfbuzz_PATCHES :=

$(eval $(call EXTERNAL_SOURCE,harfbuzz))

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
HARFBUZZ_VIEW := $(EXTERNAL_WORKROOT)/harfbuzz/view
EXT_harfbuzz_MAKEFILE := $(HARFBUZZ_MAKEFILE)
EXT_harfbuzz_DEPENDS := glib freetype libcxx
EXT_harfbuzz_HOST_TOOLS :=
EXT_harfbuzz_MESON_OPTIONS := \
	-Dglib=enabled -Dgobject=enabled -Dfreetype=enabled -Dsubset=enabled \
	-Dcairo=disabled -Dchafa=disabled -Dicu=disabled \
	-Draster=disabled -Dvector=disabled -Dgpu=disabled -Dgpu_demo=disabled \
	-Dpng=disabled -Dzlib=disabled \
	-Dtests=disabled -Dintrospection=disabled -Ddocs=disabled \
	-Dutilities=disabled -Dbenchmark=disabled \
	'-Dcpp_args=-nostdinc++ -isystem $(HARFBUZZ_VIEW)/usr/include/c++/v1'

$(eval $(call EXTERNAL_MESON,harfbuzz))

HARFBUZZ_STAGED := $(EXT_harfbuzz_STAGEDIR)/.zedbsd-staged
HARFBUZZ_CHECKED := $(EXT_harfbuzz_STAGEDIR)/.zedbsd-checked
HARFBUZZ_LIBDIR := $(EXT_harfbuzz_STAGEDIR)/usr/lib
HARFBUZZ_LICENSE := $(EXT_harfbuzz_SRCDIR)/COPYING

# The libraries, each under its SONAME, with the libraries it needs.  The
# list is the dependency contract: a change in what upstream links shows up
# here rather than on the target, and a C++ runtime appearing in it would be
# caught.  The file name carries upstream's libtool-style version
# (60000 + 100 * major + 10 * minor + micro).
HARFBUZZ_LIBRARIES := harfbuzz harfbuzz-gobject harfbuzz-subset
HARFBUZZ_SONAME_harfbuzz := libharfbuzz.so.0
HARFBUZZ_NEEDED_harfbuzz := libfreetype.so.6 libglib-2.0.so.0 libc.so
HARFBUZZ_SONAME_harfbuzz-gobject := libharfbuzz-gobject.so.0
# harfbuzz-gobject calls nothing in the C library itself, so --as-needed
# leaves libc.so out of it.
HARFBUZZ_NEEDED_harfbuzz-gobject := libharfbuzz.so.0 libglib-2.0.so.0 libgobject-2.0.so.0
HARFBUZZ_SONAME_harfbuzz-subset := libharfbuzz-subset.so.0
HARFBUZZ_NEEDED_harfbuzz-subset := libharfbuzz.so.0 libc.so
HARFBUZZ_REALNAME = $(HARFBUZZ_LIBDIR)/$(HARFBUZZ_SONAME_$(1)).61450.0

# Checks every library against its contract once the stage exists.
$(HARFBUZZ_CHECKED): $(HARFBUZZ_STAGED)
	@set -e; $(foreach library,$(HARFBUZZ_LIBRARIES),\
		$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname $(HARFBUZZ_SONAME_$(library)) \
		$(foreach needed,$(HARFBUZZ_NEEDED_$(library)),--needed $(needed)) \
		'$(call HARFBUZZ_REALNAME,$(library))';)
	@touch '$@'

$(HARFBUZZ_LICENSE): $(EXT_harfbuzz_SRCSTAMP)
	@test -f '$@'

.PHONY: harfbuzz
harfbuzz: $(HARFBUZZ_CHECKED)
	@:

# ---------------------------------------------------------------- packaging

# Each library goes in under its SONAME, which is the name the loader asks
# for.  The headers and the .pc files are the development files below.  No
# C++ runtime is listed, because the libraries do not need one.
$(eval $(call USERLAND_PACKAGE,harfbuzz,HarfBuzz text shaping library,*,n,package,,y,\
	packages/libs,libs/harfbuzz,libs/glib libs/freetype))

PACKAGE_INPUTS += $(if $(filter harfbuzz,$(USER_PROGRAMS)),\
	$(HARFBUZZ_CHECKED) $(HARFBUZZ_LICENSE))
PACKAGE_FILES += $(if $(filter harfbuzz,$(USER_PROGRAMS)),\
	$(foreach library,$(HARFBUZZ_LIBRARIES),\
		--file /usr/lib/$(HARFBUZZ_SONAME_$(library))=$(call HARFBUZZ_REALNAME,$(library))) \
	--file /usr/share/licenses/harfbuzz/COPYING=$(HARFBUZZ_LICENSE))

# The development files: the headers, the .pc files and the unversioned names
# of the libraries (external.mk).
$(eval $(call EXTERNAL_DEVELOPMENT,harfbuzz,$(EXT_harfbuzz_STAGEDIR),\
	$(addprefix lib/pkgconfig/,harfbuzz.pc harfbuzz-gobject.pc harfbuzz-subset.pc),\
	include/harfbuzz,\
	$(foreach library,$(HARFBUZZ_LIBRARIES),$(HARFBUZZ_SONAME_$(library)))))
