# zedBSD userland package: GTK 4 (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# GTK is LGPL-2.1-or-later and stays outside the source tree: only this
# Makefile, the recorded archive identity and the patches live here.  This is
# the plain upstream GTK 4 ported to zedBSD (WS115), to find where a port
# needs work; the knowledge goes to WS097's own toolkit.  The choice of 4.18.6
# and every patch's reason are in plan/ws115/port-contract.md (§1, §11).
GTK4_MAKEFILE := $(lastword $(MAKEFILE_LIST))
GTK4_DIR := $(patsubst %/,%,$(dir $(GTK4_MAKEFILE)))
include $(GTK4_DIR)/../../../base/package.mk
include $(GTK4_DIR)/../../external.mk

# 4.18.6 is the last release of 4.18, the series measured on Linux in WS114.
# Size and digest are the upstream bytes as measured; they match the
# published .sha256sum.
EXT_gtk4_VERSION := 4.18.6
EXT_gtk4_ROOT := gtk-4.18.6
EXT_gtk4_ARCHIVE := gtk-4.18.6.tar.xz
EXT_gtk4_URL := https://download.gnome.org/sources/gtk/4.18/gtk-4.18.6.tar.xz
EXT_gtk4_SIZE := 17710412
EXT_gtk4_SHA256 := e1817c650ddc3261f9a8345b3b22a26a5d80af154630dedc03cc7becefffd0fa
EXT_gtk4_PATCH_LEVEL := zedbsd3
EXT_gtk4_PATCHES := \
	$(GTK4_DIR)/patches/0001-round-without-fenv-rounding-modes.patch \
	$(GTK4_DIR)/patches/0002-use-guint-for-the-bsd-uint-type.patch \
	$(GTK4_DIR)/patches/0003-use-setjmp-with-libpng-jmpbuf-on-zedbsd.patch \
	$(GTK4_DIR)/patches/0004-leave-out-malloc-h-on-zedbsd.patch \
	$(GTK4_DIR)/patches/0005-report-missing-session-bus-as-a11y-debug.patch

$(eval $(call EXTERNAL_SOURCE,gtk4))

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
EXT_gtk4_MAKEFILE := $(GTK4_MAKEFILE)
EXT_gtk4_DEPENDS := glib cairo pango harfbuzz fribidi fontconfig freetype gdk-pixbuf \
	libpng libtiff libjpeg-turbo libepoxy libxkbcommon graphene wayland-client wayland-protocols
EXT_gtk4_HOST_TOOLS :=
EXT_gtk4_MESON_OPTIONS := \
	-Dx11-backend=false -Dwayland-backend=true -Dbroadway-backend=false \
	-Dwin32-backend=false -Dmacos-backend=false -Dandroid-backend=false \
	-Dmedia-gstreamer=disabled -Dprint-cpdb=disabled -Dprint-cups=disabled \
	-Dvulkan=disabled -Dcloudproviders=disabled -Dsysprof=disabled \
	-Dtracker=disabled -Dcolord=disabled -Daccesskit=disabled \
	-Dintrospection=disabled -Ddocumentation=false -Dscreenshots=false -Dman-pages=false \
	-Dbuild-demos=true -Dbuild-testsuite=false -Dbuild-examples=false -Dbuild-tests=false

$(eval $(call EXTERNAL_MESON,gtk4))

GTK4_STAGED := $(EXT_gtk4_STAGEDIR)/.zedbsd-staged
GTK4_CHECKED := $(EXT_gtk4_STAGEDIR)/.zedbsd-checked
GTK4_SONAME := libgtk-4.so.1
GTK4_LIBRARY := $(EXT_gtk4_STAGEDIR)/usr/lib/libgtk-4.so.1.1800.6
GTK4_LICENSE := $(EXT_gtk4_SRCDIR)/COPYING
GTK4_SCHEMA_DIR := $(EXT_gtk4_STAGEDIR)/usr/share/glib-2.0/schemas
GTK4_SCHEMAS := $(EXT_gtk4_WORKDIR)/schemas/gschemas.compiled

# The library's dependency contract: a change in what upstream links shows
# up here rather than on the target.
GTK4_NEEDED := libgmodule-2.0.so.0 libglib-2.0.so.0 libgobject-2.0.so.0 \
	libgio-2.0.so.0 libpangocairo-1.0.so.0 libpango-1.0.so.0 libharfbuzz.so.0 \
	libcairo.so.2 libharfbuzz-subset.so.0 libfribidi.so.0 libcairo-gobject.so.2 \
	libgdk_pixbuf-2.0.so.0 libepoxy.so.0 libgraphene-1.0.so.0 libpangoft2-1.0.so.0 \
	libfontconfig.so.1 libpng16.so.16 libtiff.so.6 libjpeg.so.62 libxkbcommon.so.0 \
	libwayland-client.so libwayland-egl.so libcairo-script-interpreter.so.2 libc.so

