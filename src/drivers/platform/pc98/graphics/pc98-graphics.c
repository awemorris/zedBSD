/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PC-98 graphics driver
 */

#include "kern/graphics-device.h"
#include "kern/text-display.h"
#include "kern/cdev.h"
#include "kern/file.h"
#include "kern/lock.h"
#include "kern/uaccess.h"
#include "drivers/platform/pc98/graphics/backend.h"

#include <uapi/graphics.h>
#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#define GRAPHICS_CAPABILITIES                                                  \
	(KERN_GRAPHICS_CAP_FILL | KERN_GRAPHICS_CAP_LINE |                 \
	 KERN_GRAPHICS_CAP_PATTERN | KERN_GRAPHICS_CAP_BLIT_INDEX8 |       \
	 KERN_GRAPHICS_CAP_BLIT_RGB24 | KERN_GRAPHICS_CAP_BLIT_MONO1 |     \
	 KERN_GRAPHICS_CAP_FLUSH | KERN_GRAPHICS_CAP_GLYPH)
#define GRAPHICS_MAX_RECTS 32U
#define GRAPHICS_ROW_MAX 4096U
#define GRAPHICS_MAX_MODES 16U

static struct file *graphics_owner __attribute__((section(".vfs_bss")));
static int graphics_entered __attribute__((section(".vfs_bss")));
static struct graphics_mode graphics_mode __attribute__((section(".vfs_bss")));
static uint8_t row_buffer[GRAPHICS_ROW_MAX] __attribute__((section(".vfs_bss")));
static uint32_t palette_buffer[256] __attribute__((section(".vfs_bss")));
static struct mutex graphics_lock __attribute__((section(".vfs_bss")));
static int graphics_lock_ready __attribute__((section(".vfs_bss")));

static void graphics_lock_init_once(void);
static int graphics_open(struct file *file);
static int graphics_close(struct file *file);
static int require_entered(struct file *file);
static int valid_rect(const struct graphics_rect *rect);
static int graphics_enter(uintptr_t argument);
static int graphics_get_modes(uintptr_t argument);
static int graphics_fill(uintptr_t argument, int patterned);
static int graphics_line(uintptr_t argument);
static int load_palette(const struct graphics_blit *request);
static int graphics_blit(uintptr_t argument, int patterned);
static int graphics_flush(uintptr_t argument);
static int graphics_glyph(uintptr_t argument);
static int graphics_ioctl_locked(struct file *file, unsigned long request, uintptr_t argument);
static int graphics_ioctl(struct file *file, unsigned long request, uintptr_t argument);

static const struct cdev_ops graphics_ops = {
	.open = graphics_open,
	.close = graphics_close,
	.ioctl = graphics_ioctl,
};

/*
 * Implements the drv graphics device register operation.
 */
int
drv_graphics_device_register(
	void)
{
	int error;

	graphics_lock_init_once();
	mutex_lock(&graphics_lock);

	graphics_owner = NULL;
	graphics_entered = 0;

	mutex_unlock(&graphics_lock);

	/* Obtains the cdev register result. */
	error =
		cdev_register("graphics", 0x00010001U, &graphics_ops, NULL);

	/* Returns the computed result. */
	return error;
}

/*
 * Implements the drv graphics device restore text operation.
 */
void
drv_graphics_device_restore_text(
	void)
{
	/* Handles the graphics lock ready condition. */
	if (!graphics_lock_ready) {
		kern_text_resume();

		/* Returns the computed result. */
		return;
	}

	mutex_lock(&graphics_lock);

	/* Handles the graphics entered condition. */
	if (graphics_entered) {
		drv_pc98_graphics_backend_leave();
		graphics_entered = 0;
	}

	kern_text_resume();

	mutex_unlock(&graphics_lock);
}

/* Registration happens serially on the boot CPU.  Once this flag is published, every open/close/ioctl operation uses the sleepable mutex so driver callbacks and user-memory faults never run under a spinlock. */
static void
graphics_lock_init_once(
	void)
{
	/* Handles the graphics lock ready condition. */
	if (!graphics_lock_ready) {
		(void)mutex_init(&graphics_lock, LOCK_RANK_DEVICE,
				 "graphics device");
		graphics_lock_ready = 1;
	}
}

/* Supports the graphics open operation. */
static int
graphics_open(
	struct file *file)
{
	int error = 0;

	mutex_lock(&graphics_lock);

