# zedBSD userland package: libtiff (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# libtiff is under its own permissive licence (SPDX "libtiff"; the text is
# LICENSE.md) and stays outside the source tree: only this Makefile and the
# recorded archive identity live here.  GTK 4 requires it (WS115).
# Provenance is in plan/ws034/package-inventory.md and
# plan/ws115/port-contract.md.
LIBTIFF_MAKEFILE := $(lastword $(MAKEFILE_LIST))
LIBTIFF_DIR := $(patsubst %/,%,$(dir $(LIBTIFF_MAKEFILE)))
include $(LIBTIFF_DIR)/../../../base/package.mk
include $(LIBTIFF_DIR)/../../external.mk

# 4.7.2 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; the archive's signature was checked
# (WS034 p001).
EXT_libtiff_VERSION := 4.7.2
EXT_libtiff_ROOT := tiff-4.7.2
EXT_libtiff_ARCHIVE := tiff-4.7.2.tar.xz
EXT_libtiff_URL := https://download.osgeo.org/libtiff/tiff-4.7.2.tar.xz
EXT_libtiff_SIZE := 2409652
EXT_libtiff_SHA256 := 4996f0c4f93094719b1ca5c6279b20e588773ba8a247533e486416fb662ddb88
EXT_libtiff_PATCH_LEVEL := zedbsd0
EXT_libtiff_PATCHES :=
EXT_libtiff_DEPENDS := zlib libjpeg-turbo

$(eval $(call EXTERNAL_SOURCE,libtiff))

# ---------------------------------------------------------------- build

# CMake, as for libpng.  The SONAME is libtiff.so.6.
LIBTIFF_SONAME := libtiff.so.6
LIBTIFF_CONFIGURED := $(EXT_libtiff_BUILDDIR)/.zedbsd-configured
LIBTIFF_STAGED := $(EXT_libtiff_STAGEDIR)/.zedbsd-staged
LIBTIFF_LIBRARY := $(EXT_libtiff_STAGEDIR)/usr/lib/libtiff.so.6.3.0
LIBTIFF_LICENSE := $(EXT_libtiff_SRCDIR)/LICENSE.md

# zlib and libjpeg are the staged ones.  The cross toolchain file confines
# CMake's searches to the sysroot, where neither is, so each library and its
# headers are named directly.
LIBTIFF_ZLIB_STAGE := $(EXTERNAL_WORKROOT)/zlib/stage
LIBTIFF_JPEG_STAGE := $(EXTERNAL_WORKROOT)/libjpeg-turbo/stage

# Only the shared C library, with Deflate (zlib) and JPEG compression and the
# built-in codecs.  What is off, and why:
#   tiff-cxx            the C++ stream library, which GTK does not use and
#                       which would need the C++ runtime on the target
#   tools, tests, contrib, docs, static library
#   libdeflate, jbig, lerc, lzma, zstd, webp  libraries zedBSD does not have
#   ld-version-script   the loader does not read symbol versions
LIBTIFF_CMAKE_OPTIONS := \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DCMAKE_INSTALL_LIBDIR=lib \
	-DBUILD_SHARED_LIBS=ON \
	-Dtiff-static=OFF \
	-Dtiff-cxx=OFF \
	-Dtiff-tools=OFF \
	-Dtiff-tests=OFF \
	-Dtiff-contrib=OFF \
	-Dtiff-docs=OFF \
	-Dld-version-script=OFF \
	-Dzlib=ON \
	-Djpeg=ON \
	-Dlibdeflate=OFF \
	-Djbig=OFF \
	-Dlerc=OFF \
	-Dlzma=OFF \
	-Dzstd=OFF \
	-Dwebp=OFF \
	-DZLIB_LIBRARY='$(LIBTIFF_ZLIB_STAGE)/usr/lib/libz.so' \
	-DZLIB_INCLUDE_DIR='$(LIBTIFF_ZLIB_STAGE)/usr/include' \
	-DJPEG_LIBRARY='$(LIBTIFF_JPEG_STAGE)/usr/lib/libjpeg.so' \
	-DJPEG_LIBRARY_RELEASE='$(LIBTIFF_JPEG_STAGE)/usr/lib/libjpeg.so' \
	-DJPEG_INCLUDE_DIR='$(LIBTIFF_JPEG_STAGE)/usr/include'

# The options are read when the tree is configured, so this file is an input.
$(LIBTIFF_CONFIGURED): $(EXT_libtiff_SRCSTAMP) \
	$(EXTERNAL_CROSS_STAMP) $(LIBTIFF_MAKEFILE) \
	$(LIBTIFF_ZLIB_STAGE)/.zedbsd-staged \
	$(LIBTIFF_JPEG_STAGE)/.zedbsd-staged
	@rm -rf '$(EXT_libtiff_BUILDDIR)'
	@mkdir -p '$(EXT_libtiff_BUILDDIR)'
	cmake -S '$(EXT_libtiff_SRCDIR)' -B '$(EXT_libtiff_BUILDDIR)' \
		-G Ninja \
		--toolchain '$(EXTERNAL_CROSS_DIR)/zedbsd.cmake' \
		$(LIBTIFF_CMAKE_OPTIONS)
	@touch '$@'

$(LIBTIFF_STAGED): $(LIBTIFF_CONFIGURED)
	@rm -rf '$(EXT_libtiff_STAGEDIR)'
	cmake --build '$(EXT_libtiff_BUILDDIR)' --parallel $(BUILD_JOBS)
	cmake --install '$(EXT_libtiff_BUILDDIR)' \
		--prefix '$(EXT_libtiff_STAGEDIR)/usr'
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname $(LIBTIFF_SONAME) \
		--needed libz.so.1 --needed libjpeg.so.62 --needed libc.so \
		'$(LIBTIFF_LIBRARY)'
	@touch '$@'

$(LIBTIFF_LIBRARY): $(LIBTIFF_STAGED)
	@test -f '$@'

$(LIBTIFF_LICENSE): $(EXT_libtiff_SRCSTAMP)
	@test -f '$@'

.PHONY: libtiff
libtiff: $(LIBTIFF_STAGED)
	@:

# ---------------------------------------------------------------- packaging

# The library under its SONAME.  The headers and the .pc file are the
# development files below.
$(eval $(call USERLAND_PACKAGE,libtiff,TIFF image library,*,n,package,,y,\
	packages/libs,libs/libtiff,libs/zlib libs/libjpeg-turbo))

PACKAGE_INPUTS += $(if $(filter libtiff,$(USER_PROGRAMS)),\
	$(LIBTIFF_LIBRARY) $(LIBTIFF_LICENSE))
PACKAGE_FILES += $(if $(filter libtiff,$(USER_PROGRAMS)),\
	--file /usr/lib/$(LIBTIFF_SONAME)=$(LIBTIFF_LIBRARY) \
	--file /usr/share/licenses/libtiff/LICENSE.md=$(LIBTIFF_LICENSE))

# The development files: the headers, the .pc files and the unversioned names
# of the libraries (external.mk).
$(eval $(call EXTERNAL_DEVELOPMENT,libtiff,$(EXT_libtiff_STAGEDIR),\
	$(addprefix include/,tiff.h tiffconf.h tiffio.h tiffvers.h) lib/pkgconfig/libtiff-4.pc,\
	,\
	$(LIBTIFF_SONAME)))
