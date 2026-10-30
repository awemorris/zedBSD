/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The framebuffer the firmware or the bootloader scans out, painted with
 * 80x25 text cells of the 8x16 font in the middle 640x400 pixels of the
 * screen.  Where it comes from is the board's (arm64_bsp_framebuffer_init():
 * the firmware's mailbox on a Raspberry Pi, the device tree's
 * simple-framebuffer on an S905X5M board).
 *
 * Until the kernel's tables are switched to, the painting goes through
 * the boot tables' direct map.  From then on it goes through the early
 * console's own window of the framebuffer (space.c), which the kernel's
 * console takes over and removes once it prints; the painting stops for
 * good then.
 */

#include <hal/hal.h>
#include "bsp.h"
#include "defs.h"
#include "font.h"
#include "framebuffer.h"
#include "led.h"

/* The smallest framebuffer the console fits in. */
#define MIN_WIDTH 640U
#define MIN_HEIGHT 400U

/* The console's grid, and the rows of the cursor's underline in a cell. */
#define CELL_COLUMNS 80U
#define CELL_ROWS 25U
#define CURSOR_FIRST_ROW 14U

/*
 * Where the framebuffer is painted: the boot tables' direct map from
 * arm64_framebuffer_adopt(), the early console's window from
 * arm64_framebuffer_move(), both on the boot CPU before any other runs, and
 * NULL before the firmware or the bootloader has given one and again once
 * arm64_framebuffer_forget() gave it up.  Every painter reads it once and
 * paints through what it read.
 */
static volatile uint8_t *pixels;

/*
 * Whether the view in pixels is cached, so that every painting is cleaned
 * out to the memory the display reads; a device view is not.  Set with
 * pixels.
 */
static int pixels_cached;

/*
 * Whether the framebuffer was given up to the kernel: nothing paints it
 * again.  Set once by arm64_framebuffer_forget().
 */
static int pixels_given_up;

/*
 * The framebuffer's memory and shape.  Filled once by
 * arm64_framebuffer_adopt() on the boot CPU and not changed afterwards.
 */
static uint64_t pixels_phys;
static uint64_t pixels_size;
static uint32_t width;
static uint32_t height;
static uint32_t pitch;

/* The firmware's pixel order: 1 when red is in the low byte, 0 when blue is. */
static uint32_t order;

/*
 * The bits every pixel carries set: the alpha byte of a format whose
 * plane blends by it, so that the console's pixels are opaque; 0 when the
 * top byte is not looked at.
 */
static uint32_t opaque;

/* The top-left pixel of the text grid, which is centred on the screen. */
static uint32_t x_origin;
static uint32_t y_origin;

/* The sixteen text colours as 0xRRGGBB, in the PC's attribute order. */
static const uint32_t palette[16] = {
	0x000000, 0x0000aa, 0x00aa00, 0x00aaaa, 0xaa0000, 0xaa00aa, 0xaa5500, 0xaaaaaa,
	0x555555, 0x5555ff, 0x55ff55, 0x55ffff, 0xff5555, 0xff55ff, 0xffff55, 0xffffff
};

static uint32_t colour(uint8_t index);
static volatile uint8_t *paint_target(void);
static void put_pixel(volatile uint8_t *target, unsigned x, unsigned y, uint32_t value);
static void flush_rows(volatile uint8_t *target, unsigned x, unsigned y, unsigned columns, unsigned rows);
static void fill_screen(uint32_t value);
static void flush_screen(void);

/*
 * Paints one character cell: a glyph of the 8x16 console font in the
 * attribute's foreground on its background.
 */
void
arm64_framebuffer_cell(
	unsigned row,
	unsigned column,
	int character,
	uint8_t attribute)
{
	volatile uint8_t *target;
	const uint8_t *glyph;
	uint32_t foreground;
	uint32_t background;
	uint32_t value;
	unsigned x;
	unsigned y;
	unsigned line;
	unsigned bit;
	uint8_t bits;

	/* Nothing to paint before the framebuffer exists or once it was given up. */
	target = paint_target();
	if (target == NULL)
		return;

	/* Nothing to paint off the grid. */
	if (row >= CELL_ROWS || column >= CELL_COLUMNS)
		return;

	/* The glyph of the character (the font covers every byte value). */
	glyph = &arm64_font8x16[((unsigned)character & 0xffU) * ARM64_FONT_HEIGHT];
	foreground = colour(attribute & 15U);
	background = colour(attribute >> 4);
	x = x_origin + column * 8U;
	y = y_origin + row * ARM64_FONT_HEIGHT;

	/* Each row, from its leftmost pixel in the high bit. */
	for (line = 0; line < ARM64_FONT_HEIGHT; line++) {
		bits = glyph[line];

		/* Each pixel of the row is the foreground where its bit is set. */
		for (bit = 0; bit < 8U; bit++) {
			value = background;
			if ((bits & (0x80U >> bit)) != 0)
				value = foreground;
			put_pixel(target, x + bit, y + line, value);
		}
	}

	/* The cell, out to the memory the display reads. */
	flush_rows(target, x, y, 8U, ARM64_FONT_HEIGHT);
}

