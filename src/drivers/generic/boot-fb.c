/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The framebuffer the firmware or the boot loader left on the screen, for
 * a board without its own display driver.
 *
 * The board reads the screen the platform hands over and gives it here;
 * this driver maps it once and draws into it: the text console's cells
 * and, with CONFIG_DRIVER_GRAPHICS_DEVICE, the /dev/graphics backend at
 * the firmware's (lowest) priority, so a later display driver can take
 * the screen over.
 *
 * The end gives the screen up for good: the driver's own mapping, and the
 * mapping of exactly the screen the kernel hands over once the console
 * prints through the kernel (the hardware layer's early console drew
 * through it), both removed, as the PC/AT graphics backend does.
 *
 * Every pixel is four bytes, blue or red in the low byte.  The kernel may
 * give the framebuffer's memory back through its cached view, and the
 * display reads memory past the processor's caches, so every drawing ends
 * by writing the rows it touched back to memory.  The firmware decided
 * the one mode; nothing here programs the display.
 */

#include <stddef.h>
#include <stdint.h>

#include <drivers/generic/boot-fb.h>
#include "drivers/platform/pcat/graphics/vgafont.h"

#include <uapi/errno.h>
#include <uapi/graphics.h>

#include <kern/boot.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/graphics-backend.h>
#include <kern/kcrt.h>
#include <kern/lock.h>
#include <kern/platform.h>
#include <kern/pmem.h>
#include <kern/text-display.h>

#ifdef KERN_TEST_TEXT_END_SECONDS
#include <kern/clock.h>
#include <kern/klog.h>
#include <kern/sched.h>
#include <kern/thread.h>
#endif

/* The bytes of one pixel. */
#define BOOT_FB_PIXEL_BYTES	4U

/* The glyphs /dev/graphics hands out: the 8x16 font's ASCII half. */
#define BOOT_FB_GLYPHS		128U
#define BOOT_FB_GLYPH_WIDTH	8U
#define BOOT_FB_GLYPH_HEIGHT	16U

/* The largest screen the driver accepts, the interface's own limit. */
#define BOOT_FB_MAX_SIDE	16384U

static void fb_end_lock_init_once(void);
static void fb_end_remove(void *mapping, size_t bytes);
static void fb_end_leave(int error);
#ifdef KERN_TEST_TEXT_END_SECONDS
static void fb_end_test_start(void);
static void fb_end_test(void *argument);
#endif
static uint32_t fb_value(uint32_t color);
static void fb_put(unsigned x, unsigned y, uint32_t value);
static void fb_flush_row(unsigned x, unsigned y, unsigned width);

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
static const struct drv_graphics_backend boot_fb_backend = {
	.name = "firmware framebuffer",
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
	.end = drv_boot_fb_end
};
#endif

/*
 * The mapped framebuffer and its shape, set once by drv_boot_fb_init()
 * from the boot thread before the console or /dev/graphics use them, and
 * only read afterwards.  fb_pixels is NULL until the mapping succeeded and
 * again once the end gave it back; fb_mapping, fb_physical and fb_bytes
 * are the mapping the end gives back.
 */
static volatile uint8_t *fb_pixels;
static void *fb_mapping;
static uint64_t fb_physical;
static size_t fb_bytes;

/*
 * The whole screen's memory as the platform handed it over, which the end
 * asks the kernel for to remove the early console's view.  Set once by
 * drv_boot_fb_init() once the screen is found drawable, mapped or not; 0
 * before.
 */
static uint64_t fb_screen_bytes;
static unsigned fb_width;
static unsigned fb_height;
static unsigned fb_pitch;
static unsigned fb_format;

/*
 * The bits every pixel carries set, given once by drv_boot_fb_init() with
 * the screen->
 */
static uint32_t fb_opaque;

/*
 * Serializes the end: the first caller gives the mapping back, and a later
 * one waits for it before judging.  Initialized once by the first
 * drv_boot_fb_init(), on the boot thread; fb_end_lock_ready says it was.
 */
static struct mutex fb_end_lock;
static int fb_end_lock_ready;

/*
 * Whether the end has run: nothing is drawn or mapped again.  Written once
 * under fb_end_lock (or, with no lock yet, before anything was mapped).
 */
static int fb_ended;

/*
 * Whether the end left a mapping of the screen in place, and the error of
 * the removal that failed (0 for one that cannot be removed, which the
 * end reports as EBUSY).  Written once by the first end under
 * fb_end_lock, read by every later one under it.
 */
static int fb_left;
static int fb_left_error;

