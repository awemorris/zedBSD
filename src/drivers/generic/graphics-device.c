/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Generic /dev/graphics front end over a registered backend
 */

#include "kern/graphics-backend.h"
#include "kern/graphics-device.h"
#include "kern/text-display.h"
#include "kern/cdev.h"
#include "kern/file.h"
#include "kern/lock.h"
#include "kern/uaccess.h"
#include <kern/kcrt.h>

#include <uapi/graphics.h>
#include <uapi/errno.h>

#define GRAPHICS_CAPABILITIES                                                  \
	(KERN_GRAPHICS_CAP_FILL | KERN_GRAPHICS_CAP_LINE |                 \
	 KERN_GRAPHICS_CAP_PATTERN | KERN_GRAPHICS_CAP_BLIT_INDEX8 |       \
	 KERN_GRAPHICS_CAP_BLIT_RGB24 | KERN_GRAPHICS_CAP_BLIT_MONO1 |     \
	 KERN_GRAPHICS_CAP_FLUSH | KERN_GRAPHICS_CAP_GLYPH)
#define GRAPHICS_MAX_RECTS 32U
#define GRAPHICS_ROW_MAX 4096U
#define GRAPHICS_PACKED_MAX 512U
#define GRAPHICS_MAX_MODES 16U

/*
 * Pixels per chunk of an RGB24 row: 1360 x 3 bytes fit the row buffer,
 * and 1360 is a multiple of 8 so the pattern phase is easy to follow.
 */
#define GRAPHICS_CHUNK_RGB24 1360U

/* Pixels per chunk of an INDEX8 row: one byte each. */
#define GRAPHICS_CHUNK_INDEX8 4096U

/*
 * Pixels per chunk of a MONO1 row: 512 packed bytes hold 4096 bits, and
 * the expanded row takes one byte per pixel. A multiple of 8, so every
 * chunk starts on a whole source byte.
 */
#define GRAPHICS_CHUNK_MONO1 4096U

_Static_assert(GRAPHICS_CHUNK_RGB24 * 3U <= GRAPHICS_ROW_MAX,
	       "an RGB24 chunk must fit the row buffer");
_Static_assert(GRAPHICS_CHUNK_INDEX8 <= GRAPHICS_ROW_MAX,
	       "an INDEX8 chunk must fit the row buffer");
_Static_assert(GRAPHICS_CHUNK_MONO1 <= GRAPHICS_PACKED_MAX * 8U &&
		       GRAPHICS_CHUNK_MONO1 <= GRAPHICS_ROW_MAX,
	       "a MONO1 chunk must fit the packed and the row buffer");
_Static_assert(GRAPHICS_CHUNK_MONO1 % 8U == 0,
	       "a MONO1 chunk must start on a source byte");

/*
 * The best backend offered so far, and its priority.
 *
 * Boot registration runs serially on the boot CPU, so no lock guards
 * these. They are read once when /dev/graphics registers.
 */
static const struct drv_graphics_backend *graphics_candidate
	__attribute__((section(".vfs_bss")));
static int graphics_candidate_priority __attribute__((section(".vfs_bss")));

/*
 * The backend chosen at registration; NULL before it and when none came.
 *
 * Set once during boot and never changed afterwards.
 */
static const struct drv_graphics_backend *graphics_backend
	__attribute__((section(".vfs_bss")));

/* Nonzero once the choice is made; later offers are refused. */
static int graphics_chosen __attribute__((section(".vfs_bss")));

/* The single file that has the device open, or NULL. */
static struct file *graphics_owner __attribute__((section(".vfs_bss")));

/* Nonzero while the owner is in graphics mode. */
static int graphics_entered __attribute__((section(".vfs_bss")));

/* The mode the backend entered; valid while graphics_entered. */
static struct graphics_mode graphics_mode __attribute__((section(".vfs_bss")));

