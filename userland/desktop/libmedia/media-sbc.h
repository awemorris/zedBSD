/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * SBC, the Bluetooth sub-band codec (ws205-p002): the encoder and the
 * decoder of A2DP's mandatory codec (A2DP 1.4.1, appendix B) and of the
 * mSBC frame of HFP's wide-band speech (16 kHz, mono, 15 blocks, 8
 * subbands, loudness, bitpool 26, syncword 0xAD).  Written from the
 * specification; it needs the C library alone (no other part of
 * libmedia), so that a program may also build it in.
 *
 * A frame holds blocks x subbands samples of each channel.  The encoder
 * takes them interleaved (16-bit, the channels of one instant together)
 * and writes one frame; the decoder reads one frame, tells its settings
 * and gives the samples back the same way.  A codec keeps the filters'
 * history between frames: one codec for one stream.
 */

#ifndef LIBMEDIA_MEDIA_SBC_H
#define LIBMEDIA_MEDIA_SBC_H

#include <stddef.h>
#include <stdint.h>

/* The channel modes (the frame's two bits). */
#define MEDIA_SBC_MONO		0U
#define MEDIA_SBC_DUAL		1U
#define MEDIA_SBC_STEREO	2U
#define MEDIA_SBC_JOINT		3U

/* The bit allocation methods. */
#define MEDIA_SBC_LOUDNESS	0U
#define MEDIA_SBC_SNR		1U

/* The longest frame (bytes), and the most samples of one channel in a frame. */
#define MEDIA_SBC_FRAME_MAX	520U
#define MEDIA_SBC_SAMPLES_MAX	128U

/* mSBC's frame: its bytes and its samples (one channel). */
#define MEDIA_SBC_MSBC_BYTES	57U
#define MEDIA_SBC_MSBC_SAMPLES	120U

/*
 * A stream's settings: the rate (16000, 32000, 44100 or 48000), the
 * blocks (4, 8, 12 or 16; 15 in mSBC), the channel mode, the allocation
 * method, the subbands (4 or 8), the bitpool (2 up to 16 x subbands for
 * one channel or two coded apart, 32 x subbands for stereo and joint
 * stereo, and 250 at most), and msbc (1: mSBC's frame, whose other
 * settings are then fixed).
 */
struct media_sbc_config {
	unsigned rate;
	unsigned blocks;
	unsigned channel_mode;
	unsigned allocation;
	unsigned subbands;
	unsigned bitpool;
	unsigned msbc;
};

/* One stream's encoder or decoder. */
struct media_sbc;

void media_sbc_msbc_config(struct media_sbc_config *config);
int media_sbc_check(const struct media_sbc_config *config);
size_t media_sbc_frame_bytes(const struct media_sbc_config *config);
unsigned media_sbc_channels(const struct media_sbc_config *config);
int media_sbc_encoder_create(const struct media_sbc_config *config, struct media_sbc **codec);
int media_sbc_encode(struct media_sbc *codec, const int16_t *pcm, uint8_t *frame, size_t size, size_t *length);
int media_sbc_decoder_create(struct media_sbc **codec);
int media_sbc_decode(struct media_sbc *codec, const uint8_t *frame, size_t size, size_t *used, int16_t *pcm, size_t capacity, size_t *count, struct media_sbc_config *config);
void media_sbc_destroy(struct media_sbc *codec);

#endif
