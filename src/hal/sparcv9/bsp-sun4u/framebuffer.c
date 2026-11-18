/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sun4u board's framebuffer.
 *
 * The screen is the firmware framebuffer the loader handed over: the
 * display OpenFirmware drew on, at its own size and depth, with pixels of
 * one of three formats.  The text grid of 80 by 25 cells of the 8x16 font
 * fills its top left corner.
 *
 * When the loader handed none over, the emulator's standard VGA is set up
 * directly: its Bochs display interface sets 640 by 400 pixels of 32 bits,
 * so that the grid fills it exactly.  The pixels and the registers are PCI
 * memory, little-endian, reached through the physical bypass; on a
 * big-endian machine the emulator reads the pixels big-endian unless told
 * otherwise, so the byte order is set first.
 *
 * Either way, the screen shown is what the HAL names to the kernel.
 */

#include <hal/hal.h>
#include "../asi.h"
#include "../bsp.h"
#include "font.h"
#include "framebuffer.h"

/* The screen the HAL sets up itself: its size in pixels, and the bytes of a row. */
#define FRAMEBUFFER_WIDTH	640U
#define FRAMEBUFFER_HEIGHT	400U
#define FRAMEBUFFER_DEPTH	32U
#define FRAMEBUFFER_STRIDE	(FRAMEBUFFER_WIDTH * 4U)

/* The PCI IDs of the emulator's standard VGA, the display the HAL sets up itself. */
#define FRAMEBUFFER_QEMU_VENDOR	0x1234U
#define FRAMEBUFFER_QEMU_DEVICE	0x1111U

/*
 * The register window of the standard VGA: the VGA ports from 0x3c0 at
 * offset 0x400, the Bochs display interface's registers, 16 bits each, at
 * offset 0x500, and the emulator's own registers at offset 0x600, of which
 * the second sets the byte order of the framebuffer's pixels.
 *
 * Values referenced: QEMU v10.0.0 hw/display/vga-pci.c
 * (PCI_VGA_IOPORT_OFFSET, PCI_VGA_BOCHS_OFFSET, PCI_VGA_QEXT_OFFSET,
 * PCI_VGA_QEXT_REG_BYTEORDER, PCI_VGA_QEXT_LITTLE_ENDIAN).
 */
#define FRAMEBUFFER_VGA_PORTS		0x400U
#define FRAMEBUFFER_BOCHS_REGISTERS	0x500U
#define FRAMEBUFFER_QEXT_BYTEORDER	0x604U
#define FRAMEBUFFER_QEXT_LITTLE_ENDIAN	0x1e1e1e1eU

/* The VGA attribute controller's port, relative to port 0x3c0. */
#define FRAMEBUFFER_VGA_ATTRIBUTE	0x00U

/*
 * The attribute controller's index value that lets the screen show.
 *
 * Values referenced: Linux 6.8.12 drivers/gpu/drm/tiny/bochs.c (the value
 * 0x20 written to port 0x3c0 to unblank the screen).
 */
#define FRAMEBUFFER_VGA_UNBLANK		0x20U

/*
 * The Bochs display interface's registers, by index, and the bits of its
 * enable register.
 *
 * Values referenced: QEMU v10.0.0 include/hw/display/bochs-vbe.h
 * (VBE_DISPI_INDEX_XRES, _YRES, _BPP, _ENABLE, _BANK, _VIRT_WIDTH,
 * _VIRT_HEIGHT, _X_OFFSET, _Y_OFFSET; VBE_DISPI_ENABLED,
 * VBE_DISPI_LFB_ENABLED).
 */
#define FRAMEBUFFER_BOCHS_XRES		1U
#define FRAMEBUFFER_BOCHS_YRES		2U
#define FRAMEBUFFER_BOCHS_BPP		3U
#define FRAMEBUFFER_BOCHS_ENABLE	4U
#define FRAMEBUFFER_BOCHS_BANK		5U
#define FRAMEBUFFER_BOCHS_VIRT_WIDTH	6U
#define FRAMEBUFFER_BOCHS_VIRT_HEIGHT	7U
#define FRAMEBUFFER_BOCHS_X_OFFSET	8U
#define FRAMEBUFFER_BOCHS_Y_OFFSET	9U
#define FRAMEBUFFER_BOCHS_ENABLED	0x01U
#define FRAMEBUFFER_BOCHS_LINEAR	0x40U