/* One chunk of a user row, copied in (and expanded for MONO1). */
static uint8_t row_buffer[GRAPHICS_ROW_MAX]
	__attribute__((section(".vfs_bss")));

/* The packed MONO1 source of one chunk, kept off the stack. */
static uint8_t packed_buffer[GRAPHICS_PACKED_MAX]
	__attribute__((section(".vfs_bss")));

/* The palette of the blit in progress. */
static uint32_t palette_buffer[256] __attribute__((section(".vfs_bss")));

/* Serializes open, close, and every ioctl; owned by the boot CPU's init. */
static struct mutex graphics_lock __attribute__((section(".vfs_bss")));

/* Nonzero once graphics_lock is initialized. */
static int graphics_lock_ready __attribute__((section(".vfs_bss")));

/*
 * Whether the console has left the display for good: every open and request
 * is refused from now on, and nothing gives the screen back to the text.
 * Written under graphics_lock, or before the lock exists (the boot, with no
 * open yet); not in .vfs_bss, since it may be set before the device is
 * registered.
 */
static int graphics_ended;

static void graphics_lock_init_once(void);
static int backend_end(const struct kern_text_range *ranges, unsigned count);
static int graphics_open(struct file *file);
static int graphics_close(struct file *file);
static int require_entered(struct file *file);
static int valid_rect(const struct graphics_rect *rect);
static int graphics_enter(uintptr_t argument);
static int graphics_get_modes(uintptr_t argument);
static int graphics_fill(uintptr_t argument);
static int graphics_pattern_fill(uintptr_t argument);
static int graphics_line(uintptr_t argument);
static int load_palette(const struct graphics_blit *request);
static unsigned chunk_pixels(unsigned format);
static int copy_chunk(const struct graphics_blit *request, uint64_t row_offset, unsigned first, unsigned count);
static int graphics_blit(uintptr_t argument, int patterned);
static int graphics_flush(uintptr_t argument);
static int graphics_glyph(uintptr_t argument);
static int graphics_get_caps(uintptr_t argument);
static int graphics_get_mode(uintptr_t argument);
static int graphics_ioctl_locked(struct file *file, unsigned long request, uintptr_t argument);
static int graphics_ioctl(struct file *file, unsigned long request, uintptr_t argument);

static const struct cdev_ops graphics_ops = {
	.open = graphics_open,
	.close = graphics_close,
	.ioctl = graphics_ioctl,
};

/*
 * Offers a backend for /dev/graphics.
 */
int
drv_graphics_backend_register(
	const struct drv_graphics_backend *backend,
	int priority)
{
	/* Refuses a table that cannot draw. */
	if (backend == NULL || backend->ready == NULL ||
	    backend->get_modes == NULL || backend->enter == NULL ||
	    backend->leave == NULL || backend->fill == NULL ||
	    backend->line == NULL || backend->pattern_fill == NULL ||
	    backend->blit_row == NULL || backend->flush == NULL ||
	    backend->get_glyph == NULL)
		return EINVAL;

	/* Refuses an offer after the choice, which happens only once. */
	if (graphics_chosen)
		return EBUSY;

	/* Keeps the earlier backend unless this one ranks higher. */
	if (graphics_candidate != NULL &&
	    priority <= graphics_candidate_priority)
		return 0;

	/* Records the new best backend. */
	graphics_candidate = backend;
	graphics_candidate_priority = priority;

	/* Succeeded. */
	return 0;
}

/*
 * Reports the backend /dev/graphics chose.
 */
const struct drv_graphics_backend *
drv_graphics_backend_selected(
	void)
{
	/* Returns the choice, NULL before it. */
	return graphics_backend;
}

/*
 * Chooses the backend and registers /dev/graphics.
 *
 * Without a backend the device is not created, so a board without a
 * display has no /dev/graphics.
 */
