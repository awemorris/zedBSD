# zedBSD userland package: libpng (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# libpng is under the PNG Reference Library License version 2 (Libpng-2.0)
# and stays outside the source tree: only this Makefile and the recorded
# archive identity live here.  FreeType reads the PNG glyphs of colour emoji
# fonts through it, and the GTK port (WS115) decodes PNG images with it.
# Provenance is in plan/ws034/package-inventory.md and
# plan/ws115/port-contract.md.
LIBPNG_MAKEFILE := $(lastword $(MAKEFILE_LIST))
LIBPNG_DIR := $(patsubst %/,%,$(dir $(LIBPNG_MAKEFILE)))
include $(LIBPNG_DIR)/../../../base/package.mk
include $(LIBPNG_DIR)/../../external.mk

# 1.6.58 was the latest release when recorded (1.7 is a beta).  Size and
# digest are the upstream bytes as measured; the tree was compared with the
# v1.6.58 git tag (WS034 p001).
EXT_libpng_VERSION := 1.6.58
EXT_libpng_ROOT := libpng-1.6.58
EXT_libpng_ARCHIVE := libpng-1.6.58.tar.xz
EXT_libpng_URL := https://download.sourceforge.net/libpng/libpng-1.6.58.tar.xz
EXT_libpng_SIZE := 1070096
EXT_libpng_SHA256 := 28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775
EXT_libpng_PATCH_LEVEL := zedbsd0
EXT_libpng_PATCHES :=
EXT_libpng_DEPENDS := zlib

$(eval $(call EXTERNAL_SOURCE,libpng))

# ---------------------------------------------------------------- build

# libtool would not make a shared library for an unknown system, so CMake is
# used, as for pcre2.  The SONAME is libpng16.so.16.
LIBPNG_SONAME := libpng16.so.16
LIBPNG_CONFIGURED := $(EXT_libpng_BUILDDIR)/.zedbsd-configured
LIBPNG_STAGED := $(EXT_libpng_STAGEDIR)/.zedbsd-staged
LIBPNG_LIBRARY := $(EXT_libpng_STAGEDIR)/usr/lib/libpng16.so.16.58.0
LIBPNG_HEADER_NAMES := png.h pngconf.h pnglibconf.h
LIBPNG_HEADERS := $(foreach header,$(LIBPNG_HEADER_NAMES),\
	$(EXT_libpng_STAGEDIR)/usr/include/libpng16/$(header))
LIBPNG_PC := $(EXT_libpng_STAGEDIR)/usr/lib/pkgconfig/libpng16.pc
LIBPNG_LICENSE := $(EXT_libpng_SRCDIR)/LICENSE

# The zlib it links is the staged one.  The cross toolchain file confines
# CMake's searches to the sysroot, where zlib is not, so the library and its
# headers are named directly.
LIBPNG_ZLIB_STAGE := $(EXTERNAL_WORKROOT)/zlib/stage

# Only the shared library, without the version script (the loader does not
# read symbol versions).  The tools, the tests and the static library are not
# built.  The SSE2 filters stay, as on any x86-64.
LIBPNG_CMAKE_OPTIONS := \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DCMAKE_INSTALL_LIBDIR=lib \
	-DPNG_SHARED=ON \
	-DPNG_STATIC=OFF \
	-DPNG_FRAMEWORK=OFF \
	-DPNG_TESTS=OFF \
	-DPNG_TOOLS=OFF \
	-Dld-version-script=OFF \
	-DZLIB_LIBRARY='$(LIBPNG_ZLIB_STAGE)/usr/lib/libz.so' \
	-DZLIB_INCLUDE_DIR='$(LIBPNG_ZLIB_STAGE)/usr/include'

# The options are read when the tree is configured, so this file is an input.
$(LIBPNG_CONFIGURED): $(EXT_libpng_SRCSTAMP) \
	$(EXTERNAL_CROSS_STAMP) $(LIBPNG_MAKEFILE) \
	$(LIBPNG_ZLIB_STAGE)/.zedbsd-staged
	@rm -rf '$(EXT_libpng_BUILDDIR)'
	@mkdir -p '$(EXT_libpng_BUILDDIR)'
	cmake -S '$(EXT_libpng_SRCDIR)' -B '$(EXT_libpng_BUILDDIR)' \
		-G Ninja \
		--toolchain '$(EXTERNAL_CROSS_DIR)/zedbsd.cmake' \
		$(LIBPNG_CMAKE_OPTIONS)
	@touch '$@'

$(LIBPNG_STAGED): $(LIBPNG_CONFIGURED)
	@rm -rf '$(EXT_libpng_STAGEDIR)'
	cmake --build '$(EXT_libpng_BUILDDIR)' --parallel $(BUILD_JOBS)
	cmake --install '$(EXT_libpng_BUILDDIR)' \
		--prefix '$(EXT_libpng_STAGEDIR)/usr'
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname $(LIBPNG_SONAME) --needed libz.so.1 --needed libc.so \
		'$(LIBPNG_LIBRARY)'
	@touch '$@'

$(LIBPNG_LIBRARY) $(LIBPNG_HEADERS) $(LIBPNG_PC): \
	$(LIBPNG_STAGED)
	@test -f '$@'

$(LIBPNG_LICENSE): $(EXT_libpng_SRCSTAMP)
	@test -f '$@'

.PHONY: libpng
libpng: $(LIBPNG_STAGED)
	@:

# ---------------------------------------------------------------- packaging

# The library goes in under its SONAME, which is the name the loader asks
# for; libpng16.so and libpng.so are the link-time names a compiler on the
# target looks for.
$(eval $(call USERLAND_PACKAGE,libpng,PNG reference library,*,n,package,,y,\
	packages/libs,libs/libpng,libs/zlib))

PACKAGE_INPUTS += $(if $(filter libpng,$(USER_PROGRAMS)),\
	$(LIBPNG_LIBRARY) $(LIBPNG_HEADERS) $(LIBPNG_PC) \
	$(LIBPNG_LICENSE))
PACKAGE_FILES += $(if $(filter libpng,$(USER_PROGRAMS)),\
	--file /usr/lib/$(LIBPNG_SONAME)=$(LIBPNG_LIBRARY) \
	$(foreach header,$(LIBPNG_HEADER_NAMES),\
		--file /usr/include/libpng16/$(header)=$(EXT_libpng_STAGEDIR)/usr/include/libpng16/$(header)) \
	--file /usr/lib/pkgconfig/libpng16.pc=$(LIBPNG_PC) \
	--file /usr/share/licenses/libpng/LICENSE=$(LIBPNG_LICENSE))
PACKAGE_LINKS += $(if $(filter libpng,$(USER_PROGRAMS)),\
	/usr/lib/libpng16.so=$(LIBPNG_SONAME) \
	/usr/lib/libpng.so=libpng16.so)

# libpng.pc, the name without the version some builds ask pkg-config for.
$(eval $(call EXTERNAL_DEVELOPMENT,libpng,$(EXT_libpng_STAGEDIR),\
	lib/pkgconfig/libpng.pc,\
	,\
	))
