/* PC/AT graphics-private frontend/backend boundary.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_DRIVERS_GRAPHICS_PCAT_BACKEND_H
#define ZEDBSD_DRIVERS_GRAPHICS_PCAT_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include <zedbsd/graphics.h>

struct pcat_graphics_image {
	unsigned format, width, height;
	size_t stride;
	const uint8_t *pixels;
	const uint32_t *palette;
	unsigned palette_size;
};

int pcat_graphics_backend_ready(void);
size_t pcat_graphics_backend_get_modes(struct graphics_mode_info *, size_t);
int pcat_graphics_backend_enter(struct graphics_mode *);
void pcat_graphics_backend_leave(void);
int pcat_graphics_backend_fill(const struct graphics_rect *, uint32_t);
int pcat_graphics_backend_line(unsigned, unsigned, unsigned, unsigned,
    uint32_t);
int pcat_graphics_backend_pattern_fill(const struct graphics_rect *, uint32_t,
    uint64_t);
int pcat_graphics_backend_blit(unsigned, unsigned,
    const struct pcat_graphics_image *, uint64_t, int);
int pcat_graphics_backend_flush(const struct graphics_rect *, size_t);
int pcat_graphics_backend_get_glyph(uint32_t, uint8_t[32], unsigned *,
    unsigned *);

#endif