/*
 * Draws the cursor: an underline in the two bottom rows of a cell.
 *
 * Only a visible cursor is drawn; the cell's own repaint removes it.
 */
void
arm64_framebuffer_cursor(
	unsigned row,
	unsigned column,
	int visible)
{
	volatile uint8_t *target;
	unsigned x;
	unsigned y;
	unsigned cell_x;
	unsigned cell_y;

	/* Nothing to draw before the framebuffer exists or once it was given up, for a hidden cursor, or off the grid. */
	target = paint_target();
	if (target == NULL)
		return;
	if (!visible)
		return;
	if (row >= CELL_ROWS || column >= CELL_COLUMNS)
		return;

	/* Paints the underline white. */
	cell_x = x_origin + column * 8U;
	cell_y = y_origin + row * ARM64_FONT_HEIGHT;
	for (y = CURSOR_FIRST_ROW; y < ARM64_FONT_HEIGHT; y++) {
		for (x = 0; x < 8U; x++)
			put_pixel(target, cell_x + x, cell_y + y, colour(15));
	}

	/* The underline, out to the memory the display reads. */
	flush_rows(target, cell_x, cell_y + CURSOR_FIRST_ROW, 8U, ARM64_FONT_HEIGHT - CURSOR_FIRST_ROW);
}

/*
 * Takes a 32-bit framebuffer the firmware or the bootloader scans out,
 * and clears it.
 *
 * The buffer must hold at least the console's 640x400 pixels in rows of
 * four bytes a pixel; order is 1 when red is in a pixel's low byte and 0
 * when blue is, and opaque_bits are set in every pixel (the alpha byte of
 * a plane that blends by it).  Returns 0 when the framebuffer is ready.
 */
int
arm64_framebuffer_adopt(
	uint64_t phys,
	uint64_t size,
	uint32_t pixel_width,
	uint32_t pixel_height,
	uint32_t row_pitch,
	uint32_t pixel_order,
	uint32_t opaque_bits)
{
	/* Refuses a framebuffer too small for the console. */
	if (pixel_width < MIN_WIDTH || pixel_height < MIN_HEIGHT)
		return -1;

	/* Refuses rows shorter than a row of pixels, or a buffer shorter than the rows. */
	if (row_pitch < pixel_width * 4U)
		return -1;
	if (size < (uint64_t)row_pitch * pixel_height)
		return -1;

	/* Refuses a missing buffer, or one that wraps around the address space. */
	if (phys == 0)
		return -1;
	if (phys + size < phys)
		return -1;

	/* Keeps the description for the painting. */
	pixels_phys = phys;
	pixels_size = size;
	width = pixel_width;
	height = pixel_height;
	pitch = row_pitch;
	order = pixel_order;
	opaque = opaque_bits;

	/* Reaches the framebuffer through the boot tables' direct map, cached, with the text grid centred. */
	pixels = (volatile uint8_t *)(ARM64_DIRECT_BASE + pixels_phys);
	pixels_cached = 1;
	x_origin = (width - MIN_WIDTH) / 2U;
	y_origin = (height - MIN_HEIGHT) / 2U;

	/* Hands the framebuffer to the kernel and keeps it out of the allocator. */
	arm64_boot_set_framebuffer(pixels_phys, pixels_size, width, height, pitch, order);

	/* Clears the screen to black. */
	fill_screen(colour(0));
	flush_screen();

	/* Succeeded: the framebuffer is ready for the console. */
	return 0;
}

/*
 * Reports whether the firmware's framebuffer is set up.
 */
int
arm64_framebuffer_ready(
	void)
{
	/* Succeeded: whether there is a framebuffer to paint. */
	return pixels != 0;
}

/*
 * Reports the framebuffer's memory: 1 with its physical base and size,
 * or 0 when the firmware or the bootloader gave none.
 */
int
arm64_framebuffer_extent(
	uint64_t *phys,
	uint64_t *size)
{
	/* No framebuffer was adopted. */
	if (pixels_size == 0)
		return 0;

	/* Succeeded: the memory arm64_framebuffer_adopt() took. */
	*phys = pixels_phys;
	*size = pixels_size;
	return 1;
}

/*
 * Paints the framebuffer through a new view from now on: the early
 * console's window, cached or not.  Called on the boot CPU, before any
 * other CPU runs, once the kernel's tables hold the view.
 */
void
arm64_framebuffer_move(
	volatile uint8_t *address,
	int cached)
{
	/* Nothing to move before a framebuffer exists. */
	if (pixels == 0)
		return;

	/* The new view and whether its paintings need cleaning. */
	pixels_cached = cached;
	__atomic_store_n(&pixels, address, __ATOMIC_RELEASE);
}

/*
 * Gives the framebuffer up: nothing paints it again.  The early console
 * calls it under its output lock before the window goes, so no character
 * is half painted then; only a stall report that read the view just
 * before could still be painting through it.
 */
void
arm64_framebuffer_forget(
	void)
{
	/* No painting reaches the framebuffer from now on. */
	pixels_given_up = 1;
	__atomic_store_n(&pixels, NULL, __ATOMIC_RELEASE);
}

