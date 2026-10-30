/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The framebuffer the firmware or the boot loader left on the screen, as a
 * board without its own display driver uses it: the text console's
 * surface (drivers/generic/fb-text.h) and the /dev/graphics backend at the
 * firmware's priority.
 */

#ifndef KERN_DRIVERS_GENERIC_BOOT_FB_H
#define KERN_DRIVERS_GENERIC_BOOT_FB_H

#include <stdint.h>

struct kern_boot_framebuffer;
struct kern_text_range;

/*
 * Maps the screen a board read with kern_boot_framebuffer(), its format
 * one of KERN_BOOT_FRAMEBUFFER_RGBX8888 and KERN_BOOT_FRAMEBUFFER_BGRX8888
 * and its pitch in bytes.  opaque holds the bits every pixel carries set
 * (the alpha byte of a plane that blends by it; 0 when the byte is not
 * looked at).
 *
 * Returns 0, ENODEV for a form the driver cannot draw, or the error of the
 * mapping.  A second call does nothing.
 */
int
drv_boot_fb_init(
	const struct kern_boot_framebuffer *screen,
	uint32_t opaque);

/*
 * Tells whether the framebuffer is mapped and can be drawn.
 */
int
drv_boot_fb_ready(
	void);

/*
 * Reports the size of the framebuffer in pixels; zero before it is mapped.
 */
void
drv_boot_fb_get_size(
	unsigned *width,
	unsigned *height);

/*
 * Fills a rectangle with one 0xRRGGBB color, clipped to the screen.
 */
void
drv_boot_fb_fill(
	unsigned x,
	unsigned y,
	unsigned width,
	unsigned height,
	uint32_t color);

/*
 * Draws one 8x16 glyph (16 bytes, the leftmost pixel in the high bit) at
 * a pixel position, set bits in foreground and the others in background.
 */
void
drv_boot_fb_glyph(
	unsigned x,
	unsigned y,
	const uint8_t *glyph,
	uint32_t foreground,
	uint32_t background);

/*
 * Offers the framebuffer to /dev/graphics at the firmware's priority.
 *
 * Returns 0, ENODEV when the framebuffer is not mapped or the kernel has
 * no /dev/graphics, or the error of the registration.
 */
int
drv_boot_fb_register_backend(
	void);

/*
 * Gives the framebuffer up for good once the console has left the display.
 *
 * Nothing is drawn on it again and the driver's mapping of it is given
 * back (the hardware is not written).  The hardware layer keeps its own
 * view of the screen for its stall and fatal reports, so the screen counts
 * as left: returns EBUSY when it lies in the count ranges, 0 otherwise.  A
 * later call waits for the first one and judges against its own ranges.
 * The caller stopped the console's drawing first, and holds /dev/graphics's
 * lock when /dev/graphics draws here; thread context, it may sleep.  It is
 * also /dev/graphics's backend end.
 */
int
drv_boot_fb_end(
	const struct kern_text_range *ranges,
	unsigned count);

#endif
