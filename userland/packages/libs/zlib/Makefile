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
ZLIB_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZLIB_DIR := $(patsubst %/,%,$(dir $(ZLIB_MAKEFILE)))
include $(ZLIB_DIR)/../../../base/package.mk
include $(ZLIB_DIR)/../../external.mk

# 1.3.2 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; the archive's signature was checked (WS034 p001).
# zlib.net answered the CI runners with a page, so the bytes come from the
# same release on GitHub (identical size and digest, checked 2026-10-23).
EXT_zlib_VERSION := 1.3.2
EXT_zlib_ROOT := zlib-1.3.2
EXT_zlib_ARCHIVE := zlib-1.3.2.tar.xz
EXT_zlib_URL := https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.xz
EXT_zlib_SIZE := 1320064
EXT_zlib_SHA256 := d7a0654783a4da529d1bb793b7ad9c3318020af77667bcae35f95d0e42a792f3
EXT_zlib_PATCH_LEVEL := zedbsd1
EXT_zlib_PATCHES := $(sort $(wildcard $(abspath $(ZLIB_DIR))/patches/*.patch))

$(eval $(call EXTERNAL_SOURCE,zlib))

# ---------------------------------------------------------------- build

# The library's file is named by its full version; the SONAME is libz.so.1.
ZLIB_SONAME := libz.so.1
ZLIB_CONFIGURED := $(EXT_zlib_BUILDDIR)/.zedbsd-configured
ZLIB_STAGED := $(EXT_zlib_STAGEDIR)/.zedbsd-staged
ZLIB_LIBRARY := $(EXT_zlib_STAGEDIR)/usr/lib/libz.so.$(EXT_zlib_VERSION)
ZLIB_HEADERS := $(EXT_zlib_STAGEDIR)/usr/include/zlib.h \
	$(EXT_zlib_STAGEDIR)/usr/include/zconf.h
ZLIB_PC := $(EXT_zlib_STAGEDIR)/usr/lib/pkgconfig/zlib.pc
ZLIB_LICENSE := $(EXT_zlib_SRCDIR)/LICENSE

# Only the shared library: the examples and tests are not installed, and a
# static libz.a would let a package link zlib in without anyone seeing it.
ZLIB_CMAKE_OPTIONS := \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DZLIB_BUILD_SHARED=ON \
	-DZLIB_BUILD_STATIC=OFF \
	-DZLIB_BUILD_TESTING=OFF

# The options are read when the tree is configured, so this file is an input.
$(ZLIB_CONFIGURED): $(EXT_zlib_SRCSTAMP) \
	$(EXTERNAL_CROSS_STAMP) $(ZLIB_MAKEFILE)
	@rm -rf '$(EXT_zlib_BUILDDIR)'
	@mkdir -p '$(EXT_zlib_BUILDDIR)'
	cmake -S '$(EXT_zlib_SRCDIR)' -B '$(EXT_zlib_BUILDDIR)' \
		-G Ninja \
		--toolchain '$(EXTERNAL_CROSS_DIR)/zedbsd.cmake' \
		$(ZLIB_CMAKE_OPTIONS)
	@touch '$@'

$(ZLIB_STAGED): $(ZLIB_CONFIGURED)
	@rm -rf '$(EXT_zlib_STAGEDIR)'
	cmake --build '$(EXT_zlib_BUILDDIR)' --parallel $(BUILD_JOBS)
	cmake --install '$(EXT_zlib_BUILDDIR)' \
		--prefix '$(EXT_zlib_STAGEDIR)/usr'
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname $(ZLIB_SONAME) --needed libc.so '$(ZLIB_LIBRARY)'
	@touch '$@'

$(ZLIB_LIBRARY) $(ZLIB_HEADERS) $(ZLIB_PC): \
	$(ZLIB_STAGED)
	@test -f '$@'

$(ZLIB_LICENSE): $(EXT_zlib_SRCSTAMP)
	@test -f '$@'

.PHONY: zlib
zlib: $(ZLIB_STAGED)
	@:

# ---------------------------------------------------------------- packaging

# The library goes in under its SONAME, which is the name the loader asks
# for; libz.so is the link-time name a compiler on the target looks for.
$(eval $(call USERLAND_PACKAGE,zlib,zlib compression library,*,n,package,,y,\
	packages/libs,libs/zlib,))

PACKAGE_INPUTS += $(if $(filter zlib,$(USER_PROGRAMS)),\
	$(ZLIB_LIBRARY) $(ZLIB_HEADERS) $(ZLIB_PC) \
	$(ZLIB_LICENSE))
PACKAGE_FILES += $(if $(filter zlib,$(USER_PROGRAMS)),\
	--file /usr/lib/$(ZLIB_SONAME)=$(ZLIB_LIBRARY) \
	--file /usr/include/zlib.h=$(EXT_zlib_STAGEDIR)/usr/include/zlib.h \
	--file /usr/include/zconf.h=$(EXT_zlib_STAGEDIR)/usr/include/zconf.h \
	--file /usr/lib/pkgconfig/zlib.pc=$(ZLIB_PC) \
	--file /usr/share/licenses/zlib/LICENSE=$(ZLIB_LICENSE))
PACKAGE_LINKS += $(if $(filter zlib,$(USER_PROGRAMS)),\
	/usr/lib/libz.so=$(ZLIB_SONAME))
