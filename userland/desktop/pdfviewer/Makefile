include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# PDF Viewer (ws079-p006): PDF documents in a Wayland window, interpreted and rasterized on the CPU by
# libpdf and shown with Vulkan; a continuous scroll and a page-at-a-time mode with swipes; the touch screen
# with inertia and two-finger zoom (ws081-p012); the window, its input and the frames' presenter are libkeiland's
# (ws090-p008).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
PDFVIEWER_SOURCES := userland/desktop/pdfviewer/main.c userland/desktop/pdfviewer/menu.c \
	userland/desktop/pdfviewer/titlebar.c userland/desktop/pdfviewer/view.c userland/desktop/pdfviewer/find.c \
	userland/desktop/pdfviewer/draw.c userland/desktop/pdfviewer/document.c \
	userland/desktop/pdfviewer/canvas.c \
	userland/desktop/pdfviewer/text.c userland/desktop/pdfviewer/touch.c userland/desktop/picture/png-write.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,pdfviewer,PDF Viewer,amd64,y,basic,$(PDFVIEWER_SOURCES),y,desktop,desktop/pdfviewer,desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype base/libpdf base/libz-compat,0755,program,,,bin))
