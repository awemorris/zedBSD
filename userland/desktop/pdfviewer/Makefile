# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

PDFVIEWER_SOURCES := \
	userland/desktop/pdfviewer/main.c \
	userland/desktop/pdfviewer/menu.c \
	userland/desktop/pdfviewer/titlebar.c \
	userland/desktop/pdfviewer/bar.c \
	userland/desktop/pdfviewer/view.c \
	userland/desktop/pdfviewer/find.c \
	userland/desktop/pdfviewer/draw.c \
	userland/desktop/pdfviewer/document.c \
	userland/desktop/pdfviewer/canvas.c \
	userland/desktop/pdfviewer/text.c \
	userland/desktop/pdfviewer/touch.c \
	userland/desktop/picture/png-write.c

$(eval $(call USERLAND_PACKAGE,pdfviewer,pdfviewer - PDF Viewer,x86_64,y,basic,$(PDFVIEWER_SOURCES),y,desktop,desktop/pdfviewer,desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype desktop/libpdf base/libz-compat,0755,program,,,bin))
