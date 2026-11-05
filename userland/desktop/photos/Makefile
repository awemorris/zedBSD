include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Photos uses compositor media snapshots backed by mediastorage's JSON database in ~/Pictures/Media.
# JPEG/PNG/GIF originals remain ordinary files; thumbnail caches are disposable.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
PHOTOS_SOURCES := userland/desktop/photos/main.c userland/desktop/photos/media-worker.c userland/desktop/photos/view.c userland/desktop/photos/thumbs.c \
	userland/desktop/photos/decode.c userland/desktop/photos/library.c userland/desktop/photos/exif.c \
	userland/desktop/photos/snapshot.c \
	userland/desktop/photos/cache-folders.c userland/desktop/picture/png-write.c \
	userland/desktop/preview/client.c userland/desktop/preview/zedbsd/spawn.c
# ws157-p015: the photos' pictures are decoded by keiland-preview in a sandbox, never in Photos' process.
$(eval $(call USERLAND_PACKAGE,photos,photos - Photos,amd64,n,basic,$(PHOTOS_SOURCES),y,desktop,desktop/photos,desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype desktop/mediastorage desktop/preview base/libz-compat,0755,program,,,bin))
