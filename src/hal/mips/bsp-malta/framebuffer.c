/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The malta board's framebuffer.
 *
 * The emulator's standard VGA is a PCI device with a linear framebuffer
 * and the Bochs display interface, which sets its resolution and depth.
 * The loader runs no firmware that would set up PCI, so the HAL finds the
 * device, gives its two memory ranges addresses in the board's first PCI
 * memory window and turns its memory decoding on.  The screen is set to
 * 640 by 480 pixels of 32 bits, so that 80 by 30 cells of the 8x16 font
 * fill it exactly and a text console needs no margin.  The pixels and the
 * registers are reached uncached through KSEG1; both the processor and
 * the device are little-endian.
 */

#include <hal/hal.h>
#include "../font.h"
#include "../io.h"
#include "framebuffer.h"
#include "pci.h"

/* The screen: its size in pixels, and the bytes of a row. */
#define FRAMEBUFFER_WIDTH	640U
#define FRAMEBUFFER_HEIGHT	480U
#define FRAMEBUFFER_DEPTH	32U
#define FRAMEBUFFER_STRIDE	(FRAMEBUFFER_WIDTH * 4U)

/*
 * The standard VGA's PCI identity, and its base address registers: the
 * framebuffer in the first and the register window in the third.
 *
 * Values referenced: QEMU v10.0.11 hw/display/vga-pci.c (the VGA
 * device's PCI_VENDOR_ID_QEMU and PCI_DEVICE_ID_QEMU_VGA, 0x1234 and
 * 0x1111 in include/hw/pci/pci_ids.h; the framebuffer in BAR 0 and the
 * register window in BAR 2).
 */
#define FRAMEBUFFER_PCI_VENDOR		0x1234U
#define FRAMEBUFFER_PCI_PRODUCT		0x1111U
#define FRAMEBUFFER_BAR_PIXELS		0U
#define FRAMEBUFFER_BAR_REGISTERS	2U

/*
 * The board's first PCI memory window, where the HAL places the two
 * ranges.  The PCI address of a range is its physical address.
 *
 * Values referenced: QEMU v10.0.11 hw/mips/malta.c (write_bootloader, the
 * PCI0 memory window 0 from 0x10000000, 128 MiB).
 */
#define FRAMEBUFFER_WINDOW_BASE		0x10000000U
#define FRAMEBUFFER_WINDOW_SIZE		0x08000000U

/*
 * The register window of the standard VGA: the VGA ports from 0x3c0 at
 * offset 0x400 and the Bochs display interface's registers, 16 bits each,
 * at offset 0x500.
 *
 * Values referenced: QEMU v10.0.0 hw/display/vga-pci.c
 * (PCI_VGA_IOPORT_OFFSET, PCI_VGA_BOCHS_OFFSET), the values the sun4u
 * port uses.
 */
#define FRAMEBUFFER_VGA_PORTS		0x400U
#define FRAMEBUFFER_BOCHS_REGISTERS	0x500U

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
 * The PCI device number of the display and the physical addresses of its
 * first pixel and of its register window.
 *
 * Set once by malta_framebuffer_init(); zero until then.
 */
static unsigned framebuffer_device;
static uint32_t framebuffer_pixels;
static uint32_t framebuffer_registers;

/*
 * Nonzero once the screen shows; until then nothing is drawn.  Set once
 * by malta_framebuffer_init().
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

static int place_ranges(unsigned device);
static void bochs_write(unsigned index, uint16_t value);
static void put_pixel(unsigned x, unsigned y, uint32_t value);
static void fill_black(void);

/*
 * Finds the standard VGA, sets its linear mode and clears it.
 *
 * Reports HAL_OK once the screen shows, or HAL_ERR_UNSUPPORTED when the
 * board has no such display or its ranges do not fit.
 */
int
malta_framebuffer_init(
	void)
{
	unsigned device;
	int error;

	/* Finds the display on the board's PCI bus. */
	error = malta_pci_find(FRAMEBUFFER_PCI_VENDOR, FRAMEBUFFER_PCI_PRODUCT, &device);
	if (error != HAL_OK) {
		hal_puts(MIPS_HAL_NAME " DISPLAY NONE: no standard VGA on PCI bus 0\n");
		return HAL_ERR_UNSUPPORTED;
	}

	/* Gives its framebuffer and its registers addresses, and lets it decode them. */
	error = place_ranges(device);
	if (error != HAL_OK)
		return HAL_ERR_UNSUPPORTED;

	/* Turns the display off while its mode is set. */
	bochs_write(FRAMEBUFFER_BOCHS_ENABLE, 0);

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
	mips_io_write8(framebuffer_registers + FRAMEBUFFER_VGA_PORTS + FRAMEBUFFER_VGA_ATTRIBUTE, FRAMEBUFFER_VGA_UNBLANK);

	/* Clears the screen; from now on the console draws to it. */
	framebuffer_device = device;
	fill_black();
	framebuffer_on = 1;

	/* Succeeded: the screen shows. */
	return HAL_OK;
}

/*
 * Reports whether the screen shows.
 */
int
malta_framebuffer_ready(
	void)
{
	/* Succeeded: whether malta_framebuffer_init() succeeded. */
	return framebuffer_on;
}

/*
 * Draws one text cell: a glyph in the attribute's colours.
 */
