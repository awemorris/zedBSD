/*
 * Kernel graphics driver contract
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef ZEDBSD_KERN_GRAPHICS_DRIVER_H
#define ZEDBSD_KERN_GRAPHICS_DRIVER_H

#include <stddef.h>
#include <stdint.h>

struct kern_graphics_mode {
	unsigned preferred_width;
	unsigned preferred_height;
	unsigned preferred_bits_per_pixel;
	unsigned width, height, bits_per_pixel, stride;
};

struct kern_graphics_mode_info {
	unsigned width, height, bits_per_pixel, stride;
};

struct kern_graphics_rect {
	unsigned x, y, width, height;
};

struct kern_graphics_image {
	unsigned format, width, height;
	size_t stride;
	const uint8_t *pixels;
	const uint32_t *palette;
	unsigned palette_size;
};

struct graphics_driver_ops {
	size_t (*get_modes)(void *, struct kern_graphics_mode_info *, size_t);
	int (*enter)(void *, struct kern_graphics_mode *);
	int (*clear)(void *);
	void (*leave)(void *);
	int (*fill)(void *, const struct kern_graphics_rect *, uint32_t);
	int (*line)(void *, unsigned, unsigned, unsigned, unsigned, uint32_t);
	int (*pattern_fill)(void *, const struct kern_graphics_rect *, uint32_t,
		uint64_t);
	int (*blit)(void *, unsigned, unsigned,
		const struct kern_graphics_image *, uint64_t, int);
	int (*flush)(void *, const struct kern_graphics_rect *, size_t);
	int (*get_glyph)(void *, uint32_t, uint8_t[32], unsigned *, unsigned *);
};

int graphics_driver_register(const struct graphics_driver_ops *, void *);

#endif
