/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (c) 1996-2024, Keiichi Tabata
 * Copyright (c) 2025, 2026, Awe Morris
 *
 * zedBSD graphics NEC PC-9821 Core-Graph / Cirrus GD5440 display backend, imported
 * from Boots.  The register sequence is adapted from StratoHAL
 * 98disp_cirrus.c at commit 76e909577bdf4629f11e473539b446a948fef830 and
 * is deliberately limited to the Core-Graph path at 640x480x8/24.
 * Port I/O and the linear framebuffer are injected by the embedder so
 * the driver stays compiler and host neutral.
 */

#include "drivers/graphics/pc98/display-cirrus.h"

#include <string.h>

#define WAB_INDEX 0x0faaU
#define WAB_DATA 0x0fabU
#define WAB_REG_ID 0x00U
#define WAB_REG_WINDOW 0x01U
#define WAB_REG_LINEAR 0x02U
#define WAB_REG_RELAY 0x03U
#define WAB_RELAY_SETUP 0x01U
#define WAB_RELAY_WAB 0x03U
#define CIRRUS_SLEEP 0x0ca3U
#define CIRRUS_IO 0x0ca0U
#define CIRRUS_CRTC 0x0da4U
#define CIRRUS_CRTC_MONO 0x0ba4U
#define CIRRUS_STATUS 0x0daaU
#define PC98_WAIT 0x005fU
#define PC98_GDC_MODE 0x0068U
#define PC98_VRAM_SWITCH 0x006aU

static uint8_t
in8(struct pc98_cirrus *backend, uint16_t port)
{
	return backend->port_in8(backend->io_context, port);
}

static void
out8(struct pc98_cirrus *backend, uint16_t port, uint8_t value)
{
	backend->port_out8(backend->io_context, port, value);
}

static void
wab_write(struct pc98_cirrus *backend, uint8_t index,
	  uint8_t value)
{
	out8(backend, WAB_INDEX, index);
	out8(backend, WAB_DATA, value);
}

static uint8_t
wab_read(struct pc98_cirrus *backend, uint8_t index)
{
	out8(backend, WAB_INDEX, index);
	return in8(backend, WAB_DATA);
}

static void
seq_write(struct pc98_cirrus *backend, uint8_t index,
	  uint8_t value)
{
	out8(backend, CIRRUS_IO + 4U, index);
	out8(backend, CIRRUS_IO + 5U, value);
}

static uint8_t
seq_read(struct pc98_cirrus *backend, uint8_t index)
{
	out8(backend, CIRRUS_IO + 4U, index);
	return in8(backend, CIRRUS_IO + 5U);
}

static void
gfx_write(struct pc98_cirrus *backend, uint8_t index,
	  uint8_t value)
{
	out8(backend, CIRRUS_IO + 0x0eU, index);
	out8(backend, CIRRUS_IO + 0x0fU, value);
}

static void
crtc_write(struct pc98_cirrus *backend, uint8_t index,
	   uint8_t value)
{
	uint16_t port = (in8(backend, CIRRUS_IO + 0x0cU) & 1U) ?
		CIRRUS_CRTC : CIRRUS_CRTC_MONO;

	out8(backend, port, index);
	out8(backend, port + 1U, value);
}

static uint8_t
crtc_read(struct pc98_cirrus *backend, uint8_t index)
{
	uint16_t port = (in8(backend, CIRRUS_IO + 0x0cU) & 1U) ?
		CIRRUS_CRTC : CIRRUS_CRTC_MONO;

	out8(backend, port, index);
	return in8(backend, port + 1U);
}

static void
hidden_dac_write(struct pc98_cirrus *backend, uint8_t value)
{
	unsigned i;

	(void)in8(backend, CIRRUS_IO + 8U);
	for (i = 0; i < 4; i++)
		(void)in8(backend, CIRRUS_IO + 6U);
	out8(backend, CIRRUS_IO + 6U, value);
}