int
drv_graphics_device_register(
	void)
{
	int error;

	/* Fixes the choice; later offers get EBUSY. */
	graphics_chosen = 1;
	graphics_backend = graphics_candidate;

	/* Leaves the device out when nothing can draw. */
	if (graphics_backend == NULL)
		return 0;

	/* Starts with no owner. */
	graphics_lock_init_once();
	mutex_lock(&graphics_lock);

	graphics_owner = NULL;
	graphics_entered = 0;

	mutex_unlock(&graphics_lock);

	/* Creates the device node. */
	error = cdev_register("graphics", 0x00010001U, &graphics_ops, NULL);

	/* Returns the registration result. */
	return error;
}

/*
 * Leaves graphics mode and gives the screen back to the text console.
 */
void
drv_graphics_device_restore_text(
	void)
{
	/* Only resumes the text when the device never registered (an ended console draws nothing again). */
	if (!graphics_lock_ready) {
		if (!graphics_ended)
			kern_text_resume();
		return;
	}

	/* Leaves the backend's mode and resumes the text, unless the console has left the display. */
	mutex_lock(&graphics_lock);

	if (!graphics_ended) {
		/* The owner's mode goes before the text comes back. */
		if (graphics_entered) {
			graphics_backend->leave();
			graphics_entered = 0;
		}

		kern_text_resume();
	}

	mutex_unlock(&graphics_lock);
}

/*
 * Ends /dev/graphics for good once the console has left the display.
 */
int
drv_graphics_device_end(
	const struct kern_text_range *ranges,
	unsigned count)
{
	int error;

	/*
	 * Before the device is registered there is no lock, no open and no
	 * mode.  The choice is fixed now, so a backend offered later is refused
	 * and the registration keeps this one.
	 */
	if (!graphics_lock_ready) {
		graphics_chosen = 1;
		graphics_backend = graphics_candidate;
		graphics_ended = 1;
		error = backend_end(ranges, count);
		if (error != 0)
			return error;

		/* Succeeded: nothing the backend mapped is left in the ranges. */
		return 0;
	}

	/*
	 * Drops the mode with the owner's rights, without writing the hardware,
	 * and has the backend give the display up; a later caller waits here
	 * for the first one's removals and then judges what they left.
	 */
	mutex_lock(&graphics_lock);

	graphics_ended = 1;
	graphics_entered = 0;
	error = backend_end(ranges, count);

	mutex_unlock(&graphics_lock);

	/* Reports what was left in the ranges. */
	if (error != 0)
		return error;

	/* Succeeded: nothing the backend mapped is left in the ranges. */
	return 0;
}

/* Initializes the sleepable lock on the first registration. */
static void
graphics_lock_init_once(
	void)
{
	/* Initializes the lock only once. */
	if (!graphics_lock_ready) {
		(void)mutex_init(&graphics_lock, LOCK_RANK_DEVICE,
				 "graphics device");
		graphics_lock_ready = 1;
	}
}

/*
 * Has the chosen backend give the display up; 0 when there is no backend
 * or it has nothing to give back.
 */
static int
backend_end(
	const struct kern_text_range *ranges,
	unsigned count)
{
	int error;

	/* A backend without an end keeps no mapping to give back. */
	if (graphics_backend == NULL || graphics_backend->end == NULL)
		return 0;

	/* The backend removes its mappings and judges what is left. */
	error = graphics_backend->end(ranges, count);
	if (error != 0)
		return error;

	/* Succeeded: nothing the backend mapped is left in the ranges. */
	return 0;
}

/* Gives the device to its first opener. */
static int
graphics_open(
	struct file *file)
{
	int ready;
	int error;

	/* Takes ownership when the backend is ready and the device is free. */
	error = 0;
	mutex_lock(&graphics_lock);

	/* Nothing can draw once the console has left the display for good. */
	ready = 0;
	if (!graphics_ended)
		ready = graphics_backend->ready();

