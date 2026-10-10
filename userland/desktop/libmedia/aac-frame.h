/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBMEDIA_AAC_FRAME_H
#define LIBMEDIA_AAC_FRAME_H
#include "aac-input.h"
#include "aac-bands.h"

#define MEDIA_AAC_BANDS_MAX 64U
#define MEDIA_AAC_TNS_ORDER 12U

/* One TNS region and its transmitted reflection-coefficient indices, owned by a frame. */
struct media_aac_tns {
	uint8_t length;
	uint8_t order;
	uint8_t direction;
	uint8_t resolution;
	int8_t coefficients[MEDIA_AAC_TNS_ORDER];
};

/* One channel's window grouping, shared by a common-window stereo pair. */
struct media_aac_ics {
	const uint16_t *offsets;
	uint8_t sequence;
	uint8_t shape;
	uint8_t max_sfb;
	uint8_t groups;
	uint8_t group_length[8];
	uint8_t windows;
	uint8_t band_count;
	uint8_t tns_limit;
};

/* One complete channel's syntax and decoded spectrum, retained until its filterbank finishes. */
struct media_aac_channel {
	struct media_aac_ics info;
	uint8_t type;
	uint8_t tag;
	uint8_t side;
	uint8_t common;
	uint8_t ms_mode;
	uint8_t group;
	uint8_t gain;
	uint8_t books[8][MEDIA_AAC_BANDS_MAX];
	int scales[8][MEDIA_AAC_BANDS_MAX];
	uint8_t ms[8][MEDIA_AAC_BANDS_MAX];
	uint8_t tns_count[8];
	struct media_aac_tns tns[8][3];
	int16_t quantized[1024];
	float spectrum[1024];
};

/* One parsed raw_data_block; the decoder owns this working storage, never an exported object. */
struct media_aac_frame {
	struct media_aac_config config;
	unsigned channels;
	struct media_aac_channel channel[MEDIA_AAC_MAX_CHANNELS];
};

int media_aac_frame_parse(struct media_bits *bits, const struct media_aac_config *config, struct media_aac_frame *frame);
#endif
