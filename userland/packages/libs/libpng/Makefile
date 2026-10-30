# zedBSD userland package: libpng (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# libpng is under the PNG Reference Library License version 2 (Libpng-2.0)
# and stays outside the source tree: only this Makefile and the recorded
# archive identity live here.  FreeType reads the PNG glyphs of colour emoji
# fonts through it, and the GTK port (WS115) decodes PNG images with it.
# Provenance is in plan/ws034/package-inventory.md and
# plan/ws115/port-contract.md.
ZEDBSD_LIBPNG_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_LIBPNG_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_LIBPNG_MAKEFILE)))
include $(ZEDBSD_LIBPNG_DIR)/../../../base/package.mk
include $(ZEDBSD_LIBPNG_DIR)/../../external.mk

# 1.6.58 was the latest release when recorded (1.7 is a beta).  Size and
# digest are the upstream bytes as measured; the tree was compared with the
# v1.6.58 git tag (WS034 p001).
ZEDBSD_EXT_libpng_VERSION := 1.6.58
ZEDBSD_EXT_libpng_ROOT := libpng-1.6.58
ZEDBSD_EXT_libpng_ARCHIVE := libpng-1.6.58.tar.xz
ZEDBSD_EXT_libpng_URL := https://download.sourceforge.net/libpng/libpng-1.6.58.tar.xz
ZEDBSD_EXT_libpng_SIZE := 1070096
ZEDBSD_EXT_libpng_SHA256 := 28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775
ZEDBSD_EXT_libpng_PATCH_LEVEL := zedbsd0
ZEDBSD_EXT_libpng_PATCHES :=
ZEDBSD_EXT_libpng_DEPENDS := zlib

$(eval $(call ZEDBSD_EXTERNAL_SOURCE,libpng))

# ---------------------------------------------------------------- build

# libtool would not make a shared library for an unknown system, so CMake is
# used, as for pcre2.  The SONAME is libpng16.so.16.
ZEDBSD_LIBPNG_SONAME := libpng16.so.16
ZEDBSD_LIBPNG_CONFIGURED := $(ZEDBSD_EXT_libpng_BUILDDIR)/.zedbsd-configured
ZEDBSD_LIBPNG_STAGED := $(ZEDBSD_EXT_libpng_STAGEDIR)/.zedbsd-staged
ZEDBSD_LIBPNG_LIBRARY := $(ZEDBSD_EXT_libpng_STAGEDIR)/usr/lib/libpng16.so.16.58.0
ZEDBSD_LIBPNG_HEADER_NAMES := png.h pngconf.h pnglibconf.h
ZEDBSD_LIBPNG_HEADERS := $(foreach header,$(ZEDBSD_LIBPNG_HEADER_NAMES),\
	$(ZEDBSD_EXT_libpng_STAGEDIR)/usr/include/libpng16/$(header))
ZEDBSD_LIBPNG_PC := $(ZEDBSD_EXT_libpng_STAGEDIR)/usr/lib/pkgconfig/libpng16.pc
ZEDBSD_LIBPNG_LICENSE := $(ZEDBSD_EXT_libpng_SRCDIR)/LICENSE

# The zlib it links is the staged one.  The cross toolchain file confines
# CMake's searches to the sysroot, where zlib is not, so the library and its
# headers are named directly.
ZEDBSD_LIBPNG_ZLIB_STAGE := $(ZEDBSD_EXTERNAL_WORKROOT)/zlib/stage

# Only the shared library, without the version script (the loader does not
# read symbol versions).  The tools, the tests and the static library are not
# built.  The SSE2 filters stay, as on any x86-64.
ZEDBSD_LIBPNG_CMAKE_OPTIONS := \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DCMAKE_INSTALL_LIBDIR=lib \
	-DPNG_SHARED=ON \
	-DPNG_STATIC=OFF \
	-DPNG_FRAMEWORK=OFF \
	-DPNG_TESTS=OFF \
	-DPNG_TOOLS=OFF \
	-Dld-version-script=OFF \
	-DZLIB_LIBRARY='$(ZEDBSD_LIBPNG_ZLIB_STAGE)/usr/lib/libz.so' \
	-DZLIB_INCLUDE_DIR='$(ZEDBSD_LIBPNG_ZLIB_STAGE)/usr/include'