	/* Refuses a backend that cannot draw, then a second opener. */
	if (!ready) {
		error = ENODEV;
	} else if (graphics_owner != NULL) {
		error = EBUSY;
	} else {
		graphics_owner = file;
		graphics_entered = 0;
		kern_memset(&graphics_mode, 0, sizeof(graphics_mode));
	}

	mutex_unlock(&graphics_lock);

	/* Returns the open result. */
	return error;
}

/* Releases the device and restores the text when the owner closes. */
static int
graphics_close(
	struct file *file)
{
	/* Leaves graphics mode if the owner was in it. */
	mutex_lock(&graphics_lock);

	if (graphics_owner == file) {
		/* A mode the end dropped is not left again, and the text is not resumed. */
		if (graphics_entered) {
			graphics_backend->leave();
			kern_text_resume();
			graphics_entered = 0;
		}

		graphics_owner = NULL;
	}

	mutex_unlock(&graphics_lock);

	/* Succeeded. */
	return 0;
}

/* Checks that the caller owns the device and entered graphics mode. */
static int
require_entered(
	struct file *file)
{
	/* Refuses another file. */
	if (file != graphics_owner)
		return ENXIO;

	/* Refuses an owner still in text mode. */
	if (!graphics_entered)
		return ENXIO;

	/* Succeeded. */
	return 0;
}

/* Tells whether a rectangle is non-empty and inside the entered mode. */
static int
valid_rect(
	const struct graphics_rect *rect)
{
	/* Refuses an empty rectangle. */
	if (rect->width == 0 || rect->height == 0)
		return 0;

	/* Refuses a corner outside the mode. */
	if (rect->x > graphics_mode.width || rect->y > graphics_mode.height)
		return 0;

	/* Refuses an extent past the mode's edge. */
	if (rect->width > graphics_mode.width - rect->x ||
	    rect->height > graphics_mode.height - rect->y)
		return 0;

	/* The rectangle is valid. */
	return 1;
}

/* Enters the backend's graphics mode for the owner. */
static int
graphics_enter(
	uintptr_t argument)
{
	struct graphics_mode request;
	int error;

	/* Copies the request in. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Refuses a second enter. */
	if (graphics_entered)
		return EINVAL;

	/* Refuses a preferred size with only one dimension. */
	if ((request.preferred_width == 0) != (request.preferred_height == 0))
		return EINVAL;

	/* Refuses a preferred size beyond the interface's limit. */
	if (request.preferred_width > 16384U ||
	    request.preferred_height > 16384U)
		return EINVAL;

	/* Refuses a depth the interface does not name. */
	if (request.preferred_bits_per_pixel != 0 &&
	    request.preferred_bits_per_pixel != 4 &&
	    request.preferred_bits_per_pixel != 8 &&
	    request.preferred_bits_per_pixel != 24 &&
	    request.preferred_bits_per_pixel != 32)
		return EINVAL;

	/* Records the preference the backend reads. */
	kern_memset(&graphics_mode, 0, sizeof(graphics_mode));
	graphics_mode.preferred_width = request.preferred_width;
	graphics_mode.preferred_height = request.preferred_height;
	graphics_mode.preferred_bits_per_pixel =
		request.preferred_bits_per_pixel;

	/* Stops the text console and enters the backend's mode. */
	kern_text_suspend();
	if (!graphics_backend->enter(&graphics_mode)) {
		graphics_backend->leave();
		kern_text_resume();
		return ENODEV;
	}

	graphics_entered = 1;

	/* Reports the mode the backend chose. */
	request.width = graphics_mode.width;
	request.height = graphics_mode.height;
	request.bits_per_pixel = graphics_mode.bits_per_pixel;
	request.stride = graphics_mode.stride;
	request.capabilities = GRAPHICS_CAPABILITIES;

	error = copyout(&request, argument, sizeof(request));
	if (error != 0) {
		graphics_backend->leave();
		kern_text_resume();
		graphics_entered = 0;
		return error;
	}

	/* Succeeded. */
	return 0;
}

