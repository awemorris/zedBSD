/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/graphics-device.h"
#include "kern/cdev.h"
#include "kern/file.h"
#include "kern/pc98/font.h"
#include "kern/platform.h"
#include "kern/uaccess.h"

#include <boots/graphics.h>
#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#define GRAPHICS_CAPABILITIES (BOOTS_GRAPHICS_CAP_FILL | \
	BOOTS_GRAPHICS_CAP_LINE | BOOTS_GRAPHICS_CAP_PATTERN | \
	BOOTS_GRAPHICS_CAP_BLIT_INDEX8 | BOOTS_GRAPHICS_CAP_BLIT_RGB24 | \
	BOOTS_GRAPHICS_CAP_BLIT_MONO1 | BOOTS_GRAPHICS_CAP_FLUSH | \
	BOOTS_GRAPHICS_CAP_GLYPH)
#define GRAPHICS_MAX_RECTS 32U
#define GRAPHICS_ROW_MAX 4096U

static struct file *graphics_owner __attribute__((section(".vfs_bss")));
static int graphics_entered __attribute__((section(".vfs_bss")));
static struct kern_graphics_mode graphics_mode __attribute__((section(".vfs_bss")));
static uint8_t row_buffer[GRAPHICS_ROW_MAX] __attribute__((section(".vfs_bss")));
static uint32_t palette_buffer[256] __attribute__((section(".vfs_bss")));

static int graphics_open(struct file *file)
{
	bool enabled = hal_irq_disable();
	if (graphics_owner != NULL) {
		if (enabled) hal_irq_enable();
		return EBUSY;
	}
	graphics_owner = file;
	graphics_entered = 0;
	memset(&graphics_mode, 0, sizeof(graphics_mode));
	if (enabled) hal_irq_enable();
	return 0;
}

static int graphics_close(struct file *file)
{
	if (graphics_owner != file)
		return 0;
	if (graphics_entered) {
		kern_platform_graphics_leave();
		fb_set_active(0);
		graphics_entered = 0;
	}
	graphics_owner = NULL;
	return 0;
}

static int require_entered(struct file *file)
{
	return file == graphics_owner && graphics_entered ? 0 : ENXIO;
}

static int valid_rect(const struct boots_graphics_rect *rect)
{
	return rect->width != 0 && rect->height != 0 &&
		rect->x <= graphics_mode.width && rect->y <= graphics_mode.height &&
		rect->width <= graphics_mode.width - rect->x &&
		rect->height <= graphics_mode.height - rect->y;
}

static void convert_rect(struct kern_graphics_rect *to,
			 const struct boots_graphics_rect *from)
{
	to->x = from->x; to->y = from->y;
	to->width = from->width; to->height = from->height;
}

static int graphics_enter(uintptr_t argument)
{
	struct boots_graphics_mode request;
	int error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;
	if (graphics_entered || request.reserved[0] != 0 ||
	    request.reserved[1] != 0 ||
	    (request.preferred_bits_per_pixel != 0 &&
	     request.preferred_bits_per_pixel != 8 &&
	     request.preferred_bits_per_pixel != 24))
		return EINVAL;
	memset(&graphics_mode, 0, sizeof(graphics_mode));
	graphics_mode.preferred_bits_per_pixel = request.preferred_bits_per_pixel;
	if (!kern_platform_graphics_enter(&graphics_mode))
		return ENODEV;
	graphics_entered = 1;
	fb_set_active(1);
	request.width = graphics_mode.width;
	request.height = graphics_mode.height;
	request.bits_per_pixel = graphics_mode.bits_per_pixel;
	request.stride = graphics_mode.stride;
	request.capabilities = GRAPHICS_CAPABILITIES;
	error = copyout(&request, argument, sizeof(request));
	if (error != 0) {
		kern_platform_graphics_leave();
		fb_set_active(0);
		graphics_entered = 0;
	}
	return error;
}

static int graphics_fill(uintptr_t argument, int patterned)
{
	struct kern_graphics_rect native;
	if (patterned) {
		struct boots_graphics_pattern_fill request;
		int error = copyin(argument, &request, sizeof(request));
		if (error != 0) return error;
		if (request.reserved != 0 || !valid_rect(&request.rect)) return EINVAL;
		convert_rect(&native, &request.rect);
		return kern_platform_graphics_pattern_fill(&native, request.color,
			request.pattern) ? 0 : EIO;
	} else {
		struct boots_graphics_fill request;
		int error = copyin(argument, &request, sizeof(request));
		if (error != 0) return error;
		if (request.reserved != 0 || !valid_rect(&request.rect)) return EINVAL;
		convert_rect(&native, &request.rect);
		return kern_platform_graphics_fill(&native, request.color) ? 0 : EIO;
	}
}

