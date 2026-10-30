/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sun4u firmware framebuffer (WS275).
 *
 * The loader takes the display the firmware set up (OpenBoot, OpenBIOS)
 * from the device tree; when there is none the HAL sets the QEMU standard
 * VGA itself.  Either way the HAL names the framebuffer it shows as
 * "sun4u.framebuffer.*", and this driver maps it once and draws into it:
 * the text console's cells and, with CONFIG_DRIVER_GRAPHICS_DEVICE, the
 * /dev/graphics backend at the firmware's (lowest) priority, so a later
 * card driver can take the screen over.
 *
 * The mapping inverts endianness (the kernel's device windows are
 * little-endian), so a native 32-bit store lands as a little-endian word.
 * A four-byte pixel is therefore written as one 32-bit store of
 * drv_graphics_pixel_word_le(); two pixels are never written with one
 * 64-bit store, which the inversion would swap.  An RGB332 pixel is one
 * byte, which the inversion does not touch.  The firmware decided the one
 * mode; nothing here programs the display.
 */

#include <stdint.h>

#include "drivers/platform/sun4u/sun4u-firmware-fb.h"
#include "drivers/platform/pcat/graphics/vgafont.h"

#include <uapi/errno.h>
#include <uapi/graphics.h>

#include <kern/boot.h>
#include <kern/graphics-backend.h>
#include <kern/kcrt.h>
#include <kern/lock.h>
#include <kern/platform.h>
#include <kern/pmem.h>
#include <kern/text-display.h>

/* The glyphs /dev/graphics hands out: the 8x16 font's ASCII half. */
#define SUN4U_FB_GLYPHS		128U
#define SUN4U_FB_GLYPH_WIDTH	8U
#define SUN4U_FB_GLYPH_HEIGHT	16U

/* The largest screen the driver accepts, the interface's own limit. */
#define SUN4U_FB_MAX_SIDE	16384U

/*
 * The framebuffer's mapping as the end left it: its physical range, and
 * the error of its removal (0 for a window the hardware layer keeps for
 * good).  bytes is 0 when nothing was left.
 */
struct sun4u_fb_left {
	uint64_t physical;
	uint64_t bytes;
	int error;
};

static void fb_end_lock_init_once(void);
static uint32_t fb_value(uint32_t color);
static void fb_put(unsigned x, unsigned y, uint32_t value);
static const uint64_t *fb_name(const char *name);

#if CONFIG_DRIVER_GRAPHICS_DEVICE
static int backend_ready(void);
static size_t backend_get_modes(struct graphics_mode_info *modes, size_t capacity);
static int backend_enter(struct graphics_mode *mode);
static void backend_leave(void);
static int backend_fill(const struct graphics_rect *rect, uint32_t color);
static int backend_line(unsigned x0, unsigned y0, unsigned x1, unsigned y1, uint32_t color);
static int backend_pattern_fill(const struct graphics_rect *rect, uint32_t color, uint64_t pattern);
static int backend_blit_row(unsigned x, unsigned y, const struct drv_graphics_row *row, uint64_t pattern, int patterned);
static int backend_flush(const struct graphics_rect *rects, size_t count);
static int backend_get_glyph(uint32_t codepoint, uint8_t bitmap[32], unsigned *width, unsigned *height);

/*
 * The backend table /dev/graphics draws through.  Static and never
 * changed.
 */
static const struct drv_graphics_backend sun4u_fb_backend = {
	.name = "sun4u firmware framebuffer",
	.ready = backend_ready,
	.get_modes = backend_get_modes,
	.enter = backend_enter,
	.leave = backend_leave,
	.fill = backend_fill,
	.line = backend_line,
	.pattern_fill = backend_pattern_fill,
	.blit_row = backend_blit_row,
	.flush = backend_flush,
	.get_glyph = backend_get_glyph,
	.end = drv_sun4u_fb_end
};
#endif

/*
 * The mapped framebuffer and its shape, set once by drv_sun4u_fb_init()
 * from the boot thread before the console or /dev/graphics use them, and
 * only read afterwards.  fb_pixels is NULL until the mapping succeeded and
 * again once the end removed it; fb_mapping, fb_physical and fb_bytes are
 * the mapping the end removes.
 */
static volatile uint8_t *fb_pixels;
static void *fb_mapping;
static uint64_t fb_physical;
static size_t fb_bytes;
static unsigned fb_width;
static unsigned fb_height;
static unsigned fb_pitch;
static unsigned fb_format;

