/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (c) 2025, 2026, Awe Morris
 *
 * zedBSD graphics PC-98 CGROM glyph backend, imported from Boots.
 * CGROM selection and read sequencing is adapted from StratoHAL 98glyph.c,
 * commit 76e909577bdf4629f11e473539b446a948fef830.  In particular, preserve
 * its VSYNC exclusion, 0x68 mode switch, CG-window byte layout, and symbol
 * bank handling; these details matter on physical PC-98 hardware.
 */

#include "drivers/platform/pc98/graphics/display-glyph.h"

#include <string.h>

/* Share the kernel's JIS X 0208 table instead of carrying a duplicate. */
extern const uint16_t hal_pc98_jisx0208_to_ucs[7896];

static unsigned glyph_width(uint16_t jis);
static int read_font(struct pc98_glyph *backend, uint16_t jis, uint8_t font[32]);
static void wait_vsync(struct pc98_glyph *backend);
static int glyph_measure(void *context, uint32_t codepoint, unsigned *width, unsigned *height);
static uint64_t font_pattern(const uint8_t font[32], unsigned bytes_per_row, unsigned byte_index, unsigned first_row);
static int glyph_draw(void *context, unsigned x, unsigned y, uint32_t codepoint, uint32_t foreground, uint32_t background);

/*
 * Implements the drv pc98 unicode to jis operation.
 */
uint16_t
drv_pc98_unicode_to_jis(
	uint32_t codepoint)
{
	unsigned row;
	unsigned cell;
	size_t index;
	uint16_t value;

	/* Handles the codepoint condition. */
	if (codepoint > 0xffffU)
		return 0;
	value = (uint16_t)codepoint;

	/* Validates the current value. */
	if (value < 0x80U)
		return (uint16_t)(0x2000U | value);

	/* Validates the current value. */
	if (value >= 0xff61U && value <= 0xff9fU)
		return (uint16_t)(0x20a1U + (value - 0xff61U));

	/* Process each remaining element. */
	for (index = 0; index < 7896U; index++) {
		/* Handles the hal pc98 jisx0208 to ucs condition. */
		if (hal_pc98_jisx0208_to_ucs[index] == value) {
			row = (unsigned)(index / 94U) + 0x21U;
			cell = (unsigned)(index % 94U) + 0x21U;

			/* Returns the computed result. */
			return (uint16_t)((row << 8) | cell);
		}
	}

	/* Reports successful completion. */
	return 0;
}

/*
 * Implements the drv pc98 glyph get bitmap operation.
 */