static int graphics_line(uintptr_t argument)
{
	struct boots_graphics_line request;
	int error = copyin(argument, &request, sizeof(request));
	if (error != 0) return error;
	if (request.reserved != 0 || request.x0 >= graphics_mode.width ||
	    request.x1 >= graphics_mode.width || request.y0 >= graphics_mode.height ||
	    request.y1 >= graphics_mode.height)
		return EINVAL;
	return kern_platform_graphics_line(request.x0, request.y0, request.x1,
		request.y1, request.color) ? 0 : EIO;
}

static int load_palette(const struct boots_graphics_blit *request)
{
	if (request->format == BOOTS_GRAPHICS_FORMAT_MONO1) {
		palette_buffer[0] = request->background;
		palette_buffer[1] = request->foreground;
		return 0;
	}
	if (request->format == BOOTS_GRAPHICS_FORMAT_RGB24)
		return request->palette == 0 && request->palette_count == 0 ? 0 : EINVAL;
	if (request->format != BOOTS_GRAPHICS_FORMAT_INDEX8 ||
	    request->palette == 0 || request->palette_count == 0 ||
	    request->palette_count > 256U)
		return EINVAL;
	return copyin(request->palette, palette_buffer,
		request->palette_count * sizeof(palette_buffer[0]));
}

static int graphics_blit(uintptr_t argument, int patterned)
{
	struct boots_graphics_blit request;
	struct kern_graphics_image image;
	uint64_t minimum_stride, source_offset;
	unsigned row;
	int error = copyin(argument, &request, sizeof(request));
	if (error != 0) return error;
	if (request.reserved != 0 || request.width == 0 || request.height == 0 ||
	    request.x > graphics_mode.width || request.y > graphics_mode.height ||
	    request.width > graphics_mode.width - request.x ||
	    request.height > graphics_mode.height - request.y || request.pixels == 0)
		return EINVAL;
	if (request.format == BOOTS_GRAPHICS_FORMAT_RGB24)
		minimum_stride = (uint64_t)request.width * 3U;
	else if (request.format == BOOTS_GRAPHICS_FORMAT_INDEX8)
		minimum_stride = request.width;
	else if (request.format == BOOTS_GRAPHICS_FORMAT_MONO1)
		minimum_stride = ((uint64_t)request.width + 7U) / 8U;
	else
		return EINVAL;
	if (minimum_stride > request.stride || minimum_stride > GRAPHICS_ROW_MAX ||
	    (uint64_t)request.stride * request.height > UINT32_MAX)
		return EINVAL;
	error = load_palette(&request);
	if (error != 0) return error;
	memset(&image, 0, sizeof(image));
	image.format = request.format == BOOTS_GRAPHICS_FORMAT_RGB24 ? 2U : 1U;
	image.width = request.width;
	image.height = 1;
	image.stride = request.format == BOOTS_GRAPHICS_FORMAT_RGB24 ?
		(size_t)request.width * 3U : request.width;
	image.pixels = row_buffer;
	image.palette = palette_buffer;
	image.palette_size = request.format == BOOTS_GRAPHICS_FORMAT_RGB24 ? 0U :
		request.format == BOOTS_GRAPHICS_FORMAT_MONO1 ? 2U : request.palette_count;
	for (row = 0; row < request.height; row++) {
		unsigned column;
		source_offset = (uint64_t)request.stride * row;
		if ((uint64_t)request.pixels + source_offset > UINT32_MAX)
			return EFAULT;
		if (request.format == BOOTS_GRAPHICS_FORMAT_MONO1) {
			uint8_t packed[128];
			if (minimum_stride > sizeof(packed)) return EINVAL;
			error = copyin(request.pixels + (uintptr_t)source_offset,
				packed, (size_t)minimum_stride);
			if (error != 0) return error;
			for (column = 0; column < request.width; column++)
				row_buffer[column] =
					(packed[column / 8U] >> (7U - column % 8U)) & 1U;
		} else {
			error = copyin(request.pixels + (uintptr_t)source_offset,
				row_buffer, (size_t)minimum_stride);
			if (error != 0) return error;
		}
		if (!kern_platform_graphics_blit(request.x, request.y + row, &image,
			request.pattern, patterned))
			return EIO;
	}
	return 0;
}

