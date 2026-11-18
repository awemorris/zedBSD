/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Internal texture quads preserve copy bits or perform blit sampling, without CPU destination pixel writes. */
#include <kern/device-io.h>
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/videocore/vk/vulkan-native-image.h"

/* Each full resident native image transfer uses the same padded submission budget as ordinary draws. */
#define NATIVE_IMAGE_BYTES (256ULL * 1024U * 1024U)

#include "drivers/gpu/videocore/vk/meta/texture-words.inc"

/* Two counterclockwise triangles cover the entire private viewport; each pair selects one source UV corner. */
static const uint32_t meta_corners[6][2] = {
    {0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};

static int prepare_meta(struct bcm2711_vulkan_meta_draw *meta, const struct bcm2711_vulkan_transfer *transfer, uint32_t region);
static int prepare_programs(struct bcm2711_vulkan_meta_draw *meta, VkFormat destination);
static void release_meta(struct bcm2711_vulkan_meta_draw *meta);
static int prepare_output(struct bcm2711_vulkan_native_pass *pass, struct bcm2711_vulkan_resource *image, struct bcm2711_vulkan_resource *target, uint32_t level, uint32_t layer, VkImageLayout layout, const VkRect2D *area);
static int buffer_image_whole(const struct bcm2711_vulkan_record *record, struct bcm2711_vulkan_resource **image, struct bcm2711_vulkan_resource **buffer, uint64_t *offset, uint32_t *pitch);
static int retain_raster(struct bcm2711_vulkan_resource *resource, uint64_t offset, uint64_t bytes, struct bcm2711_v3d_view **view, uint32_t *address, void **cpu);
static bool copy_whole(const struct bcm2711_vulkan_transfer *transfer, uint32_t region, const struct bcm2711_vulkan_resource *source, const struct bcm2711_vulkan_resource *destination);
static int prepare_copy(struct bcm2711_vulkan_native_pass *pass, const struct bcm2711_vulkan_transfer *transfer, struct bcm2711_vulkan_resource *source, struct bcm2711_vulkan_resource *destination, uint64_t *remaining);
static int prepare_resolve(struct bcm2711_vulkan_native_pass *pass, const struct bcm2711_vulkan_transfer *transfer, struct bcm2711_vulkan_resource *source, struct bcm2711_vulkan_resource *destination, uint64_t *remaining);
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
	error = bcm2711_vulkan_image_level_view(destination, transfer->levels[1], transfer->layers[1], &output);
	if (error != 0)
		return error;
	current = bcm2711_vulkan_image_level_layout(source, transfer->levels[0], transfer->layers[0]);
	if (current != transfer->source_layout || output.layout != transfer->destination_layout)
		return EINVAL;

	/* One complete output owner exists before compiler, texture or numerical list allocations. */
	remaining = *available;
	created = kern_calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->space = space;
	created->job.kind = BCM2711_V3D_JOB_CL;

	/* A resolve loads the multisampled source's samples into the tile buffer and stores their average. */
	if (transfer->record.opcode == GPU_OP_CMD_RESOLVE_IMAGE) {
		error = prepare_resolve(created, transfer, source, destination, &remaining);
		if (error != 0) {
			released = bcm2711_vulkan_native_pass_release(&created, true);
			if (released != 0)
				return released;
			return error;
		}

		/* Only a complete native pass consumes the caller's padded budget. */
		*available = remaining;
		*pass = created;
		return 0;
	}

	/*
	 * A whole-image copy between equal raster images needs no shader or
	 * texture: the tile buffer loads the source and stores the destination.
	 */
	whole = false;
	if (transfer->levels[0] == 0 &&
	    transfer->levels[1] == 0 &&
	    transfer->layers[0] == 0 &&
	    transfer->layers[1] == 0)
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
		error = prepare_output(created, &output, destination, transfer->levels[1], transfer->layers[1], transfer->destination_layout, &meta->area);
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
	struct bcm2711_vulkan_native_target *target;
	struct bcm2711_native_pass_target *native;
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
	target = &created->targets[0];
	native = &created->state.target[0];
	target->image = image;
	target->width = image->width;
	target->height = image->height;
	target->initial_layout = record->layout;
	target->final_layout = record->layout;
	target->format = image->format;
	target->load = VK_ATTACHMENT_LOAD_OP_LOAD;
	created->area.offset.x = 0;
	created->area.offset.y = 0;
	created->area.extent.width = image->width;
	created->area.extent.height = image->height;
	created->state.width = image->width;
	created->state.height = image->height;
	created->state.targets = 1;
	native->load = 1;
	native->store = 1;
	error = bcm2711_vulkan_native_target_format(native, image->format);

	/* Upload stores into the image from the buffer; readback stores into the buffer from the image. */
	if (error == 0 && record->opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE) {
		error = retain_raster(image, 0, image->bytes, &target->output, &address, &target->cpu);
		native->output = address;
		native->pitch = image->pitch;
		native->output_bytes = (uint32_t)image->bytes;
		if (error == 0)
			error = retain_raster(buffer, offset, span, &view, &address, &cpu);
		created->source = view;
		created->state.source = address;
		created->state.source_pitch = pitch;
		created->state.source_bytes = (uint32_t)span;
	} else if (error == 0) {
		error = retain_raster(buffer, offset, span, &target->output, &address, &target->cpu);
		native->output = address;
		native->pitch = pitch;
		native->output_bytes = (uint32_t)span;
		if (error == 0)
			error = retain_raster(image, 0, image->bytes, &view, &address, &cpu);
		created->source = view;
		created->state.source = address;
		created->state.source_pitch = image->pitch;
		created->state.source_bytes = (uint32_t)image->bytes;
	}

	/* The tiles of the whole image; the lists hold no draw: each tile is loaded from the source and stored. */
	if (error == 0)
		error = bcm2711_vulkan_native_pass_tiles(created);
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

/*
 * Tells whether an image copy or blit runs on the CPU rather than in native passes.
 *
 * RGBA8 and BGRA8 copies are tile copies or texture quads, and colour
 * blits texture quads; every other format's copy, and a depth/stencil or
 * integer colour image's blit (unscaled), keeps its texels' bytes on the
 * CPU.
 */
bool
bcm2711_vulkan_native_image_cpu(
	const struct bcm2711_vulkan_transfer *transfer)
{
	const struct bcm2711_vulkan_resource *source;
	struct bcm2711_vulkan_colour_target target;
	int error;

	/* Only a copy or blit of a known source can be the CPU's. */
	if (transfer == NULL ||
	    (transfer->record.opcode != GPU_OP_CMD_COPY_IMAGE && transfer->record.opcode != GPU_OP_CMD_BLIT_IMAGE) ||
	    transfer->record.objects[0] == NULL ||
	    transfer->record.objects[0]->payload == NULL)
		return false;
	source = transfer->record.objects[0]->payload;

	/* A depth/stencil image's copy or blit. */
	if (bcm2711_vulkan_depth_format(source->format))
		return true;

	/* A colour blit (but an integer one), and the copies of the formats the native copies keep. */
	if (transfer->record.opcode == GPU_OP_CMD_BLIT_IMAGE) {
		error = bcm2711_vulkan_colour_target(source->format, &target);
		if (error == 0 && target.integer)
			return true;
		return false;
	}
	if (source->format == VK_FORMAT_R8G8B8A8_UNORM || source->format == VK_FORMAT_B8G8R8A8_UNORM)
		return false;

	/* Succeeded: another format's copy is the CPU's. */
	return true;
}

/*
 * Copies every region of an image copy (or an unscaled blit of a
 * depth/stencil or integer colour image) on the CPU, as its texels' bytes.
 *
 * Every earlier pass has retired and published its output, so both
 * images' coherent aliases hold their current texels.  Each image is in
 * the layout the copy names at its level and layer.  A D24S8 image's
 * depth alone keeps the stencil bits of the destination's words, and its
 * stencil alone the depth bits.  The destination's snapshots and copies
 * learn which rectangles changed.
 */
int
bcm2711_vulkan_native_image_copy_run(
	const struct bcm2711_vulkan_transfer *transfer)
{
	const struct bcm2711_vulkan_colour_format *colour;
	const struct bcm2711_vulkan_transfer_region *region;
	struct bcm2711_vulkan_resource *source;
	struct bcm2711_vulkan_resource *destination;
	struct bcm2711_vulkan_resource levels[2];
	struct bcm2711_v3d_view *views[2];
	uint32_t addresses[2];
	uint32_t box[4];
	uint32_t texel;
	uint32_t mask;
	uint32_t index;
	uint32_t width;
	uint32_t height;
	uint32_t x;
	uint32_t y;
	uint8_t part;
	uint8_t *from;
	uint8_t *to;
	void *cpu[2];
	int error;

	/* A validated copy of two bound images of one texel size. */
	if (transfer == NULL ||
	    (transfer->record.opcode != GPU_OP_CMD_COPY_IMAGE && transfer->record.opcode != GPU_OP_CMD_BLIT_IMAGE) ||
	    transfer->record.objects[0] == NULL ||
	    transfer->record.objects[1] == NULL)
		return EINVAL;
	source = transfer->record.objects[0]->payload;
	destination = transfer->record.objects[1]->payload;
	if (source == NULL || destination == NULL)
		return EINVAL;
	error = bcm2711_vulkan_transfer_validate(transfer, source->device);
	if (error != 0)
		return error;
	colour = bcm2711_vulkan_colour_format(source->format);
	texel = source->texel_bytes;
	if (colour != NULL)
		texel = colour->bytes;

	/* The bits of each word a D24S8 copy moves: all of them, or one aspect's. */
	mask = 0xffffffffU;
	if (source->format == VK_FORMAT_D24_UNORM_S8_UINT && transfer->aspects == VK_IMAGE_ASPECT_DEPTH_BIT)
		mask = 0xffffff00U;
	if (source->format == VK_FORMAT_D24_UNORM_S8_UINT && transfer->aspects == VK_IMAGE_ASPECT_STENCIL_BIT)
		mask = 0xffU;

	/* Each image's level in its layer, in the layout the copy names. */
	error = bcm2711_vulkan_image_level_view(source, transfer->levels[0], transfer->layers[0], &levels[0]);
	if (error != 0)
		return error;
	error = bcm2711_vulkan_image_level_view(destination, transfer->levels[1], transfer->layers[1], &levels[1]);
	if (error != 0)
		return error;
	if (levels[0].layout != transfer->source_layout || levels[1].layout != transfer->destination_layout)
		return EINVAL;

	/* Both levels' rows through their coherent aliases. */
	for (index = 0; index < 2U; index++) {
		error = bcm2711_vulkan_resource_backing(&levels[index], 0, levels[index].bytes, &views[index], &addresses[index], &cpu[index]);
		if (error != 0)
			return error;
		if (!views[index]->buffer->uncached)
			return EIO;
		if (levels[index].pitch < levels[index].width * texel)
			return EINVAL;
	}

	/* Each region's rows, byte by byte (explicit bytes avoid an unaligned word access). */
	kern_io_read_barrier();
	for (index = 0; index < transfer->record.count; index++) {
		region = &transfer->regions[index];
		width = (uint32_t)(region->source[2] - region->source[0]);
		height = (uint32_t)(region->source[3] - region->source[1]);
		for (y = 0; y < height; y++) {
			from = (uint8_t *)cpu[0] + ((uint64_t)(uint32_t)region->source[1] + y) * levels[0].pitch + (uint64_t)(uint32_t)region->source[0] * texel;
			to = (uint8_t *)cpu[1] + ((uint64_t)(uint32_t)region->destination[1] + y) * levels[1].pitch + (uint64_t)(uint32_t)region->destination[0] * texel;
			if (mask == 0xffffffffU) {
				for (x = 0; x < width * texel; x++)
					to[x] = from[x];
				continue;
			}

			/* One aspect of a D24S8 word: its bits from the source, the rest kept. */
			for (x = 0; x < width * 4U; x++) {
				part = (uint8_t)(mask >> ((x & 3U) * 8U));
				to[x] = (uint8_t)((to[x] & ~part) | (from[x] & part));
			}
		}

		/* The destination's snapshots and copies learn the rectangle changed. */
		box[0] = (uint32_t)region->destination[0];
		box[1] = (uint32_t)region->destination[1];
		box[2] = (uint32_t)region->destination[2];
		box[3] = (uint32_t)region->destination[3];
		bcm2711_v3d_memory_written(views[1]->buffer, addresses[1] - views[1]->address, box);
	}
	kern_io_write_barrier();

	/* Succeeded: following work sees the copied texels. */
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
	error = bcm2711_vulkan_image_level_view(transfer->record.objects[0]->payload, transfer->levels[0], transfer->layers[0], &meta->level);
	if (error != 0)
		return error;
	meta->image = &meta->level;
	error = bcm2711_vulkan_image_level_view(transfer->record.objects[1]->payload, transfer->levels[1], transfer->layers[1], &destination);
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
	struct bcm2711_vulkan_colour_target colour;
	struct bcm2711_shader_key *key;
	struct bcm2711_shader_diagnostic diagnostic;
	const uint32_t *words;
	size_t count;
	uint32_t index;
	int error;

	/* The destination's format is one the tile buffer stores. */
	error = bcm2711_vulkan_colour_target(destination, &colour);
	if (error != 0)
		return error;

	/*
	 * One exact two-scalar varying interface joins coordinate, render-vertex
	 * and unblended fragment programs, which write target 0 alone: as 32-bit
	 * words for a 32-bit float format, with red and blue swapped for a
	 * BGRA one (a copy keeps the source's order).
	 */
	key = &meta->key;
	kern_memset(key, 0, sizeof(*key));
	key->varying_count = 2;
	key->varyings[1].component = 1;
	key->target_count = 1;
	if (!meta->raw && colour.swap)
		key->swap_red_blue = 1;
	if (colour.wide) {
		key->target_wide = 1;
		key->target_components[0] = colour.components;
	}
	meta->pipeline.front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	meta->pipeline.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	meta->pipeline.colour_skip = 0xfff0U;

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

/* Retains the destination's level (image, a view of target's level in a layer) and preserves all pixels outside the exact destination viewport. */
static int
prepare_output(
	struct bcm2711_vulkan_native_pass *pass,
	struct bcm2711_vulkan_resource *image,
	struct bcm2711_vulkan_resource *target,
	uint32_t level,
	uint32_t layer,
	VkImageLayout layout,
	const VkRect2D *area)
{
	struct bcm2711_vulkan_colour_target colour;
	struct bcm2711_vulkan_native_target *output;
	struct bcm2711_native_pass_target *native;
	struct bcm2711_v3d_view *view;
	uint32_t address;
	void *cpu;
	int error;

	/* The destination's format is one the tile buffer stores. */
	native = &pass->state.target[0];
	error = bcm2711_vulkan_colour_target(image->format, &colour);
	if (error != 0)
		return error;
	error = bcm2711_vulkan_native_target_format(native, image->format);
	if (error != 0)
		return error;

	/* Output backing must be independently representable before a native read/load/store can refer to it. */
	error = bcm2711_vulkan_resource_backing(image, 0, image->bytes, &view, &address, &cpu);
	if (error != 0)
		return error;
	if (view->references == 0 || view->references == 0xffffffffU)
		return EOVERFLOW;
	bcm2711_v3d_memory_retain(view);
	output = &pass->targets[0];
	output->output = view;
	output->cpu = cpu;
	output->image = target;
	output->level = level;
	output->layer = layer;
	output->width = image->width;
	output->height = image->height;
	output->initial_layout = layout;
	output->final_layout = layout;
	output->format = image->format;
	output->swap = colour.swap ? 1U : 0U;
	output->load = VK_ATTACHMENT_LOAD_OP_LOAD;

	/* Full framebuffer dimensions and selected tile bounds preserve neighbouring pixels in partial/grouped tiles. */
	pass->state.width = image->width;
	pass->state.height = image->height;
	pass->state.targets = 1;
	native->pitch = image->pitch;
	native->output = address;
	native->output_bytes = (uint32_t)image->bytes;
	native->load = 1;
	native->store = 1;
	pass->area = *area;
	error = bcm2711_vulkan_native_pass_tiles(pass);
	if (error != 0)
		return error;

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
	struct bcm2711_vulkan_colour_target colour;
	uint32_t axis;
	int error;

	/* Blits sample and may scale or convert; only a raw copy keeps every byte. */
	if (transfer->record.opcode != GPU_OP_CMD_COPY_IMAGE)
		return false;

	/* Copying an image onto itself would load tiles the same pass stores. */
	if (source == destination)
		return false;

	/* The tile load and store keep bytes, so both images share one format the tile buffer holds exactly. */
	if (source->format != destination->format)
		return false;
	error = bcm2711_vulkan_colour_target(source->format, &colour);
	if (error != 0 || !colour.exact)
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
	error = prepare_output(pass, destination, destination, 0, 0, transfer->destination_layout, &area);
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
	target = pass->targets[0].output->buffer;
	target_offset = (uint64_t)pass->state.target[0].output - pass->targets[0].output->address;
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

/*
 * Prepares a zero-draw pass in the 4x multisample mode that loads every
 * tile's samples from the multisampled source and stores their average
 * into the destination, or an integer format's sample 0 (Mesa's v3dv
 * resolves with the tile buffer the same way, a store of decimation 4x,
 * or of decimation sample 0 for an integer format: v3dvx_meta_common.c
 * emit_image_store, Decimate Mode of src/broadcom/cle/v3d_packet.xml).
 */
static int
prepare_resolve(
	struct bcm2711_vulkan_native_pass *pass,
	const struct bcm2711_vulkan_transfer *transfer,
	struct bcm2711_vulkan_resource *source,
	struct bcm2711_vulkan_resource *destination,
	uint64_t *remaining)
{
	struct bcm2711_vulkan_colour_target colour;
	struct bcm2711_v3d_view *view;
	VkRect2D area;
	uint32_t address;
	void *cpu;
	int error;

	/* The destination is the pass's output over its whole extent, stored as the samples' average. */
	area.offset.x = 0;
	area.offset.y = 0;
	area.extent.width = destination->width;
	area.extent.height = destination->height;
	error = prepare_output(pass, destination, destination, 0, 0, transfer->destination_layout, &area);
	if (error != 0)
		return error;

	/* The source's samples, held through native retirement like the output. */
	error = bcm2711_vulkan_resource_backing(source, 0, source->bytes, &view, &address, &cpu);
	if (error != 0)
		return error;
	if (view->references == 0 || view->references == 0xffffffffU)
		return EOVERFLOW;
	if (source->bytes > 0xffffffffU)
		return EINVAL;
	bcm2711_v3d_memory_retain(view);
	pass->source = view;
	pass->state.source = address;
	pass->state.source_pitch = source->pitch;
	pass->state.source_bytes = (uint32_t)source->bytes;

	/* The tile buffer in its 4x mode, the output's store resolving (an integer format's to sample 0), and the tile size of four samples. */
	error = bcm2711_vulkan_colour_target(destination->format, &colour);
	if (error != 0)
		return error;
	pass->state.msaa = 1;
	pass->state.target[0].resolve = 1;
	if (colour.integer)
		pass->state.target[0].resolve = 2;
	error = bcm2711_vulkan_native_pass_tiles(pass);
	if (error != 0)
		return error;

	/* The lists hold no draw: each tile's samples are loaded and their average stored. */
	error = bcm2711_vulkan_native_pass_build_lists(pass, remaining);
	if (error != 0)
		return error;

	/* Succeeded: the complete resolve pass owns its lists and both image views. */
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
		pass->state.boxes[kept][0] = boxes[index][0] / pass->state.tile_width;
		pass->state.boxes[kept][1] = boxes[index][1] / pass->state.tile_height;
		pass->state.boxes[kept][2] = (boxes[index][2] - 1U) / pass->state.tile_width;
		pass->state.boxes[kept][3] = (boxes[index][3] - 1U) / pass->state.tile_height;
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
	pass->state.first_x = around[0] / pass->state.tile_width;
	pass->state.first_y = around[1] / pass->state.tile_height;
	pass->state.last_x = (around[2] - 1U) / pass->state.tile_width;
	pass->state.last_y = (around[3] - 1U) / pass->state.tile_height;

	/* One box is the rectangle itself; more select their own tiles. */
	pass->state.box_count = 0;
	if (kept > 1U)
		pass->state.box_count = kept;
}
