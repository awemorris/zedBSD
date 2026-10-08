include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Photos (WS157): Keiland's library of photos in ~/Pictures/Library (imported, a database split by month and by album,
# thumbnails in the cache) -- a grid by month, albums, favourites, a photo shown whole, a slideshow.  The dates come
# from the JPEGs' EXIF (Photos' own reader) or the files' times; the pictures are decoded with libjpeg-compat,
# libpng-compat and libgif-compat through the decoding Image Viewer and Files share (userland/desktop/picture), on a
# thread of their own.  amd64 zedBSD.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
PHOTOS_SOURCES := userland/desktop/photos/main.c userland/desktop/photos/view.c userland/desktop/photos/thumbs.c \
	userland/desktop/photos/decode.c userland/desktop/photos/library.c userland/desktop/photos/exif.c \
	userland/desktop/photos/import.c \
	userland/desktop/photos/db.c userland/desktop/picture/picture.c userland/desktop/picture/png-write.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,photos,Photos,amd64,n,basic,$(PHOTOS_SOURCES),y,desktop,desktop/photos,desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype base/libz-compat base/libpng-compat base/libjpeg-compat base/libgif-compat,0755,program,,,bin))
