include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# libmedia (ws121-p002): the playing of media files the desktop's programs share, first the browser's <video> and
# <audio> -- the container reader (userland/desktop/mediafile), Video Player's decoding add-in that opens FFmpeg's
# libavcodec with dlopen when the system has it (the libavcodec package, LGPL; built without FFmpeg's headers and not
# linked to it), and the engine that plays one file on a thread of its own (media.h), without sound (WS191: it may not
# reach Keiland's sound streams, which libbrowser may not).  Its NEEDED is the C library alone.  amd64 zedBSD.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
LIBMEDIA_SOURCES := userland/desktop/libmedia/engine.c \
	userland/desktop/videoplayer/codec.c userland/desktop/videoplayer/bitstream.c \
	userland/desktop/mediafile/mediafile.c userland/desktop/mediafile/mp4.c userland/desktop/mediafile/mkv.c \
	userland/desktop/mediafile/ts.c
LIBMEDIA_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libmedia.so=$(BUILD)/dynamic/libmedia.so)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libmedia,Media playing library,amd64,n,library,$(LIBMEDIA_SOURCES),y,desktop,desktop/libmedia,,,shared-library,$(LIBMEDIA_IMAGE_DATA),,lib))