# The options are read when the tree is configured, so this file is an input.
$(ZEDBSD_LIBPNG_CONFIGURED): $(ZEDBSD_EXT_libpng_SRCSTAMP) \
	$(ZEDBSD_EXTERNAL_CROSS_STAMP) $(ZEDBSD_LIBPNG_MAKEFILE) \
	$(ZEDBSD_LIBPNG_ZLIB_STAGE)/.zedbsd-staged
	@rm -rf '$(ZEDBSD_EXT_libpng_BUILDDIR)'
	@mkdir -p '$(ZEDBSD_EXT_libpng_BUILDDIR)'
	cmake -S '$(ZEDBSD_EXT_libpng_SRCDIR)' -B '$(ZEDBSD_EXT_libpng_BUILDDIR)' \
		-G Ninja \
		--toolchain '$(ZEDBSD_EXTERNAL_CROSS_DIR)/zedbsd.cmake' \
		$(ZEDBSD_LIBPNG_CMAKE_OPTIONS)
	@touch '$@'

$(ZEDBSD_LIBPNG_STAGED): $(ZEDBSD_LIBPNG_CONFIGURED)
	@rm -rf '$(ZEDBSD_EXT_libpng_STAGEDIR)'
	cmake --build '$(ZEDBSD_EXT_libpng_BUILDDIR)' --parallel $(ZEDBSD_BUILD_JOBS)
	cmake --install '$(ZEDBSD_EXT_libpng_BUILDDIR)' \
		--prefix '$(ZEDBSD_EXT_libpng_STAGEDIR)/usr'
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role shared-library \
		--soname $(ZEDBSD_LIBPNG_SONAME) --needed libz.so.1 --needed libc.so \
		'$(ZEDBSD_LIBPNG_LIBRARY)'
	@touch '$@'

$(ZEDBSD_LIBPNG_LIBRARY) $(ZEDBSD_LIBPNG_HEADERS) $(ZEDBSD_LIBPNG_PC): \
	$(ZEDBSD_LIBPNG_STAGED)
	@test -f '$@'

$(ZEDBSD_LIBPNG_LICENSE): $(ZEDBSD_EXT_libpng_SRCSTAMP)
	@test -f '$@'

.PHONY: libpng
libpng: $(ZEDBSD_LIBPNG_STAGED)
	@:

# ---------------------------------------------------------------- packaging

# The library goes in under its SONAME, which is the name the loader asks
# for; libpng16.so and libpng.so are the link-time names a compiler on the
# target looks for.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libpng,PNG reference library,*,n,package,,y,\
	packages/libs,libs/libpng,libs/zlib))

ZEDBSD_PACKAGE_INPUTS += $(if $(filter libpng,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_LIBPNG_LIBRARY) $(ZEDBSD_LIBPNG_HEADERS) $(ZEDBSD_LIBPNG_PC) \
	$(ZEDBSD_LIBPNG_LICENSE))
ZEDBSD_PACKAGE_FILES += $(if $(filter libpng,$(ZEDBSD_USER_PROGRAMS)),\
	--file /usr/lib/$(ZEDBSD_LIBPNG_SONAME)=$(ZEDBSD_LIBPNG_LIBRARY) \
	$(foreach header,$(ZEDBSD_LIBPNG_HEADER_NAMES),\
		--file /usr/include/libpng16/$(header)=$(ZEDBSD_EXT_libpng_STAGEDIR)/usr/include/libpng16/$(header)) \
	--file /usr/lib/pkgconfig/libpng16.pc=$(ZEDBSD_LIBPNG_PC) \
	--file /usr/share/licenses/libpng/LICENSE=$(ZEDBSD_LIBPNG_LICENSE))
ZEDBSD_PACKAGE_LINKS += $(if $(filter libpng,$(ZEDBSD_USER_PROGRAMS)),\
	/usr/lib/libpng16.so=$(ZEDBSD_LIBPNG_SONAME) \
	/usr/lib/libpng.so=libpng16.so)

# libpng.pc, the name without the version some builds ask pkg-config for.
$(eval $(call ZEDBSD_EXTERNAL_DEVELOPMENT,libpng,$(ZEDBSD_EXT_libpng_STAGEDIR),\
	lib/pkgconfig/libpng.pc,\
	,\
	))
