include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

# libmedia (ws121-p002, ws177-p031): the playing of media files the desktop's programs share -- the browser's <video>
# and <audio>, Video Player and Music: the container reader (userland/desktop/mediafile), the decoders (decoder.c, a
# table of back ends; today the add-in avcodec.c that opens FFmpeg's
# libavcodec with dlopen when the system has it (the libavcodec package, LGPL; built without FFmpeg's headers and not
# linked to it)), and the engine that plays one file on a thread of its own (media.h), without sound (WS191: it may not
# reach Keiland's sound streams, which libbrowser may not).  Its NEEDED is the C library alone.  amd64 zedBSD.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# WS202 private bit/AAC input helpers are linked normally; the native decoder and app adapter migration follow.
LIBMEDIA_SOURCES := userland/desktop/libmedia/engine.c userland/desktop/libmedia/decoder.c \
	userland/desktop/libmedia/avcodec.c userland/desktop/libmedia/bitstream.c \
	userland/desktop/libmedia/bits.c userland/desktop/libmedia/aac-input.c \
	userland/desktop/libmedia/aac-huffman.c userland/desktop/libmedia/aac-codec.c \
	userland/desktop/mediafile/mediafile.c userland/desktop/mediafile/mp4.c userland/desktop/mediafile/mkv.c \
	userland/desktop/mediafile/ts.c userland/desktop/mediafile/ogg.c userland/desktop/mediafile/avi.c
LIBMEDIA_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libmedia.so=$(BUILD)/dynamic/libmedia.so)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libmedia,Media playing library,amd64,n,library,$(LIBMEDIA_SOURCES),y,desktop,desktop/libmedia,,,shared-library,$(LIBMEDIA_IMAGE_DATA),,lib))