static void
load_rgb332_palette(struct pc98_cirrus *backend)
{
	unsigned i;

	out8(backend, CIRRUS_IO + 6U, 0xffU);
	out8(backend, CIRRUS_IO + 8U, 0);
	for (i = 0; i < 256; i++) {
		unsigned red = (i >> 5) & 7U;
		unsigned green = (i >> 2) & 7U;
		unsigned blue = i & 3U;

		out8(backend, CIRRUS_IO + 9U, (uint8_t)(red * 63U / 7U));
		out8(backend, CIRRUS_IO + 9U, (uint8_t)(green * 63U / 7U));
		out8(backend, CIRRUS_IO + 9U, (uint8_t)(blue * 63U / 3U));
	}
}

static int
coregraph_id_present(struct pc98_cirrus *backend)
{
	uint8_t id = wab_read(backend, WAB_REG_ID);

	return id >= 0x58U && id <= 0x5dU;
}

static void
coregraph_gate_enter(struct pc98_cirrus *backend)
{
	out8(backend, PC98_GDC_MODE, 0x0eU);
	out8(backend, PC98_VRAM_SWITCH, 0x07U);
	out8(backend, PC98_VRAM_SWITCH, 0x8fU);
	out8(backend, PC98_VRAM_SWITCH, 0x06U);
	wab_write(backend, WAB_REG_RELAY, WAB_RELAY_WAB);
	out8(backend, PC98_WAIT, 0);
	out8(backend, PC98_WAIT, 0);
	out8(backend, CIRRUS_SLEEP, 0x01U);
}

static void
coregraph_gate_leave(struct pc98_cirrus *backend)
{
	unsigned i;

	out8(backend, CIRRUS_SLEEP, 0);
	wab_write(backend, WAB_REG_RELAY, 0);
	out8(backend, PC98_WAIT, 0);
	out8(backend, PC98_VRAM_SWITCH, 0x07U);
	out8(backend, PC98_VRAM_SWITCH, 0x8eU);
	out8(backend, PC98_VRAM_SWITCH, 0x06U);
	for (i = 0; i < 200000U; i++)
		out8(backend, PC98_WAIT, 0);
	out8(backend, PC98_GDC_MODE, 0x0fU);
}

static void
coregraph_mode_640x480(struct pc98_cirrus *backend,
		       unsigned bits_per_pixel)
{
	static const uint8_t seq_index[] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x07, 0x08,
		0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x16, 0x18,
		0x1b, 0x1c, 0x1d, 0x1e, 0x1f
	};
	static const uint8_t seq_value[] = {
		0x01, 0x01, 0x0f, 0x00, 0x0e, 0x11, 0x00,
		0x66, 0x48, 0x56, 0x60, 0x30, 0x58, 0x40,
		0x3b, 0x23, 0x3d, 0x3b, 0x20
	};
	static const uint8_t crtc[0x1c] = {
		0x5f,0x4f,0x50,0x84,0x54,0x80,0x0b,0x3e,
		0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,
		0xe5,0x87,0xdf,0x50,0x00,0xe7,0x04,0xe3,
		0xff,0x00,0x90,0x22
	};
	static const uint8_t graphics[9] = {
		0x00,0x00,0x00,0x00,0x00,0x40,0x05,0x0f,0xff
	};
	static const uint8_t attribute[21] = {
		0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
		0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
		0x41,0x00,0x0f,0x00,0x00
	};
	unsigned i;

	gfx_write(backend, 0x33U, 0);
	gfx_write(backend, 0x31U, 0x04U);
	gfx_write(backend, 0x31U, 0);
	seq_write(backend, 0x06U, 0x12U);
	seq_write(backend, 0x12U, 0);
	for (i = 0; i < sizeof(seq_index); i++) {
		uint8_t value = seq_value[i];

		if (seq_index[i] == 0x07U && bits_per_pixel == 24U)
			value = 0x15U;
		seq_write(backend, seq_index[i], value);
	}
	seq_write(backend, 0x0fU,
		  (uint8_t)((seq_read(backend, 0x0fU) & 0xdfU) | 0x20U));
	out8(backend, CIRRUS_IO + 2U, 0xe3U);
	gfx_write(backend, 0x06U, 0x05U);
	seq_write(backend, 0x00U, 0x03U);
	crtc_write(backend, 0x11U, 0x20U);
	for (i = 0; i < sizeof(crtc); i++) {
		uint8_t value = crtc[i];

		if (i == 0x13U && bits_per_pixel == 24U)
			value = 0xf0U;
		crtc_write(backend, (uint8_t)i, value);
	}
	for (i = 0; i < sizeof(graphics); i++)
		gfx_write(backend, (uint8_t)i, graphics[i]);
	(void)in8(backend, CIRRUS_STATUS);
	for (i = 0; i < sizeof(attribute); i++) {
		out8(backend, CIRRUS_IO, (uint8_t)i);
		out8(backend, CIRRUS_IO, attribute[i]);
	}
	(void)in8(backend, CIRRUS_STATUS);
	out8(backend, CIRRUS_IO, 0x20U);
	hidden_dac_write(backend, bits_per_pixel == 24U ? 0xc5U : 0x20U);
	out8(backend, CIRRUS_IO + 6U, 0xffU);
	gfx_write(backend, 0x09U, 0);
	gfx_write(backend, 0x0aU, 0);
	gfx_write(backend, 0x0bU, 0x21U);
	seq_write(backend, 0x17U,
		  (uint8_t)(seq_read(backend, 0x17U) | 0x44U));
	seq_write(backend, 0x18U,
		  (uint8_t)(seq_read(backend, 0x18U) & 0xbfU));
	gfx_write(backend, 0x31U, 0x04U);
	gfx_write(backend, 0x31U, 0);
	if (bits_per_pixel == 8U)
		load_rgb332_palette(backend);
	seq_write(backend, 0x01U, 0x21U);
}

