include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# VT100 terminal in a Wayland window, drawn with Vulkan; glyphs from libtruetype.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ZDESKTOP_TERMINAL_SOURCES := userland/base/zdesktop-terminal/main.c \
	userland/base/zdesktop-terminal/screen.c userland/base/zdesktop-terminal/keys.c \
	userland/base/zdesktop-terminal/font.c userland/base/zdesktop-terminal/render.c \
	userland/base/zdesktop-terminal/window.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,zdesktop-terminal,Wayland terminal,amd64,y,basic,$(ZDESKTOP_TERMINAL_SOURCES),y,base,base/zdesktop-terminal,base/libvulkan base/libwayland base/libtruetype,0755,program,,,bin))
