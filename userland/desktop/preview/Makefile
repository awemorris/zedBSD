# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

KEILAND_PREVIEW_SOURCES := \
	userland/desktop/preview/main.c \
	userland/desktop/preview/make.c \
	userland/desktop/preview/decode.c \
	userland/desktop/preview/scale.c \
	userland/desktop/preview/fonts.c \
	userland/desktop/preview/zedbsd/confine.c \
	userland/desktop/picture/picture.c \
	userland/desktop/preview/info.c \
	userland/desktop/photos/exif.c \
	$(wildcard $(REPO_ROOT)/userland/desktop/libpdf/*.c) \
	$(wildcard $(REPO_ROOT)/userland/desktop/libtruetype/*.c) \
	$(wildcard $(REPO_ROOT)/userland/base/libz-compat/*.c) \
	$(wildcard $(REPO_ROOT)/userland/desktop/libpng-compat/*.c) \
	$(wildcard $(REPO_ROOT)/userland/desktop/libjpeg-compat/*.c) \
	$(wildcard $(REPO_ROOT)/userland/desktop/libgif-compat/*.c)

$(eval $(call USERLAND_PACKAGE,keiland-preview,keiland-preview - Preview maker,x86_64,n,static,$(KEILAND_PREVIEW_SOURCES),y,desktop,desktop/preview,,0755,program,,,usr/libexec))
