/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PC/AT boot framebuffer, Cirrus GD5446, and standard VGA backends.
 */

#include "drivers/platform/pcat/graphics/backend.h"
#include "drivers/platform/pcat/graphics/font.h"
#include "drivers/platform/pcat/graphics/text.h"
#include "drivers/platform/pcat/graphics/pcat.h"

#include <drivers/pci/pci.h>
#include <kern/kcrt.h>
#include "kern/klog.h"
#include "kern/text-display.h"
#include "kern/pmem.h"
#include <uapi/errno.h>
#include "kern/platform.h"

#define WIDTH 640U
#define HEIGHT 480U
#define CIRRUS_APERTURE cirrus_aperture
#define VGA_APERTURE vga_aperture
#define CIRRUS_STRIDE8 WIDTH
#define CIRRUS_STRIDE24 (WIDTH * 3U)
#define PCI_CIRRUS_VENDOR 0x1013U

/* How many mappings of places the framebuffer moved away from are kept to be removed at the end. */
#define PCAT_RETIRED_MAX 4U

/* How many ranges the end may leave mapped: the retired overflow, the handoff, the frame, the legacy aperture. */
#define PCAT_LEFT_MAX 8U

/* The legacy VGA aperture, inside a window the hardware layer keeps mapped. */
#define PCAT_VGA_APERTURE_PHYSICAL 0x000a0000U
#define PCAT_VGA_APERTURE_BYTES 0x00020000U

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
static volatile uint8_t *vga_aperture, *cirrus_aperture;

/*
 * The boot framebuffer the platform handed over, read once when the
 * hardware is prepared.  linear_framebuffer points at boot_framebuffer when
 * there is one and is NULL otherwise.
 */
static struct kern_boot_framebuffer boot_framebuffer;
static const struct kern_boot_framebuffer *linear_framebuffer;
static volatile uint32_t *linear_pixels;

/*
 * The boot framebuffer once the kernel has it: the physical address it is
 * at now (its device may move it after the boot) and the bytes mapped.
 */
static uint64_t linear_physical;
static size_t linear_bytes;

/*
 * One kernel mapping of display memory the backend keeps (pixels at
 * physical, bytes long), or one range a removal left mapped (pixels NULL,
 * error the removal's error, 0 for one that cannot be removed at all).
 */
struct pcat_mapping {
	volatile uint32_t *pixels;
	uint64_t physical;
	uint64_t bytes;
	int error;
};

/*
 * The mappings of places the framebuffer moved away from: a writer may
 * have held them during the boot, so they stay until the console leaves
 * the display (retired_count of them; one past PCAT_RETIRED_MAX stays for
 * good and is counted in left).  Written by the boot's PCI assignment and
 * by the end only.
 */
static struct pcat_mapping linear_retired[PCAT_RETIRED_MAX];
static unsigned retired_count;

/*
 * Whether the console has left the display (backend_ended), and what the
 * end could not remove (left_count ranges of left).  Written once by the
 * end, under the graphics lock when /dev/graphics has one; a later end
 * reads them under the same lock.
 */
