# zedBSD userland package: graphene (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# graphene is MIT-licensed and stays outside the source tree: only this
# Makefile and the recorded archive identity live here.  It is the vector and
# matrix library GTK's scene graph (GSK) is written with, for the GTK port
# (WS115).  The same release was built in WS115 p004 as the test of the Meson
# cross contract (plan/ws115/tests/meson-package-probe.mk), without its
# GObject types; this package has them.  Provenance is in
# plan/ws034/package-inventory.md and plan/ws115/port-contract.md.
GRAPHENE_MAKEFILE := $(lastword $(MAKEFILE_LIST))
GRAPHENE_DIR := $(patsubst %/,%,$(dir $(GRAPHENE_MAKEFILE)))
include $(GRAPHENE_DIR)/../../../base/package.mk
include $(GRAPHENE_DIR)/../../external.mk

# 1.10.8 is the latest release (2022).  Size and digest are the upstream
# bytes as measured; they match the digest GNOME publishes.
EXT_graphene_VERSION := 1.10.8
EXT_graphene_ROOT := graphene-1.10.8
EXT_graphene_ARCHIVE := graphene-1.10.8.tar.xz
EXT_graphene_URL := https://download.gnome.org/sources/graphene/1.10/graphene-1.10.8.tar.xz
EXT_graphene_SIZE := 333924
EXT_graphene_SHA256 := a37bb0e78a419dcbeaa9c7027bcff52f5ec2367c25ec859da31dfde2928f279a
EXT_graphene_PATCH_LEVEL := zedbsd0
EXT_graphene_PATCHES :=

$(eval $(call EXTERNAL_SOURCE,graphene))

# ---------------------------------------------------------------- build

# Meson, through the common rule, with the GObject types GTK requires
# (graphene-gobject-1.0) and the SSE2 and GCC vector paths (every x86-64 has
# SSE2).  Introspection, the documentation and the tests are off.
EXT_graphene_MAKEFILE := $(GRAPHENE_MAKEFILE)
EXT_graphene_DEPENDS := glib
EXT_graphene_HOST_TOOLS :=
EXT_graphene_MESON_OPTIONS := \
	-Dgobject_types=true -Dsse2=true -Dgcc_vector=true -Darm_neon=false \
	-Dintrospection=disabled -Dtests=false -Dinstalled_tests=false -Dgtk_doc=false

$(eval $(call EXTERNAL_MESON,graphene))

GRAPHENE_STAGED := $(EXT_graphene_STAGEDIR)/.zedbsd-staged
GRAPHENE_CHECKED := $(EXT_graphene_STAGEDIR)/.zedbsd-checked
GRAPHENE_SONAME := libgraphene-1.0.so.0
GRAPHENE_LIBRARY := $(EXT_graphene_STAGEDIR)/usr/lib/libgraphene-1.0.so.0.1000.8
GRAPHENE_LICENSE := $(EXT_graphene_SRCDIR)/LICENSE.txt

# Checks the library against its dependency contract once the stage exists.
$(GRAPHENE_CHECKED): $(GRAPHENE_STAGED)
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role shared-library \
		--soname $(GRAPHENE_SONAME) \
		--needed libgobject-2.0.so.0 --needed libglib-2.0.so.0 --needed libc.so \
		'$(GRAPHENE_LIBRARY)'
	@touch '$@'

$(GRAPHENE_LICENSE): $(EXT_graphene_SRCSTAMP)
	@test -f '$@'

.PHONY: graphene
graphene: $(GRAPHENE_CHECKED)
	@:

# ---------------------------------------------------------------- packaging

# The library goes in under its SONAME.  The headers and the .pc files are the
# development files below.
$(eval $(call USERLAND_PACKAGE,graphene,Vector and matrix library,*,n,package,,y,\
	packages/libs,libs/graphene,libs/glib))

PACKAGE_INPUTS += $(if $(filter graphene,$(USER_PROGRAMS)),\
	$(GRAPHENE_CHECKED) $(GRAPHENE_LICENSE))
PACKAGE_FILES += $(if $(filter graphene,$(USER_PROGRAMS)),\
	--file /usr/lib/$(GRAPHENE_SONAME)=$(GRAPHENE_LIBRARY) \
	--file /usr/share/licenses/graphene/LICENSE.txt=$(GRAPHENE_LICENSE))

# The development files: the headers, the .pc files and the unversioned names
# of the libraries (external.mk).
$(eval $(call EXTERNAL_DEVELOPMENT,graphene,$(EXT_graphene_STAGEDIR),\
	lib/pkgconfig/graphene-1.0.pc lib/pkgconfig/graphene-gobject-1.0.pc,\
	include/graphene-1.0 lib/graphene-1.0/include,\
	$(GRAPHENE_SONAME)))
