/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PC/AT boot framebuffer, Cirrus GD5446, and standard VGA backends.
 */

#ifndef KERN_DRIVERS_GRAPHICS_PCAT_BACKEND_H
#define KERN_DRIVERS_GRAPHICS_PCAT_BACKEND_H

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
struct pcat_graphics_image {
	unsigned format, width, height;
	size_t stride;
	const uint8_t *pixels;
	const uint32_t *palette;
	unsigned palette_size;
	unsigned pattern_y;
};

int drv_pcat_graphics_backend_ready(void);
size_t drv_pcat_graphics_backend_get_modes(struct graphics_mode_info *, size_t);
int drv_pcat_graphics_backend_enter(struct graphics_mode *);
void drv_pcat_graphics_backend_leave(void);
int drv_pcat_graphics_backend_fill(const struct graphics_rect *, uint32_t);
int drv_pcat_graphics_backend_line(unsigned, unsigned, unsigned, unsigned,
				   uint32_t);
int drv_pcat_graphics_backend_pattern_fill(const struct graphics_rect *,
					   uint32_t, uint64_t);
int drv_pcat_graphics_backend_blit(unsigned, unsigned,
				   const struct pcat_graphics_image *, uint64_t,
				   int);
int drv_pcat_graphics_backend_flush(const struct graphics_rect *, size_t);
int drv_pcat_graphics_backend_get_glyph(uint32_t, uint8_t[32], unsigned *,
					unsigned *);

/*
 * Report the linear framebuffer this backend owns.
 *
 * The text layer draws glyphs into the same framebuffer, so it needs
 * the pixel pointer and the geometry. Returns 0 when no linear
 * framebuffer is available.
 */
int drv_pcat_graphics_backend_get_framebuffer(
	volatile uint32_t **pixels,
	unsigned *width,
	unsigned *height,
	unsigned *stride,
	int *rgbx);

/*
 * Follow the boot framebuffer after its device moved the physical range
 * [from, from + length) to to (a PCI BAR given a new address): the kernel
 * maps the new place and the text console draws there.
 */
void drv_pcat_graphics_framebuffer_moved(
	uint64_t from,
	uint64_t to,
	uint64_t length);

struct kern_text_range;

/*
 * Ends the console's use of the display for good: /dev/graphics refuses
 * every request from now on and the backend removes its mappings
 * (drv_pcat_graphics_end, in pcat-graphics.c); the backend's own part,
 * under the graphics lock (drv_pcat_graphics_backend_end).
 */
int drv_pcat_graphics_end(const struct kern_text_range *ranges, unsigned count);
int drv_pcat_graphics_backend_end(const struct kern_text_range *ranges, unsigned count);

#endif
