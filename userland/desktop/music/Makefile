include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Music (WS120): Keiland's music player -- the songs of ~/Music (m4a: AAC in MP4) by album, with their covers, played
# through libkeiland's sound stream (WS191).  The container reader and the decoding are libmedia's (ws177-p031): mediafile,
# and the add-in that opens FFmpeg's libavcodec with dlopen when the system has it (the libavcodec package, LGPL), built
# without FFmpeg's headers and not linked to it; the tags are Music's own reader (tags.c); the sound goes through Video
# Player's sound stream (audio.c, libkeiland's kl_audio_stream_*).  The covers are decoded with
# libjpeg-compat and libpng-compat.  amd64 zedBSD.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
MUSIC_SOURCES := userland/desktop/music/main.c userland/desktop/music/view.c userland/desktop/music/play.c \
	userland/desktop/music/library.c userland/desktop/music/tags.c userland/desktop/music/cover.c \
	userland/desktop/picture/picture.c \
	userland/desktop/videoplayer/audio.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,music,Music,amd64,n,basic,$(MUSIC_SOURCES) userland/desktop/media-app/decoder.c userland/desktop/media-app/avcodec.c userland/desktop/media-app/bitstream.c,y,desktop,desktop/music,desktop/libmedia desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype base/libz-compat base/libpng-compat base/libjpeg-compat base/libgif-compat,0755,program,,,bin))
