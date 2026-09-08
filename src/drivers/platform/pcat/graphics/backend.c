/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
 * PC/AT boot framebuffer, Cirrus GD5446, and standard VGA backends.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "drivers/platform/pcat/graphics/backend.h"
#include "drivers/platform/pcat/graphics/font.h"
#include "drivers/graphics/pcat.h"

#include <drivers/pci.h>
#include <hal/hal.h>
#include <string.h>
#include "bootloader/include/amd64-handoff.h"

#define WIDTH 640U
#define HEIGHT 480U
#define CIRRUS_APERTURE cirrus_aperture
#define VGA_APERTURE vga_aperture
#define CIRRUS_STRIDE8 WIDTH
#define CIRRUS_STRIDE24 (WIDTH * 3U)
#define PCI_CIRRUS_VENDOR 0x1013U

enum display_backend {
	DISPLAY_NONE,
	DISPLAY_LINEAR,
	DISPLAY_CIRRUS,
	DISPLAY_VGA
};

static enum display_backend active_backend;
static int backend_prepared;
static uint8_t active_bpp;
static int cirrus_present;
static struct drv_pci_device *cirrus_device;
static struct drv_pci_mapping cirrus_mapping;
static int vga_color_cache = -1;
static struct hal_pmem vga_memory;
static volatile uint8_t *vga_aperture, *cirrus_aperture;
static const struct zbl6_framebuffer *linear_framebuffer;
static volatile uint32_t *linear_pixels;

static const uint32_t vga_palette[16] = {
	0x000000U, 0x0000aaU, 0x00aa00U, 0x00aaaaU, 0xaa0000U, 0xaa00aaU,
	0xaa5500U, 0xaaaaaaU, 0x555555U, 0x5555ffU, 0x55ff55U, 0x55ffffU,
	0xff5555U, 0xff55ffU, 0xffff55U, 0xffffffU};

static int pcat_graphics_clear(void);
static int cirrus_enter(unsigned bits_per_pixel);
static void seq_write(uint8_t index, uint8_t value);
static void out8(uint16_t port, uint8_t value);
static uint8_t crtc_read(uint8_t index);
static uint8_t in8(uint16_t port);
static void cirrus_mode_640x480(unsigned bits_per_pixel);
static void gfx_write(uint8_t index, uint8_t value);
static uint8_t seq_read(uint8_t index);
static void crtc_write(uint8_t index, uint8_t value);
static void hidden_dac_write(uint8_t value);
static void load_rgb332_palette(void);
static void vga_graphics_mode(void);
static void vga_write_registers(const uint8_t registers[61]);
static void vga_load_palette(void);
static void vga_text_mode(void);
static uint32_t linear_color(uint32_t color);
static uint8_t rgb332(uint32_t color);
static void write_pixel(unsigned x, unsigned y, uint32_t color);
static void vga_write_pixel(unsigned x, unsigned y, uint8_t color);
static uint8_t rgb_to_vga(uint32_t color);
static int pattern_bit(uint64_t pattern, unsigned x, unsigned y);
static int pcat_graphics_prepare_hardware(void);
static int cirrus_attach(struct drv_pci_device *device, const struct drv_pci_id *id);
static int cirrus_detach(struct drv_pci_device *device, unsigned flags);

static const struct drv_pci_id cirrus_ids[] = {
	{PCI_CIRRUS_VENDOR, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID,
	 0x030000U, 0xffff00U, 0}};

static struct drv_pci_driver cirrus_driver = {.name = "cirrus-gd54xx",
					      .ids = cirrus_ids,
					      .id_count = sizeof(cirrus_ids) /
							  sizeof(cirrus_ids[0]),
					      .attach = cirrus_attach,
					      .detach = cirrus_detach};



























/*
 * Implements the drv pcat graphics backend enter operation.
 */
int
drv_pcat_graphics_backend_enter(
	struct graphics_mode *mode)
{
	unsigned requested;

	/* Handles the mode availability. */
	if (mode == NULL)
		return 0;

