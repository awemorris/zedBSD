/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * PC-98 graphics-private frontend/backend boundary.
 */

#include "drivers/platform/pc98/graphics/backend.h"
#include "drivers/platform/pc98/graphics/pc98.h"
#include "drivers/platform/pc98/graphics/display-auto.h"
#include "drivers/platform/pc98/graphics/display.h"
#include <kern/device-io.h>
#include <kern/kcrt.h>

#include "kern/klog.h"
#include "kern/text-display.h"
#include "text.h"
#include <uapi/errno.h>
#include "kern/pmem.h"

#define CIRRUS_PADDR 0xf0000000U
/* One GDC plane, and the Cirrus aperture. */
#define GDC_PLANE_SIZE 0x8000U
#define CIRRUS_APERTURE_SIZE (4U * 1024U * 1024U)

/* How many ranges the end may leave mapped: the four planes and the Cirrus aperture. */
#define PC98_LEFT_MAX 8U

/*
 * The graphics display controller's status and command ports, the status's
 * FIFO-full bit, its START and STOP commands, and how many times a command
 * waits for room in the FIFO.
 *
 * Values referenced: NEC uPD7220 GDC data sheet (the status register's
 * FIFO-full bit and the START and STOP command codes); the PC-9801's
 * graphics GDC at ports 0xa0 and 0xa2.
 */
#define GDC_STATUS_PORT 0x00a0U
#define GDC_COMMAND_PORT 0x00a2U
#define GDC_FIFO_FULL 0x02U
#define GDC_START 0x0dU
#define GDC_STOP 0x0cU
#define GDC_TIMEOUT 1000000U

/*
 * One range of display memory the end left mapped, with the error of its
 * removal (0 for one that cannot be removed at all).
 */
struct pc98_left {
	uint64_t physical;
	uint64_t bytes;
	int error;
};

/* Where the four GDC planes are, in the order gdc_memory[] keeps them. */
static const uint64_t plane_address[4] = {
	0x000a8000U, 0x000b0000U, 0x000b8000U, 0x000e0000U};

/* The four GDC planes and the Cirrus aperture, once mapped. */
static void *gdc_memory[4];
static void *cirrus_memory;

/*
 * Whether the console has left the display (backend_ended), and what the
 * end could not remove (left_count ranges of left).  Written once by the
 * end, under the graphics lock when /dev/graphics has one; a later end
 * reads them under the same lock.
 */
static int backend_ended;
static struct pc98_left left[PC98_LEFT_MAX];
static unsigned left_count;

static struct pc98_auto display;
static struct pc98_display_backend backend_hal;
static struct pc98_display_ops native_display;
static int backend_prepared;

static int pc98_graphics_prepare_hardware(void);
static void text_console_start(void);
static uint8_t port_in8(void *context, uint16_t port);
static void port_out8(void *context, uint16_t port, uint8_t value);
static int display_reset(void *context);
static int display_stop(void *context);
static int gdc_command(uint8_t value);
static void left_add(uint64_t physical, uint64_t bytes, int error);
static void mapping_remove(void *mapped, uint64_t physical, size_t bytes);

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
	kern_memset(&info, 0, sizeof(info));
	info.preferred_bits_per_pixel = mode->preferred_bits_per_pixel;
	kern_logf("graphics: enter request: preferred %u bpp\n",
		   mode->preferred_bits_per_pixel);

	/* Checks the enter result. */
	if (!native_display.enter(native_display.context, &info)) {
		kern_logf("graphics: Cirrus and GDC mode entry failed\n");

		/* Succeeded. */
		return 0;
	}

	mode->width = info.width;
	mode->height = info.height;
	mode->bits_per_pixel = info.bits_per_pixel;
	mode->stride = info.stride;
	kern_logf("graphics: %s mode %ux%ux%u stride=%u\n",
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
	int error;
	struct pc98_display_rect native;

	/* Handles the rect availability. */
	if (rect == NULL || native_display.fill == NULL)
		return 0;
	native.x = rect->x;
	native.y = rect->y;
	native.width = rect->width;
	native.height = rect->height;

	/* Computes the function result. */
	error =
		native_display.fill(native_display.context, &native, color);

	/* Returns the computed result. */
	return error;
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
	int error;

	/* Computes the function result. */
	error = native_display.line != NULL &&
			  native_display.line(native_display.context, x0, y0,
					      x1, y1, color);

	/* Returns the computed result. */
	return error;
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
	int error;
	struct pc98_display_rect native;

	/* Handles the rect availability. */
	if (rect == NULL || native_display.pattern_fill == NULL)
		return 0;
	native.x = rect->x;
	native.y = rect->y;
	native.width = rect->width;
	native.height = rect->height;

	/* Computes the function result. */
	error = native_display.pattern_fill(native_display.context,
						      &native, color, pattern);

	/* Returns the computed result. */
	return error;
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
	int error;
	struct pc98_display_image native;
	unsigned i;

	/* Handles the image availability. */
	if (image == NULL || image->palette_size > 256U)
		return 0;
	kern_memset(&native, 0, sizeof(native));
	native.format = image->format == 1U ? PC98_DISPLAY_IMAGE_INDEX8
					    : PC98_DISPLAY_IMAGE_RGB24;
	native.width = image->width;
	native.height = image->height;
	native.stride = image->stride;
	native.pixels = image->pixels;
	native.palette_size = image->palette_size;
	native.pattern_y = image->pattern_y;
	/* Process each remaining element. */
	for (i = 0; i < image->palette_size; i++)
		native.palette[i] = image->palette[i];

	/* Handles the patterned condition. */
	if (patterned) {
		/* Computes the function result. */
		error =
			native_display.draw_image_pattern != NULL &&
			native_display.draw_image_pattern(
				native_display.context, x, y, &native, pattern);

		/* Failed. */
		return error;
	}

	/* Computes the function result. */
	error = native_display.draw_image != NULL &&
			  native_display.draw_image(native_display.context, x,
						    y, &native);

	/* Returns the computed result. */
	return error;
}

