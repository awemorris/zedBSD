# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk

LIBKEILAND_MAKEFILE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))

include $(LIBKEILAND_MAKEFILE_DIR)../package.mk

LIBKEILAND_IMAGE_DATA := $(if $(TOPLEVEL_BUILD),/lib/libkeiland.so=$(BUILD)/dynamic/libkeiland.so)

LIBKEILAND_CATALOGS := $(sort $(wildcard userland/desktop/locale/*/*.tr))

LIBKEILAND_IMAGE_DATA += $(foreach catalog,$(LIBKEILAND_CATALOGS),/usr/share/keiland/locale/$(patsubst userland/desktop/locale/%,%,$(catalog))=$(catalog))

$(eval $(call USERLAND_PACKAGE,libkeiland,libkeiland,x86_64,y,library,$(LIBKEILAND_SOURCES),y,desktop,desktop/libkeiland,desktop/libwayland desktop/libtruetype desktop/libvulkan desktop/libpng-compat base/libz-compat,,shared-library,$(LIBKEILAND_IMAGE_DATA),,lib))
