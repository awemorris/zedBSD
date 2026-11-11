# zedBSD userland package: libffi (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# libffi is MIT-licensed and stays outside the source tree: only this
# Makefile, the recorded archive identity and one patch live here.  GLib's
# GObject calls signal handlers through it (WS115), and Python's ctypes
# uses the same package (WS126).  Provenance is in
# plan/ws034/package-inventory.md and plan/ws115/port-contract.md.
LIBFFI_MAKEFILE := $(lastword $(MAKEFILE_LIST))
LIBFFI_DIR := $(patsubst %/,%,$(dir $(LIBFFI_MAKEFILE)))
include $(LIBFFI_DIR)/../../../base/package.mk
include $(LIBFFI_DIR)/../../external.mk

# 3.8.0 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; they match the digest GitHub publishes for the
# release asset (WS034 p001).
EXT_libffi_VERSION := 3.8.0
EXT_libffi_ROOT := libffi-3.8.0
EXT_libffi_ARCHIVE := libffi-3.8.0.tar.gz
EXT_libffi_URL := https://github.com/libffi/libffi/releases/download/v3.8.0/libffi-3.8.0.tar.gz
EXT_libffi_SIZE := 1581626
EXT_libffi_SHA256 := 7da3e2d9a171eb0a038f592ecad3ff2bb2550f3496d87b3b29ad0cf4430c0db4
EXT_libffi_PATCH_LEVEL := zedbsd1
EXT_libffi_PATCHES := \
	$(LIBFFI_DIR)/patches/0001-recognise-the-zedbsd-target.patch
EXT_libffi_DEPENDS :=

$(eval $(call EXTERNAL_SOURCE,libffi))

# ---------------------------------------------------------------- build

# libffi has only an autotools build; the patch lets its configure and
# libtool recognise the system, and libtool then makes the shared library
# with the SONAME libffi.so.8.
LIBFFI_SONAME := libffi.so.8
LIBFFI_CONFIGURED := $(EXT_libffi_BUILDDIR)/.zedbsd-configured
LIBFFI_STAGED := $(EXT_libffi_STAGEDIR)/.zedbsd-staged
LIBFFI_LIBRARY := $(EXT_libffi_STAGEDIR)/usr/lib/libffi.so.8.5.0
LIBFFI_HEADERS := $(EXT_libffi_STAGEDIR)/usr/include/ffi.h \
	$(EXT_libffi_STAGEDIR)/usr/include/ffitarget.h
LIBFFI_PC := $(EXT_libffi_STAGEDIR)/usr/lib/pkgconfig/libffi.pc
LIBFFI_LICENSE := $(EXT_libffi_SRCDIR)/LICENSE

# --disable-symvers: the loader does not read symbol versions.
# --disable-static: only the shared library is installed.
# --disable-docs: the manual needs makeinfo, which is not needed here.
# --disable-multi-os-directory: the library goes to /usr/lib, not ../lib64.
# --disable-exec-static-tramp: static trampolines are a Linux mechanism;
#   closures fall back to the ordinary allocation, and GLib uses only calls.
LIBFFI_CONFIGURE_OPTIONS := \
	--prefix=/usr --libdir=/usr/lib \
	--disable-symvers --disable-static --enable-shared --disable-docs \
	--disable-multi-os-directory --disable-exec-static-tramp

$(LIBFFI_CONFIGURED): $(EXT_libffi_SRCSTAMP) \
	$(EXTERNAL_CROSS_STAMP) $(LIBFFI_MAKEFILE)
	@rm -rf '$(EXT_libffi_BUILDDIR)'
	@mkdir -p '$(EXT_libffi_BUILDDIR)'
	cd '$(EXT_libffi_BUILDDIR)' && \
		. '$(EXTERNAL_CROSS_ENV)' && \
		'$(EXT_libffi_SRCDIR)/configure' \
			--host="$$CROSS_TRIPLE" \
			$(LIBFFI_CONFIGURE_OPTIONS) \
			CC="$$CC" CXX="$$CXX" AR="$$AR" RANLIB="$$RANLIB" \
			NM="$$NM" STRIP="$$STRIP" \
			lt_cv_deplibs_check_method=pass_all
	@touch '$@'

$(LIBFFI_STAGED): $(LIBFFI_CONFIGURED)
	@rm -rf '$(EXT_libffi_STAGEDIR)'
	$(MAKE) -C '$(EXT_libffi_BUILDDIR)'
	$(MAKE) -C '$(EXT_libffi_BUILDDIR)' \
		DESTDIR='$(EXT_libffi_STAGEDIR)' install
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname $(LIBFFI_SONAME) --needed libc.so '$(LIBFFI_LIBRARY)'
	@touch '$@'

$(LIBFFI_LIBRARY) $(LIBFFI_HEADERS) $(LIBFFI_PC): \
	$(LIBFFI_STAGED)
	@test -f '$@'

$(LIBFFI_LICENSE): $(EXT_libffi_SRCSTAMP)
	@test -f '$@'

.PHONY: libffi
libffi: $(LIBFFI_STAGED)
	@:

# ---------------------------------------------------------------- packaging

# The library goes in under its SONAME, which is the name the loader asks
# for; libffi.so is the link-time name a compiler on the target looks for.
$(eval $(call USERLAND_PACKAGE,libffi,libffi foreign function interface,*,n,package,,y,\
	packages/libs,libs/libffi,))

PACKAGE_INPUTS += $(if $(filter libffi,$(USER_PROGRAMS)),\
	$(LIBFFI_LIBRARY) $(LIBFFI_HEADERS) $(LIBFFI_PC) \
	$(LIBFFI_LICENSE))
PACKAGE_FILES += $(if $(filter libffi,$(USER_PROGRAMS)),\
	--file /usr/lib/$(LIBFFI_SONAME)=$(LIBFFI_LIBRARY) \
	$(foreach header,$(LIBFFI_HEADERS),\
		--file /usr/include/$(notdir $(header))=$(header)) \
	--file /usr/lib/pkgconfig/libffi.pc=$(LIBFFI_PC) \
	--file /usr/share/licenses/libffi/LICENSE=$(LIBFFI_LICENSE))
PACKAGE_LINKS += $(if $(filter libffi,$(USER_PROGRAMS)),\
	/usr/lib/libffi.so=$(LIBFFI_SONAME))
