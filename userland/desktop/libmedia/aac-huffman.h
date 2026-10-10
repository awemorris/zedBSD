/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBMEDIA_AAC_HUFFMAN_H
#define LIBMEDIA_AAC_HUFFMAN_H

#include <stdint.h>

/* One normative AAC codeword and its symbol; owned by a constant codebook. */
struct media_aac_codeword {
	uint32_t word;
	uint16_t symbol;
	uint8_t length;
};

/* One complete constant codebook, independent of the eventual decoder's lookup algorithm. */
struct media_aac_codebook {
	const struct media_aac_codeword *words;
	uint16_t count;
};

/* Codebook zero encodes scalefactor differences; one through eleven encode spectral tuples. */
extern const struct media_aac_codebook media_aac_huffman_books[12];

#endif
