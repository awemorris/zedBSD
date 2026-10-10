# libmedia: original AAC-LC and standard Vulkan Video H.264 decoding, container readers and a silent video engine.
# Optional software codecs and sound streams belong to applications; this library needs libc alone.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
LIBMEDIA_SOURCES := userland/desktop/libmedia/engine.c userland/desktop/libmedia/decoder.c \
	userland/desktop/libmedia/bitstream.c userland/desktop/libmedia/picture.c \
	userland/desktop/libmedia/h264.c userland/desktop/libmedia/h264-dpb.c \
	userland/desktop/libmedia/vkvideo.c userland/desktop/libmedia/vkvideo-runtime.c \
	userland/desktop/libmedia/bits.c userland/desktop/libmedia/aac-input.c \
	userland/desktop/libmedia/aac-huffman.c userland/desktop/libmedia/aac-codec.c \
	userland/desktop/libmedia/aac-bands.c userland/desktop/libmedia/aac-frame.c \
	userland/desktop/libmedia/aac-synth.c userland/desktop/libmedia/aac.c userland/desktop/libmedia/sound.c \
	userland/desktop/mediafile/mediafile.c userland/desktop/mediafile/mp4.c userland/desktop/mediafile/mkv.c \
	userland/desktop/mediafile/ts.c userland/desktop/mediafile/ogg.c userland/desktop/mediafile/avi.c
LIBMEDIA_IMAGE_DATA := $(if $(ZEDBSD_TOPLEVEL_BUILD),/lib/libmedia.so=$(BUILD)/dynamic/libmedia.so)
$(eval $(call ZEDBSD_USERLAND_PACKAGE,libmedia,Media playing library,amd64,n,library,$(LIBMEDIA_SOURCES),y,desktop,desktop/libmedia,,,shared-library,$(LIBMEDIA_IMAGE_DATA),,lib))
