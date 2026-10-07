LIBKEILAND_MAKEFILE_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
include $(LIBKEILAND_MAKEFILE_DIR)../package.mk

# The wrapper of the compositor's own Wayland extensions (the System Menu, WS070),
# and an application's way to the desktop's system and settings through
# Keiland's system extension (kl_system_*, kl_settings_*; WS131 p011: the
# library reaches no daemon and no system file itself).  The file chooser of
# ws092-p003 moved to the widgets (ws090-p006, KEILAND_VERSION 16), which are
# part of this library since WS131 p012 (ui/).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
LIBKEILAND_SOURCES := userland/desktop/libkeiland/version.c userland/desktop/libkeiland/menu.c \
	userland/desktop/libkeiland/recent.c userland/desktop/libkeiland/titlebar.c \
	userland/desktop/libkeiland/glass.c userland/desktop/libkeiland/instance.c userland/desktop/libkeiland/appearance.c \
	userland/desktop/libkeiland/motion.c userland/desktop/libkeiland/scroll.c \
	userland/desktop/libkeiland/gesture.c \
	userland/desktop/libkeiland/desktop.c \
	userland/desktop/libkeiland/keyboard-inset.c userland/desktop/libkeiland/edit.c \
	userland/desktop/libkeiland/settings.c userland/desktop/libkeiland/settings-cache.c \
	userland/desktop/libkeiland/settings-app.c userland/desktop/settings-keys/settings-keys.c \
	userland/desktop/libkeiland/translate.c userland/desktop/libkeiland/translate-follow.c \
	userland/desktop/libkeiland/system/system.c userland/desktop/libkeiland/system/system-view.c \
	userland/desktop/libkeiland/system/system-protocol.c userland/desktop/libkeiland/system/system-monitor.c \
	userland/desktop/libkeiland/system/system-monitor-rate.c

# The desktop's shared widgets and controls (WS090; their own library until WS131 p012, named kl_ since p013,
# <keiland/keiland.h>): the drawing layer, the scroll and the input, the window, the widgets and the file chooser.
LIBKEILAND_SOURCES += \
	userland/desktop/libkeiland/ui/canvas.c \
	userland/desktop/libkeiland/ui/text.c \
	userland/desktop/libkeiland/ui/icons.c \
	userland/desktop/libkeiland/ui/icons-line.c \
	userland/desktop/libkeiland/ui/theme.c \
	userland/desktop/libkeiland/ui/input.c \
	userland/desktop/libkeiland/ui/scroll.c \
	userland/desktop/libkeiland/ui/scroll-bar.c \
	userland/desktop/libkeiland/ui/text-touch.c \
	userland/desktop/libkeiland/ui/ui.c \
	userland/desktop/libkeiland/ui/window.c \
	userland/desktop/libkeiland/ui/app.c \
	userland/desktop/libkeiland/ui/declare.c \
	userland/desktop/libkeiland/ui/globals.c \
	userland/desktop/libkeiland/ui/window-declare.c \
	userland/desktop/libkeiland/ui/present.c \
	userland/desktop/libkeiland/ui/present-copy.c \
	userland/desktop/libkeiland/ui/present-shm.c \
	userland/desktop/libkeiland/ui/clipboard.c \
	userland/desktop/libkeiland/ui/primary.c \
	userland/desktop/libkeiland/ui/text-input.c \
	userland/desktop/libkeiland/ui/text-area.c \
	userland/desktop/libkeiland/ui/tablet.c \
	userland/desktop/libkeiland/ui/edit.c \
	userland/desktop/libkeiland/ui/widgets.c \
	userland/desktop/libkeiland/ui/field.c \
	userland/desktop/libkeiland/ui/list.c \
	userland/desktop/libkeiland/ui/views.c \
	userland/desktop/libkeiland/ui/cards.c \
	userland/desktop/libkeiland/ui/chooser.c \
	userland/desktop/libkeiland/ui/chooser-model.c \
	userland/desktop/libkeiland/ui/chooser-view.c \
	userland/desktop/picture/color-glyph.c

LIBKEILAND_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libkeiland.so=$(BUILD)/dynamic/libkeiland.so)
# The catalogs of the desktop's translations (WS158, kl_tr_*): userland/desktop/locale/LANGUAGE/DOMAIN.tr, installed
# where the library reads them.
LIBKEILAND_CATALOGS := $(sort $(wildcard userland/desktop/locale/*/*.tr))
LIBKEILAND_IMAGE_DATA += $(foreach catalog,$(LIBKEILAND_CATALOGS),/usr/share/keiland/locale/$(patsubst userland/desktop/locale/%,%,$(catalog))=$(catalog))
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libkeiland,Desktop system library,amd64,y,library,$(LIBKEILAND_SOURCES),y,desktop,desktop/libkeiland,desktop/libwayland desktop/libtruetype desktop/libvulkan base/libpng-compat base/libz-compat,,shared-library,$(LIBKEILAND_IMAGE_DATA),,lib))
