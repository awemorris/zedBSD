/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The virt board's framebuffer.
 *
 * The emulator's RAM framebuffer device shows a block of guest memory as
 * the screen once the guest writes the block's address and format to the
 * device's configuration file.  The block lives in the kernel image, 640
 * by 400 pixels of 32 bits, so that 80 by 25 cells of the 8x16 font fill
 * it exactly and a text console needs no margin.
 */

#include <hal/hal.h>
#include "../space.h"
#include "fwcfg.h"
#include "font.h"
#include "framebuffer.h"

/* The screen: its size in pixels, the bytes of a row, and of the whole. */
#define FRAMEBUFFER_WIDTH	640U
#define FRAMEBUFFER_HEIGHT	400U
#define FRAMEBUFFER_STRIDE	(FRAMEBUFFER_WIDTH * 4U)
#define FRAMEBUFFER_PIXELS	(FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT)

/*
 * The pixel format: 32 bits, the top byte unused, then red, green and
 * blue, as the DRM fourcc code XR24 names it.
 *
 * Values referenced: Linux 6.8.12 include/uapi/drm/drm_fourcc.h (the
 * XRGB8888 fourcc code, the characters "XR24").
 */
#define FRAMEBUFFER_FOURCC_XRGB8888	0x34325258U

/* The configuration file of the RAM framebuffer device. */
#define FRAMEBUFFER_CONFIG_FILE		"etc/ramfb"

/*
 * The device's configuration, big-endian and without padding.
 *
 * Values referenced: QEMU v10.0.0 hw/display/ramfb.c (the configuration's
 * fields: a 64-bit address, then 32-bit fourcc, flags, width, height and
 * stride, 28 bytes, all big-endian).
 */
struct framebuffer_config {
	uint64_t address;
	uint32_t fourcc;
	uint32_t flags;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
} __attribute__((packed));

_Static_assert(sizeof(struct framebuffer_config) == 28U, "the device reads 28 bytes");

/*
 * The screen's memory, which the emulator reads as the display.
 *
 * It is in the kernel image so that its physical address is fixed from the
 * start, and page-aligned as the device asks.  The kernel's own console
 * draws into it after the HAL's early console.
 */
static uint32_t framebuffer_pixels[FRAMEBUFFER_PIXELS] __attribute__((aligned(4096)));

/*
 * The configuration written to the device, kept in the image so the device
 * can read it by physical address.  Written once on the boot hart.
 */
static struct framebuffer_config framebuffer_config_block __attribute__((aligned(16)));

/*
 * Nonzero once the device shows the memory; until then nothing is drawn.
 * Set once on the boot hart before any other hart starts.
 */
static int framebuffer_on;

/*
 * The colours of the sixteen text attributes, as XRGB8888 values.
 *
 * Values referenced: the IBM VGA text-mode palette, the sixteen default
 * attribute colours.
 */
static const uint32_t framebuffer_palette[16] = {
	0x000000U, 0x0000aaU, 0x00aa00U, 0x00aaaaU,
	0xaa0000U, 0xaa00aaU, 0xaa5500U, 0xaaaaaaU,
	0x555555U, 0x5555ffU, 0x55ff55U, 0x55ffffU,
	0xff5555U, 0xff55ffU, 0xffff55U, 0xffffffU
};

static void fill_black(void);

/*
 * Points the RAM framebuffer device at the screen's memory.
 *
 * Returns HAL_OK once the device shows it, or why it does not: there is no
 * configuration interface, or no such device.
 */
int
virt_framebuffer_init(
	void)
{
	uint16_t selector;
	uint32_t size;
	int error;

	/* Finds the device's configuration file; a machine without the device has none. */
	error = virt_fwcfg_find(FRAMEBUFFER_CONFIG_FILE, &selector, &size);
	if (error != HAL_OK)
		return error;
	if (size != sizeof(framebuffer_config_block))
		return HAL_ERR_UNSUPPORTED;

	/* Starts from a black screen before the device shows it. */
	fill_black();

	/* Describes the memory: where it is, its format, size and row length. */
	framebuffer_config_block.address = __builtin_bswap64(riscv32_virt_to_phys(framebuffer_pixels));
	framebuffer_config_block.fourcc = __builtin_bswap32(FRAMEBUFFER_FOURCC_XRGB8888);
	framebuffer_config_block.flags = 0;
	framebuffer_config_block.width = __builtin_bswap32(FRAMEBUFFER_WIDTH);
	framebuffer_config_block.height = __builtin_bswap32(FRAMEBUFFER_HEIGHT);
	framebuffer_config_block.stride = __builtin_bswap32(FRAMEBUFFER_STRIDE);

	/* Writes the description, after which the device shows the memory. */
	error = virt_fwcfg_write(selector, &framebuffer_config_block, sizeof(framebuffer_config_block));
	if (error != HAL_OK)
		return error;

	/* Succeeded: drawing reaches the screen. */
	framebuffer_on = 1;
	return HAL_OK;
}

