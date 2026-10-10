include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Photos uses compositor media snapshots backed by mediastorage's JSON database in ~/Pictures/Media.
# JPEG/PNG/GIF originals remain ordinary files; thumbnail caches are disposable.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
PHOTOS_SOURCES := userland/desktop/photos/main.c userland/desktop/photos/view.c userland/desktop/photos/thumbs.c \
	userland/desktop/photos/decode.c userland/desktop/photos/library.c userland/desktop/photos/exif.c \
	userland/desktop/photos/snapshot.c \
	userland/desktop/photos/cache-folders.c userland/desktop/picture/picture.c userland/desktop/picture/png-write.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,photos,Photos,amd64,n,basic,$(PHOTOS_SOURCES),y,desktop,desktop/photos,desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype desktop/mediastorage base/libz-compat base/libpng-compat base/libjpeg-compat base/libgif-compat,0755,program,,,bin))
