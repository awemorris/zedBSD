/*
 * zedBSD PC-98 native display backend contract
 * Copyright (C) 2025, 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_PC98_DISPLAY_H
#define ZEDBSD_PC98_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

struct pc98_display_rect {
	unsigned x, y, width, height;
};

struct pc98_display_info {
	unsigned preferred_bits_per_pixel;
	unsigned width, height, bits_per_pixel, stride;
};

enum pc98_display_image_format {
	PC98_DISPLAY_IMAGE_INDEX8 = 1,
	PC98_DISPLAY_IMAGE_RGB24 = 2,
};

struct pc98_display_image {
	enum pc98_display_image_format format;
	unsigned width, height;
	size_t stride;
	const uint8_t *pixels;
	uint32_t palette[256];
	unsigned palette_size;
};

struct pc98_display_ops {
	void *context;
	int (*enter)(void *, struct pc98_display_info *);
	void (*leave)(void *);
	int (*poll_events)(void *);
	int (*fill)(void *, const struct pc98_display_rect *, uint32_t);
	int (*line)(void *, unsigned, unsigned, unsigned, unsigned, uint32_t);
	int (*pattern_fill)(void *, const struct pc98_display_rect *, uint32_t,
		uint64_t);
	int (*draw_image)(void *, unsigned, unsigned,
		const struct pc98_display_image *);
	int (*draw_image_pattern)(void *, unsigned, unsigned,
		const struct pc98_display_image *, uint64_t);
	int (*flush)(void *, const struct pc98_display_rect *, size_t);
};

struct pc98_glyph_ops {
	void *context;
	int (*measure)(void *, uint32_t, unsigned *, unsigned *);
	int (*draw)(void *, unsigned, unsigned, uint32_t, uint32_t, uint32_t);
};

struct pc98_display_backend {
	struct pc98_display_ops display;
	struct pc98_glyph_ops glyph;
};

#endif
