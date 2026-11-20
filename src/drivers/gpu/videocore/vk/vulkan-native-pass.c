/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Native FIFO pass preparation owns all allocations before list publication, with no MMIO launch or target mutation. */
#include <kern/clock.h>
#include <kern/dcache.h>
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/videocore/bcm2711-private.h"
#include "drivers/gpu/videocore/vk/vulkan-native-pass.h"
#include "drivers/gpu/videocore/native-colour.h"

/* All padded native inputs across the enclosing submission share one finite staging budget. */
#define NATIVE_PASS_BYTES (256ULL * 1024U * 1024U)

/* The tile-list bytes reserved for each vertex or index a pass's draws read (a triangle's three: six bytes). */
#define NATIVE_PASS_LIST_BYTES 2U

static int prepare_clear(struct bcm2711_vulkan_native_pass *pass, const struct bcm2711_vulkan_record *record, uint32_t subresource);
static int prepare_target(struct bcm2711_vulkan_native_pass *pass, const struct bcm2711_vulkan_prepared_event *begin, bool split);
static int prepare_colour(struct bcm2711_vulkan_native_pass *pass, const struct bcm2711_vulkan_pass *description, const struct bcm2711_vulkan_framebuffer *framebuffer, const struct bcm2711_vulkan_record *record, uint32_t slot, bool continued, bool split);
static int prepare_target_clear(struct bcm2711_vulkan_native_pass *pass, uint32_t slot);
static int prepare_depth(struct bcm2711_vulkan_native_pass *pass, const struct bcm2711_vulkan_pass *description, const struct bcm2711_vulkan_framebuffer *framebuffer, const struct bcm2711_vulkan_record *record, bool continued, bool split);
static int prepare_depth_resolve(struct bcm2711_vulkan_native_pass *pass, const struct bcm2711_vulkan_pass *description, const struct bcm2711_vulkan_framebuffer *framebuffer, const struct bcm2711_vulkan_record *record);
static int prepare_attachments(struct bcm2711_vulkan_native_pass *pass, const struct bcm2711_vulkan_record *clear);
static int prepare_depth_clear(struct bcm2711_vulkan_native_pass *pass, const struct bcm2711_vulkan_record *clear, uint32_t aspects, bool whole);
static uint32_t cut_rect(const struct bcm2711_vulkan_native_pass *pass, const uint32_t *rect, VkRect2D *rects, uint32_t count);
static uint32_t depth_unorm(uint32_t bits, uint32_t width);
static bool pass_split(const struct bcm2711_vulkan_prepared_event *begin);
static bool pass_inside(const struct bcm2711_vulkan_prepared_event *event);
static uint32_t pass_samples(const struct bcm2711_vulkan_prepared_event *begin);
static int depth_clear_layer(struct bcm2711_vulkan_resource *image, uint64_t offset, uint32_t texel, uint32_t mask, uint32_t value);
static int occlusion_counter(struct bcm2711_vulkan_native_pass *pass, const struct bcm2711_vulkan_prepared_event *event, uint32_t *counter);
static int collect_draws(struct bcm2711_vulkan_native_pass *pass, const struct bcm2711_vulkan_prepared_event *begin, uint64_t *remaining, const struct bcm2711_vulkan_prepared_event **next);
static int allocate_storage(struct bcm2711_vulkan_native_pass *pass, uint64_t bytes, unsigned fill, uint64_t *remaining, struct bcm2711_native_storage **storage);
static int prepare_lists(struct bcm2711_vulkan_native_pass *pass, uint64_t *remaining);
static uint32_t overflow_bytes(const struct bcm2711_vulkan_native_pass *pass);
static void select_boxes(struct bcm2711_vulkan_native_pass *pass);
static bool box_add(struct bcm2711_native_pass *state, const uint32_t *box);
static void prepare_image(struct bcm2711_native_storage *storage, struct bcm2711_native_pass_image *image);

/*
 * Prepares one whole native graphics pass only after preceding FIFO writes retire.
 *
 * Every consumed input, command byte, binning/overflow allocation and output
 * view is independently owned.  The parent submission retains its pending
 * prepared primary.  An error unwinds the entire unpublished prefix without
 * changing the target or caller budget.  Partial clear is deferred to native
 * execution; loading each selected tile preserves samples outside its exact
 * render area, including neighbouring tiles in a grouped supertile.
 *
 * A vkCmdClearAttachments inside the render pass ends one native pass and
 * starts the next: the first stores every attachment, the clear event
 * begins the second, which clears and then loads them.
 */
int
bcm2711_vulkan_native_pass_create(
	struct bcm2711_v3d_space *space,
	const struct bcm2711_vulkan_prepared_event *begin,
	uint32_t view,
	uint64_t *available,
	struct bcm2711_vulkan_native_pass **pass,
	const struct bcm2711_vulkan_prepared_event **next)
{
	struct bcm2711_vulkan_native_pass *created;
	const struct bcm2711_vulkan_prepared_event *following;
	const char *stage;
	uint64_t remaining;
	bool split;
	int error;
	int released;

	/* No partially acquired pass or event cursor may escape on refusal. */
	if (pass == NULL || next == NULL)
		return EINVAL;
	*pass = NULL;
	*next = NULL;

	/* Immutable prepared pass events and a ready native owner supply all lifetime prerequisites. */
	if (space == NULL ||
	    space->native == NULL ||
	    begin == NULL ||
	    (begin->opcode != GPU_OP_CMD_BEGIN_RENDER_PASS &&
	     begin->opcode != GPU_OP_CMD_CLEAR_ATTACHMENTS) ||
	    begin->pass == NULL)
		return EINVAL;

	/* Only the existing submission-wide padded staging budget can authorize native allocations. */
	if (available == NULL || *available > NATIVE_PASS_BYTES)
		return EINVAL;
	remaining = *available;
	following = NULL;
	created = kern_calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->space = space;
	created->job.kind = BCM2711_V3D_JOB_CL;
	created->view = view;

	/* Complete output ownership precedes every draw or command-storage allocation; a pass a clear ends stores everything. */
	stage = "target";
	split = pass_split(begin);
	error = prepare_target(created, begin, split);

	/* Draw inputs are copied only after the independent framebuffer hold has succeeded. */
	if (error == 0) {
		stage = "draws";
		error = collect_draws(created, begin, &remaining, &following);
	}

	/* The tiles the draws can change, when the pass may leave the others unrendered. */
	if (error == 0)
		select_boxes(created);

	/* Native command storage follows the complete independent draw prefix. */
	if (error == 0) {
		stage = "lists";
		error = prepare_lists(created, &remaining);
	}

	/* Reports a refused preparation before teardown can replace its original failure. */
	if (error != 0) {
		kern_logf("v3d: pass prepare %s failed (%d) draws=%u bytes=%llu\n",
			  stage,
			  error,
			  created->draws,
			  (unsigned long long)created->bytes);
	}

	/* Preparation never launched DMA; each owner retires once even when a late translation teardown fails. */
	if (error != 0) {
		released = bcm2711_vulkan_native_pass_release(&created, true);
		if (released != 0)
			return released;
		return error;
	}

	/* The caller receives one complete numerical native job and the next borrowed immutable preparation cursor. */
	*available = remaining;
	*pass = created;
	*next = following;

	/* Succeeded: every possible device reference belongs to this root or its linked independent draw roots. */
	return 0;
}

/*
 * The views of the render pass a native pass begins at begin: the
 * multiview mask of the render pass its begin record names, zero for one
 * view or for an event of no render pass.
 */
uint32_t
bcm2711_vulkan_native_pass_views(
	const struct bcm2711_vulkan_prepared_event *begin)
{
	const struct bcm2711_vulkan_record *record;
	const struct bcm2711_vulkan_pass *description;

	/* The render pass the begin record names. */
	if (begin == NULL || begin->pass == NULL)
		return 0;
	if (begin->opcode != GPU_OP_CMD_BEGIN_RENDER_PASS && begin->opcode != GPU_OP_CMD_CLEAR_ATTACHMENTS)
		return 0;
	record = begin->pass;
	if (record->opcode != GPU_OP_CMD_BEGIN_RENDER_PASS ||
	    record->objects[0] == NULL ||
	    record->objects[0]->kind != I915_VK_OBJ_RENDER_PASS)
		return 0;
	description = record->objects[0]->payload;
	if (description == NULL)
		return 0;

	/* Succeeded: the subpass's views. */
	return description->view_mask;
}

/*
 * Finds the subresource a colour image clear writes in its pass number index.
 *
 * The clear writes one level of one layer (a 3D image's slice) a pass, in
 * the order of its ranges, each range's levels in order and each level's
 * layers (or slices) in order.  ENOENT when the clear has fewer passes.
 */
int
bcm2711_vulkan_native_clear_subresource(
	const struct bcm2711_vulkan_record *record,
	uint32_t index,
	uint32_t *level,
	uint32_t *layer)
{
	struct bcm2711_vulkan_resource *image;
	uint32_t range;
	uint32_t first_level;
	uint32_t levels;
	uint32_t first_layer;
	uint32_t layers;
	uint32_t step;
	uint32_t rasters;
	int error;

	/* Only a validated colour image clear has ranges. */
	if (record->opcode != GPU_OP_CMD_CLEAR_COLOR_IMAGE ||
	    record->objects[0] == NULL ||
	    record->objects[0]->kind != I915_VK_OBJ_IMAGE ||
	    record->count > BCM2711_VULKAN_CLEAR_RANGES)
		return EINVAL;
	image = record->objects[0]->payload;

	/* Walk the ranges, each level of a range clearing its layers or its slices. */
	for (range = 0; range < record->count; range++) {
		error = bcm2711_vulkan_image_clear_range(image, record->words[BCM2711_VULKAN_CLEAR_RANGE_WORD + range], &first_level, &levels, &first_layer, &layers);
		if (error != 0)
			return error;

		/* A level's passes are its layers, or a 3D image's slices at the level. */
		for (step = 0; step < levels; step++) {
			rasters = layers;
			if (image->depth != 0)
				rasters = bcm2711_vulkan_image_slices(image, first_level + step);

			/* Succeeded: the pass falls in this level. */
			if (index < rasters) {
				*level = first_level + step;
				*layer = first_layer + index;
				return 0;
			}

			/* The passes of this level come before the one sought. */
			index -= rasters;
		}
	}

	/* The clear has no pass of that number. */
	return ENOENT;
}

