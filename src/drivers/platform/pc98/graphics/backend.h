/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PC-98 graphics-private frontend/backend boundary.
 */

#ifndef KERN_DRIVERS_GRAPHICS_PC98_BACKEND_H
#define KERN_DRIVERS_GRAPHICS_PC98_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include <uapi/graphics.h>

/*
 * One image the front end hands to the blit operation.
 *
 * pattern_y is the row of the image's first row relative to the user's
 * request, so a BLIT_PATTERN drawn one row at a time keeps the pattern's
 * vertical phase.
 */
struct pc98_graphics_image {
	unsigned format, width, height;
	size_t stride;
	const uint8_t *pixels;
	const uint32_t *palette;
	unsigned palette_size;
	unsigned pattern_y;
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

struct kern_text_range;

/*
 * Ends the console's use of the display for good: /dev/graphics refuses
 * every request from now on and the backend removes its mappings
 * (drv_pc98_graphics_end, in pc98-graphics.c); the backend's own part,
 * under the graphics lock (drv_pc98_graphics_backend_end).
 */
int drv_pc98_graphics_end(const struct kern_text_range *ranges, unsigned count);
int drv_pc98_graphics_backend_end(const struct kern_text_range *ranges, unsigned count);

#endif
