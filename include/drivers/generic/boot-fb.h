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
 * one of KERN_BOOT_FRAMEBUFFER_RGBX8888, KERN_BOOT_FRAMEBUFFER_BGRX8888
 * and KERN_BOOT_FRAMEBUFFER_XRGB8888 and its pitch in bytes.  opaque
 * holds the bits every pixel carries set, as the 32-bit word the
 * processor stores (the alpha byte of a plane that blends by it; 0 when
 * the byte is not looked at).
 *
 * Returns 0, ENODEV for a form the driver cannot draw, or the error of the
 * mapping.  A second call, by either form of the start, does nothing.
 */
int
drv_boot_fb_init(
	const struct kern_boot_framebuffer *screen,
	uint32_t opaque);

/*
 * Maps a screen whose every pixel is one byte, an index into the
 * display's colour table, which the firmware loaded and the driver never
 * changes; the screen's format is not looked at and its pitch is in
 * bytes.  palette lists, as 0xRRGGBB, the colour each of the table's
 * first entries (at most 256) shows; every colour is drawn with the
 * listed entry nearest to it, and /dev/graphics reports the mode as 8 bits
 * a pixel.  The list is read during the call only.
 *
 * Returns 0, ENODEV for a form the driver cannot draw or a missing list,
 * or the error of the mapping.  A second call, by either form of the
 * start, does nothing.
 */
int
drv_boot_fb_init_indexed(
	const struct kern_boot_framebuffer *screen,
	const uint32_t *palette,
	unsigned entries);

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
 * Nothing is drawn on it again, and the driver's mapping of it and the
 * early console's, which the kernel hands over as a mapping of exactly the
 * screen once the console prints through the kernel, are removed (the
 * hardware is not written).  Returns 0 when no mapping of the screen is
 * left or the screen lies outside the count ranges, the error of a failed
 * removal, or EBUSY for a mapping kept for good, when the screen lies in
 * them.  A later call waits for the first one and judges against its own
 * ranges.
 * The caller stopped the console's drawing first, and holds /dev/graphics's
 * lock when /dev/graphics draws here; thread context, it may sleep.  It is
 * also /dev/graphics's backend end.
 */
int
drv_boot_fb_end(
	const struct kern_text_range *ranges,
	unsigned count);

#endif
