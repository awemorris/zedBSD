# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

KEILAND_TERMINAL_SOURCES := \
	userland/desktop/terminal/main.c \
	userland/desktop/terminal/run.c \
	userland/desktop/terminal/connection-process.c \
	userland/desktop/terminal/screen.c \
	userland/desktop/terminal/keys.c \
	userland/desktop/terminal/font.c \
	userland/desktop/terminal/render-build.c \
	userland/desktop/terminal/render.c \
	userland/desktop/terminal/display-vulkan.c \
	userland/desktop/terminal/window.c \
	userland/desktop/terminal/frame-wayland.c \
	userland/desktop/terminal/menu.c \
	userland/desktop/terminal/clipboard.c \
	userland/desktop/terminal/tabs.c \
	userland/desktop/terminal/primary.c \
	userland/desktop/terminal/touch.c \
	userland/desktop/terminal/width.c \
	userland/desktop/terminal/settings.c \
	userland/desktop/terminal/search.c \
	userland/desktop/picture/color-glyph.c

$(eval $(call USERLAND_PACKAGE,terminal,terminal - Wayland terminal,x86_64,y,basic,$(KEILAND_TERMINAL_SOURCES),y,desktop,desktop/terminal,desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype desktop/libpng-compat base/libz-compat,0755,program,,,bin))