/* Copies the backend's mode list out. */
static int
graphics_get_modes(
	uintptr_t argument)
{
	struct graphics_mode_list request;
	struct graphics_mode_info native[GRAPHICS_MAX_MODES];
	struct graphics_mode_info mode;
	size_t total;
	size_t returned;
	size_t i;
	int error;

	/* Copies the request in. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Refuses a malformed request. */
	if (request.reserved != 0 || request.capacity > GRAPHICS_MAX_MODES ||
	    (request.capacity != 0 && request.modes == 0))
		return EINVAL;

	/* Asks the backend for its modes. */
	total = graphics_backend->get_modes(native, request.capacity);
	returned = total < request.capacity ? total : request.capacity;

	/* Copies each mode that fits out. */
	for (i = 0; i < returned; i++) {
		mode.width = native[i].width;
		mode.height = native[i].height;
		mode.bits_per_pixel = native[i].bits_per_pixel;
		mode.stride = native[i].stride;

		error = copyout(&mode, request.modes + i * sizeof(mode),
				sizeof(mode));
		if (error != 0)
			return error;
	}

	/* Reports the full count. */
	request.count = (uint32_t)total;
	error = copyout(&request, argument, sizeof(request));

	/* Returns the copy result. */
	return error;
}

/* Fills a rectangle with one color. */
static int
graphics_fill(
	uintptr_t argument)
{
	struct graphics_fill request;
	int error;

	/* Copies the request in. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Refuses a malformed rectangle. */
	if (request.reserved != 0 || !valid_rect(&request.rect))
		return EINVAL;

	/* Draws through the backend. */
	if (!graphics_backend->fill(&request.rect, request.color))
		return EIO;

	/* Succeeded. */
	return 0;
}

/* Fills the set pixels of an 8x8 pattern in a rectangle. */
static int
graphics_pattern_fill(
	uintptr_t argument)
{
	struct graphics_pattern_fill request;
	int error;

	/* Copies the request in. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Refuses a malformed rectangle. */
	if (request.reserved != 0 || !valid_rect(&request.rect))
		return EINVAL;

	/* Draws through the backend. */
	if (!graphics_backend->pattern_fill(&request.rect, request.color,
					    request.pattern))
		return EIO;

	/* Succeeded. */
	return 0;
}

/* Draws a line between two points inside the mode. */
static int
graphics_line(
	uintptr_t argument)
{
	struct graphics_line request;
	int error;

	/* Copies the request in. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Refuses an end point outside the mode. */
	if (request.reserved != 0 || request.x0 >= graphics_mode.width ||
	    request.x1 >= graphics_mode.width ||
	    request.y0 >= graphics_mode.height ||
	    request.y1 >= graphics_mode.height)
		return EINVAL;

	/* Draws through the backend. */
	if (!graphics_backend->line(request.x0, request.y0, request.x1,
				    request.y1, request.color))
		return EIO;

	/* Succeeded. */
	return 0;
}

/* Loads the palette a blit's format needs. */
static int
load_palette(
	const struct graphics_blit *request)
{
	int error;

	/* Takes the two MONO1 colors from the request. */
	if (request->format == KERN_GRAPHICS_FORMAT_MONO1) {
		palette_buffer[0] = request->background;
		palette_buffer[1] = request->foreground;
		return 0;
	}

	/* Refuses a palette on a direct-color blit. */
	if (request->format == KERN_GRAPHICS_FORMAT_RGB24) {
		if (request->palette != 0 || request->palette_count != 0)
			return EINVAL;
		return 0;
	}

	/* Refuses an INDEX8 blit without a usable palette. */
	if (request->format != KERN_GRAPHICS_FORMAT_INDEX8 ||
	    request->palette == 0 || request->palette_count == 0 ||
	    request->palette_count > 256U)
		return EINVAL;

	/* Copies the INDEX8 palette in. */
	error = copyin(request->palette, palette_buffer,
		       request->palette_count * sizeof(palette_buffer[0]));

	/* Returns the copy result. */
	return error;
}