/*
 * Serializes the end: the first caller removes the mapping, and a later
 * one waits for it before judging what was left.  Initialized once by the
 * first drv_sun4u_fb_init(), on the boot thread; fb_end_lock_ready says
 * it was.
 */
static struct mutex fb_end_lock;
static int fb_end_lock_ready;

/*
 * Whether the end has run (nothing is drawn or mapped again), and what it
 * could not remove.  Written once under fb_end_lock (or, with no lock yet,
 * before anything was mapped) and read under it afterwards.
 */
static int fb_ended;
static struct sun4u_fb_left fb_left;

/*
 * Maps the framebuffer the HAL names.
 */
int
drv_sun4u_fb_init(
	void)
{
	const uint64_t *phys;
	const uint64_t *width;
	const uint64_t *height;
	const uint64_t *pitch;
	const uint64_t *format;
	uint64_t bytes_per_pixel;
	uint64_t size;
	void *mapped;
	int error;

	/* Readies the end's lock before anything is mapped. */
	fb_end_lock_init_once();

	/* A second call keeps the first mapping. */
	if (fb_pixels != NULL)
		return 0;

	/* Nothing is mapped again once the console has left the display. */
	if (fb_ended)
		return ENODEV;

	/* Reads the framebuffer the HAL shows; without one there is none. */
	phys = fb_name("sun4u.framebuffer.phys");
	width = fb_name("sun4u.framebuffer.width");
	height = fb_name("sun4u.framebuffer.height");
	pitch = fb_name("sun4u.framebuffer.pitch");
	format = fb_name("sun4u.framebuffer.format");
	if (phys == NULL ||
	    width == NULL ||
	    height == NULL ||
	    pitch == NULL ||
	    format == NULL)
		return ENODEV;

	/* Takes only the forms the driver draws: two four-byte orders and RGB332. */
	if (*format == KERN_BOOT_FRAMEBUFFER_BGRX8888) {
		bytes_per_pixel = 4U;
	} else if (*format == KERN_BOOT_FRAMEBUFFER_XRGB8888) {
		bytes_per_pixel = 4U;
	} else if (*format == KERN_BOOT_FRAMEBUFFER_RGB332) {
		bytes_per_pixel = 1U;
	} else {
		return ENODEV;
	}

	/* Refuses an empty or oversized screen, or rows shorter than its width. */
	if (*width == 0 ||
	    *height == 0 ||
	    *width > SUN4U_FB_MAX_SIDE ||
	    *height > SUN4U_FB_MAX_SIDE)
		return ENODEV;
	if (*pitch < *width * bytes_per_pixel || *pitch > SUN4U_FB_MAX_SIDE * 4U)
		return ENODEV;

	/* Maps every row, uncached. */
	size = *pitch * *height;
	error = kern_device_map(*phys, (size_t)size, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Records the shape and the mapping, then publishes the mapping. */
	fb_width = (unsigned)*width;
	fb_height = (unsigned)*height;
	fb_pitch = (unsigned)*pitch;
	fb_format = (unsigned)*format;
	fb_mapping = mapped;
	fb_physical = *phys;
	fb_bytes = (size_t)size;
	fb_pixels = mapped;

	/* Succeeded: the framebuffer can be drawn. */
	return 0;
}

/*
 * Tells whether the framebuffer is mapped.
 */
int
drv_sun4u_fb_ready(
	void)
{
	/* No mapping, nothing to draw on. */
	if (fb_pixels == NULL)
		return 0;

	/* Succeeded: the framebuffer is mapped. */
	return 1;
}

/*
 * Reports the size of the framebuffer.
 */
void
drv_sun4u_fb_get_size(
	unsigned *width,
	unsigned *height)
{
	/* Reports the width when the caller asked for it. */
	if (width != NULL)
		*width = fb_width;

	/* Reports the height when the caller asked for it. */
	if (height != NULL)
		*height = fb_height;
}

/*
 * Fills a rectangle with one color.
 */
void
drv_sun4u_fb_fill(
	unsigned x,
	unsigned y,
	unsigned width,
	unsigned height,
	uint32_t color)
{
	uint32_t value;
	unsigned right;
	unsigned bottom;
	unsigned column;
	unsigned row;

	/* Nothing to fill before the mapping or off the screen. */
	if (fb_pixels == NULL || x >= fb_width || y >= fb_height)
		return;

	/* Clips the rectangle to the screen. */
	right = fb_width;
	if (width < fb_width - x)
		right = x + width;
	bottom = fb_height;
	if (height < fb_height - y)
		bottom = y + height;

	/* Converts the color once, then stores it in every pixel, row by row. */
	value = fb_value(color);
	for (row = y; row < bottom; row++) {
		for (column = x; column < right; column++)
			fb_put(column, row, value);
	}
}

/*
 * Draws one 8x16 glyph.
 */
void
drv_sun4u_fb_glyph(
	unsigned x,
	unsigned y,
	const uint8_t *glyph,
	uint32_t foreground,
	uint32_t background)
{
	uint32_t set;
	uint32_t clear;
	unsigned line;
	unsigned bit;
	uint8_t bits;

	/* Nothing to draw before the mapping or without a glyph. */
	if (fb_pixels == NULL || glyph == NULL)
		return;

	/* Converts both colors once. */
	set = fb_value(foreground);
	clear = fb_value(background);

	/* Draws each line of the glyph, the leftmost pixel in the high bit. */
	for (line = 0; line < SUN4U_FB_GLYPH_HEIGHT; line++) {
		bits = glyph[line];
		for (bit = 0; bit < SUN4U_FB_GLYPH_WIDTH; bit++) {
			if ((bits & (0x80U >> bit)) != 0)
				fb_put(x + bit, y + line, set);
			else
				fb_put(x + bit, y + line, clear);
		}
	}
}

/*
 * Offers the framebuffer to /dev/graphics.
 */
int
drv_sun4u_fb_register_backend(
	void)
{
#if CONFIG_DRIVER_GRAPHICS_DEVICE
	int error;

	/* Only a mapped framebuffer is offered. */
	if (fb_pixels == NULL)
		return ENODEV;

	/* Offers it at the lowest priority, so a card driver wins. */
	error = drv_graphics_backend_register(&sun4u_fb_backend, DRV_GRAPHICS_PRIORITY_FIRMWARE);
	if (error != 0)
		return error;

	/* Succeeded: /dev/graphics can choose the framebuffer. */
	return 0;
#else
	/* The kernel has no /dev/graphics to offer it to. */
	return ENODEV;
#endif
}

/*
 * Gives the framebuffer up for good once the console has left the display.
 */
int
drv_sun4u_fb_end(
	const struct kern_text_range *ranges,
	unsigned count)
{
	void *mapping;
	int overlaps;
	int error;

	/* Without the lock nothing was ever mapped, and nothing will be. */
	if (!fb_end_lock_ready) {
		fb_ended = 1;
		return 0;
	}

	/*
	 * The first end removes the mapping; a later one waits here for it and
	 * only judges what was left.
	 */
	mutex_lock(&fb_end_lock);

	/* Only the first end has a mapping to remove. */
	if (!fb_ended) {
		/*
		 * Nothing is drawn from now on.  The callers stopped every writer
		 * first: the console's under its own exclusion, /dev/graphics under
		 * the graphics lock this runs under when it draws here; a fatal
		 * stop's reveal sees the console ended and draws nothing.
		 */
		fb_ended = 1;
		mapping = fb_mapping;
		fb_pixels = NULL;
		fb_mapping = NULL;

		/* Removes the mapping; one the hardware layer keeps, or a failed removal, is left. */
		if (mapping != NULL) {
			error = kern_device_unmap(mapping, fb_bytes);
			if (error == EBUSY) {
				fb_left.physical = fb_physical;
				fb_left.bytes = fb_bytes;
				fb_left.error = 0;
			} else if (error != 0) {
				fb_left.physical = fb_physical;
				fb_left.bytes = fb_bytes;
				fb_left.error = error;
			}
		}
	}

	/* What was left, against the caller's ranges. */
	error = 0;
	if (fb_left.bytes != 0) {
		overlaps = kern_text_range_overlaps(ranges, count, fb_left.physical, fb_left.bytes);
		if (overlaps && fb_left.error != 0) {
			/* A range of the caller's memory whose removal failed. */
			error = fb_left.error;
		} else if (overlaps) {
			/* A range of the caller's memory that stays mapped for good. */
			error = EBUSY;
		}
	}

	mutex_unlock(&fb_end_lock);

	/* Reports a mapping left in the ranges. */
	if (error != 0)
		return error;

	/* Succeeded: nothing of the framebuffer stays mapped in the ranges. */
	return 0;
}

/* Initializes the end's lock on the first start. */
static void
fb_end_lock_init_once(
	void)
{
	/* Initializes the lock only once. */
	if (!fb_end_lock_ready) {
		(void)mutex_init(&fb_end_lock, LOCK_RANK_DEVICE, "sun4u framebuffer");
		fb_end_lock_ready = 1;
	}
}

/*
 * Converts a 0xRRGGBB color to the value fb_put() stores.
 */
static uint32_t
fb_value(
	uint32_t color)
{
	uint32_t value;

	/* An RGB332 pixel is its packed byte. */
	if (fb_format == KERN_BOOT_FRAMEBUFFER_RGB332) {
		value = drv_graphics_rgb332(color);
		return value;
	}

	/* A four-byte pixel is the word that lands in the screen's byte order. */
	value = drv_graphics_pixel_word_le(fb_format, color);

	/* Succeeded: the value to store. */
	return value;
}

/*
 * Stores one converted pixel; a pixel off the screen is not stored.
 */
static void
fb_put(
	unsigned x,
	unsigned y,
	uint32_t value)
{
	volatile uint8_t *row;

	/* Clips to the screen. */
	if (x >= fb_width || y >= fb_height)
		return;

	/* An RGB332 pixel is one byte. */
	row = fb_pixels + (size_t)y * fb_pitch;
	if (fb_format == KERN_BOOT_FRAMEBUFFER_RGB332) {
		row[x] = (uint8_t)value;
		return;
	}

	/* A four-byte pixel is one 32-bit store through the inverting window. */
	*(volatile uint32_t *)(row + (size_t)x * 4U) = value;
}

/*
 * Reads one of the HAL's framebuffer names.
 */
static const uint64_t *
fb_name(
	const char *name)
{
	const uint64_t *value;

	/* Asks the platform for the named word. */
	value = kern_boot_handoff(name);

	/* Reports the word, NULL when the platform does not name it. */
	return value;
}

#if CONFIG_DRIVER_GRAPHICS_DEVICE
/* Tells /dev/graphics whether the framebuffer can be drawn. */
static int
backend_ready(
	void)
{
	int ready;

	/* The backend is ready once the framebuffer is mapped. */
	ready = drv_sun4u_fb_ready();

	/* Reports whether it is. */
	return ready;
}

/* Reports the firmware's one mode. */
static size_t
backend_get_modes(
	struct graphics_mode_info *modes,
	size_t capacity)
{
	/* Nothing without a mapping. */
	if (fb_pixels == NULL)
		return 0;

	/* Describes the mode when the caller has room for it. */
	if (modes != NULL && capacity != 0) {
		modes[0].width = fb_width;
		modes[0].height = fb_height;
		modes[0].bits_per_pixel = 32U;
		if (fb_format == KERN_BOOT_FRAMEBUFFER_RGB332)
			modes[0].bits_per_pixel = 8U;
		modes[0].stride = fb_pitch;
	}

	/* Succeeded: there is one mode. */
	return 1;
}

/* Enters the firmware's mode, whatever was asked for. */
static int
backend_enter(
	struct graphics_mode *mode)
{
	/* Refuses without a mapping. */
	if (fb_pixels == NULL || mode == NULL)
		return 0;

	/* Reports the one mode. */
	mode->width = fb_width;
	mode->height = fb_height;
	mode->bits_per_pixel = 32U;
	if (fb_format == KERN_BOOT_FRAMEBUFFER_RGB332)
		mode->bits_per_pixel = 8U;
	mode->stride = fb_pitch;

	/* Succeeded: the screen is the owner's. */
	return 1;
}

/* Leaves graphics: the firmware's mode stays, the console repaints it. */
static void
backend_leave(
	void)
{
}

/* Fills a rectangle. */
static int
backend_fill(
	const struct graphics_rect *rect,
	uint32_t color)
{
	/* Refuses a missing rectangle. */
	if (rect == NULL)
		return 0;

	/* Fills it. */
	drv_sun4u_fb_fill(rect->x, rect->y, rect->width, rect->height, color);

	/* Succeeded. */
	return 1;
}

/* Draws a line from one point to another, both included. */
static int
backend_line(
	unsigned x0,
	unsigned y0,
	unsigned x1,
	unsigned y1,
	uint32_t color)
{
	uint32_t value;
	int x;
	int y;
	int dx;
	int dy;
	int sx;
	int sy;
	int error;
	int twice;

	/* Refuses without a mapping. */
	if (fb_pixels == NULL)
		return 0;

	/* Sets up Bresenham's walk from the first point to the second. */
	value = fb_value(color);
	x = (int)x0;
	y = (int)y0;
	dx = (int)x1 - x;
	sx = 1;
	if (dx < 0) {
		dx = -dx;
		sx = -1;
	}
	dy = (int)y1 - y;
	sy = 1;
	if (dy < 0) {
		dy = -dy;
		sy = -1;
	}
	dy = -dy;
	error = dx + dy;

	/* Steps toward the second point, a pixel at a time. */
	for (;;) {
		fb_put((unsigned)x, (unsigned)y, value);

		/* Stops on the second point. */
		if (x == (int)x1 && y == (int)y1)
			break;

		/* Steps across when the error says so. */
		twice = error * 2;
		if (twice >= dy) {
			error += dy;
			x += sx;
		}

		/* Steps down or up when the error says so. */
		if (twice <= dx) {
			error += dx;
			y += sy;
		}
	}

	/* Succeeded. */
	return 1;
}

/* Fills the set pixels of a pattern over a rectangle. */
static int
backend_pattern_fill(
	const struct graphics_rect *rect,
	uint32_t color,
	uint64_t pattern)
{
	uint32_t value;
	unsigned column;
	unsigned row;
	int set;

	/* Refuses a missing rectangle or a missing mapping. */
	if (rect == NULL || fb_pixels == NULL)
		return 0;

	/* Stores the color where the pattern, anchored at the corner, is set. */
	value = fb_value(color);
	for (row = 0; row < rect->height; row++) {
		for (column = 0; column < rect->width; column++) {
			set = drv_graphics_pattern_bit(pattern, column, row);
			if (set)
				fb_put(rect->x + column, rect->y + row, value);
		}
	}

	/* Succeeded. */
	return 1;
}

/* Draws one chunk of one row of an image. */
static int
backend_blit_row(
	unsigned x,
	unsigned y,
	const struct drv_graphics_row *row,
	uint64_t pattern,
	int patterned)
{
	const uint8_t *source;
	uint32_t color;
	unsigned column;
	unsigned index;
	int set;

	/* Refuses a missing row or a missing mapping. */
	if (row == NULL || fb_pixels == NULL)
		return 0;

	/* Draws each pixel the pattern lets through. */
	for (column = 0; column < row->width; column++) {
		if (patterned) {
			set = drv_graphics_pattern_bit(pattern, row->pattern_x + column, row->pattern_y);
			if (!set)
				continue;
		}

		/* Takes the pixel's color, a palette's out-of-range index as black. */
		if (row->format == KERN_GRAPHICS_FORMAT_RGB24) {
			source = row->pixels + (size_t)column * 3U;
			color = ((uint32_t)source[0] << 16) |
				((uint32_t)source[1] << 8) |
				source[2];
		} else {
			index = row->pixels[column];
			color = 0;
			if (index < row->palette_size)
				color = row->palette[index];
		}

		fb_put(x + column, y, fb_value(color));
	}

	/* Succeeded. */
	return 1;
}

/* Nothing to push: every store already reached the screen. */
static int
backend_flush(
	const struct graphics_rect *rects,
	size_t count)
{
	(void)rects;
	(void)count;

	/* Succeeded. */
	return 1;
}

/* Copies one glyph of the 8x16 font, a question mark beyond ASCII. */
static int
backend_get_glyph(
	uint32_t codepoint,
	uint8_t bitmap[32],
	unsigned *width,
	unsigned *height)
{
	/* Refuses missing outputs. */
	if (bitmap == NULL || width == NULL || height == NULL)
		return 0;

	/* Falls back to a question mark outside the ASCII half. */
	if (codepoint >= SUN4U_FB_GLYPHS)
		codepoint = '?';

	/* Copies the glyph's sixteen lines into a cleared bitmap. */
	kern_memset(bitmap, 0, 32);
	kern_memcpy(bitmap, &drv_pcat_vgafont16[codepoint * SUN4U_FB_GLYPH_HEIGHT], SUN4U_FB_GLYPH_HEIGHT);
	*width = SUN4U_FB_GLYPH_WIDTH;
	*height = SUN4U_FB_GLYPH_HEIGHT;

	/* Succeeded. */
	return 1;
}
#endif
