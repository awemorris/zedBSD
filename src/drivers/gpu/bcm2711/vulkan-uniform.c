/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* FIFO-time scalar uniforms preserve consumed order and independently cloned ordinary descriptor intervals. */
#include <kern/kcrt.h>
#include <kern/kmem.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/native-viewport.h"
#include "drivers/gpu/bcm2711/vulkan-uniform.h"

/* A finite stream comfortably covers the compiler's current 8192-instruction, eight-uniform-per-instruction bound. */
#define VULKAN_UNIFORM_BYTES (1024U * 1024U)

static int prepare_words(const struct bcm2711_vulkan_prepared_event *draw, uint32_t stage, const struct bcm2711_vulkan_native_binding bindings[BCM2711_VULKAN_PIPELINE_SETS][BCM2711_VULKAN_LAYOUT_BINDINGS], struct bcm2711_vulkan_uniform_words *words);
static int prepare_word(const struct bcm2711_vulkan_prepared_event *draw, uint32_t stage, const struct bcm2711_vulkan_native_binding bindings[BCM2711_VULKAN_PIPELINE_SETS][BCM2711_VULKAN_LAYOUT_BINDINGS], const struct bcm2711_native_viewport *viewport, const struct bcm2711_shader_uniform *uniform, uint32_t *word);
static int resolve_descriptor(const struct bcm2711_vulkan_prepared_event *draw, const struct bcm2711_shader_uniform *uniform, VkDescriptorType type, const struct bcm2711_vulkan_descriptor **descriptor, uint32_t *slot);
static int read_block(const struct bcm2711_vulkan_descriptor *descriptor, const struct bcm2711_shader_uniform *uniform, uint32_t *word);

/*
 * Creates a complete independent CPU stream in exact compiled native consumption order.
 *
 * UBO contents are read now, after earlier queue work has executed.  The
 * prepared pending primary still owns descriptor clones and all input
 * lifetimes; this scalar result neither launches DMA nor retains GPU record
 * storage.  A failure releases the whole unpublished word prefix.
 */
int
bcm2711_vulkan_uniform_create(
	const struct bcm2711_vulkan_prepared_event *draw,
	uint32_t stage,
	const struct bcm2711_vulkan_native_binding bindings[BCM2711_VULKAN_PIPELINE_SETS][BCM2711_VULKAN_LAYOUT_BINDINGS],
	struct bcm2711_vulkan_uniform_words **words)
{
	const struct bcm2711_shader_binary *program;
	struct bcm2711_vulkan_uniform_words *created;
	int error;

	/* Every refusal leaves the caller without an incomplete scalar owner. */
	if (words == NULL)
		return EINVAL;
	*words = NULL;

	/* Only a real prepared draw can supply immutable pipeline and stage-specific copied state. */
	if (draw == NULL ||
	    draw->opcode != GPU_OP_CMD_DRAW ||
	    draw->pipeline == NULL)
		return EINVAL;

	/* Native coordinates, render vertices and fragments occupy exactly three compiled slots. */
	if (stage >= 3U)
		return EINVAL;
	program = draw->pipeline->programs[stage];

	/* Matching compiler stage metadata cannot be replaced with another slot's scalar consumption. */
	if (program == NULL || (uint32_t)program->stage != stage)
		return EINVAL;

	/* Count and allocation arithmetic are bounded before reading any compiler metadata array. */
	if (program->uniform_count > VULKAN_UNIFORM_BYTES / sizeof(uint32_t))
		return ENOTSUP;

	/* Nonempty compiler streams own an actual immutable uniform description array. */
	if (program->uniform_count != 0 && program->uniforms == NULL)
		return EINVAL;

	/* One zeroed root keeps every partial construction safely releasable. */
	created = kern_calloc(1, sizeof(*created));
	if (created == NULL)
		return ENOMEM;
	created->count = program->uniform_count;

	/* Empty streams require no scalar array; later native upload still supplies an aligned owned placeholder. */
	if (created->count != 0) {
		created->words = kern_calloc(created->count, sizeof(*created->words));
		if (created->words == NULL) {
			bcm2711_vulkan_uniform_release(created);
			return ENOMEM;
		}
	}

	/* Complete state and every late descriptor/native address refusal are checked before publication. */
	error = prepare_words(draw, stage, bindings, created);
	if (error != 0) {
		bcm2711_vulkan_uniform_release(created);
		return error;
	}

	/* Only the complete independently copied scalar sequence becomes caller-owned. */
	*words = created;

	/* Succeeded: the native job can upload these exact words without retaining a command arena or UBO CPU pointer. */
	return 0;
}

/*
 * Releases one complete or unpublished CPU uniform stream without altering native DMA ownership.
 */
void
bcm2711_vulkan_uniform_release(
	struct bcm2711_vulkan_uniform_words *words)
{
	/* Absent streams acquired no CPU storage. */
	if (words == NULL)
		return;

