# zedBSD userland package: expat (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# expat itself is MIT-licensed and stays outside the source tree: only this
# Makefile and the recorded archive identity live here.  Provenance is in
# plan/ws034/package-inventory.md.
EXPAT_MAKEFILE := $(lastword $(MAKEFILE_LIST))
EXPAT_DIR := $(patsubst %/,%,$(dir $(EXPAT_MAKEFILE)))
include $(EXPAT_DIR)/../../../base/package.mk
include $(EXPAT_DIR)/../../external.mk

# 2.8.5 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; the archive's signature was checked (WS034 p001).
EXT_expat_VERSION := 2.8.5
EXT_expat_ROOT := expat-2.8.5
EXT_expat_ARCHIVE := expat-2.8.5.tar.xz
EXT_expat_URL := https://github.com/libexpat/libexpat/releases/download/R_2_8_5/expat-2.8.5.tar.xz
EXT_expat_SIZE := 528380
EXT_expat_SHA256 := 1e727b8933ec51a77a9a9d9afcf8e688bce45d907c13e36ab7393fe36e703182
EXT_expat_PATCH_LEVEL := zedbsd0
EXT_expat_PATCHES :=

$(eval $(call EXTERNAL_SOURCE,expat))

# ---------------------------------------------------------------- build

# libtool would not make a shared library for an unknown system, so CMake is
# used (WS034 p001).  The SONAME is libexpat.so.1.
EXPAT_SONAME := libexpat.so.1
EXPAT_CONFIGURED := $(EXT_expat_BUILDDIR)/.zedbsd-configured
EXPAT_STAGED := $(EXT_expat_STAGEDIR)/.zedbsd-staged
EXPAT_LIBRARY := $(EXT_expat_STAGEDIR)/usr/lib/libexpat.so.1.12.5
EXPAT_XMLWF := $(EXT_expat_STAGEDIR)/usr/bin/xmlwf
EXPAT_HEADERS := $(EXT_expat_STAGEDIR)/usr/include/expat.h \
	$(EXT_expat_STAGEDIR)/usr/include/expat_external.h \
	$(EXT_expat_STAGEDIR)/usr/include/expat_config.h
EXPAT_PC := $(EXT_expat_STAGEDIR)/usr/lib/pkgconfig/expat.pc
EXPAT_LICENSE := $(EXT_expat_SRCDIR)/COPYING

# Symbol versioning stays off (its default): the loader does not read it.
# xmlwf is kept as the well-formedness checker it is; the examples, tests and
# the manual page (which needs docbook) are not built.
EXPAT_CMAKE_OPTIONS := \
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
$(EXPAT_CONFIGURED): $(EXT_expat_SRCSTAMP) \
	$(EXTERNAL_CROSS_STAMP) $(EXPAT_MAKEFILE)
	@rm -rf '$(EXT_expat_BUILDDIR)'
	@mkdir -p '$(EXT_expat_BUILDDIR)'
	cmake -S '$(EXT_expat_SRCDIR)' -B '$(EXT_expat_BUILDDIR)' \
		-G Ninja \
		--toolchain '$(EXTERNAL_CROSS_DIR)/zedbsd.cmake' \
		$(EXPAT_CMAKE_OPTIONS)
	@touch '$@'

$(EXPAT_STAGED): $(EXPAT_CONFIGURED)
	@rm -rf '$(EXT_expat_STAGEDIR)'
	cmake --build '$(EXT_expat_BUILDDIR)' --parallel $(BUILD_JOBS)
	cmake --install '$(EXT_expat_BUILDDIR)' \
		--prefix '$(EXT_expat_STAGEDIR)/usr'
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname $(EXPAT_SONAME) --needed libc.so '$(EXPAT_LIBRARY)'
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role application \
		--needed $(EXPAT_SONAME) --needed libc.so '$(EXPAT_XMLWF)'
	@touch '$@'

$(EXPAT_LIBRARY) $(EXPAT_XMLWF) $(EXPAT_HEADERS) \
$(EXPAT_PC): $(EXPAT_STAGED)
	@test -f '$@'

$(EXPAT_LICENSE): $(EXT_expat_SRCSTAMP)
	@test -f '$@'

.PHONY: expat
expat: $(EXPAT_STAGED)
	@:

# ---------------------------------------------------------------- packaging

# The library goes in under its SONAME, which is the name the loader asks
# for; libexpat.so is the link-time name a compiler on the target looks for.
$(eval $(call USERLAND_PACKAGE,expat,expat XML parser,*,n,package,,y,\
	packages/libs,libs/expat,))

PACKAGE_INPUTS += $(if $(filter expat,$(USER_PROGRAMS)),\
	$(EXPAT_LIBRARY) $(EXPAT_XMLWF) $(EXPAT_HEADERS) \
	$(EXPAT_PC) $(EXPAT_LICENSE))
PACKAGE_FILES += $(if $(filter expat,$(USER_PROGRAMS)),\
	--file /usr/lib/$(EXPAT_SONAME)=$(EXPAT_LIBRARY) \
	--file /usr/bin/xmlwf=$(EXPAT_XMLWF) \
	$(foreach header,$(EXPAT_HEADERS),\
		--file /usr/include/$(notdir $(header))=$(header)) \
	--file /usr/lib/pkgconfig/expat.pc=$(EXPAT_PC) \
	--file /usr/share/licenses/expat/COPYING=$(EXPAT_LICENSE))
PACKAGE_LINKS += $(if $(filter expat,$(USER_PROGRAMS)),\
	/usr/lib/libexpat.so=$(EXPAT_SONAME))
