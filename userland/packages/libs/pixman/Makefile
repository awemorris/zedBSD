# zedBSD userland package: pixman (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# pixman is MIT-licensed and stays outside the source tree: only this
# Makefile and the recorded archive identity live here.  It is the pixel
# compositing library under Cairo's image surface, for the GTK port (WS115).
# Provenance is in plan/ws034/package-inventory.md and
# plan/ws115/port-contract.md.
PIXMAN_MAKEFILE := $(lastword $(MAKEFILE_LIST))
PIXMAN_DIR := $(patsubst %/,%,$(dir $(PIXMAN_MAKEFILE)))
include $(PIXMAN_DIR)/../../../base/package.mk
include $(PIXMAN_DIR)/../../external.mk

# 0.46.4 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; they match the SHA-512 published beside the
# archive (WS034 p001).
EXT_pixman_VERSION := 0.46.4
EXT_pixman_ROOT := pixman-0.46.4
EXT_pixman_ARCHIVE := pixman-0.46.4.tar.xz
EXT_pixman_URL := https://cairographics.org/releases/pixman-0.46.4.tar.xz
EXT_pixman_SIZE := 660536
EXT_pixman_SHA256 := a098c33924754ad43f981b740f6d576c70f9ed1006e12221b1845431ebce1239
EXT_pixman_PATCH_LEVEL := zedbsd0
EXT_pixman_PATCHES :=

$(eval $(call EXTERNAL_SOURCE,pixman))

# ---------------------------------------------------------------- build

# Meson, through the common rule.  The x86 paths are MMX, SSE2 and SSSE3:
# SSE2 is in every x86-64, and pixman picks SSSE3 at run time only on a CPU
# that has it, so the library suits any amd64 machine.  The other
# architectures' paths, OpenMP, the GTK and libpng test helpers, the demos
# and the tests are off.  Thread-local storage is on for the fast-path cache;
# like GLib, the library then calls __tls_get_addr, which the loader defines
# and nothing on the link line does, so b_lundef is off.
EXT_pixman_MAKEFILE := $(PIXMAN_MAKEFILE)
EXT_pixman_DEPENDS :=
EXT_pixman_HOST_TOOLS :=
EXT_pixman_MESON_OPTIONS := \
	-Dmmx=enabled -Dsse2=enabled -Dssse3=enabled -Dgnu-inline-asm=enabled \
	-Dloongson-mmi=disabled -Dvmx=disabled -Darm-simd=disabled -Dneon=disabled \
	-Da64-neon=disabled -Dmips-dspr2=disabled -Drvv=disabled \
	-Dtls=enabled -Dopenmp=disabled -Dtimers=false -Dgnuplot=false \
	-Dgtk=disabled -Dlibpng=disabled -Dtests=disabled -Ddemos=disabled \
	-Db_lundef=false

$(eval $(call EXTERNAL_MESON,pixman))

PIXMAN_STAGED := $(EXT_pixman_STAGEDIR)/.zedbsd-staged
PIXMAN_CHECKED := $(EXT_pixman_STAGEDIR)/.zedbsd-checked
PIXMAN_SONAME := libpixman-1.so.0
PIXMAN_LIBRARY := $(EXT_pixman_STAGEDIR)/usr/lib/libpixman-1.so.0.46.4
PIXMAN_LICENSE := $(EXT_pixman_SRCDIR)/COPYING

# Checks the library against its dependency contract once the stage exists.
$(PIXMAN_CHECKED): $(PIXMAN_STAGED)
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname $(PIXMAN_SONAME) --needed libc.so \
		'$(PIXMAN_LIBRARY)'
	@touch '$@'

$(PIXMAN_LICENSE): $(EXT_pixman_SRCSTAMP)
	@test -f '$@'

.PHONY: pixman
pixman: $(PIXMAN_CHECKED)
	@:

# ---------------------------------------------------------------- packaging

# The library goes in under its SONAME.  The headers and the .pc file are the
# development files below.
$(eval $(call USERLAND_PACKAGE,pixman,Pixel manipulation library,*,n,package,,y,\
	packages/libs,libs/pixman,))

PACKAGE_INPUTS += $(if $(filter pixman,$(USER_PROGRAMS)),\
	$(PIXMAN_CHECKED) $(PIXMAN_LICENSE))
PACKAGE_FILES += $(if $(filter pixman,$(USER_PROGRAMS)),\
	--file /usr/lib/$(PIXMAN_SONAME)=$(PIXMAN_LIBRARY) \
	--file /usr/share/licenses/pixman/COPYING=$(PIXMAN_LICENSE))

# The development files: the headers, the .pc files and the unversioned names
# of the libraries (external.mk).
$(eval $(call EXTERNAL_DEVELOPMENT,pixman,$(EXT_pixman_STAGEDIR),\
	lib/pkgconfig/pixman-1.pc,\
	include/pixman-1,\
	$(PIXMAN_SONAME)))
