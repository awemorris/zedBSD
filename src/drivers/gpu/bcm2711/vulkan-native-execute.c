/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* FIFO execution clears only the exact colour render area before supervised native bin/render launch; callback completion never retires uncertain DMA. */
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/native-colour.h"
#include "drivers/gpu/bcm2711/vulkan-native-pass.h"

static int validate_pass(struct bcm2711_vulkan_native_pass *pass);
static int clear_target(struct bcm2711_vulkan_native_pass *pass);

/*
 * Executes one completely prepared native pass while its enclosing submission retains all pending owners.
 *
 * The controller mutex excludes other native execution, alias edits and
 * recovery.  CPU clear touches only the exact render area in coherent RAM
 * after complete preparation.  Native IRQ completion and output cache clean
 * determine retirement; the caller transfers the whole payload to controller
 * quarantine when retirement is false, retaining the session and prepared
 * primary until checked reset supplies an independent DMA-stop proof.
 */
int
bcm2711_vulkan_native_pass_run(
	struct bcm2711_vulkan_native_pass *pass,
	struct bcm2711_v3d_job_result *result)
{
	int error;

	/* Missing completion storage cannot report safe retirement. */
	if (result == NULL)
		return EINVAL;
	kern_memset(result, 0, sizeof(*result));
	result->retired = true;

	/* Only an independently owned prepared pass can execute. */
	if (pass == NULL)
		return EINVAL;

	/* An already executed root cannot replay clear or native work, including after uncertain failure. */
	if (pass->executed) {
		result->retired = pass->retired;
		return EBUSY;
	}

	/* Complete live mapped ownership and numerical target state are checked before any target byte changes. */
	error = validate_pass(pass);
	if (error != 0)
		return error;

	/* Exact clear conversion is fallible only before publication; ordinary load/discard never changes CPU target bytes. */
	error = clear_target(pass);
	if (error != 0)
		return error;

	/* The single-use root and every coherent CPU write precede the native runner's supervised launch. */
	pass->executed = true;
	pass->retired = false;
	kern_io_write_barrier();
	error = bcm2711_v3d_job_run(pass->space->native, &pass->job, result);
	pass->retired = result->retired;
	if (error != 0)
		return error;

	/* A successful callback without actual retired DMA cannot permit source reuse or native owner release. */
	if (!result->retired)
		return EIO;

	/* Completion and GPU output clean precede a following FIFO CPU read or independently staged sampled texture. */
	kern_io_read_barrier();

	/* The completed implicit render-pass transition becomes FIFO-visible only after successful native output retirement. */
	pass->target->layout = pass->final_layout;

	/* Succeeded: native bin/render completion and output visibility were proved by the existing IRQ/cache runner. */
	return 0;
}

/* Verifies all complete independently owned intervals and exact CPU target bounds before clear or launch. */
static int
validate_pass(
	struct bcm2711_vulkan_native_pass *pass)
{
	struct bcm2711_vulkan_native_draw *draw;
	struct bcm2711_native_storage *storage;
	struct bcm2711_buffer *buffer;
	uint32_t index;
	uint64_t offset;
	uint64_t bytes;

	/* Device admission and whole native ownership are prerequisites for any CPU target mutation. */
	if (pass->space == NULL ||
	    pass->space->native == NULL ||
	    pass->output == NULL ||
	    pass->cpu == NULL)
		return EINVAL;

	/* Faulted or stopped native hardware refuses before coherent CPU clear changes target contents. */
	if (!pass->space->native->hardware.initialized ||
	    !pass->space->native->hardware.ready ||
	    !pass->space->native->power.ready)
		return EIO;

	/* A pass's explicit initial layout must match current FIFO state unless the application intentionally discards its earlier contents. */
	if (pass->target == NULL)
		return EINVAL;
	if (pass->initial_layout != VK_IMAGE_LAYOUT_UNDEFINED && pass->initial_layout != pass->target->layout)
		return EINVAL;

	/* Actual output RAM stays coherent, mapped and independently referenced through every device write. */
	buffer = pass->output->buffer;
	if (pass->output->quarantined ||
	    pass->output->references == 0 ||
	    buffer == NULL ||
	    !buffer->uncached ||
	    buffer->address == NULL)
		return EIO;

	/* Numerical output address and copied CPU alias must describe the same complete retained mapping interval. */
	if (pass->state.output < pass->output->address)
		return EINVAL;
	offset = (uint64_t)pass->state.output - pass->output->address;
	if (offset > buffer->bytes ||
	    pass->state.output_bytes > buffer->bytes - offset ||
	    offset > pass->output->bytes ||
	    pass->state.output_bytes > pass->output->bytes - offset)
		return EINVAL;

