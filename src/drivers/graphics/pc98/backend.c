/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "drivers/graphics/pc98/backend.h"
#include "drivers/graphics/pc98.h"
#include "hal/i386/bsp-pc98/display.h"

#include "drivers/graphics/pc98/display-auto.h"
#include <hal/hal.h>
#include "drivers/graphics/pc98/display.h"
#include <string.h>

#define CIRRUS_PADDR 0xf0000000U

static struct hal_pmem gdc_memory[4];
static struct hal_pmem cirrus_memory;

static struct pc98_auto display;
static struct pc98_display_backend backend_hal;
static struct pc98_display_ops native_display;
static int backend_prepared;

static uint8_t port_in8(void *context, uint16_t port)
{
	uint8_t value;
	(void)context;
	__asm__ volatile ("inb %w1,%0" : "=a"(value) : "Nd"(port));
	return value;
}

static void port_out8(void *context, uint16_t port, uint8_t value)
{
	(void)context;
	__asm__ volatile ("outb %0,%w1" : : "a"(value), "Nd"(port));
}

static int display_reset(void *context)
{
	(void)context;
	return pc98_display_graphics_start();
}

static int display_stop(void *context)
{
	(void)context;
	return pc98_display_graphics_stop();
}

static int pc98_graphics_prepare_hardware(void)
{
	static const hal_physaddr_t plane_address[4] = {
		0x000a8000U, 0x000b0000U, 0x000b8000U, 0x000e0000U
	};
	struct hal_pmem_request request;
	unsigned i;

	memset(&request, 0, sizeof(request));
	request.size = 0x8000U;
	request.alignment = 0x1000U;
	request.type = HAL_PMEM_TYPE_VRAM;
	request.attr = HAL_PMEM_ATTR_NOCACHE;
	for (i = 0; i < 4; i++) {
		request.paddr = plane_address[i];
		if (hal_pmem_alloc(&request, &gdc_memory[i]) != HAL_OK)
			goto fail;
	}
	request.paddr = CIRRUS_PADDR;
	request.size = 4U * 1024U * 1024U;
	if (hal_pmem_alloc(&request, &cirrus_memory) != HAL_OK)
		goto fail;
	pc98_auto_default(&display, display_reset, display_stop, NULL,
		port_in8, port_out8, NULL,
		(volatile uint8_t *)cirrus_memory.vaddr);
	/* Kernel code may run while a user CR3 is active. */
	for (i = 0; i < 4; i++)
		display.gdc.planes[i] =
		    (volatile uint8_t *)gdc_memory[i].vaddr;
	if (!pc98_auto_make_hal(&backend_hal, &display))
		goto fail;
	native_display = backend_hal.display;
	return 1;

fail:
	if (cirrus_memory.size != 0)
		(void)hal_pmem_free(&cirrus_memory);
	while (i != 0) {
		i--;
		if (gdc_memory[i].size != 0)
			(void)hal_pmem_free(&gdc_memory[i]);
	}
	return 0;
}

size_t
pc98_graphics_backend_get_modes(struct graphics_mode_info *modes,
	size_t capacity)
{
	static const struct graphics_mode_info available[] = {
		{ 640U, 480U, 24U, 640U * 3U },
		{ 640U, 480U, 8U, 640U },
		{ 640U, 400U, 4U, 80U },
	};
	size_t i;

	if (modes != NULL)
		for (i = 0; i < capacity && i < sizeof(available) / sizeof(available[0]); i++)
			modes[i] = available[i];
	return sizeof(available) / sizeof(available[0]);
}