# The programs the image carries, with what each links: the two demos p010
# runs and the application gtk4-demo starts for its application demo.
GTK4_PROGRAMS := gtk4-demo gtk4-demo-application gtk4-widget-factory
GTK4_NEEDED_gtk4-demo := libgtk-4.so.1 libglib-2.0.so.0 libgobject-2.0.so.0 \
	libgio-2.0.so.0 libpangocairo-1.0.so.0 libpango-1.0.so.0 libharfbuzz.so.0 libcairo.so.2 \
	libgdk_pixbuf-2.0.so.0 libepoxy.so.0 libgraphene-1.0.so.0 libc.so
GTK4_NEEDED_gtk4-demo-application := libgtk-4.so.1 libglib-2.0.so.0 \
	libgobject-2.0.so.0 libgio-2.0.so.0 libc.so
GTK4_NEEDED_gtk4-widget-factory := libgtk-4.so.1 libglib-2.0.so.0 \
	libgobject-2.0.so.0 libgio-2.0.so.0 libpango-1.0.so.0 libgdk_pixbuf-2.0.so.0 libc.so

# Checks the library and the programs against their contracts.
$(GTK4_CHECKED): $(GTK4_STAGED)
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname $(GTK4_SONAME) \
		$(foreach needed,$(GTK4_NEEDED),--needed $(needed)) \
		'$(GTK4_LIBRARY)'
	@set -e; $(foreach program,$(GTK4_PROGRAMS),\
		$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role application \
		$(foreach needed,$(GTK4_NEEDED_$(program)),--needed $(needed)) \
		'$(EXT_gtk4_STAGEDIR)/usr/bin/$(program)';)
	@touch '$@'

# The settings schemas, compiled by the build machine's glib-compile-schemas
# (the same 2.84 release as the target's GLib) into the one file GSettings
# reads.  Nothing else on the image installs schemas yet.
$(GTK4_SCHEMAS): $(GTK4_STAGED)
	@mkdir -p '$(dir $@)'
	glib-compile-schemas --strict --targetdir='$(dir $@)' '$(GTK4_SCHEMA_DIR)'

$(GTK4_LICENSE): $(EXT_gtk4_SRCSTAMP)
	@test -f '$@'

.PHONY: gtk4
gtk4: $(GTK4_CHECKED) $(GTK4_SCHEMAS)
	@:

# ---------------------------------------------------------------- packaging

# The library under its SONAME, the demo programs, the compiled schemas and
# the licence.  GTK's own data (icons, CSS, emoji) is compiled into the
# library as GResources.  The headers and the .pc files are the development
# files below; the other tools are not installed.
$(eval $(call USERLAND_PACKAGE,gtk4,GTK 4 toolkit,x86_64,n,package,,y,\
	packages/desktop,desktop/gtk4,libs/glib libs/cairo libs/pango libs/harfbuzz libs/fribidi \
	libs/fontconfig libs/freetype libs/gdk-pixbuf libs/libpng libs/libtiff libs/libjpeg-turbo \
	libs/libepoxy libs/libxkbcommon libs/xkeyboard-config libs/graphene desktop/libwayland))

PACKAGE_INPUTS += $(if $(filter gtk4,$(USER_PROGRAMS)),\
	$(GTK4_CHECKED) $(GTK4_SCHEMAS) $(GTK4_LICENSE))
PACKAGE_FILES += $(if $(filter gtk4,$(USER_PROGRAMS)),\
	--file /usr/lib/$(GTK4_SONAME)=$(GTK4_LIBRARY) \
	$(foreach program,$(GTK4_PROGRAMS),\
		--file $(PACKAGE_BINDIR)/$(program)=$(EXT_gtk4_STAGEDIR)/usr/bin/$(program) \
		--mode $(PACKAGE_BINDIR)/$(program)=0755) \
	--file /usr/share/glib-2.0/schemas/gschemas.compiled=$(GTK4_SCHEMAS) \
	--file /usr/share/licenses/gtk4/COPYING=$(GTK4_LICENSE))

# The development files: the headers, the .pc files and the unversioned names
# of the libraries (external.mk).
$(eval $(call EXTERNAL_DEVELOPMENT,gtk4,$(EXT_gtk4_STAGEDIR),\
	$(addprefix lib/pkgconfig/,gtk4.pc gtk4-wayland.pc gtk4-unix-print.pc),\
	include/gtk-4.0,\
	$(GTK4_SONAME)))
