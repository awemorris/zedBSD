# zedBSD userland package: libjpeg-turbo (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# libjpeg-turbo is IJG AND BSD-3-Clause AND Zlib (LICENSE.md lists which part
# is which) and stays outside the source tree: only this Makefile and the
# recorded archive identity live here.  It is the libjpeg that gdk-pixbuf and
# GTK decode JPEG images with (WS115).  Provenance is in
# plan/ws034/package-inventory.md and plan/ws115/port-contract.md.
ZEDBSD_LIBJPEG_TURBO_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_LIBJPEG_TURBO_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_LIBJPEG_TURBO_MAKEFILE)))
include $(ZEDBSD_LIBJPEG_TURBO_DIR)/../../../base/package.mk
include $(ZEDBSD_LIBJPEG_TURBO_DIR)/../../external.mk

# 3.2.0 was the latest stable release when recorded.  Size and digest are
# the upstream bytes as measured; the archive's signature was checked
# (WS034 p001).
ZEDBSD_EXT_libjpeg-turbo_VERSION := 3.2.0
ZEDBSD_EXT_libjpeg-turbo_ROOT := libjpeg-turbo-3.2.0
ZEDBSD_EXT_libjpeg-turbo_ARCHIVE := libjpeg-turbo-3.2.0.tar.gz
ZEDBSD_EXT_libjpeg-turbo_URL := https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.0/libjpeg-turbo-3.2.0.tar.gz
ZEDBSD_EXT_libjpeg-turbo_SIZE := 2537858
ZEDBSD_EXT_libjpeg-turbo_SHA256 := 6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e
ZEDBSD_EXT_libjpeg-turbo_PATCH_LEVEL := zedbsd0
ZEDBSD_EXT_libjpeg-turbo_PATCHES :=
ZEDBSD_EXT_libjpeg-turbo_DEPENDS :=

$(eval $(call ZEDBSD_EXTERNAL_SOURCE,libjpeg-turbo))

# ---------------------------------------------------------------- build

# CMake, as upstream builds it.  The SONAME is libjpeg.so.62 (the libjpeg 6b
# interface, which is what gdk-pixbuf and GTK use).
ZEDBSD_LIBJPEG_TURBO_SONAME := libjpeg.so.62
ZEDBSD_LIBJPEG_TURBO_CONFIGURED := $(ZEDBSD_EXT_libjpeg-turbo_BUILDDIR)/.zedbsd-configured
ZEDBSD_LIBJPEG_TURBO_STAGED := $(ZEDBSD_EXT_libjpeg-turbo_STAGEDIR)/.zedbsd-staged
ZEDBSD_LIBJPEG_TURBO_LIBRARY := $(ZEDBSD_EXT_libjpeg-turbo_STAGEDIR)/usr/lib/libjpeg.so.62.4.0
ZEDBSD_LIBJPEG_TURBO_LICENSE := $(ZEDBSD_EXT_libjpeg-turbo_SRCDIR)/LICENSE.md
ZEDBSD_LIBJPEG_TURBO_README_IJG := $(ZEDBSD_EXT_libjpeg-turbo_SRCDIR)/README.ijg

# Only the shared libjpeg.  What is off, and why:
#   static library, TurboJPEG API, tools, tests  nothing here uses them
#   HAVE_VERSION_SCRIPT  set false ahead of upstream's probe: the loader does
#                        not read symbol versions (as for pcre2 and libpng)
# The SIMD code (nasm, which the build machine has) stays: libjpeg-turbo
# chooses the AVX2 or SSE2 path when it runs.
ZEDBSD_LIBJPEG_TURBO_CMAKE_OPTIONS := \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DCMAKE_INSTALL_LIBDIR=lib \
	-DENABLE_SHARED=ON \
	-DENABLE_STATIC=OFF \
	-DWITH_TURBOJPEG=OFF \
	-DWITH_TOOLS=OFF \
	-DWITH_TESTS=OFF \
	-DWITH_SIMD=ON \
	-DREQUIRE_SIMD=ON \
	-DHAVE_VERSION_SCRIPT=FALSE