void
malta_framebuffer_cell(
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
	if (row >= MALTA_CONSOLE_ROWS || column >= MALTA_CONSOLE_COLUMNS)
		return;

	/* The glyph of the character (the font covers every byte value) and its colours. */
	glyph = &mips_font8x16[((unsigned)character & 0xffU) * MIPS_FONT_HEIGHT];
	foreground = framebuffer_palette[attribute & 15U];
	background = framebuffer_palette[(attribute >> 4) & 15U];

	/* Each row of the glyph, from its leftmost pixel in the high bit. */
	for (line = 0; line < MIPS_FONT_HEIGHT; line++) {
		bits = glyph[line];
		for (bit = 0; bit < 8U; bit++) {
			value = background;
			if ((bits & (0x80U >> bit)) != 0)
				value = foreground;
			put_pixel(column * 8U + bit, row * MIPS_FONT_HEIGHT + line, value);
		}
	}
}

/*
 * Draws the cursor: the two bottom pixel rows of a cell in white.
 */
void
malta_framebuffer_cursor(
	unsigned row,
	unsigned column)
{
	unsigned line;
	unsigned bit;

	/* Nothing to draw before the screen is shown, or off the grid. */
	if (!framebuffer_on)
		return;
	if (row >= MALTA_CONSOLE_ROWS || column >= MALTA_CONSOLE_COLUMNS)
		return;

	/* Whitens rows 14 and 15 of the cell. */
	for (line = MIPS_FONT_HEIGHT - 2U; line < MIPS_FONT_HEIGHT; line++) {
		for (bit = 0; bit < 8U; bit++)
			put_pixel(column * 8U + bit, row * MIPS_FONT_HEIGHT + line, framebuffer_palette[15]);
	}
}

/*
 * Prints where the screen is.
 */
void
malta_framebuffer_describe(
	void)
{
	/* Describes a shown screen; a missing one was reported when it was looked for. */
	if (!framebuffer_on)
		return;
	hal_printf(MIPS_HAL_NAME " DISPLAY VGA %ux%ux%u pci=0:%u fb=%08x mmio=%08x\n",
		   FRAMEBUFFER_WIDTH,
		   FRAMEBUFFER_HEIGHT,
		   FRAMEBUFFER_DEPTH,
		   framebuffer_device,
		   framebuffer_pixels,
		   framebuffer_registers);
}

/*
 * Places the framebuffer at the start of the PCI memory window and the
 * register window right after it, and turns memory decoding on.
 */
static int
place_ranges(
	unsigned device)
{
	uint32_t pixels_size;
	uint32_t registers_size;
	uint32_t registers_base;
	uint32_t command;

	/* Measures the two ranges; a device without them is not the display this draws on. */
	pixels_size = malta_pci_bar_size(device, FRAMEBUFFER_BAR_PIXELS);
	registers_size = malta_pci_bar_size(device, FRAMEBUFFER_BAR_REGISTERS);
	if (pixels_size < FRAMEBUFFER_STRIDE * FRAMEBUFFER_HEIGHT || registers_size == 0) {
		hal_printf(MIPS_HAL_NAME " DISPLAY NONE: VGA ranges %08x %08x\n", pixels_size, registers_size);
		return HAL_ERR_UNSUPPORTED;
	}

	/*
	 * The framebuffer starts the window, aligned to its power-of-two
	 * size; the registers follow it, which keeps them aligned to their
	 * own smaller size.  Both must end inside the window.
	 */
	registers_base = FRAMEBUFFER_WINDOW_BASE + pixels_size;
	if (pixels_size > FRAMEBUFFER_WINDOW_SIZE - registers_size) {
		hal_printf(MIPS_HAL_NAME " DISPLAY NONE: VGA ranges %08x %08x do not fit\n", pixels_size, registers_size);
		return HAL_ERR_UNSUPPORTED;
	}

	/* Writes the two addresses with decoding off, then turns memory decoding on. */
	command = malta_pci_read(device, MALTA_PCI_COMMAND);
	malta_pci_write(device, MALTA_PCI_COMMAND, command & ~(uint32_t)MALTA_PCI_COMMAND_MEMORY & 0xffffU);
	malta_pci_write(device, MALTA_PCI_BAR0 + FRAMEBUFFER_BAR_PIXELS * 4U, FRAMEBUFFER_WINDOW_BASE);
	malta_pci_write(device, MALTA_PCI_BAR0 + FRAMEBUFFER_BAR_REGISTERS * 4U, registers_base);
	malta_pci_write(device, MALTA_PCI_COMMAND, (command | MALTA_PCI_COMMAND_MEMORY) & 0xffffU);

	/* Keeps the physical addresses the pixels and the registers are drawn through. */
	framebuffer_pixels = FRAMEBUFFER_WINDOW_BASE;
	framebuffer_registers = registers_base;

	/* Succeeded: both ranges are decoded. */
	return HAL_OK;
}

/* Writes one register of the Bochs display interface. */
static void
bochs_write(
	unsigned index,
	uint16_t value)
{
	/* Each register is a 16-bit word at twice its index. */
	mips_io_write16(framebuffer_registers + FRAMEBUFFER_BOCHS_REGISTERS + index * 2U, value);
}

/* Stores one pixel of the screen. */
static void
put_pixel(
	unsigned x,
	unsigned y,
	uint32_t value)
{
	uint32_t address;

	/* The pixel's place in the framebuffer, an XRGB8888 word. */
	address = framebuffer_pixels + y * FRAMEBUFFER_STRIDE + x * 4U;
	mips_io_write32(address, value);
}

/* Paints the whole screen black. */
static void
fill_black(
	void)
{
	unsigned x;
	unsigned y;

	/* Clears every pixel, row by row. */
	for (y = 0; y < FRAMEBUFFER_HEIGHT; y++) {
		for (x = 0; x < FRAMEBUFFER_WIDTH; x++)
			put_pixel(x, y, 0);
	}
}