	/* The actual coherent CPU alias has exactly the same logical binding offset as the native target pointer. */
	if ((uint8_t *)buffer->address + offset != pass->cpu)
		return EINVAL;

	/* Exact row pitch and full framebuffer rows stay inside the logical native image, including trailing row padding. */
	bytes = (uint64_t)pass->state.pitch * pass->state.height;
	if (pass->state.width == 0 ||
	    pass->state.width > 4096U ||
	    pass->state.height == 0 ||
	    pass->state.height > 4096U ||
	    pass->state.pitch < pass->state.width * 4U ||
	    bytes > pass->state.output_bytes)
		return EINVAL;

	/* Exact nonempty clear bounds cannot underflow or exceed the already checked complete framebuffer. */
	if (pass->area.offset.x < 0 ||
	    pass->area.offset.y < 0 ||
	    pass->area.extent.width == 0 ||
	    pass->area.extent.height == 0)
		return EINVAL;

	/* Widen before adding logical render bounds to their signed origins. */
	if ((uint64_t)pass->area.offset.x + pass->area.extent.width > pass->state.width || (uint64_t)pass->area.offset.y + pass->area.extent.height > pass->state.height)
		return EINVAL;

	/* Every command/pool/state/overflow allocation remains live before the native runner's final mapping checks. */
	if (pass->count != BCM2711_VULKAN_PASS_STORAGE || pass->job.kind != BCM2711_V3D_JOB_CL)
		return EINVAL;
	for (index = 0; index < pass->count; index++) {
		storage = pass->storage[index];

		/* Quarantined or dead command mappings cannot be revalidated by a successful CPU clear. */
		if (storage == NULL ||
		    storage->space != pass->space ||
		    storage->view == NULL ||
		    storage->view->quarantined ||
		    storage->view->references == 0)
			return EIO;
	}

	/* Linked independent draw inputs must stay mapped through the same native pass execution. */
	draw = pass->first;
	while (draw != NULL) {
		for (index = 0; index < draw->count; index++) {
			storage = draw->storage[index];

			/* Input mappings remain independently held even when their public resource or pipeline identity was withdrawn. */
			if (storage == NULL ||
			    storage->space != pass->space ||
			    storage->view == NULL ||
			    storage->view->quarantined ||
			    storage->view->references == 0)
				return EIO;
		}

		/* Complete numerical draw chains own only native input roots. */
		draw = draw->next;
	}

	/* Succeeded: CPU clear and subsequent native launch can consume only complete live independently held owners. */
	return 0;
}

/* Clears exactly the copied colour render area through the retained coherent alias immediately before native execution. */
static int
clear_target(
	struct bcm2711_vulkan_native_pass *pass)
{
	uint8_t *pixel;
	uint8_t *row;
	uint32_t colour;
	uint32_t swap;
	uint32_t x;
	uint32_t y;
	int error;

	/* Ordinary loading and discarded initial contents require no CPU clear; the native list still preserves out-of-area samples. */
	if (pass->load != VK_ATTACHMENT_LOAD_OP_CLEAR)
		return 0;

	/* Only the implemented numerical colour storage formats supply an exact clamped packed clear. */
	swap = 0;
	if (pass->format == VK_FORMAT_B8G8R8A8_UNORM)
		swap = 1;
	else if (pass->format != VK_FORMAT_R8G8B8A8_UNORM)
		return ENOTSUP;
	error = bcm2711_native_colour_pack(pass->clear, swap, &colour);
	if (error != 0)
		return error;

	/* Each bounded row starts inside the logical coherent image and writes only render-area samples, never row padding. */
	row = (uint8_t *)pass->cpu + (uint64_t)pass->area.offset.y * pass->state.pitch + (uint32_t)pass->area.offset.x * 4U;
	for (y = 0; y < pass->area.extent.height; y++) {
		pixel = row;
		for (x = 0; x < pass->area.extent.width; x++) {
			/* Explicit bytes preserve exact RGBA/BGRA storage order without an unaligned CPU word store. */
			pixel[0] = (uint8_t)colour;
			pixel[1] = (uint8_t)(colour >> 8);
			pixel[2] = (uint8_t)(colour >> 16);
			pixel[3] = (uint8_t)(colour >> 24);
			pixel += 4;
		}

		/* Pitch advances only when another checked logical render row exists, never past the complete final allocation. */
		if (y + 1U < pass->area.extent.height)
			row += pass->state.pitch;
	}

	/* Succeeded: exact coherent target writes await the native publication barrier and never change surrounding samples. */
	return 0;
}