static int backend_ended;
static struct pcat_mapping left[PCAT_LEFT_MAX];
static unsigned left_count;

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
static void left_add(uint64_t physical, uint64_t bytes, int error);
static void mapping_remove(volatile uint32_t *pixels, uint64_t physical, size_t bytes);
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
static void text_console_start(void);
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
		mode->stride = linear_framebuffer->pitch * 4U;
		(void)pcat_graphics_clear();
		kern_logf("graphics: boot framebuffer %ux%ux32 stride=%u\n",
			   mode->width, mode->height, mode->stride);


	/* Reports operation failure. */
		return 1;
	}

	/* Handles the cirrus present condition. */
	requested = mode->preferred_bits_per_pixel == 24U ? 24U : 8U;
	if (cirrus_present && cirrus_enter(requested)) {
		mode->width = WIDTH;
		mode->height = HEIGHT;
		mode->bits_per_pixel = requested;
		mode->stride =
			requested == 24U ? CIRRUS_STRIDE24 : CIRRUS_STRIDE8;
		kern_logf("graphics: PC/AT Cirrus %ux%ux%u stride=%u\n", WIDTH,
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
	kern_logf("graphics: PC/AT VGA fallback %ux%ux4 planar\n", WIDTH,
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
			modes[0].stride = linear_framebuffer->pitch * 4U;
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
	kern_logf("graphics: PC/AT text mode restored\n");
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
				    (size_t)y * linear_framebuffer->pitch +
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

		/* Handles the twice condition. */
		twice = error * 2;
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
			if (patterned &&
			    !pattern_bit(pattern, x, image->pattern_y + y))
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
	int error;

	/* Obtains the drv pcat font get glyph result. */
	error =
		drv_pcat_font_get_glyph(codepoint, bitmap, width, height);

	/* Returns the computed result. */
	return error;
}

/*
 * Implements the drv pcat graphics pci register operation.
 */
int
drv_pcat_graphics_pci_register(
	void)
{
	int error;

	/* Obtains the drv pci driver register result. */
	error = drv_pci_driver_register(&cirrus_driver);

	/* Returns the computed result. */
	return error;
}

/*
 * Implements the drv pcat graphics prepare operation.
 */
int
drv_pcat_graphics_prepare(
	void)
{
	/* Checks the pcat graphics prepare hardware result. */
	backend_prepared = 0;
	if (!pcat_graphics_prepare_hardware())
		return 0;
	backend_prepared = 1;

	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the drv pcat graphics backend ready operation.
 */
/*
 * Reports the linear framebuffer for the text layer.
 */
int
drv_pcat_graphics_backend_get_framebuffer(
	volatile uint32_t **pixels,
	unsigned *width,
	unsigned *height,
	unsigned *stride,
	int *rgbx)
{
	/* Reports no framebuffer until the boot handoff supplied one. */
	if (linear_pixels == NULL || linear_framebuffer == NULL)
		return 0;

	/* Publishes the pixel pointer and the geometry. */
	if (pixels != NULL)
		*pixels = linear_pixels;
	if (width != NULL)
		*width = linear_framebuffer->width;
	if (height != NULL)
		*height = linear_framebuffer->height;
	if (stride != NULL)
		*stride = linear_framebuffer->pitch;
	if (rgbx != NULL) {
		*rgbx = linear_framebuffer->format ==
			KERN_BOOT_FRAMEBUFFER_RGBX8888;
	}

	/* Reports an available framebuffer. */
	return 1;
}

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
	int error;
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
	error = drv_pcat_graphics_backend_fill(&screen, 0);

	/* Returns the computed result. */
	return error;
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

	/* Handles the chip condition. */
	chip = crtc_read(0x27U);
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
		/* Handles the seq index condition. */
		value_local = seq_value[i];
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
		/* Checks the current index. */
		value_local1 = crtc[i];
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
	if (linear_framebuffer->format == KERN_BOOT_FRAMEBUFFER_RGBX8888) {
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
		linear_pixels[(size_t)y * linear_framebuffer->pitch + x] =
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

		/* Handles the distance condition. */
		distance = (unsigned)(dr * dr + dg * dg + db * db);
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
	void *aperture;
	void *mapped;
	int error;

	drv_pcat_font_init();

	/* Reads the boot framebuffer the platform handed over, if any. */
	linear_framebuffer = NULL;
	error = kern_boot_framebuffer("pcat.framebuffer", &boot_framebuffer);
	if (error == 0)
		linear_framebuffer = &boot_framebuffer;

	/*
	 * The boot framebuffer is the kernel's once handed over: it maps the
	 * physical address itself, so it can map it again if the device moves
	 * it; uncached, the type of the HAL's mapping of the same pages (no
	 * two types for one page).
	 */
	linear_pixels = NULL;
	if (linear_framebuffer != NULL) {
		error = kern_device_map(linear_framebuffer->physical_base, (size_t)linear_framebuffer->size, KERN_DEVICE_UNCACHED, &mapped);
		if (error == 0) {
			linear_pixels = mapped;
			linear_physical = linear_framebuffer->physical_base;
			linear_bytes = (size_t)linear_framebuffer->size;

			/* Reports a usable linear framebuffer. */
			text_console_start();
			return 1;
		}

		/* Without the mapping the boot framebuffer is not used. */
		kern_logf("graphics: boot framebuffer at 0x%llx not mapped (%d)\n", (unsigned long long)linear_framebuffer->physical_base,
			  error);
	}

	/* Maps the uncached legacy VGA aperture. */
	if (kern_device_map(0x000a0000U, 0x00020000U,
			    KERN_DEVICE_UNCACHED, &aperture) != 0)
		return 0;
	vga_aperture = (volatile uint8_t *)aperture;

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
		kern_logf("graphics: PCI Cirrus absent; VGA fallback ready\n");

	/*
	 * Without a framebuffer the text layer falls back to VGA text
	 * memory, which this aperture has just made reachable.
	 */
	text_console_start();

	/* Reports operation failure. */
	return 1;
}

/*
 * Brings up the text grid and hands HAL output over to it.
 *
 * From here on kern_logf() and /dev/console share one cursor. Nothing
 * happens when the layer found no surface: the HAL early console keeps
 * the screen and the kernel simply has no console device.
 */
static void
text_console_start(
	void)
{
	int ready;

	/* Publishes the retained-cell renderer before any kernel log is routed to it. */
	drv_pcat_text_init();

	/* Keeps early output until the complete text backend can draw and report snapshot mutations. */
	ready = drv_pcat_text_ready();
	if (ready != 0) {
		__atomic_store_n(
			&kernel_putc,
			kern_text_kernel_putc,
			__ATOMIC_RELEASE);
	}

	/* Succeeded: available kernel text now uses the same redraw notification as console writes. */
	return;
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
	kern_logf("graphics: PCI Cirrus %04x:%04x at %u:%u.%u\n",
		   drv_pci_device_vendor(device),
		   drv_pci_device_product(device), address.bus, address.device,
		   address.function);

	/* Checks the drv pci device bar result. */
	if (drv_pci_device_bar(device, 0, &bar) == 0) {
		kern_logf("graphics: Cirrus BAR0=%08x size=%u KiB\n",
			   (unsigned)bar.bus_address,
			   (unsigned)(bar.size / 1024U));
	}

	/* Succeeded. */
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

	/* Succeeded. */
	return 0;
}

/*
 * Follows the boot framebuffer after its device moved the physical range
 * [from, from + length) to to: maps the framebuffer's new place and has the
 * text console draw there.  The old mapping stays until the console leaves
 * the display (a writer may still hold it; what it writes now reaches
 * nothing).  Nothing changes for a framebuffer outside the range, or once
 * the console has left the display.
 */
void
drv_pcat_graphics_framebuffer_moved(
	uint64_t from,
	uint64_t to,
	uint64_t length)
{
	volatile uint32_t *previous;
	uint64_t physical;
	void *mapped;
	int error;

	/* Only the framebuffer the kernel mapped, inside the moved range, while the console uses it. */
	if (backend_ended || linear_pixels == NULL || linear_physical < from || linear_physical - from >= length)
		return;
	physical = to + (linear_physical - from);

	/* The new place, mapped as the old one. */
	error = kern_device_map(physical, linear_bytes, KERN_DEVICE_UNCACHED, &mapped);
	if (error != 0) {
		kern_logf("graphics: boot framebuffer moved to 0x%llx, not mapped (%d)\n", (unsigned long long)physical, error);
		return;
	}

	/* The old mapping is kept to be removed when the console leaves the display (one too many stays for good). */
	previous = linear_pixels;
	if (retired_count < PCAT_RETIRED_MAX) {
		linear_retired[retired_count].pixels = previous;
		linear_retired[retired_count].physical = linear_physical;
		linear_retired[retired_count].bytes = linear_bytes;
		retired_count++;
	} else {
		left_add(linear_physical, linear_bytes, 0);
	}

	/* The text console, then the graphics paths, draw through it. */
	drv_pcat_text_retarget(previous, mapped);
	linear_pixels = mapped;
	linear_physical = physical;
	kern_logf("graphics: boot framebuffer moved to 0x%llx\n", (unsigned long long)physical);
}

/*
 * Gives the display up for good, once the console has left it: nothing is
 * drawn through the backend again, and every kernel mapping of display
 * memory it made (the framebuffer, the places it moved away from, the
 * legacy and Cirrus apertures) is removed, with the hardware layer's early
 * console mapping of the boot framebuffer, which the hardware layer hands
 * over to one mapping of exactly that framebuffer once the kernel prints.
 * Returns 0 when nothing left mapped lies in the count ranges, the error
 * of a removal that failed for a range in them, or EBUSY for a range in
 * them that cannot be removed.  A later call judges what the first left
 * against its own ranges.  The caller holds the graphics lock, or it
 * runs before /dev/graphics has one.
 */
int
drv_pcat_graphics_backend_end(
	const struct kern_text_range *ranges,
	unsigned count)
{
	void *handed;
	unsigned index;
	int overlaps;
	int error;

	/* The first end removes the mappings; a later one only judges. */
	if (!backend_ended) {
		/* Nothing is drawn through the backend from now on. */
		backend_ended = 1;
		backend_prepared = 0;
		active_backend = DISPLAY_NONE;

		/* The framebuffer the kernel drew through. */
		if (linear_pixels != NULL)
			mapping_remove(linear_pixels, linear_physical, linear_bytes);
		linear_pixels = NULL;

		/* The places the framebuffer moved away from. */
		for (index = 0U; index < retired_count; index++)
			mapping_remove(linear_retired[index].pixels, linear_retired[index].physical, (size_t)linear_retired[index].bytes);
		retired_count = 0U;

		/*
		 * The hardware layer's early console mapping, handed over by a
		 * mapping of exactly the boot framebuffer and removed at once.
		 * Without the hand-over the mapping made is an ordinary one, and
		 * it goes the same way.
		 */
		if (linear_framebuffer != NULL) {
			error = kern_device_map(linear_framebuffer->physical_base, (size_t)linear_framebuffer->size, KERN_DEVICE_UNCACHED, &handed);
			if (error == 0) {
				mapping_remove(handed, linear_framebuffer->physical_base, (size_t)linear_framebuffer->size);
			} else {
				left_add(linear_framebuffer->physical_base, (size_t)linear_framebuffer->size, error);
			}
		}

		/* The Cirrus aperture (the device stays claimed; its registers are not written). */
		if (cirrus_mapping.address != NULL) {
			drv_pci_device_unmap_bar(cirrus_device, &cirrus_mapping);
			cirrus_mapping.address = NULL;
		}
		cirrus_aperture = NULL;

		/*
		 * The legacy aperture's mapping is given back like the others, but
		 * it lies in a window the hardware layer keeps mapped for good, so
		 * its range counts as left whatever the removal reports.
		 */
		if (vga_aperture != NULL) {
			(void)kern_device_unmap((void *)(uintptr_t)vga_aperture, PCAT_VGA_APERTURE_BYTES);
			left_add(PCAT_VGA_APERTURE_PHYSICAL, PCAT_VGA_APERTURE_BYTES, 0);
		}
		vga_aperture = NULL;
	}

	/* What was left, against the caller's ranges: a failed removal first. */
	for (index = 0U; index < left_count; index++) {
		/* A range of the caller's memory whose removal failed. */
		overlaps = kern_text_range_overlaps(ranges, count, left[index].physical, left[index].bytes);
		if (overlaps && left[index].error != 0)
			return left[index].error;
	}

	/* Then a range that stays mapped for good. */
	for (index = 0U; index < left_count; index++) {
		/* A range of the caller's memory that cannot be removed. */
		overlaps = kern_text_range_overlaps(ranges, count, left[index].physical, left[index].bytes);
		if (overlaps)
			return EBUSY;
	}

	/* Succeeded: nothing the backend mapped is left in the ranges. */
	return 0;
}

/*
 * Records a range of display memory the end leaves mapped, with the error
 * of its removal (0 for one that cannot be removed).  A full table keeps
 * its last entry as the whole address space, so that every range judged
 * later finds it.
 */
static void
left_add(
	uint64_t physical,
	uint64_t bytes,
	int error)
{
	/* A table with room takes the range itself. */
	if (left_count < PCAT_LEFT_MAX) {
		left[left_count].pixels = NULL;
		left[left_count].physical = physical;
		left[left_count].bytes = bytes;
		left[left_count].error = error;
		left_count++;
		return;
	}

	/* A full table stands for everything from now on. */
	left[PCAT_LEFT_MAX - 1U].physical = 0U;
	left[PCAT_LEFT_MAX - 1U].bytes = UINT64_MAX;
	left[PCAT_LEFT_MAX - 1U].error = 0;
}

/*
 * Removes one kernel mapping of display memory, recording the range as
 * left when the removal fails (EBUSY from the hardware layer: a window it
 * keeps mapped).
 */
static void
mapping_remove(
	volatile uint32_t *pixels,
	uint64_t physical,
	size_t bytes)
{
	int error;

	/* The kernel's own call, which asks the hardware layer. */
	error = kern_device_unmap((void *)(uintptr_t)pixels, bytes);

	/* A window the hardware layer keeps is left for good; another failure is left with its error. */
	if (error == EBUSY) {
		left_add(physical, bytes, 0);
	} else if (error != 0) {
		left_add(physical, bytes, error);
	}

	/* Succeeded: the mapping is gone or recorded as left. */
	return;
}
