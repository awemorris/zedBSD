# zedBSD userland package: GLib (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# GLib is LGPL-2.1-or-later and stays outside the source tree: only this
# Makefile and the recorded archive identity live here.  It is the base of
# the upstream GTK 4 port (WS115): GLib, GObject, GIO and GModule.
# Provenance and the choice of 2.84 (the build machine's own GLib tools are
# 2.84.4, so they serve this build) are in plan/ws115/port-contract.md.
ZEDBSD_GLIB_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_GLIB_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_GLIB_MAKEFILE)))
include $(ZEDBSD_GLIB_DIR)/../../../base/package.mk
include $(ZEDBSD_GLIB_DIR)/../../external.mk

# 2.84.4 is the last release of the 2.84 series.  Size and digest are the
# upstream bytes as measured; they match the digest GNOME publishes
# (glib-2.84.4.sha256sum).
ZEDBSD_EXT_glib_VERSION := 2.84.4
ZEDBSD_EXT_glib_ROOT := glib-2.84.4
ZEDBSD_EXT_glib_ARCHIVE := glib-2.84.4.tar.xz
ZEDBSD_EXT_glib_URL := https://download.gnome.org/sources/glib/2.84/glib-2.84.4.tar.xz
ZEDBSD_EXT_glib_SIZE := 5618200
ZEDBSD_EXT_glib_SHA256 := 8a9ea10943c36fc117e253f80c91e477b673525ae45762942858aef57631bb90
ZEDBSD_EXT_glib_PATCH_LEVEL := zedbsd7
ZEDBSD_EXT_glib_PATCHES := \
	$(ZEDBSD_GLIB_DIR)/patches/0001-accept-nameser-definitions-from-resolv-h.patch \
	$(ZEDBSD_GLIB_DIR)/patches/0002-include-strings-h-for-strcasecmp.patch \
	$(ZEDBSD_GLIB_DIR)/patches/0003-refuse-record-lookups-without-arpa-nameser.patch \
	$(ZEDBSD_GLIB_DIR)/patches/0004-build-without-ip-socket-options.patch \
	$(ZEDBSD_GLIB_DIR)/patches/0005-build-resolver-fuzz-target-only-with-arpa-nameser.patch

$(eval $(call ZEDBSD_EXTERNAL_SOURCE,glib))

# ---------------------------------------------------------------- build

# Meson, through the common rule.  iconv and libintl come from the C
# library.  What is left out, and why:
#   tests, installed_tests   need running on the target
#   introspection            runs target programs during the build
#   man-pages, documentation need tools the build does not have
#   nls                      the translations are not shipped
#   selinux, libmount        absent on zedBSD
#   dtrace, systemtap, sysprof, libelf  tracing and profiling hooks
# b_lundef is off because the libraries that use thread-local storage call
# __tls_get_addr, which the loader (/lib/ld.so) defines and no library on the
# link line does: libc.so itself only imports it.  A shared library is linked
# with undefined symbols allowed, as libc.so is.
# There is no inotify or kqueue, so file monitoring polls; there is no
# session bus, and GApplication then runs as a non-unique application
# without saying anything (port-contract §3, §7).
ZEDBSD_EXT_glib_MAKEFILE := $(ZEDBSD_GLIB_MAKEFILE)
ZEDBSD_EXT_glib_DEPENDS := libffi pcre2 zlib
ZEDBSD_EXT_glib_HOST_TOOLS :=
ZEDBSD_EXT_glib_MESON_OPTIONS := \
	-Dtests=false -Dinstalled_tests=false -Dintrospection=disabled \
	-Dman-pages=disabled -Ddocumentation=false -Dnls=disabled \
	-Dselinux=disabled -Dlibmount=disabled -Dxattr=true \
	-Ddtrace=disabled -Dsystemtap=disabled -Dsysprof=disabled \
	-Dlibelf=disabled -Doss_fuzz=disabled -Db_lundef=false

$(eval $(call ZEDBSD_EXTERNAL_MESON,glib))

ZEDBSD_GLIB_STAGED := $(ZEDBSD_EXT_glib_STAGEDIR)/.zedbsd-staged
ZEDBSD_GLIB_CHECKED := $(ZEDBSD_EXT_glib_STAGEDIR)/.zedbsd-checked
ZEDBSD_GLIB_LIBDIR := $(ZEDBSD_EXT_glib_STAGEDIR)/usr/lib
ZEDBSD_GLIB_LICENSE := $(ZEDBSD_EXT_glib_SRCDIR)/COPYING

