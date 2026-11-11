# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

VIDEOPLAYER_SOURCES := \
	userland/desktop/videoplayer/main.c \
	userland/desktop/videoplayer/media.c \
	userland/desktop/videoplayer/audio.c \
	userland/desktop/videoplayer/present.c

$(eval $(call USERLAND_PACKAGE,videoplayer,videoplayer - Video Player,x86_64,n,basic,$(VIDEOPLAYER_SOURCES) userland/desktop/media-app/decoder.c userland/desktop/media-app/avcodec.c userland/desktop/media-app/bitstream.c,y,desktop,desktop/videoplayer,desktop/libmedia desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype,0755,program,,,bin))
