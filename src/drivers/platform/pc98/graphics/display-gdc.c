/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (c) 1996-2024, Keiichi Tabata
 * Copyright (c) 2025, 2026, Awe Morris
 *
 * zedBSD graphics NEC PC-9800 GDC safe-mode display backend, imported from
 * Boots. Display sequencing is adapted from StratoHAL 98disp_gdc.c at commit
 * 76e909577bdf4629f11e473539b446a948fef830, altered to preserve text
 * VRAM and update only requested rectangles.  Port I/O is injected by
 * the embedder so the driver stays compiler neutral.
 */

#include "drivers/platform/pc98/graphics/display-gdc.h"

#include <string.h>

#define GDC_WIDTH 640U
#define GDC_HEIGHT 400U
#define GDC_STRIDE (GDC_WIDTH / 8U)

static void clear_planes(struct pc98_gdc *backend);
static int gdc_command(struct pc98_gdc *backend, uint8_t command);
static uint8_t rgb_to_gdc(uint32_t color);
static void gdc_write_pixel(struct pc98_gdc *backend, unsigned x, unsigned y, uint8_t color);
static int pattern_bit(uint64_t pattern, unsigned x, unsigned y);
static int gdc_enter(void *context, struct pc98_display_info *info);
static void gdc_leave(void *context);
static int gdc_fill(void *context, const struct pc98_display_rect *rect, uint32_t color);
static int gdc_line(void *context, unsigned x0, unsigned y0, unsigned x1, unsigned y1, uint32_t color);
static int gdc_pattern_fill(void *context, const struct pc98_display_rect *rect, uint32_t color, uint64_t pattern);
static int gdc_draw_image_common(void *context, unsigned destination_x, unsigned destination_y, const struct pc98_display_image *image, uint64_t pattern);
static int gdc_draw_image(void *context, unsigned destination_x, unsigned destination_y, const struct pc98_display_image *image);
static int gdc_draw_image_pattern(void *context, unsigned destination_x, unsigned destination_y, const struct pc98_display_image *image, uint64_t pattern);
static int gdc_flush(void *context, const struct pc98_display_rect *rectangles, size_t rectangle_count);

/*
 * Implements the drv pc98 gdc clear graphics operation.
 */
int
drv_pc98_gdc_clear_graphics(
	struct pc98_gdc *backend)
{
	/* Handles the backend availability. */
	if (backend == NULL || backend->port_out8 == NULL ||
	    backend->planes[0] == NULL || backend->planes[1] == NULL ||
	    backend->planes[2] == NULL || backend->planes[3] == NULL) {
		/* Succeeded. */
		return 0;
	}

