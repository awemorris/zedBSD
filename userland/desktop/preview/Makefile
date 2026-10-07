include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# keiland-preview (WS168 p003): makes the preview of a picture or a PDF from fd 0 into fd 1, started by Files and
# Settings with sandbox_spawn (plan/ws168/phase001/phase.md).  A static program (the class static: the sandbox opens
# no file, so it cannot load a shared library), with the decoders compiled in: libz-compat, libpng-compat,
# libjpeg-compat, libgif-compat through userland/desktop/picture, libtruetype and libpdf (built without substitute
# font files, PDF_FONT_FILES 0) and the C library's mathematics (both in platform/amd64/vmunix.mk).  amd64 zedBSD.
# fonts.c carries the desktop's Mahora (sans, bold, mono) in the program and gives it to libpdf in memory (ws177-p010).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
KEILAND_PREVIEW_SOURCES := userland/desktop/preview/main.c userland/desktop/preview/make.c userland/desktop/preview/decode.c \
	userland/desktop/preview/scale.c userland/desktop/preview/fonts.c userland/desktop/preview/zedbsd/confine.c userland/desktop/picture/picture.c \
	$(wildcard userland/base/libpdf/*.c) $(wildcard userland/desktop/libtruetype/*.c) \
	$(wildcard userland/base/libz-compat/*.c) $(wildcard userland/base/libpng-compat/*.c) \
	$(wildcard userland/base/libjpeg-compat/*.c) $(wildcard userland/base/libgif-compat/*.c)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,keiland-preview,Preview maker,amd64,n,static,$(KEILAND_PREVIEW_SOURCES),y,desktop,desktop/preview,,0755,program,,,usr/libexec))