/*
 * Prepares an independently owned native clear of one subresource without CPU framebuffer mutation.
 *
 * A zero-draw bin list and native tile clear/store clear the level of the
 * layer (or slice) the clear's pass number subresource writes
 * (bcm2711_vulkan_native_clear_subresource).  The prepared primary retains
 * the logical image; this root independently retains its exact backing
 * view through uncertain native completion, using the ordinary whole-pass
 * retirement mechanism.
 */
int
bcm2711_vulkan_native_clear_create(
	struct bcm2711_v3d_space *space,
	const struct bcm2711_vulkan_prepared_event *event,
	uint32_t subresource,
	uint64_t *available,
	struct bcm2711_vulkan_native_pass **pass)
{
	struct bcm2711_vulkan_native_pass *created;
	uint64_t remaining;
	int error;
	int released;

	/* Every refusal leaves the caller's native owner and budget unchanged. */
	if (pass == NULL)
		return EINVAL;
	*pass = NULL;

	/* Only an exact prepared clear and native address-space owner supply the complete input graph. */
	if (space == NULL ||
	    space->native == NULL ||
	    event == NULL ||
	    event->record == NULL ||
	    event->opcode != GPU_OP_CMD_CLEAR_COLOR_IMAGE)
		return EINVAL;

	/* Native storage cannot exceed the enclosing submission budget. */
	if (available == NULL || *available > NATIVE_PASS_BYTES)
		return EINVAL;

	/* One CPU root owns all independently retained output and list allocations before publication. */
	remaining = *available;
	created = kern_calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->space = space;
	created->job.kind = BCM2711_V3D_JOB_CL;
	error = prepare_clear(created, event->record, subresource);
	if (error == 0)
		error = prepare_lists(created, &remaining);

	/* No native work launched, so every complete unpublished prefix can retire without changing the output. */
	if (error != 0) {
		released = bcm2711_vulkan_native_pass_release(&created, true);
		if (released != 0)
			return released;
		return error;
	}

	/* Publication consumes only successfully prepared padded storage, exactly as an ordinary graphics pass does. */
	*available = remaining;
	*pass = created;

	/* Succeeded: native tile clear/store owns every eventual DMA reference and reads no user graphics state. */
	return 0;
}

/*
 * Builds the complete native lists for an unpublished independently owned graphics or internal transfer pass.
 */
int
bcm2711_vulkan_native_pass_build_lists(
	struct bcm2711_vulkan_native_pass *pass,
	uint64_t *available)
{
	uint64_t remaining;
	int error;

	/* Only a fresh native root with an exact independent output may acquire command and overflow storage. */
	if (pass == NULL ||
	    pass->space == NULL ||
	    pass->count != 0 ||
	    pass->executed)
		return EINVAL;
	if (available == NULL || *available > NATIVE_PASS_BYTES)
		return EINVAL;

	/* A partial allocation failure keeps every mapping on the root while leaving the enclosing caller budget unchanged. */
	remaining = *available;
	error = prepare_lists(pass, &remaining);
	if (error != 0)
		return error;
	*available = remaining;

	/* Succeeded: all lists are cleaned and every native address belongs to the same complete unpublished pass. */
	return 0;
}

/*
 * Fills a native target's tile buffer fields for a colour attachment format.
 *
 * The output format, internal type, bpp and clamp and the raster's bytes a
 * pixel follow the format's colour target description.
 */
int
bcm2711_vulkan_native_target_format(
	struct bcm2711_native_pass_target *target,
	VkFormat format)
{
	struct bcm2711_vulkan_colour_target colour;
	int error;

	/* A colour attachment format. */
	error = bcm2711_vulkan_colour_target(format, &colour);
	if (error != 0)
		return error;

	/* Succeeded: the target's place in the tile buffer. */
	target->format = colour.output;
	target->type = colour.type;
	target->bpp = colour.bpp;
	target->clamp = colour.clamp;
	target->texel = colour.texel;
	return 0;
}

/*
 * Chooses a pass's tile size from its targets, then the rectangle of the
 * tiles its render area covers.
 */
int
bcm2711_vulkan_native_pass_tiles(
	struct bcm2711_vulkan_native_pass *pass)
{
	uint32_t end_x;
	uint32_t end_y;
	int error;

	/* The tile size the targets leave room for; the engine refuses targets it does not draw into. */
	error = bcm2711_native_pass_tile(&pass->state);
	if (error != 0)
		return error;

	/* A nonempty render area of the framebuffer. */
	if (pass->area.offset.x < 0 ||
	    pass->area.offset.y < 0 ||
	    pass->area.extent.width == 0 ||
	    pass->area.extent.height == 0)
		return EINVAL;

	/* Succeeded: the tiles from the area's first pixel to its last. */
	end_x = (uint32_t)pass->area.offset.x + pass->area.extent.width - 1U;
	end_y = (uint32_t)pass->area.offset.y + pass->area.extent.height - 1U;
	pass->state.first_x = (uint32_t)pass->area.offset.x / pass->state.tile_width;
	pass->state.last_x = end_x / pass->state.tile_width;
	pass->state.first_y = (uint32_t)pass->area.offset.y / pass->state.tile_height;
	pass->state.last_y = end_y / pass->state.tile_height;
	return 0;
}

/*
 * Retires a whole native pass only after no-launch, completion or checked global reset proves DMA stopped.
 */
int
bcm2711_vulkan_native_pass_release(
	struct bcm2711_vulkan_native_pass **pass,
	bool retired)
{
	struct bcm2711_vulkan_native_pass *owned;
	struct bcm2711_vulkan_native_draw *draw;
	struct bcm2711_vulkan_native_draw *next;
	uint32_t index;
	int first;
	int error;

	/* A missing pointer cannot express explicit ownership consumption. */
	if (pass == NULL)
		return EINVAL;
	owned = *pass;
	if (owned == NULL)
		return 0;

	/* Callback completion never proves the device has stopped reading any input or writing the framebuffer. */
	if (!retired)
		return EBUSY;

	/* The whole root is consumed once; failed mapping teardown is held independently by the native address space. */
	*pass = NULL;
	first = 0;
	draw = owned->first;
	while (draw != NULL) {
		/* Preserve the linked successor before consuming each complete independent draw. */
		next = draw->next;
		error = bcm2711_vulkan_native_draw_release(&draw, true);
		if (first == 0 && error != 0)
			first = error;
		draw = next;
	}
	error = bcm2711_vulkan_native_draw_pool_release(&owned->draw_pool);
	if (first == 0 && error != 0)
		first = error;

	/* Every command/tile/overflow owner retires even if an earlier draw teardown failed. */
	for (index = 0; index < owned->count; index++) {
		error = bcm2711_native_storage_release(owned->space, &owned->storage[index], true);
		if (first == 0 && error != 0)
			first = error;
	}

	/* The independent framebuffer holds remain through all referenced native-input retirement. */
	for (index = 0; index < BCM2711_NATIVE_PASS_TARGETS; index++) {
		if (owned->targets[index].output == NULL)
			continue;
		error = bcm2711_v3d_memory_release(owned->space, owned->targets[index].output);
		if (first == 0 && error != 0)
			first = error;
	}

	/* A TLB copy's source hold retires with the output, after the copy's DMA stopped. */
	if (owned->source != NULL) {
		error = bcm2711_v3d_memory_release(owned->space, owned->source);
		if (first == 0 && error != 0)
			first = error;
	}

	/* A depth/stencil image's hold retires with the output, after the tiles that moved it stopped. */
	if (owned->depth != NULL) {
		error = bcm2711_v3d_memory_release(owned->space, owned->depth);
		if (first == 0 && error != 0)
			first = error;
	}
	if (owned->resolve != NULL) {
		error = bcm2711_v3d_memory_release(owned->space, owned->resolve);
		if (first == 0 && error != 0)
			first = error;
	}

	/* Only the consumed CPU root disappears; uncertain failed-unmap storage remains native-space owned. */
	kern_free(owned);
	if (first != 0)
		return first;

	/* Succeeded: all independent references of the complete pass have retired. */
	return 0;
}

