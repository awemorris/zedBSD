/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/graphics-device.h"
#include "kern/graphics-driver.h"
#include "kern/cdev.h"
#include "kern/file.h"
#include "kern/lock.h"
#include "kern/uaccess.h"

#include <zedbsd/graphics.h>
#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#define GRAPHICS_CAPABILITIES (ZEDBSD_GRAPHICS_CAP_FILL | \
	ZEDBSD_GRAPHICS_CAP_LINE | ZEDBSD_GRAPHICS_CAP_PATTERN | \
	ZEDBSD_GRAPHICS_CAP_BLIT_INDEX8 | ZEDBSD_GRAPHICS_CAP_BLIT_RGB24 | \
	ZEDBSD_GRAPHICS_CAP_BLIT_MONO1 | ZEDBSD_GRAPHICS_CAP_FLUSH | \
	ZEDBSD_GRAPHICS_CAP_GLYPH)
#define GRAPHICS_MAX_RECTS 32U
#define GRAPHICS_ROW_MAX 4096U

static struct file *graphics_owner __attribute__((section(".vfs_bss")));
static int graphics_entered __attribute__((section(".vfs_bss")));
static const struct graphics_driver_ops *graphics_driver
	__attribute__((section(".vfs_bss")));
static void *graphics_driver_context __attribute__((section(".vfs_bss")));
static struct kern_graphics_mode graphics_mode __attribute__((section(".vfs_bss")));
static uint8_t row_buffer[GRAPHICS_ROW_MAX] __attribute__((section(".vfs_bss")));
static uint32_t palette_buffer[256] __attribute__((section(".vfs_bss")));
static struct mutex graphics_lock __attribute__((section(".vfs_bss")));
static int graphics_lock_ready __attribute__((section(".vfs_bss")));

/* Registration happens serially on the boot CPU.  Once this flag is
 * published, every open/close/ioctl operation uses the sleepable mutex so
 * driver callbacks and user-memory faults never run under a spinlock. */
static void
graphics_lock_init_once(void)
{
	if (!graphics_lock_ready) {
		(void)mutex_init(&graphics_lock,LOCK_RANK_DEVICE,"graphics device");
		graphics_lock_ready=1;
	}
}

static int graphics_open(struct file *file)
{
	int error=0;
	mutex_lock(&graphics_lock);
	if (graphics_driver == NULL) {
		error=ENODEV;
	} else if (graphics_owner != NULL) {
		error=EBUSY;
	} else {
		graphics_owner = file;
		graphics_entered = 0;
		memset(&graphics_mode, 0, sizeof(graphics_mode));
	}
	mutex_unlock(&graphics_lock);
	return error;
}

static int graphics_close(struct file *file)
{
	mutex_lock(&graphics_lock);
	if (graphics_owner == file) {
		if (graphics_entered) {
			graphics_driver->leave(graphics_driver_context);
			hal_cons_resume();
			graphics_entered = 0;
		}
		graphics_owner = NULL;
	}
	mutex_unlock(&graphics_lock);
	return 0;
}

static int require_entered(struct file *file)
{
	return file == graphics_owner && graphics_entered ? 0 : ENXIO;
}

static int valid_rect(const struct zedbsd_graphics_rect *rect)
{
	return rect->width != 0 && rect->height != 0 &&
		rect->x <= graphics_mode.width && rect->y <= graphics_mode.height &&
		rect->width <= graphics_mode.width - rect->x &&
		rect->height <= graphics_mode.height - rect->y;
}

static void convert_rect(struct kern_graphics_rect *to,
			 const struct zedbsd_graphics_rect *from)
{
	to->x = from->x; to->y = from->y;
	to->width = from->width; to->height = from->height;
}

static int graphics_enter(uintptr_t argument)
{
	struct zedbsd_graphics_mode request;
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
	hal_cons_suspend();
	if (!graphics_driver->enter(graphics_driver_context, &graphics_mode)) {
		graphics_driver->leave(graphics_driver_context);
		hal_cons_resume();
		return ENODEV;
	}
	graphics_entered = 1;
	request.width = graphics_mode.width;
	request.height = graphics_mode.height;
	request.bits_per_pixel = graphics_mode.bits_per_pixel;
	request.stride = graphics_mode.stride;
	request.capabilities = GRAPHICS_CAPABILITIES;
	error = copyout(&request, argument, sizeof(request));
	if (error != 0) {
		graphics_driver->leave(graphics_driver_context);
		hal_cons_resume();
		graphics_entered = 0;
	}
	return error;
}