/*
 * The physical addresses of the first pixel and of the register window,
 * the screen's size in pixels, the bytes of a row and the
 * KERN_BOOT_FRAMEBUFFER_* format of a pixel.
 *
 * Set once by sun4u_framebuffer_init() on the boot processor; zero until
 * then.
 */
static uint64_t framebuffer_pixels;
static uint64_t framebuffer_registers;
static unsigned framebuffer_width;
static unsigned framebuffer_height;
static uint64_t framebuffer_pitch;
static uint32_t framebuffer_format;

/*
 * The screen shown, as the HAL names it to the kernel.  Filled once by
 * sun4u_framebuffer_init() when the screen shows, and not changed
 * afterwards.
 */
static struct sun4u_framebuffer_names framebuffer_shown;

/*
 * Nonzero once the screen shows; until then nothing is drawn.  Set once
 * by sun4u_framebuffer_init().
 */
static int framebuffer_on;

/*
 * The colours of the sixteen text attributes, as 0xRRGGBB values.
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

static int firmware_init(const struct sun4u_boot_handoff *handoff);
static int bochs_init(const struct sun4u_boot_handoff *handoff);
static void bochs_write(unsigned index, uint16_t value);
static uint8_t rgb332(uint32_t value);
static void put_pixel(unsigned x, unsigned y, uint32_t value);
static void fill_black(void);

/*
 * Shows the screen and clears it: the firmware framebuffer, or else the
 * standard VGA set to its linear mode.
 */
int
sun4u_framebuffer_init(
	const struct sun4u_boot_handoff *handoff)
{
	int status;

	/* The firmware framebuffer comes first. */
	status = firmware_init(handoff);

	/* Without one, the HAL sets up the standard VGA itself. */
	if (status != HAL_OK)
		status = bochs_init(handoff);
	if (status != HAL_OK)
		return status;

	/* Names the screen shown. */
	framebuffer_shown.phys = framebuffer_pixels;
	framebuffer_shown.size = framebuffer_pitch * framebuffer_height;
	framebuffer_shown.width = framebuffer_width;
	framebuffer_shown.height = framebuffer_height;
	framebuffer_shown.pitch = framebuffer_pitch;
	framebuffer_shown.format = framebuffer_format;

	/* Clears the screen; from now on the console draws to it. */
	fill_black();
	framebuffer_on = 1;

	/* Succeeded: the screen shows. */
	return HAL_OK;
}

/*
 * Reports the screen shown, or NULL when there is none.
 */
struct sun4u_framebuffer_names *
sun4u_framebuffer_shown(
	void)
{
	/* Nothing is named before the screen shows. */
	if (!framebuffer_on)
		return NULL;

	/* Succeeded: the screen sun4u_framebuffer_init() showed. */
	return &framebuffer_shown;
}

/* Takes the firmware framebuffer the loader handed over, as it is. */
static int
firmware_init(
	const struct sun4u_boot_handoff *handoff)
{
	uint64_t row_bytes;

	/* Refuses a missing framebuffer or one without pixels. */
	if (handoff->framebuffer_phys == 0)
		return HAL_ERR_UNSUPPORTED;
	if (handoff->framebuffer_width == 0 || handoff->framebuffer_height == 0)
		return HAL_ERR_UNSUPPORTED;

	/* Refuses a format the HAL cannot draw, and rows shorter than their pixels. */
	if (handoff->framebuffer_format == KERN_BOOT_FRAMEBUFFER_RGB332) {
		/* One byte a pixel. */
		row_bytes = handoff->framebuffer_width;
	} else if (handoff->framebuffer_format == KERN_BOOT_FRAMEBUFFER_BGRX8888 ||
		   handoff->framebuffer_format == KERN_BOOT_FRAMEBUFFER_XRGB8888) {
		/* Four bytes a pixel. */
		row_bytes = (uint64_t)handoff->framebuffer_width * 4U;
	} else {
		/* Any other format is not drawn. */
		return HAL_ERR_UNSUPPORTED;
	}
	if (handoff->framebuffer_pitch < row_bytes)
		return HAL_ERR_UNSUPPORTED;

	/* Keeps the screen as the firmware left it. */
	framebuffer_pixels = handoff->framebuffer_phys;
	framebuffer_width = handoff->framebuffer_width;
	framebuffer_height = handoff->framebuffer_height;
	framebuffer_pitch = handoff->framebuffer_pitch;
	framebuffer_format = handoff->framebuffer_format;
	framebuffer_shown.vendor = handoff->framebuffer_vendor;
	framebuffer_shown.device = handoff->framebuffer_device;

	/* Succeeded: the firmware framebuffer is the screen. */
	return HAL_OK;
}

