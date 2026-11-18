/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Private, bounded MSB-first reading of an elementary stream's syntax. */
#ifndef LIBMEDIA_BITS_H
#define LIBMEDIA_BITS_H

#include <stddef.h>
#include <stdint.h>

/* A borrowed byte span and sticky read error, owned by its parser call. */
struct media_bits {
	const uint8_t *data;
	size_t count;
	size_t position;
	int error;
};

void media_bits_init(struct media_bits *bits, const void *data, size_t size);
uint32_t media_bits_read(struct media_bits *bits, unsigned count);
uint32_t media_bits_read1(struct media_bits *bits);
void media_bits_skip(struct media_bits *bits, size_t count);
void media_bits_align(struct media_bits *bits);
size_t media_bits_left(const struct media_bits *bits);
uint32_t media_bits_ue(struct media_bits *bits);
int32_t media_bits_se(struct media_bits *bits);
int media_rbsp_unescape(const uint8_t *source, size_t size, uint8_t *destination, size_t capacity, size_t *written);

#endif