	/* Checks the drv pc98 graphics backend ready result. */
	if (!drv_pc98_graphics_backend_ready()) {
		error = ENODEV;
	} else if (graphics_owner != NULL) {
		error = EBUSY;
	} else {
		graphics_owner = file;
		graphics_entered = 0;
		memset(&graphics_mode, 0, sizeof(graphics_mode));
	}

	mutex_unlock(&graphics_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the graphics close operation. */
static int
graphics_close(
	struct file *file)
{
	mutex_lock(&graphics_lock);

	/* Handles the graphics owner condition. */
	if (graphics_owner == file) {
		/* Handles the graphics entered condition. */
		if (graphics_entered) {
			drv_pc98_graphics_backend_leave();
			kern_text_resume();
			graphics_entered = 0;
		}

		graphics_owner = NULL;
	}

	mutex_unlock(&graphics_lock);

	/* Succeeded. */
	return 0;
}

/* Supports the require entered operation. */
static int
require_entered(
	struct file *file)
{
	/* Returns the computed result. */
	return file == graphics_owner && graphics_entered ? 0 : ENXIO;
}

/* Supports the valid rect operation. */
static int
valid_rect(
	const struct graphics_rect *rect)
{
	/* Returns the computed result. */
	return rect->width != 0 && rect->height != 0 &&
	       rect->x <= graphics_mode.width &&
	       rect->y <= graphics_mode.height &&
	       rect->width <= graphics_mode.width - rect->x &&
	       rect->height <= graphics_mode.height - rect->y;
}

/* Supports the graphics enter operation. */
static int
graphics_enter(
	uintptr_t argument)
{
	struct graphics_mode request;
	int error = copyin(argument, &request, sizeof(request));

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the graphics entered condition. */
	if (graphics_entered ||
	    ((request.preferred_width == 0) !=
	     (request.preferred_height == 0)) ||
	    request.preferred_width > 16384U ||
	    request.preferred_height > 16384U ||
	    (request.preferred_bits_per_pixel != 0 &&
	     request.preferred_bits_per_pixel != 4 &&
	     request.preferred_bits_per_pixel != 8 &&
	     request.preferred_bits_per_pixel != 24 &&
	     request.preferred_bits_per_pixel != 32)) {
		/* Failed. */
		return EINVAL;
	}
	memset(&graphics_mode, 0, sizeof(graphics_mode));
	graphics_mode.preferred_width = request.preferred_width;
	graphics_mode.preferred_height = request.preferred_height;
	graphics_mode.preferred_bits_per_pixel =
		request.preferred_bits_per_pixel;
	kern_text_suspend();

	/* Checks the drv pc98 graphics backend enter result. */
	if (!drv_pc98_graphics_backend_enter(&graphics_mode)) {
		drv_pc98_graphics_backend_leave();
		kern_text_resume();

		/* Failed. */
		return ENODEV;
	}

	graphics_entered = 1;
	request.width = graphics_mode.width;
	request.height = graphics_mode.height;
	request.bits_per_pixel = graphics_mode.bits_per_pixel;
	request.stride = graphics_mode.stride;
	request.capabilities = GRAPHICS_CAPABILITIES;

	/* Checks the operation status. */
	error = copyout(&request, argument, sizeof(request));
	if (error != 0) {
		drv_pc98_graphics_backend_leave();
		kern_text_resume();
		graphics_entered = 0;
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the graphics get modes operation. */
static int
graphics_get_modes(
	uintptr_t argument)
{
	int function_result;
	struct graphics_mode_list request;
	struct graphics_mode_info native[GRAPHICS_MAX_MODES];
	struct graphics_mode_info result;
	size_t total, returned, i;
	int error;

	/* Checks the operation status. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Handles the request condition. */
	if (request.reserved != 0 || request.capacity > GRAPHICS_MAX_MODES ||
	    (request.capacity != 0 && request.modes == 0)) {
		/* Failed. */
		return EINVAL;
	}
	total = drv_pc98_graphics_backend_get_modes(native, request.capacity);
	returned = total < request.capacity ? total : request.capacity;
	/* Process each element required by the operation. */
	for (i = 0; i < returned; i++) {
		result.width = native[i].width;
		result.height = native[i].height;
		result.bits_per_pixel = native[i].bits_per_pixel;
		result.stride = native[i].stride;

		/* Checks the operation status. */
		error = copyout(&result, request.modes + i * sizeof(result),
				sizeof(result));
		if (error != 0)
			return error;
	}

	request.count = (uint32_t)total;

	/* Obtains the copyout result. */
	function_result = copyout(&request, argument, sizeof(request));

	/* Returns the computed result. */
	return function_result;
}

/* Supports the graphics fill operation. */
static int
graphics_fill(
	uintptr_t argument,
	int patterned)
{
	int error;
	struct graphics_pattern_fill request_local;
	int error_local;
	struct graphics_fill request_local1;
	int error_local2;

	/* Handles the patterned condition. */
	if (patterned) {
		error_local =
			copyin(argument, &request_local, sizeof(request_local));

		/* Checks the operation status. */
		if (error_local != 0)
			return error_local;

		/* Checks the valid rect result. */
		if (request_local.reserved != 0 ||
		    !valid_rect(&request_local.rect))
			return EINVAL;

		/* Computes the function result. */
		error =
			drv_pc98_graphics_backend_pattern_fill(
				&request_local.rect, request_local.color,
				request_local.pattern)
				? 0
				: EIO;

		/* Failed. */
		return error;
	} else {
		/* Checks the operation status. */
		error_local2 = copyin(argument, &request_local1,
				      sizeof(request_local1));
		if (error_local2 != 0)
			return error_local2;

		/* Checks the valid rect result. */
		if (request_local1.reserved != 0 ||
		    !valid_rect(&request_local1.rect))
			return EINVAL;

		/* Computes the function result. */
		error =
			drv_pc98_graphics_backend_fill(&request_local1.rect,
						       request_local1.color)
				? 0
				: EIO;

		/* Failed. */
		return error;
	}
}

/* Supports the graphics line operation. */
static int
graphics_line(
	uintptr_t argument)
{
	int function_result;
	struct graphics_line request;
	int error = copyin(argument, &request, sizeof(request));

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the request condition. */
	if (request.reserved != 0 || request.x0 >= graphics_mode.width ||
	    request.x1 >= graphics_mode.width ||
	    request.y0 >= graphics_mode.height ||
	    request.y1 >= graphics_mode.height) {
		/* Failed. */
		return EINVAL;
	}

	/* Computes the function result. */
	function_result = drv_pc98_graphics_backend_line(request.x0, request.y0,
							 request.x1, request.y1,
							 request.color)
				  ? 0
				  : EIO;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the load palette operation. */
static int
load_palette(
	const struct graphics_blit *request)
{
	int error;

	/* Handles the request condition. */
	if (request->format == KERN_GRAPHICS_FORMAT_MONO1) {
		palette_buffer[0] = request->background;
		palette_buffer[1] = request->foreground;

		/* Succeeded. */
		return 0;
	}

	/* Handles the request condition. */
	if (request->format == KERN_GRAPHICS_FORMAT_RGB24) {
		return request->palette == 0 && request->palette_count == 0
			       ? 0
			       : EINVAL;
	}

	/* Handles the request condition. */
	if (request->format != KERN_GRAPHICS_FORMAT_INDEX8 ||
	    request->palette == 0 || request->palette_count == 0 ||
	    request->palette_count > 256U) {
		/* Failed. */
		return EINVAL;
	}

	/* Obtains the copyin result. */
	error =
		copyin(request->palette, palette_buffer,
		       request->palette_count * sizeof(palette_buffer[0]));

	/* Returns the computed result. */
	return error;
}

/* Supports the graphics blit operation. */
static int
graphics_blit(
	uintptr_t argument,
	int patterned)
{
	uint8_t packed[128];
	unsigned column;
	struct graphics_blit request;
	struct pc98_graphics_image image;
	uint64_t minimum_stride, source_offset;
	unsigned row;
	int error = copyin(argument, &request, sizeof(request));

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the request condition. */
	if (request.reserved != 0 || request.width == 0 ||
	    request.height == 0 || request.x > graphics_mode.width ||
	    request.y > graphics_mode.height ||
	    request.width > graphics_mode.width - request.x ||
	    request.height > graphics_mode.height - request.y ||
	    request.pixels == 0) {
		/* Failed. */
		return EINVAL;
	}

	/* Handles the request condition. */
	if (request.format == KERN_GRAPHICS_FORMAT_RGB24)
		minimum_stride = (uint64_t)request.width * 3U;
	else if (request.format == KERN_GRAPHICS_FORMAT_INDEX8)
		minimum_stride = request.width;
	else if (request.format == KERN_GRAPHICS_FORMAT_MONO1)
		minimum_stride = ((uint64_t)request.width + 7U) / 8U;
	else {
		/* Failed. */
		return EINVAL;
	}

	/* Handles the minimum stride condition. */
	if (minimum_stride > request.stride ||
	    minimum_stride > GRAPHICS_ROW_MAX) {
		/* Failed. */
		return EINVAL;
	}

	/* Checks the operation status. */
	error = load_palette(&request);
	if (error != 0)
		return error;
	memset(&image, 0, sizeof(image));
	image.format = request.format == KERN_GRAPHICS_FORMAT_RGB24 ? 2U : 1U;
	image.width = request.width;
	image.height = 1;
	image.stride = request.format == KERN_GRAPHICS_FORMAT_RGB24
			       ? (size_t)request.width * 3U
			       : request.width;
	image.pixels = row_buffer;
	image.palette = palette_buffer;
	image.palette_size = request.format == KERN_GRAPHICS_FORMAT_RGB24 ? 0U
			     : request.format == KERN_GRAPHICS_FORMAT_MONO1
				     ? 2U
				     : request.palette_count;
	/* Process each element required by the operation. */
	for (row = 0; row < request.height; row++) {
		/* Handles the source offset condition. */
		source_offset = (uint64_t)request.stride * row;
		if (source_offset > UINTPTR_MAX - (uintptr_t)request.pixels)
			return EFAULT;

		/* Handles the request condition. */
		if (request.format == KERN_GRAPHICS_FORMAT_MONO1) {
			/* Handles the minimum stride condition. */
			if (minimum_stride > sizeof(packed))
				return EINVAL;

			/* Checks the operation status. */
			error = copyin(request.pixels +
					       (uintptr_t)source_offset,
				       packed, (size_t)minimum_stride);
			if (error != 0)
				return error;
			/* Process each element required by the operation. */
			for (column = 0; column < request.width; column++) {
				row_buffer[column] = (packed[column / 8U] >>
						      (7U - column % 8U)) &
						     1U;
			}
		} else {
			/* Checks the operation status. */
			error = copyin(request.pixels +
					       (uintptr_t)source_offset,
				       row_buffer, (size_t)minimum_stride);
			if (error != 0)
				return error;
		}

		/* Checks the drv pc98 graphics backend blit result. */
		if (!drv_pc98_graphics_backend_blit(request.x, request.y + row,
						    &image, request.pattern,
						    patterned)) {
			/* Failed. */
			return EIO;
		}
	}

	/* Succeeded. */
	return 0;
}

/* Supports the graphics flush operation. */
static int
graphics_flush(
	uintptr_t argument)
{
	int function_result;
	struct graphics_flush request;
	struct graphics_rect input[GRAPHICS_MAX_RECTS];
	unsigned i;
	int error = copyin(argument, &request, sizeof(request));

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the request condition. */
	if (request.rectangle_count > GRAPHICS_MAX_RECTS ||
	    (request.rectangle_count != 0 && request.rectangles == 0)) {
		/* Failed. */
		return EINVAL;
	}

	/* Handles the request condition. */
	if (request.rectangle_count != 0) {
		/* Checks the operation status. */
		error = copyin(request.rectangles, input,
			       request.rectangle_count * sizeof(input[0]));
		if (error != 0)
			return error;
	}

	/* Process each remaining element. */
	for (i = 0; i < request.rectangle_count; i++) {
		/* Checks the valid rect result. */
		if (!valid_rect(&input[i]))
			return EINVAL;
	}

	/* Computes the function result. */
	function_result =
		drv_pc98_graphics_backend_flush(input, request.rectangle_count)
			? 0
			: EIO;

	/* Returns the computed result. */
	return function_result;
}

/* Supports the graphics glyph operation. */
static int
graphics_glyph(
	uintptr_t argument)
{
	struct graphics_glyph request;
	uint8_t bitmap[32];
	unsigned width, height;
	int error = copyin(argument, &request, sizeof(request));

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Handles the request condition. */
	if (request.reserved != 0 || request.bitmap == 0 ||
	    request.bitmap_capacity < sizeof(bitmap)) {
		/* Failed. */
		return EINVAL;
	}

	/* Checks the drv pc98 graphics backend get glyph result. */
	if (!drv_pc98_graphics_backend_get_glyph(request.codepoint, bitmap,
						 &width, &height)) {
		/* Failed. */
		return EINVAL;
	}
	request.width = width;
	request.height = height;
	request.stride = width / 8U;
	request.bearing_x = 0;
	request.bearing_y = 0;
	request.advance = width;
	request.format = KERN_GRAPHICS_GLYPH_MSB1;
	request.bitmap_size = request.stride * height;

	/* Checks the operation status. */
	error = copyout(bitmap, request.bitmap, request.bitmap_size);
	if (error == 0)
		error = copyout(&request, argument, sizeof(request));

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Supports the graphics ioctl locked operation. */
static int
graphics_ioctl_locked(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	struct graphics_mode mode;
	int function_result;
	struct graphics_mode_info modes[GRAPHICS_MAX_MODES];
	size_t count, i;
	int error;

	/* Handles the file condition. */
	if (file != graphics_owner)
		return EBADF;

	/* Handles the request condition. */
	if (request == KERN_GRAPHICS_GET_CAPS) {
		struct graphics_caps caps = {GRAPHICS_CAPABILITIES, 0, 0, 0};

		/* Checks the remaining item count. */
		count = drv_pc98_graphics_backend_get_modes(modes,
							    GRAPHICS_MAX_MODES);
		if (count > GRAPHICS_MAX_MODES)
			count = GRAPHICS_MAX_MODES;
		/* Process each remaining element. */
		for (i = 0; i < count; i++) {
			/* Handles the modes condition. */
			if (modes[i].width > caps.maximum_width)
				caps.maximum_width = modes[i].width;

			/* Handles the modes condition. */
			if (modes[i].height > caps.maximum_height)
				caps.maximum_height = modes[i].height;
		}

		/* Obtains the copyout result. */
		function_result = copyout(&caps, argument, sizeof(caps));

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the request condition. */
	if (request == KERN_GRAPHICS_GET_MODES) {
		/* Obtains the graphics get modes result. */
		function_result = graphics_get_modes(argument);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the request condition. */
	if (request == KERN_GRAPHICS_ENTER) {
		/* Obtains the graphics enter result. */
		function_result = graphics_enter(argument);

		/* Returns the computed result. */
		return function_result;
	}

	/* Checks the operation status. */
	error = require_entered(file);
	if (error != 0)
		return error;
	/* Dispatch the selected operation case. */
	switch (request) {
	case KERN_GRAPHICS_GET_MODE:
		mode.preferred_width = graphics_mode.preferred_width;
		mode.preferred_height = graphics_mode.preferred_height;
		mode.preferred_bits_per_pixel = graphics_mode.preferred_bits_per_pixel;
		mode.width = graphics_mode.width;
		mode.height = graphics_mode.height;
		mode.bits_per_pixel = graphics_mode.bits_per_pixel;
		mode.stride = graphics_mode.stride;
		mode.capabilities = GRAPHICS_CAPABILITIES;

		/* Obtains the copyout result. */
		function_result = copyout(&mode, argument, sizeof(mode));

		/* Returns the computed result. */
		return function_result;
	case KERN_GRAPHICS_FILL_RECT:
		/* Obtains the graphics fill result. */
		function_result = graphics_fill(argument, 0);

		/* Returns the computed result. */
		return function_result;
	case KERN_GRAPHICS_DRAW_LINE:
		/* Obtains the graphics line result. */
		function_result = graphics_line(argument);

		/* Returns the computed result. */
		return function_result;
	case KERN_GRAPHICS_PATTERN_FILL:
		/* Obtains the graphics fill result. */
		function_result = graphics_fill(argument, 1);

		/* Returns the computed result. */
		return function_result;
	case KERN_GRAPHICS_BLIT:
		/* Obtains the graphics blit result. */
		function_result = graphics_blit(argument, 0);

		/* Returns the computed result. */
		return function_result;
	case KERN_GRAPHICS_BLIT_PATTERN:
		/* Obtains the graphics blit result. */
		function_result = graphics_blit(argument, 1);

		/* Returns the computed result. */
		return function_result;
	case KERN_GRAPHICS_FLUSH:
		/* Obtains the graphics flush result. */
		function_result = graphics_flush(argument);

		/* Returns the computed result. */
		return function_result;
	case KERN_GRAPHICS_GET_GLYPH:
		/* Obtains the graphics glyph result. */
		function_result = graphics_glyph(argument);

		/* Returns the computed result. */
		return function_result;
	default:
		/* Failed. */
		return EOPNOTSUPP;
	}
}

/* Supports the graphics ioctl operation. */
static int
graphics_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	int error;

	mutex_lock(&graphics_lock);

	error = graphics_ioctl_locked(file, request, argument);

	mutex_unlock(&graphics_lock);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}
