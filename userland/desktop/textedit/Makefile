# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

TEXTEDIT_SOURCES := \
	userland/desktop/textedit/main.c \
	userland/desktop/textedit/queue.c \
	userland/desktop/textedit/menu.c \
	userland/desktop/textedit/glass.c \
	userland/desktop/textedit/app.c \
	userland/desktop/textedit/edit.c \
	userland/desktop/textedit/draw.c \
	userland/desktop/textedit/buffer.c \
	userland/desktop/textedit/undo.c \
	userland/desktop/textedit/file.c \
	userland/desktop/textedit/layout.c \
	userland/desktop/textedit/find.c \
	userland/desktop/textedit/canvas.c \
	userland/desktop/textedit/text.c \
	userland/desktop/picture/color-glyph.c

$(eval $(call USERLAND_PACKAGE,textedit,textedit - Text Editor,x86_64,y,basic,$(TEXTEDIT_SOURCES),y,desktop,desktop/textedit,desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype desktop/libpng-compat base/libz-compat,0755,program,,,bin))
