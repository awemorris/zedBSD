# zedBSD userland package: pixman (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# pixman is MIT-licensed and stays outside the source tree: only this
# Makefile and the recorded archive identity live here.  It is the pixel
# compositing library under Cairo's image surface, for the GTK port (WS115).
# Provenance is in plan/ws034/package-inventory.md and
# plan/ws115/port-contract.md.
ZEDBSD_PIXMAN_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_PIXMAN_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_PIXMAN_MAKEFILE)))
include $(ZEDBSD_PIXMAN_DIR)/../../../base/package.mk
include $(ZEDBSD_PIXMAN_DIR)/../../external.mk

# 0.46.4 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; they match the SHA-512 published beside the
# archive (WS034 p001).
ZEDBSD_EXT_pixman_VERSION := 0.46.4
ZEDBSD_EXT_pixman_ROOT := pixman-0.46.4
ZEDBSD_EXT_pixman_ARCHIVE := pixman-0.46.4.tar.xz
ZEDBSD_EXT_pixman_URL := https://cairographics.org/releases/pixman-0.46.4.tar.xz
ZEDBSD_EXT_pixman_SIZE := 660536
ZEDBSD_EXT_pixman_SHA256 := a098c33924754ad43f981b740f6d576c70f9ed1006e12221b1845431ebce1239
ZEDBSD_EXT_pixman_PATCH_LEVEL := zedbsd0
ZEDBSD_EXT_pixman_PATCHES :=

$(eval $(call ZEDBSD_EXTERNAL_SOURCE,pixman))

# ---------------------------------------------------------------- build

# Meson, through the common rule.  The x86 paths are MMX, SSE2 and SSSE3:
# SSE2 is in every x86-64, and pixman picks SSSE3 at run time only on a CPU
# that has it, so the library suits any amd64 machine.  The other
# architectures' paths, OpenMP, the GTK and libpng test helpers, the demos
# and the tests are off.  Thread-local storage is on for the fast-path cache;
# like GLib, the library then calls __tls_get_addr, which the loader defines
# and nothing on the link line does, so b_lundef is off.
ZEDBSD_EXT_pixman_MAKEFILE := $(ZEDBSD_PIXMAN_MAKEFILE)
ZEDBSD_EXT_pixman_DEPENDS :=
ZEDBSD_EXT_pixman_HOST_TOOLS :=
ZEDBSD_EXT_pixman_MESON_OPTIONS := \
	-Dmmx=enabled -Dsse2=enabled -Dssse3=enabled -Dgnu-inline-asm=enabled \
	-Dloongson-mmi=disabled -Dvmx=disabled -Darm-simd=disabled -Dneon=disabled \
	-Da64-neon=disabled -Dmips-dspr2=disabled -Drvv=disabled \
	-Dtls=enabled -Dopenmp=disabled -Dtimers=false -Dgnuplot=false \
	-Dgtk=disabled -Dlibpng=disabled -Dtests=disabled -Ddemos=disabled \
	-Db_lundef=false

$(eval $(call ZEDBSD_EXTERNAL_MESON,pixman))

ZEDBSD_PIXMAN_STAGED := $(ZEDBSD_EXT_pixman_STAGEDIR)/.zedbsd-staged
ZEDBSD_PIXMAN_CHECKED := $(ZEDBSD_EXT_pixman_STAGEDIR)/.zedbsd-checked
ZEDBSD_PIXMAN_SONAME := libpixman-1.so.0
ZEDBSD_PIXMAN_LIBRARY := $(ZEDBSD_EXT_pixman_STAGEDIR)/usr/lib/libpixman-1.so.0.46.4
ZEDBSD_PIXMAN_LICENSE := $(ZEDBSD_EXT_pixman_SRCDIR)/COPYING

# Checks the library against its dependency contract once the stage exists.
$(ZEDBSD_PIXMAN_CHECKED): $(ZEDBSD_PIXMAN_STAGED)
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role shared-library \
		--soname $(ZEDBSD_PIXMAN_SONAME) --needed libc.so \
		'$(ZEDBSD_PIXMAN_LIBRARY)'
	@touch '$@'

$(ZEDBSD_PIXMAN_LICENSE): $(ZEDBSD_EXT_pixman_SRCSTAMP)
	@test -f '$@'

.PHONY: pixman
pixman: $(ZEDBSD_PIXMAN_CHECKED)
	@:

# ---------------------------------------------------------------- packaging

# The library goes in under its SONAME.  The headers and the .pc file are the
# development files below.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,pixman,Pixel manipulation library,*,n,package,,y,\
	packages/libs,libs/pixman,))

ZEDBSD_PACKAGE_INPUTS += $(if $(filter pixman,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_PIXMAN_CHECKED) $(ZEDBSD_PIXMAN_LICENSE))
ZEDBSD_PACKAGE_FILES += $(if $(filter pixman,$(ZEDBSD_USER_PROGRAMS)),\
	--file /usr/lib/$(ZEDBSD_PIXMAN_SONAME)=$(ZEDBSD_PIXMAN_LIBRARY) \
	--file /usr/share/licenses/pixman/COPYING=$(ZEDBSD_PIXMAN_LICENSE))

# The development files: the headers, the .pc files and the unversioned names
# of the libraries (external.mk).
$(eval $(call ZEDBSD_EXTERNAL_DEVELOPMENT,pixman,$(ZEDBSD_EXT_pixman_STAGEDIR),\
	lib/pkgconfig/pixman-1.pc,\
	include/pixman-1,\
	$(ZEDBSD_PIXMAN_SONAME)))
