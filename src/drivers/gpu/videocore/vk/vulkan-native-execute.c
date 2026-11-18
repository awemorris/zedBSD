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

#include "drivers/gpu/videocore/bcm2711-private.h"
#include "drivers/gpu/videocore/native-colour.h"
#include "drivers/gpu/videocore/vk/vulkan-native-pass.h"

/* The most tile boxes a pass records as writes of their own; more, and its area is recorded. */
#define NATIVE_EXECUTE_WRITTEN_BOXES 4U

static int validate_pass(struct bcm2711_vulkan_native_pass *pass);
static int validate_target(const struct bcm2711_vulkan_native_pass *pass, uint32_t slot);
static int clear_target(struct bcm2711_vulkan_native_pass *pass, uint32_t slot);
static int clear_rects(struct bcm2711_vulkan_native_pass *pass, uint32_t slot);
static int clear_box(const struct bcm2711_vulkan_native_pass *pass, uint32_t slot, const VkRect2D *rect, const uint32_t *words);
static void target_written(const struct bcm2711_vulkan_native_pass *pass, uint32_t slot, int error);
static int clear_depth_rects(struct bcm2711_vulkan_native_pass *pass);
static int convert_textures(struct bcm2711_vulkan_native_pass *pass, struct bcm2711_v3d_job_result *result);
static int convert_volume(struct bcm2711_vulkan_native_pass *pass, const struct bcm2711_vulkan_native_texture_copy *entry, struct bcm2711_v3d_job_result *result);

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
	uint32_t slot;
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
	for (slot = 0; slot < pass->state.targets; slot++) {
		error = clear_target(pass, slot);
		if (error != 0)
			return error;
	}

	/* The rectangles a vkCmdClearAttachments clears before this continued pass loads its tiles, depth/stencil first. */
	error = clear_depth_rects(pass);
	if (error != 0)
		return error;
	for (slot = 0; slot < pass->state.targets; slot++) {
		error = clear_rects(pass, slot);
		if (error != 0)
			return error;
	}

	/* Identify expensive CPU clear without confusing it with native queue execution. */
	elapsed = kern_ticks_to_ms(clock_ticks() - started);
	if (elapsed >= 100U) {
		kern_logf(
			"v3d: CPU target ms=%llu size=%ux%u area=%ux%u targets=%u\n",
			(unsigned long long)elapsed,
			pass->state.width,
			pass->state.height,
			pass->area.extent.width,
			pass->area.extent.height,
			pass->state.targets);
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
	int error;

	/* The texture formatting unit fills every converted texture before any draw samples it. */
	error = convert_textures(pass, result);
	pass->retired = result->retired;
	if (error != 0)
		return error;
	pass->converted = true;

	/* The draws run once all their textures are in place. */
	error = bcm2711_v3d_job_run(pass->space->native, &pass->job, result);
	pass->retired = result->retired;
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
	uint32_t index;

	/* Snapshots the unit filled are valid for later passes from now on. */
	if (pass->converted) {
		for (draw = pass->first; draw != NULL; draw = draw->next) {
			for (index = 0; index < draw->texture_count; index++) {
				if (draw->textures[index].convert && draw->textures[index].snapshot != NULL)
					bcm2711_native_snapshot_filled(draw->textures[index].snapshot);
			}
		}
	}

	/* Launched or not, the render area of each output may have changed, so snapshots and copies of it are stale. */
	for (index = 0; index < pass->state.targets; index++)
		target_written(pass, index, error);

	/* A stored depth/stencil image too: a later sampling of its depth or stencil converts it again. */
	if (pass->depth != NULL && pass->state.depth_store != 0)
		bcm2711_v3d_memory_written(pass->depth->buffer, (uint64_t)pass->state.depth_output - pass->depth->address, NULL);

	/* The storage buffers the draws' vertex programs may have stored into. */
	for (draw = pass->first; draw != NULL; draw = draw->next) {
		for (index = 0; index < draw->stored_count; index++)
			bcm2711_v3d_memory_written(draw->stored[index], draw->stored_offsets[index], NULL);
	}
	if (error != 0)
		return error;

	/* A completed whole-image copy leaves its output equal to the source at the copied generation. */
	if (pass->sync && result->retired && pass->targets[0].output != NULL) {
		output = pass->targets[0].output->buffer;
		offset = (uint64_t)pass->state.target[0].output - pass->targets[0].output->address;
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

	/* The completed implicit render-pass transitions become FIFO-visible only after successful native output retirement. */
	for (index = 0; index < pass->state.targets; index++) {
		if (pass->targets[index].image != NULL)
			bcm2711_vulkan_image_set_level_layout(pass->targets[index].image, pass->targets[index].level, pass->targets[index].layer, pass->targets[index].final_layout);
	}

	/* The depth/stencil attachment's layer's transition is committed with it. */
	if (pass->depth_target != NULL)
		bcm2711_vulkan_image_set_level_layout(pass->depth_target, 0, pass->depth_layer, pass->depth_final_layout);

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
	VkImageLayout current;
	uint32_t index;
	uint32_t expected;
	uint64_t offset;
	uint64_t bytes;
	int error;

	/* Device admission and whole native ownership are prerequisites for any CPU target mutation. */
	if (pass->space == NULL ||
	    pass->space->native == NULL)
		return EINVAL;

	/* Deferred translations are not executable until the batch owner proves publication. */
	if (pass->space->deferred)
		return EBUSY;

	/* Faulted or stopped native hardware refuses before coherent CPU clear changes target contents. */
	if (!pass->space->native->hardware.initialized ||
	    !pass->space->native->hardware.ready ||
	    !pass->space->native->power.ready)
		return EIO;

	/* Each colour target's image, layout and mapping. */
	if (pass->state.targets > BCM2711_NATIVE_PASS_TARGETS)
		return EINVAL;
	for (index = 0; index < pass->state.targets; index++) {
		error = validate_target(pass, index);
		if (error != 0)
			return error;
	}

	/* A loaded or stored depth/stencil image's layer starts in the layout the pass names, unless the pass discards it. */
	if (pass->depth_target != NULL) {
		current = bcm2711_vulkan_image_level_layout(pass->depth_target, 0, pass->depth_layer);
		if (pass->depth_initial_layout != VK_IMAGE_LAYOUT_UNDEFINED && pass->depth_initial_layout != current)
			return EINVAL;
	}

	/* The framebuffer has the bounded raster geometry. */
	if (pass->state.width == 0 ||
	    pass->state.width > 4096U ||
	    pass->state.height == 0 ||
	    pass->state.height > 4096U)
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
		bytes = (uint64_t)pass->state.depth_pitch * pass->state.height * (pass->state.msaa + 1U);
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
		bytes = (uint64_t)pass->state.source_pitch * pass->state.height * (pass->state.msaa + 1U);
		if (offset > pass->source->bytes ||
		    pass->state.source_bytes > pass->source->bytes - offset ||
		    bytes > pass->state.source_bytes)
			return EINVAL;
	}

	/* Every command/pool/state/overflow allocation remains live before the native runner's final mapping checks. */
	expected = BCM2711_VULKAN_PASS_LISTS;
	if (pass->occlusion != NULL)
		expected++;
	if (pass->count != expected || pass->job.kind != BCM2711_V3D_JOB_CL)
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

/* Clears exactly one colour target's copied render area through the retained coherent alias immediately before native execution. */
static int
clear_target(
	struct bcm2711_vulkan_native_pass *pass,
	uint32_t slot)
{
	/* Ordinary loading and discarded initial contents require no CPU clear; the native list still preserves out-of-area samples. */
	if (pass->targets[slot].output == NULL || pass->targets[slot].load != VK_ATTACHMENT_LOAD_OP_CLEAR)
		return 0;

	/* Complete-image clears are encoded in tile memory and need no CPU writes. */
	if (pass->state.target[slot].load == 0)
		return 0;

	/* Succeeded once the render area holds the clear colour. */
	return clear_box(pass, slot, &pass->area, pass->targets[slot].clear);
}

/*
 * Clears a continued pass's colour rectangles of one target through the retained coherent alias.
 *
 * The rectangles lie inside the render area, which validate_pass bounded
 * by the framebuffer and the image's rows.
 */
static int
clear_rects(
	struct bcm2711_vulkan_native_pass *pass,
	uint32_t slot)
{
	const struct bcm2711_vulkan_native_target *target;
	const VkRect2D *rect;
	uint32_t index;
	int error;

	/* A target without colour rectangles writes no colour. */
	target = &pass->targets[slot];
	if (target->output == NULL || target->rect_count == 0)
		return 0;

	/* Each rectangle, kept inside the render area when it was prepared. */
	for (index = 0; index < target->rect_count && index < BCM2711_VULKAN_CLEAR_RECTS; index++) {
		rect = &target->rects[index];
		if (rect->offset.x < pass->area.offset.x ||
		    rect->offset.y < pass->area.offset.y ||
		    (uint64_t)rect->offset.x + rect->extent.width > (uint64_t)pass->area.offset.x + pass->area.extent.width ||
		    (uint64_t)rect->offset.y + rect->extent.height > (uint64_t)pass->area.offset.y + pass->area.extent.height)
			return EINVAL;
		error = clear_box(pass, slot, rect, target->rect_clear);
		if (error != 0)
			return error;
	}

	/* Succeeded: the rectangles hold the colour before the tiles load them. */
	return 0;
}

/*
 * Writes a clear colour into a rectangle (inside the framebuffer) of one
 * colour target through its coherent alias, as one pixel of the target's
 * format repeated.
 */
static int
clear_box(
	const struct bcm2711_vulkan_native_pass *pass,
	uint32_t slot,
	const VkRect2D *rect,
	const uint32_t *words)
{
	const struct bcm2711_native_pass_target *native;
	const struct bcm2711_vulkan_native_target *target;
	uint8_t texel[16];
	uint8_t *pixel;
	uint8_t *row;
	uint32_t bytes;
	uint32_t index;
	uint32_t scale;
	uint32_t x;
	uint32_t y;
	int error;

	/* One pixel of the clear, packed as the target stores it. */
	target = &pass->targets[slot];
	native = &pass->state.target[slot];
	error = bcm2711_native_colour_texel(words, native->format, target->swap, texel, &bytes);
	if (error != 0)
		return error;
	if (bytes != native->texel)
		return EINVAL;

	/* A multisampled raster holds a pixel's samples as a 2x2 block: the rectangle is twice as wide and high in it. */
	scale = 1U;
	if (pass->state.msaa != 0 && native->resolve == 0)
		scale = 2U;

	/* Each bounded row starts inside the logical coherent image and writes only the rectangle's samples, never row padding. */
	row = (uint8_t *)target->cpu + (uint64_t)rect->offset.y * scale * native->pitch + (uint32_t)rect->offset.x * scale * bytes;
	for (y = 0; y < rect->extent.height * scale; y++) {
		pixel = row;
		for (x = 0; x < rect->extent.width * scale; x++) {
			/* Explicit bytes keep the storage order without an unaligned CPU word store. */
			for (index = 0; index < bytes; index++)
				pixel[index] = texel[index];
			pixel += bytes;
		}

		/* Pitch advances only when another checked row exists, never past the complete final allocation. */
		if (y + 1U < rect->extent.height * scale)
			row += native->pitch;
	}

	/* Succeeded: exact coherent target writes await the native publication barrier and never change surrounding samples. */
	return 0;
}

/*
 * Checks one colour target before a CPU clear or the launch: its image's
 * current layout, its live coherent mapping and the numerical interval and
 * CPU alias of its rows.
 */
static int
validate_target(
	const struct bcm2711_vulkan_native_pass *pass,
	uint32_t slot)
{
	const struct bcm2711_vulkan_native_target *target;
	const struct bcm2711_native_pass_target *native;
	struct bcm2711_buffer *buffer;
	VkImageLayout current;
	uint64_t offset;
	uint64_t bytes;
	uint32_t samples;

	/* An unused slot has no image. */
	target = &pass->targets[slot];
	native = &pass->state.target[slot];
	if (target->output == NULL) {
		if (native->output != 0)
			return EINVAL;
		return 0;
	}

	/* A pass's explicit initial layout must match current FIFO state unless the application intentionally discards its earlier contents. */
	if (target->image == NULL || target->cpu == NULL)
		return EINVAL;
	current = bcm2711_vulkan_image_level_layout(target->image, target->level, target->layer);
	if (target->initial_layout != VK_IMAGE_LAYOUT_UNDEFINED && target->initial_layout != current)
		return EINVAL;

	/* Actual output RAM stays coherent, mapped and independently referenced through every device write. */
	buffer = target->output->buffer;
	if (target->output->quarantined ||
	    target->output->references == 0 ||
	    buffer == NULL ||
	    !buffer->uncached ||
	    buffer->address == NULL)
		return EIO;

	/* Numerical output address and copied CPU alias must describe the same complete retained mapping interval. */
	if (native->output < target->output->address)
		return EINVAL;
	offset = (uint64_t)native->output - target->output->address;
	if (offset > buffer->bytes ||
	    native->output_bytes > buffer->bytes - offset ||
	    offset > target->output->bytes ||
	    native->output_bytes > target->output->bytes - offset)
		return EINVAL;

	/* The actual coherent CPU alias has exactly the same logical binding offset as the native target pointer. */
	if ((uint8_t *)buffer->address + offset != target->cpu)
		return EINVAL;

	/* Exact row pitch and full framebuffer rows stay inside the logical native image, including trailing row padding. */
	bytes = (uint64_t)native->pitch * pass->state.height;
	samples = 1U;
	if (pass->state.msaa != 0 && native->resolve == 0)
		samples = 2U;
	if (native->texel == 0 ||
	    native->pitch < pass->state.width * native->texel * samples ||
	    bytes * samples > native->output_bytes)
		return EINVAL;

	/* Succeeded: the target can be cleared and drawn into. */
	return 0;
}

/*
 * Records which part of one colour target's output a pass changed, so
 * snapshots and copies of it are stale.
 *
 * A completed pass narrowed to a few tile boxes changed only them: each
 * is recorded alone, so that a later copy takes the boxes and not the
 * area around them.  A failed one, a copy, or one of more boxes records
 * its area.
 */
static void
target_written(
	const struct bcm2711_vulkan_native_pass *pass,
	uint32_t slot,
	int error)
{
	struct bcm2711_buffer *output;
	uint64_t offset;
	uint32_t box[4];
	uint32_t tiles[4];
	uint32_t index;

	/* An unused slot, or one the tiles do not store, changed nothing. */
	if (pass->targets[slot].output == NULL || pass->state.target[slot].store == 0)
		return;
	output = pass->targets[slot].output->buffer;
	offset = (uint64_t)pass->state.target[slot].output - pass->targets[slot].output->address;
	box[0] = (uint32_t)pass->area.offset.x;
	box[1] = (uint32_t)pass->area.offset.y;
	box[2] = box[0] + pass->area.extent.width;
	box[3] = box[1] + pass->area.extent.height;

	/* A raster of all four samples changed somewhere the pixel boxes do not describe. */
	if (pass->state.msaa != 0 && pass->state.target[slot].resolve == 0) {
		bcm2711_v3d_memory_written(output, offset, NULL);
		return;
	}

	/* Each of a completed pass's few boxes, in pixels inside the render area. */
	if (error == 0 &&
	    pass->state.source == 0 &&
	    pass->state.box_count != 0 &&
	    pass->state.box_count <= NATIVE_EXECUTE_WRITTEN_BOXES) {
		for (index = 0; index < pass->state.box_count; index++) {
			tiles[0] = pass->state.boxes[index][0] * pass->state.tile_width;
			tiles[1] = pass->state.boxes[index][1] * pass->state.tile_height;
			tiles[2] = (pass->state.boxes[index][2] + 1U) * pass->state.tile_width;
			tiles[3] = (pass->state.boxes[index][3] + 1U) * pass->state.tile_height;
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
		return;
	}

	/* Any other pass: its whole area. */
	bcm2711_v3d_memory_written(output, offset, box);
}

/*
 * Writes a continued pass's depth/stencil rectangles through the image's coherent alias.
 *
 * Each 32-bit pixel keeps the bits depth_mask does not select.  The
 * rectangles lie inside the render area, and validate_pass bounded the
 * image's rows.
 */
static int
clear_depth_rects(
	struct bcm2711_vulkan_native_pass *pass)
{
	const VkRect2D *rect;
	uint8_t *pixel;
	uint8_t *row;
	uint32_t texel;
	uint32_t word;
	uint32_t index;
	uint32_t scale;
	uint32_t x;
	uint32_t y;

	/* A pass without depth/stencil rectangles writes nothing. */
	if (pass->depth_rect_count == 0)
		return 0;
	if (pass->depth == NULL || pass->depth_cpu == NULL)
		return EINVAL;

	/* A D16 pixel is two bytes, the others four; a multisampled image's pixel is a 2x2 block of samples. */
	texel = 4U;
	if (pass->state.depth == 3U)
		texel = 2U;
	scale = 1U;
	if (pass->state.msaa != 0)
		scale = 2U;

	/* Each rectangle's rows, a read and a write of each pixel's word. */
	for (index = 0; index < pass->depth_rect_count && index < BCM2711_VULKAN_CLEAR_RECTS; index++) {
		rect = &pass->depth_rects[index];
		if ((uint64_t)rect->offset.x + rect->extent.width > pass->state.width ||
		    (uint64_t)rect->offset.y + rect->extent.height > pass->state.height ||
		    pass->state.depth_pitch < pass->state.width * texel * scale)
			return EINVAL;
		row = (uint8_t *)pass->depth_cpu + (uint64_t)rect->offset.y * scale * pass->state.depth_pitch + (uint32_t)rect->offset.x * scale * texel;
		for (y = 0; y < rect->extent.height * scale; y++) {
			pixel = row;
			for (x = 0; x < rect->extent.width * scale; x++) {
				/* The bits not cleared stay; explicit bytes avoid an unaligned word access. */
				word = (uint32_t)pixel[0] | (uint32_t)pixel[1] << 8;
				if (texel == 4U)
					word |= (uint32_t)pixel[2] << 16 | (uint32_t)pixel[3] << 24;
				word = (word & ~pass->depth_mask) | (pass->depth_value & pass->depth_mask);
				pixel[0] = (uint8_t)word;
				pixel[1] = (uint8_t)(word >> 8);
				if (texel == 4U) {
					pixel[2] = (uint8_t)(word >> 16);
					pixel[3] = (uint8_t)(word >> 24);
				}
				pixel += texel;
			}

			/* The next row, only while one remains. */
			if (y + 1U < rect->extent.height * scale)
				row += pass->state.depth_pitch;
		}
	}

	/* Succeeded: the rectangles hold the values before the tiles load them. */
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
	struct bcm2711_v3d_job_result *result)
{
	struct bcm2711_vulkan_native_draw *draw;
	struct bcm2711_vulkan_native_texture_copy *entry;
	struct bcm2711_v3d_job job;
	uint32_t index;
	uint32_t level;
	uint32_t layer;
	uint32_t layers;
	int error;

	/* Nothing has launched yet, so a pass without conversions retires as before. */
	result->retired = true;
	kern_memset(&job, 0, sizeof(job));
	job.kind = BCM2711_V3D_JOB_TFU;

	/* Converts the textures in draw order; shared textures belong to the first draw only. */
	for (draw = pass->first; draw != NULL; draw = draw->next) {
		for (index = 0; index < draw->texture_count; index++) {
			/* A texture the CPU already converted needs no unit job. */
			entry = &draw->textures[index];
			if (!entry->convert)
				continue;

			/* A 3D tree's slices are placed level by level. */
			if (entry->volume && entry->levels > 1U) {
				error = convert_volume(pass, entry, result);
				if (error != 0)
					return error;
				continue;
			}

			/* Each layer is converted by the same jobs, moved to the layer in both the raster and the pixels. */
			layers = entry->layers;
			if (layers == 0)
				layers = 1;
			for (layer = 0; layer < layers; layer++) {
				/* Each conversion completes before the next starts; one queue holds one job. */
				job.command.tfu = entry->tfu;
				job.command.tfu.input += (uint32_t)(layer * entry->input_stride);
				job.command.tfu.output += layer * entry->output_stride;
				error = bcm2711_v3d_job_run(pass->space->native, &job, result);
				if (error != 0)
					return error;

				/* A mip tree's smaller levels follow its level 0, each from its own raster. */
				for (level = 1; level < entry->levels; level++) {
					error = bcm2711_vulkan_native_mip_job(entry, level, &job.command.tfu);
					if (error != 0)
						return error;
					job.command.tfu.input += (uint32_t)(layer * entry->input_stride);
					job.command.tfu.output += layer * entry->output_stride;
					error = bcm2711_v3d_job_run(pass->space->native, &job, result);
					if (error != 0)
						return error;
				}
			}
		}
	}

	/* Succeeded: every converted texture is in memory for the following CL job. */
	return 0;
}

/* Converts every slice of every level of a 3D mip tree, one unit job each, after the jobs before it. */
static int
convert_volume(
	struct bcm2711_vulkan_native_pass *pass,
	const struct bcm2711_vulkan_native_texture_copy *entry,
	struct bcm2711_v3d_job_result *result)
{
	struct bcm2711_v3d_job job;
	uint32_t level;
	uint32_t slice;
	uint32_t slices;
	int error;

	/* Each level's slices, from level 0's; the job of a slice reports how many the level has. */
	kern_memset(&job, 0, sizeof(job));
	job.kind = BCM2711_V3D_JOB_TFU;
	for (level = 0; level < entry->levels; level++) {
		slices = 1;
		for (slice = 0; slice < slices; slice++) {
			error = bcm2711_vulkan_native_volume_job(entry, level, slice, &job.command.tfu, &slices);
			if (error != 0)
				return error;
			error = bcm2711_v3d_job_run(pass->space->native, &job, result);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the whole tree is in memory for the following CL job. */
	return 0;
}
