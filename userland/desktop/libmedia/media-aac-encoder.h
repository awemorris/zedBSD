/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The AAC-LC encoder (ws205-p003): 1024 samples of each channel (one or
 * two, 32, 44.1 or 48 kHz) a frame into one raw access unit (a
 * raw_data_block: an SCE or a CPE and END), at a bit rate the caller sets
 * (at most 256 kbit/s, the A2DP headsets' AAC).  Written from ISO/IEC
 * 14496-3's syntax, with libmedia's decoder's normative tables (the
 * Huffman books and the scalefactor bands).
 *
 * What it uses (the user's decision of 2026-10-11, WS205 D3): long
 * windows with the sine shape, mid/side stereo per band, a simple
 * psychoacoustic threshold per band and a rate loop; no TNS, no PNS, no
 * short windows yet.  The first access unit carries the windows' first
 * half (1024 samples of delay, as every AAC encoder has).
 *
 * The caller wraps the units: media_aac_adts_header for an ADTS stream
 * (a file, the tests), media_aac_audio_specific_config for an MP4 or a
 * LATM stream (A2DP).
 */

#ifndef LIBMEDIA_MEDIA_AAC_ENCODER_H
#define LIBMEDIA_MEDIA_AAC_ENCODER_H

#include <stddef.h>
#include <stdint.h>

/* The samples of one channel a frame, the most bytes of a unit of two channels, and the highest bit rate. */
#define MEDIA_AAC_FRAME_SAMPLES	1024U
#define MEDIA_AAC_UNIT_MAX	1536U
#define MEDIA_AAC_BITRATE_MAX	256000U

/* The bytes of an ADTS header (without CRC) and of an AudioSpecificConfig. */
#define MEDIA_AAC_ADTS_BYTES	7U
#define MEDIA_AAC_CONFIG_BYTES	2U

/* One stream's encoder. */
struct media_aac_encoder;

int media_aac_encoder_create(unsigned rate, unsigned channels, unsigned bitrate, struct media_aac_encoder **encoder);
int media_aac_encode(struct media_aac_encoder *encoder, const int16_t *pcm, uint8_t *unit, size_t size, size_t *length);
int media_aac_adts_header(const struct media_aac_encoder *encoder, size_t length, uint8_t header[MEDIA_AAC_ADTS_BYTES]);
int media_aac_audio_specific_config(const struct media_aac_encoder *encoder, uint8_t config[MEDIA_AAC_CONFIG_BYTES]);
void media_aac_encoder_destroy(struct media_aac_encoder *encoder);

#endif