	/*
	 * Match the real-mode loader's transition sequence.  In particular,
	 * disable GRCG/EGC interception before touching all four planar VRAM
	 * apertures; firmware and Cirrus may leave those controls non-default.
	 */
	backend->port_out8(backend->io_context, 0x7cU, 0x00U);
	backend->port_out8(backend->io_context, 0x5fU, 0x00U);
	backend->port_out8(backend->io_context, 0x6aU, 0x07U);
	backend->port_out8(backend->io_context, 0x6aU, 0x20U);
	backend->port_out8(backend->io_context, 0x6aU, 0x04U);
	backend->port_out8(backend->io_context, 0x6aU, 0x06U);
	backend->port_out8(backend->io_context, 0x6aU, 0x01U);
	backend->port_out8(backend->io_context, 0x5fU, 0x01U);
	clear_planes(backend);

	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the drv pc98 gdc default operation.
 */
void
drv_pc98_gdc_default(
	struct pc98_gdc *backend,
	pc98_display_reset_fn display_reset,
	pc98_display_reset_fn display_stop,
	void *bios_context,
	pc98_in8_fn port_in8,
	pc98_out8_fn port_out8,
	void *io_context)
{
	memset(backend, 0, sizeof(*backend));
	backend->bios_context = bios_context;
	backend->display_reset = display_reset;
	backend->display_stop = display_stop;
	backend->io_context = io_context;
	backend->port_in8 = port_in8;
	backend->port_out8 = port_out8;
	backend->planes[0] = (volatile uint8_t *)0x000a8000U;
	backend->planes[1] = (volatile uint8_t *)0x000b0000U;
	backend->planes[2] = (volatile uint8_t *)0x000b8000U;
	backend->planes[3] = (volatile uint8_t *)0x000e0000U;
}

/*
 * Implements the drv pc98 gdc make hal operation.
 */
int
drv_pc98_gdc_make_hal(
	struct pc98_display_backend *hal,
	struct pc98_gdc *backend)
{
	/* Handles the hal availability. */
	if (hal == NULL || backend == NULL)
		return 0;
	memset(hal, 0, sizeof(*hal));
	hal->display.context = backend;
	hal->display.enter = gdc_enter;
	hal->display.leave = gdc_leave;
	hal->display.fill = gdc_fill;
	hal->display.line = gdc_line;
	hal->display.pattern_fill = gdc_pattern_fill;
	hal->display.draw_image = gdc_draw_image;
	hal->display.draw_image_pattern = gdc_draw_image_pattern;
	hal->display.flush = gdc_flush;

	/* Reports operation failure. */
	return 1;
}

/* Supports the clear planes operation. */
static void
clear_planes(
	struct pc98_gdc *backend)
{
	unsigned plane;
	unsigned offset;

