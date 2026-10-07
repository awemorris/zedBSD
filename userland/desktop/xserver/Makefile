include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# The compositor's X11 server: rootless, each top-level X window a window of the desktop (WS069 p008).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
KEILAND_X11SERVER_SOURCES := userland/desktop/xserver/main.c \
	userland/desktop/xserver/server.c userland/desktop/xserver/protocol.c \
	userland/desktop/xserver/window.c userland/desktop/xserver/rootless.c \
	userland/desktop/xserver/wayland.c userland/desktop/xserver/keymap.c \
	userland/desktop/xserver/glyphs.c userland/desktop/xserver/glx.c \
	userland/desktop/xserver/vulkan.c userland/desktop/xserver/selection.c
# GLX (WS178): libGLX.so, one of the server's libraries, which X programs link with libGL (-lGL -lGLX);
# a private libX11 is built in.  The image carries it whenever the server is selected.
KEILAND_LIBGLX_SOURCES := userland/desktop/xserver/libGLX/glx.c userland/x11/libX11/xlib.c
KEILAND_X11SERVER_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libGLX.so=$(BUILD)/dynamic/libGLX.so)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,xserver,X11 server for the compositor,amd64,y,basic,$(KEILAND_X11SERVER_SOURCES),y,desktop,desktop/xserver,desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype desktop/libGL,0755,program,$(KEILAND_X11SERVER_IMAGE_DATA),,bin))