# The options are read when the tree is configured, so this file is an input.
$(ZEDBSD_LIBJPEG_TURBO_CONFIGURED): $(ZEDBSD_EXT_libjpeg-turbo_SRCSTAMP) \
	$(ZEDBSD_EXTERNAL_CROSS_STAMP) $(ZEDBSD_LIBJPEG_TURBO_MAKEFILE)
	@rm -rf '$(ZEDBSD_EXT_libjpeg-turbo_BUILDDIR)'
	@mkdir -p '$(ZEDBSD_EXT_libjpeg-turbo_BUILDDIR)'
	cmake -S '$(ZEDBSD_EXT_libjpeg-turbo_SRCDIR)' -B '$(ZEDBSD_EXT_libjpeg-turbo_BUILDDIR)' \
		-G Ninja \
		--toolchain '$(ZEDBSD_EXTERNAL_CROSS_DIR)/zedbsd.cmake' \
		$(ZEDBSD_LIBJPEG_TURBO_CMAKE_OPTIONS)
	@touch '$@'

$(ZEDBSD_LIBJPEG_TURBO_STAGED): $(ZEDBSD_LIBJPEG_TURBO_CONFIGURED)
	@rm -rf '$(ZEDBSD_EXT_libjpeg-turbo_STAGEDIR)'
	cmake --build '$(ZEDBSD_EXT_libjpeg-turbo_BUILDDIR)' --parallel $(ZEDBSD_BUILD_JOBS)
	cmake --install '$(ZEDBSD_EXT_libjpeg-turbo_BUILDDIR)' \
		--prefix '$(ZEDBSD_EXT_libjpeg-turbo_STAGEDIR)/usr'
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role shared-library \
		--soname $(ZEDBSD_LIBJPEG_TURBO_SONAME) --needed libc.so \
		'$(ZEDBSD_LIBJPEG_TURBO_LIBRARY)'
	@touch '$@'

$(ZEDBSD_LIBJPEG_TURBO_LIBRARY): $(ZEDBSD_LIBJPEG_TURBO_STAGED)
	@test -f '$@'

$(ZEDBSD_LIBJPEG_TURBO_LICENSE) $(ZEDBSD_LIBJPEG_TURBO_README_IJG): \
	$(ZEDBSD_EXT_libjpeg-turbo_SRCSTAMP)
	@test -f '$@'

.PHONY: libjpeg-turbo
libjpeg-turbo: $(ZEDBSD_LIBJPEG_TURBO_STAGED)
	@:

# ---------------------------------------------------------------- packaging

# The library under its SONAME, with LICENSE.md and the IJG notice it
# points to.  The headers and the .pc file are the development files below.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libjpeg-turbo,JPEG image codec,*,n,package,,y,\
	packages/libs,libs/libjpeg-turbo,))

ZEDBSD_PACKAGE_INPUTS += $(if $(filter libjpeg-turbo,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_LIBJPEG_TURBO_LIBRARY) $(ZEDBSD_LIBJPEG_TURBO_LICENSE) \
	$(ZEDBSD_LIBJPEG_TURBO_README_IJG))
ZEDBSD_PACKAGE_FILES += $(if $(filter libjpeg-turbo,$(ZEDBSD_USER_PROGRAMS)),\
	--file /usr/lib/$(ZEDBSD_LIBJPEG_TURBO_SONAME)=$(ZEDBSD_LIBJPEG_TURBO_LIBRARY) \
	--file /usr/share/licenses/libjpeg-turbo/LICENSE.md=$(ZEDBSD_LIBJPEG_TURBO_LICENSE) \
	--file /usr/share/licenses/libjpeg-turbo/README.ijg=$(ZEDBSD_LIBJPEG_TURBO_README_IJG))

# The development files: the headers, the .pc files and the unversioned names
# of the libraries (external.mk).
$(eval $(call ZEDBSD_EXTERNAL_DEVELOPMENT,libjpeg-turbo,$(ZEDBSD_EXT_libjpeg-turbo_STAGEDIR),\
	$(addprefix include/,jconfig.h jerror.h jmorecfg.h jpeglib.h) lib/pkgconfig/libjpeg.pc,\
	,\
	$(ZEDBSD_LIBJPEG_TURBO_SONAME)))
