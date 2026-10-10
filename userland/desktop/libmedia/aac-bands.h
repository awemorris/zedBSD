/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBMEDIA_AAC_BANDS_H
#define LIBMEDIA_AAC_BANDS_H
#include <stdint.h>

/* Immutable normative scalefactor-band boundaries and TNS limits for one frequency index. */
struct media_aac_bands {
	const uint16_t *long_offsets;
	const uint16_t *short_offsets;
	uint8_t long_count;
	uint8_t short_count;
	uint8_t long_tns;
	uint8_t short_tns;
};

extern const struct media_aac_bands media_aac_bands[13];
#endif
