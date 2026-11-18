/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's queue: a submission's command buffers become one
 * command stream in the session's stage memory, which the render node's
 * ring calls (sched.c); the submission returns once the work ended, and
 * its fence is signaled then.
 *
 * The stream asks for the endpoints first, sets the scoreboard entries,
 * and then runs each record in order: copies, fills and clears as loops
 * of the front end's own loads and stores (no shader is needed for
 * them), dispatches as compute runs, and render passes as tiler and
 * fragment runs (vk-draw.c).  Every run is waited for before the next
 * record, so a pipeline barrier has nothing left to do.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/mali/vk/vk-build.h"
#include "drivers/gpu/mali/vk/vk-desc.h"
#include "drivers/gpu/mali/vk/vk-meta.h"
#include "drivers/gpu/mali/vk/vk-meta-spirv.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The most submit infos and command buffers of one submission. */
#define SUBMITS_MAX		64U
#define COMMAND_BUFFERS_MAX	64U

/* The FAU words a program takes at most, and the resource tables of a compute program. */
#define FAU_WORDS_MAX		256U
#define TABLES_COMPUTE		2U
#define TABLES_VERTEX		4U
#define TABLES_IMAGES		(MALI_SHADER_TABLE_IMAGES + 1U)

/*
 * The threads one shader core runs at once, at most, that thread local
 * storage is made for, and the most bytes of it a run takes.  The bound
 * is above any Valhall core's thread count (an inference: Mesa's own
 * fallbacks for Bifrost and Valhall are 384 to 768), so the storage is
 * never short; the GPU's own count is not read here.
 */
#define TLS_THREADS_PER_CORE	2048U
#define TLS_TOTAL_MAX		(64ULL << 20)
#define TLS_ALIGN		4096U

/*
 * The workgroup memory of a dispatch: the workgroups of one core that
 * hold an instance at once are at most the threads TLS_THREADS_PER_CORE
 * says over a workgroup's threads, and no more than the dispatch has; all
 * the instances of all the cores take at most WLS_TOTAL_MAX, fewer
 * instances when they would take more.  A core with fewer instances than
 * the workgroups it could run holds the others back until one is free,
 * so the bound costs speed and never correctness.  The memory does not
 * cross a 4 GiB line.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/lib/pan_desc.h (pan_wls_instances: the
 * product of the workgroup counts each rounded up to a power of two;
 * pan_calc_wls_instances: no more instances than a core runs at once,
 * fewer holding workgroups back; pan_calc_total_wls_size: an instance's
 * bytes times the instances times the cores' number range);
 * src/panfrost/lib/pan_desc.c (pan_emit_tls: the base 4096-byte
 * aligned, inside one 4 GiB part); src/panfrost/vulkan/csf/
 * panvk_vX_cmd_dispatch.c (cmd_dispatch_prepare_tls: the memory taken
 * for each dispatch).
 */
#define WLS_TOTAL_MAX		(32ULL << 20)
#define WLS_ALIGN		4096U

/*
 * A texel of D24_UNORM_S8_UINT: one 32-bit word, the depth's 24-bit
 * normalized value in bits 0..23 and the stencil in bits 24..31 (the
 * framebuffer writes the attachment so, and a copy's depth aspect in a
 * buffer is the same word with its top byte undefined).  The fields'
 * shifts and masks, and the highest 16- and 24-bit normalized values.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/lib/pan_desc.c (translate_zs_format and
 * translate_s_format: the ZS format D24S8 and the stencil format X24S8
 * of one surface for PIPE_FORMAT_Z24_UNORM_S8_UINT, whose name puts the
 * depth in the low 24 bits and the stencil above them, as X24S8 names
 * 24 unused low bits under the stencil); the Vulkan specification 1.0
 * (the buffer layout of a depth aspect of D24_UNORM_S8_UINT as
 * X8_D24_UNORM_PACK32, of a stencil aspect as tightly packed S8_UINT
 * bytes).
 */
#define D24S8_DEPTH_SHIFT	0U
#define D24S8_DEPTH_MASK	0x00ffffffU
#define D24S8_STENCIL_SHIFT	24U
#define D24S8_STENCIL_MASK	0xffU
#define UNORM16_MAX		0xffffU
#define UNORM24_MAX		0x00ffffffU

/* A dispatch's workgroup memory: the instances a core, an instance's bytes, and the base. */
struct workgroup_memory {
	uint32_t instances;
	uint32_t instance_bytes;
	uint64_t base;
};