#ifdef KERN_TEST_TEXT_END_SECONDS
/*
 * The console's end as a display driver's first flip asks for it,
 * KERN_TEST_TEXT_END_SECONDS after the screen is mapped: what
 * kern_text_end() returned (fb_end_test_result), and 1 once it has
 * returned (fb_end_test_done), written once by the thread that asks, and
 * the address of the mapping of exactly the screen the end was handed and
 * removed (fb_end_test_handed), written once by the end.  Nothing in the
 * kernel reads them; they are volatile so that the stores and the symbols
 * stay in the image for a debugger.
 */
static volatile int fb_end_test_result;
static volatile int fb_end_test_done;
static void *volatile fb_end_test_handed;

/* Whether the thread that ends the console was started; written by the first mapping only. */
static int fb_end_test_started;
#endif

/*
 * Maps the screen a board read.
 */
int
drv_boot_fb_init(
	const struct kern_boot_framebuffer *screen,
	uint32_t opaque)
{
	uint64_t rows_bytes;
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

	/* Without a screen there is nothing to map. */
	if (screen == NULL)
		return ENODEV;

	/* Takes only the two pixel orders the driver draws. */
	if (screen->format != KERN_BOOT_FRAMEBUFFER_BGRX8888 &&
	    screen->format != KERN_BOOT_FRAMEBUFFER_RGBX8888)
		return ENODEV;

	/* Refuses an empty or oversized screen-> */
	if (screen->width == 0 ||
	    screen->height == 0 ||
	    screen->width > BOOT_FB_MAX_SIDE ||
	    screen->height > BOOT_FB_MAX_SIDE)
		return ENODEV;

	/* Refuses rows shorter than the width, or a buffer shorter than the rows. */
	if (screen->pitch < screen->width * BOOT_FB_PIXEL_BYTES ||
	    screen->pitch > BOOT_FB_MAX_SIDE * BOOT_FB_PIXEL_BYTES)
		return ENODEV;
	rows_bytes = (uint64_t)screen->pitch * screen->height;
	if (screen->size < rows_bytes)
		return ENODEV;

	/* The screen's rows, which the end judges whether or not they are mapped. */
	fb_physical = screen->physical_base;
	fb_bytes = (size_t)rows_bytes;
	fb_screen_bytes = screen->size;

	/* Maps every row. */
	error = kern_device_map(screen->physical_base, (size_t)rows_bytes, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0)
		return error;

	/* Records the shape and the mapping, then publishes the mapping. */
	fb_width = screen->width;
	fb_height = screen->height;
	fb_pitch = screen->pitch;
	fb_format = screen->format;
	fb_opaque = opaque;
	fb_mapping = mapped;
	fb_pixels = mapped;

#ifdef KERN_TEST_TEXT_END_SECONDS
	/* The test build ends the console over the screen after its wait. */
	fb_end_test_start();
#endif

	/* Succeeded: the framebuffer can be drawn. */
	return 0;
}

/*
 * Tells whether the framebuffer is mapped.
 */