/* Retains every colour target's coherent output view and copies immutable numerical area/lifecycle state without modifying any target sample. */
static int
prepare_target(
	struct bcm2711_vulkan_native_pass *pass,
	const struct bcm2711_vulkan_prepared_event *begin,
	bool split)
{
	const struct bcm2711_vulkan_record *record;
	const struct bcm2711_vulkan_pass *description;
	const struct bcm2711_vulkan_framebuffer *framebuffer;
	uint32_t slot;
	bool continued;
	int error;

	/* The retained primary supplies exact typed pass/framebuffer ownership rather than a public-handle relookup. */
	record = begin->pass;
	if (record->opcode != GPU_OP_CMD_BEGIN_RENDER_PASS ||
	    record->objects[0] == NULL ||
	    record->objects[1] == NULL)
		return EINVAL;

	/* The targets require complete retained immutable descriptions. */
	if (record->objects[0]->kind != I915_VK_OBJ_RENDER_PASS || record->objects[1]->kind != I915_VK_OBJ_FRAMEBUFFER)
		return EINVAL;
	description = record->objects[0]->payload;
	framebuffer = record->objects[1]->payload;
	if (description == NULL ||
	    framebuffer == NULL ||
	    description->colour_count > BCM2711_NATIVE_PASS_TARGETS)
		return EINVAL;

	/* Exact nonempty render bounds are checked before narrowing any tile endpoint. */
	if (framebuffer->width == 0 ||
	    framebuffer->height == 0)
		return EINVAL;
	if (record->area.offset.x < 0 ||
	    record->area.offset.y < 0 ||
	    record->area.extent.width == 0 ||
	    record->area.extent.height == 0)
		return EINVAL;
	if ((uint64_t)record->area.offset.x + record->area.extent.width > framebuffer->width || (uint64_t)record->area.offset.y + record->area.extent.height > framebuffer->height)
		return EINVAL;

	/*
	 * Default tile loads preserve partial render areas and extra grouped
	 * supertile tiles.  Between the native passes a clear splits it into,
	 * the attachments are in the subpass's layouts and loaded.
	 */
	continued = false;
	if (begin->opcode == GPU_OP_CMD_CLEAR_ATTACHMENTS)
		continued = true;
	pass->state.width = framebuffer->width;
	pass->state.height = framebuffer->height;
	pass->state.targets = description->colour_count;
	if (description->samples == VK_SAMPLE_COUNT_4_BIT)
		pass->state.msaa = 1;

	/* A pass without attachments rasterizes with its pipelines' samples, which Vulkan makes the same for all its draws. */
	if (description->attachment_count == 0 && pass_samples(begin) == VK_SAMPLE_COUNT_4_BIT)
		pass->state.msaa = 1;
	pass->area = record->area;

	/* Each used colour slot's target. */
	for (slot = 0; slot < description->colour_count; slot++) {
		if (!description->colour_used[slot])
			continue;
		error = prepare_colour(pass, description, framebuffer, record, slot, continued, split);
		if (error != 0)
			return error;
	}

	/* A depth/stencil attachment is cleared, loaded and stored in the tile buffer. */
	if (description->depth) {
		error = prepare_depth(pass, description, framebuffer, record, continued, split);
		if (error != 0)
			return error;
	}

	/* A resolved depth/stencil attachment is resolved by the last native pass of the render pass. */
	if (description->depth_resolve && !split) {
		error = prepare_depth_resolve(pass, description, framebuffer, record);
		if (error != 0)
			return error;
	}

	/* The tile size the targets choose, and the tiles of the render area. */
	error = bcm2711_vulkan_native_pass_tiles(pass);
	if (error != 0)
		return error;

	/* A continued pass first clears what its clear names. */
	if (continued) {
		error = prepare_attachments(pass, begin->record);
		if (error != 0)
			return error;
	}

	/* Succeeded: all later target references borrow these independently retained views, never a public framebuffer identity. */
	return 0;
}

/*
 * Retains one colour slot's coherent output view and copies its
 * lifecycle, format and clear state, choosing a tile clear when the
 * attachment clears all of it.
 */
