# The source list of libmedia, read by its Makefile and apps/browser-win32/Makefile.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
LIBMEDIA_SOURCES := userland/desktop/libmedia/engine.c userland/desktop/libmedia/decoder.c \
	userland/desktop/libmedia/bitstream.c userland/desktop/libmedia/picture.c \
	userland/desktop/libmedia/h264.c userland/desktop/libmedia/h264-dpb.c \
	userland/desktop/libmedia/vkvideo.c userland/desktop/libmedia/vkvideo-runtime.c \
	userland/desktop/libmedia/bits.c userland/desktop/libmedia/aac-input.c \
	userland/desktop/libmedia/aac-huffman.c userland/desktop/libmedia/aac-codec.c \
	userland/desktop/libmedia/aac-bands.c userland/desktop/libmedia/aac-frame.c \
	userland/desktop/libmedia/aac-synth.c userland/desktop/libmedia/aac.c userland/desktop/libmedia/sound.c \
	userland/desktop/libmedia/sbc.c userland/desktop/libmedia/aac-encode.c \
	userland/desktop/libmedia/vcodec-bitreader.c userland/desktop/libmedia/vcodec-nal.c \
	userland/desktop/libmedia/vcodec-cabac.c userland/desktop/libmedia/h264sw-nal.c \
	userland/desktop/libmedia/h264sw-tables.c userland/desktop/libmedia/h264sw-ps.c \
	userland/desktop/libmedia/h264sw-slice.c userland/desktop/libmedia/h264sw-frame.c \
	userland/desktop/libmedia/h264sw-poc.c userland/desktop/libmedia/h264sw-dpb.c \
	userland/desktop/libmedia/h264sw-reflist.c userland/desktop/libmedia/h264sw-mb.c \
	userland/desktop/libmedia/h264sw-cavlc.c userland/desktop/libmedia/h264sw-cabac.c \
	userland/desktop/libmedia/h264sw-cabac-init.c \
	userland/desktop/libmedia/h264sw-cabac-se.c userland/desktop/libmedia/h264sw-macroblock.c \
	userland/desktop/libmedia/h264sw-transform.c userland/desktop/libmedia/h264sw-intra.c \
	userland/desktop/libmedia/h264sw-mvpred.c userland/desktop/libmedia/h264sw-direct.c \
	userland/desktop/libmedia/h264sw-interp.c userland/desktop/libmedia/h264sw-weight.c \
	userland/desktop/libmedia/h264sw-recon.c userland/desktop/libmedia/h264sw-deblock.c \
	userland/desktop/libmedia/h264sw-decoder.c userland/desktop/libmedia/h264sw-backend.c \
	userland/desktop/mediafile/mediafile.c userland/desktop/mediafile/mp4.c userland/desktop/mediafile/mkv.c \
	userland/desktop/mediafile/ts.c userland/desktop/mediafile/ogg.c userland/desktop/mediafile/avi.c
