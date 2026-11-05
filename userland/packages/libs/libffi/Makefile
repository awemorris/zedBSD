# zedBSD userland package: libffi (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# libffi is MIT-licensed and stays outside the source tree: only this
# Makefile, the recorded archive identity and one patch live here.  GLib's
# GObject calls signal handlers through it (WS115), and Python's ctypes
# uses the same package (WS126).  Provenance is in
# plan/ws034/package-inventory.md and plan/ws115/port-contract.md.
ZEDBSD_LIBFFI_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_LIBFFI_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_LIBFFI_MAKEFILE)))
include $(ZEDBSD_LIBFFI_DIR)/../../../base/package.mk
include $(ZEDBSD_LIBFFI_DIR)/../../external.mk

# 3.8.0 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; they match the digest GitHub publishes for the
# release asset (WS034 p001).
ZEDBSD_EXT_libffi_VERSION := 3.8.0
ZEDBSD_EXT_libffi_ROOT := libffi-3.8.0
ZEDBSD_EXT_libffi_ARCHIVE := libffi-3.8.0.tar.gz
ZEDBSD_EXT_libffi_URL := https://github.com/libffi/libffi/releases/download/v3.8.0/libffi-3.8.0.tar.gz
ZEDBSD_EXT_libffi_SIZE := 1581626
ZEDBSD_EXT_libffi_SHA256 := 7da3e2d9a171eb0a038f592ecad3ff2bb2550f3496d87b3b29ad0cf4430c0db4
ZEDBSD_EXT_libffi_PATCH_LEVEL := zedbsd1
ZEDBSD_EXT_libffi_PATCHES := \
	$(ZEDBSD_LIBFFI_DIR)/patches/0001-recognise-the-zedbsd-target.patch
ZEDBSD_EXT_libffi_DEPENDS :=

$(eval $(call ZEDBSD_EXTERNAL_SOURCE,libffi))

# ---------------------------------------------------------------- build

# libffi has only an autotools build; the patch lets its configure and
# libtool recognise the system, and libtool then makes the shared library
# with the SONAME libffi.so.8.
ZEDBSD_LIBFFI_SONAME := libffi.so.8
ZEDBSD_LIBFFI_CONFIGURED := $(ZEDBSD_EXT_libffi_BUILDDIR)/.zedbsd-configured
ZEDBSD_LIBFFI_STAGED := $(ZEDBSD_EXT_libffi_STAGEDIR)/.zedbsd-staged
ZEDBSD_LIBFFI_LIBRARY := $(ZEDBSD_EXT_libffi_STAGEDIR)/usr/lib/libffi.so.8.5.0
ZEDBSD_LIBFFI_HEADERS := $(ZEDBSD_EXT_libffi_STAGEDIR)/usr/include/ffi.h \
	$(ZEDBSD_EXT_libffi_STAGEDIR)/usr/include/ffitarget.h
ZEDBSD_LIBFFI_PC := $(ZEDBSD_EXT_libffi_STAGEDIR)/usr/lib/pkgconfig/libffi.pc
ZEDBSD_LIBFFI_LICENSE := $(ZEDBSD_EXT_libffi_SRCDIR)/LICENSE

# --disable-symvers: the loader does not read symbol versions.
# --disable-static: only the shared library is installed.
# --disable-docs: the manual needs makeinfo, which is not needed here.
# --disable-multi-os-directory: the library goes to /usr/lib, not ../lib64.
# --disable-exec-static-tramp: static trampolines are a Linux mechanism;
#   closures fall back to the ordinary allocation, and GLib uses only calls.
ZEDBSD_LIBFFI_CONFIGURE_OPTIONS := \
	--prefix=/usr --libdir=/usr/lib \
	--disable-symvers --disable-static --enable-shared --disable-docs \
	--disable-multi-os-directory --disable-exec-static-tramp

$(ZEDBSD_LIBFFI_CONFIGURED): $(ZEDBSD_EXT_libffi_SRCSTAMP) \
	$(ZEDBSD_EXTERNAL_CROSS_STAMP) $(ZEDBSD_LIBFFI_MAKEFILE)
	@rm -rf '$(ZEDBSD_EXT_libffi_BUILDDIR)'
	@mkdir -p '$(ZEDBSD_EXT_libffi_BUILDDIR)'
	cd '$(ZEDBSD_EXT_libffi_BUILDDIR)' && \
		. '$(ZEDBSD_EXTERNAL_CROSS_ENV)' && \
		'$(ZEDBSD_EXT_libffi_SRCDIR)/configure' \
			--host="$$ZEDBSD_CROSS_TRIPLE" \
			$(ZEDBSD_LIBFFI_CONFIGURE_OPTIONS) \
			CC="$$CC" CXX="$$CXX" AR="$$AR" RANLIB="$$RANLIB" \
			NM="$$NM" STRIP="$$STRIP" \
			lt_cv_deplibs_check_method=pass_all
	@touch '$@'

$(ZEDBSD_LIBFFI_STAGED): $(ZEDBSD_LIBFFI_CONFIGURED)
	@rm -rf '$(ZEDBSD_EXT_libffi_STAGEDIR)'
	$(MAKE) -C '$(ZEDBSD_EXT_libffi_BUILDDIR)'
	$(MAKE) -C '$(ZEDBSD_EXT_libffi_BUILDDIR)' \
		DESTDIR='$(ZEDBSD_EXT_libffi_STAGEDIR)' install
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role shared-library \
		--soname $(ZEDBSD_LIBFFI_SONAME) --needed libc.so '$(ZEDBSD_LIBFFI_LIBRARY)'
	@touch '$@'

$(ZEDBSD_LIBFFI_LIBRARY) $(ZEDBSD_LIBFFI_HEADERS) $(ZEDBSD_LIBFFI_PC): \
	$(ZEDBSD_LIBFFI_STAGED)
	@test -f '$@'

$(ZEDBSD_LIBFFI_LICENSE): $(ZEDBSD_EXT_libffi_SRCSTAMP)
	@test -f '$@'

.PHONY: libffi
libffi: $(ZEDBSD_LIBFFI_STAGED)
	@:

# ---------------------------------------------------------------- packaging

# The library goes in under its SONAME, which is the name the loader asks
# for; libffi.so is the link-time name a compiler on the target looks for.
$(eval $(call USERLAND_PACKAGE,libffi,libffi foreign function interface,*,n,package,,y,\
	packages/libs,libs/libffi,))

ZEDBSD_PACKAGE_INPUTS += $(if $(filter libffi,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_LIBFFI_LIBRARY) $(ZEDBSD_LIBFFI_HEADERS) $(ZEDBSD_LIBFFI_PC) \
	$(ZEDBSD_LIBFFI_LICENSE))
ZEDBSD_PACKAGE_FILES += $(if $(filter libffi,$(ZEDBSD_USER_PROGRAMS)),\
	--file /usr/lib/$(ZEDBSD_LIBFFI_SONAME)=$(ZEDBSD_LIBFFI_LIBRARY) \
	$(foreach header,$(ZEDBSD_LIBFFI_HEADERS),\
		--file /usr/include/$(notdir $(header))=$(header)) \
	--file /usr/lib/pkgconfig/libffi.pc=$(ZEDBSD_LIBFFI_PC) \
	--file /usr/share/licenses/libffi/LICENSE=$(ZEDBSD_LIBFFI_LICENSE))
ZEDBSD_PACKAGE_LINKS += $(if $(filter libffi,$(ZEDBSD_USER_PROGRAMS)),\
	/usr/lib/libffi.so=$(ZEDBSD_LIBFFI_SONAME))
