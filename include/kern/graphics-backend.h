/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The backend face of the generic /dev/graphics front end.
 *
 * The front end (src/drivers/generic/graphics-device.c) owns the device
 * node, the single open, the user copies, and the splitting of a user row
 * into chunks that fit its kernel buffers. A backend only draws: it gets
 * already-validated rectangles and rows of at most a few thousand pixels.
 *
 * Several backends may register during boot; the one with the highest
 * priority is chosen once, when the front end registers /dev/graphics.
 * A firmware framebuffer registers with DRV_GRAPHICS_PRIORITY_FIRMWARE,
 * the lowest. A later card driver takes the display over this way:
 *
 *   1. It matches only display nodes the loader enumerated (no kernel PCI
 *      scan), using the platform's named handoff values.
 *   2. During boot, before /dev/graphics is registered, it calls
 *      drv_graphics_backend_register() with a higher priority.
 *   3. It moves the text console to its own surface by calling
 *      kern_text_register() again (a second call replaces the first).
 *   4. The firmware mapping is no longer used after that; its window is
 *      not returned then.
 *
 * Takeover happens only at boot, so nothing here waits for graphics to be
 * left before a backend changes.
 *
 * Once the console's use of the display ends for good (kern_text_end(),
 * when a GPU flipped its first image), the front end refuses every open
 * and request and has the chosen backend give its mappings back through
 * its optional end operation; the firmware framebuffer's driver gives its
 * own mapping back then too, chosen or not.
 */

#ifndef KERN_KERN_GRAPHICS_BACKEND_H
#define KERN_KERN_GRAPHICS_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include <uapi/graphics.h>
#include "kern/boot.h"

struct kern_text_range;

/* The priority of a framebuffer the firmware left behind. */
#define DRV_GRAPHICS_PRIORITY_FIRMWARE	0

/* The priority of a driver that programs the display card itself. */
#define DRV_GRAPHICS_PRIORITY_CARD	100

/*
 * One chunk of one row of a blit, as the front end hands it to a backend.
 *
 * The pixels live in the front end's row buffer and are valid only during
 * the call. format is KERN_GRAPHICS_FORMAT_INDEX8 (one palette index per
 * byte; a MONO1 row arrives expanded to indexes 0 and 1 with a two-entry
 * palette) or KERN_GRAPHICS_FORMAT_RGB24 (three bytes R, G, B per pixel).
 * pattern_x and pattern_y are the position of the chunk's first pixel
 * relative to the request's top-left corner, so a BLIT_PATTERN pattern
 * keeps its phase across chunks and rows.
 */
struct drv_graphics_row {
	unsigned format;
	unsigned width;
	const uint8_t *pixels;
	const uint32_t *palette;
	unsigned palette_size;
	unsigned pattern_x;
	unsigned pattern_y;
};

/*
 * The table of drawing operations one backend provides.
 *
 * The table is static and lives as long as the kernel. Every operation
 * runs under the front end's sleepable lock, with arguments the front end
 * already checked against the entered mode. A drawing operation returns
 * nonzero on success and zero when the hardware refused.
 *
 * end is optional (NULL for a backend with nothing to give back). It runs
 * under the same lock, or before /dev/graphics registers with no lock at
 * all, once the console has left the display for good: nothing is drawn
 * through the backend again and every kernel mapping of display memory it
 * made is removed, without writing the hardware. It returns 0 when
 * nothing left mapped lies in the count ranges, the error of a removal
 * that failed for a mapping in them, or EBUSY for a mapping in them that
 * cannot be removed. A later call judges what the first one left against
 * its own ranges.
 */
struct drv_graphics_backend {
	const char *name;
	int (*ready)(void);
	size_t (*get_modes)(struct graphics_mode_info *modes, size_t capacity);
	int (*enter)(struct graphics_mode *mode);
	void (*leave)(void);
	int (*fill)(const struct graphics_rect *rect, uint32_t color);
	int (*line)(unsigned x0, unsigned y0, unsigned x1, unsigned y1, uint32_t color);
	int (*pattern_fill)(const struct graphics_rect *rect, uint32_t color, uint64_t pattern);
	int (*blit_row)(unsigned x, unsigned y, const struct drv_graphics_row *row, uint64_t pattern, int patterned);
	int (*flush)(const struct graphics_rect *rects, size_t count);
	int (*get_glyph)(uint32_t codepoint, uint8_t bitmap[32], unsigned *width, unsigned *height);
	int (*end)(const struct kern_text_range *ranges, unsigned count);
};

