# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

PHOTOS_SOURCES := \
	userland/desktop/photos/main.c \
	userland/desktop/photos/media-worker.c \
	userland/desktop/photos/view.c \
	userland/desktop/photos/thumbs.c \
	userland/desktop/photos/decode.c \
	userland/desktop/photos/library.c \
	userland/desktop/photos/exif.c \
	userland/desktop/photos/snapshot.c \
	userland/desktop/photos/cache-folders.c \
	userland/desktop/picture/png-write.c \
	userland/desktop/preview/client.c \
	userland/desktop/preview/zedbsd/spawn.c

$(eval $(call USERLAND_PACKAGE,photos,photos - Photos,x86_64,n,basic,$(PHOTOS_SOURCES),y,desktop,desktop/photos,desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype desktop/mediastorage desktop/preview base/libz-compat,0755,program,,,bin))