/* Returns how many pixels of a row one chunk of a format carries. */
static unsigned
chunk_pixels(
	unsigned format)
{
	/* Picks the chunk that fits the buffers for this format. */
	if (format == KERN_GRAPHICS_FORMAT_RGB24)
		return GRAPHICS_CHUNK_RGB24;
	if (format == KERN_GRAPHICS_FORMAT_MONO1)
		return GRAPHICS_CHUNK_MONO1;

	/* INDEX8 is one byte per pixel. */
	return GRAPHICS_CHUNK_INDEX8;
}

/*
 * Copies pixels [first, first + count) of one user row into the row
 * buffer, expanding MONO1 to one index byte per pixel.
 */
static int
copy_chunk(
	const struct graphics_blit *request,
	uint64_t row_offset,
	unsigned first,
	unsigned count)
{
	uint64_t offset;
	size_t bytes;
	unsigned column;
	int error;

	/* Locates and sizes the chunk in the user's row. */
	if (request->format == KERN_GRAPHICS_FORMAT_RGB24) {
		offset = row_offset + (uint64_t)first * 3U;
		bytes = (size_t)count * 3U;
	} else if (request->format == KERN_GRAPHICS_FORMAT_MONO1) {
		offset = row_offset + first / 8U;
		bytes = ((size_t)count + 7U) / 8U;
	} else {
		offset = row_offset + first;
		bytes = count;
	}

	/* Refuses an address that would wrap. */
	if (offset > UINTPTR_MAX - (uintptr_t)request->pixels)
		return EFAULT;

	/* Copies a direct or indexed chunk straight into the row buffer. */
	if (request->format != KERN_GRAPHICS_FORMAT_MONO1) {
		error = copyin(request->pixels + (uintptr_t)offset, row_buffer,
			       bytes);
		return error;
	}

	/* Copies the packed MONO1 bits in. */
	error = copyin(request->pixels + (uintptr_t)offset, packed_buffer,
		       bytes);
	if (error != 0)
		return error;

	/* Expands each bit, high bit first, to an index of 0 or 1. */
	for (column = 0; column < count; column++) {
		row_buffer[column] =
			(packed_buffer[column / 8U] >> (7U - column % 8U)) & 1U;
	}

	/* Succeeded. */
	return 0;
}

/*
 * Draws a user image row by row, each row in chunks the buffers hold.
 *
 * The pattern phase of every chunk is its offset from the request's
 * top-left corner, so a BLIT_PATTERN looks the same however it is split.
 */