/*
 * Offers a backend for /dev/graphics.
 *
 * Call during boot, before the front end registers the device. The
 * highest priority wins; an equal priority keeps the earlier backend.
 * Returns 0, EINVAL for a null or incomplete table, or EBUSY once the
 * choice has been made.
 */
int
drv_graphics_backend_register(
	const struct drv_graphics_backend *backend,
	int priority);

/*
 * Reports the backend /dev/graphics chose, or NULL before the choice or
 * when no backend registered.
 */
const struct drv_graphics_backend *
drv_graphics_backend_selected(void);

/*
 * Ends /dev/graphics for good once the console has left the display.
 *
 * A graphics mode entered is dropped without writing the hardware, every
 * open and request is refused with ENODEV from now on, closing does not
 * give the screen back, and the chosen backend's end, when it has one,
 * removes its mappings. Before /dev/graphics registers, the choice of the
 * backend is fixed here, so no backend offered later can draw. Returns
 * what the backend's end returns (0 without one); a later call has it
 * judge again against its own ranges. Thread context, no lock held; it
 * may sleep.
 */
int
drv_graphics_device_end(
	const struct kern_text_range *ranges,
	unsigned count);

/*
 * Tells whether a pattern pixel is set.
 *
 * x and y are relative to the request's top-left corner. Row y of the
 * 8x8 pattern is byte y of the 64-bit value, and the most significant bit
 * of that byte is column 0.
 */
static __inline int
drv_graphics_pattern_bit(
	uint64_t pattern,
	unsigned x,
	unsigned y)
{
	uint8_t row;

	/* Picks the pattern byte of this row. */
	row = (uint8_t)(pattern >> ((y & 7U) * 8U));

	/* Tests the column's bit, column 0 being the high bit. */
	if ((row & (uint8_t)(0x80U >> (x & 7U))) == 0)
		return 0;

	/* The pixel is set. */
	return 1;
}

/*
 * Converts a 0xRRGGBB color to the RGB332 byte (red bits 7-5, green
 * bits 4-2, blue bits 1-0).
 */
static __inline uint8_t
drv_graphics_rgb332(
	uint32_t color)
{
	uint8_t red;
	uint8_t green;
	uint8_t blue;

	/* Keeps the top three bits of red. */
	red = (uint8_t)((color >> 16) & 0xe0U);

	/* Keeps the top three bits of green, shifted under red. */
	green = (uint8_t)(((color >> 8) & 0xe0U) >> 3);

	/* Keeps the top two bits of blue at the bottom. */
	blue = (uint8_t)((color & 0xc0U) >> 6);

	/* Returns the packed byte. */
	return (uint8_t)(red | green | blue);
}

/*
 * Returns the 32-bit value a backend stores, as a native uint32_t store
 * through an invert-endian (little-endian) mapping, so a 0xRRGGBB color
 * lands in memory in the framebuffer's byte order.
 *
 * KERN_BOOT_FRAMEBUFFER_BGRX8888 wants the bytes B, G, R, X in increasing
 * address order: that is the color itself as a little-endian word.
 * KERN_BOOT_FRAMEBUFFER_XRGB8888 wants X, R, G, B: the color byte-swapped.
 * Never store two pixels with one 64-bit store through such a mapping;
 * the inversion would swap the two pixels as well.
 */
static __inline uint32_t
drv_graphics_pixel_word_le(
	unsigned format,
	uint32_t color)
{
	uint32_t swapped;

	/* Keeps the color as it is for the blue-first order. */
	if (format != KERN_BOOT_FRAMEBUFFER_XRGB8888)
		return color & 0x00ffffffU;

	/* Reverses the four bytes for the X-first order. */
	swapped = (color & 0x000000ffU) << 24;
	swapped |= (color & 0x0000ff00U) << 8;
	swapped |= (color & 0x00ff0000U) >> 8;

	/* Returns the swapped word, whose top byte is blue. */
	return swapped;
}

#endif
