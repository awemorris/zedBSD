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

#include "drivers/gpu/bcm2711/vulkan-native-image.h"

/* Each full resident native image transfer uses the same padded submission budget as ordinary draws. */
#define NATIVE_IMAGE_BYTES (256ULL * 1024U * 1024U)

#include "drivers/gpu/bcm2711/meta/texture-words.inc"

/* Two counterclockwise triangles cover the entire private viewport; each pair selects one source UV corner. */
static const uint32_t meta_corners[6][2] = {
    {0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};

static int prepare_meta(struct bcm2711_vulkan_meta_draw *meta, const struct bcm2711_vulkan_transfer *transfer, uint32_t region);
static int prepare_programs(struct bcm2711_vulkan_meta_draw *meta, VkFormat destination);
static void release_meta(struct bcm2711_vulkan_meta_draw *meta);
static int prepare_output(struct bcm2711_vulkan_native_pass *pass, struct bcm2711_vulkan_resource *image, VkImageLayout layout, const VkRect2D *area);
static uint32_t ratio_bits(uint32_t numerator, uint32_t denominator);

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
	uint64_t remaining;
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
	if (source->layout != transfer->source_layout || destination->layout != transfer->destination_layout)
		return EINVAL;

	/* One complete output owner exists before compiler, texture or numerical list allocations. */
	remaining = *available;
	created = kern_calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->space = space;
	created->job.kind = BCM2711_V3D_JOB_CL;
	meta = kern_calloc(1, sizeof(*meta));
	if (meta == NULL) {
		kern_free(created);
		return ENOMEM;
	}

	/* The temporary CPU description retains no namespace object and is never part of the native DMA owner graph. */
	error = prepare_meta(meta, transfer, region);
	if (error == 0)
		error = prepare_output(created, destination, transfer->destination_layout, &meta->area);
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

/* Copies directed source UVs and exact positive destination viewport bounds using only integer IEEE construction. */
static int
prepare_meta(
	struct bcm2711_vulkan_meta_draw *meta,
	const struct bcm2711_vulkan_transfer *transfer,
	uint32_t region_index)
{
	const struct bcm2711_vulkan_transfer_region *region;
	struct bcm2711_vulkan_resource *destination;
	uint32_t source[4];
	uint32_t minimum[2];
	uint32_t maximum[2];
	uint32_t uv[4];
	uint32_t axis;
	uint32_t index;
	uint32_t extent;

	/* Source/destination logical ownership remains on the independently pending primary. */
	region = &transfer->regions[region_index];
	meta->image = transfer->record.objects[0]->payload;
	destination = transfer->record.objects[1]->payload;
	meta->width = destination->width;
	meta->height = destination->height;
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

/* Retains the complete coherent destination and preserves all pixels outside the exact destination viewport. */
static int
prepare_output(
	struct bcm2711_vulkan_native_pass *pass,
	struct bcm2711_vulkan_resource *image,
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
	pass->target = image;
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
