# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

MUSIC_SOURCES := \
	userland/desktop/music/main.c \
	userland/desktop/music/view.c \
	userland/desktop/music/play.c \
	userland/desktop/music/library.c \
	userland/desktop/music/tags.c \
	userland/desktop/music/cover.c \
	userland/desktop/picture/picture.c \
	userland/desktop/videoplayer/audio.c

$(eval $(call USERLAND_PACKAGE,music,music - Music,x86_64,n,basic,$(MUSIC_SOURCES) userland/desktop/media-app/decoder.c userland/desktop/media-app/avcodec.c userland/desktop/media-app/bitstream.c,y,desktop,desktop/music,desktop/libmedia desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype base/libz-compat desktop/libpng-compat desktop/libjpeg-compat desktop/libgif-compat,0755,program,,,bin))
