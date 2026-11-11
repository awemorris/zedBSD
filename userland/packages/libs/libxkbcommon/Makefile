# zedBSD userland package: libxkbcommon (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# libxkbcommon is under MIT-style licences (LICENSE lists them) and stays
# outside the source tree: only this Makefile and the recorded archive
# identity live here.  GTK reads the keymap the Wayland compositor sends,
# and makes its default keymap from xkeyboard-config, with it (WS115).
# Provenance is in plan/ws034/package-inventory.md and
# plan/ws115/port-contract.md.
LIBXKBCOMMON_MAKEFILE := $(lastword $(MAKEFILE_LIST))
LIBXKBCOMMON_DIR := $(patsubst %/,%,$(dir $(LIBXKBCOMMON_MAKEFILE)))
include $(LIBXKBCOMMON_DIR)/../../../base/package.mk
include $(LIBXKBCOMMON_DIR)/../../external.mk

# 1.13.2 was the latest stable release when recorded.  Upstream publishes
# no tarball since 1.8; this is GitHub's archive of the xkbcommon-1.13.2 tag,
# whose commit was checked against the tag (WS034 p001).
EXT_libxkbcommon_VERSION := 1.13.2
EXT_libxkbcommon_ROOT := libxkbcommon-xkbcommon-1.13.2
EXT_libxkbcommon_ARCHIVE := libxkbcommon-1.13.2.tar.gz
EXT_libxkbcommon_URL := https://github.com/xkbcommon/libxkbcommon/archive/refs/tags/xkbcommon-1.13.2.tar.gz
EXT_libxkbcommon_SIZE := 1243485
EXT_libxkbcommon_SHA256 := acc4d5f7c3cbba5f9f8d08d8bdbeede84ecede46792f47929aa9321873385528
EXT_libxkbcommon_PATCH_LEVEL := zedbsd3
EXT_libxkbcommon_PATCHES := \
	$(LIBXKBCOMMON_DIR)/patches/0001-leave-out-version-script-on-zedbsd.patch \
	$(LIBXKBCOMMON_DIR)/patches/0002-define-static-assert-without-assert-h-support.patch \
	$(LIBXKBCOMMON_DIR)/patches/0003-build-tests-only-where-they-can-run.patch

$(eval $(call EXTERNAL_SOURCE,libxkbcommon))

# ---------------------------------------------------------------- build

# Meson, through the common rule, with bison from the build machine.  Only
# libxkbcommon itself: the X11 library, xkbregistry, the tools, the Wayland
# helpers, bash completion and the documentation are off.  The data root is
# where xkeyboard-config installs; patch 0001 leaves the symbol version
# script out, patch 0002 supplies the C11 static_assert the C library's
# <assert.h> lacks, and patch 0003 leaves out the tests, which a cross build
# cannot run.
EXT_libxkbcommon_MAKEFILE := $(LIBXKBCOMMON_MAKEFILE)
EXT_libxkbcommon_DEPENDS := xkeyboard-config
EXT_libxkbcommon_HOST_TOOLS :=
EXT_libxkbcommon_MESON_OPTIONS := \
	-Dxkb-config-root=/usr/share/xkeyboard-config-2 \
	-Denable-tools=false -Denable-x11=false -Denable-docs=false \
	-Denable-wayland=false -Denable-xkbregistry=false \
	-Denable-bash-completion=false

$(eval $(call EXTERNAL_MESON,libxkbcommon))

LIBXKBCOMMON_STAGED := $(EXT_libxkbcommon_STAGEDIR)/.zedbsd-staged
LIBXKBCOMMON_CHECKED := $(EXT_libxkbcommon_STAGEDIR)/.zedbsd-checked
LIBXKBCOMMON_SONAME := libxkbcommon.so.0
LIBXKBCOMMON_LIBRARY := $(EXT_libxkbcommon_STAGEDIR)/usr/lib/libxkbcommon.so.0.13.2
LIBXKBCOMMON_LICENSE := $(EXT_libxkbcommon_SRCDIR)/LICENSE

# Checks the library against its dependency contract once the stage exists.
$(LIBXKBCOMMON_CHECKED): $(LIBXKBCOMMON_STAGED)
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname $(LIBXKBCOMMON_SONAME) --needed libc.so \
		'$(LIBXKBCOMMON_LIBRARY)'
	@touch '$@'

$(LIBXKBCOMMON_LICENSE): $(EXT_libxkbcommon_SRCSTAMP)
	@test -f '$@'

.PHONY: libxkbcommon
libxkbcommon: $(LIBXKBCOMMON_CHECKED)
	@:

# ---------------------------------------------------------------- packaging

# The library under its SONAME; the data comes with xkeyboard-config.  The
# headers and the .pc file are the development files below.
$(eval $(call USERLAND_PACKAGE,libxkbcommon,Keymap handling library,*,n,package,,y,\
	packages/libs,libs/libxkbcommon,libs/xkeyboard-config))

PACKAGE_INPUTS += $(if $(filter libxkbcommon,$(USER_PROGRAMS)),\
	$(LIBXKBCOMMON_CHECKED) $(LIBXKBCOMMON_LICENSE))
PACKAGE_FILES += $(if $(filter libxkbcommon,$(USER_PROGRAMS)),\
	--file /usr/lib/$(LIBXKBCOMMON_SONAME)=$(LIBXKBCOMMON_LIBRARY) \
	--file /usr/share/licenses/libxkbcommon/LICENSE=$(LIBXKBCOMMON_LICENSE))

# The development files: the headers, the .pc files and the unversioned names
# of the libraries (external.mk).
$(eval $(call EXTERNAL_DEVELOPMENT,libxkbcommon,$(EXT_libxkbcommon_STAGEDIR),\
	lib/pkgconfig/xkbcommon.pc,\
	include/xkbcommon,\
	$(LIBXKBCOMMON_SONAME)))