static uint8_t
rgb332(uint32_t color)
{
	return (uint8_t)(((color >> 16) & 0xe0U) |
			 ((color >> 11) & 0x1cU) | ((color >> 6) & 0x03U));
}

static int
pattern_bit(uint64_t pattern, unsigned x, unsigned y)
{
	uint8_t row = (uint8_t)(pattern >> ((y & 7U) * 8U));

	return (row & (uint8_t)(0x80U >> (x & 7U))) != 0;
}

static void
write_pixel(struct pc98_cirrus *backend, unsigned x, unsigned y,
	    uint32_t color)
{
	volatile uint8_t *pixel;

	if (backend->bits_per_pixel == 8U) {
		backend->framebuffer[y * PC98_DISPLAY_CIRRUS_STRIDE_8 + x] =
			rgb332(color);
		return;
	}
	pixel = backend->framebuffer + y * PC98_DISPLAY_CIRRUS_STRIDE_24 + x * 3U;
	pixel[0] = (uint8_t)color;
	pixel[1] = (uint8_t)(color >> 8);
	pixel[2] = (uint8_t)(color >> 16);
}

static int
cirrus_enter(void *context, struct pc98_display_info *info)
{
	struct pc98_cirrus *backend = context;
	uint8_t relay_setup;
	uint8_t chip;
	unsigned bits_per_pixel;
	unsigned stride;
	unsigned visible_bytes;
	unsigned i;

	if (backend == NULL || info == NULL || backend->port_in8 == NULL ||
	    backend->port_out8 == NULL || backend->framebuffer == NULL ||
	    !coregraph_id_present(backend))
		return 0;
	bits_per_pixel = info->preferred_bits_per_pixel == 24U ? 24U : 8U;
	stride = bits_per_pixel == 24U ? PC98_DISPLAY_CIRRUS_STRIDE_24 :
		PC98_DISPLAY_CIRRUS_STRIDE_8;
	visible_bytes = stride * PC98_DISPLAY_CIRRUS_HEIGHT;
	backend->saved_sleep = in8(backend, CIRRUS_SLEEP);
	backend->saved_window = wab_read(backend, WAB_REG_WINDOW);
	backend->saved_linear = wab_read(backend, WAB_REG_LINEAR);
	backend->saved_relay = wab_read(backend, WAB_REG_RELAY);
	out8(backend, CIRRUS_SLEEP, (uint8_t)(backend->saved_sleep | 1U));
	relay_setup = (uint8_t)((backend->saved_relay & (uint8_t)~2U) |
				WAB_RELAY_SETUP);
	wab_write(backend, WAB_REG_RELAY, relay_setup);
	/* The motherboard ID is only a hint; validate the temporarily woken VGA. */
	seq_write(backend, 0x06U, 0x12U);
	chip = crtc_read(backend, 0x27U);
	if (chip == 0 || chip == 0xffU)
		goto fail;
	wab_write(backend, WAB_REG_LINEAR, 0xf0U);
	if (wab_read(backend, WAB_REG_LINEAR) != 0xf0U)
		goto fail;
	coregraph_mode_640x480(backend, bits_per_pixel);
	/* Keep the motherboard GDC on the monitor while Cirrus is configured
	 * and its visible framebuffer is erased.  WAB_REG_RELAY bit 1 in
	 * coregraph_gate_enter() is the actual GDC-to-Cirrus scanout switch. */
	for (i = 0; i < visible_bytes; i++)
		backend->framebuffer[i] = 0;
	coregraph_gate_enter(backend);
	seq_write(backend, 0x01U, 0x01U);
	backend->bits_per_pixel = (uint8_t)bits_per_pixel;
	backend->active = 1;
	info->width = PC98_DISPLAY_CIRRUS_WIDTH;
	info->height = PC98_DISPLAY_CIRRUS_HEIGHT;
	info->bits_per_pixel = bits_per_pixel;
	info->stride = stride;
	return 1;

fail:
	wab_write(backend, WAB_REG_LINEAR, backend->saved_linear);
	wab_write(backend, WAB_REG_WINDOW, backend->saved_window);
	wab_write(backend, WAB_REG_RELAY, backend->saved_relay);
	out8(backend, CIRRUS_SLEEP, backend->saved_sleep);
	return 0;
}

