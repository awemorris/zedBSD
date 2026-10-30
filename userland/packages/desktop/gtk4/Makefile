# zedBSD userland package: GTK 4 (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# GTK is LGPL-2.1-or-later and stays outside the source tree: only this
# Makefile, the recorded archive identity and the patches live here.  This is
# the plain upstream GTK 4 ported to zedBSD (WS115), to find where a port
# needs work; the knowledge goes to WS097's own toolkit.  The choice of 4.18.6
# and every patch's reason are in plan/ws115/port-contract.md (§1, §11).
ZEDBSD_GTK4_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_GTK4_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_GTK4_MAKEFILE)))
include $(ZEDBSD_GTK4_DIR)/../../../base/package.mk
include $(ZEDBSD_GTK4_DIR)/../../external.mk

# 4.18.6 is the last release of 4.18, the series measured on Linux in WS114.
# Size and digest are the upstream bytes as measured; they match the
# published .sha256sum.
ZEDBSD_EXT_gtk4_VERSION := 4.18.6
ZEDBSD_EXT_gtk4_ROOT := gtk-4.18.6
ZEDBSD_EXT_gtk4_ARCHIVE := gtk-4.18.6.tar.xz
ZEDBSD_EXT_gtk4_URL := https://download.gnome.org/sources/gtk/4.18/gtk-4.18.6.tar.xz
ZEDBSD_EXT_gtk4_SIZE := 17710412
ZEDBSD_EXT_gtk4_SHA256 := e1817c650ddc3261f9a8345b3b22a26a5d80af154630dedc03cc7becefffd0fa
ZEDBSD_EXT_gtk4_PATCH_LEVEL := zedbsd3
ZEDBSD_EXT_gtk4_PATCHES := \
	$(ZEDBSD_GTK4_DIR)/patches/0001-round-without-fenv-rounding-modes.patch \
	$(ZEDBSD_GTK4_DIR)/patches/0002-use-guint-for-the-bsd-uint-type.patch \
	$(ZEDBSD_GTK4_DIR)/patches/0003-use-setjmp-with-libpng-jmpbuf-on-zedbsd.patch \
	$(ZEDBSD_GTK4_DIR)/patches/0004-leave-out-malloc-h-on-zedbsd.patch \
	$(ZEDBSD_GTK4_DIR)/patches/0005-report-missing-session-bus-as-a11y-debug.patch

$(eval $(call ZEDBSD_EXTERNAL_SOURCE,gtk4))

# ---------------------------------------------------------------- build

# Meson, through the common rule.  The user's decisions (WS115): the Wayland
# backend only; drawing with Cairo, then GL through libepoxy (Vulkan off);
# no portals or D-Bus services.  Off, and why:
#   x11, broadway, win32, macos, android  other window systems
#   media-gstreamer, print-cups/cpdb, cloudproviders, sysprof, tracker,
#   colord, accesskit                     services zedBSD does not have
#   introspection                         runs target programs in the build
#   documentation, screenshots, man pages, tests, testsuite, examples
# The demos (gtk4-demo, gtk4-widget-factory) stay: WS115 p010 runs them.
# HarfBuzz is in the dependency closure, and with it the C++ runtime package
# whose headers HarfBuzz compiled against (WS115 p006).
ZEDBSD_EXT_gtk4_MAKEFILE := $(ZEDBSD_GTK4_MAKEFILE)
ZEDBSD_EXT_gtk4_DEPENDS := glib cairo pango harfbuzz fribidi fontconfig freetype gdk-pixbuf \
	libpng libtiff libjpeg-turbo libepoxy libxkbcommon graphene wayland-client wayland-protocols
ZEDBSD_EXT_gtk4_HOST_TOOLS :=
ZEDBSD_EXT_gtk4_MESON_OPTIONS := \
	-Dx11-backend=false -Dwayland-backend=true -Dbroadway-backend=false \
	-Dwin32-backend=false -Dmacos-backend=false -Dandroid-backend=false \
	-Dmedia-gstreamer=disabled -Dprint-cpdb=disabled -Dprint-cups=disabled \
	-Dvulkan=disabled -Dcloudproviders=disabled -Dsysprof=disabled \
	-Dtracker=disabled -Dcolord=disabled -Daccesskit=disabled \
	-Dintrospection=disabled -Ddocumentation=false -Dscreenshots=false -Dman-pages=false \
	-Dbuild-demos=true -Dbuild-testsuite=false -Dbuild-examples=false -Dbuild-tests=false

$(eval $(call ZEDBSD_EXTERNAL_MESON,gtk4))

ZEDBSD_GTK4_STAGED := $(ZEDBSD_EXT_gtk4_STAGEDIR)/.zedbsd-staged
ZEDBSD_GTK4_CHECKED := $(ZEDBSD_EXT_gtk4_STAGEDIR)/.zedbsd-checked
ZEDBSD_GTK4_SONAME := libgtk-4.so.1
ZEDBSD_GTK4_LIBRARY := $(ZEDBSD_EXT_gtk4_STAGEDIR)/usr/lib/libgtk-4.so.1.1800.6
ZEDBSD_GTK4_LICENSE := $(ZEDBSD_EXT_gtk4_SRCDIR)/COPYING
ZEDBSD_GTK4_SCHEMA_DIR := $(ZEDBSD_EXT_gtk4_STAGEDIR)/usr/share/glib-2.0/schemas
ZEDBSD_GTK4_SCHEMAS := $(ZEDBSD_EXT_gtk4_WORKDIR)/schemas/gschemas.compiled