static int submit(struct mali_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int build_stream(struct mali_vk_session *session, struct mali_vk_command_buffer **commands, uint32_t count, uint64_t *address, uint32_t *bytes);
static void run_records(struct mali_vk_build *build, const struct mali_vk_command_buffer *command);
static void run_dispatch(struct mali_vk_build *build, const struct mali_vk_record *entry);
static uint64_t storage_descriptor(struct mali_vk_build *build, uint32_t tls_bytes, const struct workgroup_memory *wls);
static uint64_t core_range(const struct mali_vk_build *build);
static int workgroup_memory(struct mali_vk_build *build, const struct mali_shader_program *program, const uint32_t *counts, struct workgroup_memory *wls);
static uint32_t power_above(uint32_t value);
static void run_copy_buffer(struct mali_vk_build *build, const struct mali_vk_record *entry);
static void run_fill_buffer(struct mali_vk_build *build, const struct mali_vk_record *entry);
static void run_copy_image(struct mali_vk_build *build, const struct mali_vk_record *entry);
static void run_buffer_image(struct mali_vk_build *build, const struct mali_vk_record *entry, bool to_image);
static void run_clear_image(struct mali_vk_build *build, const struct mali_vk_record *entry);
static void run_clear_depth(struct mali_vk_build *build, const struct mali_vk_record *entry);
static void clear_depth_range(struct mali_vk_build *build, const struct mali_vk_image *image, const VkImageSubresourceRange *range, uint32_t pattern, uint32_t depth, uint32_t stencil);
static uint32_t pack_depth(const struct mali_vk_format *format, uint32_t depth_bits, uint32_t stencil);
static uint32_t format_aspects(const struct mali_vk_format *format);
static void copy_image_aspect(struct mali_vk_build *build, const struct mali_vk_image *source, const struct mali_vk_image *destination, const VkImageCopy *copy, uint32_t layers);
static void buffer_image_aspect(struct mali_vk_build *build, const struct mali_vk_buffer *buffer, const struct mali_vk_image *image, const VkBufferImageCopy *region, uint32_t layers, uint64_t buffer_pitch, uint64_t buffer_layer, bool to_image);
static void run_aspect(struct mali_vk_build *build, const struct mali_vk_meta_aspect *aspect);
static bool blit_is_copy(const VkImageBlit *blit, const struct mali_vk_image *source, const struct mali_vk_image *destination);
static void run_blit(struct mali_vk_build *build, const VkImageBlit *blit, const struct mali_vk_image *source, const struct mali_vk_image *destination, uint32_t filter);
static void copy_span(struct mali_vk_build *build, uint64_t destination, uint64_t source, uint64_t bytes);
static uint32_t pack_color(const struct mali_vk_format *format, const uint32_t *words);
static uint32_t unorm8(uint32_t bits);
static const struct mali_vk_descriptor *find_slot(const struct mali_vk_descriptor_set *const *sets, uint32_t set, uint32_t binding);
static int write_texture(struct mali_vk_build *build, const struct mali_vk_descriptor *slot, volatile uint32_t *texture, volatile uint32_t *sampler);
static int write_view(struct mali_vk_build *build, const struct mali_vk_image_view *view, bool storage, volatile uint32_t *texture);
static uint32_t view_layers(const struct mali_vk_image_view *view);
static uint32_t view_levels(const struct mali_vk_image_view *view);
static bool view_is_cube(const struct mali_vk_image_view *view);
static void store_words(volatile uint32_t *destination, const uint32_t *source, unsigned count);
static uint32_t range_bytes(const struct mali_vk_descriptor *slot);
static void texture_size(const struct mali_vk_descriptor *slot, bool storage, uint32_t *words);

/* Runs the queue's submission. */
int
mali_vk_queue_route(
	struct mali_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Only the submission; the waits are the device's. */
	*handled = 0;
	if (opcode != GPU_OP_QUEUE_SUBMIT)
		return 0;
	*handled = 1;
	error = submit(session, reader, reply);
	if (error != 0)
		return error;

	/* Succeeded: the submission ran. */
	return 0;
}

/* Takes stage memory for the submission being built. */
volatile uint8_t *
mali_vk_build_take(
	struct mali_vk_build *build,
	uint64_t bytes,
	uint64_t align,
	uint64_t *gpu_address)
{
	volatile uint8_t *view;
	int error;

	/* Nothing after a failure. */
	*gpu_address = 0;
	if (build->error != 0)
		return NULL;

	/* The bytes, cleared. */
	error = mali_stage_take(build->session->owner, bytes, align, &view, gpu_address);
	if (error != 0) {
		build->error = error;
		return NULL;
	}
	kern_memset((void *)(uintptr_t)view, 0, (size_t)bytes);

	/* Succeeded: the bytes are the submission's. */
	return view;
}

/* Writes a shader program descriptor into the stage and reports its address. */
uint64_t
mali_vk_build_shader(
	struct mali_vk_build *build,
	unsigned stage,
	uint32_t work_registers,
	uint32_t preload,
	uint64_t code)
{
	uint32_t words[8];
	volatile uint8_t *view;
	uint64_t address;

	/* The descriptor. */
	view = mali_vk_build_take(build, MALI_DESC_BYTES, MALI_DESC_BYTES, &address);
	if (view == NULL)
		return 0;
	mali_desc_shader(words, stage, work_registers, preload, code);
	store_words((volatile uint32_t *)(volatile void *)view, words, 8);

	/* Succeeded: its address. */
	return address;
}

/*
 * Writes a program's FAU words and resource table into the stage.  The
 * FAU pointer carries its count of 64-bit entries in its top byte, the
 * table pointer its count of tables in its low bits.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/vulkan/csf/panvk_vX_cmd_dispatch.c (the FAU
 * pointer with its count at bit 56); src/panfrost/vulkan/
 * panvk_vX_cmd_desc_state.c (the resource table pointer with its count
 * in the low bits, the count a multiple of four).
 */
int
mali_vk_build_resources(
	struct mali_vk_build *build,
	const struct mali_shader_program *program,
	const struct mali_vk_descriptor_set *const *sets,
	const struct mali_vk_graphics *graphics,
	const uint32_t *viewport_words,
	uint64_t system_address,
	uint64_t *fau,
	uint64_t *table)
{
	const struct mali_vk_descriptor *slot;
	const struct mali_vk_buffer *buffer;
	uint32_t words[FAU_WORDS_MAX];
	uint32_t entry[8];
	volatile uint8_t *view;
	volatile uint8_t *tables;
	volatile uint8_t *textures;
	volatile uint8_t *samplers;
	volatile uint8_t *attributes;
	volatile uint8_t *vertex_buffers;
	uint64_t address;
	uint64_t textures_address;
	uint64_t samplers_address;
	uint64_t attributes_address;
	uint64_t buffers_address;
	uint64_t tables_address;
	uint32_t index;
	uint32_t count;
	uint32_t location;
	int error;

	/* The FAU words: push constants, then each buffer's address, then the viewport. */
	*fau = 0;
	*table = 0;
	if (program->fau_words > FAU_WORDS_MAX)
		return ENOTSUP;
	kern_memset(words, 0, sizeof(words));
	if (program->push_bytes > MALI_VK_PUSH_BYTES)
		return ENOTSUP;
	kern_memcpy(words, build->push, program->push_bytes);
	for (index = 0; index < program->buffer_count; index++) {
		address = 0;
		if (program->buffers[index].set == DRV_GPU_IR_SYSTEM_SET) {
			address = system_address;
		} else {
			slot = find_slot(sets, program->buffers[index].set, program->buffers[index].binding);
			if (slot != NULL && slot->buffer != NULL) {
				buffer = slot->buffer->payload;
				address = mali_vk_buffer_address(buffer) + slot->offset;
			}
		}

		if (program->buffers[index].fau_word + 1U >= FAU_WORDS_MAX)
			return ENOTSUP;
		words[program->buffers[index].fau_word] = (uint32_t)address;
		words[program->buffers[index].fau_word + 1U] = (uint32_t)(address >> 32);

		/* The range's bytes, when the program reads the array's length. */
		if (program->buffers[index].size_fau_word != MALI_SHADER_NO_FAU_WORD) {
			if (program->buffers[index].size_fau_word >= FAU_WORDS_MAX)
				return ENOTSUP;
			slot = NULL;
			if (program->buffers[index].set != DRV_GPU_IR_SYSTEM_SET)
				slot = find_slot(sets, program->buffers[index].set, program->buffers[index].binding);
			words[program->buffers[index].size_fau_word] = range_bytes(slot);
		}
	}

	/* A queried texture's or storage image's size: its width, height, layers (or cubes) and levels. */
	for (index = 0; index < program->texture_count; index++) {
		if (program->textures[index].size_fau_word == MALI_SHADER_NO_FAU_WORD)
			continue;
		if (program->textures[index].size_fau_word + 4U > FAU_WORDS_MAX)
			return ENOTSUP;
		texture_size(find_slot(sets, program->textures[index].set, program->textures[index].binding), false, &words[program->textures[index].size_fau_word]);
	}
	for (index = 0; index < program->image_count; index++) {
		if (program->images[index].size_fau_word == MALI_SHADER_NO_FAU_WORD)
			continue;
		if (program->images[index].size_fau_word + 4U > FAU_WORDS_MAX)
			return ENOTSUP;
		texture_size(find_slot(sets, program->images[index].set, program->images[index].binding), true, &words[program->images[index].size_fau_word]);
	}

	if (viewport_words != NULL && program->viewport_fau_word + 6U <= FAU_WORDS_MAX)
		kern_memcpy(&words[program->viewport_fau_word], viewport_words, 6U * sizeof(uint32_t));

	/* Into the stage. */
	if (program->fau_words != 0U) {
		view = mali_vk_build_take(build, (uint64_t)program->fau_words * 4U, 64U, &address);
		if (view == NULL)
			return build->error;
		store_words((volatile uint32_t *)(volatile void *)view, words, program->fau_words);
		*fau = address | ((uint64_t)(program->fau_words / 2U) << 56);
	}

	/* The resource table: textures, samplers, for a vertex program its attributes and buffers, then the storage images. */
	count = TABLES_COMPUTE;
	if (graphics != NULL)
		count = TABLES_VERTEX;
	if (program->image_count != 0U)
		count = TABLES_IMAGES;
	count = (count + MALI_DESC_TABLE_ALIGN - 1U) & ~(MALI_DESC_TABLE_ALIGN - 1U);
	tables = mali_vk_build_take(build, (uint64_t)count * MALI_DESC_RESOURCE_BYTES, 64U, &tables_address);
	if (tables == NULL)
		return build->error;

	/* The textures and samplers of the program's combined image samplers. */
	if (program->texture_count != 0U) {
		textures = mali_vk_build_take(build, (uint64_t)program->texture_count * MALI_DESC_BYTES * 2U, 64U, &textures_address);
		samplers = mali_vk_build_take(build, (uint64_t)program->texture_count * MALI_DESC_BYTES, 64U, &samplers_address);
		if (textures == NULL || samplers == NULL)
			return build->error;
		for (index = 0; index < program->texture_count; index++) {
			slot = find_slot(sets, program->textures[index].set, program->textures[index].binding);
			error = write_texture(build,
					      slot,
					      (volatile uint32_t *)(volatile void *)(textures + (uint64_t)program->textures[index].index * MALI_DESC_BYTES),
					      (volatile uint32_t *)(volatile void *)(samplers + (uint64_t)program->textures[index].index * MALI_DESC_BYTES));
			if (error != 0)
				return error;
		}

		mali_desc_resource(entry, textures_address, (uint64_t)program->texture_count * MALI_DESC_BYTES);
		store_words((volatile uint32_t *)(volatile void *)(tables + MALI_SHADER_TABLE_TEXTURES * MALI_DESC_RESOURCE_BYTES), entry, 4);
		mali_desc_resource(entry, samplers_address, (uint64_t)program->texture_count * MALI_DESC_BYTES);
		store_words((volatile uint32_t *)(volatile void *)(tables + MALI_SHADER_TABLE_SAMPLERS * MALI_DESC_RESOURCE_BYTES), entry, 4);
	}

	/* A vertex program's attributes by location, and the vertex buffers by binding. */
	if (graphics != NULL && graphics->attribute_count != 0U) {
		attributes = mali_vk_build_take(build, (uint64_t)MALI_SHADER_LOCATIONS * MALI_DESC_BYTES, 64U, &attributes_address);
		vertex_buffers = mali_vk_build_take(build, (uint64_t)MALI_VK_VERTEX_BINDINGS_MAX * MALI_DESC_BYTES, 64U, &buffers_address);
		if (attributes == NULL || vertex_buffers == NULL)
			return build->error;
		for (index = 0; index < graphics->attribute_count; index++) {
			location = graphics->attribute_location[index];
			if (location >= MALI_SHADER_LOCATIONS)
				return ENOTSUP;
			mali_desc_attribute(entry,
					    graphics->attribute_format[index]->pixel,
					    MALI_SHADER_TABLE_VERTEX_BUFFERS,
					    graphics->binding_rate[graphics->attribute_binding[index]] == VK_VERTEX_INPUT_RATE_INSTANCE,
					    (int32_t)graphics->attribute_offset[index],
					    graphics->attribute_binding[index],
					    graphics->binding_stride[graphics->attribute_binding[index]]);
			store_words((volatile uint32_t *)(volatile void *)(attributes + (uint64_t)location * MALI_DESC_BYTES), entry, 8);
		}

		for (index = 0; index < graphics->binding_count; index++) {
			buffer = build->vertex_buffers[index];
			if (buffer == NULL)
				continue;
			address = mali_vk_buffer_address(buffer) + build->vertex_offsets[index];
			mali_desc_buffer(entry, address, (uint32_t)(buffer->size - build->vertex_offsets[index]));
			store_words((volatile uint32_t *)(volatile void *)(vertex_buffers + (uint64_t)index * MALI_DESC_BYTES), entry, 8);
		}

		mali_desc_resource(entry, attributes_address, (uint64_t)MALI_SHADER_LOCATIONS * MALI_DESC_BYTES);
		store_words((volatile uint32_t *)(volatile void *)(tables + MALI_SHADER_TABLE_ATTRIBUTES * MALI_DESC_RESOURCE_BYTES), entry, 4);
		mali_desc_resource(entry, buffers_address, (uint64_t)MALI_VK_VERTEX_BINDINGS_MAX * MALI_DESC_BYTES);
		store_words((volatile uint32_t *)(volatile void *)(tables + MALI_SHADER_TABLE_VERTEX_BUFFERS * MALI_DESC_RESOURCE_BYTES), entry, 4);
	}

	/* The storage images, each a texture descriptor written for storage (with its planes after it). */
	if (program->image_count != 0U) {
		textures = mali_vk_build_take(build, (uint64_t)program->image_count * MALI_DESC_BYTES, 64U, &textures_address);
		if (textures == NULL)
			return build->error;
		for (index = 0; index < program->image_count; index++) {
			slot = find_slot(sets, program->images[index].set, program->images[index].binding);
			if (slot == NULL || slot->view == NULL)
				continue;
			error = write_view(build,
					   slot->view->payload,
					   true,
					   (volatile uint32_t *)(volatile void *)(textures + (uint64_t)program->images[index].index * MALI_DESC_BYTES));
			if (error != 0)
				return error;
		}
		mali_desc_resource(entry, textures_address, (uint64_t)program->image_count * MALI_DESC_BYTES);
		store_words((volatile uint32_t *)(volatile void *)(tables + MALI_SHADER_TABLE_IMAGES * MALI_DESC_RESOURCE_BYTES), entry, 4);
	}

	/* Succeeded: the table pointer with its count. */
	*table = tables_address | count;
	return 0;
}

/*
 * Writes a thread storage descriptor into the stage: a program's thread
 * local storage (tls_bytes a thread, none for zero) for every thread of
 * every core the GPU has (by the highest core number), and no workgroup
 * memory.
 */
uint64_t
mali_vk_build_thread_storage(
	struct mali_vk_build *build,
	uint32_t tls_bytes)
{
	/* Without workgroup memory. */
	return storage_descriptor(build, tls_bytes, NULL);
}

/*
 * Keeps a bound pipeline, sets, push constants, vertex and index
 * buffers, viewport or scissor, the depth bias and the stencil masks
 * and references.
 */
void
mali_vk_build_bind(
	struct mali_vk_build *build,
	const struct mali_vk_record *entry)
{
	const struct mali_vk_pipeline *pipeline;
	uint32_t index;
	uint32_t first;

	/* One case a binding command. */
	switch (entry->opcode) {
	case GPU_OP_CMD_BIND_PIPELINE:
		if (entry->objects[0] == NULL)
			break;
		pipeline = entry->objects[0]->payload;
		if (entry->values[0] == VK_PIPELINE_BIND_POINT_COMPUTE) {
			build->compute = pipeline;
		} else {
			build->graphics = pipeline;
		}
		break;
	case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
		first = entry->values[1];
		for (index = 0; index < entry->object_count && first + index < MALI_VK_SETS_MAX; index++) {
			if (entry->objects[index] == NULL)
				continue;
			if (entry->values[0] == VK_PIPELINE_BIND_POINT_COMPUTE) {
				build->compute_sets[first + index] = entry->objects[index]->payload;
			} else {
				build->graphics_sets[first + index] = entry->objects[index]->payload;
			}
		}
		break;
	case GPU_OP_CMD_PUSH_CONSTANTS:
		if (entry->data != NULL && entry->values[0] + entry->data_bytes <= MALI_VK_PUSH_BYTES)
			kern_memcpy(&build->push[entry->values[0]], entry->data, entry->data_bytes);
		break;
	case GPU_OP_CMD_BIND_VERTEX_BUFFERS:
		first = entry->values[0];
		for (index = 0; index < entry->object_count && first + index < MALI_VK_VERTEX_BINDINGS_MAX; index++) {
			build->vertex_buffers[first + index] = NULL;
			if (entry->objects[index] != NULL)
				build->vertex_buffers[first + index] = entry->objects[index]->payload;
			build->vertex_offsets[first + index] = entry->wide[index];
		}
		break;
	case GPU_OP_CMD_BIND_INDEX_BUFFER:
		build->index_buffer = NULL;
		if (entry->objects[0] != NULL)
			build->index_buffer = entry->objects[0]->payload;
		build->index_offset = entry->wide[0];
		build->index_type = entry->values[0];
		break;
	case GPU_OP_CMD_SET_VIEWPORT:
		if (entry->data != NULL) {
			kern_memcpy(&build->viewport, entry->data, sizeof(build->viewport));
			build->viewport_set = true;
		}
		break;
	case GPU_OP_CMD_SET_SCISSOR:
		if (entry->data != NULL) {
			kern_memcpy(&build->scissor, entry->data, sizeof(build->scissor));
			build->scissor_set = true;
		}
		break;
	case GPU_OP_CMD_SET_DEPTH_BIAS:
	case GPU_OP_CMD_SET_STENCIL_COMPARE_MASK:
	case GPU_OP_CMD_SET_STENCIL_WRITE_MASK:
	case GPU_OP_CMD_SET_STENCIL_REFERENCE:
		mali_vk_draw_set_state(build, entry);
		break;
	default:
		break;
	}
}

/*
 * Writes a compute run into the stream: its staging registers (the
 * tables, the FAU, the program, the storage, the workgroup size and
 * count, no offsets), the run of one workgroup a task along x, and a
 * wait for it.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/vulkan/csf/panvk_vX_cmd_dispatch.c
 * (cmd_dispatch_shader: the staging registers a compute run reads).
 */
void
mali_vk_build_compute(
	struct mali_vk_build *build,
	uint64_t table,
	uint64_t fau,
	uint64_t shader,
	uint64_t storage,
	const uint32_t *local_size,
	const uint32_t *counts)
{
	/* The staging registers. */
	mali_cs_move64(&build->cs, MALI_CS_COMPUTE_SRT, table);
	mali_cs_move64(&build->cs, MALI_CS_COMPUTE_FAU, fau);
	mali_cs_move64(&build->cs, MALI_CS_COMPUTE_SPD, shader);
	mali_cs_move64(&build->cs, MALI_CS_COMPUTE_TSD, storage);
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_COMPUTE_GLOBAL_OFFSET, 0));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_COMPUTE_WORKGROUP, mali_desc_workgroup(local_size[0], local_size[1], local_size[2])));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_COMPUTE_OFFSET_X, 0));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_COMPUTE_OFFSET_Y, 0));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_COMPUTE_OFFSET_Z, 0));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_COMPUTE_SIZE_X, counts[0]));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_COMPUTE_SIZE_Y, counts[1]));
	mali_cs_emit(&build->cs, mali_cs_move32(MALI_CS_COMPUTE_SIZE_Z, counts[2]));

	/* One workgroup a task along x, then the wait for the run. */
	mali_cs_emit(&build->cs, mali_cs_run_compute(1, MALI_CS_AXIS_X, 0, 0, 0, 0));
	mali_cs_emit(&build->cs, mali_cs_wait(MALI_VK_WAIT_ALL));
}