# The libraries, each under its SONAME, with the libraries it needs.  The
# list is the dependency contract: a change in what upstream links shows
# up here rather than on the target.
ZEDBSD_GLIB_LIBRARIES := glib gobject gio gmodule gthread
ZEDBSD_GLIB_SONAME_glib := libglib-2.0.so.0
ZEDBSD_GLIB_NEEDED_glib := libpcre2-8.so.0 libc.so
ZEDBSD_GLIB_SONAME_gobject := libgobject-2.0.so.0
ZEDBSD_GLIB_NEEDED_gobject := libglib-2.0.so.0 libffi.so.8 libc.so
ZEDBSD_GLIB_SONAME_gio := libgio-2.0.so.0
ZEDBSD_GLIB_NEEDED_gio := libglib-2.0.so.0 libgobject-2.0.so.0 libgmodule-2.0.so.0 libz.so.1 libc.so
ZEDBSD_GLIB_SONAME_gmodule := libgmodule-2.0.so.0
ZEDBSD_GLIB_NEEDED_gmodule := libglib-2.0.so.0 libc.so
ZEDBSD_GLIB_SONAME_gthread := libgthread-2.0.so.0
# libgthread is an empty compatibility library that calls nothing in the C
# library, so --as-needed leaves libc.so out of it.
ZEDBSD_GLIB_NEEDED_gthread := libglib-2.0.so.0
ZEDBSD_GLIB_REALNAME = $(ZEDBSD_GLIB_LIBDIR)/$(ZEDBSD_GLIB_SONAME_$(1)).8400.4

# Checks every library against its contract once the stage exists.
$(ZEDBSD_GLIB_CHECKED): $(ZEDBSD_GLIB_STAGED)
	@set -e; $(foreach library,$(ZEDBSD_GLIB_LIBRARIES),\
		$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role shared-library \
		--soname $(ZEDBSD_GLIB_SONAME_$(library)) \
		$(foreach needed,$(ZEDBSD_GLIB_NEEDED_$(library)),--needed $(needed)) \
		'$(call ZEDBSD_GLIB_REALNAME,$(library))';)
	@touch '$@'

$(ZEDBSD_GLIB_LICENSE): $(ZEDBSD_EXT_glib_SRCSTAMP)
	@test -f '$@'

.PHONY: glib
glib: $(ZEDBSD_GLIB_CHECKED)
	@:

# ---------------------------------------------------------------- packaging

# Each library goes in under its SONAME, which is the name the loader asks
# for.  The headers and the .pc files are the development files below; the
# GIO tools are not installed.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,glib,GLib utility library,*,n,package,,y,\
	packages/libs,libs/glib,libs/libffi libs/pcre2 libs/zlib))

ZEDBSD_PACKAGE_INPUTS += $(if $(filter glib,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_GLIB_CHECKED) $(ZEDBSD_GLIB_LICENSE))
ZEDBSD_PACKAGE_FILES += $(if $(filter glib,$(ZEDBSD_USER_PROGRAMS)),\
	$(foreach library,$(ZEDBSD_GLIB_LIBRARIES),\
		--file /usr/lib/$(ZEDBSD_GLIB_SONAME_$(library))=$(call ZEDBSD_GLIB_REALNAME,$(library))) \
	--file /usr/share/licenses/glib/COPYING=$(ZEDBSD_GLIB_LICENSE))

# The development files: the headers, the .pc files and the unversioned names
# of the libraries (external.mk).
$(eval $(call ZEDBSD_EXTERNAL_DEVELOPMENT,glib,$(ZEDBSD_EXT_glib_STAGEDIR),\
	$(addprefix lib/pkgconfig/,glib-2.0.pc gobject-2.0.pc gio-2.0.pc gio-unix-2.0.pc gmodule-2.0.pc \
	gmodule-export-2.0.pc gmodule-no-export-2.0.pc gthread-2.0.pc),\
	include/glib-2.0 include/gio-unix-2.0 lib/glib-2.0/include,\
	$(foreach library,$(ZEDBSD_GLIB_LIBRARIES),$(ZEDBSD_GLIB_SONAME_$(library)))))
