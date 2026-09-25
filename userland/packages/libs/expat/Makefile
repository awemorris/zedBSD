# zedBSD userland package: expat (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# expat itself is MIT-licensed and stays outside the source tree: only this
# Makefile and the recorded archive identity live here.  Provenance is in
# plan/ws034/package-inventory.md.
ZEDBSD_EXPAT_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_EXPAT_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_EXPAT_MAKEFILE)))
include $(ZEDBSD_EXPAT_DIR)/../../../base/package.mk
include $(ZEDBSD_EXPAT_DIR)/../../external.mk

# 2.8.5 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; the archive's signature was checked (WS034 p001).
ZEDBSD_EXT_expat_VERSION := 2.8.5
ZEDBSD_EXT_expat_ROOT := expat-2.8.5
ZEDBSD_EXT_expat_ARCHIVE := expat-2.8.5.tar.xz
ZEDBSD_EXT_expat_URL := https://github.com/libexpat/libexpat/releases/download/R_2_8_5/expat-2.8.5.tar.xz
ZEDBSD_EXT_expat_SIZE := 528380
ZEDBSD_EXT_expat_SHA256 := 1e727b8933ec51a77a9a9d9afcf8e688bce45d907c13e36ab7393fe36e703182
ZEDBSD_EXT_expat_PATCH_LEVEL := zedbsd0
ZEDBSD_EXT_expat_PATCHES :=

$(eval $(call ZEDBSD_EXTERNAL_SOURCE,expat))

# ---------------------------------------------------------------- build

# libtool would not make a shared library for an unknown system, so CMake is
# used (WS034 p001).  The SONAME is libexpat.so.1.
ZEDBSD_EXPAT_SONAME := libexpat.so.1
ZEDBSD_EXPAT_CONFIGURED := $(ZEDBSD_EXT_expat_BUILDDIR)/.zedbsd-configured
ZEDBSD_EXPAT_STAGED := $(ZEDBSD_EXT_expat_STAGEDIR)/.zedbsd-staged
ZEDBSD_EXPAT_LIBRARY := $(ZEDBSD_EXT_expat_STAGEDIR)/usr/lib/libexpat.so.1.12.5
ZEDBSD_EXPAT_XMLWF := $(ZEDBSD_EXT_expat_STAGEDIR)/usr/bin/xmlwf
ZEDBSD_EXPAT_HEADERS := $(ZEDBSD_EXT_expat_STAGEDIR)/usr/include/expat.h \
	$(ZEDBSD_EXT_expat_STAGEDIR)/usr/include/expat_external.h \
	$(ZEDBSD_EXT_expat_STAGEDIR)/usr/include/expat_config.h
ZEDBSD_EXPAT_PC := $(ZEDBSD_EXT_expat_STAGEDIR)/usr/lib/pkgconfig/expat.pc
ZEDBSD_EXPAT_LICENSE := $(ZEDBSD_EXT_expat_SRCDIR)/COPYING

# Symbol versioning stays off (its default): the loader does not read it.
# xmlwf is kept as the well-formedness checker it is; the examples, tests and
# the manual page (which needs docbook) are not built.
ZEDBSD_EXPAT_CMAKE_OPTIONS := \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DEXPAT_SHARED_LIBS=ON \
	-DEXPAT_SYMBOL_VERSIONING=OFF \
	-DEXPAT_BUILD_TOOLS=ON \
	-DEXPAT_BUILD_EXAMPLES=OFF \
	-DEXPAT_BUILD_TESTS=OFF \
	-DEXPAT_BUILD_DOCS=OFF \
	-DEXPAT_BUILD_FUZZERS=OFF \
	-DEXPAT_BUILD_PKGCONFIG=ON

# The options are read when the tree is configured, so this file is an input.
$(ZEDBSD_EXPAT_CONFIGURED): $(ZEDBSD_EXT_expat_SRCSTAMP) \
	$(ZEDBSD_EXTERNAL_CROSS_STAMP) $(ZEDBSD_EXPAT_MAKEFILE)
	@rm -rf '$(ZEDBSD_EXT_expat_BUILDDIR)'
	@mkdir -p '$(ZEDBSD_EXT_expat_BUILDDIR)'
	cmake -S '$(ZEDBSD_EXT_expat_SRCDIR)' -B '$(ZEDBSD_EXT_expat_BUILDDIR)' \
		-G Ninja \
		--toolchain '$(ZEDBSD_EXTERNAL_CROSS_DIR)/zedbsd.cmake' \
		$(ZEDBSD_EXPAT_CMAKE_OPTIONS)
	@touch '$@'

$(ZEDBSD_EXPAT_STAGED): $(ZEDBSD_EXPAT_CONFIGURED)
	@rm -rf '$(ZEDBSD_EXT_expat_STAGEDIR)'
	cmake --build '$(ZEDBSD_EXT_expat_BUILDDIR)' --parallel $(ZEDBSD_BUILD_JOBS)
	cmake --install '$(ZEDBSD_EXT_expat_BUILDDIR)' \
		--prefix '$(ZEDBSD_EXT_expat_STAGEDIR)/usr'
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role shared-library \
		--soname $(ZEDBSD_EXPAT_SONAME) --needed libc.so '$(ZEDBSD_EXPAT_LIBRARY)'
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role application \
		--needed $(ZEDBSD_EXPAT_SONAME) --needed libc.so '$(ZEDBSD_EXPAT_XMLWF)'
	@touch '$@'

$(ZEDBSD_EXPAT_LIBRARY) $(ZEDBSD_EXPAT_XMLWF) $(ZEDBSD_EXPAT_HEADERS) \
$(ZEDBSD_EXPAT_PC): $(ZEDBSD_EXPAT_STAGED)
	@test -f '$@'

$(ZEDBSD_EXPAT_LICENSE): $(ZEDBSD_EXT_expat_SRCSTAMP)
	@test -f '$@'

.PHONY: expat
expat: $(ZEDBSD_EXPAT_STAGED)
	@:

# ---------------------------------------------------------------- packaging

# The library goes in under its SONAME, which is the name the loader asks
# for; libexpat.so is the link-time name a compiler on the target looks for.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,expat,expat XML parser,*,n,package,,y,\
	packages/libs,libs/expat,))

ZEDBSD_PACKAGE_INPUTS += $(if $(filter expat,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_EXPAT_LIBRARY) $(ZEDBSD_EXPAT_XMLWF) $(ZEDBSD_EXPAT_HEADERS) \
	$(ZEDBSD_EXPAT_PC) $(ZEDBSD_EXPAT_LICENSE))
ZEDBSD_PACKAGE_FILES += $(if $(filter expat,$(ZEDBSD_USER_PROGRAMS)),\
	--file /usr/lib/$(ZEDBSD_EXPAT_SONAME)=$(ZEDBSD_EXPAT_LIBRARY) \
	--file /usr/bin/xmlwf=$(ZEDBSD_EXPAT_XMLWF) \
	$(foreach header,$(ZEDBSD_EXPAT_HEADERS),\
		--file /usr/include/$(notdir $(header))=$(header)) \
	--file /usr/lib/pkgconfig/expat.pc=$(ZEDBSD_EXPAT_PC) \
	--file /usr/share/licenses/expat/COPYING=$(ZEDBSD_EXPAT_LICENSE))
ZEDBSD_PACKAGE_LINKS += $(if $(filter expat,$(ZEDBSD_USER_PROGRAMS)),\
	/usr/lib/libexpat.so=$(ZEDBSD_EXPAT_SONAME))
