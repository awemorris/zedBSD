/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Immutable Vulkan state lowers to a complete independently copied native BCL state sequence. */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-native-draw.h"

static void clip_axis(int32_t viewport_begin, int32_t viewport_end, int32_t area_begin, uint32_t area_size, int32_t scissor_begin, uint32_t scissor_size, uint32_t limit, uint32_t *begin, uint32_t *size);

/*
 * Prepares complete binning state from one frozen draw while its native upload root owns every referenced GPU interval.
 *
 * Drawable, render area, viewport and user scissor are intersected before
 * hardware guardband clipping.  All scalar state is copied; the resulting
 * sequence retains no pipeline, framebuffer or command-record pointer.
 */
int
bcm2711_vulkan_native_bin_prepare(
	const struct bcm2711_vulkan_prepared_event *event,
	struct bcm2711_vulkan_native_draw *draw)
{
	struct bcm2711_native_bin state;
	const struct bcm2711_shader_binary *fragment;
	uint32_t index;
	int error;

	/* The pending graphics graph or private meta description supplies only copied drawable and render-area numbers. */
	if (event == NULL ||
	    draw == NULL ||
	    event->pipeline == NULL)
		return EINVAL;
	draw->bin_bytes = 0;
	if (event->width == 0 ||
	    event->width > 4096U ||
	    event->height == 0 ||
	    event->height > 4096U)
		return EINVAL;

	/* Exact nonnegative target/scissor bounds cannot exceed the complete logical drawable. */
	if (event->area.offset.x < 0 ||
	    event->area.offset.y < 0 ||
	    event->scissor.offset.x < 0 ||
	    event->scissor.offset.y < 0)
		return EINVAL;
	if ((uint64_t)event->area.offset.x + event->area.extent.width > event->width || (uint64_t)event->area.offset.y + event->area.extent.height > event->height)
		return EINVAL;

	/* Viewport preparation validates all six copied IEEE words before any packet is published. */
	kern_memset(&state, 0, sizeof(state));
	error = bcm2711_native_viewport_prepare(event->viewport, &state.viewport);
	if (error != 0)
		return error;
	error = bcm2711_native_viewport_clip_prepare(event->viewport, &state.clipper);
	if (error != 0)
		return error;

	/* Native guardband clipping requires explicit viewport restriction as well as drawable and user scissors. */
	clip_axis(state.clipper.bounds[0], state.clipper.bounds[2], event->area.offset.x, event->area.extent.width, event->scissor.offset.x, event->scissor.extent.width, event->width, &state.window[0], &state.window[2]);
	clip_axis(state.clipper.bounds[1], state.clipper.bounds[3], event->area.offset.y, event->area.extent.height, event->scissor.offset.y, event->scissor.extent.height, event->height, &state.window[1], &state.window[3]);

	/* Facing choices are exact immutable pipeline fields; unsupported bits cannot silently change culling. */
	if ((event->pipeline->cull & ~(VK_CULL_MODE_FRONT_BIT | VK_CULL_MODE_BACK_BIT)) != 0)
		return EINVAL;
	if (event->pipeline->front != VK_FRONT_FACE_COUNTER_CLOCKWISE && event->pipeline->front != VK_FRONT_FACE_CLOCKWISE)
		return EINVAL;
	if ((event->pipeline->cull & VK_CULL_MODE_FRONT_BIT) == 0)
		state.forward = 1;
	if ((event->pipeline->cull & VK_CULL_MODE_BACK_BIT) == 0)
		state.reverse = 1;
	if (event->pipeline->front == VK_FRONT_FACE_COUNTER_CLOCKWISE)
		state.clockwise = 1;

	/* Native varying flag order follows the exact fragment scalar FIFO, including locations crossing group twenty-four. */
	fragment = event->pipeline->programs[BCM2711_SHADER_FRAGMENT];
	if (fragment == NULL || fragment->input_count > BCM2711_SHADER_INTERFACE_WORDS)
		return EINVAL;
	for (index = 0; index < fragment->input_count; index++) {
		if (fragment->inputs[index].flat > 1U || fragment->inputs[index].noperspective > 1U)
			return EINVAL;
		if (fragment->inputs[index].flat != 0)
			state.flat |= 1U << index;
		if (fragment->inputs[index].noperspective != 0)
			state.noperspective |= 1U << index;
	}

	/* Only complete owned shader/fetch intervals enter the numerical BCL sequence. */
	state.shader = draw->shader;
	state.attributes = draw->attributes;
	state.vertices = draw->vertices;
	error = bcm2711_native_bin_encode(&state, draw->bin, sizeof(draw->bin));
	if (error != 0)
		return error;
	draw->bin_bytes = BCM2711_NATIVE_BIN_BYTES;

	/* Succeeded: the enclosing pass can copy this whole self-contained sequence into its owned bin stream. */
	return 0;
}

/* Intersects one signed viewport axis with the render area, user scissor and complete drawable, preserving empty intersections. */
static void
clip_axis(
	int32_t viewport_begin,
	int32_t viewport_end,
	int32_t area_begin,
	uint32_t area_size,
	int32_t scissor_begin,
	uint32_t scissor_size,
	uint32_t limit,
	uint32_t *begin,
	uint32_t *size)
{
	int64_t minimum;
	int64_t maximum;
	int64_t end;

	/* All additions stay in signed sixty-four-bit arithmetic before native unsigned pixel conversion. */
	minimum = viewport_begin;
	maximum = viewport_end;
	if (minimum < area_begin)
		minimum = area_begin;
	end = (int64_t)area_begin + area_size;
	if (maximum > end)
		maximum = end;
	if (minimum < scissor_begin)
		minimum = scissor_begin;
	end = (int64_t)scissor_begin + scissor_size;
	if (maximum > end)
		maximum = end;

	/* The drawable bounds also prevent coarse guardband state from admitting writes to another image's storage. */
	if (minimum < 0)
		minimum = 0;
	if (maximum > limit)
		maximum = limit;

	/* Entirely off-drawable intersections have a representable empty native clip window. */
	if (minimum > limit || maximum < 0) {
		minimum = 0;
		maximum = 0;
	}

	/* Other empty intersections collapse at their bounded minimum pixel. */
	if (maximum < minimum)
		maximum = minimum;
	*begin = (uint32_t)minimum;
	*size = (uint32_t)(maximum - minimum);

	/* Succeeded: native geometry never underflows or wraps a logical empty rectangle. */
	return;
}
