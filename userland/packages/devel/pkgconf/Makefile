# zedBSD userland package: pkgconf (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# pkgconf is the pkg-config a program built on the running system asks for
# the compiler and linker flags of the libraries it uses (pkg-config gtk4).
# It is ISC-licensed and stays outside the source tree: only this Makefile
# and the recorded archive identity live here.  The command is pkgconf, with
# pkg-config as a link to it, as the build tools look for either name.
ZEDBSD_PKGCONF_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_PKGCONF_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_PKGCONF_MAKEFILE)))
include $(ZEDBSD_PKGCONF_DIR)/../../../base/package.mk
include $(ZEDBSD_PKGCONF_DIR)/../../external.mk

# 3.0.10 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; the SHA-512 of the same bytes is the one
# Alpine Linux's package records.
ZEDBSD_EXT_pkgconf_VERSION := 3.0.10
ZEDBSD_EXT_pkgconf_ROOT := pkgconf-3.0.10
ZEDBSD_EXT_pkgconf_ARCHIVE := pkgconf-3.0.10.tar.xz
ZEDBSD_EXT_pkgconf_URL := https://distfiles.ariadne.space/pkgconf/pkgconf-3.0.10.tar.xz
ZEDBSD_EXT_pkgconf_SIZE := 429268
ZEDBSD_EXT_pkgconf_SHA256 := 9371b2f6372da574d4c6dc96fe9b1463037bcf6afcb6671c1555afde7b17e2de
ZEDBSD_EXT_pkgconf_PATCH_LEVEL := zedbsd0
ZEDBSD_EXT_pkgconf_PATCHES :=

$(eval $(call ZEDBSD_EXTERNAL_SOURCE,pkgconf))

# ---------------------------------------------------------------- build

# Meson, through the common rule.  The search path is the system's own:
# /usr/lib/pkgconfig and /usr/share/pkgconfig, where the packages put their
# .pc files, and /usr/lib and /usr/include are the system directories whose
# flags pkgconf leaves out.  Its test programs are built but not installed.
ZEDBSD_EXT_pkgconf_MAKEFILE := $(ZEDBSD_PKGCONF_MAKEFILE)
ZEDBSD_EXT_pkgconf_DEPENDS :=
ZEDBSD_EXT_pkgconf_HOST_TOOLS :=
ZEDBSD_EXT_pkgconf_MESON_OPTIONS := \
	-Dwith-pkg-config-dir=/usr/lib/pkgconfig:/usr/share/pkgconfig \
	-Dwith-system-libdir=/usr/lib -Dwith-system-includedir=/usr/include

$(eval $(call ZEDBSD_EXTERNAL_MESON,pkgconf))

ZEDBSD_PKGCONF_STAGED := $(ZEDBSD_EXT_pkgconf_STAGEDIR)/.zedbsd-staged
ZEDBSD_PKGCONF_CHECKED := $(ZEDBSD_EXT_pkgconf_STAGEDIR)/.zedbsd-checked
ZEDBSD_PKGCONF_SONAME := libpkgconf.so.8
ZEDBSD_PKGCONF_LIBRARY := $(ZEDBSD_EXT_pkgconf_STAGEDIR)/usr/lib/libpkgconf.so.8.0.0
ZEDBSD_PKGCONF_PROGRAM := $(ZEDBSD_EXT_pkgconf_STAGEDIR)/usr/bin/pkgconf
ZEDBSD_PKGCONF_LICENSE := $(ZEDBSD_EXT_pkgconf_SRCDIR)/COPYING

# Checks the library and the command against their dependency contracts once
# the stage exists.
$(ZEDBSD_PKGCONF_CHECKED): $(ZEDBSD_PKGCONF_STAGED)
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role shared-library \
		--soname $(ZEDBSD_PKGCONF_SONAME) --needed libc.so \
		'$(ZEDBSD_PKGCONF_LIBRARY)'
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role application \
		--needed $(ZEDBSD_PKGCONF_SONAME) --needed libc.so \
		'$(ZEDBSD_PKGCONF_PROGRAM)'
	@touch '$@'

$(ZEDBSD_PKGCONF_LICENSE): $(ZEDBSD_EXT_pkgconf_SRCSTAMP)
	@test -f '$@'

.PHONY: pkgconf
pkgconf: $(ZEDBSD_PKGCONF_CHECKED)
	@:

# ---------------------------------------------------------------- packaging

# The library under its SONAME, the command and its pkg-config name, and the
# licence.  The library's own headers and .pc file are for programs that
# link libpkgconf, which nothing on the system does, so they stay out.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,pkgconf,pkg-config for builds on the system,*,n,package,,y,\
	packages/devel,devel/pkgconf,))

ZEDBSD_PACKAGE_INPUTS += $(if $(filter pkgconf,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_PKGCONF_CHECKED) $(ZEDBSD_PKGCONF_LICENSE))
ZEDBSD_PACKAGE_FILES += $(if $(filter pkgconf,$(ZEDBSD_USER_PROGRAMS)),\
	--file /usr/lib/$(ZEDBSD_PKGCONF_SONAME)=$(ZEDBSD_PKGCONF_LIBRARY) \
	--file $(ZEDBSD_PACKAGE_BINDIR)/pkgconf=$(ZEDBSD_PKGCONF_PROGRAM) \
	--mode $(ZEDBSD_PACKAGE_BINDIR)/pkgconf=0755 \
	--file /usr/share/licenses/pkgconf/COPYING=$(ZEDBSD_PKGCONF_LICENSE))
ZEDBSD_PACKAGE_LINKS += $(if $(filter pkgconf,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_PACKAGE_BINDIR)/pkg-config=pkgconf)