int
pc98_graphics_backend_enter(struct graphics_mode *mode)
{
	struct pc98_display_info info;
	if (mode == NULL || native_display.enter == NULL)
		return 0;
	memset(&info, 0, sizeof(info));
	info.preferred_bits_per_pixel = mode->preferred_bits_per_pixel;
	hal_printf("graphics: enter request: preferred %u bpp\n",
	    mode->preferred_bits_per_pixel);
	if (!native_display.enter(native_display.context, &info)) {
		hal_printf("graphics: Cirrus and GDC mode entry failed\n");
		return 0;
	}
	mode->width = info.width;
	mode->height = info.height;
	mode->bits_per_pixel = info.bits_per_pixel;
	mode->stride = info.stride;
	hal_printf("graphics: %s mode %ux%ux%u stride=%u\n",
	    display.active == &display.cirrus_hal.display ? "Cirrus" : "GDC",
	    info.width, info.height, info.bits_per_pixel, info.stride);
	return 1;
}

void
pc98_graphics_backend_leave(void)
{
	if (native_display.leave != NULL)
		native_display.leave(native_display.context);
}

int
pc98_graphics_backend_fill(const struct graphics_rect *rect, uint32_t color)
{
	struct pc98_display_rect native;
	if (rect == NULL || native_display.fill == NULL)
		return 0;
	native.x = rect->x; native.y = rect->y;
	native.width = rect->width; native.height = rect->height;
	return native_display.fill(native_display.context, &native, color);
}

int
pc98_graphics_backend_line(unsigned x0, unsigned y0, unsigned x1,
	unsigned y1, uint32_t color)
{
	return native_display.line != NULL && native_display.line(
		native_display.context, x0, y0, x1, y1, color);
}

int
pc98_graphics_backend_pattern_fill(const struct graphics_rect *rect,
	uint32_t color, uint64_t pattern)
{
	struct pc98_display_rect native;
	if (rect == NULL || native_display.pattern_fill == NULL)
		return 0;
	native.x = rect->x; native.y = rect->y;
	native.width = rect->width; native.height = rect->height;
	return native_display.pattern_fill(native_display.context, &native,
		color, pattern);
}

int
pc98_graphics_backend_blit(unsigned x, unsigned y,
	const struct pc98_graphics_image *image, uint64_t pattern, int patterned)
{
	struct pc98_display_image native;
	unsigned i;
	if (image == NULL || image->palette_size > 256U)
		return 0;
	memset(&native, 0, sizeof(native));
	native.format = image->format == 1U ? PC98_DISPLAY_IMAGE_INDEX8 :
		PC98_DISPLAY_IMAGE_RGB24;
	native.width = image->width;
	native.height = image->height;
	native.stride = image->stride;
	native.pixels = image->pixels;
	native.palette_size = image->palette_size;
	for (i = 0; i < image->palette_size; i++)
		native.palette[i] = image->palette[i];
	if (patterned)
		return native_display.draw_image_pattern != NULL &&
			native_display.draw_image_pattern(native_display.context,
				x, y, &native, pattern);
	return native_display.draw_image != NULL && native_display.draw_image(
		native_display.context, x, y, &native);
}

int
pc98_graphics_backend_flush(const struct graphics_rect *rectangles,
	size_t count)
{
	struct pc98_display_rect native[32];
	size_t i;
	if (count > 32U || native_display.flush == NULL)
		return 0;
	for (i = 0; i < count; i++) {
		native[i].x = rectangles[i].x;
		native[i].y = rectangles[i].y;
		native[i].width = rectangles[i].width;
		native[i].height = rectangles[i].height;
	}
	return native_display.flush(native_display.context,
		count == 0 ? NULL : native, count);
}

int
pc98_graphics_backend_get_glyph(uint32_t codepoint, uint8_t font[32],
	unsigned *width, unsigned *height)
{
	return pc98_glyph_get_bitmap(&display.glyph, codepoint, font, width,
	    height);
}


int
pc98_graphics_prepare(void)
{
	backend_prepared = 0;
	if (!pc98_graphics_prepare_hardware())
		return 0;
	backend_prepared = 1;
	return 1;
}

int
pc98_graphics_backend_ready(void)
{
	return backend_prepared;
}
