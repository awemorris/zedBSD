/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* FIFO execution clears only the exact colour render area before supervised native bin/render launch; callback completion never retires uncertain DMA. */
#include <kern/clock.h>
#include <kern/device-io.h>
#include <kern/klog.h>
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/bcm2711-private.h"
#include "drivers/gpu/bcm2711/native-colour.h"
#include "drivers/gpu/bcm2711/vk/vulkan-native-pass.h"

/* The most tile boxes a pass records as writes of their own; more, and its area is recorded. */
#define NATIVE_EXECUTE_WRITTEN_BOXES 4U

static int validate_pass(struct bcm2711_vulkan_native_pass *pass);
static int clear_target(struct bcm2711_vulkan_native_pass *pass);
static int convert_textures(struct bcm2711_vulkan_native_pass *pass, struct bcm2711_v3d_job_result *result, uint64_t *ticks, uint32_t *jobs, uint64_t *longest);

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

	/* The three parts in order under the caller's mutex. */
	error = bcm2711_vulkan_native_pass_start(pass, result);
	if (error != 0)
		return error;
	error = bcm2711_vulkan_native_pass_launch(pass, result);
	error = bcm2711_vulkan_native_pass_finish(pass, result, error);
	if (error != 0)
		return error;

	/* Succeeded: the pass ran and its output is published. */
	return 0;
}

/*
 * Validates the pass and performs its CPU clear before launch.
 *
 * Nothing has launched when it fails, so result->retired stays true.
 */
int
bcm2711_vulkan_native_pass_start(
	struct bcm2711_vulkan_native_pass *pass,
	struct bcm2711_v3d_job_result *result)
{
	uint64_t started;
	uint64_t elapsed;
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
	started = clock_ticks();
	error = validate_pass(pass);
	if (error != 0)
		return error;

	/* Exact clear conversion is fallible only before publication; ordinary load/discard never changes CPU target bytes. */
	error = clear_target(pass);
	if (error != 0)
		return error;

	/* Identify expensive CPU clear without confusing it with native queue execution. */
	elapsed = kern_ticks_to_ms(clock_ticks() - started);
	if (elapsed >= 100U) {
		kern_logf(
			"v3d: CPU target ms=%llu size=%ux%u area=%ux%u load=%u\n",
			(unsigned long long)elapsed,
			pass->state.width,
			pass->state.height,
			pass->area.extent.width,
			pass->area.extent.height,
			pass->state.load);
	}

	/* The single-use root and every coherent CPU write precede the native runner's supervised launch. */
	pass->executed = true;
	pass->retired = false;
	pass->converted = false;
	kern_io_write_barrier();

	/* Succeeded: the pass may launch. */
	return 0;
}

/*
 * Runs the pass's texture conversions and its bin/render job.
 *
 * Inside the executor's window this runs without the controller mutex, so
 * it writes only the pass and the result; the finish publishes the rest.
 */
int
bcm2711_vulkan_native_pass_launch(
	struct bcm2711_vulkan_native_pass *pass,
	struct bcm2711_v3d_job_result *result)
{
	uint64_t converted;
	uint64_t longest;
	uint32_t jobs;
	int error;

	/* The texture formatting unit fills every converted texture before any draw samples it. */
	error = convert_textures(pass, result, &converted, &jobs, &longest);
	pass->retired = result->retired;
	if (error != 0)
		return error;
	pass->converted = true;

	/* The draws run once all their textures are in place. */
	error = bcm2711_v3d_job_run(pass->space->native, &pass->job, result);
	pass->retired = result->retired;
	result->tfu_ticks = converted;
	result->tfu_jobs = jobs;
	result->tfu_max_ticks = longest;
	if (error != 0)
		return error;

	/* Succeeded: the jobs ran; the finish checks retirement. */
	return 0;
}

/*
 * Publishes a launched pass's output under the controller mutex.
 *
 * error is the launch's.  The output may have changed even when it failed.
 */
