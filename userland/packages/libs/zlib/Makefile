# zedBSD userland package: zlib (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# zlib itself is under the zlib licence and stays outside the source tree:
# only this Makefile, the patch beside it and the recorded archive identity
# live here.  Provenance is in plan/ws034/package-inventory.md.
#
# This is the upstream zlib that external packages (curl, git, glib, ...)
# build against.  The base system's own programs use libz-compat instead
# (WS035), so the two never meet in one process by accident.
ZEDBSD_ZLIB_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_ZLIB_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_ZLIB_MAKEFILE)))
include $(ZEDBSD_ZLIB_DIR)/../../../base/package.mk
include $(ZEDBSD_ZLIB_DIR)/../../external.mk

# 1.3.2 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; the archive's signature was checked (WS034 p001).
ZEDBSD_EXT_zlib_VERSION := 1.3.2
ZEDBSD_EXT_zlib_ROOT := zlib-1.3.2
ZEDBSD_EXT_zlib_ARCHIVE := zlib-1.3.2.tar.xz
ZEDBSD_EXT_zlib_URL := https://zlib.net/zlib-1.3.2.tar.xz
ZEDBSD_EXT_zlib_SIZE := 1320064
ZEDBSD_EXT_zlib_SHA256 := d7a0654783a4da529d1bb793b7ad9c3318020af77667bcae35f95d0e42a792f3
ZEDBSD_EXT_zlib_PATCH_LEVEL := zedbsd1
ZEDBSD_EXT_zlib_PATCHES := $(sort $(wildcard $(abspath $(ZEDBSD_ZLIB_DIR))/patches/*.patch))

$(eval $(call ZEDBSD_EXTERNAL_SOURCE,zlib))

# ---------------------------------------------------------------- build

# The library's file is named by its full version; the SONAME is libz.so.1.
ZEDBSD_ZLIB_SONAME := libz.so.1
ZEDBSD_ZLIB_CONFIGURED := $(ZEDBSD_EXT_zlib_BUILDDIR)/.zedbsd-configured
ZEDBSD_ZLIB_STAGED := $(ZEDBSD_EXT_zlib_STAGEDIR)/.zedbsd-staged
ZEDBSD_ZLIB_LIBRARY := $(ZEDBSD_EXT_zlib_STAGEDIR)/usr/lib/libz.so.$(ZEDBSD_EXT_zlib_VERSION)
ZEDBSD_ZLIB_HEADERS := $(ZEDBSD_EXT_zlib_STAGEDIR)/usr/include/zlib.h \
	$(ZEDBSD_EXT_zlib_STAGEDIR)/usr/include/zconf.h
ZEDBSD_ZLIB_PC := $(ZEDBSD_EXT_zlib_STAGEDIR)/usr/lib/pkgconfig/zlib.pc
ZEDBSD_ZLIB_LICENSE := $(ZEDBSD_EXT_zlib_SRCDIR)/LICENSE

# Only the shared library: the examples and tests are not installed, and a
# static libz.a would let a package link zlib in without anyone seeing it.
ZEDBSD_ZLIB_CMAKE_OPTIONS := \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DZLIB_BUILD_SHARED=ON \
	-DZLIB_BUILD_STATIC=OFF \
	-DZLIB_BUILD_TESTING=OFF

# The options are read when the tree is configured, so this file is an input.
$(ZEDBSD_ZLIB_CONFIGURED): $(ZEDBSD_EXT_zlib_SRCSTAMP) \
	$(ZEDBSD_EXTERNAL_CROSS_STAMP) $(ZEDBSD_ZLIB_MAKEFILE)
	@rm -rf '$(ZEDBSD_EXT_zlib_BUILDDIR)'
	@mkdir -p '$(ZEDBSD_EXT_zlib_BUILDDIR)'
	cmake -S '$(ZEDBSD_EXT_zlib_SRCDIR)' -B '$(ZEDBSD_EXT_zlib_BUILDDIR)' \
		-G Ninja \
		--toolchain '$(ZEDBSD_EXTERNAL_CROSS_DIR)/zedbsd.cmake' \
		$(ZEDBSD_ZLIB_CMAKE_OPTIONS)
	@touch '$@'

$(ZEDBSD_ZLIB_STAGED): $(ZEDBSD_ZLIB_CONFIGURED)
	@rm -rf '$(ZEDBSD_EXT_zlib_STAGEDIR)'
	cmake --build '$(ZEDBSD_EXT_zlib_BUILDDIR)' --parallel $(ZEDBSD_BUILD_JOBS)
	cmake --install '$(ZEDBSD_EXT_zlib_BUILDDIR)' \
		--prefix '$(ZEDBSD_EXT_zlib_STAGEDIR)/usr'
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role shared-library \
		--soname $(ZEDBSD_ZLIB_SONAME) --needed libc.so '$(ZEDBSD_ZLIB_LIBRARY)'
	@touch '$@'

$(ZEDBSD_ZLIB_LIBRARY) $(ZEDBSD_ZLIB_HEADERS) $(ZEDBSD_ZLIB_PC): \
	$(ZEDBSD_ZLIB_STAGED)
	@test -f '$@'

$(ZEDBSD_ZLIB_LICENSE): $(ZEDBSD_EXT_zlib_SRCSTAMP)
	@test -f '$@'

.PHONY: zlib
zlib: $(ZEDBSD_ZLIB_STAGED)
	@:

# ---------------------------------------------------------------- packaging

# The library goes in under its SONAME, which is the name the loader asks
# for; libz.so is the link-time name a compiler on the target looks for.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,zlib,zlib compression library,*,n,package,,y,\
	packages/libs,libs/zlib,))

ZEDBSD_PACKAGE_INPUTS += $(if $(filter zlib,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_ZLIB_LIBRARY) $(ZEDBSD_ZLIB_HEADERS) $(ZEDBSD_ZLIB_PC) \
	$(ZEDBSD_ZLIB_LICENSE))
ZEDBSD_PACKAGE_FILES += $(if $(filter zlib,$(ZEDBSD_USER_PROGRAMS)),\
	--file /usr/lib/$(ZEDBSD_ZLIB_SONAME)=$(ZEDBSD_ZLIB_LIBRARY) \
	--file /usr/include/zlib.h=$(ZEDBSD_EXT_zlib_STAGEDIR)/usr/include/zlib.h \
	--file /usr/include/zconf.h=$(ZEDBSD_EXT_zlib_STAGEDIR)/usr/include/zconf.h \
	--file /usr/lib/pkgconfig/zlib.pc=$(ZEDBSD_ZLIB_PC) \
	--file /usr/share/licenses/zlib/LICENSE=$(ZEDBSD_ZLIB_LICENSE))
ZEDBSD_PACKAGE_LINKS += $(if $(filter zlib,$(ZEDBSD_USER_PROGRAMS)),\
	/usr/lib/libz.so=$(ZEDBSD_ZLIB_SONAME))
