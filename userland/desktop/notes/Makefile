include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Notes, the handwritten notebook (plan/ws079/design-input-notes.md section 5): pages of pen strokes drawn with
# Vulkan in a Wayland window, saved as PDF with libpdf (vector outlines and the edit data), with a journal for recovery;
# ws175: the edits of the PDF's objects and the images (edit.c, compressed through libz-compat; the image files read by
# picture-file.c with picture.c's EXIF orientation and libpng-compat);
# the fingers scroll and zoom the page (ws081-p013).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
KEILAND_NOTES_SOURCES := userland/desktop/notes/main.c \
	userland/desktop/notes/document.c userland/desktop/notes/edit.c userland/desktop/notes/encode.c \
	userland/desktop/notes/journal.c userland/desktop/notes/save.c \
	userland/desktop/notes/geometry.c userland/desktop/notes/render.c \
	userland/desktop/notes/window.c userland/desktop/notes/menu.c \
	userland/desktop/notes/tablet.c userland/desktop/notes/ui.c \
	userland/desktop/notes/touch.c userland/desktop/notes/picture-file.c userland/desktop/notes/box.c userland/desktop/picture/picture.c userland/desktop/picture/png-write.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,notes,Handwritten notebook,amd64,y,basic,$(KEILAND_NOTES_SOURCES),y,desktop,desktop/notes,desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype base/libz-compat base/libpng-compat base/libjpeg-compat base/libgif-compat base/libpdf,0755,program,,,bin))
