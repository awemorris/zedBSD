/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Private AAC Huffman syntax; callers own readers and receive output only on success. */
#ifndef LIBMEDIA_AAC_CODEC_H
#define LIBMEDIA_AAC_CODEC_H

#include "bits.h"

int media_aac_huffman_symbol(struct media_bits *bits, unsigned book, uint16_t *symbol);
int media_aac_scalefactor(struct media_bits *bits, int *difference);
int media_aac_spectral(struct media_bits *bits, unsigned book, int16_t coefficients[4], unsigned *count);

#endif
