/* PC-98 graphics-private frontend/backend boundary.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
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

int pc98_graphics_backend_ready(void);
size_t pc98_graphics_backend_get_modes(struct graphics_mode_info *, size_t);
int pc98_graphics_backend_enter(struct graphics_mode *);
void pc98_graphics_backend_leave(void);
int pc98_graphics_backend_fill(const struct graphics_rect *, uint32_t);
int pc98_graphics_backend_line(unsigned, unsigned, unsigned, unsigned,
    uint32_t);
int pc98_graphics_backend_pattern_fill(const struct graphics_rect *, uint32_t,
    uint64_t);
int pc98_graphics_backend_blit(unsigned, unsigned,
    const struct pc98_graphics_image *, uint64_t, int);
int pc98_graphics_backend_flush(const struct graphics_rect *, size_t);
int pc98_graphics_backend_get_glyph(uint32_t, uint8_t[32], unsigned *,
    unsigned *);

#endif
