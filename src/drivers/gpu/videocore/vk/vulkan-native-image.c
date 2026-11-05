/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Internal texture quads preserve copy bits or perform blit sampling, without CPU destination pixel writes. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vk/vulkan-native-image.h"

/* Each full resident native image transfer uses the same padded submission budget as ordinary draws. */
#define NATIVE_IMAGE_BYTES (256ULL * 1024U * 1024U)

#include "drivers/gpu/bcm2711/vk/meta/texture-words.inc"

/* Two counterclockwise triangles cover the entire private viewport; each pair selects one source UV corner. */
static const uint32_t meta_corners[6][2] = {
    {0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};

static int prepare_meta(struct bcm2711_vulkan_meta_draw *meta, const struct bcm2711_vulkan_transfer *transfer, uint32_t region);
static int prepare_programs(struct bcm2711_vulkan_meta_draw *meta, VkFormat destination);
static void release_meta(struct bcm2711_vulkan_meta_draw *meta);
static int prepare_output(struct bcm2711_vulkan_native_pass *pass, struct bcm2711_vulkan_resource *image, struct bcm2711_vulkan_resource *target, uint32_t level, VkImageLayout layout, const VkRect2D *area);
static int buffer_image_whole(const struct bcm2711_vulkan_record *record, struct bcm2711_vulkan_resource **image, struct bcm2711_vulkan_resource **buffer, uint64_t *offset, uint32_t *pitch);
static int retain_raster(struct bcm2711_vulkan_resource *resource, uint64_t offset, uint64_t bytes, struct bcm2711_v3d_view **view, uint32_t *address, void **cpu);
static bool copy_whole(const struct bcm2711_vulkan_transfer *transfer, uint32_t region, const struct bcm2711_vulkan_resource *source, const struct bcm2711_vulkan_resource *destination);
static int prepare_copy(struct bcm2711_vulkan_native_pass *pass, const struct bcm2711_vulkan_transfer *transfer, struct bcm2711_vulkan_resource *source, struct bcm2711_vulkan_resource *destination, uint64_t *remaining);
static uint32_t ratio_bits(uint32_t numerator, uint32_t denominator);
static void copy_narrow(struct bcm2711_vulkan_native_pass *pass, const struct bcm2711_vulkan_resource *destination, uint32_t (*boxes)[4], uint32_t count);

/*
 * Prepares one actual FIFO image transfer region as an independently owned native texture draw and pass.
 *
 * Source bytes are staged only after preceding queue writes retire.  The
 * parent prepared primary owns both exact logical images.  Uploaded shader,
 * texture, vertex, scalar and command storage plus the independent output
 * view belong to the returned root through uncertain native completion.
 */
int
bcm2711_vulkan_native_image_create(
	struct bcm2711_v3d_space *space,
	const struct bcm2711_vulkan_transfer *transfer,
	uint32_t region,
	uint64_t *available,
	struct bcm2711_vulkan_native_pass **pass)
{
	struct bcm2711_vulkan_native_pass *created;
	struct bcm2711_vulkan_meta_draw *meta;
	struct bcm2711_vulkan_resource *source;
	struct bcm2711_vulkan_resource *destination;
	struct bcm2711_vulkan_resource output;
	VkImageLayout current;
	uint64_t remaining;
	bool whole;
	int error;
	int released;

	/* No partial native root or budget deduction escapes an ordinary preparation refusal. */
	if (pass == NULL)
		return EINVAL;
	*pass = NULL;
	if (space == NULL ||
	    space->native == NULL ||
	    transfer == NULL ||
	    region >= transfer->record.count)
		return EINVAL;
	if (available == NULL || *available > NATIVE_IMAGE_BYTES)
		return EINVAL;

	/* Immutable transfer validation precedes current layout checks and any coherent source byte read. */
	if (transfer->record.objects[0] == NULL || transfer->record.objects[0]->payload == NULL)
		return EINVAL;
	source = transfer->record.objects[0]->payload;
	error = bcm2711_vulkan_transfer_validate(transfer, source->device);
	if (error != 0)
		return error;
	destination = transfer->record.objects[1]->payload;
	error = bcm2711_vulkan_image_level_view(destination, transfer->levels[1], &output);
	if (error != 0)
		return error;
	current = bcm2711_vulkan_image_level_layout(source, transfer->levels[0]);
	if (current != transfer->source_layout || output.layout != transfer->destination_layout)
		return EINVAL;

	/* One complete output owner exists before compiler, texture or numerical list allocations. */
	remaining = *available;
	created = kern_calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->space = space;
	created->job.kind = BCM2711_V3D_JOB_CL;

	/*
	 * A whole-image copy between equal raster images needs no shader or
	 * texture: the tile buffer loads the source and stores the destination.
	 */
	whole = false;
	if (transfer->levels[0] == 0 && transfer->levels[1] == 0)
		whole = copy_whole(transfer, region, source, destination);
	if (whole) {
		error = prepare_copy(created, transfer, source, destination, &remaining);
		if (error != 0) {
			released = bcm2711_vulkan_native_pass_release(&created, true);
			if (released != 0)
				return released;

			/* An unchanged source leaves no pass: the destination already holds its bytes. */
			if (error == EALREADY)
				return 0;
			return error;
		}

		/* Only a complete native pass consumes the caller's padded budget. */
		*available = remaining;
		*pass = created;

		/* Succeeded: the destination changes only when the native tile copy executes. */
		return 0;
	}

	/* Every other transfer draws one texture quad. */
	meta = kern_calloc(1, sizeof(*meta));
	if (meta == NULL) {
		kern_free(created);
		return ENOMEM;
	}

	/* The temporary CPU description retains no namespace object and is never part of the native DMA owner graph. */
	error = prepare_meta(meta, transfer, region);
	if (error == 0)
		error = prepare_output(created, &output, destination, transfer->levels[1], transfer->destination_layout, &meta->area);
	if (error == 0)
		error = prepare_programs(meta, destination->format);
	if (error == 0)
		error = bcm2711_vulkan_native_meta_draw_create(space, meta, &remaining, &created->first);

	/* Attach the whole independently uploaded draw before a later command-list allocation can fail. */
	if (error == 0) {
		created->last = created->first;
		created->draws = 1;
		created->bytes += created->first->bytes;
		error = bcm2711_vulkan_native_pass_build_lists(created, &remaining);
	}

	/* All compiler/geometry inputs were copied into native owners, or every unpublished prefix can retire before launch. */
	release_meta(meta);
	if (error != 0) {
		released = bcm2711_vulkan_native_pass_release(&created, true);
		if (released != 0)
			return released;
		return error;
	}

	/* Only a complete native pass consumes the caller's padded budget and becomes independently caller-owned. */
	*available = remaining;
	*pass = created;

	/* Succeeded: destination pixels remain unchanged until the existing native IRQ/cache runner executes this complete texture quad. */
	return 0;
}

/*
 * Prepares one whole-image copy between a raster image and a buffer as a zero-draw TLB copy pass.
 *
 * The tile buffer loads every tile from the source raster and stores it to
 * the destination raster at the same coordinates.  ENOTSUP, returned before
 * any allocation, leaves other copies to the exact CPU row copy.
 */
int
bcm2711_vulkan_native_buffer_image_create(
	struct bcm2711_v3d_space *space,
	const struct bcm2711_vulkan_record *record,
	uint64_t *available,
	struct bcm2711_vulkan_native_pass **pass)
{
	struct bcm2711_vulkan_native_pass *created;
	struct bcm2711_vulkan_resource *image;
	struct bcm2711_vulkan_resource *buffer;
	struct bcm2711_v3d_view *view;
	uint64_t offset;
	uint64_t remaining;
	uint64_t span;
	uint32_t pitch;
	uint32_t address;
	void *cpu;
	int error;
	int released;

	/* No partial native root or budget deduction escapes a refusal. */
	if (pass == NULL)
		return EINVAL;
	*pass = NULL;
	if (space == NULL ||
	    space->native == NULL ||
	    record == NULL ||
	    available == NULL ||
	    *available > NATIVE_IMAGE_BYTES)
		return EINVAL;

	/* Only one complete whole-image region with raster-compatible strides takes the tile path. */
	view = NULL;
	address = 0;
	error = buffer_image_whole(record, &image, &buffer, &offset, &pitch);
	if (error != 0)
		return error;

	/* The image's current layout is the one the command recorded, exactly as the CPU copy requires. */
	if (image->layout != record->layout)
		return EINVAL;

	/* One complete output owner exists before command-list allocations. */
	remaining = *available;
	created = kern_calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->space = space;
	created->job.kind = BCM2711_V3D_JOB_CL;

	/* The buffer side covers every row the tiles read or write at its own pitch. */
	span = (uint64_t)pitch * image->height;

	/* The image keeps its recorded layout; the pass changes no layout. */
	created->target = image;
	created->initial_layout = record->layout;
	created->final_layout = record->layout;
	created->area.offset.x = 0;
	created->area.offset.y = 0;
	created->area.extent.width = image->width;
	created->area.extent.height = image->height;
	created->format = image->format;
	created->load = VK_ATTACHMENT_LOAD_OP_LOAD;
	created->state.width = image->width;
	created->state.height = image->height;
	created->state.last_x = (image->width - 1U) / 64U;
	created->state.last_y = (image->height - 1U) / 64U;
	created->state.load = 1;
	created->state.store = 1;

	/* Upload stores into the image from the buffer; readback stores into the buffer from the image. */
	if (record->opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE) {
		error = retain_raster(image, 0, image->bytes, &created->output, &address, &created->cpu);
		created->state.output = address;
		created->state.pitch = image->pitch;
		created->state.output_bytes = (uint32_t)image->bytes;
		if (error == 0)
			error = retain_raster(buffer, offset, span, &view, &address, &cpu);
		created->source = view;
		created->state.source = address;
		created->state.source_pitch = pitch;
		created->state.source_bytes = (uint32_t)span;
	} else {
		error = retain_raster(buffer, offset, span, &created->output, &address, &created->cpu);
		created->state.output = address;
		created->state.pitch = pitch;
		created->state.output_bytes = (uint32_t)span;
		if (error == 0)
			error = retain_raster(image, 0, image->bytes, &view, &address, &cpu);
		created->source = view;
		created->state.source = address;
		created->state.source_pitch = image->pitch;
		created->state.source_bytes = (uint32_t)image->bytes;
	}

	/* The lists hold no draw: each tile is loaded from the source and stored. */
	if (error == 0)
		error = bcm2711_vulkan_native_pass_build_lists(created, &remaining);
	if (error != 0) {
		released = bcm2711_vulkan_native_pass_release(&created, true);
		if (released != 0)
			return released;
		return error;
	}

	/* Only a complete native pass consumes the caller's padded budget. */
	*available = remaining;
	*pass = created;

	/* Succeeded: both rasters change only when the native tile copy executes. */
	return 0;
}

/* Copies directed source UVs and exact positive destination viewport bounds using only integer IEEE construction. */
static int
prepare_meta(
	struct bcm2711_vulkan_meta_draw *meta,
	const struct bcm2711_vulkan_transfer *transfer,
	uint32_t region_index)
{
	const struct bcm2711_vulkan_transfer_region *region;
	struct bcm2711_vulkan_resource destination;
	uint32_t source[4];
	uint32_t minimum[2];
	uint32_t maximum[2];
	uint32_t uv[4];
	uint32_t axis;
	uint32_t index;
	uint32_t extent;
	int error;

	/* Source/destination logical ownership remains on the independently pending primary. */
	region = &transfer->regions[region_index];
	error = bcm2711_vulkan_image_level_view(transfer->record.objects[0]->payload, transfer->levels[0], &meta->level);
	if (error != 0)
		return error;
	meta->image = &meta->level;
	error = bcm2711_vulkan_image_level_view(transfer->record.objects[1]->payload, transfer->levels[1], &destination);
	if (error != 0)
		return error;
	meta->width = destination.width;
	meta->height = destination.height;
	meta->filter = transfer->filter;
	meta->raw = false;
	if (transfer->record.opcode == GPU_OP_CMD_COPY_IMAGE)
		meta->raw = true;

	/* Destination reversal swaps only the corresponding source endpoints while the hardware viewport remains positive. */
	for (axis = 0; axis < 2; axis++) {
		minimum[axis] = (uint32_t)region->destination[axis];
		maximum[axis] = (uint32_t)region->destination[axis + 2U];
		source[axis] = (uint32_t)region->source[axis];
		source[axis + 2U] = (uint32_t)region->source[axis + 2U];
		if (minimum[axis] > maximum[axis]) {
			minimum[axis] = (uint32_t)region->destination[axis + 2U];
			maximum[axis] = (uint32_t)region->destination[axis];
			source[axis] = (uint32_t)region->source[axis + 2U];
			source[axis + 2U] = (uint32_t)region->source[axis];
		}

		/* Complete validated image dimensions supply nonzero normalization divisors and nonempty destination extents. */
		extent = meta->image->width;
		if (axis != 0)
			extent = meta->image->height;
		if (extent == 0 || maximum[axis] <= minimum[axis])
			return EINVAL;
		uv[axis] = ratio_bits(source[axis], extent);
		uv[axis + 2U] = ratio_bits(source[axis + 2U], extent);
		meta->viewport[axis] = ratio_bits(minimum[axis], 1);
		meta->viewport[axis + 2U] = ratio_bits(maximum[axis] - minimum[axis], 1);
	}

	/* The private unit clip quad maps every destination pixel centre to Vulkan's directed source affine sample coordinates. */
	meta->area.offset.x = (int32_t)minimum[0];
	meta->area.offset.y = (int32_t)minimum[1];
	meta->area.extent.width = maximum[0] - minimum[0];
	meta->area.extent.height = maximum[1] - minimum[1];
	meta->viewport[5] = 0x3f800000U;
	for (index = 0; index < 6; index++) {
		meta->vertices[index * 6U] = 0xbf800000U;
		if (meta_corners[index][0] != 0)
			meta->vertices[index * 6U] = 0x3f800000U;
		meta->vertices[index * 6U + 1U] = 0xbf800000U;
		if (meta_corners[index][1] != 0)
			meta->vertices[index * 6U + 1U] = 0x3f800000U;
		meta->vertices[index * 6U + 3U] = 0x3f800000U;
		meta->vertices[index * 6U + 4U] = uv[meta_corners[index][0] * 2U];
		meta->vertices[index * 6U + 5U] = uv[meta_corners[index][1] * 2U + 1U];
	}

	/* Succeeded: no kernel floating-point operation or caller-owned vertex metadata enters internal transfer geometry. */
	return 0;
}

/* Compiles three independently owned native variants of the own immutable internal texture SPIR-V modules. */
static int
prepare_programs(
	struct bcm2711_vulkan_meta_draw *meta,
	VkFormat destination)
{
	struct bcm2711_shader_key *key;
	struct bcm2711_shader_diagnostic diagnostic;
	const uint32_t *words;
	size_t count;
	uint32_t index;
	int error;

	/* One exact two-scalar varying interface joins coordinate, render-vertex and unblended fragment programs. */
	key = &meta->key;
	kern_memset(key, 0, sizeof(*key));
	key->varying_count = 2;
	key->varyings[1].component = 1;
	if (!meta->raw && destination == VK_FORMAT_B8G8R8A8_UNORM)
		key->swap_red_blue = 1;
	meta->pipeline.front = VK_FRONT_FACE_COUNTER_CLOCKWISE;

	/* The ordinary kernel SPIR-V compiler owns code and consumed scalar metadata; no external native binary is imported. */
	for (index = 0; index < 3; index++) {
		words = meta_vert_words;
		count = sizeof(meta_vert_words) / sizeof(meta_vert_words[0]);
		if (index == BCM2711_SHADER_FRAGMENT) {
			words = meta_frag_words;
			count = sizeof(meta_frag_words) / sizeof(meta_frag_words[0]);
		}

		/* Generate this exact stage into an independently releasable compiler owner. */
		error = bcm2711_shader_compile(words, count, (enum bcm2711_shader_stage)index, key, &meta->pipeline.programs[index], &diagnostic);
		if (error != 0)
			return error;
	}

	/* Succeeded: independently uploaded native copies may outlive all temporary internal program metadata. */
	return 0;
}

/* Retires every temporary compiler prefix only after complete native upload or a no-launch preparation refusal. */
static void
release_meta(
	struct bcm2711_vulkan_meta_draw *meta)
{
	uint32_t index;

	/* Every completed compiler stage has an independent owner, even after a later stage refuses. */
	for (index = 0; index < 3; index++)
		bcm2711_shader_binary_free(meta->pipeline.programs[index]);
	kern_free(meta);

	/* Succeeded: no temporary geometry or compiler array remains needed by native DMA. */
	return;
}

/* Retains the destination's level (image, a view of target's level) and preserves all pixels outside the exact destination viewport. */
static int
prepare_output(
	struct bcm2711_vulkan_native_pass *pass,
	struct bcm2711_vulkan_resource *image,
	struct bcm2711_vulkan_resource *target,
	uint32_t level,
	VkImageLayout layout,
	const VkRect2D *area)
{
	struct bcm2711_v3d_view *view;
	uint32_t address;
	void *cpu;
	int error;

	/* Output backing must be independently representable before a native read/load/store can refer to it. */
	error = bcm2711_vulkan_resource_backing(image, 0, image->bytes, &view, &address, &cpu);
	if (error != 0)
		return error;
	if (view->references == 0 || view->references == 0xffffffffU)
		return EOVERFLOW;
	bcm2711_v3d_memory_retain(view);
	pass->output = view;
	pass->cpu = cpu;
	pass->target = target;
	pass->target_level = level;
	pass->initial_layout = layout;
	pass->final_layout = layout;

	/* Full framebuffer dimensions and selected tile bounds preserve neighbouring pixels in partial/grouped tiles. */
	pass->state.width = image->width;
	pass->state.height = image->height;
	pass->state.pitch = image->pitch;
	pass->state.output = address;
	pass->state.output_bytes = (uint32_t)image->bytes;
	pass->state.first_x = (uint32_t)area->offset.x / 64U;
	pass->state.first_y = (uint32_t)area->offset.y / 64U;
	pass->state.last_x = ((uint32_t)area->offset.x + area->extent.width - 1U) / 64U;
	pass->state.last_y = ((uint32_t)area->offset.y + area->extent.height - 1U) / 64U;
	pass->state.load = 1;
	pass->state.store = 1;
	pass->area = *area;
	pass->format = image->format;
	pass->load = VK_ATTACHMENT_LOAD_OP_LOAD;

	/* Succeeded: native texture drawing writes only the viewport while unchanged samples are loaded and stored unchanged. */
	return 0;
}

/* Constructs nearest-even binary32 bits for a bounded nonnegative rational without kernel floating-point state. */
static uint32_t
ratio_bits(
	uint32_t numerator,
	uint32_t denominator)
{
	uint64_t scaled;
	uint64_t divisor;
	uint64_t significand;
	uint64_t remainder;
	int32_t exponent;

	/* Exact zero has no normalization loop and retains positive zero. */
	if (numerator == 0)
		return 0;

	/* Inputs are at most 4096 with a nonzero divisor; every normalization and 23-bit shift fits uint64. */
	scaled = numerator;
	divisor = denominator;
	exponent = 0;
	while (scaled < divisor) {
		scaled <<= 1;
		exponent--;
	}

	/* Ratios greater than one arise only for exact integer viewport fields. */
	while (scaled >= divisor * 2U) {
		divisor <<= 1;
		exponent++;
	}

	/* One quotient and remainder preserve exact half-way information for IEEE nearest-even rounding. */
	scaled <<= 23;
	significand = scaled / divisor;
	remainder = scaled % divisor;
	if (remainder * 2U > divisor ||
	    (remainder * 2U == divisor &&
	    (significand & 1U) != 0))
		significand++;
	if (significand == (1U << 24)) {
		significand >>= 1;
		exponent++;
	}

	/* Succeeded: finite copied rational fields have one complete IEEE word and no kernel FP instruction. */
	return ((uint32_t)(exponent + 127) << 23) | ((uint32_t)significand & 0x7fffffU);
}

/* Tells whether one copy region is a whole-image copy that the tile buffer can perform unchanged. */
static bool
copy_whole(
	const struct bcm2711_vulkan_transfer *transfer,
	uint32_t region,
	const struct bcm2711_vulkan_resource *source,
	const struct bcm2711_vulkan_resource *destination)
{
	const struct bcm2711_vulkan_transfer_region *box;
	uint32_t axis;

	/* Blits sample and may scale or convert; only a raw copy keeps every byte. */
	if (transfer->record.opcode != GPU_OP_CMD_COPY_IMAGE)
		return false;

	/* Copying an image onto itself would load tiles the same pass stores. */
	if (source == destination)
		return false;

	/* The tile load and store keep bytes, so both images share one implemented 32-bit format. */
	if (source->format != destination->format)
		return false;
	if (source->format != VK_FORMAT_R8G8B8A8_UNORM && source->format != VK_FORMAT_B8G8R8A8_UNORM)
		return false;

	/* Tiles load and store at the same coordinates, so both images have one extent. */
	if (source->width != destination->width || source->height != destination->height)
		return false;

	/* Raster loads and stores need 64-byte aligned pitches the 32-bit address space can hold. */
	if ((source->pitch & 63U) != 0 || (destination->pitch & 63U) != 0)
		return false;
	if (source->bytes > 0xffffffffU || destination->bytes > 0xffffffffU)
		return false;

	/* Whole tiles are stored, so the region covers the whole destination from the same origin. */
	box = &transfer->regions[region];
	for (axis = 0; axis < 2U; axis++) {
		/* The source and destination boxes start at the origin of both images. */
		if (box->source[axis] != 0 || box->destination[axis] != 0)
			return false;
	}

	/* The boxes end at the images' full extent. */
	if (box->source[2] != (int32_t)destination->width || box->source[3] != (int32_t)destination->height)
		return false;
	if (box->destination[2] != (int32_t)destination->width || box->destination[3] != (int32_t)destination->height)
		return false;

	/* Succeeded: the tile buffer can copy the whole image byte for byte. */
	return true;
}

/* Prepares a zero-draw pass that loads every tile from the source and stores it to the destination. */
static int
prepare_copy(
	struct bcm2711_vulkan_native_pass *pass,
	const struct bcm2711_vulkan_transfer *transfer,
	struct bcm2711_vulkan_resource *source,
	struct bcm2711_vulkan_resource *destination,
	uint64_t *remaining)
{
	struct bcm2711_v3d_view *view;
	struct bcm2711_buffer *target;
	VkRect2D area;
	uint64_t target_offset;
	uint32_t address;
	uint32_t boxes[BCM2711_NATIVE_PASS_BOXES][4];
	uint32_t count;
	bool changed;
	void *cpu;
	int error;

	/* The destination is the pass's output over its whole extent. */
	area.offset.x = 0;
	area.offset.y = 0;
	area.extent.width = destination->width;
	area.extent.height = destination->height;
	error = prepare_output(pass, destination, destination, 0, transfer->destination_layout, &area);
	if (error != 0)
		return error;

	/* The source's backing must be representable before its address enters the tile loads. */
	error = bcm2711_vulkan_resource_backing(source, 0, source->bytes, &view, &address, &cpu);
	if (error != 0)
		return error;
	if (view->references == 0 || view->references == 0xffffffffU)
		return EOVERFLOW;

	/*
	 * When the destination still equals this source at an earlier
	 * generation, only the tiles the source changed since then are copied;
	 * the other tiles of both images are already equal.  A CPU mapping of
	 * either run could change bytes without a recorded write.
	 */
	pass->sync = true;
	pass->sync_identity = view->buffer->identity;
	pass->sync_offset = address - view->address;
	pass->sync_generation = view->buffer->generation;
	target = pass->output->buffer;
	target_offset = (uint64_t)pass->state.output - pass->output->address;
	changed = false;
	if (!target->host_mapped &&
	    !view->buffer->host_mapped &&
	    view->buffer->identity != 0 &&
	    target->synced_identity == view->buffer->identity &&
	    target->synced_offset == pass->sync_offset &&
	    target->synced_target_offset == target_offset &&
	    target->synced_generation == target->generation)
		changed = bcm2711_v3d_memory_changes_boxes(view->buffer, pass->sync_offset, target->synced_source_generation, boxes, BCM2711_NATIVE_PASS_BOXES, &count);

	/* Nothing changed: the destination already equals the source. */
	if (changed && count == 0)
		return EALREADY;

	/*
	 * A known change narrows the copy to the box around the changed boxes,
	 * clamped to the image, and to the tiles of the boxes themselves:
	 * a frame that drew two places apart copies the two.
	 */
	if (changed)
		copy_narrow(pass, destination, boxes, count);

	/* The pass holds the source's view through native retirement, like its output. */
	bcm2711_v3d_memory_retain(view);
	pass->source = view;
	pass->state.source = address;
	pass->state.source_pitch = source->pitch;
	pass->state.source_bytes = (uint32_t)source->bytes;

	/* The lists hold no draw: each tile is loaded from the source and stored. */
	error = bcm2711_vulkan_native_pass_build_lists(pass, remaining);
	if (error != 0)
		return error;

	/* Succeeded: the complete copy pass owns its lists and both image views. */
	return 0;
}

/* Tells whether a buffer-image copy is one whole-image region whose buffer rows a raster tile load or store can address. */
static int
buffer_image_whole(
	const struct bcm2711_vulkan_record *record,
	struct bcm2711_vulkan_resource **image,
	struct bcm2711_vulkan_resource **buffer,
	uint64_t *offset,
	uint32_t *pitch)
{
	const struct bcm2711_vulkan_buffer_copy *copy;
	const VkBufferImageCopy *region;
	uint32_t image_slot;
	uint32_t row_length;

	/* Only uploads and readbacks between one image and one buffer qualify. */
	if (record->opcode != GPU_OP_CMD_COPY_BUFFER_TO_IMAGE && record->opcode != GPU_OP_CMD_COPY_IMAGE_TO_BUFFER)
		return ENOTSUP;
	if (record->count != 1 || record->objects[0] == NULL || record->objects[1] == NULL)
		return ENOTSUP;
	image_slot = 0;
	if (record->opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE)
		image_slot = 1;
	if (record->objects[image_slot]->kind != I915_VK_OBJ_IMAGE || record->objects[1U - image_slot]->kind != I915_VK_OBJ_BUFFER)
		return ENOTSUP;
	*image = record->objects[image_slot]->payload;
	*buffer = record->objects[1U - image_slot]->payload;
	if (*image == NULL || *buffer == NULL)
		return ENOTSUP;

	/* The tiles keep bytes, so only the implemented 32-bit colour formats qualify. */
	if ((*image)->format != VK_FORMAT_R8G8B8A8_UNORM && (*image)->format != VK_FORMAT_B8G8R8A8_UNORM)
		return ENOTSUP;

	/* The region is the whole single-layer colour image from its origin. */
	copy = (const struct bcm2711_vulkan_buffer_copy *)record;
	region = &copy->regions.images[0];
	if (region->imageSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
	    region->imageSubresource.mipLevel != 0 ||
	    region->imageSubresource.baseArrayLayer != 0 ||
	    region->imageSubresource.layerCount != 1)
		return ENOTSUP;
	if (region->imageOffset.x != 0 || region->imageOffset.y != 0 || region->imageOffset.z != 0)
		return ENOTSUP;
	if (region->imageExtent.width != (*image)->width ||
	    region->imageExtent.height != (*image)->height ||
	    region->imageExtent.depth != 1)
		return ENOTSUP;

	/* Buffer rows start 64-byte aligned at a 64-byte multiple pitch the raster field can hold. */
	row_length = region->bufferRowLength;
	if (row_length == 0)
		row_length = (*image)->width;
	if (row_length < (*image)->width || row_length > 0xfffffU / 4U)
		return ENOTSUP;
	if (((row_length * 4U) & 63U) != 0 || (region->bufferOffset & 63U) != 0)
		return ENOTSUP;
	if (((*image)->pitch & 63U) != 0 || (*image)->bytes > 0xffffffffU)
		return ENOTSUP;

	/* The buffer holds every row of the image at its pitch. */
	if (region->bufferOffset > (*buffer)->bytes ||
	    (uint64_t)row_length * 4U * (*image)->height > (*buffer)->bytes - region->bufferOffset)
		return ENOTSUP;
	if ((uint64_t)row_length * 4U * (*image)->height > 0xffffffffU)
		return ENOTSUP;

	/* Succeeded: one tile pass copies the whole image byte for byte. */
	*offset = region->bufferOffset;
	*pitch = row_length * 4U;
	return 0;
}

/* Resolves and holds one raster interval of a resource for a native pass. */
static int
retain_raster(
	struct bcm2711_vulkan_resource *resource,
	uint64_t offset,
	uint64_t bytes,
	struct bcm2711_v3d_view **view,
	uint32_t *address,
	void **cpu)
{
	int error;

	/* The view must be representable before its address enters a tile packet. */
	*view = NULL;
	error = bcm2711_vulkan_resource_backing(resource, offset, bytes, view, address, cpu);
	if (error != 0) {
		*view = NULL;
		return error;
	}
	if ((*view)->references == 0 || (*view)->references == 0xffffffffU) {
		*view = NULL;
		return EOVERFLOW;
	}

	/* The pass holds the view through native retirement. */
	bcm2711_v3d_memory_retain(*view);

	/* Succeeded: the caller's pass owns one reference of the view. */
	return 0;
}

/*
 * Narrows a whole-image copy pass to the changed boxes of its source
 * (pixels, left, top, right, bottom): its area to the box around them,
 * clamped to the destination, and its tiles to the boxes' tiles.
 * Boxes left with nothing inside the image leave the copy
 * whole.
 */
static void
copy_narrow(
	struct bcm2711_vulkan_native_pass *pass,
	const struct bcm2711_vulkan_resource *destination,
	uint32_t (*boxes)[4],
	uint32_t count)
{
	uint32_t around[4];
	uint32_t index;
	uint32_t kept;

	/* Each box clamped to the image; empty ones left out; the box around the rest. */
	kept = 0;
	around[0] = UINT32_MAX;
	around[1] = UINT32_MAX;
	around[2] = 0;
	around[3] = 0;
	for (index = 0; index < count; index++) {
		/* The box inside the image. */
		if (boxes[index][2] > destination->width)
			boxes[index][2] = destination->width;
		if (boxes[index][3] > destination->height)
			boxes[index][3] = destination->height;
		if (boxes[index][0] >= boxes[index][2] || boxes[index][1] >= boxes[index][3])
			continue;

		/* Kept, as its tiles, and in the box around them. */
		pass->state.boxes[kept][0] = boxes[index][0] / 64U;
		pass->state.boxes[kept][1] = boxes[index][1] / 64U;
		pass->state.boxes[kept][2] = (boxes[index][2] - 1U) / 64U;
		pass->state.boxes[kept][3] = (boxes[index][3] - 1U) / 64U;
		kept++;
		if (boxes[index][0] < around[0])
			around[0] = boxes[index][0];
		if (boxes[index][1] < around[1])
			around[1] = boxes[index][1];
		if (boxes[index][2] > around[2])
			around[2] = boxes[index][2];
		if (boxes[index][3] > around[3])
			around[3] = boxes[index][3];
	}

	/* Nothing inside the image: the copy stays whole, as before. */
	if (kept == 0)
		return;

	/* The area and the rectangle of tiles around the boxes. */
	pass->area.offset.x = (int32_t)around[0];
	pass->area.offset.y = (int32_t)around[1];
	pass->area.extent.width = around[2] - around[0];
	pass->area.extent.height = around[3] - around[1];
	pass->state.first_x = around[0] / 64U;
	pass->state.first_y = around[1] / 64U;
	pass->state.last_x = (around[2] - 1U) / 64U;
	pass->state.last_y = (around[3] - 1U) / 64U;

	/* One box is the rectangle itself; more select their own tiles. */
	pass->state.box_count = 0;
	if (kept > 1U)
		pass->state.box_count = kept;
}
