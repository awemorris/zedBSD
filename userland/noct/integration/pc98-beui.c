/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <userland/noct/integration/pc98-beui.h>
#include <userland/noct/integration/platform.h>
#include "hal/i386/bsp-pc98/display.h"
#include "kern/platform.h"
#include <hal/hal.h>
#include "beui-pc98-auto.h"
#include <string.h>

#define CIRRUS_APERTURE 0xf0000000U

static struct noct_beui_pc98_auto display;
static struct noct_beui_hal hal;
static struct noct_beui_display_hal native_display;

static int proxy_enter(void *context, struct noct_beui_display_info *info)
{
	int ok;
	(void)context;
	fb_set_active(1);
	ok = native_display.enter(native_display.context, info);
	if (!ok)
		fb_set_active(0);
	return ok;
}

static void proxy_leave(void *context)
{
	(void)context;
	native_display.leave(native_display.context);
	fb_set_active(0);
}

static int proxy_poll(void *context)
{
	(void)context;
	return native_display.poll_events == NULL ? 1 :
		native_display.poll_events(native_display.context);
}

static int proxy_fill(void *context, const struct noct_beui_rect *rect,
		      uint32_t color)
{
	(void)context;
	return native_display.fill(native_display.context, rect, color);
}

static int proxy_line(void *context, unsigned x0, unsigned y0, unsigned x1,
		      unsigned y1, uint32_t color)
{
	(void)context;
	return native_display.line(native_display.context, x0, y0, x1, y1, color);
}

static int proxy_pattern_fill(void *context,
			      const struct noct_beui_rect *rect,
			      uint32_t color, uint64_t pattern)
{
	(void)context;
	return native_display.pattern_fill(native_display.context, rect, color,
					   pattern);
}

static int proxy_draw_image(void *context, unsigned x, unsigned y,
			    const struct noct_beui_image *image)
{
	(void)context;
	return native_display.draw_image(native_display.context, x, y, image);
}

static int proxy_draw_image_pattern(void *context, unsigned x, unsigned y,
				    const struct noct_beui_image *image,
				    uint64_t pattern)
{
	(void)context;
	return native_display.draw_image_pattern(native_display.context, x, y,
						 image, pattern);
}

static int proxy_flush(void *context,
		       const struct noct_beui_rect *rectangles, size_t count)
{
	(void)context;
	return native_display.flush(native_display.context, rectangles, count);
}

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
	return zedbsd_pc98_display_graphics_start();
}

static int display_stop(void *context)
{
	(void)context;
	return zedbsd_pc98_display_graphics_stop();
}

int
zedbsd_pc98_beui_init(uint64_t (*milliseconds)(void *),
		     int (*key_state)(void *, int), void (*drain)(void *))
{
	noct_beui_pc98_auto_default(&display, display_reset, display_stop, NULL,
				     port_in8, port_out8, NULL,
				     (volatile uint8_t *)CIRRUS_APERTURE);
	/* Kernel code keeps running under a user CR3.  Use the supervisor-only
	 * higher-half aliases, not the low identity window, for GDC VRAM. */
	display.gdc.planes[0] = (volatile uint8_t *)0x800a8000U;
	display.gdc.planes[1] = (volatile uint8_t *)0x800b0000U;
	display.gdc.planes[2] = (volatile uint8_t *)0x800b8000U;
	display.gdc.planes[3] = (volatile uint8_t *)0x800e0000U;
	if (!noct_beui_pc98_auto_make_hal(&hal, &display))
		return 0;
	native_display = hal.display;
	hal.display.context = NULL;
	hal.display.enter = proxy_enter;
	hal.display.leave = proxy_leave;
	hal.display.poll_events = proxy_poll;
	hal.display.fill = proxy_fill;
	hal.display.line = proxy_line;
	hal.display.pattern_fill = proxy_pattern_fill;
	hal.display.draw_image = proxy_draw_image;
	hal.display.draw_image_pattern = proxy_draw_image_pattern;
	hal.display.flush = proxy_flush;
	hal.clock.context = NULL;
	hal.clock.milliseconds = milliseconds;
	hal.input.context = NULL;
	hal.input.is_key_down = key_state;
	hal.input.drain = drain;
	zedbsd_noct_set_beui_hal(&hal);
	return 1;
}

int
zedbsd_pc98_beui_clear_graphics(void)
{
	return noct_beui_pc98_gdc_clear_graphics(&display.gdc);
}

int
kern_platform_graphics_enter(struct kern_graphics_mode *mode)
{
	struct noct_beui_display_info info;

	if (mode == NULL || native_display.enter == NULL)
		return 0;
	memset(&info, 0, sizeof(info));
	info.preferred_bits_per_pixel = mode->preferred_bits_per_pixel;
	if (!native_display.enter(native_display.context, &info))
		return 0;
	mode->width = info.width;
	mode->height = info.height;
	mode->bits_per_pixel = info.bits_per_pixel;
	mode->stride = info.stride;
	return 1;
}

void
kern_platform_graphics_leave(void)
{
	if (native_display.leave != NULL)
		native_display.leave(native_display.context);
}

int
kern_platform_graphics_fill(const struct kern_graphics_rect *rect,
			    uint32_t color)
{
	struct noct_beui_rect native;
	if (rect == NULL || native_display.fill == NULL)
		return 0;
	native.x = rect->x; native.y = rect->y;
	native.width = rect->width; native.height = rect->height;
	return native_display.fill(native_display.context, &native, color);
}

int
kern_platform_graphics_line(unsigned x0, unsigned y0, unsigned x1,
			    unsigned y1, uint32_t color)
{
	return native_display.line != NULL &&
		native_display.line(native_display.context, x0, y0, x1, y1, color);
}

int
kern_platform_graphics_pattern_fill(const struct kern_graphics_rect *rect,
				    uint32_t color, uint64_t pattern)
{
	struct noct_beui_rect native;
	if (rect == NULL || native_display.pattern_fill == NULL)
		return 0;
	native.x = rect->x; native.y = rect->y;
	native.width = rect->width; native.height = rect->height;
	return native_display.pattern_fill(native_display.context, &native, color,
		pattern);
}

int
kern_platform_graphics_blit(unsigned x, unsigned y,
			    const struct kern_graphics_image *image,
			    uint64_t pattern, int patterned)
{
	struct noct_beui_image native;
	unsigned i;
	if (image == NULL || image->palette_size > 256U)
		return 0;
	memset(&native, 0, sizeof(native));
	native.format = image->format == 1U ? NOCT_BEUI_IMAGE_INDEX8 :
		NOCT_BEUI_IMAGE_RGB24;
	native.width = image->width;
	native.height = image->height;
	native.stride = image->stride;
	native.pixels = image->pixels;
	native.palette_size = image->palette_size;
	for (i = 0; i < image->palette_size; i++)
		native.palette[i] = image->palette[i];
	if (patterned)
		return native_display.draw_image_pattern != NULL &&
			native_display.draw_image_pattern(native_display.context, x, y,
				&native, pattern);
	return native_display.draw_image != NULL &&
		native_display.draw_image(native_display.context, x, y, &native);
}

int
kern_platform_graphics_flush(const struct kern_graphics_rect *rectangles,
			     size_t count)
{
	struct noct_beui_rect native[32];
	size_t i;
	if (count > 32U || native_display.flush == NULL)
		return 0;
	for (i = 0; i < count; i++) {
		native[i].x = rectangles[i].x; native[i].y = rectangles[i].y;
		native[i].width = rectangles[i].width;
		native[i].height = rectangles[i].height;
	}
	return native_display.flush(native_display.context,
		count == 0 ? NULL : native, count);
}