static int graphics_flush(uintptr_t argument)
{
	struct boots_graphics_flush request;
	struct boots_graphics_rect input[GRAPHICS_MAX_RECTS];
	struct kern_graphics_rect native[GRAPHICS_MAX_RECTS];
	unsigned i;
	int error = copyin(argument, &request, sizeof(request));
	if (error != 0) return error;
	if (request.rectangle_count > GRAPHICS_MAX_RECTS ||
	    (request.rectangle_count != 0 && request.rectangles == 0))
		return EINVAL;
	if (request.rectangle_count != 0) {
		error = copyin(request.rectangles, input,
			request.rectangle_count * sizeof(input[0]));
		if (error != 0) return error;
	}
	for (i = 0; i < request.rectangle_count; i++) {
		if (!valid_rect(&input[i])) return EINVAL;
		convert_rect(&native[i], &input[i]);
	}
	return kern_platform_graphics_flush(native, request.rectangle_count) ? 0 : EIO;
}

static int graphics_glyph(uintptr_t argument)
{
	struct boots_graphics_glyph request;
	uint8_t bitmap[32];
	unsigned width, height;
	int error = copyin(argument, &request, sizeof(request));
	if (error != 0) return error;
	if (request.reserved != 0 || request.bitmap == 0 ||
	    request.bitmap_capacity < sizeof(bitmap))
		return EINVAL;
	if (!pc98_font_get_glyph(request.codepoint, bitmap, &width, &height))
		return EINVAL;
	request.width = width; request.height = height;
	request.stride = width / 8U; request.bearing_x = 0;
	request.bearing_y = 0; request.advance = width;
	request.format = BOOTS_GRAPHICS_GLYPH_MSB1;
	request.bitmap_size = request.stride * height;
	error = copyout(bitmap, request.bitmap, request.bitmap_size);
	if (error == 0) error = copyout(&request, argument, sizeof(request));
	return error;
}

static int graphics_ioctl(struct file *file, unsigned long request,
			  uintptr_t argument)
{
	int error;
	if (file != graphics_owner)
		return EBADF;
	if (request == BOOTS_GRAPHICS_GET_CAPS) {
		const struct boots_graphics_caps caps = {
			GRAPHICS_CAPABILITIES, 640U, 480U, 0U
		};
		return copyout(&caps, argument, sizeof(caps));
	}
	if (request == BOOTS_GRAPHICS_ENTER)
		return graphics_enter(argument);
	error = require_entered(file);
	if (error != 0) return error;
	switch (request) {
	case BOOTS_GRAPHICS_GET_MODE: {
		const struct boots_graphics_mode mode = {
			graphics_mode.preferred_bits_per_pixel, graphics_mode.width,
			graphics_mode.height, graphics_mode.bits_per_pixel,
			graphics_mode.stride, GRAPHICS_CAPABILITIES, { 0, 0 }
		};
		return copyout(&mode, argument, sizeof(mode));
	}
	case BOOTS_GRAPHICS_FILL_RECT: return graphics_fill(argument, 0);
	case BOOTS_GRAPHICS_DRAW_LINE: return graphics_line(argument);
	case BOOTS_GRAPHICS_PATTERN_FILL: return graphics_fill(argument, 1);
	case BOOTS_GRAPHICS_BLIT: return graphics_blit(argument, 0);
	case BOOTS_GRAPHICS_BLIT_PATTERN: return graphics_blit(argument, 1);
	case BOOTS_GRAPHICS_FLUSH: return graphics_flush(argument);
	case BOOTS_GRAPHICS_GET_GLYPH: return graphics_glyph(argument);
	default: return EOPNOTSUPP;
	}
}

static const struct cdev_ops graphics_ops = {
	.open = graphics_open,
	.close = graphics_close,
	.ioctl = graphics_ioctl,
};

int graphics_device_register(void)
{
	graphics_owner = NULL;
	graphics_entered = 0;
	return cdev_register("graphics", 0x00010001U, &graphics_ops, NULL);
}