int
drv_boot_fb_ready(
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
drv_boot_fb_get_size(
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
drv_boot_fb_fill(
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

	/* Nothing to fill before the mapping or off the screen-> */
	if (fb_pixels == NULL || x >= fb_width || y >= fb_height)
		return;

	/* Clips the rectangle to the screen-> */
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

		/* The row, out to the memory the display reads. */
		fb_flush_row(x, row, right - x);
	}
}

/*
 * Draws one 8x16 glyph.
 */
void
drv_boot_fb_glyph(
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
	for (line = 0; line < BOOT_FB_GLYPH_HEIGHT; line++) {
		bits = glyph[line];
		for (bit = 0; bit < BOOT_FB_GLYPH_WIDTH; bit++) {
			if ((bits & (0x80U >> bit)) != 0)
				fb_put(x + bit, y + line, set);
			else
				fb_put(x + bit, y + line, clear);
		}

		/* The line, out to the memory the display reads. */
		fb_flush_row(x, y + line, BOOT_FB_GLYPH_WIDTH);
	}
}

/*
 * Offers the framebuffer to /dev/graphics.
 */
int
drv_boot_fb_register_backend(
	void)
{
#if CONFIG_DRIVER_GRAPHICS_DEVICE
	int error;

	/* Only a mapped framebuffer is offered. */
	if (fb_pixels == NULL)
		return ENODEV;

	/* Offers it at the lowest priority, so a display driver wins. */
	error = drv_graphics_backend_register(&boot_fb_backend, DRV_GRAPHICS_PRIORITY_FIRMWARE);
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
drv_boot_fb_end(
	const struct kern_text_range *ranges,
	unsigned count)
{
	void *mapping;
	void *handed;
	int overlaps;
	int error;

	/* Without the lock nothing was ever mapped, and nothing will be. */
	if (!fb_end_lock_ready) {
		fb_ended = 1;
		return 0;
	}

	/*
	 * The first end gives the mapping back; a later one waits here for it
	 * and only judges.
	 */
	mutex_lock(&fb_end_lock);

	/* Only the first end has a mapping to give back. */
	if (!fb_ended) {
		/*
		 * Nothing is drawn from now on.  The callers stopped every writer
		 * first: the console's under its own lock, /dev/graphics under the
		 * graphics lock this runs under when it draws here; a fatal stop's
		 * reveal sees the console ended and draws nothing.
		 */
		fb_ended = 1;
		mapping = fb_mapping;
		fb_pixels = NULL;
		fb_mapping = NULL;

		/* Gives the driver's own mapping back. */
		if (mapping != NULL)
			fb_end_remove(mapping, fb_bytes);

		/*
		 * The early console's mapping, handed over by a mapping of exactly
		 * the screen once the console prints through the kernel, and
		 * removed at once.  Without the hand-over the mapping made is an
		 * ordinary one, and it goes the same way.
		 */
		if (fb_screen_bytes != 0) {
			error = kern_device_map(fb_physical, (size_t)fb_screen_bytes, KERN_DEVICE_UNCACHED, &handed);
			if (error == 0) {
#ifdef KERN_TEST_TEXT_END_SECONDS
				fb_end_test_handed = handed;
#endif
				fb_end_remove(handed, (size_t)fb_screen_bytes);
			} else {
				fb_end_leave(error);
			}
		}
	}

	/* A screen left mapped counts against the caller's ranges. */
	error = 0;
	if (fb_left) {
		overlaps = kern_text_range_overlaps(ranges, count, fb_physical, fb_screen_bytes);
		if (overlaps) {
			error = EBUSY;
			if (fb_left_error != 0)
				error = fb_left_error;
		}
	}

	mutex_unlock(&fb_end_lock);

	/* Reports a screen left mapped in the ranges. */
	if (error != 0)
		return error;

	/* Succeeded: nothing of the framebuffer lies in the ranges. */
	return 0;
}

/*
 * Removes one mapping of the screen at the end, recording the screen as
 * left when the removal fails (EBUSY from the kernel: a mapping kept for
 * good).
 */
static void
fb_end_remove(
	void *mapping,
	size_t bytes)
{
	int error;

	/* Asks the kernel to remove it. */
	error = kern_device_unmap(mapping, bytes);

	/* A mapping kept for good is left without an error; another failure with its own. */
	if (error == EBUSY) {
		fb_end_leave(0);
	} else if (error != 0) {
		fb_end_leave(error);
	}
}

/*
 * Records the screen as left mapped by the end, keeping the first failed
 * removal's error (0 for a mapping kept for good).
 */
static void
fb_end_leave(
	int error)
{
	/* The first record keeps its error; a later one adds only an error the first lacked. */
	if (!fb_left || fb_left_error == 0)
		fb_left_error = error;
	fb_left = 1;
}

#ifdef KERN_TEST_TEXT_END_SECONDS
/* Starts, once a boot, the thread that ends the console after the configured wait. */
static void
fb_end_test_start(
	void)
{
	struct thread *thread;
	int error;

	/* Only the first mapping starts it. */
	if (fb_end_test_started)
		return;
	fb_end_test_started = 1;

	/* The thread, at the default priority. */
	error = kthread_create(fb_end_test, NULL, SCHED_PRIORITY_DEFAULT, &thread);
	if (error != 0) {
		kern_logf("boot framebuffer: the timed end did not start (%d)\n", error);
		return;
	}

	/* Lets it run. */
	thread_start(thread);
}

/*
 * Waits KERN_TEST_TEXT_END_SECONDS, then ends the console's use of the
 * display once over the screen's range, as a display driver's first flip
 * does, and keeps the result.
 */
static void
fb_end_test(
	void *argument)
{
	struct kern_text_range range;
	uint64_t deadline;
	int error;

	(void)argument;

	/* The wait, whole even if the sleep comes back early. */
	deadline = sched_ticks() + kern_ms_to_ticks((uint64_t)KERN_TEST_TEXT_END_SECONDS * 1000U);
	while (sched_ticks() < deadline)
		sched_sleep(deadline);

	/* Ends over the screen's range. */
	range.physical = fb_physical;
	range.bytes = fb_screen_bytes;
	error = kern_text_end(&range, 1U);

	/* The result, then the mark that it is there. */
	fb_end_test_result = error;
	__atomic_store_n(&fb_end_test_done, 1, __ATOMIC_RELEASE);
	kern_logf("boot framebuffer: the timed end returned %d, the screen's mapping at %p removed\n", error, fb_end_test_handed);
}
#endif

/* Initializes the end's lock on the first start. */
static void
fb_end_lock_init_once(
	void)
{
	/* Initializes the lock only once. */
	if (!fb_end_lock_ready) {
		(void)mutex_init(&fb_end_lock, LOCK_RANK_DEVICE, "boot framebuffer");
		fb_end_lock_ready = 1;
	}
}

/*
 * Converts a 0xRRGGBB color to the word fb_put() stores, in the screen's
 * byte order with the opaque bits set.
 */
static uint32_t
fb_value(
	uint32_t color)
{
	uint32_t value;

	/* Blue in the low byte is the color's own order. */
	value = color & 0x00ffffffU;

	/* Red in the low byte exchanges red and blue. */
	if (fb_format == KERN_BOOT_FRAMEBUFFER_RGBX8888) {
		value = ((color & 0x000000ffU) << 16) |
			(color & 0x0000ff00U) |
			((color & 0x00ff0000U) >> 16);
	}

	/* Sets the bits a plane that blends by alpha needs to show the pixel. */
	value |= fb_opaque;

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

	/* Clips to the screen-> */
	if (x >= fb_width || y >= fb_height)
		return;

	/* A pixel is one 32-bit store. */
	row = fb_pixels + (size_t)y * fb_pitch;
	*(volatile uint32_t *)(row + (size_t)x * BOOT_FB_PIXEL_BYTES) = value;
}

/*
 * Writes width pixels of a row from x back to the memory the display
 * reads, clipped to the screen->
 */
static void
fb_flush_row(
	unsigned x,
	unsigned y,
	unsigned width)
{
	volatile uint8_t *start;

	/* Nothing to write back off the screen-> */
	if (x >= fb_width || y >= fb_height || width == 0)
		return;

	/* Clips the run to the row. */
	if (width > fb_width - x)
		width = fb_width - x;

	/* Cleans the run from the data cache, then orders it before the display's reads. */
	start = fb_pixels + (size_t)y * fb_pitch + (size_t)x * BOOT_FB_PIXEL_BYTES;
	kern_dcache_clean_range((const void *)start, (size_t)width * BOOT_FB_PIXEL_BYTES);
	kern_io_barrier();
}

#if CONFIG_DRIVER_GRAPHICS_DEVICE
/* Tells /dev/graphics whether the framebuffer can be drawn. */
static int
backend_ready(
	void)
{
	int ready;

	/* The backend is ready once the framebuffer is mapped. */
	ready = drv_boot_fb_ready();

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
		modes[0].stride = fb_pitch;
	}

	/* Succeeded: there is one mode. */
	return 1;
}

/* Enters the firmware's mode, whatever was asked for, on a cleared screen-> */
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
	mode->stride = fb_pitch;

	/* Hands the owner a black screen, without the console's text. */
	drv_boot_fb_fill(0, 0, fb_width, fb_height, 0x000000U);

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
	/* Refuses a missing rectangle or a missing mapping. */
	if (rect == NULL || fb_pixels == NULL)
		return 0;

	/* Fills it. */
	drv_boot_fb_fill(rect->x, rect->y, rect->width, rect->height, color);

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
		fb_flush_row((unsigned)x, (unsigned)y, 1U);

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

		/* The row, out to the memory the display reads. */
		fb_flush_row(rect->x, rect->y + row, rect->width);
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

	/* The chunk, out to the memory the display reads. */
	fb_flush_row(x, y, row->width);

	/* Succeeded. */
	return 1;
}

/* Nothing to push: every drawing already reached memory. */
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
	if (codepoint >= BOOT_FB_GLYPHS)
		codepoint = '?';

	/* Copies the glyph's sixteen lines into a cleared bitmap. */
	kern_memset(bitmap, 0, 32);
	kern_memcpy(bitmap, &drv_pcat_vgafont16[codepoint * BOOT_FB_GLYPH_HEIGHT], BOOT_FB_GLYPH_HEIGHT);
	*width = BOOT_FB_GLYPH_WIDTH;
	*height = BOOT_FB_GLYPH_HEIGHT;

	/* Succeeded. */
	return 1;
}
#endif