/*
 * vkQueueSubmit: [queue][count][count][submit infos][fence] -> [result].
 * Every command buffer of every info runs in one stream; the fence is
 * signaled once it ended.
 */
static int
submit(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkSubmitInfo *infos;
	struct mali_vk_command_buffer **commands;
	struct mali_vk_command_buffer *command;
	struct mali_vk_fence *fence;
	uint64_t count;
	uint64_t index;
	uint64_t address;
	uint32_t bytes;
	uint32_t total;
	uint32_t buffer;
	int error;

	/* The queue and the infos. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > SUBMITS_MAX)
		return EINVAL;
	infos = mali_vk_arena_take(session, (size_t)count * sizeof(*infos) + 8U);
	commands = mali_vk_arena_take(session, COMMAND_BUFFERS_MAX * sizeof(*commands));
	if (infos == NULL || commands == NULL)
		return EINVAL;
	for (index = 0; index < count; index++)
		i915_vkc_dec_VkSubmitInfo(reader, &session->arena, &infos[index]);
	fence = mali_vk_payload(session, I915_VK_OBJ_FENCE, drv_i915_wire_read_u64(reader));
	if (reader->error != 0)
		return EINVAL;

	/*
	 * Every command buffer, executable.  One whose recording failed is
	 * refused with the errno its End answered, so that the submission
	 * fails with the End's result; nothing has gone to the GPU yet.
	 */
	total = 0;
	error = 0;
	for (index = 0; error == 0 && index < count; index++) {
		for (buffer = 0; buffer < infos[index].commandBufferCount; buffer++) {
			command = mali_vk_payload(session, I915_VK_OBJ_COMMAND_BUFFER, mali_vk_handle(infos[index].pCommandBuffers[buffer]));
			if (command != NULL && command->state == MALI_VK_COMMAND_INVALID && command->error != 0) {
				error = command->error;
				break;
			}
			if (command == NULL || command->state != MALI_VK_COMMAND_EXECUTABLE || total == COMMAND_BUFFERS_MAX) {
				error = EINVAL;
				break;
			}
			commands[total] = command;
			total++;
		}
	}

	/* The stream, then its run on the GPU. */
	bytes = 0;
	if (error == 0 && total != 0U)
		error = build_stream(session, commands, total, &address, &bytes);
	if (error == 0 && bytes != 0U)
		error = mali_sched_run(session->owner, address, bytes);
	mali_stage_reset(session->owner);

	/* The fence is signaled once the work ended; a failed one too, so a waiter is not left. */
	if (fence != NULL)
		fence->signaled = true;
	if (error != 0)
		kern_logf("mali: vk: submission of %u command buffers failed (%d)\n", (unsigned)total, error);

	/* Succeeded: the result. */
	drv_i915_wire_reply_u32(reply, mali_vk_result(error));
	return 0;
}

/* Builds one stream of the command buffers' records in the stage. */
static int
build_stream(
	struct mali_vk_session *session,
	struct mali_vk_command_buffer **commands,
	uint32_t count,
	uint64_t *address,
	uint32_t *bytes)
{
	struct mali_vk_build *build;
	volatile uint8_t *view;
	uint32_t index;
	int error;

	/* The front end must have the registers the stream uses. */
	*bytes = 0;
	if (session->owner->render->queue.registers < MALI_VK_REGISTERS_NEEDED + 4U) {
		kern_logf("mali: vk: the command stream has %u registers, %u needed\n",
			  session->owner->render->queue.registers,
			  MALI_VK_REGISTERS_NEEDED + 4U);
		return ENOTSUP;
	}

	/* The executor's own programs the stream runs, none yet (a stream that is not run leaves none behind). */
	session->owner->meta_used = 0;

	/* The build's state. */
	build = kern_calloc(1, sizeof(*build));
	if (build == NULL)
		return ENOMEM;
	build->session = session;

	/* The stream's memory. */
	view = mali_vk_build_take(build, (uint64_t)MALI_VK_STREAM_INSTRUCTIONS * MALI_CS_INSTRUCTION_BYTES, 64U, address);
	if (view == NULL) {
		error = build->error;
		kern_free(build);
		return error;
	}
	mali_cs_init(&build->cs, (volatile uint64_t *)(volatile void *)view, MALI_VK_STREAM_INSTRUCTIONS);

	/* The endpoints, and the scoreboard entries endpoint work and loads and stores signal. */
	mali_cs_emit(&build->cs, mali_cs_request_resource(MALI_CS_RESOURCE_COMPUTE | MALI_CS_RESOURCE_FRAGMENT | MALI_CS_RESOURCE_TILER | MALI_CS_RESOURCE_IDVS));
	mali_cs_emit(&build->cs, mali_cs_set_scoreboard(MALI_VK_SLOT_ENDPOINT, MALI_VK_SLOT_LOAD_STORE));

	/* Each command buffer's records in order. */
	for (index = 0; index < count && build->error == 0; index++)
		run_records(build, commands[index]);

	/* Every run waited for, the endpoints given back. */
	mali_cs_emit(&build->cs, mali_cs_wait(MALI_VK_WAIT_ALL));
	mali_cs_emit(&build->cs, mali_cs_request_resource(0));

	/* A record that could not be built, or a stream that did not fit. */
	error = build->error;
	if (error == 0 && build->cs.error != 0) {
		kern_logf("mali: vk: the command stream did not fit (%u instructions)\n", MALI_VK_STREAM_INSTRUCTIONS);
		error = E2BIG;
	}

	*bytes = mali_cs_bytes(&build->cs);
	kern_free(build);
	if (error != 0)
		return error;

	/* Succeeded: the stream is built. */
	return 0;
}

