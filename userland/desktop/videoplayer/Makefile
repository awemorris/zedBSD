include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# Video Player (WS122): opens, plays, pauses, stops and seeks a video file, in a libkeiland window; the sound goes to
# libkeiland's sound stream (WS191).  The container reader and the decoding are libmedia's (ws177-p031): mediafile, and
# the add-in that opens FFmpeg's libavcodec, libavutil and libswscale (the libavcodec package, LGPL) with dlopen when
# the system has them, built without FFmpeg's headers and not linked to it.  amd64 zedBSD.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
VIDEOPLAYER_SOURCES := userland/desktop/videoplayer/main.c userland/desktop/videoplayer/media.c \
	userland/desktop/videoplayer/audio.c
$(eval $(call ZEDBSD_USERLAND_PACKAGE,videoplayer,Video Player,amd64,n,basic,$(VIDEOPLAYER_SOURCES),y,desktop,desktop/videoplayer,desktop/libmedia desktop/libvulkan desktop/libwayland desktop/libkeiland desktop/libtruetype,0755,program,,,bin))
