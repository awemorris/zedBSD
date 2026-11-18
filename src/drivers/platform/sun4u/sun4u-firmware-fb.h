/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The framebuffer the firmware (OpenBoot, OpenBIOS) or the HAL left on the
 * sun4u screen: the text console's surface and the /dev/graphics backend.
 */

#ifndef KERN_DRIVERS_SUN4U_FIRMWARE_FB_H
#define KERN_DRIVERS_SUN4U_FIRMWARE_FB_H

#include <stdint.h>

/*
 * Maps the framebuffer the HAL names ("sun4u.framebuffer.*").
 *
 * Returns 0, or ENODEV when the HAL shows no framebuffer or names a form
 * the driver cannot draw, or the error of the mapping.  A second call
 * does nothing.
 */
int
drv_sun4u_fb_init(
	void);

/*
 * Tells whether the framebuffer is mapped and can be drawn.
 */
int
drv_sun4u_fb_ready(
	void);

/*
 * Reports the size of the framebuffer in pixels; zero before it is mapped.
 */
void
drv_sun4u_fb_get_size(
	unsigned *width,
	unsigned *height);

/*
 * Fills a rectangle with one 0xRRGGBB color, clipped to the screen.
 */
void
drv_sun4u_fb_fill(
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
drv_sun4u_fb_glyph(
	unsigned x,
	unsigned y,
	const uint8_t *glyph,
	uint32_t foreground,
	uint32_t background);

/*
 * Offers the framebuffer to /dev/graphics at the firmware's priority.
 *
 * Returns 0, ENODEV when the framebuffer is not mapped, or the error of
 * the registration.
 */
int
drv_sun4u_fb_register_backend(
	void);

struct kern_text_range;

/*
 * Gives the framebuffer up for good once the console has left the display.
 *
 * Nothing is drawn on it again, and the kernel's mapping of it is removed
 * (the hardware is not written).  Returns 0 when nothing left mapped lies
 * in the count ranges, the error of a removal that failed for a mapping in
 * them, or EBUSY for a mapping in them the hardware layer keeps.  A later
 * call waits for the first one's removal and judges what it left against
 * its own ranges.  The caller stopped the console's drawing first, and
 * holds /dev/graphics's lock when /dev/graphics draws here; thread
 * context, it may sleep.  It is also /dev/graphics's backend end.
 */
int
drv_sun4u_fb_end(
	const struct kern_text_range *ranges,
	unsigned count);

#endif
