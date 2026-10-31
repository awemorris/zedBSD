include $(dir $(lastword $(MAKEFILE_LIST)))sources.mk
LIBKEILAND_MAKEFILE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
include $(LIBKEILAND_MAKEFILE_DIR)../package.mk

# The wrapper of the compositor's own Wayland extensions (the System Menu, WS070),
# and an application's way to the desktop's system and settings through
# Keiland's system extension (kl_system_*, kl_settings_*; WS131 p011: the
# library reaches no daemon and no system file itself).  The file chooser of
# ws092-p003 moved to the widgets (ws090-p006, KEILAND_VERSION 16), which are
# part of this library since WS131 p012 (ui/).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

LIBKEILAND_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libkeiland.so=$(BUILD)/dynamic/libkeiland.so)
# The catalogs of the desktop's translations (WS158, kl_tr_*): userland/desktop/locale/LANGUAGE/DOMAIN.tr, installed
# where the library reads them.
LIBKEILAND_CATALOGS := $(sort $(wildcard userland/desktop/locale/*/*.tr))
LIBKEILAND_IMAGE_DATA += $(foreach catalog,$(LIBKEILAND_CATALOGS),/usr/share/keiland/locale/$(patsubst userland/desktop/locale/%,%,$(catalog))=$(catalog))
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libkeiland,libkeiland,amd64,y,library,$(LIBKEILAND_SOURCES),y,desktop,desktop/libkeiland,desktop/libwayland desktop/libtruetype desktop/libvulkan desktop/libpng-compat base/libz-compat,,shared-library,$(LIBKEILAND_IMAGE_DATA),,lib))