/*
 * Implements the drv pc98 graphics backend flush operation.
 */
int
drv_pc98_graphics_backend_flush(
	const struct graphics_rect *rectangles,
	size_t count)
{
	int error;
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
	error = native_display.flush(
		native_display.context, count == 0 ? NULL : native, count);

	/* Returns the computed result. */
	return error;
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
	int error;

	/* Obtains the drv pc98 glyph get bitmap result. */
	error = drv_pc98_glyph_get_bitmap(&display.glyph, codepoint,
						    font, width, height);

	/* Returns the computed result. */
	return error;
}

/*
 * Implements the drv pc98 graphics prepare operation.
 */
int
drv_pc98_graphics_prepare(
	void)
{
	int prepared;

	/* Brings up the graphics planes, which may not all be present. */
	backend_prepared = 0;
	prepared = pc98_graphics_prepare_hardware();
	backend_prepared = prepared != 0;

	/*
	 * Text memory is always present on this board, so the console
	 * comes up either way. This publishes the text table and hands
	 * HAL output over, after which kern_logf() and /dev/console
	 * share one cursor.
	 */
	text_console_start();

	/* Reports whether the graphics planes are usable. */
	return prepared;
}

/*
 * Brings up the text grid and hands HAL output over to it.
 */
static void
text_console_start(
	void)
{
	drv_pc98_text_init();

	/* Publishes the handover only once the layer can actually draw. */
	if (drv_pc98_text_ready()) {
		__atomic_store_n(&kernel_putc, drv_pc98_text_putc,
				 __ATOMIC_RELEASE);
	}
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

/*
 * Gives the display up for good, once the console has left it: nothing is
 * drawn through the backend again, and every kernel mapping of display
 * memory it made (the four GDC planes and the Cirrus aperture) is removed;
 * the hardware is not written.  Returns 0 when nothing left mapped lies in
 * the count ranges, the error of a removal that failed for a range in
 * them, or EBUSY for a range in them that cannot be removed.  A later call
 * judges what the first left against its own ranges.  The caller holds
 * the graphics lock, or it runs before /dev/graphics has one.
 */
int
drv_pc98_graphics_backend_end(
	const struct kern_text_range *ranges,
	unsigned count)
{
	unsigned index;
	int overlaps;

	/* The first end removes the mappings; a later one only judges. */
	if (!backend_ended) {
		/* Nothing is drawn through the backend from now on. */
		backend_ended = 1;
		backend_prepared = 0;
		kern_memset(&native_display, 0, sizeof(native_display));

		/* Removes each of the four GDC planes' mappings. */
		for (index = 0U; index < 4U; index++) {
			/* The native display no longer reaches the plane. */
			display.gdc.planes[index] = NULL;

			/* The plane's mapping, when the start-up made one. */
			if (gdc_memory[index] != NULL)
				mapping_remove(gdc_memory[index], plane_address[index], GDC_PLANE_SIZE);
			gdc_memory[index] = NULL;
		}

		/* The Cirrus aperture's mapping (its registers are not written). */
		if (cirrus_memory != NULL)
			mapping_remove(cirrus_memory, CIRRUS_PADDR, CIRRUS_APERTURE_SIZE);
		cirrus_memory = NULL;
	}

	/* What was left, against the caller's ranges: a failed removal first. */
	for (index = 0U; index < left_count; index++) {
		/* A range of the caller's memory whose removal failed. */
		overlaps = kern_text_range_overlaps(ranges, count, left[index].physical, left[index].bytes);
		if (overlaps && left[index].error != 0)
			return left[index].error;
	}

	/* Then a range that stays mapped for good. */
	for (index = 0U; index < left_count; index++) {
		/* A range of the caller's memory that cannot be removed. */
		overlaps = kern_text_range_overlaps(ranges, count, left[index].physical, left[index].bytes);
		if (overlaps)
			return EBUSY;
	}

	/* Succeeded: nothing the backend mapped is left in the ranges. */
	return 0;
}

/* Supports the pc98 graphics prepare hardware operation. */
static int
pc98_graphics_prepare_hardware(
	void)
{
	unsigned i;

	/* Maps the four uncached GDC planes. */
	for (i = 0; i < 4; i++) {
		/* Stops at the first plane the kernel cannot map. */
		if (kern_device_map(plane_address[i], GDC_PLANE_SIZE,
				    KERN_DEVICE_UNCACHED,
				    &gdc_memory[i]) != 0)
			goto fail;
	}

	/* Maps the uncached Cirrus aperture. */
	if (kern_device_map(CIRRUS_PADDR, CIRRUS_APERTURE_SIZE,
			    KERN_DEVICE_UNCACHED, &cirrus_memory) != 0)
		goto fail;
	drv_pc98_auto_default(&display, display_reset, display_stop, NULL,
			      port_in8, port_out8, NULL,
			      (volatile uint8_t *)cirrus_memory);

	/* Kernel code may run while a user CR3 is active. */
	for (i = 0; i < 4; i++)
		display.gdc.planes[i] = (volatile uint8_t *)gdc_memory[i];

	/* Checks the drv pc98 auto make hal result. */
	if (!drv_pc98_auto_make_hal(&backend_hal, &display))
		goto fail;
	native_display = backend_hal.display;

	/* Reports operation failure. */
	return 1;

fail:

	/* Releases the Cirrus aperture when it was mapped. */
	if (cirrus_memory != NULL) {
		(void)kern_device_unmap(cirrus_memory,
					CIRRUS_APERTURE_SIZE);
		cirrus_memory = NULL;
	}

	/* Releases every plane mapped before the failure. */
	while (i != 0) {
		i--;

		/* Skips a plane this attempt never reached. */
		if (gdc_memory[i] == NULL)
			continue;
		(void)kern_device_unmap(gdc_memory[i], GDC_PLANE_SIZE);
		gdc_memory[i] = NULL;
	}

	/* Succeeded. */
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
	int sent;

	(void)context;

	/* Starts the graphics display controller. */
	sent = gdc_command(GDC_START);

	/* Reports that the controller never had room for the command. */
	if (!sent)
		return 0;

	/* Succeeded: the controller took the command. */
	return 1;
}

/* Supports the display stop operation. */
static int
display_stop(
	void *context)
{
	int sent;

	(void)context;

	/* Stops the graphics display controller. */
	sent = gdc_command(GDC_STOP);

	/* Reports that the controller never had room for the command. */
	if (!sent)
		return 0;

	/* Succeeded: the controller took the command. */
	return 1;
}

/*
 * Records a range of display memory the end leaves mapped, with the error
 * of its removal (0 for one that cannot be removed).  A full table keeps
 * its last entry as the whole address space, so that every range judged
 * later finds it.
 */
static void
left_add(
	uint64_t physical,
	uint64_t bytes,
	int error)
{
	/* A table with room takes the range itself. */
	if (left_count < PC98_LEFT_MAX) {
		left[left_count].physical = physical;
		left[left_count].bytes = bytes;
		left[left_count].error = error;
		left_count++;
		return;
	}

	/* A full table stands for everything from now on. */
	left[PC98_LEFT_MAX - 1U].physical = 0U;
	left[PC98_LEFT_MAX - 1U].bytes = UINT64_MAX;
	left[PC98_LEFT_MAX - 1U].error = 0;
}

/*
 * Removes one kernel mapping of display memory, recording the range as
 * left when the removal fails (EBUSY from the hardware layer: a window it
 * keeps mapped).
 */
static void
mapping_remove(
	void *mapped,
	uint64_t physical,
	size_t bytes)
{
	int error;

	/* The kernel's own call, which asks the hardware layer. */
	error = kern_device_unmap(mapped, bytes);

	/* A window the hardware layer keeps is left for good; another failure is left with its error. */
	if (error == EBUSY) {
		left_add(physical, bytes, 0);
	} else if (error != 0) {
		left_add(physical, bytes, error);
	}
}

/*
 * Sends one command to the graphics display controller once its FIFO has
 * room.  Returns 1 when it was sent, 0 when the FIFO stayed full.
 */
static int
gdc_command(
	uint8_t value)
{
	unsigned timeout;
	uint8_t status;

	/* Waits a bounded time for room in the FIFO. */
	status = GDC_FIFO_FULL;
	for (timeout = GDC_TIMEOUT; timeout != 0; timeout--) {
		status = kern_io_in8(GDC_STATUS_PORT);
		if ((status & GDC_FIFO_FULL) == 0)
			break;
	}

	/* The FIFO never had room. */
	if ((status & GDC_FIFO_FULL) != 0)
		return 0;

	/* Sends the command now that the FIFO has room. */
	kern_io_out8(GDC_COMMAND_PORT, value);

	/* Succeeded: the command was sent. */
	return 1;
}
