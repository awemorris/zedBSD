include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Settings (WS089): the Kei desktop's settings, a Wayland window drawn on the CPU and shown with Vulkan, the list of
# pages and the page floating as glass cards; text from libtruetype.  The canvas, text and icons are libkeiland's
# (ws090-p009); the Kei mark (userland/desktop/artwork) is compiled in unchanged (plan/ws089/design.md).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
KEILAND_SETTINGS_SOURCES := userland/desktop/settings/main.c \
	userland/desktop/settings/window.c userland/desktop/settings/present.c \
	userland/desktop/settings/glass.c userland/desktop/settings/titlebar.c \
	userland/desktop/settings/menu.c userland/desktop/settings/ui.c \
	userland/desktop/settings/widgets.c userland/desktop/settings/glyphs.c \
	userland/desktop/settings/pages.c userland/desktop/settings/machine.c \
	userland/desktop/settings/page-home.c userland/desktop/settings/page-about.c userland/desktop/settings/welcome.c userland/desktop/settings/page-users.c userland/desktop/settings/page-users-admin.c userland/desktop/settings/page-users-pin.c userland/desktop/settings/page-users-keys.c userland/desktop/settings/page-users-password.c userland/desktop/settings/dialog.c \
	userland/desktop/settings/page-soon.c userland/desktop/settings/page-power.c userland/desktop/settings/page-notifications.c userland/desktop/settings/page-network.c userland/desktop/settings/page-wired.c userland/desktop/settings/wired.c userland/desktop/settings/page-storage.c userland/desktop/settings/page-sharing.c userland/desktop/settings/page-printers.c userland/desktop/settings/page-bluetooth.c userland/desktop/settings/page-languages.c userland/desktop/settings/storage.c userland/desktop/settings/storage-scan.c userland/desktop/settings/storage-trash.c \
	userland/desktop/settings/network.c userland/desktop/settings/search.c \
	userland/desktop/settings/look.c userland/desktop/settings/palette.c userland/desktop/settings/page-look.c userland/desktop/settings/page-display.c userland/desktop/settings/arrange.c userland/desktop/settings/page-input.c userland/desktop/settings/sound.c userland/desktop/settings/system.c \
	userland/desktop/artwork/mark.c userland/desktop/preview/client.c userland/desktop/preview/zedbsd/spawn.c
# ws168-p004: the wallpapers' tiles are made by keiland-preview in a sandbox; Settings links no decoder.
$(eval $(call ZEDBSD_USERLAND_PACKAGE,settings,Settings,amd64,y,basic,$(KEILAND_SETTINGS_SOURCES),y,desktop,desktop/settings,desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype desktop/preview,0755,program,,,bin))