/*
 * Reports whether the screen is shown.
 */
int
virt_framebuffer_ready(
	void)
{
	/* Succeeded: whether virt_framebuffer_init() succeeded. */
	return framebuffer_on;
}

/*
 * Draws one text cell: a glyph in the attribute's colours.
 */
void
virt_framebuffer_cell(
	unsigned row,
	unsigned column,
	int character,
	uint8_t attribute)
{
	const uint8_t *glyph;
	uint32_t foreground;
	uint32_t background;
	uint32_t value;
	unsigned line;
	unsigned bit;
	uint8_t bits;
	size_t first;

	/* Nothing to draw before the screen is shown, or off the grid. */
	if (!framebuffer_on)
		return;
	if (row >= VIRT_CONSOLE_ROWS || column >= VIRT_CONSOLE_COLUMNS)
		return;

	/* The glyph of the character (the font covers every byte value) and its colours. */
	glyph = &virt_font8x16[((unsigned)character & 0xffU) * VIRT_FONT_HEIGHT];
	foreground = framebuffer_palette[attribute & 15U];
	background = framebuffer_palette[(attribute >> 4) & 15U];
	first = (size_t)row * VIRT_FONT_HEIGHT * FRAMEBUFFER_WIDTH + (size_t)column * 8U;

	/* Each row of the glyph, from its leftmost pixel in the high bit. */
	for (line = 0; line < VIRT_FONT_HEIGHT; line++) {
		bits = glyph[line];
		for (bit = 0; bit < 8U; bit++) {
			value = background;
			if ((bits & (0x80U >> bit)) != 0)
				value = foreground;
			framebuffer_pixels[first + (size_t)line * FRAMEBUFFER_WIDTH + bit] = value;
		}
	}
}

/*
 * Draws the cursor: the two bottom pixel rows of a cell in white.
 */
void
virt_framebuffer_cursor(
	unsigned row,
	unsigned column)
{
	unsigned line;
	unsigned bit;
	size_t first;

	/* Nothing to draw before the screen is shown, or off the grid. */
	if (!framebuffer_on)
		return;
	if (row >= VIRT_CONSOLE_ROWS || column >= VIRT_CONSOLE_COLUMNS)
		return;

	/* Whitens rows 14 and 15 of the cell. */
	first = (size_t)row * VIRT_FONT_HEIGHT * FRAMEBUFFER_WIDTH + (size_t)column * 8U;
	for (line = VIRT_FONT_HEIGHT - 2U; line < VIRT_FONT_HEIGHT; line++) {
		for (bit = 0; bit < 8U; bit++)
			framebuffer_pixels[first + (size_t)line * FRAMEBUFFER_WIDTH + bit] = framebuffer_palette[15];
	}
}

/*
 * Writes the screen's address and format to the boot log.
 */
void
virt_framebuffer_describe(
	void)
{
	/* Describes a shown screen; a missing one was reported when it was looked for. */
	if (!framebuffer_on)
		return;
	hal_printf("RISCV32 FRAMEBUFFER %ux%u phys=%lx\n",
		   FRAMEBUFFER_WIDTH,
		   FRAMEBUFFER_HEIGHT,
		   (unsigned long)riscv32_virt_to_phys(framebuffer_pixels));
}

/* Paints the whole screen black. */
static void
fill_black(
	void)
{
	size_t index;

	/* Clears every pixel. */
	for (index = 0; index < FRAMEBUFFER_PIXELS; index++)
		framebuffer_pixels[index] = 0;
}
