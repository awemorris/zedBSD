include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Music (WS120): Keiland's music player -- the songs of ~/Music (m4a: AAC in MP4) by album, with their covers, played
# through libkeiland's sound stream (WS191).  The container is read by mediafile (userland/desktop/mediafile) and the tags by Music's own reader
# (tags.c); the decoding is Video Player's add-in (userland/desktop/videoplayer/codec.c) that opens FFmpeg's
# libavcodec with dlopen when the system has it (the libavcodec package, LGPL), built without FFmpeg's headers and
# not linked to it; the sound goes through Video Player's sound stream (audio.c, libkeiland's kl_audio_stream_*).  The covers are decoded with
# libjpeg-compat and libpng-compat.  amd64 zedBSD.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
MUSIC_SOURCES := userland/desktop/music/main.c userland/desktop/music/view.c userland/desktop/music/play.c \
	userland/desktop/music/library.c userland/desktop/music/tags.c userland/desktop/music/cover.c \
	userland/desktop/picture/picture.c \
	userland/desktop/videoplayer/audio.c userland/desktop/videoplayer/codec.c \
	userland/desktop/videoplayer/bitstream.c userland/desktop/mediafile/mediafile.c \
	userland/desktop/mediafile/mp4.c userland/desktop/mediafile/mkv.c userland/desktop/mediafile/ts.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,music,Music,amd64,n,basic,$(MUSIC_SOURCES),y,desktop,desktop/music,desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype base/libz-compat base/libpng-compat base/libjpeg-compat base/libgif-compat,0755,program,,,bin))