static int
graphics_blit(
	uintptr_t argument,
	int patterned)
{
	struct graphics_blit request;
	struct drv_graphics_row chunk;
	uint64_t minimum_stride;
	uint64_t row_offset;
	unsigned chunk_limit;
	unsigned first;
	unsigned count;
	unsigned row;
	int error;

	/* Copies the request in. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Refuses an empty or out-of-mode image. */
	if (request.reserved != 0 || request.width == 0 ||
	    request.height == 0 || request.x > graphics_mode.width ||
	    request.y > graphics_mode.height ||
	    request.width > graphics_mode.width - request.x ||
	    request.height > graphics_mode.height - request.y ||
	    request.pixels == 0)
		return EINVAL;

	/* Computes the bytes one source row needs. */
	if (request.format == KERN_GRAPHICS_FORMAT_RGB24)
		minimum_stride = (uint64_t)request.width * 3U;
	else if (request.format == KERN_GRAPHICS_FORMAT_INDEX8)
		minimum_stride = request.width;
	else if (request.format == KERN_GRAPHICS_FORMAT_MONO1)
		minimum_stride = ((uint64_t)request.width + 7U) / 8U;
	else
		return EINVAL;

	/* Refuses a stride shorter than a row. */
	if (minimum_stride > request.stride)
		return EINVAL;

	/* Loads the palette. */
	error = load_palette(&request);
	if (error != 0)
		return error;

	/* Describes the chunks the backend gets. */
	kern_memset(&chunk, 0, sizeof(chunk));
	chunk.format = request.format == KERN_GRAPHICS_FORMAT_RGB24
			       ? KERN_GRAPHICS_FORMAT_RGB24
			       : KERN_GRAPHICS_FORMAT_INDEX8;
	chunk.pixels = row_buffer;
	chunk.palette = palette_buffer;
	if (request.format == KERN_GRAPHICS_FORMAT_RGB24)
		chunk.palette_size = 0;
	else if (request.format == KERN_GRAPHICS_FORMAT_MONO1)
		chunk.palette_size = 2;
	else
		chunk.palette_size = request.palette_count;
	chunk_limit = chunk_pixels(request.format);

	/* Draws each row, chunk by chunk. */
	for (row = 0; row < request.height; row++) {
		row_offset = (uint64_t)request.stride * row;

		for (first = 0; first < request.width; first += count) {
			count = request.width - first;
			if (count > chunk_limit)
				count = chunk_limit;

			error = copy_chunk(&request, row_offset, first, count);
			if (error != 0)
				return error;

			chunk.width = count;
			chunk.pattern_x = first;
			chunk.pattern_y = row;
			if (!graphics_backend->blit_row(request.x + first,
							request.y + row, &chunk,
							request.pattern,
							patterned))
				return EIO;
		}
	}

	/* Succeeded. */
	return 0;
}

/* Pushes the given rectangles, or the whole screen, to the display. */
static int
graphics_flush(
	uintptr_t argument)
{
	struct graphics_flush request;
	struct graphics_rect input[GRAPHICS_MAX_RECTS];
	unsigned i;
	int error;

	/* Copies the request in. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Refuses too many rectangles or a missing list. */
	if (request.rectangle_count > GRAPHICS_MAX_RECTS ||
	    (request.rectangle_count != 0 && request.rectangles == 0))
		return EINVAL;

	/* Copies the rectangles in. */
	if (request.rectangle_count != 0) {
		error = copyin(request.rectangles, input,
			       request.rectangle_count * sizeof(input[0]));
		if (error != 0)
			return error;
	}

	/* Refuses a rectangle outside the mode. */
	for (i = 0; i < request.rectangle_count; i++) {
		if (!valid_rect(&input[i]))
			return EINVAL;
	}

	/* Flushes through the backend. */
	if (!graphics_backend->flush(input, request.rectangle_count))
		return EIO;

	/* Succeeded. */
	return 0;
}

/* Copies one glyph of the backend's font out. */
static int
graphics_glyph(
	uintptr_t argument)
{
	struct graphics_glyph request;
	uint8_t bitmap[32];
	unsigned width;
	unsigned height;
	int error;

	/* Copies the request in. */
	error = copyin(argument, &request, sizeof(request));
	if (error != 0)
		return error;

	/* Refuses a missing or short bitmap. */
	if (request.reserved != 0 || request.bitmap == 0 ||
	    request.bitmap_capacity < sizeof(bitmap))
		return EINVAL;

	/* Asks the backend for the glyph. */
	if (!graphics_backend->get_glyph(request.codepoint, bitmap, &width,
					 &height))
		return EINVAL;

	/* Describes the one-bit glyph. */
	request.width = width;
	request.height = height;
	request.stride = width / 8U;
	request.bearing_x = 0;
	request.bearing_y = 0;
	request.advance = width;
	request.format = KERN_GRAPHICS_GLYPH_MSB1;
	request.bitmap_size = request.stride * height;

	/* Copies the bitmap and the description out. */
	error = copyout(bitmap, request.bitmap, request.bitmap_size);
	if (error != 0)
		return error;

	error = copyout(&request, argument, sizeof(request));

	/* Returns the copy result. */
	return error;
}