/* Sets the standard VGA to 640 by 400 pixels of 32 bits, read little-endian. */
static int
bochs_init(
	const struct sun4u_boot_handoff *handoff)
{
	/* Refuses a display the loader did not find, or one too small for the screen. */
	if (handoff->display_framebuffer == 0 || handoff->display_registers == 0)
		return HAL_ERR_UNSUPPORTED;
	if (handoff->display_framebuffer_size < FRAMEBUFFER_STRIDE * FRAMEBUFFER_HEIGHT)
		return HAL_ERR_UNSUPPORTED;

	/* The PCI addresses as the processor sees them, and the screen. */
	framebuffer_pixels = handoff->pci_memory_base + handoff->display_framebuffer;
	framebuffer_registers = handoff->pci_memory_base + handoff->display_registers;
	framebuffer_width = FRAMEBUFFER_WIDTH;
	framebuffer_height = FRAMEBUFFER_HEIGHT;
	framebuffer_pitch = FRAMEBUFFER_STRIDE;
	framebuffer_format = KERN_BOOT_FRAMEBUFFER_BGRX8888;
	framebuffer_shown.vendor = FRAMEBUFFER_QEMU_VENDOR;
	framebuffer_shown.device = FRAMEBUFFER_QEMU_DEVICE;

	/* Turns the display off while its mode is set, and reads the pixels little-endian. */
	bochs_write(FRAMEBUFFER_BOCHS_ENABLE, 0);
	sparcv9_phys_write32_le(framebuffer_registers + FRAMEBUFFER_QEXT_BYTEORDER, FRAMEBUFFER_QEXT_LITTLE_ENDIAN);

	/* Sets the resolution, the depth and an unscrolled view of the whole framebuffer. */
	bochs_write(FRAMEBUFFER_BOCHS_XRES, (uint16_t)FRAMEBUFFER_WIDTH);
	bochs_write(FRAMEBUFFER_BOCHS_YRES, (uint16_t)FRAMEBUFFER_HEIGHT);
	bochs_write(FRAMEBUFFER_BOCHS_BPP, (uint16_t)FRAMEBUFFER_DEPTH);
	bochs_write(FRAMEBUFFER_BOCHS_BANK, 0);
	bochs_write(FRAMEBUFFER_BOCHS_VIRT_WIDTH, (uint16_t)FRAMEBUFFER_WIDTH);
	bochs_write(FRAMEBUFFER_BOCHS_VIRT_HEIGHT, (uint16_t)FRAMEBUFFER_HEIGHT);
	bochs_write(FRAMEBUFFER_BOCHS_X_OFFSET, 0);
	bochs_write(FRAMEBUFFER_BOCHS_Y_OFFSET, 0);

	/* Turns the linear mode on, and lets the VGA show it. */
	bochs_write(FRAMEBUFFER_BOCHS_ENABLE, FRAMEBUFFER_BOCHS_ENABLED | FRAMEBUFFER_BOCHS_LINEAR);
	sparcv9_phys_write8(framebuffer_registers + FRAMEBUFFER_VGA_PORTS + FRAMEBUFFER_VGA_ATTRIBUTE, FRAMEBUFFER_VGA_UNBLANK);

	/* Succeeded: the standard VGA shows its linear mode. */
	return HAL_OK;
}

/*
 * Reports whether the screen shows.
 */
int
sun4u_framebuffer_ready(
	void)
{
	/* Succeeded: whether sun4u_framebuffer_init() succeeded. */
	return framebuffer_on;
}

/*
 * Draws one text cell: a glyph in the attribute's colours.
 */
