include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Text Editor (WS092, plan/ws092/design.md): plain UTF-8 text in a Wayland window, drawn on the CPU and
# shown with Vulkan through libkeiland's window (WS090); undo and redo, the clipboard and the primary selection,
# the menu bar in the titlebar, finding and replacing in panels (BUG-248), and the touch screen (one finger
# selects, two scroll with inertia).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
TEXTEDIT_SOURCES := userland/desktop/textedit/main.c userland/desktop/textedit/queue.c \
	userland/desktop/textedit/menu.c \
	userland/desktop/textedit/glass.c userland/desktop/textedit/app.c \
	userland/desktop/textedit/edit.c userland/desktop/textedit/draw.c \
	userland/desktop/textedit/buffer.c userland/desktop/textedit/undo.c \
	userland/desktop/textedit/file.c userland/desktop/textedit/layout.c \
	userland/desktop/textedit/find.c userland/desktop/textedit/canvas.c \
	userland/desktop/textedit/text.c userland/desktop/picture/color-glyph.c
$(eval $(call USERLAND_PACKAGE,textedit,textedit - Text Editor,amd64,y,basic,$(TEXTEDIT_SOURCES),y,desktop,desktop/textedit,desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype desktop/libpng-compat base/libz-compat,0755,program,,,bin))