static void
cirrus_leave(void *context)
{
	struct pc98_cirrus *backend = context;

	if (backend == NULL || !backend->active)
		return;
	seq_write(backend, 0x01U, 0x21U);
	coregraph_gate_leave(backend);
	wab_write(backend, WAB_REG_LINEAR, backend->saved_linear);
	wab_write(backend, WAB_REG_WINDOW, backend->saved_window);
	/* coregraph_gate_leave() deliberately selects the motherboard GDC and
	 * puts Cirrus to sleep.  Restoring the saved relay/sleep registers here
	 * would immediately select and wake Cirrus again, leaving later text-VRAM
	 * output invisible.  Saved values are only for the failed-enter rollback. */
	backend->active = 0;
	backend->bits_per_pixel = 0;
}

static int
cirrus_fill(void *context, const struct pc98_display_rect *rect,
	    uint32_t color)
{
	struct pc98_cirrus *backend = context;
	unsigned y;

	if (backend->bits_per_pixel == 8U) {
		uint8_t pixel = rgb332(color);

		for (y = rect->y; y < rect->y + rect->height; y++) {
			volatile uint8_t *row = backend->framebuffer +
				y * PC98_DISPLAY_CIRRUS_STRIDE_8 + rect->x;
			unsigned x;

			for (x = 0; x < rect->width; x++)
				row[x] = pixel;
		}
		return 1;
	}
	for (y = rect->y; y < rect->y + rect->height; y++) {
		volatile uint8_t *row = backend->framebuffer +
			y * PC98_DISPLAY_CIRRUS_STRIDE_24 + rect->x * 3U;
		unsigned x;

		for (x = 0; x < rect->width; x++) {
			row[x * 3U] = (uint8_t)color;
			row[x * 3U + 1U] = (uint8_t)(color >> 8);
			row[x * 3U + 2U] = (uint8_t)(color >> 16);
		}
	}
	return 1;
}