	/* Process each element required by the operation. */
	for (plane = 0; plane < 4; plane++) {
		/* Process each element required by the operation. */
		for (offset = 0; offset < PC98_DISPLAY_GDC_PLANE_BYTES;
		     offset++)
			backend->planes[plane][offset] = 0;
	}
}

/* Supports the gdc command operation. */
static int
gdc_command(
	struct pc98_gdc *backend,
	uint8_t command)
{
	unsigned timeout;

	/* Process each element required by the operation. */
	for (timeout = 100000U; timeout != 0; timeout--) {
		/* Checks the port in8 result. */
		if (!(backend->port_in8(backend->io_context, 0x60) & 0x02U))
			break;
	}

	/* Handles the timeout condition. */
	if (timeout == 0)
		return 0;
	backend->port_out8(backend->io_context, 0x62, command);

	/* Reports operation failure. */
	return 1;
}

/* Supports the rgb to gdc operation. */
static uint8_t
rgb_to_gdc(
	uint32_t color)
{
	unsigned red = (color >> 16) & 0xffU;
	unsigned green = (color >> 8) & 0xffU;
	unsigned blue = color & 0xffU;
	unsigned luminance = (red + (green << 1) + blue) >> 2;

	/*
	 * Keep the integer-only StratoHAL RGBI conversion model, but use a
	 * half-range threshold for each component and a green-weighted
	 * luminance for the intensity plane.  The B/R/G bit order is the
	 * native PC-98 GDC plane order.
	 */
	return (uint8_t)((blue > 127U ? 1U : 0U) | (red > 127U ? 2U : 0U) |
			 (green > 127U ? 4U : 0U) |
			 (luminance > 127U ? 8U : 0U));
}

/* Supports the gdc write pixel operation. */
static void
gdc_write_pixel(
	struct pc98_gdc *backend,
	unsigned x,
	unsigned y,
	uint8_t color)
{
	uint8_t old;
	unsigned offset = y * GDC_STRIDE + (x >> 3);
	uint8_t mask = (uint8_t)(0x80U >> (x & 7U));
	unsigned plane;

	/* Process each element required by the operation. */
	for (plane = 0; plane < 4; plane++) {
		old = backend->planes[plane][offset];

		backend->planes[plane][offset] =
			(uint8_t)((old & (uint8_t)~mask) |
				  (((color >> plane) & 1U) ? mask : 0U));
	}
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

/* Supports the gdc enter operation. */
static int
gdc_enter(
	void *context,
	struct pc98_display_info *info)
{
	struct pc98_gdc *backend = context;

	/* Handles the backend availability. */
	if (backend == NULL || info == NULL || backend->display_reset == NULL ||
	    backend->port_in8 == NULL || backend->port_out8 == NULL ||
	    backend->planes[0] == NULL || backend->planes[1] == NULL ||
	    backend->planes[2] == NULL || backend->planes[3] == NULL) {
		/* Succeeded. */
		return 0;
	}

	/*
	 * Clear every graphics plane before starting the slave GDC.  Otherwise
	 * firmware VRAM is briefly visible between GDC_START and this clear.
	 */
	if (!drv_pc98_gdc_clear_graphics(backend) ||
	    !backend->display_reset(backend->bios_context)) {
		/* Succeeded. */
		return 0;
	}

	/* Hide text only after the clean graphics display is running. */
	if (!gdc_command(backend, 0x0c)) {
		(void)gdc_command(backend, 0x0d);

		/* Succeeded. */
		return 0;
	}

	info->width = GDC_WIDTH;
	info->height = GDC_HEIGHT;
	info->bits_per_pixel = 4;
	info->stride = GDC_STRIDE;

	/* Reports operation failure. */
	return 1;
}

/* Supports the gdc leave operation. */
static void
gdc_leave(
	void *context)
{
	struct pc98_gdc *backend = context;

	/* Handles the display stop availability. */
	if (backend->display_stop != NULL)
		(void)backend->display_stop(backend->bios_context);
	clear_planes(backend);
	(void)gdc_command(backend, 0x0d);
}

/* Supports the gdc fill operation. */
static int
gdc_fill(
	void *context,
	const struct pc98_display_rect *rect,
	uint32_t color)
{
	uint8_t old;
	uint8_t mask;
	unsigned offset;
	struct pc98_gdc *backend = context;
	uint8_t gdc_color = rgb_to_gdc(color);
	unsigned first_byte = rect->x >> 3;
	unsigned last_pixel = rect->x + rect->width - 1U;
	unsigned last_byte = last_pixel >> 3;
	unsigned y;
	unsigned byte;
	unsigned plane;

	/* Process each element required by the operation. */
	for (y = rect->y; y < rect->y + rect->height; y++) {
		/* Process each element required by the operation. */
		for (byte = first_byte; byte <= last_byte; byte++) {
			mask = 0xffU;
			offset = y * GDC_STRIDE + byte;

			/* Classifies the current byte. */
			if (byte == first_byte)
				mask &= (uint8_t)(0xffU >> (rect->x & 7U));

			/* Classifies the current byte. */
			if (byte == last_byte) {
				mask &= (uint8_t)(0xffU
						  << (7U - (last_pixel & 7U)));
			}

			/* Process each element required by the operation. */
			for (plane = 0; plane < 4; plane++) {
				old = backend->planes[plane][offset];

				/* Sets this plane's bit of the pixel from the colour. */
				backend->planes[plane][offset] =
					(uint8_t)((old & (uint8_t)~mask) |
						  (((gdc_color >> plane) & 1U)
							   ? mask
							   : 0));
			}
		}
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the gdc line operation. */
static int
gdc_line(
	void *context,
	unsigned x0,
	unsigned y0,
	unsigned x1,
	unsigned y1,
	uint32_t color)
{
	int twice_error;
	struct pc98_gdc *backend = context;
	int x = (int)x0;
	int y = (int)y0;
	int target_x = (int)x1;
	int target_y = (int)y1;
	int delta_x = target_x >= x ? target_x - x : x - target_x;
	int step_x = x < target_x ? 1 : -1;
	int delta_y = target_y >= y ? y - target_y : target_y - y;
	int step_y = y < target_y ? 1 : -1;
	int error = delta_x + delta_y;
	uint8_t gdc_color = rgb_to_gdc(color);

	/* Continue until the operation reaches a terminal state. */
	for (;;) {
		gdc_write_pixel(backend, (unsigned)x, (unsigned)y, gdc_color);

		/* Checks the current horizontal value. */
		if (x == target_x && y == target_y)
			break;

		/* Checks the operation status. */
		twice_error = error * 2;
		if (twice_error >= delta_y) {
			error += delta_y;
			x += step_x;
		}

		/* Checks the operation status. */
		if (twice_error <= delta_x) {
			error += delta_x;
			y += step_y;
		}
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the gdc pattern fill operation. */
static int
gdc_pattern_fill(
	void *context,
	const struct pc98_display_rect *rect,
	uint32_t color,
	uint64_t pattern)
{
	unsigned x;
	struct pc98_gdc *backend = context;
	uint8_t gdc_color = rgb_to_gdc(color);
	unsigned y;

	/* Process each element required by the operation. */
	for (y = rect->y; y < rect->y + rect->height; y++) {
		/* Process each element required by the operation. */
		for (x = rect->x; x < rect->x + rect->width; x++) {
			/* Checks the pattern bit result. */
			if (pattern_bit(pattern, x - rect->x, y - rect->y))
				gdc_write_pixel(backend, x, y, gdc_color);
		}
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the gdc draw image common operation. */
static int
gdc_draw_image_common(
	void *context,
	unsigned destination_x,
	unsigned destination_y,
	const struct pc98_display_image *image,
	uint64_t pattern)
{
	unsigned index;
	const uint8_t *pixel;
	uint32_t rgb;
	const uint8_t *row;
	unsigned x;
	struct pc98_gdc *backend = context;
	unsigned y;

	/* Process each element required by the operation. */
	for (y = 0; y < image->height; y++) {
		row = image->pixels + (size_t)y * image->stride;

		/* Process each element required by the operation. */
		for (x = 0; x < image->width; x++) {
			/* Checks the pattern bit result. */
			if (!pattern_bit(pattern, x, y))
				continue;

			/* Handles the image condition. */
			if (image->format == PC98_DISPLAY_IMAGE_INDEX8) {
				index = row[x];

				rgb = index < image->palette_size
					      ? image->palette[index]
					      : 0;
			} else {
				pixel = row + (size_t)x * 3U;

				/* Packs the three source bytes into one pixel. */
				rgb = ((uint32_t)pixel[0] << 16) |
				      ((uint32_t)pixel[1] << 8) | pixel[2];
			}

			gdc_write_pixel(backend, destination_x + x,
					destination_y + y, rgb_to_gdc(rgb));
		}
	}

	/* Reports operation failure. */
	return 1;
}

/* Supports the gdc draw image operation. */
static int
gdc_draw_image(
	void *context,
	unsigned destination_x,
	unsigned destination_y,
	const struct pc98_display_image *image)
{
	int error;

	/* Obtains the gdc draw image common result. */
	error = gdc_draw_image_common(
		context, destination_x, destination_y, image, UINT64_MAX);

	/* Returns the computed result. */
	return error;
}

/* Supports the gdc draw image pattern operation. */
static int
gdc_draw_image_pattern(
	void *context,
	unsigned destination_x,
	unsigned destination_y,
	const struct pc98_display_image *image,
	uint64_t pattern)
{
	int error;

	/* Obtains the gdc draw image common result. */
	error = gdc_draw_image_common(context, destination_x,
						destination_y, image, pattern);

	/* Returns the computed result. */
	return error;
}

/* Supports the gdc flush operation. */
static int
gdc_flush(
	void *context,
	const struct pc98_display_rect *rectangles,
	size_t rectangle_count)
{
	/* GDC VRAM is updated directly; there is no backing-image copy. */
	(void)context;
	(void)rectangles;
	(void)rectangle_count;

	/* Reports operation failure. */
	return 1;
}