int
drv_pc98_glyph_get_bitmap(
	struct pc98_glyph *backend,
	uint32_t codepoint,
	uint8_t font[32],
	unsigned *width,
	unsigned *height)
{
	int function_result;
	uint16_t jis;

	/* Handles the backend availability. */
	if (backend == NULL || font == NULL || width == NULL ||
	    height == NULL || codepoint > 0x10ffffU ||
	    (codepoint >= 0xd800U && codepoint <= 0xdfffU))

		/* Reports successful completion. */
		return 0;
	jis = drv_pc98_unicode_to_jis(codepoint);

	/* Handles the jis condition. */
	if (jis == 0)
		jis = drv_pc98_unicode_to_jis('?');
	*width = glyph_width(jis);
	*height = 16U;
	/* Obtains the read font result. */
	function_result = read_font(backend, jis, font);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pc98 glyph default operation.
 */
void
drv_pc98_glyph_default(
	struct pc98_glyph *backend,
	struct pc98_display_ops *display,
	pc98_in8_fn port_in8,
	pc98_out8_fn port_out8,
	void *io_context)
{
	memset(backend, 0, sizeof(*backend));
	backend->port_in8 = port_in8;
	backend->port_out8 = port_out8;
	backend->io_context = io_context;
	backend->cg_window = (volatile uint8_t *)0x800a4000U;
	backend->display = display;
}

/*
 * Implements the drv pc98 glyph make hal operation.
 */
int
drv_pc98_glyph_make_hal(
	struct pc98_glyph_ops *hal,
	struct pc98_glyph *backend)
{
	/* Handles the hal availability. */
	if (hal == NULL || backend == NULL || backend->display == NULL ||
	    backend->port_in8 == NULL || backend->port_out8 == NULL ||
	    backend->cg_window == NULL)

		/* Reports successful completion. */
		return 0;
	memset(hal, 0, sizeof(*hal));
	hal->context = backend;
	hal->measure = glyph_measure;
	hal->draw = glyph_draw;

	/* Reports operation failure. */
	return 1;
}

/* Supports the glyph width operation. */
static unsigned
glyph_width(
	uint16_t jis)
{
	/* Returns the computed result. */
	return (jis >> 8) == 0x20U ? 8U : 16U;
}

/* Supports the read font operation. */
static int
read_font(
	struct pc98_glyph *backend,
	uint16_t jis,
	uint8_t font[32])
{
	uint8_t row = (uint8_t)(jis >> 8);
	uint8_t cell = (uint8_t)jis;
	int special = (row >= 0x29U && row <= 0x2fU) ||
		      (row >= 0x76U && row <= 0x7fU);
	unsigned index;

	/* Handles the port in8 availability. */
	if (backend->port_in8 == NULL || backend->port_out8 == NULL ||
	    backend->cg_window == NULL)

		/* Reports successful completion. */
		return 0;

	/* Handles the special condition. */
	if (!special) {
		/* Process each remaining element. */
		for (index = 0; index < 64U; index++) {
			/* Handles the backend condition. */
			if (backend->cache[index].valid &&
			    backend->cache[index].jis == jis) {
				memcpy(font, backend->cache[index].font, 32);

				/* Reports operation failure. */
				return 1;
			}
		}
	}
	memset(font, 0, 32);
	wait_vsync(backend);
	backend->port_out8(backend->io_context, 0x68, 0x0b);

	/* Handles the row condition. */
	if (row == 0x20U) {
		backend->port_out8(backend->io_context, 0xa1, 0x00);
		backend->port_out8(backend->io_context, 0xa3, cell);
		backend->port_out8(backend->io_context, 0xa5, 0x00);
		/* Process each remaining element. */
		for (index = 0; index < 16U; index++)
			font[index] = backend->cg_window[index * 2U + 1U];
	} else if (!special) {
		backend->port_out8(backend->io_context, 0xa1, cell);
		backend->port_out8(backend->io_context, 0xa3,
				   (uint8_t)(row - 0x20U));
		backend->port_out8(backend->io_context, 0xa5, 0x00);
		/* Process each remaining element. */
		for (index = 0; index < 32U; index++)
			font[index] = backend->cg_window[index];
	} else {
		backend->port_out8(backend->io_context, 0xa1, cell);
		backend->port_out8(backend->io_context, 0xa3,
				   (uint8_t)(row - 0x20U));
		backend->port_out8(backend->io_context, 0xa5, 0x20);
		/* Process each remaining element. */
		for (index = 0; index < 16U; index++)
			font[index * 2U] = backend->cg_window[index * 2U + 1U];
		backend->port_out8(backend->io_context, 0xa5, 0x00);
		/* Process each remaining element. */
		for (index = 0; index < 16U; index++) {
			font[index * 2U + 1U] =
				backend->cg_window[index * 2U + 1U];
		}
	}

	backend->port_out8(backend->io_context, 0x68, 0x0a);

	/* Handles the special condition. */
	if (!special) {
		index = backend->cache_next++ % 64U;
		backend->cache[index].jis = jis;
		backend->cache[index].valid = 1;
		memcpy(backend->cache[index].font, font, 32);
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the wait vsync operation. */
static void
wait_vsync(
	struct pc98_glyph *backend)
{
	/* Continue while the operation condition remains true. */
	while (backend->port_in8(backend->io_context, 0x60) & 0x20U)
		;
	/* Continue while the operation condition remains true. */
	while (!(backend->port_in8(backend->io_context, 0x60) & 0x20U))
		;
}

/* Supports the glyph measure operation. */
static int
glyph_measure(
	void *context,
	uint32_t codepoint,
	unsigned *width,
	unsigned *height)
{
	uint16_t jis;

	(void)context;

	/* Handles the width availability. */
	if (width == NULL || height == NULL)
		return 0;
	jis = drv_pc98_unicode_to_jis(codepoint);

	/* Handles the jis condition. */
	if (jis == 0)
		jis = drv_pc98_unicode_to_jis('?');
	*width = glyph_width(jis);
	*height = 16;
	/* Reports operation failure. */
	return 1;
}

/* Supports the font pattern operation. */
static uint64_t
font_pattern(
	const uint8_t font[32],
	unsigned bytes_per_row,
	unsigned byte_index,
	unsigned first_row)
{
	uint64_t pattern = 0;
	unsigned row;

	/* Process each element required by the operation. */
	for (row = 0; row < 8U; row++) {
		pattern |= (uint64_t)font[(first_row + row) * bytes_per_row +
					  byte_index]
			   << (row * 8U);
	}

	/* Returns the computed result. */
	return pattern;
}

/* Supports the glyph draw operation. */
static int
glyph_draw(
	void *context,
	unsigned x,
	unsigned y,
	uint32_t codepoint,
	uint32_t foreground,
	uint32_t background)
{
	struct pc98_glyph *backend = context;
	struct pc98_display_rect rectangle;
	uint8_t font[32];
	uint16_t jis = drv_pc98_unicode_to_jis(codepoint);
	unsigned width;
	unsigned column;
	unsigned band;

	/* Handles the jis condition. */
	if (jis == 0)
		jis = drv_pc98_unicode_to_jis('?');
	width = glyph_width(jis);

	/* Checks the read font result. */
	if (backend == NULL || backend->display == NULL ||
	    backend->display->fill == NULL ||
	    backend->display->pattern_fill == NULL ||
	    !read_font(backend, jis, font))

		/* Reports successful completion. */
		return 0;

	rectangle.x = x;
	rectangle.y = y;
	rectangle.width = width;
	rectangle.height = 16;

	/* Checks the fill result. */
	if (!backend->display->fill(backend->display->context, &rectangle,
				    background))

		/* Reports successful completion. */
		return 0;

	/* Process each element required by the operation. */
	for (band = 0; band < 2U; band++) {
		/* Process each element required by the operation. */
		for (column = 0; column < width / 8U; column++) {
			rectangle.x = x + column * 8U;
			rectangle.y = y + band * 8U;
			rectangle.width = 8;
			rectangle.height = 8;

			/* Checks the pattern fill result. */
			if (!backend->display->pattern_fill(
				    backend->display->context, &rectangle,
				    foreground,
				    font_pattern(font, width / 8U, column,
						 band * 8U)))

				/* Reports successful completion. */
				return 0;
		}
	}

	/* Reports operation failure. */
	return 1;
}