	/* These copied words own no GPU mapping and can retire after upload or any pre-launch refusal. */
	kern_free(words->words);
	kern_free(words);

	/* Succeeded: the retired draw has no uniform binding left. */
	return;
}

/* Resolves each consumed word once after the exact dynamic viewport is checked. */
static int
prepare_words(
	const struct bcm2711_vulkan_prepared_event *draw,
	uint32_t stage,
	const struct bcm2711_vulkan_native_binding bindings[BCM2711_VULKAN_PIPELINE_SETS][BCM2711_VULKAN_LAYOUT_BINDINGS],
	struct bcm2711_vulkan_uniform_words *words)
{
	const struct bcm2711_shader_binary *program;
	struct bcm2711_native_viewport viewport;
	uint32_t index;
	int error;

	/* Integer viewport preparation preserves the native fixed-point XY and Vulkan depth conventions without kernel FP. */
	error = bcm2711_native_viewport_prepare(draw->viewport, &viewport);
	if (error != 0)
		return error;

	/* Every stream word follows actual emitted consumption, including repeated constants and repeated block reads. */
	program = draw->pipeline->programs[stage];
	for (index = 0; index < words->count; index++) {
		error = prepare_word(draw, stage, bindings, &viewport, &program->uniforms[index], &words->words[index]);
		if (error != 0)
			return error;
	}

	/* Succeeded: no scalar in this complete unpublished stream depends on a later public identity lookup. */
	return 0;
}

/* Converts one compiler-consumed constant, pushed scalar, UBO word or owned descriptor pointer into exact bits. */
static int
prepare_word(
	const struct bcm2711_vulkan_prepared_event *draw,
	uint32_t stage,
	const struct bcm2711_vulkan_native_binding bindings[BCM2711_VULKAN_PIPELINE_SETS][BCM2711_VULKAN_LAYOUT_BINDINGS],
	const struct bcm2711_native_viewport *viewport,
	const struct bcm2711_shader_uniform *uniform,
	uint32_t *word)
{
	const struct bcm2711_vulkan_descriptor *descriptor;
	uint32_t push_stage;
	uint32_t slot;
	uint32_t address;
	int error;

	/* Coordinate and vertex programs consume the vertex push snapshot; fragment pushes are independently copied. */
	push_stage = 0;
	if (stage == BCM2711_SHADER_FRAGMENT)
		push_stage = 1;

	/* Exact kinds determine which copied state or retained descriptor interval contributes this native word. */
	switch (uniform->kind) {
	case BCM2711_SHADER_CONSTANT:
		*word = uniform->bits;
		break;
	case BCM2711_SHADER_PUSH:
		/* Earlier draw validation checked exact layout provenance; this read still bounds the immutable copied array. */
		if ((uniform->offset & 3U) != 0 || uniform->offset >= 128U)
			return EINVAL;
		*word = draw->push[push_stage][uniform->offset / 4U];
		break;
	case BCM2711_SHADER_BLOCK:
		error = resolve_descriptor(draw, uniform, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &descriptor, &slot);
		if (error != 0)
			return error;
		error = read_block(descriptor, uniform, word);
		if (error != 0)
			return error;
		break;
	case BCM2711_SHADER_TEXTURE:
	case BCM2711_SHADER_SAMPLER:
		/* Internal texture shaders use exactly one staged binding and require no fabricated user layout or descriptor. */
		if (draw->meta != NULL) {
			if (uniform->set != 0 ||
			    uniform->binding != 0 ||
			    bindings == NULL)
				return EINVAL;
			slot = 0;
		} else {
			error = resolve_descriptor(draw, uniform, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &descriptor, &slot);
			if (error != 0)
				return error;

			/* Both retained components precede any ordinary numerical binding selection. */
			if (descriptor->view == NULL ||
			    descriptor->sampler == NULL ||
			    bindings == NULL)
				return EINVAL;
		}

		/* Select the independently uploaded numerical record after either complete interface validation. */
		address = bindings[uniform->set][slot].texture;
		if (uniform->kind == BCM2711_SHADER_SAMPLER)
			address = bindings[uniform->set][slot].sampler;

		/* Native descriptor slots reserve five low bits for exact compiler configuration flags. */
		if (address == 0 || (address & 31U) != 0)
			return EINVAL;

		/* Whole 24-byte records cannot extend past the encoded native VA address space. */
		if (address > 0xffffffffU - 23U || uniform->bits > 31U)
			return EINVAL;
		*word = address | uniform->bits;
		break;
	case BCM2711_SHADER_VIEWPORT_X:
		*word = viewport->x_scale;
		break;
	case BCM2711_SHADER_VIEWPORT_Y:
		*word = viewport->y_scale;
		break;
	case BCM2711_SHADER_VIEWPORT_Z:
		*word = viewport->depth_scale;
		break;
	case BCM2711_SHADER_DEPTH_OFFSET:
		*word = viewport->depth_offset;
		break;
	default:
		return ENOTSUP;
	}

	/* Succeeded: one exact native consumption has no retained CPU source pointer. */
	return 0;
}

