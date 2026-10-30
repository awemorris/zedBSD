# zedBSD userland package: gdk-pixbuf (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# gdk-pixbuf is LGPL-2.1-or-later and stays outside the source tree: only
# this Makefile and the recorded archive identity live here.  It loads the
# images GTK shows (icons, textures), for the GTK port (WS115).  Provenance
# is in plan/ws034/package-inventory.md and plan/ws115/port-contract.md.
ZEDBSD_GDK_PIXBUF_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_GDK_PIXBUF_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_GDK_PIXBUF_MAKEFILE)))
include $(ZEDBSD_GDK_PIXBUF_DIR)/../../../base/package.mk
include $(ZEDBSD_GDK_PIXBUF_DIR)/../../external.mk

# 2.44.8 was the latest release when recorded.  Size and digest are the
# upstream bytes as measured; they match the digest GNOME publishes.
ZEDBSD_EXT_gdk-pixbuf_VERSION := 2.44.8
ZEDBSD_EXT_gdk-pixbuf_ROOT := gdk-pixbuf-2.44.8
ZEDBSD_EXT_gdk-pixbuf_ARCHIVE := gdk-pixbuf-2.44.8.tar.xz
ZEDBSD_EXT_gdk-pixbuf_URL := https://download.gnome.org/sources/gdk-pixbuf/2.44/gdk-pixbuf-2.44.8.tar.xz
ZEDBSD_EXT_gdk-pixbuf_SIZE := 6037756
ZEDBSD_EXT_gdk-pixbuf_SHA256 := 919f529512961a12e81cd4b4b466a48c3933469e7f9a310c6513cd4fb252ba3c
ZEDBSD_EXT_gdk-pixbuf_PATCH_LEVEL := zedbsd0
ZEDBSD_EXT_gdk-pixbuf_PATCHES :=

$(eval $(call ZEDBSD_EXTERNAL_SOURCE,gdk-pixbuf))

# ---------------------------------------------------------------- build

# Meson, through the common rule.  Every loader is built into the library, so
# there is no loaders.cache to make on the target: PNG, JPEG, TIFF and GIF,
# plus the small format loaders upstream builds by default.  What is off, and
# why:
#   glycin            the sandboxed loader service, which is Linux-only
#   others            the weakly maintained loaders upstream itself turns off
#   gio_sniffing      it asks shared-mime-info, which zedBSD does not have; the
#                     loaders' own signatures identify the format instead
#   thumbnailer, man, docs, introspection, tests, relocatable, android
ZEDBSD_EXT_gdk-pixbuf_MAKEFILE := $(ZEDBSD_GDK_PIXBUF_MAKEFILE)
ZEDBSD_EXT_gdk-pixbuf_DEPENDS := glib libpng libjpeg-turbo libtiff
ZEDBSD_EXT_gdk-pixbuf_HOST_TOOLS :=
ZEDBSD_EXT_gdk-pixbuf_MESON_OPTIONS := \
	-Dpng=enabled -Djpeg=enabled -Dtiff=enabled -Dgif=enabled \
	-Dglycin=disabled -Dandroid=disabled -Dothers=disabled \
	-Dbuiltin_loaders=all -Dgio_sniffing=false -Dthumbnailer=disabled \
	-Dgtk_doc=false -Ddocumentation=false -Dintrospection=disabled -Dman=false \
	-Drelocatable=false -Dnative_windows_loaders=false \
	-Dtests=false -Dinstalled_tests=false

$(eval $(call ZEDBSD_EXTERNAL_MESON,gdk-pixbuf))

ZEDBSD_GDK_PIXBUF_STAGED := $(ZEDBSD_EXT_gdk-pixbuf_STAGEDIR)/.zedbsd-staged
ZEDBSD_GDK_PIXBUF_CHECKED := $(ZEDBSD_EXT_gdk-pixbuf_STAGEDIR)/.zedbsd-checked
ZEDBSD_GDK_PIXBUF_SONAME := libgdk_pixbuf-2.0.so.0
ZEDBSD_GDK_PIXBUF_LIBRARY := $(ZEDBSD_EXT_gdk-pixbuf_STAGEDIR)/usr/lib/libgdk_pixbuf-2.0.so.0.4400.8
ZEDBSD_GDK_PIXBUF_LICENSE := $(ZEDBSD_EXT_gdk-pixbuf_SRCDIR)/COPYING

# Checks the library against its dependency contract once the stage exists.
$(ZEDBSD_GDK_PIXBUF_CHECKED): $(ZEDBSD_GDK_PIXBUF_STAGED)
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role shared-library \
		--soname $(ZEDBSD_GDK_PIXBUF_SONAME) \
		--needed libglib-2.0.so.0 --needed libgobject-2.0.so.0 --needed libgmodule-2.0.so.0 --needed libgio-2.0.so.0 --needed libpng16.so.16 --needed libjpeg.so.62 --needed libtiff.so.6 --needed libc.so \
		'$(ZEDBSD_GDK_PIXBUF_LIBRARY)'
	@touch '$@'

$(ZEDBSD_GDK_PIXBUF_LICENSE): $(ZEDBSD_EXT_gdk-pixbuf_SRCSTAMP)
	@test -f '$@'

.PHONY: gdk-pixbuf
gdk-pixbuf: $(ZEDBSD_GDK_PIXBUF_CHECKED)
	@:

# ---------------------------------------------------------------- packaging

# The library goes in under its SONAME.  The headers and the .pc file are the
# development files below.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,gdk-pixbuf,Image loading library,*,n,package,,y,\
	packages/libs,libs/gdk-pixbuf,libs/glib libs/libpng libs/libjpeg-turbo libs/libtiff))

ZEDBSD_PACKAGE_INPUTS += $(if $(filter gdk-pixbuf,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_GDK_PIXBUF_CHECKED) $(ZEDBSD_GDK_PIXBUF_LICENSE))
ZEDBSD_PACKAGE_FILES += $(if $(filter gdk-pixbuf,$(ZEDBSD_USER_PROGRAMS)),\
	--file /usr/lib/$(ZEDBSD_GDK_PIXBUF_SONAME)=$(ZEDBSD_GDK_PIXBUF_LIBRARY) \
	--file /usr/share/licenses/gdk-pixbuf/COPYING=$(ZEDBSD_GDK_PIXBUF_LICENSE))

# The development files: the headers, the .pc files and the unversioned names
# of the libraries (external.mk).
$(eval $(call ZEDBSD_EXTERNAL_DEVELOPMENT,gdk-pixbuf,$(ZEDBSD_EXT_gdk-pixbuf_STAGEDIR),\
	lib/pkgconfig/gdk-pixbuf-2.0.pc,\
	include/gdk-pixbuf-2.0,\
	$(ZEDBSD_GDK_PIXBUF_SONAME)))
