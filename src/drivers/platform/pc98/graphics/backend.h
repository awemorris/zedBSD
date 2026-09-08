/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PC-98 graphics-private frontend/backend boundary.
 */

#ifndef ZEDBSD_DRIVERS_GRAPHICS_PC98_BACKEND_H
#define ZEDBSD_DRIVERS_GRAPHICS_PC98_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include <zedbsd/graphics.h>

struct pc98_graphics_image {
	unsigned format, width, height;
	size_t stride;
	const uint8_t *pixels;
	const uint32_t *palette;
	unsigned palette_size;
};

int drv_pc98_graphics_backend_ready(void);
size_t drv_pc98_graphics_backend_get_modes(struct graphics_mode_info *, size_t);
int drv_pc98_graphics_backend_enter(struct graphics_mode *);
void drv_pc98_graphics_backend_leave(void);
int drv_pc98_graphics_backend_fill(const struct graphics_rect *, uint32_t);
int drv_pc98_graphics_backend_line(unsigned, unsigned, unsigned, unsigned,
				   uint32_t);
int drv_pc98_graphics_backend_pattern_fill(const struct graphics_rect *,
					   uint32_t, uint64_t);
int drv_pc98_graphics_backend_blit(unsigned, unsigned,
				   const struct pc98_graphics_image *, uint64_t,
				   int);
int drv_pc98_graphics_backend_flush(const struct graphics_rect *, size_t);
int drv_pc98_graphics_backend_get_glyph(uint32_t, uint8_t[32], unsigned *,
					unsigned *);

#endif
