/* -*- mode: c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "drivers/platform/pc98/graphics/backend.h"
#include "drivers/graphics/pc98.h"
#include "hal/i386/bsp-pc98/display.h"

#include "drivers/platform/pc98/graphics/display-auto.h"
#include <hal/hal.h>
#include "drivers/platform/pc98/graphics/display.h"
#include <string.h>

#define CIRRUS_PADDR 0xf0000000U

static struct hal_pmem gdc_memory[4];
static struct hal_pmem cirrus_memory;

static struct pc98_auto display;
static struct pc98_display_backend backend_hal;
static struct pc98_display_ops native_display;
static int backend_prepared;

static int pc98_graphics_prepare_hardware(void);
static uint8_t port_in8(void *context, uint16_t port);
static void port_out8(void *context, uint16_t port, uint8_t value);
static int display_reset(void *context);
static int display_stop(void *context);

/*
 * Implements the drv pc98 graphics backend get modes operation.
 */
size_t
drv_pc98_graphics_backend_get_modes(
	struct graphics_mode_info *modes,
	size_t capacity)
{
	size_t function_result;
	static const struct graphics_mode_info available[] = {
		{640U, 480U, 24U, 640U * 3U},
		{640U, 480U, 8U, 640U},
		{640U, 400U, 4U, 80U},
	};
	size_t i;

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
 * Implements the drv pc98 graphics backend enter operation.
 */
int
drv_pc98_graphics_backend_enter(
	struct graphics_mode *mode)
{
	struct pc98_display_info info;

	/* Handles the mode availability. */
	if (mode == NULL || native_display.enter == NULL)
		return 0;
	memset(&info, 0, sizeof(info));
	info.preferred_bits_per_pixel = mode->preferred_bits_per_pixel;
	hal_printf("graphics: enter request: preferred %u bpp\n",
		   mode->preferred_bits_per_pixel);

	/* Checks the enter result. */
	if (!native_display.enter(native_display.context, &info)) {
		hal_printf("graphics: Cirrus and GDC mode entry failed\n");

		/* Reports successful completion. */
		return 0;
	}
	mode->width = info.width;
	mode->height = info.height;
	mode->bits_per_pixel = info.bits_per_pixel;
	mode->stride = info.stride;
	hal_printf("graphics: %s mode %ux%ux%u stride=%u\n",
		   display.active == &display.cirrus_hal.display ? "Cirrus"
								 : "GDC",
		   info.width, info.height, info.bits_per_pixel, info.stride);

	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the drv pc98 graphics backend leave operation.
 */
void
drv_pc98_graphics_backend_leave(
	void)
{
	/* Handles the leave availability. */
	if (native_display.leave != NULL)
		native_display.leave(native_display.context);
}

/*
 * Implements the drv pc98 graphics backend fill operation.
 */
int
drv_pc98_graphics_backend_fill(
	const struct graphics_rect *rect,
	uint32_t color)
{
	int function_result;
	struct pc98_display_rect native;

	/* Handles the rect availability. */
	if (rect == NULL || native_display.fill == NULL)
		return 0;
	native.x = rect->x;
	native.y = rect->y;
	native.width = rect->width;
	native.height = rect->height;

	/* Computes the function result. */
	function_result =
		native_display.fill(native_display.context, &native, color);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pc98 graphics backend line operation.
 */
int
drv_pc98_graphics_backend_line(
	unsigned x0,
	unsigned y0,
	unsigned x1,
	unsigned y1,
	uint32_t color)
{
	int function_result;

	/* Computes the function result. */
	function_result = native_display.line != NULL &&
			  native_display.line(native_display.context, x0, y0,
					      x1, y1, color);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pc98 graphics backend pattern fill operation.
 */
int
drv_pc98_graphics_backend_pattern_fill(
	const struct graphics_rect *rect,
	uint32_t color,
	uint64_t pattern)
{
	int function_result;
	struct pc98_display_rect native;

	/* Handles the rect availability. */
	if (rect == NULL || native_display.pattern_fill == NULL)
		return 0;
	native.x = rect->x;
	native.y = rect->y;
	native.width = rect->width;
	native.height = rect->height;

	/* Computes the function result. */
	function_result = native_display.pattern_fill(native_display.context,
						      &native, color, pattern);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pc98 graphics backend blit operation.
 */
int
drv_pc98_graphics_backend_blit(
	unsigned x,
	unsigned y,
	const struct pc98_graphics_image *image,
	uint64_t pattern,
	int patterned)
{
	int function_result;
	struct pc98_display_image native;
	unsigned i;

	/* Handles the image availability. */
	if (image == NULL || image->palette_size > 256U)
		return 0;
	memset(&native, 0, sizeof(native));
	native.format = image->format == 1U ? PC98_DISPLAY_IMAGE_INDEX8
					    : PC98_DISPLAY_IMAGE_RGB24;
	native.width = image->width;
	native.height = image->height;
	native.stride = image->stride;
	native.pixels = image->pixels;
	native.palette_size = image->palette_size;
	/* Process each remaining element. */
	for (i = 0; i < image->palette_size; i++)
		native.palette[i] = image->palette[i];

	/* Handles the patterned condition. */
	if (patterned) {
		/* Computes the function result. */
		function_result =
			native_display.draw_image_pattern != NULL &&
			native_display.draw_image_pattern(
				native_display.context, x, y, &native, pattern);

		/* Returns the computed result. */
		return function_result;
	}

	/* Computes the function result. */
	function_result = native_display.draw_image != NULL &&
			  native_display.draw_image(native_display.context, x,
						    y, &native);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pc98 graphics backend flush operation.
 */
int
drv_pc98_graphics_backend_flush(
	const struct graphics_rect *rectangles,
	size_t count)
{
	int function_result;
	struct pc98_display_rect native[32];
	size_t i;

	/* Handles the flush availability. */
	if (count > 32U || native_display.flush == NULL)
		return 0;
	/* Process each remaining element. */
	for (i = 0; i < count; i++) {
		native[i].x = rectangles[i].x;
		native[i].y = rectangles[i].y;
		native[i].width = rectangles[i].width;
		native[i].height = rectangles[i].height;
	}

	/* Computes the function result. */
	function_result = native_display.flush(
		native_display.context, count == 0 ? NULL : native, count);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pc98 graphics backend get glyph operation.
 */
int
drv_pc98_graphics_backend_get_glyph(
	uint32_t codepoint,
	uint8_t font[32],
	unsigned *width,
	unsigned *height)
{
	int function_result;

	/* Obtains the drv pc98 glyph get bitmap result. */
	function_result = drv_pc98_glyph_get_bitmap(&display.glyph, codepoint,
						    font, width, height);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Implements the drv pc98 graphics prepare operation.
 */
int
drv_pc98_graphics_prepare(
	void)
{
	backend_prepared = 0;

	/* Checks the pc98 graphics prepare hardware result. */
	if (!pc98_graphics_prepare_hardware())
		return 0;
	backend_prepared = 1;

	/* Reports operation failure. */
	return 1;
}

/*
 * Implements the drv pc98 graphics backend ready operation.
 */
int
drv_pc98_graphics_backend_ready(
	void)
{
	/* Returns the computed result. */
	return backend_prepared;
}

/* Supports the pc98 graphics prepare hardware operation. */
static int
pc98_graphics_prepare_hardware(
	void)
{
	static const hal_physaddr_t plane_address[4] = {
		0x000a8000U, 0x000b0000U, 0x000b8000U, 0x000e0000U};
	struct hal_pmem_request request;
	unsigned i;

	memset(&request, 0, sizeof(request));
	request.size = 0x8000U;
	request.alignment = 0x1000U;
	request.type = HAL_PMEM_TYPE_VRAM;
	request.attr = HAL_PMEM_ATTR_NOCACHE;
	/* Process each element required by the operation. */
	for (i = 0; i < 4; i++) {
		request.paddr = plane_address[i];

		/* Checks the hal pmem alloc result. */
		if (hal_pmem_alloc(&request, &gdc_memory[i]) != HAL_OK)
			goto fail;
	}
	request.paddr = CIRRUS_PADDR;
	request.size = 4U * 1024U * 1024U;

	/* Checks the hal pmem alloc result. */
	if (hal_pmem_alloc(&request, &cirrus_memory) != HAL_OK)
		goto fail;
	drv_pc98_auto_default(&display, display_reset, display_stop, NULL,
			      port_in8, port_out8, NULL,
			      (volatile uint8_t *)cirrus_memory.vaddr);

	/* Kernel code may run while a user CR3 is active. */
	for (i = 0; i < 4; i++)
		display.gdc.planes[i] = (volatile uint8_t *)gdc_memory[i].vaddr;

	/* Checks the drv pc98 auto make hal result. */
	if (!drv_pc98_auto_make_hal(&backend_hal, &display))
		goto fail;
	native_display = backend_hal.display;

	/* Reports operation failure. */
	return 1;

fail:

	/* Handles the cirrus memory condition. */
	if (cirrus_memory.size != 0)
		(void)hal_pmem_free(&cirrus_memory);
	/* Continue while the operation condition remains true. */
	while (i != 0) {
		i--;

		/* Handles the gdc memory condition. */
		if (gdc_memory[i].size != 0)
			(void)hal_pmem_free(&gdc_memory[i]);
	}

	/* Reports successful completion. */
	return 0;
}

/* Supports the port in8 operation. */
static uint8_t
port_in8(
	void *context,
	uint16_t port)
{
	uint8_t value;

	(void)context;
	__asm__ volatile("inb %w1,%0" : "=a"(value) : "Nd"(port));

	/* Returns the computed result. */
	return value;
}

/* Supports the port out8 operation. */
static void
port_out8(
	void *context,
	uint16_t port,
	uint8_t value)
{
	(void)context;
	__asm__ volatile("outb %0,%w1" : : "a"(value), "Nd"(port));
}

/* Supports the display reset operation. */
static int
display_reset(
	void *context)
{
	int function_result;

	(void)context;

	/* Obtains the pc98 display graphics start result. */
	function_result = pc98_display_graphics_start();

	/* Returns the computed result. */
	return function_result;
}

/* Supports the display stop operation. */
static int
display_stop(
	void *context)
{
	int function_result;

	(void)context;

	/* Obtains the pc98 display graphics stop result. */
	function_result = pc98_display_graphics_stop();

	/* Returns the computed result. */
	return function_result;
}