/* Reports the capabilities and the largest mode. */
static int
graphics_get_caps(
	uintptr_t argument)
{
	struct graphics_caps caps = {GRAPHICS_CAPABILITIES, 0, 0, 0};
	struct graphics_mode_info modes[GRAPHICS_MAX_MODES];
	size_t count;
	size_t i;
	int error;

	/* Asks the backend for its modes. */
	count = graphics_backend->get_modes(modes, GRAPHICS_MAX_MODES);
	if (count > GRAPHICS_MAX_MODES)
		count = GRAPHICS_MAX_MODES;

	/* Finds the largest width and height. */
	for (i = 0; i < count; i++) {
		if (modes[i].width > caps.maximum_width)
			caps.maximum_width = modes[i].width;
		if (modes[i].height > caps.maximum_height)
			caps.maximum_height = modes[i].height;
	}

	/* Copies the capabilities out. */
	error = copyout(&caps, argument, sizeof(caps));

	/* Returns the copy result. */
	return error;
}

/* Reports the entered mode. */
static int
graphics_get_mode(
	uintptr_t argument)
{
	struct graphics_mode mode;
	int error;

	/* Describes the entered mode. */
	kern_memset(&mode, 0, sizeof(mode));
	mode.preferred_width = graphics_mode.preferred_width;
	mode.preferred_height = graphics_mode.preferred_height;
	mode.preferred_bits_per_pixel = graphics_mode.preferred_bits_per_pixel;
	mode.width = graphics_mode.width;
	mode.height = graphics_mode.height;
	mode.bits_per_pixel = graphics_mode.bits_per_pixel;
	mode.stride = graphics_mode.stride;
	mode.capabilities = GRAPHICS_CAPABILITIES;

	/* Copies the mode out. */
	error = copyout(&mode, argument, sizeof(mode));

	/* Returns the copy result. */
	return error;
}

/* Dispatches one ioctl of the owner. */
static int
graphics_ioctl_locked(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	int error;

	/* Refuses a file that is not the owner. */
	if (file != graphics_owner)
		return EBADF;

	/* Serves the requests allowed before enter. */
	if (request == KERN_GRAPHICS_GET_CAPS)
		return graphics_get_caps(argument);
	if (request == KERN_GRAPHICS_GET_MODES)
		return graphics_get_modes(argument);
	if (request == KERN_GRAPHICS_ENTER)
		return graphics_enter(argument);

	/* Refuses drawing before enter. */
	error = require_entered(file);
	if (error != 0)
		return error;

	/* Serves the drawing requests. */
	switch (request) {
	case KERN_GRAPHICS_GET_MODE:
		return graphics_get_mode(argument);
	case KERN_GRAPHICS_FILL_RECT:
		return graphics_fill(argument);
	case KERN_GRAPHICS_DRAW_LINE:
		return graphics_line(argument);
	case KERN_GRAPHICS_PATTERN_FILL:
		return graphics_pattern_fill(argument);
	case KERN_GRAPHICS_BLIT:
		return graphics_blit(argument, 0);
	case KERN_GRAPHICS_BLIT_PATTERN:
		return graphics_blit(argument, 1);
	case KERN_GRAPHICS_FLUSH:
		return graphics_flush(argument);
	case KERN_GRAPHICS_GET_GLYPH:
		return graphics_glyph(argument);
	default:
		return EOPNOTSUPP;
	}
}

/* Runs one ioctl under the device lock. */
static int
graphics_ioctl(
	struct file *file,
	unsigned long request,
	uintptr_t argument)
{
	int error;

	/* Dispatches the request under the lock; nothing reaches the display once the console has ended. */
	mutex_lock(&graphics_lock);

	error = ENODEV;
	if (!graphics_ended)
		error = graphics_ioctl_locked(file, request, argument);

	mutex_unlock(&graphics_lock);

	/* Returns the request's result. */
	return error;
}
