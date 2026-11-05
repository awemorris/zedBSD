# zedBSD userland package: pcre2 (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# PCRE2 is BSD-3-Clause WITH PCRE2-exception and stays outside the source
# tree: only this Makefile and the recorded archive identity live here.
# GLib's regular expressions (GRegex) are built on its 8-bit library.
# Provenance is in plan/ws034/package-inventory.md and
# plan/ws115/port-contract.md.
ZEDBSD_PCRE2_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_PCRE2_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_PCRE2_MAKEFILE)))
include $(ZEDBSD_PCRE2_DIR)/../../../base/package.mk
include $(ZEDBSD_PCRE2_DIR)/../../external.mk

# 10.48 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; the archive's signature was checked (WS034 p001).
ZEDBSD_EXT_pcre2_VERSION := 10.48
ZEDBSD_EXT_pcre2_ROOT := pcre2-10.48
ZEDBSD_EXT_pcre2_ARCHIVE := pcre2-10.48.tar.bz2
ZEDBSD_EXT_pcre2_URL := https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.48/pcre2-10.48.tar.bz2
ZEDBSD_EXT_pcre2_SIZE := 2213024
ZEDBSD_EXT_pcre2_SHA256 := b6c68fdf6f3ac31388b50aa89ff0fc49c00c987c16e7b5146491d12003f2c8ed
ZEDBSD_EXT_pcre2_PATCH_LEVEL := zedbsd0
ZEDBSD_EXT_pcre2_PATCHES :=
ZEDBSD_EXT_pcre2_DEPENDS :=

$(eval $(call ZEDBSD_EXTERNAL_SOURCE,pcre2))

# ---------------------------------------------------------------- build

# libtool would not make a shared library for an unknown system, so CMake is
# used (WS034 p001).  The SONAME is libpcre2-8.so.0.
ZEDBSD_PCRE2_SONAME := libpcre2-8.so.0
ZEDBSD_PCRE2_CONFIGURED := $(ZEDBSD_EXT_pcre2_BUILDDIR)/.zedbsd-configured
ZEDBSD_PCRE2_STAGED := $(ZEDBSD_EXT_pcre2_STAGEDIR)/.zedbsd-staged
ZEDBSD_PCRE2_LIBRARY := $(ZEDBSD_EXT_pcre2_STAGEDIR)/usr/lib/libpcre2-8.so.0.16.0
ZEDBSD_PCRE2_HEADERS := $(ZEDBSD_EXT_pcre2_STAGEDIR)/usr/include/pcre2.h
ZEDBSD_PCRE2_PC := $(ZEDBSD_EXT_pcre2_STAGEDIR)/usr/lib/pkgconfig/libpcre2-8.pc
ZEDBSD_PCRE2_LICENSE := $(ZEDBSD_EXT_pcre2_SRCDIR)/LICENCE.md

# Only the 8-bit library, without symbol versioning (the loader does not read
# it) and without the JIT, which would need memory that is both writable and
# executable.  pcre2grep, pcre2test and the tests are not built.
ZEDBSD_PCRE2_CMAKE_OPTIONS := \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DCMAKE_INSTALL_LIBDIR=lib \
	-DBUILD_SHARED_LIBS=ON \
	-DBUILD_STATIC_LIBS=OFF \
	-DPCRE2_BUILD_PCRE2_8=ON \
	-DPCRE2_BUILD_PCRE2_16=OFF \
	-DPCRE2_BUILD_PCRE2_32=OFF \
	-DPCRE2_SUPPORT_JIT=OFF \
	-DPCRE2_SUPPORT_UNICODE=ON \
	-DPCRE2_SYMVERS=OFF \
	-DPCRE2_BUILD_PCRE2GREP=OFF \
	-DPCRE2_BUILD_TESTS=OFF \
	-DPCRE2_SHOW_REPORT=OFF

# The options are read when the tree is configured, so this file is an input.
$(ZEDBSD_PCRE2_CONFIGURED): $(ZEDBSD_EXT_pcre2_SRCSTAMP) \
	$(ZEDBSD_EXTERNAL_CROSS_STAMP) $(ZEDBSD_PCRE2_MAKEFILE)
	@rm -rf '$(ZEDBSD_EXT_pcre2_BUILDDIR)'
	@mkdir -p '$(ZEDBSD_EXT_pcre2_BUILDDIR)'
	cmake -S '$(ZEDBSD_EXT_pcre2_SRCDIR)' -B '$(ZEDBSD_EXT_pcre2_BUILDDIR)' \
		-G Ninja \
		--toolchain '$(ZEDBSD_EXTERNAL_CROSS_DIR)/zedbsd.cmake' \
		$(ZEDBSD_PCRE2_CMAKE_OPTIONS)
	@touch '$@'

$(ZEDBSD_PCRE2_STAGED): $(ZEDBSD_PCRE2_CONFIGURED)
	@rm -rf '$(ZEDBSD_EXT_pcre2_STAGEDIR)'
	cmake --build '$(ZEDBSD_EXT_pcre2_BUILDDIR)' --parallel $(ZEDBSD_BUILD_JOBS)
	cmake --install '$(ZEDBSD_EXT_pcre2_BUILDDIR)' \
		--prefix '$(ZEDBSD_EXT_pcre2_STAGEDIR)/usr'
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role shared-library \
		--soname $(ZEDBSD_PCRE2_SONAME) --needed libc.so '$(ZEDBSD_PCRE2_LIBRARY)'
	@touch '$@'

$(ZEDBSD_PCRE2_LIBRARY) $(ZEDBSD_PCRE2_HEADERS) $(ZEDBSD_PCRE2_PC): \
	$(ZEDBSD_PCRE2_STAGED)
	@test -f '$@'

$(ZEDBSD_PCRE2_LICENSE): $(ZEDBSD_EXT_pcre2_SRCSTAMP)
	@test -f '$@'

.PHONY: pcre2
pcre2: $(ZEDBSD_PCRE2_STAGED)
	@:

# ---------------------------------------------------------------- packaging

# The library goes in under its SONAME, which is the name the loader asks
# for; libpcre2-8.so is the link-time name a compiler on the target looks for.
$(eval $(call USERLAND_PACKAGE,pcre2,PCRE2 regular expression library,*,n,package,,y,\
	packages/libs,libs/pcre2,))

ZEDBSD_PACKAGE_INPUTS += $(if $(filter pcre2,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_PCRE2_LIBRARY) $(ZEDBSD_PCRE2_HEADERS) $(ZEDBSD_PCRE2_PC) \
	$(ZEDBSD_PCRE2_LICENSE))
ZEDBSD_PACKAGE_FILES += $(if $(filter pcre2,$(ZEDBSD_USER_PROGRAMS)),\
	--file /usr/lib/$(ZEDBSD_PCRE2_SONAME)=$(ZEDBSD_PCRE2_LIBRARY) \
	--file /usr/include/pcre2.h=$(ZEDBSD_PCRE2_HEADERS) \
	--file /usr/lib/pkgconfig/libpcre2-8.pc=$(ZEDBSD_PCRE2_PC) \
	--file /usr/share/licenses/pcre2/LICENCE.md=$(ZEDBSD_PCRE2_LICENSE))
ZEDBSD_PACKAGE_LINKS += $(if $(filter pcre2,$(ZEDBSD_USER_PROGRAMS)),\
	/usr/lib/libpcre2-8.so=$(ZEDBSD_PCRE2_SONAME))