int
bcm2711_vulkan_native_pass_finish(
	struct bcm2711_vulkan_native_pass *pass,
	struct bcm2711_v3d_job_result *result,
	int error)
{
	struct bcm2711_vulkan_native_draw *draw;
	struct bcm2711_buffer *output;
	uint64_t offset;
	uint32_t box[4];
	uint32_t tiles[4];
	uint32_t index;
	bool written;

	/* Snapshots the unit filled are valid for later passes from now on. */
	if (pass->converted) {
		for (draw = pass->first; draw != NULL; draw = draw->next) {
			for (index = 0; index < draw->texture_count; index++) {
				if (draw->textures[index].convert && draw->textures[index].snapshot != NULL)
					bcm2711_native_snapshot_filled(draw->textures[index].snapshot);
			}
		}
	}

	/* Launched or not, the render area of the output may have changed, so snapshots and copies of it are stale. */
	output = pass->output->buffer;
	offset = (uint64_t)pass->state.output - pass->output->address;
	box[0] = (uint32_t)pass->area.offset.x;
	box[1] = (uint32_t)pass->area.offset.y;
	box[2] = box[0] + pass->area.extent.width;
	box[3] = box[1] + pass->area.extent.height;

	/*
	 * A completed pass narrowed to a few tile boxes changed only them:
	 * each is recorded alone, so that a later copy takes the
	 * boxes and not the area around them.  A failed one, a copy, or one of
	 * more boxes records its area.
	 */
	written = false;
	if (error == 0 &&
	    pass->state.source == 0 &&
	    pass->state.box_count != 0 &&
	    pass->state.box_count <= NATIVE_EXECUTE_WRITTEN_BOXES)
		written = true;
	if (written) {
		for (index = 0; index < pass->state.box_count; index++) {
			/* The box's tiles in pixels, inside the render area. */
			tiles[0] = pass->state.boxes[index][0] * 64U;
			tiles[1] = pass->state.boxes[index][1] * 64U;
			tiles[2] = (pass->state.boxes[index][2] + 1U) * 64U;
			tiles[3] = (pass->state.boxes[index][3] + 1U) * 64U;
			if (tiles[0] < box[0])
				tiles[0] = box[0];
			if (tiles[1] < box[1])
				tiles[1] = box[1];
			if (tiles[2] > box[2])
				tiles[2] = box[2];
			if (tiles[3] > box[3])
				tiles[3] = box[3];
			bcm2711_v3d_memory_written(output, offset, tiles);
		}
	}

	/* Any other pass: its whole area. */
	if (!written)
		bcm2711_v3d_memory_written(output, offset, box);
	if (error != 0)
		return error;

	/* A completed whole-image copy leaves its output equal to the source at the copied generation. */
	if (pass->sync && result->retired) {
		output->synced_identity = pass->sync_identity;
		output->synced_offset = pass->sync_offset;
		output->synced_target_offset = offset;
		output->synced_source_generation = pass->sync_generation;
		output->synced_generation = output->generation;
	}

	/* A successful callback without actual retired DMA cannot permit source reuse or native owner release. */
	if (!result->retired)
		return EIO;

	/* Completion and GPU output clean precede a following FIFO CPU read or independently staged sampled texture. */
	kern_io_read_barrier();

	/* The completed implicit render-pass transition becomes FIFO-visible only after successful native output retirement. */
	bcm2711_vulkan_image_set_level_layout(pass->target, pass->target_level, pass->final_layout);

	/* The depth/stencil attachment's transition is committed with it. */
	if (pass->depth_target != NULL)
		pass->depth_target->layout = pass->depth_final_layout;

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
	VkImageLayout current;
	uint32_t index;
	uint64_t offset;
	uint64_t bytes;

	/* Device admission and whole native ownership are prerequisites for any CPU target mutation. */
	if (pass->space == NULL ||
	    pass->space->native == NULL ||
	    pass->output == NULL ||
	    pass->cpu == NULL)
		return EINVAL;

	/* Deferred translations are not executable until the batch owner proves publication. */
	if (pass->space->deferred)
		return EBUSY;

	/* Faulted or stopped native hardware refuses before coherent CPU clear changes target contents. */
	if (!pass->space->native->hardware.initialized ||
	    !pass->space->native->hardware.ready ||
	    !pass->space->native->power.ready)
		return EIO;

	/* A pass's explicit initial layout must match current FIFO state unless the application intentionally discards its earlier contents. */
	if (pass->target == NULL)
		return EINVAL;
	current = bcm2711_vulkan_image_level_layout(pass->target, pass->target_level);
	if (pass->initial_layout != VK_IMAGE_LAYOUT_UNDEFINED && pass->initial_layout != current)
		return EINVAL;

	/* A loaded or stored depth/stencil image starts in the layout the pass names, unless the pass discards it. */
	if (pass->depth_target != NULL &&
	    pass->depth_initial_layout != VK_IMAGE_LAYOUT_UNDEFINED &&
	    pass->depth_initial_layout != pass->depth_target->layout)
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

	/* A loaded or stored depth/stencil image stays mapped and covers every row its tiles move. */
	if (pass->depth != NULL) {
		if (pass->depth->quarantined || pass->depth->references == 0)
			return EIO;
		offset = (uint64_t)pass->state.depth_output - pass->depth->address;
		bytes = (uint64_t)pass->state.depth_pitch * pass->state.height;
		if (offset > pass->depth->bytes ||
		    pass->state.depth_bytes > pass->depth->bytes - offset ||
		    bytes > pass->state.depth_bytes)
			return EINVAL;
	}

	/* A TLB copy's source stays mapped and covers every row its tile loads read. */
	if (pass->source != NULL) {
		if (pass->source->quarantined || pass->source->references == 0)
			return EIO;
		offset = (uint64_t)pass->state.source - pass->source->address;
		bytes = (uint64_t)pass->state.source_pitch * pass->state.height;
		if (offset > pass->source->bytes ||
		    pass->state.source_bytes > pass->source->bytes - offset ||
		    bytes > pass->state.source_bytes)
			return EINVAL;
	}

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

		/* Lent snapshot pixels stay mapped while this draw holds its use. */
		for (index = 0; index < draw->texture_count; index++) {
			/* Private pixels were checked among the owned storage above. */
			if (draw->textures[index].snapshot == NULL)
				continue;
			storage = draw->textures[index].pixels;
			if (storage == NULL ||
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

	/* Complete-image clears are encoded in tile memory and need no CPU writes. */
	if (pass->state.load == 0)
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

/*
 * Runs the texture formatting unit for each texture the pass's draws convert.
 *
 * A conversion that fails after launch leaves result->retired false, so the
 * caller keeps the whole pass for checked recovery like a failed CL job.
 */
static int
convert_textures(
	struct bcm2711_vulkan_native_pass *pass,
	struct bcm2711_v3d_job_result *result,
	uint64_t *ticks,
	uint32_t *jobs,
	uint64_t *longest)
{
	struct bcm2711_vulkan_native_draw *draw;
	struct bcm2711_v3d_job job;
	uint64_t started;
	uint64_t step;
	uint32_t index;
	uint32_t level;
	int error;

	/* Nothing has launched yet, so a pass without conversions retires as before. */
	*ticks = 0;
	*jobs = 0;
	*longest = 0;
	result->retired = true;
	kern_memset(&job, 0, sizeof(job));
	job.kind = BCM2711_V3D_JOB_TFU;

	/* Converts the textures in draw order; shared textures belong to the first draw only. */
	started = clock_ticks();
	for (draw = pass->first; draw != NULL; draw = draw->next) {
		for (index = 0; index < draw->texture_count; index++) {
			/* A texture the CPU already converted needs no unit job. */
			if (!draw->textures[index].convert)
				continue;

			/* Each conversion completes before the next starts; one queue holds one job. */
			job.command.tfu = draw->textures[index].tfu;
			step = clock_ticks();
			error = bcm2711_v3d_job_run(pass->space->native, &job, result);
			if (error != 0)
				return error;

			/* The diagnostic count and the longest conversion. */
			step = clock_ticks() - step;
			(*jobs)++;
			if (step > *longest)
				*longest = step;

			/* A mip tree's smaller levels follow its level 0, each from its own raster. */
			for (level = 1; level < draw->textures[index].levels; level++) {
				error = bcm2711_vulkan_native_mip_job(&draw->textures[index], level, &job.command.tfu);
				if (error != 0)
					return error;
				step = clock_ticks();
				error = bcm2711_v3d_job_run(pass->space->native, &job, result);
				if (error != 0)
					return error;
				step = clock_ticks() - step;
				(*jobs)++;
				if (step > *longest)
					*longest = step;
			}
		}
	}
	*ticks = clock_ticks() - started;

	/* Succeeded: every converted texture is in memory for the following CL job. */
	return 0;
}