/*
 * Reports whether a physical range meets the framebuffer once it was given
 * up, so that nothing paints there: 1 when it does, 0 otherwise.
 */
int
arm64_framebuffer_given_up(
	uint64_t phys,
	uint64_t size)
{
	int given_up;

	/* A framebuffer still held, or never given, keeps every range paintable. */
	given_up = __atomic_load_n(&pixels_given_up, __ATOMIC_ACQUIRE);
	if (!given_up)
		return 0;

	/* A range wholly before or wholly after the framebuffer does not meet it. */
	if (phys + size <= pixels_phys)
		return 0;
	if (phys >= pixels_phys + pixels_size)
		return 0;

	/* Succeeded: the range meets the given-up framebuffer. */
	return 1;
}

/*
 * Shows a test pattern for three seconds: the left third red, the middle
 * green and the right blue, with a white frame 16 pixels wide.  Seen on a
 * board, it tells whether the display shows what the CPU writes, where, and
 * in which colour order.
 */
void
arm64_framebuffer_test_pattern(
	void)
{
	uint32_t value;
	unsigned x;
	unsigned y;

	/* Nothing to show without a framebuffer. */
	if (pixels == 0)
		return;

	/* Each pixel: the frame, or the colour of its third. */
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			value = colour(12);
			if (x >= width / 3U)
				value = colour(10);
			if (x >= width / 3U * 2U)
				value = colour(9);
			if (x < 16U || y < 16U || x >= width - 16U || y >= height - 16U)
				value = colour(15);
			put_pixel(pixels, x, y, value);
		}
	}

	/* Out to memory, then time to look at it. */
	flush_screen();
	arm64_delay_ms(3000U);

	/* Black again for the console. */
	fill_screen(colour(0));
	flush_screen();
}

/*
 * Reports what the firmware gave for the framebuffer.
 */
void
arm64_framebuffer_describe(
	void)
{
	/* The size, the row length, the order and where it is. */
	hal_printf(ARM64_BOARD_TAG " FRAMEBUFFER %ux%u pitch=%u order=%u phys=%llx size=%llx\n",
		   width,
		   height,
		   pitch,
		   order,
		   (unsigned long long)pixels_phys,
		   (unsigned long long)pixels_size);
}

/* Turns a text colour into a pixel in the firmware's byte order. */
static uint32_t
colour(
	uint8_t index)
{
	uint32_t rgb;
	uint32_t swapped;

	/* The colour as 0xRRGGBB. */
	rgb = palette[index & 15U];

	/* Red in the low byte is the palette's own order. */
	if (order)
		return rgb | opaque;

	/* Succeeded: blue in the low byte, red and blue exchanged. */
	swapped = ((rgb & 0xffU) << 16) | (rgb & 0xff00U) | ((rgb >> 16) & 0xffU);
	return swapped | opaque;
}

/* Reads the view to paint through once: NULL before a framebuffer exists or once it was given up. */
static volatile uint8_t *
paint_target(
	void)
{
	volatile uint8_t *target;

	/* The view the last move or the adoption published. */
	target = __atomic_load_n(&pixels, __ATOMIC_ACQUIRE);

	/* Succeeded: the view, or NULL. */
	return target;
}

/* Writes one pixel through a view of the framebuffer. */
static void
put_pixel(
	volatile uint8_t *target,
	unsigned x,
	unsigned y,
	uint32_t value)
{
	volatile uint32_t *pixel;

	/* The pixel's word: its row, then its column of four bytes. */
	pixel = (volatile uint32_t *)(target + (size_t)y * pitch + (size_t)x * 4U);
	*pixel = value;
}

/*
 * Makes rows of pixels reach memory.  A cached view keeps what the CPU
 * wrote in the data cache, and the display reads memory past the CPU's
 * caches, so a pixel that stays there is never shown.
 */
static void
flush_rows(
	volatile uint8_t *target,
	unsigned x,
	unsigned y,
	unsigned columns,
	unsigned rows)
{
	uintptr_t start;
	unsigned line;

	/* Each row's bytes of a cached view, cleaned from the data cache to memory. */
	if (pixels_cached) {
		for (line = 0; line < rows; line++) {
			start = (uintptr_t)(target + (size_t)(y + line) * pitch + (size_t)x * 4U);
			hal_dcache_clean_range(start, (size_t)columns * 4U);
		}
	}

	/* The display is outside the CPU's shareable domain: a full barrier. */
	__asm__ volatile("dsb sy" ::: "memory");
}

/* Sets every pixel of the screen to one value. */
static void
fill_screen(
	uint32_t value)
{
	unsigned x;
	unsigned y;

	/* Each pixel, row by row. */
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++)
			put_pixel(pixels, x, y, value);
	}
}

/* Makes the whole framebuffer reach memory. */
static void
flush_screen(
	void)
{
	/* Cleans the buffer from the data cache, then a full barrier for the display. */
	hal_dcache_clean_range((uintptr_t)pixels, (size_t)pixels_size);
	__asm__ volatile("dsb sy" ::: "memory");
}