/* Runs one command buffer's records into the stream. */
static void
run_records(
	struct mali_vk_build *build,
	const struct mali_vk_command_buffer *command)
{
	const struct mali_vk_record *entry;
	uint32_t index;
	int error;

	/* Each record in order; a pass takes its records to its end. */
	for (index = 0; index < command->record_count && build->error == 0; index++) {
		entry = &command->records[index];

		/* One case a recorded command. */
		switch (entry->opcode) {
		case GPU_OP_CMD_BIND_PIPELINE:
		case GPU_OP_CMD_BIND_DESCRIPTOR_SETS:
		case GPU_OP_CMD_PUSH_CONSTANTS:
		case GPU_OP_CMD_BIND_VERTEX_BUFFERS:
		case GPU_OP_CMD_BIND_INDEX_BUFFER:
		case GPU_OP_CMD_SET_VIEWPORT:
		case GPU_OP_CMD_SET_SCISSOR:
		case GPU_OP_CMD_SET_DEPTH_BIAS:
		case GPU_OP_CMD_SET_STENCIL_COMPARE_MASK:
		case GPU_OP_CMD_SET_STENCIL_WRITE_MASK:
		case GPU_OP_CMD_SET_STENCIL_REFERENCE:
			mali_vk_build_bind(build, entry);
			break;
		case GPU_OP_CMD_DISPATCH:
			run_dispatch(build, entry);
			break;
		case GPU_OP_CMD_COPY_BUFFER:
			run_copy_buffer(build, entry);
			break;
		case GPU_OP_CMD_FILL_BUFFER:
			run_fill_buffer(build, entry);
			break;
		case GPU_OP_CMD_COPY_IMAGE:
		case GPU_OP_CMD_BLIT_IMAGE:
			run_copy_image(build, entry);
			break;
		case GPU_OP_CMD_COPY_BUFFER_TO_IMAGE:
			run_buffer_image(build, entry, true);
			break;
		case GPU_OP_CMD_COPY_IMAGE_TO_BUFFER:
			run_buffer_image(build, entry, false);
			break;
		case GPU_OP_CMD_CLEAR_COLOR_IMAGE:
			run_clear_image(build, entry);
			break;
		case GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE:
			run_clear_depth(build, entry);
			break;
		case GPU_OP_CMD_BEGIN_RENDER_PASS:
			error = mali_vk_draw_pass(build, command, &index);
			if (error != 0 && build->error == 0)
				build->error = error;
			break;
		default:
			/* Draws and clears outside a pass are not valid Vulkan; they are left out. */
			break;
		}
	}
}

/*
 * Runs a dispatch: the program's descriptors, the staging registers of
 * a compute run (the tables, the FAU, the program, the storage, the
 * workgroup size and count), the run, and a wait for it.
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/vulkan/csf/panvk_vX_cmd_dispatch.c
 * (cmd_dispatch_shader: the staging registers a compute run reads).
 */
static void
run_dispatch(
	struct mali_vk_build *build,
	const struct mali_vk_record *entry)
{
	const struct mali_shader_program *program;
	struct workgroup_memory wls;
	volatile uint8_t *view;
	volatile uint32_t *counts;
	uint64_t fau;
	uint64_t table;
	uint64_t shader;
	uint64_t storage;
	uint64_t counts_address;
	uint32_t index;
	int error;

	/* A bound compute pipeline. */
	if (build->compute == NULL || build->compute->program == NULL) {
		build->error = EINVAL;
		return;
	}
	program = build->compute->program;

	/* gl_NumWorkGroups, when the program reads it. */
	counts_address = 0;
	for (index = 0; index < program->buffer_count; index++) {
		if (program->buffers[index].set != DRV_GPU_IR_SYSTEM_SET)
			continue;
		view = mali_vk_build_take(build, 16U, 16U, &counts_address);
		if (view == NULL)
			return;
		counts = (volatile uint32_t *)(volatile void *)view;
		counts[0] = entry->values[0];
		counts[1] = entry->values[1];
		counts[2] = entry->values[2];
	}

	/* The FAU and the tables, the program descriptor and the storage. */
	error = mali_vk_build_resources(build, program, build->compute_sets, NULL, NULL, counts_address, &fau, &table);
	if (error != 0) {
		build->error = error;
		return;
	}

	shader = mali_vk_build_shader(build, MALI_DESC_STAGE_COMPUTE, program->work_registers, program->preload, build->compute->code_address);
	if (program->shared_bytes == 0U) {
		storage = mali_vk_build_thread_storage(build, program->tls_bytes);
	} else {
		/* With the workgroup memory of the dispatch's workgroups. */
		error = workgroup_memory(build, program, entry->values, &wls);
		if (error != 0) {
			if (build->error == 0)
				build->error = error;
			return;
		}
		storage = storage_descriptor(build, program->tls_bytes, &wls);
	}

	if (storage == 0U || shader == 0U)
		return;

	/* The staging registers of the run, the run, and the wait for it. */
	mali_vk_build_compute(build, table, fau, shader, storage, program->local_size, entry->values);
}

/*
 * Takes the workgroup memory of a dispatch of a program with shared
 * memory, for the workgroup counts: as many instances a core as
 * WLS_TOTAL_MAX and the bounds above allow, from stage memory that
 * does not cross a 4 GiB line (not cleared: shared memory starts
 * undefined).  ENOTSUP, said once, for a program whose shared memory is
 * above the most; the stage's error, or ENOSPC, when it has no room.
 */
static int
workgroup_memory(
	struct mali_vk_build *build,
	const struct mali_shader_program *program,
	const uint32_t *counts,
	struct workgroup_memory *wls)
{
	volatile uint8_t *view;
	uint64_t threads;
	uint64_t total;
	uint64_t cores;
	uint64_t base;
	uint32_t instances;
	uint32_t most;
	uint32_t attempt;
	int error;

	/* An instance's bytes. */
	kern_memset(wls, 0, sizeof(*wls));
	if (program->shared_bytes > MALI_DESC_WLS_MAX_BYTES) {
		if (!build->session->owner->wls_warned)
			kern_logf("mali: vk: a dispatch of %u bytes of workgroup memory is not run\n", (unsigned)program->shared_bytes);
		build->session->owner->wls_warned = true;
		return ENOTSUP;
	}
	wls->instance_bytes = mali_desc_wls_bytes(program->shared_bytes);

	/* The workgroups a core holds at once: no more than its threads take, nor than the dispatch has. */
	threads = (uint64_t)program->local_size[0] * program->local_size[1] * program->local_size[2];
	if (threads == 0U)
		threads = 1U;
	most = power_above((uint32_t)((TLS_THREADS_PER_CORE + threads - 1U) / threads));
	instances = power_above(counts[0]);
	if (instances < most)
		instances *= power_above(counts[1]);
	if (instances < most)
		instances *= power_above(counts[2]);
	if (instances > most || instances == 0U)
		instances = most;

	/* Fewer instances while all the cores' would take more than the most. */
	cores = core_range(build);
	total = (uint64_t)wls->instance_bytes * instances * cores;
	while (total > WLS_TOTAL_MAX && instances > 1U) {
		instances /= 2U;
		total = (uint64_t)wls->instance_bytes * instances * cores;
	}
	if (total > WLS_TOTAL_MAX) {
		build->error = ENOTSUP;
		return ENOTSUP;
	}
	wls->instances = instances;

	/* The memory, again past a 4 GiB line the first take crosses. */
	if (build->error != 0)
		return build->error;
	for (attempt = 0; attempt < 2U; attempt++) {
		error = mali_stage_take(build->session->owner, total, WLS_ALIGN, &view, &base);
		if (error != 0) {
			build->error = error;
			return error;
		}
		if ((base >> 32) == ((base + total - 1U) >> 32)) {
			wls->base = base;
			return 0;
		}
	}

	/* Two takes crossed a line: the stage's memory has no run for it. */
	build->error = ENOSPC;
	return ENOSPC;
}

/* Gives the least power of two not below a value (one for zero), at most 2^31. */
static uint32_t
power_above(
	uint32_t value)
{
	uint32_t power;

	/* Doubling from one. */
	power = 1U;
	while (power < value && power < 0x80000000U)
		power *= 2U;

	/* Succeeded: the power. */
	return power;
}

/*
 * Writes a thread storage descriptor into the stage, with a thread's
 * local storage (tls_bytes, none for zero) and, when wls is not NULL,
 * the workgroup memory it names.  The thread local storage is made for
 * every thread of every core the GPU has (by the highest core number).
 *
 * Procedure referenced: Mesa 26.2.6 src/panfrost/lib/pan_scratch.c
 * (pan_get_total_stack_size: a thread's bytes a power of two of at least
 * 16, times the threads of a core, times the cores' number range).
 */
static uint64_t
storage_descriptor(
	struct mali_vk_build *build,
	uint32_t tls_bytes,
	const struct workgroup_memory *wls)
{
	uint32_t words[8];
	volatile uint8_t *view;
	volatile uint8_t *storage_view;
	uint64_t address;
	uint64_t storage;
	uint64_t total;
	uint64_t cores;
	uint32_t thread_bytes;
	int error;

	/* A thread's bytes, a power of two of at least the least. */
	storage = 0;
	thread_bytes = 0;
	if (tls_bytes != 0U) {
		if (tls_bytes > MALI_DESC_TLS_MAX_BYTES) {
			build->error = ENOTSUP;
			return 0;
		}
		thread_bytes = MALI_DESC_TLS_MIN_BYTES;
		while (thread_bytes < tls_bytes)
			thread_bytes *= 2U;

		/* The cores' number range, and the storage of all their threads (not cleared: a thread writes before it reads). */
		cores = core_range(build);
		total = (uint64_t)thread_bytes * TLS_THREADS_PER_CORE * cores;
		if (total > TLS_TOTAL_MAX) {
			build->error = ENOTSUP;
			return 0;
		}
		if (build->error != 0)
			return 0;
		error = mali_stage_take(build->session->owner, total, TLS_ALIGN, &storage_view, &storage);
		if (error != 0) {
			build->error = error;
			return 0;
		}
	}

	/* The descriptor, with the workgroup memory when there is some. */
	view = mali_vk_build_take(build, MALI_DESC_BYTES, 64U, &address);
	if (view == NULL)
		return 0;
	mali_desc_thread_storage(words, thread_bytes, storage);
	if (wls != NULL)
		mali_desc_workgroup_storage(words, wls->instances, wls->instance_bytes, wls->base);
	store_words((volatile uint32_t *)(volatile void *)view, words, 8);

	/* Succeeded: its address. */
	return address;
}