/* Resolves one cloned canonical binding through immutable pipeline layout metadata, without consulting public set identities. */
static int
resolve_descriptor(
	const struct bcm2711_vulkan_prepared_event *draw,
	const struct bcm2711_shader_uniform *uniform,
	VkDescriptorType type,
	const struct bcm2711_vulkan_descriptor **descriptor,
	uint32_t *slot)
{
	const struct bcm2711_vulkan_pipeline_layout *layout;
	const struct bcm2711_vulkan_set_layout *set_layout;
	uint32_t index;

	/* Missing immutable layout ownership cannot supply a native descriptor definition. */
	*descriptor = NULL;
	if (draw->pipeline->owner.parent == NULL || draw->pipeline->owner.parent->payload == NULL)
		return EINVAL;
	layout = draw->pipeline->owner.parent->payload;

	/* Exact set bounds precede any array access or binding-mask shift. */
	if (uniform->set >= layout->count || uniform->set >= BCM2711_VULKAN_PIPELINE_SETS)
		return EINVAL;

	/* Each referenced set retains an actual typed immutable interface through the pending primary graph. */
	if (layout->sets[uniform->set] == NULL || layout->sets[uniform->set]->payload == NULL)
		return EINVAL;
	set_layout = layout->sets[uniform->set]->payload;

	/* Canonical slots stay inside the finite cloned descriptor array. */
	if (set_layout->count > BCM2711_VULKAN_LAYOUT_BINDINGS)
		return EINVAL;

	/* Declaration order never substitutes for the compiler's exact Vulkan binding number. */
	for (index = 0; index < set_layout->count; index++) {
		/* Absent declared bindings contribute no native scalar input. */
		if (set_layout->bindings[index].number != uniform->binding)
			continue;

		/* A different descriptor class cannot reinterpret the cloned resource payload. */
		if (set_layout->bindings[index].type != type)
			return EINVAL;

		/* Only successfully acquired consumed descriptor clones may be read at native execution. */
		if ((draw->used[uniform->set] & (1U << index)) == 0)
			return EINVAL;
		*descriptor = &draw->descriptors[uniform->set][index];
		*slot = index;

		/* The matching clone has been resolved; later declarations cannot replace its exact binding. */
		break;
	}

	/* An absent exact binding refuses the complete unpublished scalar stream. */
	if (*descriptor == NULL)
		return EINVAL;

	/* Succeeded: this borrowed clone stays owned by the prepared primary through native retirement. */
	return 0;
}

/* Reads one current coherent UBO word after checking its exact descriptor and logical resource intervals. */
static int
read_block(
	const struct bcm2711_vulkan_descriptor *descriptor,
	const struct bcm2711_shader_uniform *uniform,
	uint32_t *word)
{
	struct bcm2711_vulkan_resource *buffer;
	struct bcm2711_v3d_view *view;
	const uint8_t *source;
	void *cpu;
	uint64_t offset;
	uint32_t address;
	int error;

	/* Exact typed buffer ownership and naturally aligned word metadata are required before payload access. */
	if (descriptor->buffer == NULL || (uniform->offset & 3U) != 0)
		return EINVAL;

	/* An image or another typed object cannot supply a UBO word through the same numeric identity. */
	if (descriptor->buffer->kind != I915_VK_OBJ_BUFFER || descriptor->buffer->payload == NULL)
		return EINVAL;

	/* Logical descriptor range checks exclude rounded allocation padding and another binding's neighbouring data. */
	if (uniform->offset > descriptor->bytes || 4U > descriptor->bytes - uniform->offset)
		return EINVAL;

	/* The full absolute word offset is checked before adding its descriptor and compiled static offsets. */
	if (uniform->offset > UINT64_MAX - descriptor->offset)
		return EOVERFLOW;
	offset = descriptor->offset + uniform->offset;
	buffer = descriptor->buffer->payload;

	/* Current coherent backing must cover this exact four-byte logical buffer interval. */
	error = bcm2711_vulkan_resource_backing(buffer, offset, 4, &view, &address, &cpu);
	if (error != 0)
		return error;

	/* The native shader word follows the target's little-endian user ABI without an unaligned CPU load. */
	source = cpu;
	*word = (uint32_t)source[0];
	*word |= (uint32_t)source[1] << 8;
	*word |= (uint32_t)source[2] << 16;
	*word |= (uint32_t)source[3] << 24;

	/* Succeeded: prior queue writes are observed now, without retaining the mapped UBO CPU address in the result. */
	return 0;
}