static int graphics_fill(uintptr_t argument, int patterned)
{
	struct kern_graphics_rect native;
	if (patterned) {
		struct zedbsd_graphics_pattern_fill request;
		int error = copyin(argument, &request, sizeof(request));
		if (error != 0) return error;
		if (request.reserved != 0 || !valid_rect(&request.rect)) return EINVAL;
		convert_rect(&native, &request.rect);
		return graphics_driver->pattern_fill(graphics_driver_context, &native,
			request.color, request.pattern) ? 0 : EIO;
	} else {
		struct zedbsd_graphics_fill request;
		int error = copyin(argument, &request, sizeof(request));
		if (error != 0) return error;
		if (request.reserved != 0 || !valid_rect(&request.rect)) return EINVAL;
		convert_rect(&native, &request.rect);
		return graphics_driver->fill(graphics_driver_context, &native,
			request.color) ? 0 : EIO;
	}
}

static int graphics_line(uintptr_t argument)
{
	struct zedbsd_graphics_line request;
	int error = copyin(argument, &request, sizeof(request));
	if (error != 0) return error;
	if (request.reserved != 0 || request.x0 >= graphics_mode.width ||
	    request.x1 >= graphics_mode.width || request.y0 >= graphics_mode.height ||
	    request.y1 >= graphics_mode.height)
		return EINVAL;
	return graphics_driver->line(graphics_driver_context, request.x0,
		request.y0, request.x1, request.y1, request.color) ? 0 : EIO;
}

static int load_palette(const struct zedbsd_graphics_blit *request)
{
	if (request->format == ZEDBSD_GRAPHICS_FORMAT_MONO1) {
		palette_buffer[0] = request->background;
		palette_buffer[1] = request->foreground;
		return 0;
	}
	if (request->format == ZEDBSD_GRAPHICS_FORMAT_RGB24)
		return request->palette == 0 && request->palette_count == 0 ? 0 : EINVAL;
	if (request->format != ZEDBSD_GRAPHICS_FORMAT_INDEX8 ||
	    request->palette == 0 || request->palette_count == 0 ||
	    request->palette_count > 256U)
		return EINVAL;
	return copyin(request->palette, palette_buffer,
		request->palette_count * sizeof(palette_buffer[0]));
}