	/* Handles the linear pixels availability. */
	if (linear_pixels != NULL) {
		active_backend = DISPLAY_LINEAR;
		active_bpp = 32U;
		mode->width = linear_framebuffer->width;
		mode->height = linear_framebuffer->height;
		mode->bits_per_pixel = 32U;
		mode->stride = linear_framebuffer->stride * 4U;
		(void)pcat_graphics_clear();
		hal_printf("graphics: boot framebuffer %ux%ux32 stride=%u\n",
			   mode->width, mode->height, mode->stride);

		/* Reports operation failure. */
		return 1;
	}
	requested = mode->preferred_bits_per_pixel == 24U ? 24U : 8U;

	/* Handles the cirrus present condition. */
	if (cirrus_present && cirrus_enter(requested)) {
		mode->width = WIDTH;
		mode->height = HEIGHT;
		mode->bits_per_pixel = requested;
		mode->stride =
			requested == 24U ? CIRRUS_STRIDE24 : CIRRUS_STRIDE8;
		hal_printf("graphics: PC/AT Cirrus %ux%ux%u stride=%u\n", WIDTH,
			   HEIGHT, requested, mode->stride);

		/* Reports operation failure. */
		return 1;
	}
	vga_graphics_mode();
	active_backend = DISPLAY_VGA;
	active_bpp = 4;
	mode->width = WIDTH;
	mode->height = HEIGHT;
	mode->bits_per_pixel = 4;
	mode->stride = WIDTH / 8U;
	(void)pcat_graphics_clear();
	hal_printf("graphics: PC/AT VGA fallback %ux%ux4 planar\n", WIDTH,
		   HEIGHT);

	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the drv pcat graphics backend get modes operation.
 */
size_t
drv_pcat_graphics_backend_get_modes(
	struct graphics_mode_info *modes,
	size_t capacity)
{
	size_t function_result;
	static const struct graphics_mode_info available[] = {
		{WIDTH, HEIGHT, 24U, CIRRUS_STRIDE24},
		{WIDTH, HEIGHT, 8U, CIRRUS_STRIDE8},
		{WIDTH, HEIGHT, 4U, WIDTH / 8U},
	};
	size_t i;

	/* Handles the linear framebuffer availability. */
	if (linear_framebuffer != NULL) {
		/* Handles the modes availability. */
		if (modes != NULL && capacity != 0) {
			modes[0].width = linear_framebuffer->width;
			modes[0].height = linear_framebuffer->height;
			modes[0].bits_per_pixel = 32U;
			modes[0].stride = linear_framebuffer->stride * 4U;
		}

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the modes availability. */
	if (modes != NULL) {
		/* Process each remaining element. */
		for (i = 0; i < capacity &&
			    i < sizeof(available) / sizeof(available[0]);
		     i++)
			modes[i] = available[i];
	}

	/* Computes the function result. */
	function_result = sizeof(available) / sizeof(available[0]);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pcat graphics backend leave operation.
 */
void
drv_pcat_graphics_backend_leave(
	void)
{
	/* Handles the active backend condition. */
	if (active_backend == DISPLAY_NONE)
		return;

	/* Handles the active backend condition. */
	if (active_backend == DISPLAY_LINEAR) {
		active_backend = DISPLAY_NONE;
		active_bpp = 0;

		/* Returns the computed result. */
		return;
	}

	/* Handles the active backend condition. */
	if (active_backend == DISPLAY_CIRRUS) {
		seq_write(0x01U, 0x21U);
		seq_write(0x06U, 0x12U);
		seq_write(0x07U, 0x00U);
		hidden_dac_write(0x00U);
		gfx_write(0x0bU, 0x00U);
	}
	vga_text_mode();
	active_backend = DISPLAY_NONE;
	active_bpp = 0;
	hal_printf("graphics: PC/AT text mode restored\n");
}

/*
 * Implements the drv pcat graphics backend fill operation.
 */
int
drv_pcat_graphics_backend_fill(
	const struct graphics_rect *rect,
	uint32_t color)
{
	uint32_t pixel_local;
	volatile uint32_t *row_local;
	uint8_t pixel_local2;
	volatile uint8_t *row_local1;
	volatile uint8_t *row_local3;
	unsigned x, y;

	/* Handles the rect availability. */
	if (rect == NULL || active_backend == DISPLAY_NONE)
		return 0;

	/* Handles the active backend condition. */
	if (active_backend == DISPLAY_LINEAR) {
		pixel_local = linear_color(color);
		/* Process each element required by the operation. */
		for (y = rect->y; y < rect->y + rect->height; y++) {
			row_local = linear_pixels +
				    (size_t)y * linear_framebuffer->stride +
				    rect->x;
			/* Process each element required by the operation. */
			for (x = 0; x < rect->width; x++)
				row_local[x] = pixel_local;
		}

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the active backend condition. */
	if (active_backend == DISPLAY_CIRRUS && active_bpp == 8U) {
		pixel_local2 = rgb332(color);
		/* Process each element required by the operation. */
		for (y = rect->y; y < rect->y + rect->height; y++) {
			row_local1 =
				CIRRUS_APERTURE + y * CIRRUS_STRIDE8 + rect->x;
			/* Process each element required by the operation. */
			for (x = 0; x < rect->width; x++)
				row_local1[x] = pixel_local2;
		}

		/* Reports operation failure. */
		return 1;
	}

	/* Handles the active backend condition. */
	if (active_backend == DISPLAY_CIRRUS && active_bpp == 24U) {
		/* Process each element required by the operation. */
		for (y = rect->y; y < rect->y + rect->height; y++) {
			row_local3 = CIRRUS_APERTURE + y * CIRRUS_STRIDE24 +
				     rect->x * 3U;
			/* Process each element required by the operation. */
			for (x = 0; x < rect->width; x++) {
				row_local3[x * 3U] = (uint8_t)color;
				row_local3[x * 3U + 1U] = (uint8_t)(color >> 8);
				row_local3[x * 3U + 2U] =
					(uint8_t)(color >> 16);
			}
		}

		/* Reports operation failure. */
		return 1;
	}
	/* Process each element required by the operation. */
	for (y = rect->y; y < rect->y + rect->height; y++) {
		/* Process each element required by the operation. */
		for (x = rect->x; x < rect->x + rect->width; x++)
			write_pixel(x, y, color);
	}

	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the drv pcat graphics backend line operation.
 */
int
drv_pcat_graphics_backend_line(
	unsigned x0,
	unsigned y0,
	unsigned x1,
	unsigned y1,
	uint32_t color)
{
	int twice;
	int x = (int)x0, y = (int)y0;
	int target_x = (int)x1, target_y = (int)y1;
	int dx = target_x >= x ? target_x - x : x - target_x;
	int sx = x < target_x ? 1 : -1;
	int dy = target_y >= y ? y - target_y : target_y - y;
	int sy = y < target_y ? 1 : -1;
	int error = dx + dy;

	/* Handles the active backend condition. */
	if (active_backend == DISPLAY_NONE)
		return 0;
	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		write_pixel((unsigned)x, (unsigned)y, color);

		/* Checks the current horizontal value. */
		if (x == target_x && y == target_y)
			break;
		twice = error * 2;

		/* Handles the twice condition. */
		if (twice >= dy) {
			error += dy;
			x += sx;
		}

		/* Handles the twice condition. */
		if (twice <= dx) {
			error += dx;
			y += sy;
		}
	}

	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the drv pcat graphics backend pattern fill operation.
 */
int
drv_pcat_graphics_backend_pattern_fill(
	const struct graphics_rect *rect,
	uint32_t color,
	uint64_t pattern)
{
	unsigned x, y;

	/* Handles the rect availability. */
	if (rect == NULL || active_backend == DISPLAY_NONE)
		return 0;
	/* Process each element required by the operation. */
	for (y = 0; y < rect->height; y++) {
		/* Process each element required by the operation. */
		for (x = 0; x < rect->width; x++) {
			/* Handles the pattern bit condition. */
			if (pattern_bit(pattern, x, y))
				write_pixel(rect->x + x, rect->y + y, color);
		}
	}

	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the drv pcat graphics backend blit operation.
 */
int
drv_pcat_graphics_backend_blit(
	unsigned destination_x,
	unsigned destination_y,
	const struct pcat_graphics_image *image,
	uint64_t pattern,
	int patterned)
{
	unsigned index;
	const uint8_t *source;
	uint32_t rgb;
	const uint8_t *row;
	unsigned x, y;

	/* Handles the image availability. */
	if (image == NULL || active_backend == DISPLAY_NONE)
		return 0;
	/* Process each element required by the operation. */
	for (y = 0; y < image->height; y++) {
		row = image->pixels + (size_t)y * image->stride;
		/* Process each element required by the operation. */
		for (x = 0; x < image->width; x++) {
			/* Checks the pattern bit result. */
			if (patterned && !pattern_bit(pattern, x, y))
				continue;

			/* Handles the image condition. */
			if (image->format == 1U) {
				index = row[x];
				rgb = index < image->palette_size
					      ? image->palette[index]
					      : 0;
			} else {
				source = row + (size_t)x * 3U;
				rgb = ((uint32_t)source[0] << 16) |
				      ((uint32_t)source[1] << 8) | source[2];
			}
			write_pixel(destination_x + x, destination_y + y, rgb);
		}
	}

	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the drv pcat graphics backend flush operation.
 */
int
drv_pcat_graphics_backend_flush(
	const struct graphics_rect *rectangles,
	size_t count)
{
	(void)rectangles;
	(void)count;

	/* Returns the computed result. */
	return active_backend != DISPLAY_NONE;
}

/*
 * Implements the drv pcat graphics backend get glyph operation.
 */
int
drv_pcat_graphics_backend_get_glyph(
	uint32_t codepoint,
	uint8_t bitmap[32],
	unsigned *width,
	unsigned *height)
{
	int function_result;

	/* Obtains the drv pcat font get glyph result. */
	function_result =
		drv_pcat_font_get_glyph(codepoint, bitmap, width, height);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pcat graphics pci register operation.
 */
int
drv_pcat_graphics_pci_register(
	void)
{
	int function_result;

	/* Obtains the drv pci driver register result. */
	function_result = drv_pci_driver_register(&cirrus_driver);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pcat graphics prepare operation.
 */
int
drv_pcat_graphics_prepare(
	void)
{
	backend_prepared = 0;

	/* Checks the pcat graphics prepare hardware result. */
	if (!pcat_graphics_prepare_hardware())
		return 0;
	backend_prepared = 1;

	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the drv pcat graphics backend ready operation.
 */
int
drv_pcat_graphics_backend_ready(
	void)
{
	/* Returns the computed result. */
	return backend_prepared;
}

/* Supports the pcat graphics clear operation. */
static int
pcat_graphics_clear(
	void)
{
	int function_result;
	struct graphics_rect screen = {0, 0, WIDTH, HEIGHT};

	/* Handles the active backend condition. */
	if (active_backend == DISPLAY_NONE)
		return 0;

	/* Handles the active backend condition. */
	if (active_backend == DISPLAY_LINEAR) {
		screen.width = linear_framebuffer->width;
		screen.height = linear_framebuffer->height;
	}

	/* Obtains the drv pcat graphics backend fill result. */
	function_result = drv_pcat_graphics_backend_fill(&screen, 0);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the cirrus enter operation. */
static int
cirrus_enter(
	unsigned bits_per_pixel)
{
	unsigned bytes, i;
	uint8_t chip;

	/* Checks the drv pci device enable result. */
	if (drv_pci_device_enable(cirrus_device) != 0)
		return 0;
	seq_write(0x06U, 0x12U);
	chip = crtc_read(0x27U);

	/* Handles the chip condition. */
	if (chip == 0 || chip == 0xffU)
		return 0;
	cirrus_mode_640x480(bits_per_pixel);
	bytes = (bits_per_pixel == 24U ? CIRRUS_STRIDE24 : CIRRUS_STRIDE8) *
		HEIGHT;
	/* Process each element required by the operation. */
	for (i = 0; i < bytes; i++)
		CIRRUS_APERTURE[i] = 0;
	seq_write(0x01U, 0x01U);
	active_bpp = (uint8_t)bits_per_pixel;
	active_backend = DISPLAY_CIRRUS;

	/* Reports operation failure. */
	return 1;
}

/* Supports the seq write operation. */
static void
seq_write(
	uint8_t index,
	uint8_t value)
{
	out8(0x3c4U, index);
	out8(0x3c5U, value);
}

/* Supports the out8 operation. */
static void
out8(
	uint16_t port,
	uint8_t value)
{
	__asm__ volatile("outb %0,%w1" : : "a"(value), "Nd"(port));
}

/* Supports the crtc read operation. */
static uint8_t
crtc_read(
	uint8_t index)
{
	uint8_t function_result;
	uint16_t port = (in8(0x3ccU) & 1U) ? 0x3d4U : 0x3b4U;

	out8(port, index);

	/* Obtains the in8 result. */
	function_result = in8((uint16_t)(port + 1U));

	/* Returns the computed result. */
	return function_result;
}

/* Supports the in8 operation. */
static uint8_t
in8(
	uint16_t port)
{
	uint8_t value;

	__asm__ volatile("inb %w1,%0" : "=a"(value) : "Nd"(port));

	/* Returns the computed result. */
	return value;
}

/* Supports the cirrus mode 640x480 operation. */
static void
cirrus_mode_640x480(
	unsigned bits_per_pixel)
{
	uint8_t value_local;
	uint8_t value_local1;
	static const uint8_t seq_index[] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x07, 0x08, 0x0b, 0x0c, 0x0d,
		0x0e, 0x0f, 0x16, 0x18, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
	static const uint8_t seq_value[] = {
		0x01, 0x01, 0x0f, 0x00, 0x0e, 0x11, 0x00, 0x66, 0x48, 0x56,
		0x60, 0x30, 0x58, 0x40, 0x3b, 0x23, 0x3d, 0x3b, 0x20};
	static const uint8_t crtc[0x1c] = {
		0x5f, 0x4f, 0x50, 0x84, 0x54, 0x80, 0x0b, 0x3e, 0x00, 0x40,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe5, 0x87, 0xdf, 0x50,
		0x00, 0xe7, 0x04, 0xe3, 0xff, 0x00, 0x90, 0x22};
	static const uint8_t graphics[9] = {0,	  0,	0,    0,   0,
					    0x40, 0x05, 0x0f, 0xff};
	static const uint8_t attribute[21] = {
		0,    1,    2,	  3,	4,    5,    6, 7,    8, 9, 0x0a,
		0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x41, 0, 0x0f, 0, 0};
	unsigned i;

	gfx_write(0x33U, 0);
	gfx_write(0x31U, 0x04U);
	gfx_write(0x31U, 0);
	seq_write(0x06U, 0x12U);
	seq_write(0x12U, 0);
	/* Process each remaining element. */
	for (i = 0; i < sizeof(seq_index); i++) {
		value_local = seq_value[i];

		/* Handles the seq index condition. */
		if (seq_index[i] == 0x07U && bits_per_pixel == 24U)
			value_local = 0x15U;
		seq_write(seq_index[i], value_local);
	}
	seq_write(0x0fU, (uint8_t)((seq_read(0x0fU) & 0xdfU) | 0x20U));
	out8(0x3c2U, 0xe3U);
	gfx_write(0x06U, 0x05U);
	seq_write(0x00U, 0x03U);
	crtc_write(0x11U, 0x20U);
	/* Process each remaining element. */
	for (i = 0; i < sizeof(crtc); i++) {
		value_local1 = crtc[i];

		/* Checks the current index. */
		if (i == 0x13U && bits_per_pixel == 24U)
			value_local1 = 0xf0U;
		crtc_write((uint8_t)i, value_local1);
	}
	/* Process each remaining element. */
	for (i = 0; i < sizeof(graphics); i++)
		gfx_write((uint8_t)i, graphics[i]);
	(void)in8(0x3daU);
	/* Process each remaining element. */
	for (i = 0; i < sizeof(attribute); i++) {
		out8(0x3c0U, (uint8_t)i);
		out8(0x3c0U, attribute[i]);
	}
	(void)in8(0x3daU);
	out8(0x3c0U, 0x20U);
	hidden_dac_write(bits_per_pixel == 24U ? 0xc5U : 0x20U);
	out8(0x3c6U, 0xffU);
	gfx_write(0x09U, 0);
	gfx_write(0x0aU, 0);
	gfx_write(0x0bU, 0x21U);
	seq_write(0x17U, (uint8_t)(seq_read(0x17U) | 0x44U));
	seq_write(0x18U, (uint8_t)(seq_read(0x18U) & 0xbfU));
	gfx_write(0x31U, 0x04U);
	gfx_write(0x31U, 0);

	/* Handles the bits per pixel condition. */
	if (bits_per_pixel == 8U)
		load_rgb332_palette();
	seq_write(0x01U, 0x21U);
}

/* Supports the gfx write operation. */
static void
gfx_write(
	uint8_t index,
	uint8_t value)
{
	out8(0x3ceU, index);
	out8(0x3cfU, value);
}

/* Supports the seq read operation. */
static uint8_t
seq_read(
	uint8_t index)
{
	uint8_t function_result;

	out8(0x3c4U, index);

	/* Obtains the in8 result. */
	function_result = in8(0x3c5U);

	/* Returns the computed result. */
	return function_result;
}

/* Supports the crtc write operation. */
static void
crtc_write(
	uint8_t index,
	uint8_t value)
{
	uint16_t port = (in8(0x3ccU) & 1U) ? 0x3d4U : 0x3b4U;

	out8(port, index);
	out8((uint16_t)(port + 1U), value);
}

/* Supports the hidden dac write operation. */
static void
hidden_dac_write(
	uint8_t value)
{
	unsigned i;

	(void)in8(0x3c8U);
	/* Process each element required by the operation. */
	for (i = 0; i < 4U; i++)
		(void)in8(0x3c6U);
	out8(0x3c6U, value);
}

/* Supports the load rgb332 palette operation. */
static void
load_rgb332_palette(
	void)
{
	unsigned red;
	unsigned green;
	unsigned blue;
	unsigned i;

	out8(0x3c6U, 0xffU);
	out8(0x3c8U, 0);
	/* Process each element required by the operation. */
	for (i = 0; i < 256U; i++) {
		red = (i >> 5) & 7U;
		green = (i >> 2) & 7U;
		blue = i & 3U;
		out8(0x3c9U, (uint8_t)(red * 63U / 7U));
		out8(0x3c9U, (uint8_t)(green * 63U / 7U));
		out8(0x3c9U, (uint8_t)(blue * 63U / 3U));
	}
}

/* Supports the vga graphics mode operation. */
static void
vga_graphics_mode(
	void)
{
	static const uint8_t mode[61] = {
		0xe3, 0x03, 0x01, 0x0f, 0x00, 0x06, 0x5f, 0x4f, 0x50,
		0x82, 0x54, 0x80, 0x0b, 0x3e, 0x00, 0x40, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xea, 0x0c, 0xdf, 0x28, 0x00,
		0xe7, 0x04, 0xe3, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x05, 0x0f, 0xff, 0x00, 0x01, 0x02, 0x03, 0x04,
		0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
		0x0e, 0x0f, 0x01, 0x00, 0x0f, 0x00, 0x00};

	vga_write_registers(mode);
	vga_load_palette();
	vga_color_cache = -1;
}

/* Supports the vga write registers operation. */
static void
vga_write_registers(
	const uint8_t registers[61])
{
	const uint8_t *value = registers;
	unsigned i;

	out8(0x3c2U, *value++);
	/* Process each element required by the operation. */
	for (i = 0; i < 5U; i++)
		seq_write((uint8_t)i, *value++);
	crtc_write(0x03U, (uint8_t)(crtc_read(0x03U) | 0x80U));
	crtc_write(0x11U, (uint8_t)(crtc_read(0x11U) & 0x7fU));
	/* Process each element required by the operation. */
	for (i = 0; i < 25U; i++)
		crtc_write((uint8_t)i, *value++);
	/* Process each element required by the operation. */
	for (i = 0; i < 9U; i++)
		gfx_write((uint8_t)i, *value++);
	/* Process each element required by the operation. */
	for (i = 0; i < 21U; i++) {
		(void)in8(0x3daU);
		out8(0x3c0U, (uint8_t)i);
		out8(0x3c0U, *value++);
	}
	(void)in8(0x3daU);
	out8(0x3c0U, 0x20U);
}

/* Supports the vga load palette operation. */
static void
vga_load_palette(
	void)
{
	unsigned i;

	out8(0x3c6U, 0xffU);
	out8(0x3c8U, 0);
	/* Process each element required by the operation. */
	for (i = 0; i < 16U; i++) {
		out8(0x3c9U, (uint8_t)(((vga_palette[i] >> 16) & 0xffU) >> 2));
		out8(0x3c9U, (uint8_t)(((vga_palette[i] >> 8) & 0xffU) >> 2));
		out8(0x3c9U, (uint8_t)((vga_palette[i] & 0xffU) >> 2));
	}
}

/* Supports the vga text mode operation. */
static void
vga_text_mode(
	void)
{
	static const uint8_t mode[61] = {
		0x67, 0x03, 0x00, 0x03, 0x00, 0x02, 0x5f, 0x4f, 0x50,
		0x82, 0x55, 0x81, 0xbf, 0x1f, 0x00, 0x4f, 0x0d, 0x0e,
		0x00, 0x00, 0x00, 0x50, 0x9c, 0x0e, 0x8f, 0x28, 0x1f,
		0x96, 0xb9, 0xa3, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x10, 0x0e, 0x00, 0xff, 0x00, 0x01, 0x02, 0x03, 0x04,
		0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
		0x0e, 0x0f, 0x0c, 0x00, 0x0f, 0x08, 0x00};

	vga_write_registers(mode);
	drv_pcat_font_restore_ascii();
}

/* Supports the linear color operation. */
static uint32_t
linear_color(
	uint32_t color)
{
	/* Handles the linear framebuffer condition. */
	if (linear_framebuffer->format == ZBL6_FRAMEBUFFER_RGBX8888) {
		return ((color & 0xff0000U) >> 16) | (color & 0x00ff00U) |
		       ((color & 0x0000ffU) << 16);
	}

	/* Returns the computed result. */
	return color;
}

/* Supports the rgb332 operation. */
static uint8_t
rgb332(
	uint32_t color)
{
	/* Returns the computed result. */
	return (uint8_t)(((color >> 16) & 0xe0U) | ((color >> 11) & 0x1cU) |
			 ((color >> 6) & 3U));
}

/* Supports the write pixel operation. */
static void
write_pixel(
	unsigned x,
	unsigned y,
	uint32_t color)
{
	volatile uint8_t *pixel;

	/* Handles the active backend condition. */
	if (active_backend == DISPLAY_LINEAR) {
		linear_pixels[(size_t)y * linear_framebuffer->stride + x] =
			linear_color(color);
	} else if (active_backend == DISPLAY_VGA) {
		vga_write_pixel(x, y, rgb_to_vga(color));
	} else if (active_bpp == 8U) {
		CIRRUS_APERTURE[y * CIRRUS_STRIDE8 + x] = rgb332(color);
	} else {
		pixel = CIRRUS_APERTURE + y * CIRRUS_STRIDE24 + x * 3U;
		pixel[0] = (uint8_t)color;
		pixel[1] = (uint8_t)(color >> 8);
		pixel[2] = (uint8_t)(color >> 16);
	}
}

/* Supports the vga write pixel operation. */
static void
vga_write_pixel(
	unsigned x,
	unsigned y,
	uint8_t color)
{
	unsigned offset = y * (WIDTH / 8U) + x / 8U;
	uint8_t mask = (uint8_t)(0x80U >> (x & 7U));
	volatile uint8_t latch;

	/* Handles the vga color cache condition. */
	if (vga_color_cache != color) {
		seq_write(0x02U, 0x0fU);
		gfx_write(0x00U, color);
		gfx_write(0x01U, 0x0fU);
		gfx_write(0x05U, 0x00U);
		vga_color_cache = color;
	}
	gfx_write(0x08U, mask);
	latch = VGA_APERTURE[offset];
	(void)latch;
	VGA_APERTURE[offset] = 0xffU;
}

/* Supports the rgb to vga operation. */
static uint8_t
rgb_to_vga(
	uint32_t color)
{
	int dr;
	int dg;
	int db;
	unsigned distance;
	unsigned best = 0, best_distance = UINT32_MAX, i;
	int red = (int)((color >> 16) & 0xffU);
	int green = (int)((color >> 8) & 0xffU);
	int blue = (int)(color & 0xffU);

	/* Process each element required by the operation. */
	for (i = 0; i < 16U; i++) {
		dr = red - (int)((vga_palette[i] >> 16) & 0xffU);
		dg = green - (int)((vga_palette[i] >> 8) & 0xffU);
		db = blue - (int)(vga_palette[i] & 0xffU);
		distance = (unsigned)(dr * dr + dg * dg + db * db);

		/* Handles the distance condition. */
		if (distance < best_distance) {
			best = i;
			best_distance = distance;
		}
	}

	/* Returns the computed result. */
	return (uint8_t)best;
}

/* Supports the pattern bit operation. */
static int
pattern_bit(
	uint64_t pattern,
	unsigned x,
	unsigned y)
{
	uint8_t row = (uint8_t)(pattern >> ((y & 7U) * 8U));

	/* Returns the computed result. */
	return (row & (uint8_t)(0x80U >> (x & 7U))) != 0;
}

/* Supports the pcat graphics prepare hardware operation. */
static int
pcat_graphics_prepare_hardware(
	void)
{
	uint64_t aligned;
	uint64_t offset;
	struct hal_pmem_request request = {0x000a0000U, 0x00020000U, 0x1000U,
					   HAL_PMEM_TYPE_VRAM,
					   HAL_PMEM_ATTR_NOCACHE};

	drv_pcat_font_init();
	linear_framebuffer = hal_get_arch_handoff("pcat.framebuffer");
	linear_pixels = NULL;

	/* Handles the linear framebuffer availability. */
	if (linear_framebuffer != NULL) {
		aligned = linear_framebuffer->physical_base & ~0x1fffffULL;
		offset = linear_framebuffer->physical_base - aligned;
		linear_pixels =
			(volatile uint32_t
				 *)(uintptr_t)(ZBL6_FRAMEBUFFER_VIRTUAL_BASE +
					       offset);

		/* Reports operation failure. */
		return 1;
	}

	/* Checks the hal pmem alloc result. */
	if (hal_pmem_alloc(&request, &vga_memory) != HAL_OK)
		return 0;
	vga_aperture = (volatile uint8_t *)vga_memory.vaddr;

	/* Handles the cirrus present condition. */
	if (cirrus_present) {
		/* Checks the drv pci device claim bar result. */
		if (drv_pci_device_claim_bar(cirrus_device, 0) == 0 &&
		    drv_pci_device_map_bar_region(
			    cirrus_device, 0, 0, 4U * 1024U * 1024U,
			    DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE |
				    DRV_PCI_MAP_NOCACHE,
			    &cirrus_mapping) == 0) {
			cirrus_aperture =
				(volatile uint8_t *)cirrus_mapping.address;
		} else {
			drv_pci_device_release_bar(cirrus_device, 0);
			cirrus_present = 0;
		}
	}

	/* Handles the cirrus present condition. */
	if (!cirrus_present)
		hal_printf("graphics: PCI Cirrus absent; VGA fallback ready\n");

	/* Reports operation failure. */
	return 1;
}

/* Supports the cirrus attach operation. */
static int
cirrus_attach(
	struct drv_pci_device *device,
	const struct drv_pci_id *id)
{
	struct drv_pci_address address;
	struct drv_pci_bar bar;

	/* Records the device and reports what was found. */
	(void)id;
	cirrus_device = device;
	cirrus_present = 1;
	drv_pci_device_address(device, &address);
	hal_printf("graphics: PCI Cirrus %04x:%04x at %u:%u.%u\n",
		   drv_pci_device_vendor(device),
		   drv_pci_device_product(device), address.bus, address.device,
		   address.function);

	/* Checks the drv pci device bar result. */
	if (drv_pci_device_bar(device, 0, &bar) == 0) {
		hal_printf("graphics: Cirrus BAR0=%08x size=%u KiB\n",
			   (unsigned)bar.bus_address,
			   (unsigned)(bar.size / 1024U));
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the cirrus detach operation. */
static int
cirrus_detach(
	struct drv_pci_device *device,
	unsigned flags)
{
	(void)flags;

	/* Handles the device condition. */
	if (device != cirrus_device)
		return 0;

	/* Handles the address availability. */
	if (cirrus_mapping.address != NULL)
		drv_pci_device_unmap_bar(device, &cirrus_mapping);
	drv_pci_device_release_bar(device, 0);
	cirrus_device = NULL;
	cirrus_present = 0;

	/* Reports successful completion. */
	return 0;
}