# The library's dependency contract: a change in what upstream links shows
# up here rather than on the target.
ZEDBSD_GTK4_NEEDED := libgmodule-2.0.so.0 libglib-2.0.so.0 libgobject-2.0.so.0 \
	libgio-2.0.so.0 libpangocairo-1.0.so.0 libpango-1.0.so.0 libharfbuzz.so.0 \
	libcairo.so.2 libharfbuzz-subset.so.0 libfribidi.so.0 libcairo-gobject.so.2 \
	libgdk_pixbuf-2.0.so.0 libepoxy.so.0 libgraphene-1.0.so.0 libpangoft2-1.0.so.0 \
	libfontconfig.so.1 libpng16.so.16 libtiff.so.6 libjpeg.so.62 libxkbcommon.so.0 \
	libwayland-client.so libwayland-egl.so libcairo-script-interpreter.so.2 libc.so

# The programs the image carries, with what each links: the two demos p010
# runs and the application gtk4-demo starts for its application demo.
ZEDBSD_GTK4_PROGRAMS := gtk4-demo gtk4-demo-application gtk4-widget-factory
ZEDBSD_GTK4_NEEDED_gtk4-demo := libgtk-4.so.1 libglib-2.0.so.0 libgobject-2.0.so.0 \
	libgio-2.0.so.0 libpangocairo-1.0.so.0 libpango-1.0.so.0 libharfbuzz.so.0 libcairo.so.2 \
	libgdk_pixbuf-2.0.so.0 libepoxy.so.0 libgraphene-1.0.so.0 libc.so
ZEDBSD_GTK4_NEEDED_gtk4-demo-application := libgtk-4.so.1 libglib-2.0.so.0 \
	libgobject-2.0.so.0 libgio-2.0.so.0 libc.so
ZEDBSD_GTK4_NEEDED_gtk4-widget-factory := libgtk-4.so.1 libglib-2.0.so.0 \
	libgobject-2.0.so.0 libgio-2.0.so.0 libpango-1.0.so.0 libgdk_pixbuf-2.0.so.0 libc.so

# Checks the library and the programs against their contracts.
$(ZEDBSD_GTK4_CHECKED): $(ZEDBSD_GTK4_STAGED)
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role shared-library \
		--soname $(ZEDBSD_GTK4_SONAME) \
		$(foreach needed,$(ZEDBSD_GTK4_NEEDED),--needed $(needed)) \
		'$(ZEDBSD_GTK4_LIBRARY)'
	@set -e; $(foreach program,$(ZEDBSD_GTK4_PROGRAMS),\
		$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role application \
		$(foreach needed,$(ZEDBSD_GTK4_NEEDED_$(program)),--needed $(needed)) \
		'$(ZEDBSD_EXT_gtk4_STAGEDIR)/usr/bin/$(program)';)
	@touch '$@'

# The settings schemas, compiled by the build machine's glib-compile-schemas
# (the same 2.84 release as the target's GLib) into the one file GSettings
# reads.  Nothing else on the image installs schemas yet.
$(ZEDBSD_GTK4_SCHEMAS): $(ZEDBSD_GTK4_STAGED)
	@mkdir -p '$(dir $@)'
	glib-compile-schemas --strict --targetdir='$(dir $@)' '$(ZEDBSD_GTK4_SCHEMA_DIR)'

$(ZEDBSD_GTK4_LICENSE): $(ZEDBSD_EXT_gtk4_SRCSTAMP)
	@test -f '$@'

.PHONY: gtk4
gtk4: $(ZEDBSD_GTK4_CHECKED) $(ZEDBSD_GTK4_SCHEMAS)
	@:

# ---------------------------------------------------------------- packaging

# The library under its SONAME, the demo programs, the compiled schemas and
# the licence.  GTK's own data (icons, CSS, emoji) is compiled into the
# library as GResources.  The headers and the .pc files are the development
# files below; the other tools are not installed.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,gtk4,GTK 4 toolkit,amd64,n,package,,y,\
	packages/desktop,desktop/gtk4,libs/glib libs/cairo libs/pango libs/harfbuzz libs/fribidi \
	libs/fontconfig libs/freetype libs/gdk-pixbuf libs/libpng libs/libtiff libs/libjpeg-turbo \
	libs/libepoxy libs/libxkbcommon libs/xkeyboard-config libs/graphene desktop/libwayland))

ZEDBSD_PACKAGE_INPUTS += $(if $(filter gtk4,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_GTK4_CHECKED) $(ZEDBSD_GTK4_SCHEMAS) $(ZEDBSD_GTK4_LICENSE))
ZEDBSD_PACKAGE_FILES += $(if $(filter gtk4,$(ZEDBSD_USER_PROGRAMS)),\
	--file /usr/lib/$(ZEDBSD_GTK4_SONAME)=$(ZEDBSD_GTK4_LIBRARY) \
	$(foreach program,$(ZEDBSD_GTK4_PROGRAMS),\
		--file $(ZEDBSD_PACKAGE_BINDIR)/$(program)=$(ZEDBSD_EXT_gtk4_STAGEDIR)/usr/bin/$(program) \
		--mode $(ZEDBSD_PACKAGE_BINDIR)/$(program)=0755) \
	--file /usr/share/glib-2.0/schemas/gschemas.compiled=$(ZEDBSD_GTK4_SCHEMAS) \
	--file /usr/share/licenses/gtk4/COPYING=$(ZEDBSD_GTK4_LICENSE))

# The development files: the headers, the .pc files and the unversioned names
# of the libraries (external.mk).
$(eval $(call ZEDBSD_EXTERNAL_DEVELOPMENT,gtk4,$(ZEDBSD_EXT_gtk4_STAGEDIR),\
	$(addprefix lib/pkgconfig/,gtk4.pc gtk4-wayland.pc gtk4-unix-print.pc),\
	include/gtk-4.0,\
	$(ZEDBSD_GTK4_SONAME)))