/* Reports the cores' number range: one past the highest core the GPU has. */
static uint64_t
core_range(
	const struct mali_vk_build *build)
{
	/* The highest bit of the shader cores present, at least core 0. */
	return 64U - (uint64_t)__builtin_clzll(build->session->owner->render->device->shader_present | 1U);
}

/* Runs vkCmdCopyBuffer: each region as a span. */
static void
run_copy_buffer(
	struct mali_vk_build *build,
	const struct mali_vk_record *entry)
{
	const struct mali_vk_buffer *source;
	const struct mali_vk_buffer *destination;
	const VkBufferCopy *regions;
	uint32_t index;

	/* Both buffers, bound. */
	if (entry->objects[0] == NULL || entry->objects[1] == NULL)
		return;
	source = entry->objects[0]->payload;
	destination = entry->objects[1]->payload;
	regions = entry->data;
	if (mali_vk_buffer_address(source) == 0U || mali_vk_buffer_address(destination) == 0U)
		return;

	/* Each region inside both buffers. */
	for (index = 0; index < entry->values[7] && regions != NULL; index++) {
		if (regions[index].srcOffset + regions[index].size > source->size || regions[index].dstOffset + regions[index].size > destination->size)
			continue;
		copy_span(build,
			  mali_vk_buffer_address(destination) + regions[index].dstOffset,
			  mali_vk_buffer_address(source) + regions[index].srcOffset,
			  regions[index].size);
	}
}

/* Runs vkCmdFillBuffer: the range filled with the word. */
static void
run_fill_buffer(
	struct mali_vk_build *build,
	const struct mali_vk_record *entry)
{
	const struct mali_vk_buffer *buffer;
	uint64_t bytes;

	/* A bound buffer. */
	if (entry->objects[0] == NULL)
		return;
	buffer = entry->objects[0]->payload;
	if (mali_vk_buffer_address(buffer) == 0U || entry->wide[0] >= buffer->size)
		return;

	/* VK_WHOLE_SIZE fills to the end, rounded down to words. */
	bytes = entry->wide[1];
	if (bytes == VK_WHOLE_SIZE || bytes > buffer->size - entry->wide[0])
		bytes = (buffer->size - entry->wide[0]) & ~3ULL;

	/* One row. */
	mali_cs_fill_rows(&build->cs, MALI_VK_DATA_REGISTER, MALI_VK_SCRATCH_REGISTER, mali_vk_buffer_address(buffer) + entry->wide[0], 0, entry->values[0], (uint32_t)bytes, 1, MALI_VK_SLOT_LOAD_STORE);
}

/*
 * Runs vkCmdCopyImage, and vkCmdBlitImage of the same size and format:
 * the rows of each region copied between the levels of the linear
 * images.
 */
static void
run_copy_image(
	struct mali_vk_build *build,
	const struct mali_vk_record *entry)
{
	const struct mali_vk_image *source;
	const struct mali_vk_image *destination;
	const VkImageCopy *copies;
	const VkImageBlit *blits;
	uint32_t index;
	int32_t sx;
	int32_t sy;
	int32_t dx;
	int32_t dy;
	uint32_t width;
	uint32_t height;
	uint32_t source_layer;
	uint32_t destination_layer;
	uint32_t source_level;
	uint32_t destination_level;
	uint32_t layers;
	uint32_t layer;
	uint32_t aspect;
	uint64_t from;
	uint64_t to;

	/* Both images, bound, of one texel size. */
	if (entry->objects[0] == NULL || entry->objects[1] == NULL)
		return;
	source = entry->objects[0]->payload;
	destination = entry->objects[1]->payload;
	if (mali_vk_image_address(source) == 0U || mali_vk_image_address(destination) == 0U || source->format->bytes != destination->format->bytes)
		return;

	/* Each region: a blit only when it neither scales nor flips. */
	for (index = 0; index < entry->values[7] && entry->data != NULL; index++) {
		if (entry->opcode == GPU_OP_CMD_COPY_IMAGE) {
			copies = entry->data;
			sx = copies[index].srcOffset.x;
			sy = copies[index].srcOffset.y;
			dx = copies[index].dstOffset.x;
			dy = copies[index].dstOffset.y;
			width = copies[index].extent.width;
			height = copies[index].extent.height;
			source_layer = copies[index].srcSubresource.baseArrayLayer;
			destination_layer = copies[index].dstSubresource.baseArrayLayer;
			source_level = copies[index].srcSubresource.mipLevel;
			destination_level = copies[index].dstSubresource.mipLevel;
			layers = copies[index].srcSubresource.layerCount;
		} else {
			blits = entry->data;
			sx = blits[index].srcOffsets[0].x;
			sy = blits[index].srcOffsets[0].y;
			dx = blits[index].dstOffsets[0].x;
			dy = blits[index].dstOffsets[0].y;
			width = (uint32_t)(blits[index].srcOffsets[1].x - sx);
			height = (uint32_t)(blits[index].srcOffsets[1].y - sy);
			source_layer = blits[index].srcSubresource.baseArrayLayer;
			destination_layer = blits[index].dstSubresource.baseArrayLayer;
			source_level = blits[index].srcSubresource.mipLevel;
			destination_level = blits[index].dstSubresource.mipLevel;
			layers = blits[index].srcSubresource.layerCount;
			if (!blit_is_copy(&blits[index], source, destination)) {
				run_blit(build, &blits[index], source, destination, entry->values[5]);
				continue;
			}
		}

		/* Inside both images' levels. */
		if (source_level >= source->levels || destination_level >= destination->levels)
			continue;
		if (sx < 0 || sy < 0 || dx < 0 || dy < 0 || width == 0U || height == 0U)
			continue;
		if ((uint32_t)sx + width > source->level[source_level].width || (uint32_t)sy + height > source->level[source_level].height)
			continue;
		if ((uint32_t)dx + width > destination->level[destination_level].width || (uint32_t)dy + height > destination->level[destination_level].height)
			continue;

		/* The layers, inside both images. */
		if (layers == VK_REMAINING_ARRAY_LAYERS && source_layer < source->layers)
			layers = source->layers - source_layer;
		if (layers == 0U || source_layer >= source->layers || layers > source->layers - source_layer)
			continue;
		if (destination_layer >= destination->layers || layers > destination->layers - destination_layer)
			continue;

		/*
		 * One aspect of a copy between two depth and stencil images
		 * moves its field alone, keeping the other's bits; both aspects
		 * are the whole words below.
		 */
		aspect = 0;
		if (entry->opcode == GPU_OP_CMD_COPY_IMAGE && source->format->vk == VK_FORMAT_D24_UNORM_S8_UINT && destination->format->vk == VK_FORMAT_D24_UNORM_S8_UINT)
			aspect = copies[index].srcSubresource.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
		if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT || aspect == VK_IMAGE_ASPECT_STENCIL_BIT) {
			copy_image_aspect(build, source, destination, &copies[index], layers);
			continue;
		}

		/* Each layer's rows of the level. */
		for (layer = 0; layer < layers; layer++) {
			from = mali_vk_image_level_address(source, source_level, source_layer + layer);
			from += (uint64_t)sy * source->level[source_level].pitch + (uint64_t)sx * source->format->bytes;
			to = mali_vk_image_level_address(destination, destination_level, destination_layer + layer);
			to += (uint64_t)dy * destination->level[destination_level].pitch + (uint64_t)dx * destination->format->bytes;
			mali_cs_copy_rows(&build->cs,
					  MALI_VK_DATA_REGISTER,
					  MALI_VK_SCRATCH_REGISTER,
					  to,
					  (int32_t)destination->level[destination_level].pitch,
					  from,
					  (int32_t)source->level[source_level].pitch,
					  width * source->format->bytes,
					  height,
					  MALI_VK_SLOT_LOAD_STORE);
		}
	}
}

/*
 * Says whether a blit region is a copy the stream's own loops take: of
 * one format, the same size on both sides, neither side running
 * backwards.
 */
static bool
blit_is_copy(
	const VkImageBlit *blit,
	const struct mali_vk_image *source,
	const struct mali_vk_image *destination)
{
	int32_t width;
	int32_t height;

	/* One format. */
	if (source->format != destination->format)
		return false;

	/* The source forwards. */
	width = blit->srcOffsets[1].x - blit->srcOffsets[0].x;
	height = blit->srcOffsets[1].y - blit->srcOffsets[0].y;
	if (width <= 0 || height <= 0)
		return false;

	/* The target forwards and of the same size. */
	if (blit->dstOffsets[1].x - blit->dstOffsets[0].x != width)
		return false;
	if (blit->dstOffsets[1].y - blit->dstOffsets[0].y != height)
		return false;

	/* Succeeded: a copy. */
	return true;
}

/*
 * Runs a blit region the loops do not take on the executor's blit
 * program; one it cannot take is left out, said once a submission.
 */
static void
run_blit(
	struct mali_vk_build *build,
	const VkImageBlit *blit,
	const struct mali_vk_image *source,
	const struct mali_vk_image *destination,
	uint32_t filter)
{
	int error;

	/* The compute run of the region. */
	error = mali_vk_meta_blit(build, source, destination, blit, filter);
	if (error == 0)
		return;

	/* A region the program does not take, as before. */
	if (error == ENOTSUP) {
		if (!build->load_warned)
			kern_logf("mali: vk: a scaling or converting blit is not run\n");
		build->load_warned = true;
		return;
	}

	/* The stage's failure fails the submission. */
	if (build->error == 0)
		build->error = error;
}