static int
cirrus_line(void *context, unsigned x0, unsigned y0, unsigned x1,
	    unsigned y1, uint32_t color)
{
	struct pc98_cirrus *backend = context;
	int x = (int)x0;
	int y = (int)y0;
	int target_x = (int)x1;
	int target_y = (int)y1;
	int delta_x = target_x >= x ? target_x - x : x - target_x;
	int step_x = x < target_x ? 1 : -1;
	int delta_y = target_y >= y ? y - target_y : target_y - y;
	int step_y = y < target_y ? 1 : -1;
	int error = delta_x + delta_y;

	for (;;) {
		int twice_error;

		write_pixel(backend, (unsigned)x, (unsigned)y, color);
		if (x == target_x && y == target_y)
			break;
		twice_error = error * 2;
		if (twice_error >= delta_y) {
			error += delta_y;
			x += step_x;
		}
		if (twice_error <= delta_x) {
			error += delta_x;
			y += step_y;
		}
	}
	return 1;
}

static int
cirrus_pattern_fill(void *context, const struct pc98_display_rect *rect,
		    uint32_t color, uint64_t pattern)
{
	struct pc98_cirrus *backend = context;
	unsigned y;

	for (y = 0; y < rect->height; y++) {
		unsigned x;

		for (x = 0; x < rect->width; x++)
			if (pattern_bit(pattern, x, y))
				write_pixel(backend, rect->x + x, rect->y + y,
					    color);
	}
	return 1;
}

static int
cirrus_draw_image_common(void *context, unsigned destination_x,
			 unsigned destination_y,
			 const struct pc98_display_image *image, uint64_t pattern)
{
	struct pc98_cirrus *backend = context;
	unsigned y;

	for (y = 0; y < image->height; y++) {
		const uint8_t *row = image->pixels + (size_t)y * image->stride;
		unsigned x;

		for (x = 0; x < image->width; x++) {
			uint32_t rgb;

			if (!pattern_bit(pattern, x, y))
				continue;
			if (image->format == PC98_DISPLAY_IMAGE_INDEX8) {
				unsigned index = row[x];

				rgb = index < image->palette_size ?
					image->palette[index] : 0;
			} else {
				const uint8_t *source = row + (size_t)x * 3U;

				rgb = ((uint32_t)source[0] << 16) |
				      ((uint32_t)source[1] << 8) | source[2];
			}
			write_pixel(backend, destination_x + x,
				    destination_y + y, rgb);
		}
	}
	return 1;
}

static int
cirrus_draw_image(void *context, unsigned x, unsigned y,
		  const struct pc98_display_image *image)
{
	return cirrus_draw_image_common(context, x, y, image, UINT64_MAX);
}

static int
cirrus_draw_image_pattern(void *context, unsigned x, unsigned y,
			  const struct pc98_display_image *image,
			  uint64_t pattern)
{
	return cirrus_draw_image_common(context, x, y, image, pattern);
}

static int
cirrus_flush(void *context, const struct pc98_display_rect *rectangles,
	     size_t rectangle_count)
{
	(void)context;
	(void)rectangles;
	(void)rectangle_count;
	return 1;
}

void
pc98_cirrus_default(struct pc98_cirrus *backend,
			       pc98_in8_fn port_in8,
			       pc98_out8_fn port_out8,
			       void *io_context, volatile uint8_t *framebuffer)
{
	memset(backend, 0, sizeof(*backend));
	backend->port_in8 = port_in8;
	backend->port_out8 = port_out8;
	backend->io_context = io_context;
	backend->framebuffer = framebuffer;
}

int
pc98_cirrus_make_hal(struct pc98_display_backend *hal,
				 struct pc98_cirrus *backend)
{
	if (hal == NULL || backend == NULL)
		return 0;
	memset(hal, 0, sizeof(*hal));
	hal->display.context = backend;
	hal->display.enter = cirrus_enter;
	hal->display.leave = cirrus_leave;
	hal->display.fill = cirrus_fill;
	hal->display.line = cirrus_line;
	hal->display.pattern_fill = cirrus_pattern_fill;
	hal->display.draw_image = cirrus_draw_image;
	hal->display.draw_image_pattern = cirrus_draw_image_pattern;
	hal->display.flush = cirrus_flush;
	return 1;
}