void
sun4u_framebuffer_cell(
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

	/* Nothing to draw before the screen is shown, or off the grid. */
	if (!framebuffer_on)
		return;
	if (row >= SUN4U_CONSOLE_ROWS || column >= SUN4U_CONSOLE_COLUMNS)
		return;

	/* The glyph of the character (the font covers every byte value) and its colours. */
	glyph = &sun4u_font8x16[((unsigned)character & 0xffU) * SUN4U_FONT_HEIGHT];
	foreground = framebuffer_palette[attribute & 15U];
	background = framebuffer_palette[(attribute >> 4) & 15U];

	/* Each row of the glyph, from its leftmost pixel in the high bit. */
	for (line = 0; line < SUN4U_FONT_HEIGHT; line++) {
		bits = glyph[line];
		for (bit = 0; bit < 8U; bit++) {
			value = background;
			if ((bits & (0x80U >> bit)) != 0)
				value = foreground;
			put_pixel(column * 8U + bit, row * SUN4U_FONT_HEIGHT + line, value);
		}
	}
}

/*
 * Draws the cursor: the two bottom pixel rows of a cell in white.
 */
void
sun4u_framebuffer_cursor(
	unsigned row,
	unsigned column)
{
	unsigned line;
	unsigned bit;

	/* Nothing to draw before the screen is shown, or off the grid. */
	if (!framebuffer_on)
		return;
	if (row >= SUN4U_CONSOLE_ROWS || column >= SUN4U_CONSOLE_COLUMNS)
		return;

	/* Whitens rows 14 and 15 of the cell. */
	for (line = SUN4U_FONT_HEIGHT - 2U; line < SUN4U_FONT_HEIGHT; line++) {
		for (bit = 0; bit < 8U; bit++)
			put_pixel(column * 8U + bit, row * SUN4U_FONT_HEIGHT + line, framebuffer_palette[15]);
	}
}

/* Writes one register of the Bochs display interface. */
static void
bochs_write(
	unsigned index,
	uint16_t value)
{
	/* Each register is a little-endian 16-bit word at twice its index. */
	sparcv9_phys_write16_le(framebuffer_registers + FRAMEBUFFER_BOCHS_REGISTERS + index * 2U, value);
}

/*
 * Reduces a 0xRRGGBB colour to its KERN_BOOT_FRAMEBUFFER_RGB332 index: the
 * top three bits of red and of green and the top two of blue.
 */
static uint8_t
rgb332(
	uint32_t value)
{
	uint8_t index;

	/* Packs red in bits 7-5, green in bits 4-2 and blue in bits 1-0. */
	index = (uint8_t)(((value >> 16) & 0xe0U) | ((value >> 11) & 0x1cU) | ((value >> 6) & 3U));

	/* Succeeded: the palette index. */
	return index;
}

/* Stores one pixel of the screen; a pixel off the screen is not stored. */
static void
put_pixel(
	unsigned x,
	unsigned y,
	uint32_t value)
{
	uint64_t address;
	uint32_t swapped;

	/* The text grid may reach beyond a small screen. */
	if (x >= framebuffer_width || y >= framebuffer_height)
		return;

	/* A palette index is one byte. */
	address = framebuffer_pixels + (uint64_t)y * framebuffer_pitch;
	if (framebuffer_format == KERN_BOOT_FRAMEBUFFER_RGB332) {
		sparcv9_phys_write8(address + x, rgb332(value));
		return;
	}

	/* A little-endian word is blue first in memory. */
	address += (uint64_t)x * 4U;
	if (framebuffer_format == KERN_BOOT_FRAMEBUFFER_BGRX8888) {
		sparcv9_phys_write32_le(address, value);
		return;
	}

	/* The unused byte, red, green and blue: the word with its bytes reversed. */
	swapped = ((value & 0xffU) << 24) | ((value & 0xff00U) << 8) |
		((value >> 8) & 0xff00U) | (value >> 24);
	sparcv9_phys_write32_le(address, swapped);
}

/* Paints the whole screen black. */
static void
fill_black(
	void)
{
	unsigned x;
	unsigned y;

	/* Clears every pixel, row by row. */
	for (y = 0; y < framebuffer_height; y++) {
		for (x = 0; x < framebuffer_width; x++)
			put_pixel(x, y, 0);
	}
}
