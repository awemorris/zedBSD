# zedBSD userland package: libepoxy (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# libepoxy is MIT-licensed and stays outside the source tree: only this
# Makefile and the recorded archive identity live here.  It is the GL
# function loader GTK always links; GTK's GL renderer reaches zedBSD's EGL
# and GLES through it (WS115, R1 = Cairo then GL).  Provenance is in
# plan/ws034/package-inventory.md and plan/ws115/port-contract.md §6.
LIBEPOXY_MAKEFILE := $(lastword $(MAKEFILE_LIST))
LIBEPOXY_DIR := $(patsubst %/,%,$(dir $(LIBEPOXY_MAKEFILE)))
include $(LIBEPOXY_DIR)/../../../base/package.mk
include $(LIBEPOXY_DIR)/../../external.mk

# 1.5.10 is the latest release (2022).  Size and digest are the upstream
# bytes as measured; they match the digest GNOME publishes.
EXT_libepoxy_VERSION := 1.5.10
EXT_libepoxy_ROOT := libepoxy-1.5.10
EXT_libepoxy_ARCHIVE := libepoxy-1.5.10.tar.xz
EXT_libepoxy_URL := https://download.gnome.org/sources/libepoxy/1.5/libepoxy-1.5.10.tar.xz
EXT_libepoxy_SIZE := 223528
EXT_libepoxy_SHA256 := 072cda4b59dd098bba8c2363a6247299db1fa89411dc221c8b81b8ee8192e623
EXT_libepoxy_PATCH_LEVEL := zedbsd2
EXT_libepoxy_PATCHES := \
	$(LIBEPOXY_DIR)/patches/0001-load-zedbsd-egl-and-gl-library-names.patch \
	$(LIBEPOXY_DIR)/patches/0002-treat-library-as-absent-without-rtld-noload.patch

$(eval $(call EXTERNAL_SOURCE,libepoxy))

# ---------------------------------------------------------------- build

# Meson, through the common rule.  epoxy carries its own GL and EGL headers
# and finds the implementation at run time with dlopen(); patch 0001 gives it
# the names of zedBSD's libraries (libEGL.so, libGLESv2.so, libGL.so, libGLX.so), and
# patch 0002 copes with a dlopen() without RTLD_NOLOAD.  EGL
# is on; GLX and the rest of X11 are off (GTK runs on Wayland here), as are
# the tests and the documentation.
EXT_libepoxy_MAKEFILE := $(LIBEPOXY_MAKEFILE)
EXT_libepoxy_DEPENDS :=
EXT_libepoxy_HOST_TOOLS :=
EXT_libepoxy_MESON_OPTIONS := -Degl=yes -Dglx=no -Dx11=false -Dtests=false -Ddocs=false

$(eval $(call EXTERNAL_MESON,libepoxy))

LIBEPOXY_STAGED := $(EXT_libepoxy_STAGEDIR)/.zedbsd-staged
LIBEPOXY_CHECKED := $(EXT_libepoxy_STAGEDIR)/.zedbsd-checked
LIBEPOXY_SONAME := libepoxy.so.0
LIBEPOXY_LIBRARY := $(EXT_libepoxy_STAGEDIR)/usr/lib/libepoxy.so.0.0.0
LIBEPOXY_LICENSE := $(EXT_libepoxy_SRCDIR)/COPYING

# Checks the library against its dependency contract once the stage exists.
$(LIBEPOXY_CHECKED): $(LIBEPOXY_STAGED)
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname $(LIBEPOXY_SONAME) \
		--needed libc.so \
		'$(LIBEPOXY_LIBRARY)'
	@touch '$@'

$(LIBEPOXY_LICENSE): $(EXT_libepoxy_SRCSTAMP)
	@test -f '$@'

.PHONY: libepoxy
libepoxy: $(LIBEPOXY_CHECKED)
	@:

# ---------------------------------------------------------------- packaging

# The library goes in under its SONAME.  The headers and the .pc file are the
# development files below.
$(eval $(call USERLAND_PACKAGE,libepoxy,GL function pointer loader,*,n,package,,y,\
	packages/libs,libs/libepoxy,))

PACKAGE_INPUTS += $(if $(filter libepoxy,$(USER_PROGRAMS)),\
	$(LIBEPOXY_CHECKED) $(LIBEPOXY_LICENSE))
PACKAGE_FILES += $(if $(filter libepoxy,$(USER_PROGRAMS)),\
	--file /usr/lib/$(LIBEPOXY_SONAME)=$(LIBEPOXY_LIBRARY) \
	--file /usr/share/licenses/libepoxy/COPYING=$(LIBEPOXY_LICENSE))

# The development files: the headers, the .pc files and the unversioned names
# of the libraries (external.mk).
$(eval $(call EXTERNAL_DEVELOPMENT,libepoxy,$(EXT_libepoxy_STAGEDIR),\
	lib/pkgconfig/epoxy.pc,\
	include/epoxy,\
	$(LIBEPOXY_SONAME)))
