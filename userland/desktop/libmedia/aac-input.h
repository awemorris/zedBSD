/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Private AAC-LC configuration and transport syntax, without a decoder dependency. */
#ifndef LIBMEDIA_AAC_INPUT_H
#define LIBMEDIA_AAC_INPUT_H

#include "bits.h"

/* The decoder's planned channel bound; transport can describe larger unsupported programs. */
#define MEDIA_AAC_MAX_CHANNELS 8U

/* One PCE element's identity and speaker group, in transmitted order. */
struct media_aac_channel_element {
	uint8_t type;
	uint8_t tag;
	uint8_t group;
};

/* A validated LC configuration; the parser publishes it only on success. */
struct media_aac_config {
	uint32_t rate;
	uint8_t rate_index;
	uint8_t channel_configuration;
	uint8_t channels;
	uint8_t element_count;
	uint8_t program_tag;
	uint8_t matrix_present;
	uint8_t matrix_index;
	uint8_t pseudo_surround;
	struct media_aac_channel_element elements[MEDIA_AAC_MAX_CHANNELS];
};

/*
 * One framed ADTS packet; block offsets are absolute within this frame.
 * Unindexed later blocks in unprotected frames keep zero offsets.
 * CRC contents and raw payload syntax are checked by the future frame parser.
 */
struct media_aac_adts {
	struct media_aac_config config;
	size_t frame_size;
	size_t header_size;
	size_t block_offset[4];
	uint8_t blocks;
	uint8_t protected_frame;
};

int media_aac_config_parse(const uint8_t *data, size_t size, struct media_aac_config *config);
int media_aac_pce_parse(struct media_bits *bits, struct media_aac_config *config);
int media_aac_adts_parse(const uint8_t *data, size_t size, struct media_aac_adts *frame);

#endif