static int graphics_blit(uintptr_t argument, int patterned)
{
	struct zedbsd_graphics_blit request;
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
	if (request.format == ZEDBSD_GRAPHICS_FORMAT_RGB24)
		minimum_stride = (uint64_t)request.width * 3U;
	else if (request.format == ZEDBSD_GRAPHICS_FORMAT_INDEX8)
		minimum_stride = request.width;
	else if (request.format == ZEDBSD_GRAPHICS_FORMAT_MONO1)
		minimum_stride = ((uint64_t)request.width + 7U) / 8U;
	else
		return EINVAL;
	if (minimum_stride > request.stride || minimum_stride > GRAPHICS_ROW_MAX)
		return EINVAL;
	error = load_palette(&request);
	if (error != 0) return error;
	memset(&image, 0, sizeof(image));
	image.format = request.format == ZEDBSD_GRAPHICS_FORMAT_RGB24 ? 2U : 1U;
	image.width = request.width;
	image.height = 1;
	image.stride = request.format == ZEDBSD_GRAPHICS_FORMAT_RGB24 ?
		(size_t)request.width * 3U : request.width;
	image.pixels = row_buffer;
	image.palette = palette_buffer;
	image.palette_size = request.format == ZEDBSD_GRAPHICS_FORMAT_RGB24 ? 0U :
		request.format == ZEDBSD_GRAPHICS_FORMAT_MONO1 ? 2U : request.palette_count;
	for (row = 0; row < request.height; row++) {
		unsigned column;
		source_offset = (uint64_t)request.stride * row;
		if (source_offset > UINTPTR_MAX - (uintptr_t)request.pixels)
			return EFAULT;
		if (request.format == ZEDBSD_GRAPHICS_FORMAT_MONO1) {
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
		if (!graphics_driver->blit(graphics_driver_context, request.x,
			request.y + row, &image, request.pattern, patterned))
			return EIO;
	}
	return 0;
}

static int graphics_flush(uintptr_t argument)
{
	struct zedbsd_graphics_flush request;
	struct zedbsd_graphics_rect input[GRAPHICS_MAX_RECTS];
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
	return graphics_driver->flush(graphics_driver_context, native,
		request.rectangle_count) ? 0 : EIO;
}

static int graphics_glyph(uintptr_t argument)
{
	struct zedbsd_graphics_glyph request;
	uint8_t bitmap[32];
	unsigned width, height;
	int error = copyin(argument, &request, sizeof(request));
	if (error != 0) return error;
	if (request.reserved != 0 || request.bitmap == 0 ||
	    request.bitmap_capacity < sizeof(bitmap))
		return EINVAL;
	if (!graphics_driver->get_glyph(graphics_driver_context,
	    request.codepoint, bitmap, &width, &height))
		return EINVAL;
	request.width = width; request.height = height;
	request.stride = width / 8U; request.bearing_x = 0;
	request.bearing_y = 0; request.advance = width;
	request.format = ZEDBSD_GRAPHICS_GLYPH_MSB1;
	request.bitmap_size = request.stride * height;
	error = copyout(bitmap, request.bitmap, request.bitmap_size);
	if (error == 0) error = copyout(&request, argument, sizeof(request));
	return error;
}

static int graphics_ioctl_locked(struct file *file, unsigned long request,
			  uintptr_t argument)
{
	int error;
	if (file != graphics_owner)
		return EBADF;
	if (request == ZEDBSD_GRAPHICS_GET_CAPS) {
		const struct zedbsd_graphics_caps caps = {
			GRAPHICS_CAPABILITIES, 640U, 480U, 0U
		};
		return copyout(&caps, argument, sizeof(caps));
	}
	if (request == ZEDBSD_GRAPHICS_ENTER)
		return graphics_enter(argument);
	error = require_entered(file);
	if (error != 0) return error;
	switch (request) {
	case ZEDBSD_GRAPHICS_GET_MODE: {
		const struct zedbsd_graphics_mode mode = {
			graphics_mode.preferred_bits_per_pixel, graphics_mode.width,
			graphics_mode.height, graphics_mode.bits_per_pixel,
			graphics_mode.stride, GRAPHICS_CAPABILITIES, { 0, 0 }
		};
		return copyout(&mode, argument, sizeof(mode));
	}
	case ZEDBSD_GRAPHICS_FILL_RECT: return graphics_fill(argument, 0);
	case ZEDBSD_GRAPHICS_DRAW_LINE: return graphics_line(argument);
	case ZEDBSD_GRAPHICS_PATTERN_FILL: return graphics_fill(argument, 1);
	case ZEDBSD_GRAPHICS_BLIT: return graphics_blit(argument, 0);
	case ZEDBSD_GRAPHICS_BLIT_PATTERN: return graphics_blit(argument, 1);
	case ZEDBSD_GRAPHICS_FLUSH: return graphics_flush(argument);
	case ZEDBSD_GRAPHICS_GET_GLYPH: return graphics_glyph(argument);
	default: return EOPNOTSUPP;
	}
}

static int
graphics_ioctl(struct file *file,unsigned long request,uintptr_t argument)
{
	int error;
	mutex_lock(&graphics_lock);
	error=graphics_ioctl_locked(file,request,argument);
	mutex_unlock(&graphics_lock);
	return error;
}

static const struct cdev_ops graphics_ops = {
	.open = graphics_open,
	.close = graphics_close,
	.ioctl = graphics_ioctl,
};

int graphics_device_register(void)
{
	graphics_lock_init_once();
	mutex_lock(&graphics_lock);
	graphics_owner = NULL;
	graphics_entered = 0;
	mutex_unlock(&graphics_lock);
	return cdev_register("graphics", 0x00010001U, &graphics_ops, NULL);
}

int
graphics_driver_register(const struct graphics_driver_ops *ops, void *context)
{
	int error=0;

	if (ops == NULL || ops->enter == NULL || ops->clear == NULL ||
	    ops->leave == NULL || ops->fill == NULL || ops->line == NULL ||
	    ops->pattern_fill == NULL || ops->blit == NULL ||
	    ops->flush == NULL || ops->get_glyph == NULL)
		return EINVAL;
	graphics_lock_init_once();
	mutex_lock(&graphics_lock);
	if (graphics_driver != NULL) {
		error=EBUSY;
	} else {
		graphics_driver = ops;
		graphics_driver_context = context;
	}
	mutex_unlock(&graphics_lock);
	return error;
}

void
graphics_device_restore_text(void)
{
	if (!graphics_lock_ready) {
		hal_cons_resume();
		return;
	}
	mutex_lock(&graphics_lock);
	if (graphics_entered && graphics_driver != NULL) {
		graphics_driver->leave(graphics_driver_context);
		graphics_entered = 0;
	}
	hal_cons_resume();
	mutex_unlock(&graphics_lock);
}