/* Runs vkCmdCopyBufferToImage or vkCmdCopyImageToBuffer: the rows of each region's layers of its level. */
static void
run_buffer_image(
	struct mali_vk_build *build,
	const struct mali_vk_record *entry,
	bool to_image)
{
	const struct mali_vk_buffer *buffer;
	const struct mali_vk_image *image;
	const VkBufferImageCopy *regions;
	uint64_t buffer_pitch;
	uint64_t buffer_layer;
	uint64_t image_place;
	uint64_t buffer_place;
	uint32_t aspects;
	uint32_t buffer_texel;
	bool stencil;
	uint32_t row_texels;
	uint32_t image_rows;
	uint32_t index;
	uint32_t width;
	uint32_t height;
	uint32_t layer;
	uint32_t layers;
	uint32_t level;
	uint32_t pitch;

	/* The buffer and the image, bound. */
	if (entry->objects[0] == NULL || entry->objects[1] == NULL)
		return;
	if (to_image) {
		buffer = entry->objects[0]->payload;
		image = entry->objects[1]->payload;
	} else {
		image = entry->objects[0]->payload;
		buffer = entry->objects[1]->payload;
	}

	if (mali_vk_buffer_address(buffer) == 0U || mali_vk_image_address(image) == 0U)
		return;

	/* Each region inside the image and the buffer. */
	regions = entry->data;
	for (index = 0; index < entry->values[7] && regions != NULL; index++) {
		width = regions[index].imageExtent.width;
		height = regions[index].imageExtent.height;
		level = regions[index].imageSubresource.mipLevel;
		if (level >= image->levels)
			continue;
		pitch = image->level[level].pitch;

		/*
		 * The bytes of a texel in the buffer: a stencil aspect's one
		 * byte (only an image with a stencil has it), every other
		 * aspect's the whole texel.
		 */
		aspects = format_aspects(image->format);
		buffer_texel = image->format->bytes;
		stencil = false;
		if ((regions[index].imageSubresource.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0U) {
			if ((aspects & VK_IMAGE_ASPECT_STENCIL_BIT) == 0U) {
				kern_logf("mali: vk: a copy of a stencil aspect is not run\n");
				continue;
			}
			buffer_texel = 1U;
			stencil = true;
		}

		/* The region inside the level. */
		if (regions[index].imageOffset.x < 0 || regions[index].imageOffset.y < 0 || width == 0U || height == 0U)
			continue;
		if ((uint32_t)regions[index].imageOffset.x + width > image->level[level].width || (uint32_t)regions[index].imageOffset.y + height > image->level[level].height)
			continue;

		/* The buffer's rows are as long as said, or as the region. */
		row_texels = regions[index].bufferRowLength;
		if (row_texels == 0U)
			row_texels = width;
		buffer_pitch = (uint64_t)row_texels * buffer_texel;

		/* The layers, inside the image; a layer's rows in the buffer are as many as said, or the region's. */
		layers = regions[index].imageSubresource.layerCount;
		if (layers == VK_REMAINING_ARRAY_LAYERS && regions[index].imageSubresource.baseArrayLayer < image->layers)
			layers = image->layers - regions[index].imageSubresource.baseArrayLayer;
		if (layers == 0U || regions[index].imageSubresource.baseArrayLayer >= image->layers || layers > image->layers - regions[index].imageSubresource.baseArrayLayer)
			continue;
		image_rows = regions[index].bufferImageHeight;
		if (image_rows < height)
			image_rows = height;
		buffer_layer = buffer_pitch * image_rows;
		if (regions[index].bufferOffset + buffer_layer * (layers - 1U) + buffer_pitch * (height - 1U) + (uint64_t)width * buffer_texel > buffer->size)
			continue;

		/*
		 * A stencil aspect moves its bytes alone; so does a depth aspect
		 * into an image that also holds a stencil, which keeps the
		 * stencil.  A depth aspect out of it is the whole words, the
		 * stencil's byte in the top byte Vulkan leaves undefined.
		 */
		if (stencil || (to_image && aspects == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
			buffer_image_aspect(build, buffer, image, &regions[index], layers, buffer_pitch, buffer_layer, to_image);
			continue;
		}

		/* Each layer's rows one way or the other. */
		for (layer = 0; layer < layers; layer++) {
			image_place = mali_vk_image_level_address(image, level, regions[index].imageSubresource.baseArrayLayer + layer);
			image_place += (uint64_t)regions[index].imageOffset.y * pitch + (uint64_t)regions[index].imageOffset.x * image->format->bytes;
			buffer_place = mali_vk_buffer_address(buffer) + regions[index].bufferOffset + buffer_layer * layer;
			if (to_image) {
				mali_cs_copy_rows(&build->cs, MALI_VK_DATA_REGISTER, MALI_VK_SCRATCH_REGISTER, image_place, (int32_t)pitch, buffer_place, (int32_t)buffer_pitch, width * image->format->bytes, height, MALI_VK_SLOT_LOAD_STORE);
			} else {
				mali_cs_copy_rows(&build->cs, MALI_VK_DATA_REGISTER, MALI_VK_SCRATCH_REGISTER, buffer_place, (int32_t)buffer_pitch, image_place, (int32_t)pitch, width * image->format->bytes, height, MALI_VK_SLOT_LOAD_STORE);
			}
		}
	}
}

/* Runs vkCmdClearColorImage: the rows of each range's levels of its layers filled with the packed colour. */
static void
run_clear_image(
	struct mali_vk_build *build,
	const struct mali_vk_record *entry)
{
	const struct mali_vk_image *image;
	const VkImageSubresourceRange *ranges;
	uint32_t pattern;
	uint32_t index;
	uint32_t layer;
	uint32_t layers;
	uint32_t level;
	uint32_t levels;

	/* A bound image of a texel size a 32-bit pattern repeats in. */
	if (entry->objects[0] == NULL)
		return;
	image = entry->objects[0]->payload;
	if (mali_vk_image_address(image) == 0U || image->format->bytes > 4U || image->format->bytes == 3U)
		return;

	/* The colour, then each range's levels and layers inside the image. */
	pattern = pack_color(image->format, entry->values);
	ranges = entry->data;
	for (index = 0; index < entry->values[7] && ranges != NULL; index++) {
		layers = ranges[index].layerCount;
		if (layers == VK_REMAINING_ARRAY_LAYERS && ranges[index].baseArrayLayer < image->layers)
			layers = image->layers - ranges[index].baseArrayLayer;
		if (ranges[index].baseArrayLayer >= image->layers || layers > image->layers - ranges[index].baseArrayLayer)
			continue;
		levels = ranges[index].levelCount;
		if (levels == VK_REMAINING_MIP_LEVELS && ranges[index].baseMipLevel < image->levels)
			levels = image->levels - ranges[index].baseMipLevel;
		if (ranges[index].baseMipLevel >= image->levels || levels > image->levels - ranges[index].baseMipLevel)
			continue;
		for (level = ranges[index].baseMipLevel; level < ranges[index].baseMipLevel + levels; level++) {
			for (layer = 0; layer < layers; layer++) {
				mali_cs_fill_rows(&build->cs,
						  MALI_VK_DATA_REGISTER,
						  MALI_VK_SCRATCH_REGISTER,
						  mali_vk_image_level_address(image, level, ranges[index].baseArrayLayer + layer),
						  (int32_t)image->level[level].pitch,
						  pattern,
						  (uint32_t)(((uint64_t)image->level[level].width * image->format->bytes + 3U) & ~3ULL),
						  image->level[level].height,
						  MALI_VK_SLOT_LOAD_STORE);
			}
		}
	}
}

/*
 * Runs vkCmdClearDepthStencilImage: each range's levels of its layers
 * filled with the packed depth and stencil (the depth converted to the
 * format's normalized value, rounded to nearest, or kept as its float
 * bits), or, for one aspect of an image holding both, that aspect's
 * field written alone.
 */
static void
run_clear_depth(
	struct mali_vk_build *build,
	const struct mali_vk_record *entry)
{
	const struct mali_vk_image *image;
	const VkImageSubresourceRange *ranges;
	uint32_t pattern;
	uint32_t depth;
	uint32_t stencil;
	uint32_t index;

	/* A bound image of a depth format. */
	if (entry->objects[0] == NULL)
		return;
	image = entry->objects[0]->payload;
	if (mali_vk_image_address(image) == 0U || !image->format->depth)
		return;

	/* The whole texel's pattern, and the fields a single aspect writes. */
	stencil = entry->values[1] & D24S8_STENCIL_MASK;
	pattern = pack_depth(image->format, entry->values[0], stencil);
	depth = mali_meta_unorm(entry->values[0], UNORM24_MAX);

	/* Each range. */
	ranges = entry->data;
	for (index = 0; index < entry->values[7] && ranges != NULL && build->error == 0; index++)
		clear_depth_range(build, image, &ranges[index], pattern, depth, stencil);
}

/*
 * Clears one range of a depth image: its levels and layers inside the
 * image, each level's rows of each layer filled with the pattern when
 * the range clears every aspect the format has, or one run of the
 * aspect copy a level writing the depth's or the stencil's field alone.
 */
static void
clear_depth_range(
	struct mali_vk_build *build,
	const struct mali_vk_image *image,
	const VkImageSubresourceRange *range,
	uint32_t pattern,
	uint32_t depth,
	uint32_t stencil)
{
	struct mali_vk_meta_aspect aspect;
	uint32_t aspects;
	uint32_t layers;
	uint32_t layer;
	uint32_t levels;
	uint32_t level;

	/* The aspects of the format the range clears; none clears nothing. */
	aspects = range->aspectMask & format_aspects(image->format);
	if (aspects == 0U)
		return;

	/* The layers and the levels inside the image. */
	layers = range->layerCount;
	if (layers == VK_REMAINING_ARRAY_LAYERS && range->baseArrayLayer < image->layers)
		layers = image->layers - range->baseArrayLayer;
	if (layers == 0U || range->baseArrayLayer >= image->layers || layers > image->layers - range->baseArrayLayer)
		return;
	levels = range->levelCount;
	if (levels == VK_REMAINING_MIP_LEVELS && range->baseMipLevel < image->levels)
		levels = image->levels - range->baseMipLevel;
	if (levels == 0U || range->baseMipLevel >= image->levels || levels > image->levels - range->baseMipLevel)
		return;

	/* Every aspect: each level's rows of each layer, in whole words (a 16-bit texel's pattern repeats). */
	if (aspects == format_aspects(image->format)) {
		for (level = range->baseMipLevel; level < range->baseMipLevel + levels; level++) {
			for (layer = 0; layer < layers; layer++) {
				mali_cs_fill_rows(&build->cs,
						  MALI_VK_DATA_REGISTER,
						  MALI_VK_SCRATCH_REGISTER,
						  mali_vk_image_level_address(image, level, range->baseArrayLayer + layer),
						  (int32_t)image->level[level].pitch,
						  pattern,
						  (uint32_t)(((uint64_t)image->level[level].width * image->format->bytes + 3U) & ~3ULL),
						  image->level[level].height,
						  MALI_VK_SLOT_LOAD_STORE);
			}
		}

		return;
	}

	/* One aspect of a depth and stencil texel: its field, the value, written alone. */
	kern_memset(&aspect, 0, sizeof(aspect));
	aspect.layers = layers;
	aspect.target_step = 4U;
	aspect.flags = MALI_META_ASPECT_CONSTANT;
	if (aspects == VK_IMAGE_ASPECT_DEPTH_BIT) {
		aspect.target_shift = D24S8_DEPTH_SHIFT;
		aspect.mask = D24S8_DEPTH_MASK;
		aspect.value = depth;
	} else {
		aspect.target_shift = D24S8_STENCIL_SHIFT;
		aspect.mask = D24S8_STENCIL_MASK;
		aspect.value = stencil;
	}

	/* One run a level, its layers in the run. */
	for (level = range->baseMipLevel; level < range->baseMipLevel + levels && build->error == 0; level++) {
		aspect.target = mali_vk_image_level_address(image, level, range->baseArrayLayer);
		aspect.width = image->level[level].width;
		aspect.height = image->level[level].height;
		aspect.target_pitch = image->level[level].pitch;
		aspect.target_layer = image->layer_bytes;
		run_aspect(build, &aspect);
	}
}

/*
 * Packs a depth clear (the float's bits) and a stencil into the texel
 * of a depth format, repeated to 32 bits: D16 the 16-bit normalized
 * depth twice, X8_D24 the 24-bit one with a zero top byte, D24S8 the
 * 24-bit one under the stencil, D32 the float's bits.
 */
static uint32_t
pack_depth(
	const struct mali_vk_format *format,
	uint32_t depth_bits,
	uint32_t stencil)
{
	uint32_t texel;

	/* One case a depth format. */
	switch (format->vk) {
	case VK_FORMAT_D16_UNORM:
		texel = mali_meta_unorm(depth_bits, UNORM16_MAX) * 0x00010001U;
		break;
	case VK_FORMAT_X8_D24_UNORM_PACK32:
		texel = mali_meta_unorm(depth_bits, UNORM24_MAX) << D24S8_DEPTH_SHIFT;
		break;
	case VK_FORMAT_D24_UNORM_S8_UINT:
		texel = (mali_meta_unorm(depth_bits, UNORM24_MAX) << D24S8_DEPTH_SHIFT) | (stencil << D24S8_STENCIL_SHIFT);
		break;
	default:
		texel = depth_bits;
		break;
	}

	/* Succeeded: the pattern. */
	return texel;
}

/* Gives the aspects a format's texel holds: depth and stencil, depth alone, or colour. */
static uint32_t
format_aspects(
	const struct mali_vk_format *format)
{
	/* D24S8 is the one format of both. */
	if (format->vk == VK_FORMAT_D24_UNORM_S8_UINT)
		return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

	/* The other depth formats. */
	if (format->depth)
		return VK_IMAGE_ASPECT_DEPTH_BIT;

	/* A colour format. */
	return VK_IMAGE_ASPECT_COLOR_BIT;
}

/*
 * Copies one aspect (the depth or the stencil alone) of a region
 * between two D24S8 images (checked inside both levels and layer ranges
 * by the caller): that field of each texel, the target's other bits
 * kept, the layers in one run.
 */
static void
copy_image_aspect(
	struct mali_vk_build *build,
	const struct mali_vk_image *source,
	const struct mali_vk_image *destination,
	const VkImageCopy *copy,
	uint32_t layers)
{
	struct mali_vk_meta_aspect aspect;
	uint32_t source_level;
	uint32_t destination_level;

	/* Both sides' first texels, pitches and layers, a word a texel. */
	source_level = copy->srcSubresource.mipLevel;
	destination_level = copy->dstSubresource.mipLevel;
	kern_memset(&aspect, 0, sizeof(aspect));
	aspect.source = mali_vk_image_level_address(source, source_level, copy->srcSubresource.baseArrayLayer);
	aspect.source += (uint64_t)copy->srcOffset.y * source->level[source_level].pitch + (uint64_t)copy->srcOffset.x * 4U;
	aspect.target = mali_vk_image_level_address(destination, destination_level, copy->dstSubresource.baseArrayLayer);
	aspect.target += (uint64_t)copy->dstOffset.y * destination->level[destination_level].pitch + (uint64_t)copy->dstOffset.x * 4U;
	aspect.width = copy->extent.width;
	aspect.height = copy->extent.height;
	aspect.layers = layers;
	aspect.source_pitch = source->level[source_level].pitch;
	aspect.target_pitch = destination->level[destination_level].pitch;
	aspect.source_layer = source->layer_bytes;
	aspect.target_layer = destination->layer_bytes;
	aspect.source_step = 4U;
	aspect.target_step = 4U;

	/* The field: the depth's low 24 bits, or the stencil's top byte. */
	if ((copy->srcSubresource.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0U) {
		aspect.source_shift = D24S8_DEPTH_SHIFT;
		aspect.target_shift = D24S8_DEPTH_SHIFT;
		aspect.mask = D24S8_DEPTH_MASK;
	} else {
		aspect.source_shift = D24S8_STENCIL_SHIFT;
		aspect.target_shift = D24S8_STENCIL_SHIFT;
		aspect.mask = D24S8_STENCIL_MASK;
	}

	/* The run. */
	run_aspect(build, &aspect);
}

/*
 * Copies one aspect of a region between a buffer and a D24S8 image
 * (checked inside the level, the layers and the buffer by the caller):
 * the stencil's packed bytes in the buffer, or the depth's words, and
 * the field alone in each texel word of the image.  Into the image the
 * texel's other field is kept; out of it the buffer's bytes around the
 * region's stencil bytes are kept, several of them sharing a word.
 */
static void
buffer_image_aspect(
	struct mali_vk_build *build,
	const struct mali_vk_buffer *buffer,
	const struct mali_vk_image *image,
	const VkBufferImageCopy *region,
	uint32_t layers,
	uint64_t buffer_pitch,
	uint64_t buffer_layer,
	bool to_image)
{
	struct mali_vk_meta_aspect aspect;
	uint64_t image_place;
	uint64_t buffer_place;
	uint32_t level;
	uint32_t buffer_step;
	uint32_t shift;
	uint32_t mask;

	/* The image's first texel, and the buffer's. */
	level = region->imageSubresource.mipLevel;
	image_place = mali_vk_image_level_address(image, level, region->imageSubresource.baseArrayLayer);
	image_place += (uint64_t)region->imageOffset.y * image->level[level].pitch + (uint64_t)region->imageOffset.x * 4U;
	buffer_place = mali_vk_buffer_address(buffer) + region->bufferOffset;

	/* The field: the stencil's byte (one a texel in the buffer), or the depth's 24 bits (a word a texel). */
	if ((region->imageSubresource.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0U) {
		buffer_step = 1U;
		shift = D24S8_STENCIL_SHIFT;
		mask = D24S8_STENCIL_MASK;
	} else {
		buffer_step = 4U;
		shift = D24S8_DEPTH_SHIFT;
		mask = D24S8_DEPTH_MASK;
	}

	/* The run's extent and field. */
	kern_memset(&aspect, 0, sizeof(aspect));
	aspect.width = region->imageExtent.width;
	aspect.height = region->imageExtent.height;
	aspect.layers = layers;
	aspect.mask = mask;

	/* The two sides one way or the other; the buffer's packed stencil bytes share words. */
	if (to_image) {
		aspect.source = buffer_place;
		aspect.source_pitch = buffer_pitch;
		aspect.source_layer = buffer_layer;
		aspect.source_step = buffer_step;
		aspect.target = image_place;
		aspect.target_pitch = image->level[level].pitch;
		aspect.target_layer = image->layer_bytes;
		aspect.target_step = 4U;
		aspect.target_shift = shift;
	} else {
		aspect.source = image_place;
		aspect.source_pitch = image->level[level].pitch;
		aspect.source_layer = image->layer_bytes;
		aspect.source_step = 4U;
		aspect.source_shift = shift;
		aspect.target = buffer_place;
		aspect.target_pitch = buffer_pitch;
		aspect.target_layer = buffer_layer;
		aspect.target_step = buffer_step;
		if (buffer_step == 1U)
			aspect.flags = MALI_META_ASPECT_SHARED;
	}

	/* The run. */
	run_aspect(build, &aspect);
}

/*
 * Writes one run of the aspect copy; a run it cannot take is left out,
 * said once a submission, and the stage's failure fails the submission.
 */
static void
run_aspect(
	struct mali_vk_build *build,
	const struct mali_vk_meta_aspect *aspect)
{
	int error;

	/* The compute run. */
	error = mali_vk_meta_aspect(build, aspect);
	if (error == 0)
		return;

	/* A run the program does not take. */
	if (error == ENOTSUP) {
		if (!build->aspect_warned)
			kern_logf("mali: vk: a copy or a clear of one aspect of a depth and stencil image is not run\n");
		build->aspect_warned = true;
		return;
	}

	/* The stage's failure fails the submission. */
	if (build->error == 0)
		build->error = error;
}

/* Copies one span: its whole words as one row; a span of other bytes is left out once with a line. */
static void
copy_span(
	struct mali_vk_build *build,
	uint64_t destination,
	uint64_t source,
	uint64_t bytes)
{
	/* Whole words at word addresses. */
	if ((bytes & 3U) != 0U || (destination & 3U) != 0U || (source & 3U) != 0U || bytes > 0xffffffffULL) {
		kern_logf("mali: vk: a copy of %llu bytes not in words is not run\n", (unsigned long long)bytes);
		return;
	}

	/* One row. */
	mali_cs_copy_rows(&build->cs, MALI_VK_DATA_REGISTER, MALI_VK_SCRATCH_REGISTER, destination, 0, source, 0, (uint32_t)bytes, 1, MALI_VK_SLOT_LOAD_STORE);
}

/*
 * Packs a clear colour (the float bits of the union) into the texel of
 * a format of 8-bit components, repeated to 32 bits: R, G, B, A in
 * memory order (B and R exchanged for a BGRA format).
 */
static uint32_t
pack_color(
	const struct mali_vk_format *format,
	const uint32_t *words)
{
	uint32_t red;
	uint32_t green;
	uint32_t blue;
	uint32_t alpha;
	uint32_t texel;

	/* The components as 8-bit unsigned normalized. */
	red = unorm8(words[0]);
	green = unorm8(words[1]);
	blue = unorm8(words[2]);
	alpha = unorm8(words[3]);

	/* The memory order of the format. */
	if (format->vk == VK_FORMAT_B8G8R8A8_UNORM || format->vk == VK_FORMAT_B8G8R8A8_SRGB) {
		texel = blue | (green << 8) | (red << 16) | (alpha << 24);
	} else if (format->bytes == 1U) {
		texel = red * 0x01010101U;
	} else if (format->bytes == 2U) {
		texel = (red | (green << 8)) * 0x00010001U;
	} else {
		texel = red | (green << 8) | (blue << 16) | (alpha << 24);
	}

	/* Succeeded: the pattern. */
	return texel;
}

/*
 * Converts a float's bits to an 8-bit unsigned normalized value, rounded
 * to nearest, without floating-point instructions: zero for a negative
 * value or NaN, 255 for one and more.
 */
static uint32_t
unorm8(
	uint32_t bits)
{
	uint64_t mantissa;
	uint32_t exponent;
	uint32_t shift;

	/* A negative value, a NaN or zero. */
	exponent = (bits >> 23) & 0xffU;
	if ((bits & 0x80000000U) != 0U || exponent == 0U)
		return 0;
	if (exponent == 0xffU && (bits & 0x7fffffU) != 0U)
		return 0;

	/* One and more. */
	if (exponent >= 127U)
		return 255;

	/* (1.m * 2^(e - 127)) * 255, rounded. */
	mantissa = (uint64_t)((bits & 0x7fffffU) | 0x800000U) * 255U;
	shift = 23U + (127U - exponent);
	if (shift >= 63U)
		return 0;
	return (uint32_t)((mantissa + (1ULL << (shift - 1U))) >> shift);
}

/* Finds the descriptor of a (set, binding) among the bound sets, or NULL. */
static const struct mali_vk_descriptor *
find_slot(
	const struct mali_vk_descriptor_set *const *sets,
	uint32_t set,
	uint32_t binding)
{
	const struct mali_vk_descriptor_set *bound;
	uint32_t index;

	/* A bound set. */
	if (set >= MALI_VK_SETS_MAX || sets[set] == NULL)
		return NULL;
	bound = sets[set];

	/* The binding's slot. */
	for (index = 0; index < bound->layout.count; index++) {
		if (bound->layout.bindings[index].binding == binding)
			return &bound->slots[index];
	}

	/* No such binding. */
	return NULL;
}

/* Writes a combined image sampler's texture (with its planes) and sampler descriptors. */
static int
write_texture(
	struct mali_vk_build *build,
	const struct mali_vk_descriptor *slot,
	volatile uint32_t *texture,
	volatile uint32_t *sampler)
{
	const struct mali_vk_sampler *state;
	int error;

	/* A view of a bound image, and its sampler; a missing one leaves null descriptors. */
	if (slot == NULL || slot->view == NULL || slot->sampler == NULL)
		return 0;
	state = slot->sampler->payload;

	/* The texture, naming its planes. */
	error = write_view(build, slot->view->payload, false, texture);
	if (error != 0)
		return error;

	/* The sampler. */
	store_words(sampler, state->words, 8);

	/* Succeeded: both are written. */
	return 0;
}

/*
 * Writes a view's texture descriptor, sampled or for storage, and its
 * planes (one a level of a layer: the levels of the view's first layer,
 * then of the next): a cube view as a cube of its layers' count over
 * six, another as a 2D image of its layers; its extent the view's first
 * level's.  A storage view reads its first level only.  An unbound
 * image leaves a null descriptor.
 *
 * Procedure referenced: Mesa 26.2.6 src/panfrost/lib/pan_texture.c
 * (pan_emit_iview_texture_payload for PAN_ARCH 7 and on: the layers
 * outside, the levels inside; emit_generic_plane and
 * get_linear_or_u_tiled_plane_props: a level's address, size, row
 * stride and extent; pan_texture_get_extent: the texture's extent of
 * the first level).
 */
static int
write_view(
	struct mali_vk_build *build,
	const struct mali_vk_image_view *view,
	bool storage,
	volatile uint32_t *texture)
{
	const struct mali_vk_image *image;
	struct mali_desc_view described;
	uint32_t words[8];
	volatile uint8_t *planes;
	const struct mali_surface_level *place;
	uint64_t planes_address;
	uint64_t address;
	uint64_t plane;
	uint32_t layers;
	uint32_t layer;
	uint32_t levels;
	uint32_t level;

	/* A bound image, and the view's levels inside it. */
	image = view->image->payload;
	if (mali_vk_image_address(image) == 0U || view->base_level >= image->levels)
		return 0;
	layers = view_layers(view);
	levels = view_levels(view);
	if (storage)
		levels = 1U;
	if (levels > image->levels - view->base_level)
		levels = image->levels - view->base_level;

	/* The planes: each layer's levels from the view's first. */
	planes = mali_vk_build_take(build, (uint64_t)layers * levels * MALI_DESC_BYTES, MALI_DESC_BYTES, &planes_address);
	if (planes == NULL)
		return build->error;
	for (layer = 0; layer < layers; layer++) {
		for (level = 0; level < levels; level++) {
			place = &image->level[view->base_level + level];
			address = mali_vk_image_level_address(image, view->base_level + level, view->base_layer + layer);
			mali_desc_plane(words, view->format->bytes, address, place->pitch, place->bytes, place->width, place->height);
			plane = ((uint64_t)layer * levels + level) * MALI_DESC_BYTES;
			store_words((volatile uint32_t *)(volatile void *)(planes + plane), words, 8);
		}
	}

	/* The texture: a cube of the layers over six, or a 2D image of the layers. */
	kern_memset(&described, 0, sizeof(described));
	described.dimension = MALI_DESC_DIMENSION_2D;
	described.array_size = layers;
	if (!storage && view_is_cube(view)) {
		described.dimension = MALI_DESC_DIMENSION_CUBE;
		described.array_size = layers / 6U;
	}
	described.pixel = view->format->pixel;
	described.width = image->level[view->base_level].width;
	described.height = image->level[view->base_level].height;
	described.depth = 1U;
	described.levels = levels;
	described.swizzle = view->swizzle;
	described.storage = storage;
	described.planes = planes_address;
	mali_desc_texture_view(words, &described);
	store_words(texture, words, 8);

	/* Succeeded: the descriptor and its planes are written. */
	return 0;
}

/* Reports a view's layers: at least one. */
static uint32_t
view_layers(
	const struct mali_vk_image_view *view)
{
	/* A view made without a count has one layer. */
	if (view->layer_count == 0U)
		return 1U;

	/* The view's own count. */
	return view->layer_count;
}

/* Reports a view's levels: at least one. */
static uint32_t
view_levels(
	const struct mali_vk_image_view *view)
{
	/* A view made without a count has one level. */
	if (view->level_count == 0U)
		return 1U;

	/* The view's own count. */
	return view->level_count;
}

/* Reports whether a view is a cube or a cube array of whole cubes. */
static bool
view_is_cube(
	const struct mali_vk_image_view *view)
{
	/* Only the cube types, of six layers a cube. */
	if (view->view_type != VK_IMAGE_VIEW_TYPE_CUBE && view->view_type != VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)
		return false;
	if (view_layers(view) % 6U != 0U)
		return false;

	/* A cube view. */
	return true;
}

/* Stores words through an uncached view. */
static void
store_words(
	volatile uint32_t *destination,
	const uint32_t *source,
	unsigned count)
{
	unsigned index;

	for (index = 0; index < count; index++)
		destination[index] = source[index];
}

/* Reports the bytes of a buffer descriptor's range: VK_WHOLE_SIZE is to the buffer's end. */
static uint32_t
range_bytes(
	const struct mali_vk_descriptor *slot)
{
	const struct mali_vk_buffer *buffer;
	uint64_t bytes;

	/* No buffer has no bytes. */
	if (slot == NULL || slot->buffer == NULL)
		return 0;
	buffer = slot->buffer->payload;
	if (slot->offset >= buffer->size)
		return 0;

	/* The range, within the buffer. */
	bytes = slot->range;
	if (bytes == VK_WHOLE_SIZE || bytes > buffer->size - slot->offset)
		bytes = buffer->size - slot->offset;
	if (bytes > 0xffffffffULL)
		bytes = 0xffffffffULL;
	return (uint32_t)bytes;
}

/*
 * Writes a view's four size words: the width and height of its first
 * level, its layers (cubes of a cube array), its levels (one of a
 * storage image's); zero without a view.
 */
static void
texture_size(
	const struct mali_vk_descriptor *slot,
	bool storage,
	uint32_t *words)
{
	const struct mali_vk_image_view *view;
	const struct mali_vk_image *image;

	/* No view. */
	words[0] = 0;
	words[1] = 0;
	words[2] = 0;
	words[3] = 0;
	if (slot == NULL || slot->view == NULL)
		return;

	/* The view's image. */
	view = slot->view->payload;
	image = view->image->payload;
	words[0] = image->width;
	words[1] = image->height;
	if (view->base_level < image->levels) {
		words[0] = image->level[view->base_level].width;
		words[1] = image->level[view->base_level].height;
	}
	words[2] = view_layers(view);
	if (view_is_cube(view))
		words[2] = view_layers(view) / 6U;
	words[3] = view_levels(view);
	if (storage)
		words[3] = 1U;
}
