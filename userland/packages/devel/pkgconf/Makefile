# zedBSD userland package: pkgconf (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# pkgconf is the pkg-config a program built on the running system asks for
# the compiler and linker flags of the libraries it uses (pkg-config gtk4).
# It is ISC-licensed and stays outside the source tree: only this Makefile
# and the recorded archive identity live here.  The command is pkgconf, with
# pkg-config as a link to it, as the build tools look for either name.
PKGCONF_MAKEFILE := $(lastword $(MAKEFILE_LIST))
PKGCONF_DIR := $(patsubst %/,%,$(dir $(PKGCONF_MAKEFILE)))
include $(PKGCONF_DIR)/../../../base/package.mk
include $(PKGCONF_DIR)/../../external.mk

# 3.0.10 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; the SHA-512 of the same bytes is the one
# Alpine Linux's package records.
EXT_pkgconf_VERSION := 3.0.10
EXT_pkgconf_ROOT := pkgconf-3.0.10
EXT_pkgconf_ARCHIVE := pkgconf-3.0.10.tar.xz
EXT_pkgconf_URL := https://distfiles.ariadne.space/pkgconf/pkgconf-3.0.10.tar.xz
EXT_pkgconf_SIZE := 429268
EXT_pkgconf_SHA256 := 9371b2f6372da574d4c6dc96fe9b1463037bcf6afcb6671c1555afde7b17e2de
EXT_pkgconf_PATCH_LEVEL := zedbsd0
EXT_pkgconf_PATCHES :=

$(eval $(call EXTERNAL_SOURCE,pkgconf))

# ---------------------------------------------------------------- build

# Meson, through the common rule.  The search path is the system's own:
# /usr/lib/pkgconfig and /usr/share/pkgconfig, where the packages put their
# .pc files, and /usr/lib and /usr/include are the system directories whose
# flags pkgconf leaves out.  Its test programs are built but not installed.
EXT_pkgconf_MAKEFILE := $(PKGCONF_MAKEFILE)
EXT_pkgconf_DEPENDS :=
EXT_pkgconf_HOST_TOOLS :=
EXT_pkgconf_MESON_OPTIONS := \
	-Dwith-pkg-config-dir=/usr/lib/pkgconfig:/usr/share/pkgconfig \
	-Dwith-system-libdir=/usr/lib -Dwith-system-includedir=/usr/include

$(eval $(call EXTERNAL_MESON,pkgconf))

PKGCONF_STAGED := $(EXT_pkgconf_STAGEDIR)/.zedbsd-staged
PKGCONF_CHECKED := $(EXT_pkgconf_STAGEDIR)/.zedbsd-checked
PKGCONF_SONAME := libpkgconf.so.8
PKGCONF_LIBRARY := $(EXT_pkgconf_STAGEDIR)/usr/lib/libpkgconf.so.8.0.0
PKGCONF_PROGRAM := $(EXT_pkgconf_STAGEDIR)/usr/bin/pkgconf
PKGCONF_LICENSE := $(EXT_pkgconf_SRCDIR)/COPYING

# Checks the library and the command against their dependency contracts once
# the stage exists.
$(PKGCONF_CHECKED): $(PKGCONF_STAGED)
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname $(PKGCONF_SONAME) --needed libc.so \
		'$(PKGCONF_LIBRARY)'
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role application \
		--needed $(PKGCONF_SONAME) --needed libc.so \
		'$(PKGCONF_PROGRAM)'
	@touch '$@'

$(PKGCONF_LICENSE): $(EXT_pkgconf_SRCSTAMP)
	@test -f '$@'

.PHONY: pkgconf
pkgconf: $(PKGCONF_CHECKED)
	@:

# ---------------------------------------------------------------- packaging

# The library under its SONAME, the command and its pkg-config name, and the
# licence.  The library's own headers and .pc file are for programs that
# link libpkgconf, which nothing on the system does, so they stay out.
$(eval $(call USERLAND_PACKAGE,pkgconf,pkg-config for builds on the system,*,n,package,,y,\
	packages/devel,devel/pkgconf,))

PACKAGE_INPUTS += $(if $(filter pkgconf,$(USER_PROGRAMS)),\
	$(PKGCONF_CHECKED) $(PKGCONF_LICENSE))
PACKAGE_FILES += $(if $(filter pkgconf,$(USER_PROGRAMS)),\
	--file /usr/lib/$(PKGCONF_SONAME)=$(PKGCONF_LIBRARY) \
	--file $(PACKAGE_BINDIR)/pkgconf=$(PKGCONF_PROGRAM) \
	--mode $(PACKAGE_BINDIR)/pkgconf=0755 \
	--file /usr/share/licenses/pkgconf/COPYING=$(PKGCONF_LICENSE))
PACKAGE_LINKS += $(if $(filter pkgconf,$(USER_PROGRAMS)),\
	$(PACKAGE_BINDIR)/pkg-config=pkgconf)