static int
prepare_colour(
	struct bcm2711_vulkan_native_pass *pass,
	const struct bcm2711_vulkan_pass *description,
	const struct bcm2711_vulkan_framebuffer *framebuffer,
	const struct bcm2711_vulkan_record *record,
	uint32_t slot,
	bool continued,
	bool split)
{
	const VkAttachmentDescription *attachment;
	const struct bcm2711_vulkan_image_view *image_view;
	struct bcm2711_vulkan_colour_target colour;
	struct bcm2711_vulkan_native_target *target;
	struct bcm2711_native_pass_target *native;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_vulkan_image_level level;
	struct bcm2711_v3d_view *view;
	uint32_t address;
	void *cpu;
	int error;

	/* The framebuffer's view of the slot names the attachment's image. */
	attachment = &description->colours[slot];
	if (framebuffer->colours[slot] == NULL ||
	    framebuffer->colours[slot]->kind != I915_VK_OBJ_IMAGE_VIEW)
		return EINVAL;
	image_view = framebuffer->colours[slot]->payload;
	if (image_view == NULL ||
	    image_view->owner.parent == NULL ||
	    image_view->owner.parent->kind != I915_VK_OBJ_IMAGE)
		return EINVAL;
	image = image_view->owner.parent->payload;
	if (image == NULL ||
	    (image->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0 ||
	    image->format != attachment->format ||
	    image->bytes > 0xffffffffU)
		return EINVAL;

	/* The tile buffer's description of the format. */
	native = &pass->state.target[slot];
	error = bcm2711_vulkan_colour_target(image->format, &colour);
	if (error != 0)
		return error;
	error = bcm2711_vulkan_native_target_format(native, image->format);
	if (error != 0)
		return error;

	/* The view's mip level of its layer: the image's own raster, or a smaller level's after it, in the layer's place. */
	error = bcm2711_vulkan_image_level(image, image_view->base_level, &level);
	if (error != 0)
		return error;
	if (framebuffer->width > level.width || framebuffer->height > level.height || level.bytes > 0xffffffffU)
		return EINVAL;
	if (image_view->base_layer + pass->view >= bcm2711_vulkan_image_layers(image))
		return EINVAL;
	level.offset += (uint64_t)(image_view->base_layer + pass->view) * image->layer_stride;

	/* Actual coherent logical image storage provides a native interval and CPU clear alias protected by its independent view hold. */
	error = bcm2711_vulkan_resource_backing(image, level.offset, level.bytes, &view, &address, &cpu);
	if (error != 0)
		return error;

	/* The checked reference is acquired before any copied CPU alias or numerical address enters the pass. */
	if (view->references == 0 || view->references == 0xffffffffU)
		return EOVERFLOW;
	bcm2711_v3d_memory_retain(view);
	target = &pass->targets[slot];
	target->output = view;
	target->cpu = cpu;

	/*
	 * The retained primary keeps the logical attachment alive while native
	 * execution commits its implicit layout lifecycle.  Between the native
	 * passes a clear splits it into, the target is in the subpass's
	 * attachment layout.
	 */
	target->image = image;
	target->level = image_view->base_level;
	target->layer = image_view->base_layer + pass->view;
	target->width = level.width;
	target->height = level.height;
	target->initial_layout = attachment->initialLayout;
	target->final_layout = attachment->finalLayout;
	if (continued)
		target->initial_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	if (split)
		target->final_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	/* The tiles load the target and store it when the attachment is stored, or when a clear splits the pass. */
	native->output = address;
	native->output_bytes = (uint32_t)level.bytes;
	native->pitch = level.pitch;
	native->load = 1;
	if (attachment->storeOp == VK_ATTACHMENT_STORE_OP_STORE || split)
		native->store = 1;

	/* Clear state is copied without touching the actual target before whole-pass preparation succeeds; a continued pass loads. */
	target->format = image->format;
	target->swap = 0;
	if (colour.swap)
		target->swap = 1;
	target->load = attachment->loadOp;
	if (continued)
		target->load = VK_ATTACHMENT_LOAD_OP_LOAD;
	kern_memcpy(target->clear, &record->words[description->colour_attachment[slot] * 4U], sizeof(target->clear));

	/* Whole-image clears can initialize tile memory without uncached CPU writes or an image load. */
	error = prepare_target_clear(pass, slot);
	if (error != 0)
		return error;

	/* Succeeded: the slot's target is retained and described. */
	return 0;
}

/*
 * Prepares the pass's depth/stencil attachment (ws141-p017).
 *
 * The tile buffer clears it in every tile to the begin's values.  An aspect
 * that loads makes the tiles load the image; one that stores makes them
 * store it.  A store from a render area smaller than the framebuffer first
 * loads the image, so pixels outside the area keep their values; one that
 * also clears there cannot, and is refused.
 *
 * A pass that continues its render pass after a clear loads the
 * attachment; one that a clear ends stores it.  Between them it is in the
 * subpass's depth/stencil attachment layout.
 */
static int
prepare_depth(
	struct bcm2711_vulkan_native_pass *pass,
	const struct bcm2711_vulkan_pass *description,
	const struct bcm2711_vulkan_framebuffer *framebuffer,
	const struct bcm2711_vulkan_record *record,
	bool continued,
	bool split)
{
	const VkAttachmentDescription *attachment;
	const struct bcm2711_vulkan_image_view *image_view;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_vulkan_resource layer_view;
	struct bcm2711_v3d_view *view;
	uint32_t address;
	void *cpu;
	bool whole;
	bool clears;
	int error;

	/* The framebuffer's depth/stencil view names the attachment's image. */
	attachment = &description->depth_stencil;
	if (framebuffer->depth == NULL ||
	    framebuffer->depth->kind != I915_VK_OBJ_IMAGE_VIEW ||
	    framebuffer->depth->payload == NULL)
		return EINVAL;
	image_view = framebuffer->depth->payload;
	if (image_view->owner.parent == NULL || image_view->owner.parent->kind != I915_VK_OBJ_IMAGE)
		return EINVAL;
	image = image_view->owner.parent->payload;

	/* A depth/stencil attachment image of the pass's format. */
	if (image == NULL ||
	    (image->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0 ||
	    image->format != attachment->format ||
	    image->bytes > 0xffffffffU)
		return EINVAL;

	/* The tiles load and store the array layer the framebuffer's view names (its view's, under multiview), as an image of its own. */
	error = bcm2711_vulkan_image_level_view(image, 0, image_view->base_layer + pass->view, &layer_view);
	if (error != 0)
		return error;

	/* The kind of attachment, and its clear values from the begin's second entry. */
	pass->state.depth = 1;
	if (attachment->format == VK_FORMAT_X8_D24_UNORM_PACK32)
		pass->state.depth = 2;
	if (attachment->format == VK_FORMAT_D16_UNORM)
		pass->state.depth = 3;
	if (attachment->format == VK_FORMAT_D32_SFLOAT)
		pass->state.depth = 4;
	pass->state.depth_clear = 0x3f800000U;
	pass->state.stencil_clear = 0;
	clears = false;
	if (!continued &&
	    (attachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ||
	     attachment->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR))
		clears = true;
	if (clears && attachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
		pass->state.depth_clear = record->words[description->depth_attachment * 4U];
	if (clears && attachment->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_CLEAR)
		pass->state.stencil_clear = record->words[description->depth_attachment * 4U + 1U] & 0xffU;

	/* Loads and stores follow the aspects' operations, or the split of the render pass by a clear. */
	if (attachment->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD || attachment->stencilLoadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
		pass->state.depth_load = 1;
	if (continued)
		pass->state.depth_load = 1;
	if (attachment->storeOp == VK_ATTACHMENT_STORE_OP_STORE || attachment->stencilStoreOp == VK_ATTACHMENT_STORE_OP_STORE)
		pass->state.depth_store = 1;
	if (split)
		pass->state.depth_store = 1;

	/* A stored attachment outside a smaller render area keeps its values by being loaded first. */
	whole = false;
	if (record->area.offset.x == 0 &&
	    record->area.offset.y == 0 &&
	    record->area.extent.width == framebuffer->width &&
	    record->area.extent.height == framebuffer->height)
		whole = true;
	if (pass->state.depth_store != 0 && !whole) {
		if (clears)
			return ENOTSUP;
		pass->state.depth_load = 1;
	}

	/* The layer's layout is checked and committed like the colour target's, whether or not the tiles move it. */
	pass->depth_target = image;
	pass->depth_layer = image_view->base_layer + pass->view;
	pass->depth_initial_layout = attachment->initialLayout;
	pass->depth_final_layout = attachment->finalLayout;
	if (continued)
		pass->depth_initial_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	if (split)
		pass->depth_final_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	/* An attachment kept in the tile buffer needs no image storage. */
	if (pass->state.depth_load == 0 && pass->state.depth_store == 0)
		return 0;

	/* The layer's coherent storage is held through the pass's retirement, like the output's. */
	error = bcm2711_vulkan_resource_backing(&layer_view, 0, layer_view.bytes, &view, &address, &cpu);
	if (error != 0)
		return error;
	if (view->references == 0 || view->references == 0xffffffffU)
		return EOVERFLOW;
	bcm2711_v3d_memory_retain(view);
	pass->depth = view;
	pass->depth_cpu = cpu;
	pass->state.depth_output = address;
	pass->state.depth_pitch = layer_view.pitch;
	pass->state.depth_bytes = (uint32_t)layer_view.bytes;

	/* Succeeded: the tiles move the attachment through its retained image. */
	return 0;
}

/*
 * Prepares the resolve of the depth/stencil attachment at the end of the
 * render pass: the tiles store sample 0 of each pixel into the layer of the
 * resolve attachment the framebuffer's view names (its view's, under
 * multiview).  A tile store writes whole tiles, so a render area smaller
 * than the framebuffer is stored into a raster of the pass's own
 * (prepare_lists) and copied from there on the CPU (the finish), which
 * leaves the attachment outside the render area as it was.
 */
static int
prepare_depth_resolve(
	struct bcm2711_vulkan_native_pass *pass,
	const struct bcm2711_vulkan_pass *description,
	const struct bcm2711_vulkan_framebuffer *framebuffer,
	const struct bcm2711_vulkan_record *record)
{
	const struct bcm2711_vulkan_image_view *image_view;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_vulkan_resource layer_view;
	struct bcm2711_v3d_view *view;
	const VkAttachmentDescription *attachment;
	uint32_t address;
	void *cpu;
	int error;

	/* The framebuffer's resolve view names an image of one sample in the attachment's format. */
	attachment = &description->depth_stencil;
	if (pass->state.depth == 0 ||
	    pass->state.msaa == 0 ||
	    framebuffer->depth_resolve == NULL ||
	    framebuffer->depth_resolve->kind != I915_VK_OBJ_IMAGE_VIEW ||
	    framebuffer->depth_resolve->payload == NULL)
		return EINVAL;
	image_view = framebuffer->depth_resolve->payload;
	if (image_view->owner.parent == NULL || image_view->owner.parent->kind != I915_VK_OBJ_IMAGE)
		return EINVAL;
	image = image_view->owner.parent->payload;
	if (image == NULL ||
	    image->samples != VK_SAMPLE_COUNT_1_BIT ||
	    image->format != attachment->format ||
	    image->bytes > 0xffffffffU)
		return EINVAL;

	/* The layer the framebuffer's view names, as an image of its own, its layout checked and committed at the pass's ends. */
	error = bcm2711_vulkan_image_level_view(image, 0, image_view->base_layer + pass->view, &layer_view);
	if (error != 0)
		return error;
	pass->resolve_target = image;
	pass->resolve_layer = image_view->base_layer + pass->view;
	pass->resolve_initial_layout = description->depth_resolved.initialLayout;
	pass->resolve_final_layout = description->depth_resolved.finalLayout;

	/* Its coherent storage is held through the pass's retirement, like the attachment's. */
	error = bcm2711_vulkan_resource_backing(&layer_view, 0, layer_view.bytes, &view, &address, &cpu);
	if (error != 0)
		return error;
	if (view->references == 0 || view->references == 0xffffffffU)
		return EOVERFLOW;
	bcm2711_v3d_memory_retain(view);
	pass->resolve = view;
	pass->state.depth_resolve = 1;
	pass->state.depth_resolve_output = address;
	pass->state.depth_resolve_pitch = layer_view.pitch;
	pass->state.depth_resolve_bytes = (uint32_t)layer_view.bytes;

	/* A smaller render area: the CPU copies it from the pass's raster into the layer (the raster is made with the lists). */
	pass->resolve_address = address;
	pass->resolve_bytes = (uint32_t)layer_view.bytes;
	pass->resolve_cpu = cpu;
	pass->resolve_pitch = layer_view.pitch;
	pass->resolve_texel = attachment->format == VK_FORMAT_D16_UNORM ? 2U : 4U;
	if (record->area.offset.x != 0 ||
	    record->area.offset.y != 0 ||
	    record->area.extent.width != framebuffer->width ||
	    record->area.extent.height != framebuffer->height) {
		if (cpu == NULL)
			return EINVAL;
		pass->state.depth_resolve_output = 0;
	}

	/* Succeeded: the tiles store the resolve. */
	return 0;
}

/* Selects a colour target's native tile clear only when no sample outside the render area needs preservation. */
static int
prepare_target_clear(
	struct bcm2711_vulkan_native_pass *pass,
	uint32_t slot)
{
	struct bcm2711_vulkan_native_target *target;
	struct bcm2711_native_pass_target *native;
	int error;

	/* Loading and discarding retain the existing tile preservation path. */
	target = &pass->targets[slot];
	native = &pass->state.target[slot];
	if (target->load != VK_ATTACHMENT_LOAD_OP_CLEAR)
		return 0;

	/* A partial rectangle still needs exact CPU clear followed by tile load. */
	if (pass->area.offset.x != 0 || pass->area.offset.y != 0)
		return 0;

	/* Cover the whole backing level, including when the framebuffer is smaller. */
	if (pass->area.extent.width != target->width ||
	    pass->area.extent.height != target->height)
		return 0;

	/* The tile buffer's clear colour of the target's type; unrepresentable clear state refuses before any native command is published. */
	error = bcm2711_native_colour_clear(target->clear, native->type, native->bpp, target->swap, native->clear);
	if (error != 0)
		return error;

	/* Clear initializes every rendered tile; no prior image contents are consumed. */
	native->load = 0;

	/* Succeeded: native execution owns the complete clear and subsequent drawing. */
	return 0;
}

/* Builds complete linked native draw owners and locates the exact end of this immutable prepared pass. */
static int
collect_draws(
	struct bcm2711_vulkan_native_pass *pass,
	const struct bcm2711_vulkan_prepared_event *begin,
	uint64_t *remaining,
	const struct bcm2711_vulkan_prepared_event **next)
{
	const struct bcm2711_vulkan_prepared_event *event;
	struct bcm2711_vulkan_native_draw *draw;
	uint32_t counter;
	int error;

	/* Only ordered draw (and query end) events belonging to the same immutable BEGIN record can enter this native pass. */
	event = begin->next;
	while (event != NULL && pass_inside(event)) {
		/* A foreign pass pointer cannot extend one native framebuffer's lifetime or command list. */
		if (event->pass != begin->pass)
			return EINVAL;

		/* A query's end runs once the pass has completed; zero vertices or instances perform no GPU reads and therefore allocate no owners. */
		if (event->opcode == GPU_OP_CMD_END_QUERY || event->draw[0] == 0 || event->draw[1] == 0) {
			event = event->next;
			continue;
		}

		/* The pass's counter of the draw's occlusion query, if it has one. */
		error = occlusion_counter(pass, event, &counter);
		if (error != 0)
			return error;

		/* Complete native draw preparation occurs at FIFO execution after preceding GPU output becomes CPU-visible. */
		error = bcm2711_vulkan_native_draw_create(pass->space, event, pass->view, remaining, pass->first, &pass->draw_pool, &draw);
		if (error != 0)
			return error;
		draw->occlusion = counter;

		/* Attach immediately so every later error retires the whole draw prefix. */
		if (pass->last == NULL)
			pass->first = draw;
		else
			pass->last->next = draw;
		pass->last = draw;
		pass->draws++;
		pass->bytes += draw->bytes;
		event = event->next;
	}

	/* The complete immutable pass must end explicitly before a following pass or transfer may execute. */
	if (event == NULL ||
	    (event->opcode != GPU_OP_CMD_END_RENDER_PASS &&
	     event->opcode != GPU_OP_CMD_CLEAR_ATTACHMENTS) ||
	    event->pass != begin->pass)
		return EINVAL;

	/* A clear of the attachments starts the native pass that continues this one. */
	*next = event->next;
	if (event->opcode == GPU_OP_CMD_CLEAR_ATTACHMENTS)
		*next = event;

	/* Succeeded: the complete native draw chain owns no borrowed command or descriptor record. */
	return 0;
}

/* Acquires one padded native interval and immediately attaches it to the pass before any subsequent failure. */
static int
allocate_storage(
	struct bcm2711_vulkan_native_pass *pass,
	uint64_t bytes,
	unsigned fill,
	uint64_t *remaining,
	struct bcm2711_native_storage **storage)
{
	uint64_t padded;
	int error;

	/* A refused interval leaves the existing owned prefix unchanged. */
	*storage = NULL;
	if (bytes == 0 || bytes > NATIVE_PASS_BYTES)
		return EINVAL;
	padded = (bytes + 4095U) & ~4095ULL;

	/* Every physical page, including command padding and overflow reserves, consumes the shared job budget. */
	if (pass->count == BCM2711_VULKAN_PASS_STORAGE || padded > *remaining)
		return ENOMEM;
	error = bcm2711_native_storage_create_as(pass->space, bytes, fill, storage);
	if (error != 0)
		return error;
	pass->storage[pass->count] = *storage;
	pass->count++;
	pass->bytes += padded;
	*remaining -= padded;

	/* Succeeded: every complete physical/mapped owner is reachable from the whole pass before bytes are published. */
	return 0;
}

/*
 * Narrows a pass that loads its own output to the tiles its draws' clip
 * windows meet.
 *
 * Every other tile would be loaded and stored back unchanged: no draw
 * writes a pixel outside its window, and a partial clear was written to
 * the image before the launch.  A pass that clears natively, or whose
 * windows need more boxes than the pass holds, renders every tile of its
 * rectangle as before.
 */
static void
select_boxes(
	struct bcm2711_vulkan_native_pass *pass)
{
	const struct bcm2711_vulkan_native_draw *draw;
	uint32_t box[4];
	uint32_t slot;
	bool added;

	/* Only a pass whose tiles start from its own outputs can leave a tile as it is. */
	pass->state.box_count = 0;
	if (pass->state.source != 0)
		return;
	for (slot = 0; slot < pass->state.targets; slot++) {
		if (pass->state.target[slot].output != 0 && pass->state.target[slot].load == 0)
			return;
	}

	/* A stored depth/stencil attachment not loaded first must be written in every tile. */
	if (pass->state.depth_store != 0 && pass->state.depth_load == 0)
		return;

	/* Each draw's window, as the tiles it covers inside the pass's rectangle. */
	for (draw = pass->first; draw != NULL; draw = draw->next) {
		/* A window with no pixel draws nothing. */
		if (draw->window[2] == 0 || draw->window[3] == 0)
			continue;

		/* The window's first and last tiles, kept inside the rectangle. */
		box[0] = draw->window[0] / pass->state.tile_width;
		box[1] = draw->window[1] / pass->state.tile_height;
		box[2] = (draw->window[0] + draw->window[2] - 1U) / pass->state.tile_width;
		box[3] = (draw->window[1] + draw->window[3] - 1U) / pass->state.tile_height;
		if (box[0] < pass->state.first_x)
			box[0] = pass->state.first_x;
		if (box[1] < pass->state.first_y)
			box[1] = pass->state.first_y;
		if (box[2] > pass->state.last_x)
			box[2] = pass->state.last_x;
		if (box[3] > pass->state.last_y)
			box[3] = pass->state.last_y;
		if (box[0] > box[2] || box[1] > box[3])
			continue;

		/* Too many different boxes: every tile, as before. */
		added = box_add(&pass->state, box);
		if (!added) {
			pass->state.box_count = 0;
			return;
		}
	}
}

/*
 * Adds a tile box to a pass's boxes: nothing when one holds it already,
 * the boxes it holds replaced by it.  Returns false when no room is left.
 */
static bool
box_add(
	struct bcm2711_native_pass *state,
	const uint32_t *box)
{
	uint32_t index;

	/* A box that holds it already. */
	for (index = 0; index < state->box_count; index++) {
		if (state->boxes[index][0] <= box[0] &&
		    state->boxes[index][1] <= box[1] &&
		    state->boxes[index][2] >= box[2] &&
		    state->boxes[index][3] >= box[3])
			return true;
	}

	/* The boxes it holds are taken out, the last moved into each one's place. */
	index = 0;
	while (index < state->box_count) {
		if (box[0] <= state->boxes[index][0] &&
		    box[1] <= state->boxes[index][1] &&
		    box[2] >= state->boxes[index][2] &&
		    box[3] >= state->boxes[index][3]) {
			state->box_count--;
			kern_memcpy(state->boxes[index], state->boxes[state->box_count], sizeof(state->boxes[index]));
			continue;
		}
		index++;
	}

	/* No room for another box. */
	if (state->box_count == BCM2711_NATIVE_PASS_BOXES)
		return false;

	/* Succeeded: the box is the pass's. */
	kern_memcpy(state->boxes[state->box_count], box, sizeof(state->boxes[0]));
	state->box_count++;
	return true;
}

/* Allocates, assembles and cleans every native list/binning/overflow input before publishing the complete CL job. */
static int
prepare_lists(
	struct bcm2711_vulkan_native_pass *pass,
	uint64_t *remaining)
{
	struct bcm2711_native_pass_sizes sizes;
	struct bcm2711_native_pass_image bin;
	struct bcm2711_native_pass_image render;
	struct bcm2711_native_pass_image tile;
	struct bcm2711_native_storage *storage;
	struct bcm2711_vulkan_native_draw *draw;
	struct bcm2711_v3d_cl_job *job;
	uint32_t overflow;
	uint32_t index;
	uint32_t cursor;
	uint32_t counter;
	int error;

	/* Exact lengths and complete tile geometry precede physical command-storage acquisition. */
	error = bcm2711_native_pass_measure(&pass->state, pass->draws, &sizes);
	if (error != 0)
		return error;

	/* Three independent list images precede the pool and tile state in the fixed whole-pass owner vector. */
	error = allocate_storage(pass, sizes.bin, BCM2711_NATIVE_STORAGE_ZEROED, remaining, &storage);
	if (error != 0)
		return error;
	prepare_image(storage, &bin);
	error = allocate_storage(pass, sizes.render, BCM2711_NATIVE_STORAGE_ZEROED, remaining, &storage);
	if (error != 0)
		return error;
	prepare_image(storage, &render);
	error = allocate_storage(pass, sizes.tile, BCM2711_NATIVE_STORAGE_ZEROED, remaining, &storage);
	if (error != 0)
		return error;
	prepare_image(storage, &tile);
	/* The binner writes the tile lists it later reads into the pool, which the CPU never touches; a reused pool is neither zeroed nor cleaned. */
	error = allocate_storage(pass, sizes.pool, BCM2711_NATIVE_STORAGE_DEVICE, remaining, &storage);
	if (error != 0)
		return error;
	pass->state.pool = storage->view->address;
	pass->state.pool_bytes = (uint32_t)storage->view->bytes;
	error = allocate_storage(pass, sizes.state, BCM2711_NATIVE_STORAGE_ZEROED, remaining, &storage);
	if (error != 0)
		return error;
	pass->state.state = storage->view->address;
	pass->state.state_bytes = (uint32_t)storage->view->bytes;

	/*
	 * The raster of one sample a resolve of a smaller render area stores
	 * into (prepare_depth_resolve), as large as the framebuffer, with the
	 * pitch of the resolve attachment's layer.
	 */
	if (pass->state.depth_resolve != 0 && pass->state.depth_resolve_output == 0) {
		error = allocate_storage(pass, (uint64_t)pass->resolve_pitch * pass->state.height, BCM2711_NATIVE_STORAGE_ZEROED, remaining, &storage);
		if (error != 0)
			return error;
		pass->resolve_scratch = storage;
		pass->state.depth_resolve_output = storage->view->address;
		pass->state.depth_resolve_bytes = (uint32_t)storage->view->bytes;
	}

	/*
	 * The occlusion counters, one zeroed group that the tile buffer adds
	 * to (each storage starts a page, so the group is 1024-byte aligned).
	 */
	if (pass->occlusion_count != 0U) {
		error = allocate_storage(pass, BCM2711_VULKAN_OCCLUSION_GROUP_BYTES, BCM2711_NATIVE_STORAGE_ZEROED, remaining, &storage);
		if (error != 0)
			return error;
		pass->occlusion = storage;
	}

	/* Each complete numerical draw sequence references only its attached independently mapped inputs. */
	cursor = BCM2711_NATIVE_PASS_BIN_PREFIX;
	draw = pass->first;
	while (draw != NULL) {
		/* An incomplete numerical draw cannot enter the enclosing command image. */
		if (draw->bin_bytes != BCM2711_NATIVE_BIN_BYTES)
			return EINVAL;
		kern_memcpy(bin.bytes + cursor, draw->bin, draw->bin_bytes);

		/* A counting draw names its counter in the Occlusion Query Counter packet, little-endian. */
		if (draw->occlusion != 0U) {
			counter = pass->occlusion->view->address + (draw->occlusion - 1U) * 4U;
			for (index = 0; index < 4U; index++)
				bin.bytes[cursor + BCM2711_NATIVE_BIN_OCCLUSION + index] = (uint8_t)(counter >> (index * 8U));
		}
		cursor += draw->bin_bytes;
		draw = draw->next;
	}

	/* The whole three-list serializer checks all complete native intervals before any sequence becomes active. */
	error = bcm2711_native_pass_encode(&pass->state, pass->draws, &bin, &render, &tile);
	if (error != 0)
		return error;

	/*
	 * Four independently mapped overflow reserves support supervised native
	 * binning without allocation in IRQ service, sized by the pass's work.
	 */
	job = &pass->job.command.cl;
	overflow = overflow_bytes(pass);
	for (index = 0; index < 4U; index++) {
		/* The binner reads only what it wrote into an overflow reserve, which the CPU never touches; a reused one is neither zeroed nor cleaned. */
		error = allocate_storage(pass, overflow, BCM2711_NATIVE_STORAGE_DEVICE, remaining, &storage);
		if (error != 0)
			return error;
		job->overflow[index].address = storage->view->address;
		job->overflow[index].bytes = (uint32_t)storage->view->bytes;
	}

	/* Every initialized command, tile/pool and overflow allocation becomes visible before its job address is published (a clean device-only one has nothing to write back). */
	for (index = 0; index < pass->count; index++) {
		error = bcm2711_native_storage_clean(pass->storage[index]);
		if (error != 0)
			return error;
	}

	/* Complete bin/render endpoints borrow only this root's independently retained mapped owners. */
	job->bin_start = bin.address;
	job->bin_end = bin.address + bin.used;
	job->render_start = render.address;
	job->render_end = render.address + render.used;
	job->pool_address = pass->state.pool;
	job->pool_bytes = pass->state.pool_bytes;
	job->state_address = pass->state.state;
	job->state_bytes = pass->state.state_bytes;
	job->overflow_count = 4;
	job->clean_output = true;

	/* Succeeded: the caller owns a complete cleaned native graphics job without any target mutation or launch. */
	return 0;
}

/*
 * The size of each of a pass's four overflow pools: enough for the tile
 * lists of its primitives (every vertex or index of every instance counted
 * at NATIVE_PASS_LIST_BYTES), in whole 256 KiB units between one unit and
 * the largest pool.  Running out stops the pass, so the estimate errs
 * high: a triangle the binner keeps takes about one to two bytes in each
 * tile it meets.
 */
static uint32_t
overflow_bytes(
	const struct bcm2711_vulkan_native_pass *pass)
{
	const struct bcm2711_vulkan_native_draw *draw;
	uint64_t elements;
	uint64_t pool;

	/* Every vertex or index the pass's draws read, over all their instances. */
	elements = 0;
	for (draw = pass->first; draw != NULL; draw = draw->next)
		elements += (uint64_t)draw->vertices * (draw->instances != 0 ? draw->instances : 1U);

	/* A quarter of the estimate a pool, rounded up to whole units and bounded. */
	pool = (elements * NATIVE_PASS_LIST_BYTES + 3U) / 4U;
	pool = (pool + BCM2711_V3D_OVERFLOW_UNIT - 1U) / BCM2711_V3D_OVERFLOW_UNIT * BCM2711_V3D_OVERFLOW_UNIT;
	if (pool < BCM2711_V3D_OVERFLOW_UNIT)
		pool = BCM2711_V3D_OVERFLOW_UNIT;
	if (pool > BCM2711_V3D_OVERFLOW_MAX)
		pool = BCM2711_V3D_OVERFLOW_MAX;

	/* Succeeded: the size of each pool. */
	return (uint32_t)pool;
}

/* Copies one complete owned mapping's numerical and CPU interval into an unpublished serializer descriptor. */
static void
prepare_image(
	struct bcm2711_native_storage *storage,
	struct bcm2711_native_pass_image *image)
{
	/* Native-storage creation bounds all complete allocation sizes before this narrowing and exposes initialized cached CPU bytes. */
	image->bytes = storage->view->buffer->address;
	image->address = storage->view->address;
	image->capacity = (uint32_t)storage->view->bytes;
	image->used = 0;

	/* Succeeded: the descriptor borrows exactly one complete independently retained native interval. */
	return;
}

/* Copies one subresource's numerical target and native clear colour before allocating any command storage. */
static int
prepare_clear(
	struct bcm2711_vulkan_native_pass *pass,
	const struct bcm2711_vulkan_record *record,
	uint32_t subresource)
{
	struct bcm2711_vulkan_colour_target colour;
	struct bcm2711_vulkan_native_target *target;
	struct bcm2711_native_pass_target *native;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_vulkan_resource level_view;
	struct bcm2711_v3d_view *view;
	uint32_t address;
	uint32_t level;
	uint32_t layer;
	void *cpu;
	int error;

	/* The independently pending primary supplies a typed immutable clear selection. */
	if (record->opcode != GPU_OP_CMD_CLEAR_COLOR_IMAGE ||
	    record->semantic_error != 0 ||
	    record->objects[0] == NULL)
		return EINVAL;

	/* The ranges name subresources of one colour image. */
	if (record->objects[0]->kind != I915_VK_OBJ_IMAGE ||
	    record->count == 0 ||
	    record->count > BCM2711_VULKAN_CLEAR_RANGES)
		return EINVAL;

	/* The clear preserves its explicit general or transfer destination layout. */
	if (record->layout != VK_IMAGE_LAYOUT_GENERAL && record->layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
		return EINVAL;

	/* A complete typed binding and destination usage precede all physical backing access. */
	image = record->objects[0]->payload;
	if (image == NULL ||
	    image->memory == NULL ||
	    (image->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
		return EINVAL;

	/* The level of the layer (or slice) this pass clears, as a one-level, one-layer image of its own. */
	error = bcm2711_vulkan_native_clear_subresource(record, subresource, &level, &layer);
	if (error != 0)
		return error;
	error = bcm2711_vulkan_image_level_view(image, level, layer, &level_view);
	if (error != 0)
		return error;

	/* Only complete native raster geometry enters the integer colour conversion. */
	if (level_view.width == 0 ||
	    level_view.width > 4096U ||
	    level_view.height == 0 ||
	    level_view.height > 4096U ||
	    level_view.bytes > 0xffffffffU)
		return EINVAL;

	/* A format the tile buffer holds; others have no native clear. */
	native = &pass->state.target[0];
	error = bcm2711_vulkan_colour_target(image->format, &colour);
	if (error != 0)
		return error;
	error = bcm2711_vulkan_native_target_format(native, image->format);
	if (error != 0)
		return error;

	/* The tile buffer's clear colour, converted as an attachment's clear is. */
	error = bcm2711_native_colour_clear(record->words, native->type, native->bpp, colour.swap ? 1U : 0U, native->clear);
	if (error != 0)
		return error;

	/* Exact coherent output backing of the level gains an independent view reference before any numerical alias enters the root. */
	error = bcm2711_vulkan_resource_backing(&level_view, 0, level_view.bytes, &view, &address, &cpu);
	if (error != 0)
		return error;

	/* The independent output edge must remain representable through all later preparation failures. */
	if (view->references == 0 || view->references == 0xffffffffU)
		return EOVERFLOW;
	bcm2711_v3d_memory_retain(view);

	/* The target keeps the image itself, whose layout at the level and layer the pass checks and keeps. */
	target = &pass->targets[0];
	target->output = view;
	target->cpu = cpu;
	target->image = image;
	target->level = level;
	target->layer = layer;
	target->width = level_view.width;
	target->height = level_view.height;
	target->initial_layout = record->layout;
	target->final_layout = record->layout;
	target->format = image->format;
	target->load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;

	/* Full target tile coverage requires no load, and hardware clear/store writes only valid framebuffer pixels (every sample of a multisampled image). */
	pass->state.width = level_view.width;
	pass->state.height = level_view.height;
	pass->state.targets = 1;
	if (image->samples == 4U)
		pass->state.msaa = 1;
	native->pitch = level_view.pitch;
	native->output = address;
	native->output_bytes = (uint32_t)level_view.bytes;
	native->store = 1;
	pass->area.extent.width = level_view.width;
	pass->area.extent.height = level_view.height;
	error = bcm2711_vulkan_native_pass_tiles(pass);
	if (error != 0)
		return error;

	/* Succeeded: the existing CPU partial-clear path is disabled for this independently owned native clear. */
	return 0;
}

/*
 * Prepares the clears of the vkCmdClearAttachments a continued pass starts with.
 *
 * A colour slot's clear of its whole target becomes a tile clear; other
 * colour rectangles, kept inside the render area, are cleared on the CPU
 * before the tiles load.  A depth/stencil clear of the whole framebuffer
 * and every aspect the attachment has becomes a tile clear, and the image
 * is not loaded; other depth/stencil rectangles are written on the CPU
 * like the colour ones, keeping the aspects not cleared.
 */
static int
prepare_attachments(
	struct bcm2711_vulkan_native_pass *pass,
	const struct bcm2711_vulkan_record *clear)
{
	struct bcm2711_vulkan_native_target *target;
	const uint32_t *rect;
	uint32_t aspects;
	uint32_t index;
	uint32_t slot;
	bool whole_area;
	bool covered;
	int error;

	/* Whether the render area is the whole framebuffer, and whether one rectangle covers it. */
	aspects = clear->words[BCM2711_VULKAN_CLEAR_ASPECTS];
	whole_area = false;
	if (pass->area.offset.x == 0 &&
	    pass->area.offset.y == 0 &&
	    pass->area.extent.width == pass->state.width &&
	    pass->area.extent.height == pass->state.height)
		whole_area = true;
	covered = false;
	for (index = 0; index < clear->count; index++) {
		rect = &clear->words[BCM2711_VULKAN_CLEAR_RECT_WORD + index * 4U];
		if (rect[0] == 0 &&
		    rect[1] == 0 &&
		    rect[2] >= pass->state.width &&
		    rect[3] >= pass->state.height)
			covered = true;
	}

	/* Each colour slot the clear names: a whole target clears in the tile buffer, as a clearing load does; other rectangles are cut to the render area. */
	for (slot = 0; slot < pass->state.targets; slot++) {
		target = &pass->targets[slot];
		if ((aspects & (1U << (BCM2711_VULKAN_CLEAR_COLOUR_SHIFT + slot))) == 0 || target->output == NULL)
			continue;
		kern_memcpy(target->rect_clear, &clear->words[slot * 4U], sizeof(target->rect_clear));
		target->rect_count = 0;
		if (whole_area &&
		    covered &&
		    pass->state.width == target->width &&
		    pass->state.height == target->height) {
			target->load = VK_ATTACHMENT_LOAD_OP_CLEAR;
			kern_memcpy(target->clear, &clear->words[slot * 4U], sizeof(target->clear));
			error = prepare_target_clear(pass, slot);
			if (error != 0)
				return error;
			continue;
		}
		for (index = 0; index < clear->count; index++)
			target->rect_count = cut_rect(pass, &clear->words[BCM2711_VULKAN_CLEAR_RECT_WORD + index * 4U], target->rects, target->rect_count);
	}

	/* The depth/stencil attachment's part, when the clear names it and the pass has one. */
	if ((aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0 && pass->state.depth != 0) {
		error = prepare_depth_clear(pass, clear, aspects, whole_area && covered);
		if (error != 0)
			return error;
	}

	/* Succeeded: the pass clears what the clear names, then draws. */
	return 0;
}

/*
 * Clears a depth/stencil image on the CPU at its place in the queue.
 *
 * Every earlier pass has retired and published its output, so the image's
 * coherent alias holds its current pixels.  Each pixel keeps the bits of
 * the aspects not cleared: D24S8 and X8_D24 hold depth in bits 8 to 31 and
 * stencil in bits 0 to 7, D16 a 16-bit depth.  The layout stays the one the
 * command names, which must be the image's current one.
 */
int
bcm2711_vulkan_native_depth_clear_run(
	const struct bcm2711_vulkan_record *record)
{
	struct bcm2711_vulkan_resource *image;
	uint32_t texel;
	uint32_t mask;
	uint32_t value;
	uint32_t layers;
	uint32_t layer;
	VkImageLayout current;
	int error;

	/* A validated clear of a depth/stencil image in the layout it names. */
	if (record == NULL ||
	    record->opcode != GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE ||
	    record->objects[0] == NULL ||
	    record->objects[0]->payload == NULL)
		return EINVAL;
	image = record->objects[0]->payload;
	if (!bcm2711_vulkan_depth_format(image->format))
		return EINVAL;

	/* The cleared bits and their value, by format. */
	texel = 4U;
	mask = 0;
	if ((record->words[2] & VK_IMAGE_ASPECT_DEPTH_BIT) != 0)
		mask |= 0xffffff00U;
	if ((record->words[2] & VK_IMAGE_ASPECT_STENCIL_BIT) != 0 && image->format == VK_FORMAT_D24_UNORM_S8_UINT)
		mask |= 0xffU;
	value = depth_unorm(record->words[0], 24U) << 8 | (record->words[1] & 0xffU);
	if (image->format == VK_FORMAT_D16_UNORM) {
		texel = 2U;
		mask = 0;
		if ((record->words[2] & VK_IMAGE_ASPECT_DEPTH_BIT) != 0)
			mask = 0xffffU;
		value = depth_unorm(record->words[0], 16U);
	}
	if (image->format == VK_FORMAT_D32_SFLOAT) {
		mask = 0;
		if ((record->words[2] & VK_IMAGE_ASPECT_DEPTH_BIT) != 0)
			mask = 0xffffffffU;
		value = record->words[0];
	}

	/* The layers cleared, from the first (recording bounded them to the image's). */
	layers = bcm2711_vulkan_image_layers(image);
	if (record->first < layers)
		layers = record->first;
	if (image->pitch < image->width * texel * (image->samples == 4U ? 2U : 1U))
		return EINVAL;

	/* Every cleared layer is in the layout the command names before any is written. */
	for (layer = 0; layer < layers; layer++) {
		current = bcm2711_vulkan_image_level_layout(image, 0, layer);
		if (current != record->layout)
			return EINVAL;
	}

	/* Each layer's pixels take the cleared bits. */
	for (layer = 0; layer < layers; layer++) {
		error = depth_clear_layer(image, (uint64_t)layer * image->layer_stride, texel, mask, value);
		if (error != 0)
			return error;
	}

	/* Succeeded: following work sees the cleared image. */
	return 0;
}

/* Clears one layer of a depth/stencil image, at offset in its binding, through its coherent alias. */
static int
depth_clear_layer(
	struct bcm2711_vulkan_resource *image,
	uint64_t offset,
	uint32_t texel,
	uint32_t mask,
	uint32_t value)
{
	struct bcm2711_v3d_view *view;
	uint8_t *pixel;
	uint8_t *row;
	uint32_t address;
	uint32_t word;
	uint32_t columns;
	uint32_t lines;
	uint32_t x;
	uint32_t y;
	void *cpu;
	int error;

	/* The layer's rows through the image's coherent alias. */
	error = bcm2711_vulkan_resource_backing(image, offset, image->bytes, &view, &address, &cpu);
	if (error != 0)
		return error;
	if (!view->buffer->uncached)
		return EIO;

	/* Every pixel (every sample of a multisampled image's 2x2 blocks): the bits not cleared stay; explicit bytes avoid an unaligned word access. */
	columns = image->width;
	lines = image->height;
	if (image->samples == 4U) {
		columns *= 2U;
		lines *= 2U;
	}
	kern_io_read_barrier();
	row = cpu;
	for (y = 0; y < lines; y++) {
		pixel = row;
		for (x = 0; x < columns; x++) {
			word = (uint32_t)pixel[0] | (uint32_t)pixel[1] << 8;
			if (texel == 4U)
				word |= (uint32_t)pixel[2] << 16 | (uint32_t)pixel[3] << 24;
			word = (word & ~mask) | (value & mask);
			pixel[0] = (uint8_t)word;
			pixel[1] = (uint8_t)(word >> 8);
			if (texel == 4U) {
				pixel[2] = (uint8_t)(word >> 16);
				pixel[3] = (uint8_t)(word >> 24);
			}
			pixel += texel;
		}

		/* The next row, only while one remains. */
		if (y + 1U < lines)
			row += image->pitch;
	}
	kern_io_write_barrier();

	/* The image's snapshots and copies learn that its bytes changed. */
	bcm2711_v3d_memory_written(view->buffer, address - view->address, NULL);

	/* Succeeded: the layer is cleared. */
	return 0;
}

/*
 * Prepares a continued pass's clear of its depth/stencil attachment.
 *
 * Every aspect the attachment has, over the whole framebuffer, is a tile
 * clear without a load.  Otherwise the loaded image's rectangles are
 * written on the CPU first, each pixel only in the aspects cleared.
 */
static int
prepare_depth_clear(
	struct bcm2711_vulkan_native_pass *pass,
	const struct bcm2711_vulkan_record *clear,
	uint32_t aspects,
	bool whole)
{
	uint32_t needed;
	uint32_t index;

	/* The aspects the attachment has: X8_D24 and D16 have depth alone. */
	needed = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	if (pass->state.depth != 1)
		needed = VK_IMAGE_ASPECT_DEPTH_BIT;

	/* A whole clear is the tile buffer's; a partial one needs the loaded image's coherent alias. */
	if ((aspects & needed) == needed && whole) {
		pass->state.depth_load = 0;
		pass->state.depth_clear = clear->words[BCM2711_VULKAN_CLEAR_DEPTH];
		pass->state.stencil_clear = clear->words[BCM2711_VULKAN_CLEAR_DEPTH + 1U] & 0xffU;
	} else if (pass->depth == NULL || pass->depth_cpu == NULL || pass->state.depth_load == 0) {
		return EINVAL;
	} else {
		/* D24S8 and X8_D24 keep depth in bits 8 to 31 and stencil in 0 to 7; D16 is a 16-bit depth. */
		pass->depth_mask = 0;
		if ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0)
			pass->depth_mask |= 0xffffff00U;
		if ((aspects & VK_IMAGE_ASPECT_STENCIL_BIT) != 0 && pass->state.depth == 1)
			pass->depth_mask |= 0xffU;
		pass->depth_value = depth_unorm(clear->words[BCM2711_VULKAN_CLEAR_DEPTH], 24U) << 8 | (clear->words[BCM2711_VULKAN_CLEAR_DEPTH + 1U] & 0xffU);
		if (pass->state.depth == 3) {
			pass->depth_mask &= 0xffffU;
			if ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0)
				pass->depth_mask = 0xffffU;
			pass->depth_value = depth_unorm(clear->words[BCM2711_VULKAN_CLEAR_DEPTH], 16U);
		}

		/* A D32_SFLOAT pixel is the clear's float itself. */
		if (pass->state.depth == 4) {
			pass->depth_mask = 0;
			if ((aspects & VK_IMAGE_ASPECT_DEPTH_BIT) != 0)
				pass->depth_mask = 0xffffffffU;
			pass->depth_value = clear->words[BCM2711_VULKAN_CLEAR_DEPTH];
		}
		pass->depth_rect_count = 0;
		for (index = 0; pass->depth_mask != 0 && index < clear->count; index++)
			pass->depth_rect_count = cut_rect(pass, &clear->words[BCM2711_VULKAN_CLEAR_RECT_WORD + index * 4U], pass->depth_rects, pass->depth_rect_count);
	}

	/* Succeeded: the attachment is cleared before the draws. */
	return 0;
}

/* Adds the part of a clear rectangle (x, y, width, height) inside the render area to rects, which holds count; returns the new count. */
static uint32_t
cut_rect(
	const struct bcm2711_vulkan_native_pass *pass,
	const uint32_t *rect,
	VkRect2D *rects,
	uint32_t count)
{
	int64_t left;
	int64_t top;
	int64_t right;
	int64_t bottom;

	/* The rectangle's edges, moved inside the render area. */
	left = rect[0];
	top = rect[1];
	right = (int64_t)rect[0] + rect[2];
	bottom = (int64_t)rect[1] + rect[3];
	if (left < pass->area.offset.x)
		left = pass->area.offset.x;
	if (top < pass->area.offset.y)
		top = pass->area.offset.y;
	if (right > (int64_t)pass->area.offset.x + pass->area.extent.width)
		right = (int64_t)pass->area.offset.x + pass->area.extent.width;
	if (bottom > (int64_t)pass->area.offset.y + pass->area.extent.height)
		bottom = (int64_t)pass->area.offset.y + pass->area.extent.height;

	/* Nothing of it is inside, or no room is left (the record holds at most as many). */
	if (left >= right || top >= bottom || count >= BCM2711_VULKAN_CLEAR_RECTS)
		return count;

	/* The part inside, kept for the CPU clear. */
	rects[count].offset.x = (int32_t)left;
	rects[count].offset.y = (int32_t)top;
	rects[count].extent.width = (uint32_t)(right - left);
	rects[count].extent.height = (uint32_t)(bottom - top);

	/* Succeeded: one more rectangle. */
	return count + 1U;
}

/*
 * Converts a depth float's bits to unsigned normalized depth of width bits
 * (16 or 24), rounding to the nearest, without floating-point instructions.
 *
 * A value below zero (or a NaN with the sign set) is 0, one of 1.0 or more
 * (or another NaN) all ones.  The value, below 1.0 here, is m * 2^(e - 150);
 * A = value * 2^(2 width) = m << (e - 150 + 2 width) is exact when that
 * shift is not negative (smaller values round to 0), and
 * value * (2^width - 1) = (A * 2^width - A) / 2^(2 width), which splits A
 * into its high and low width bits to stay within 64 bits.
 */
static uint32_t
depth_unorm(
	uint32_t bits,
	uint32_t width)
{
	uint64_t whole;
	int64_t fraction;
	uint32_t exponent;
	uint32_t most;
	uint32_t high;
	uint32_t low;
	uint32_t depth;

	/* Negative values and zero, then 1.0 and beyond. */
	most = (1U << width) - 1U;
	if ((bits & 0x80000000U) != 0 || bits == 0)
		return 0;
	if (bits >= 0x3f800000U)
		return most;

	/* Values below 2^-(width + 1) round to 0. */
	exponent = bits >> 23;
	if (exponent < 150U - 2U * width)
		return 0;

	/* A, split into its high and low halves. */
	whole = (uint64_t)((bits & 0x7fffffU) | 0x800000U) << (exponent - (150U - 2U * width));
	high = (uint32_t)(whole >> width);
	low = (uint32_t)(whole & most);

	/* high, plus the rest rounded: a fraction in (-1, 1) of 2^(2 width), rounded half up. */
	fraction = ((int64_t)low - (int64_t)high) * ((int64_t)1 << width) - (int64_t)low + ((int64_t)1 << (2U * width - 1U));
	depth = high;
	if (fraction < 0)
		depth--;
	else if (fraction >= ((int64_t)1 << (2U * width)))
		depth++;

	/* The width bits, at most all ones. */
	if (depth > most)
		depth = most;

	/* Succeeded: the normalized depth. */
	return depth;
}

/* Reports whether the draws after a pass's first event end at a clear of the attachments rather than at the end of the render pass. */
static bool
pass_split(
	const struct bcm2711_vulkan_prepared_event *begin)
{
	const struct bcm2711_vulkan_prepared_event *event;

	/* The pass's draws, up to the event that ends them. */
	event = begin->next;
	while (event != NULL && pass_inside(event))
		event = event->next;

	/* A clear of the same render pass's attachments splits it. */
	if (event != NULL &&
	    event->opcode == GPU_OP_CMD_CLEAR_ATTACHMENTS &&
	    event->pass == begin->pass)
		return true;

	/* Succeeded: the end of the render pass, or a malformed list collect_draws refuses, ends the pass. */
	return false;
}

/* Gives the samples of the first pipeline a pass draws with: one when it draws nothing. */
static uint32_t
pass_samples(
	const struct bcm2711_vulkan_prepared_event *begin)
{
	const struct bcm2711_vulkan_prepared_event *event;

	/* The pass's draws, up to the event that ends them. */
	event = begin->next;
	while (event != NULL && pass_inside(event)) {
		if (event->pipeline != NULL &&
		    (event->opcode == GPU_OP_CMD_DRAW || event->opcode == GPU_OP_CMD_DRAW_INDEXED) &&
		    event->pipeline->samples != 0)
			return event->pipeline->samples;
		event = event->next;
	}

	/* Succeeded: a pass without a draw has one sample. */
	return VK_SAMPLE_COUNT_1_BIT;
}

/* Reports whether an event belongs inside a native pass: a draw, or an occlusion query's end. */
static bool
pass_inside(
	const struct bcm2711_vulkan_prepared_event *event)
{
	/* The draws, and the ends the pass runs after it completes. */
	if (event->opcode == GPU_OP_CMD_DRAW || event->opcode == GPU_OP_CMD_DRAW_INDEXED)
		return true;
	if (event->opcode == GPU_OP_CMD_END_QUERY && bcm2711_vulkan_occlusion_record(event->record) != NULL)
		return true;

	/* Succeeded: anything else ends the pass's draws. */
	return false;
}

/*
 * Gives a draw the pass's counter of its occlusion query, plus one (zero
 * when the draw counts nothing); the draws of one query share its
 * counter.  A pass holds one group of counters.
 */
static int
occlusion_counter(
	struct bcm2711_vulkan_native_pass *pass,
	const struct bcm2711_vulkan_prepared_event *event,
	uint32_t *counter)
{
	uint32_t index;

	/* A draw outside any query. */
	*counter = 0U;
	if (event->occlusion == NULL)
		return 0;

	/* The query's counter, when an earlier draw of the pass has it. */
	for (index = 0; index < pass->occlusion_count; index++) {
		if (pass->occlusion_pools[index] == event->occlusion && pass->occlusion_queries[index] == event->occlusion_query) {
			*counter = index + 1U;
			return 0;
		}
	}

	/* A new counter of the group. */
	if (pass->occlusion_count == BCM2711_VULKAN_OCCLUSION_COUNTERS) {
		kern_logf("v3d: pass counts more than %u occlusion queries\n", (unsigned)BCM2711_VULKAN_OCCLUSION_COUNTERS);
		return ENOTSUP;
	}
	pass->occlusion_pools[pass->occlusion_count] = event->occlusion;
	pass->occlusion_queries[pass->occlusion_count] = event->occlusion_query;
	pass->occlusion_count++;
	*counter = pass->occlusion_count;

	/* Succeeded: the draw has its counter. */
	return 0;
}

/*
 * After a pass has completed: adds each counter to its query (the CPU
 * reads what the tile buffer stored), then makes available the queries
 * whose end the pass ran.
 */
void
bcm2711_vulkan_native_pass_counted(
	struct bcm2711_vulkan_native_pass *pass,
	const struct bcm2711_vulkan_prepared_event *first,
	const struct bcm2711_vulkan_prepared_event *end,
	bool last)
{
	const struct bcm2711_vulkan_prepared_event *event;
	const uint8_t *counters;
	uint32_t samples;
	uint32_t index;
	uint32_t views;

	/* The counters, as the device stored them. */
	if (pass != NULL && pass->occlusion != NULL) {
		counters = pass->occlusion->view->buffer->address;
		kern_dcache_invalidate_range(counters, BCM2711_VULKAN_OCCLUSION_GROUP_BYTES);
		for (index = 0; index < pass->occlusion_count; index++) {
			samples = (uint32_t)counters[index * 4U] |
				  ((uint32_t)counters[index * 4U + 1U] << 8) |
				  ((uint32_t)counters[index * 4U + 2U] << 16) |
				  ((uint32_t)counters[index * 4U + 3U] << 24);
			bcm2711_vulkan_occlusion_add(pass->occlusion_pools[index], pass->occlusion_queries[index], samples);
		}
	}

	/* A query of a multiview pass is complete once the last view has counted into it. */
	if (!last)
		return;

	/* The ends among the pass's events, with the queries of the other views under multiview. */
	views = (uint32_t)__builtin_popcount(bcm2711_vulkan_native_pass_views(first));
	for (event = first; event != NULL && event != end; event = event->next) {
		if (event->opcode != GPU_OP_CMD_END_QUERY)
			continue;
		if (views > 1U)
			bcm2711_vulkan_occlusion_end_views(event->record, views);
		else
			bcm2711_vulkan_occlusion_run(event->record);
	}

	/* Succeeded: the queries count the pass. */
	return;
}
