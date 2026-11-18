/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's own programs, the blit and the aspect copy that run
 * them.  See vk-meta.h.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <kern/lock.h>
#include <uapi/errno.h>

#include "drivers/gpu/mali/vk/vk-desc.h"
#include "drivers/gpu/mali/vk/vk-meta.h"
#include "drivers/gpu/mali/vk/vk-meta-spirv.h"

/* The resource tables of a program's table pointer: none used, the hardware's multiple of empty ones. */
#define META_TABLES		MALI_DESC_TABLE_ALIGN


/* The blit's and the aspect copy's buffers by binding: the source and the target. */
#define SOURCE_BINDING		0U
#define TARGET_BINDING		1U

/* The blit's push constant words, in the order the program declares them. */
#define BLIT_X			0U
#define BLIT_Y			1U
#define BLIT_STEP_X		2U
#define BLIT_STEP_Y		3U
#define BLIT_WIDTH		4U
#define BLIT_HEIGHT		5U
#define BLIT_SOURCE_PITCH	6U
#define BLIT_TARGET_PITCH	7U
#define BLIT_SOURCE_LAYER	8U
#define BLIT_TARGET_LAYER	9U
#define BLIT_SOURCE_WIDTH	10U
#define BLIT_SOURCE_HEIGHT	11U
#define BLIT_FLAGS		12U

/* The aspect copy's push constant words, in the order the program declares them. */
#define ASPECT_WIDTH		0U
#define ASPECT_HEIGHT		1U
#define ASPECT_SOURCE_PITCH	2U
#define ASPECT_TARGET_PITCH	3U
#define ASPECT_SOURCE_LAYER	4U
#define ASPECT_TARGET_LAYER	5U
#define ASPECT_SOURCE_STEP	6U
#define ASPECT_TARGET_STEP	7U
#define ASPECT_SOURCE_SHIFT	8U
#define ASPECT_TARGET_SHIFT	9U
#define ASPECT_MASK		10U
#define ASPECT_VALUE		11U
#define ASPECT_FLAGS		12U

/*
 * The last byte offset from either side's first texel the aspect copy
 * takes: the program's offsets are 32-bit, and staying below 2^31 keeps
 * them clear of a signed reading.
 */
#define ASPECT_OFFSET_MAX	0x7fffffffULL

/* The component orders of the formats the blit takes, and none. */
#define ORDER_RGBA		0
#define ORDER_BGRA		1
#define ORDER_NONE		-1

/*
 * What each program is, by number, in the lines that say it is not
 * there (the clear's colour programs share the last entry).
 */
static const char *const meta_names[] = {
	"that scales or converts a blit",
	"that places a clear inside a pass",
	"that colours a clear inside a pass",
	"that copies or clears one aspect of a depth and stencil image",
};

/* The entry of meta_names the aspect copy has. */
#define ASPECT_NAME		3U

static int compile_program(unsigned which, struct mali_shader_program **program);
static const struct mali_shader_program *node_program(struct mali_render *render, unsigned which);
static bool program_fits(const struct mali_shader_program *program, unsigned which, const char **reason);
static bool compute_fits(const struct mali_shader_program *program, uint32_t push_bytes, uint32_t group, const char **reason);
static bool aspect_side_fits(uint64_t address, uint32_t width, uint32_t height, uint32_t layers, uint64_t pitch, uint64_t layer, uint32_t step);
static int session_code(struct mali_session *session, const struct mali_shader_program *program, unsigned which, uint64_t *address);
static const char *meta_name(unsigned which);
static int blit_order(const struct mali_vk_format *format);
static uint32_t blit_layers(const VkImageSubresourceLayers *layers, const struct mali_vk_image *image);
static void store(volatile uint8_t *view, const uint32_t *words, unsigned count);

/* Gives one of the executor's programs and the session's copy of its code. */
int
mali_vk_meta_program(
	struct mali_vk_build *build,
	unsigned which,
	const struct mali_shader_program **program,
	uint64_t *code)
{
	const struct mali_shader_program *made;
	struct mali_session *session;
	int error;

	/* One of the programs. */
	*program = NULL;
	*code = 0;
	if (which >= MALI_RENDER_META_PROGRAMS)
		return ENOTSUP;

	/* The node's program; without it the work is left out. */
	session = build->session->owner;
	made = node_program(session->render, which);
	if (made == NULL)
		return ENOTSUP;

	/* The session's copy of its code. */
	error = session_code(session, made, which, code);
	if (error != 0)
		return ENOTSUP;

	/*
	 * meta_used tells the submission that its stream runs the program,
	 * so a failed one stops the node from using it.
	 */
	session->meta_used |= 1U << which;

	/* Succeeded: the program and its code. */
	*program = made;
	return 0;
}

/*
 * Writes a program's FAU words into the stage and reports the FAU
 * pointer, its count of 64-bit entries in its top byte (zero for none).
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/vulkan/csf/panvk_vX_cmd_dispatch.c (the FAU
 * pointer with its count at bit 56).
 */
int
mali_vk_meta_fau(
	struct mali_vk_build *build,
	const uint32_t *words,
	uint32_t count,
	uint64_t *fau)
{
	volatile uint8_t *view;
	uint64_t address;

	/* Nothing to point at. */
	*fau = 0;
	if (count == 0U)
		return 0;

	/* Into the stage. */
	view = mali_vk_build_take(build, (uint64_t)count * 4U, 64U, &address);
	if (view == NULL)
		return build->error;
	store(view, words, count);

	/* Succeeded: the pointer with its count. */
	*fau = address | ((uint64_t)(count / 2U) << 56);
	return 0;
}

/*
 * Writes a resource table of empty tables into the stage, for a program
 * that reads no resource, and reports its pointer with its count (zero
 * with the build's error set when the stage has no room).
 */
uint64_t
mali_vk_meta_tables(
	struct mali_vk_build *build)
{
	volatile uint8_t *view;
	uint64_t address;

	/* The tables, cleared. */
	view = mali_vk_build_take(build, (uint64_t)META_TABLES * MALI_DESC_RESOURCE_BYTES, 64U, &address);
	if (view == NULL)
		return 0;

	/* Succeeded: the pointer with its count. */
	return address | META_TABLES;
}

/*
 * Writes one region of a blit as a compute run of the blit program: one
 * thread a target texel of each layer, from the region's normalized
 * target rectangle (a flipped target flips the source instead), the
 * steps through the source in 16.16 fixed point, and red and blue
 * exchanged between the two orders.  An sRGB format is blitted as its
 * UNORM bytes (the filter weighs the encoded values).
 *
 * Procedure referenced: the Vulkan specification 1.0 (vkCmdBlitImage:
 * the source coordinate of a target texel's centre, the scale from the
 * two regions, the clamp to the source's edge, a nearest filter's texel
 * and a linear filter's four around the coordinate less half a texel).
 */
int
mali_vk_meta_blit(
	struct mali_vk_build *build,
	const struct mali_vk_image *source,
	const struct mali_vk_image *target,
	const VkImageBlit *region,
	uint32_t filter)
{
	const struct mali_shader_program *program;
	const struct mali_surface_level *source_level;
	const struct mali_surface_level *target_level;
	uint32_t words[MALI_VK_META_FAU_WORDS];
	uint32_t counts[3];
	int32_t source_x0;
	int32_t source_x1;
	int32_t source_y0;
	int32_t source_y1;
	int32_t target_x0;
	int32_t target_x1;
	int32_t target_y0;
	int32_t target_y1;
	int32_t swap;
	int source_order;
	int target_order;
	uint32_t width;
	uint32_t height;
	uint32_t layers;
	uint64_t code;
	uint64_t source_address;
	uint64_t target_address;
	uint64_t fau;
	uint64_t table;
	uint64_t shader;
	uint64_t storage;
	uint64_t address;
	uint32_t index;
	uint32_t word;
	int error;

	/* Two images of four bytes a texel, RGBA or BGRA. */
	source_order = blit_order(source->format);
	target_order = blit_order(target->format);
	if (source_order == ORDER_NONE || target_order == ORDER_NONE)
		return ENOTSUP;

	/* One slice of depth on both sides. */
	if (region->srcOffsets[0].z != 0 || region->srcOffsets[1].z != 1)
		return ENOTSUP;
	if (region->dstOffsets[0].z != 0 || region->dstOffsets[1].z != 1)
		return ENOTSUP;

	/* The corners; a target that runs backwards is turned and the source with it. */
	source_x0 = region->srcOffsets[0].x;
	source_x1 = region->srcOffsets[1].x;
	source_y0 = region->srcOffsets[0].y;
	source_y1 = region->srcOffsets[1].y;
	target_x0 = region->dstOffsets[0].x;
	target_x1 = region->dstOffsets[1].x;
	target_y0 = region->dstOffsets[0].y;
	target_y1 = region->dstOffsets[1].y;
	if (target_x1 < target_x0) {
		swap = target_x0;
		target_x0 = target_x1;
		target_x1 = swap;
		swap = source_x0;
		source_x0 = source_x1;
		source_x1 = swap;
	}

	if (target_y1 < target_y0) {
		swap = target_y0;
		target_y0 = target_y1;
		target_y1 = swap;
		swap = source_y0;
		source_y0 = source_y1;
		source_y1 = swap;
	}

	/* No texel to write, or a target region outside its image: nothing runs. */
	if (target_x1 == target_x0 || target_y1 == target_y0)
		return 0;
	if (source_x1 == source_x0 || source_y1 == source_y0)
		return 0;
	if (target_x0 < 0 || target_y0 < 0)
		return 0;
	/* The layers of a level, inside both images. */
	layers = blit_layers(&region->srcSubresource, source);
	if (layers == 0U)
		return 0;
	if (blit_layers(&region->dstSubresource, target) < layers)
		return 0;
	source_level = &source->level[region->srcSubresource.mipLevel];
	target_level = &target->level[region->dstSubresource.mipLevel];
	if ((uint32_t)target_x1 > target_level->width || (uint32_t)target_y1 > target_level->height)
		return 0;
	width = (uint32_t)(target_x1 - target_x0);
	height = (uint32_t)(target_y1 - target_y0);

	/* The program and its code. */
	error = mali_vk_meta_program(build, MALI_VK_META_BLIT, &program, &code);
	if (error != 0)
		return ENOTSUP;

	/* The source from its level of its first layer; the target from the region's first texel of its level. */
	source_address = mali_vk_image_level_address(source, region->srcSubresource.mipLevel, region->srcSubresource.baseArrayLayer);
	target_address = mali_vk_image_level_address(target, region->dstSubresource.mipLevel, region->dstSubresource.baseArrayLayer);
	if (source_address == 0U || target_address == 0U)
		return 0;
	target_address += (uint64_t)target_y0 * target_level->pitch + (uint64_t)target_x0 * 4U;

	/* The push constants: the source's corner and steps, the region, the pitches and layers in words, the source's extent, the flags. */
	kern_memset(words, 0, sizeof(words));
	words[BLIT_X] = (uint32_t)source_x0;
	words[BLIT_Y] = (uint32_t)source_y0;
	words[BLIT_STEP_X] = (uint32_t)(int32_t)(((int64_t)(source_x1 - source_x0) * 65536) / (int64_t)width);
	words[BLIT_STEP_Y] = (uint32_t)(int32_t)(((int64_t)(source_y1 - source_y0) * 65536) / (int64_t)height);
	words[BLIT_WIDTH] = width;
	words[BLIT_HEIGHT] = height;
	words[BLIT_SOURCE_PITCH] = source_level->pitch / 4U;
	words[BLIT_TARGET_PITCH] = target_level->pitch / 4U;
	words[BLIT_SOURCE_LAYER] = (uint32_t)(source->layer_bytes / 4U);
	words[BLIT_TARGET_LAYER] = (uint32_t)(target->layer_bytes / 4U);
	words[BLIT_SOURCE_WIDTH] = source_level->width;
	words[BLIT_SOURCE_HEIGHT] = source_level->height;
	if (filter == VK_FILTER_LINEAR)
		words[BLIT_FLAGS] |= MALI_META_BLIT_LINEAR;
	if (source_order != target_order)
		words[BLIT_FLAGS] |= MALI_META_BLIT_SWAP_RB;

	/* Each buffer's address at its FAU words, by its binding (the program's shape was checked). */
	for (index = 0; index < program->buffer_count; index++) {
		address = source_address;
		if (program->buffers[index].binding == TARGET_BINDING)
			address = target_address;
		word = program->buffers[index].fau_word;
		words[word] = (uint32_t)address;
		words[word + 1U] = (uint32_t)(address >> 32);
	}

	/* The FAU words into the stage. */
	error = mali_vk_meta_fau(build, words, program->fau_words, &fau);
	if (error != 0)
		return error;

	/* The empty tables, the program descriptor and the storage without local memory. */
	table = mali_vk_meta_tables(build);
	shader = mali_vk_build_shader(build, MALI_DESC_STAGE_COMPUTE, program->work_registers, program->preload, code);
	storage = mali_vk_build_thread_storage(build, 0);
	if (table == 0U || shader == 0U || storage == 0U)
		return build->error;

	/* One workgroup a block of eight by eight target texels of a layer, waited for. */
	counts[0] = (width + MALI_META_BLIT_GROUP - 1U) / MALI_META_BLIT_GROUP;
	counts[1] = (height + MALI_META_BLIT_GROUP - 1U) / MALI_META_BLIT_GROUP;
	counts[2] = layers;
	mali_vk_build_compute(build, table, fau, shader, storage, program->local_size, counts);

	/* Succeeded: the region is in the stream. */
	return 0;
}

/*
 * Writes one run of the aspect copy: one thread a texel of each layer,
 * the push constants the run's description, the source and the target
 * at their buffers' FAU words.  A clear's source is the target (not
 * read).
 */
int
mali_vk_meta_aspect(
	struct mali_vk_build *build,
	const struct mali_vk_meta_aspect *aspect)
{
	const struct mali_shader_program *program;
	uint32_t words[MALI_VK_META_FAU_WORDS];
	uint32_t counts[3];
	uint64_t source;
	uint64_t code;
	uint64_t fau;
	uint64_t table;
	uint64_t shader;
	uint64_t storage;
	uint64_t address;
	uint32_t index;
	uint32_t word;
	bool fits;
	int error;

	/* No texel to write: nothing runs. */
	if (aspect->width == 0U || aspect->height == 0U || aspect->layers == 0U)
		return 0;

	/* A clear reads nothing; its source points at the target so that no address is wild. */
	source = aspect->source;
	if ((aspect->flags & MALI_META_ASPECT_CONSTANT) != 0U)
		source = aspect->target;

	/* Both sides word aligned, their texels' offsets inside the program's range. */
	fits = aspect_side_fits(source, aspect->width, aspect->height, aspect->layers, aspect->source_pitch, aspect->source_layer, aspect->source_step);
	if (!fits)
		return ENOTSUP;
	fits = aspect_side_fits(aspect->target, aspect->width, aspect->height, aspect->layers, aspect->target_pitch, aspect->target_layer, aspect->target_step);
	if (!fits)
		return ENOTSUP;

	/* The program and its code. */
	error = mali_vk_meta_program(build, MALI_VK_META_ASPECT, &program, &code);
	if (error != 0)
		return ENOTSUP;

	/* The push constants: the extent, both sides' pitches, layers, steps and shifts, the field, the flags. */
	kern_memset(words, 0, sizeof(words));
	words[ASPECT_WIDTH] = aspect->width;
	words[ASPECT_HEIGHT] = aspect->height;
	words[ASPECT_SOURCE_PITCH] = (uint32_t)aspect->source_pitch;
	words[ASPECT_TARGET_PITCH] = (uint32_t)aspect->target_pitch;
	words[ASPECT_SOURCE_STEP] = aspect->source_step;
	words[ASPECT_TARGET_STEP] = aspect->target_step;
	words[ASPECT_SOURCE_SHIFT] = aspect->source_shift;
	words[ASPECT_TARGET_SHIFT] = aspect->target_shift;
	words[ASPECT_MASK] = aspect->mask;
	words[ASPECT_VALUE] = aspect->value;
	words[ASPECT_FLAGS] = aspect->flags;

	/* The layers' sizes, read only with a second layer (a single layer's may not fit a word). */
	if (aspect->layers > 1U) {
		words[ASPECT_SOURCE_LAYER] = (uint32_t)aspect->source_layer;
		words[ASPECT_TARGET_LAYER] = (uint32_t)aspect->target_layer;
	}

	/* Each buffer's address at its FAU words, by its binding (the program's shape was checked). */
	for (index = 0; index < program->buffer_count; index++) {
		address = source;
		if (program->buffers[index].binding == TARGET_BINDING)
			address = aspect->target;
		word = program->buffers[index].fau_word;
		words[word] = (uint32_t)address;
		words[word + 1U] = (uint32_t)(address >> 32);
	}

	/* The FAU words into the stage. */
	error = mali_vk_meta_fau(build, words, program->fau_words, &fau);
	if (error != 0)
		return error;

	/* The empty tables, the program descriptor and the storage without local memory. */
	table = mali_vk_meta_tables(build);
	shader = mali_vk_build_shader(build, MALI_DESC_STAGE_COMPUTE, program->work_registers, program->preload, code);
	storage = mali_vk_build_thread_storage(build, 0);
	if (table == 0U || shader == 0U || storage == 0U)
		return build->error;

	/* One workgroup a block of eight by eight texels of a layer, waited for. */
	counts[0] = (aspect->width + MALI_META_ASPECT_GROUP - 1U) / MALI_META_ASPECT_GROUP;
	counts[1] = (aspect->height + MALI_META_ASPECT_GROUP - 1U) / MALI_META_ASPECT_GROUP;
	counts[2] = aspect->layers;
	mali_vk_build_compute(build, table, fau, shader, storage, program->local_size, counts);

	/* Succeeded: the run is in the stream. */
	return 0;
}

/*
 * Compiles one of the executor's programs from its SPIR-V: the blit, the
 * aspect copy, the clear's vertex program, or the clear's fragment
 * program with its colour moved to the program's location.
 */
static int
compile_program(
	unsigned which,
	struct mali_shader_program **program)
{
	struct mali_shader_diagnostic diagnostic;
	const uint32_t *words;
	uint32_t *moved;
	const char *reason;
	size_t count;
	uint32_t stage;
	int error;

	/* The module and its stage. */
	*program = NULL;
	moved = NULL;
	if (which == MALI_VK_META_BLIT) {
		words = mali_meta_blit_spirv;
		count = mali_meta_blit_words;
		stage = MALI_SHADER_COMPUTE;
	} else if (which == MALI_VK_META_ASPECT) {
		words = mali_meta_aspect_spirv;
		count = mali_meta_aspect_words;
		stage = MALI_SHADER_COMPUTE;
	} else if (which == MALI_VK_META_CLEAR_VERTEX) {
		words = mali_meta_clear_vertex_spirv;
		count = mali_meta_clear_vertex_words;
		stage = MALI_SHADER_VERTEX;
	} else {
		moved = kern_malloc(mali_meta_clear_color_words * sizeof(uint32_t));
		if (moved == NULL)
			return ENOMEM;
		error = mali_meta_clear_color_at(moved, mali_meta_clear_color_words, which - MALI_VK_META_CLEAR_COLOR);
		if (error != 0) {
			kern_free(moved);
			return EINVAL;
		}
		words = moved;
		count = mali_meta_clear_color_words;
		stage = MALI_SHADER_FRAGMENT;
	}

	/* The program. */
	kern_memset(&diagnostic, 0, sizeof(diagnostic));
	error = mali_shader_compile(words, count, stage, NULL, program, &diagnostic);
	if (moved != NULL)
		kern_free(moved);
	if (error != 0) {
		reason = diagnostic.reason;
		if (reason == NULL)
			reason = "no reason";
		kern_logf("mali: vk: the program %s is not compiled (%d) at %u: %s; that work is not run\n",
			  meta_name(which),
			  error,
			  (unsigned)diagnostic.instruction,
			  reason);
		*program = NULL;
		return error;
	}

	/* Succeeded: the program is the caller's. */
	return 0;
}

/*
 * Gives the node's program, compiling it the first time: NULL when it
 * does not compile or does not have the shape its work writes
 * descriptors for, said once for the node, or after a submission that
 * ran it failed.  Two first submissions may both compile; one program
 * is kept and the other freed.
 */
static const struct mali_shader_program *
node_program(
	struct mali_render *render,
	unsigned which)
{
	struct mali_shader_program *program;
	struct mali_shader_program *kept;
	const char *reason;
	unsigned long enabled;
	bool refused;
	bool fits;
	int error;

	/* The program made already, or the refusal said already. */
	enabled = spin_lock_irqsave(&render->guard);

	kept = render->meta[which];
	refused = (render->meta_refused & (1U << which)) != 0U;

	spin_unlock_irqrestore(&render->guard, enabled);

	if (refused)
		return NULL;
	if (kept != NULL)
		return kept;

	/* The program from its SPIR-V. */
	error = compile_program(which, &program);
	if (error != 0)
		program = NULL;

	/* The shape its work writes descriptors for. */
	if (program != NULL) {
		reason = NULL;
		fits = program_fits(program, which, &reason);
		if (!fits) {
			kern_logf("mali: vk: the program %s %s; that work is not run\n", meta_name(which), reason);
			mali_shader_program_free(program);
			program = NULL;
		}
	}

	/* Keeps the first program made, or the refusal. */
	enabled = spin_lock_irqsave(&render->guard);

	if (program == NULL)
		render->meta_refused |= 1U << which;
	else if (render->meta[which] == NULL)
		render->meta[which] = program;
	kept = render->meta[which];
	refused = (render->meta_refused & (1U << which)) != 0U;

	spin_unlock_irqrestore(&render->guard, enabled);

	/* The program another submission kept first goes. */
	if (program != NULL && program != kept)
		mali_shader_program_free(program);

	/* A refusal, made here or by a failed submission meanwhile. */
	if (refused)
		return NULL;

	/* Succeeded: the kept program. */
	return kept;
}

/*
 * Says whether a compiled program has the shape its work writes
 * descriptors for: no textures, images or thread local storage; the
 * blit and the aspect copy each a compute program of eight by eight
 * threads with its two storage buffers at set 0, bindings 0 and 1,
 * whose lengths it does not read; the clear's vertex program only a
 * position from the vertex index and a rectangle; the clear's fragment
 * program one colour at its location, from push constants, without
 * inputs or a discard.
 */
static bool
program_fits(
	const struct mali_shader_program *program,
	unsigned which,
	const char **reason)
{
	bool fits;

	/* Nothing the executor's descriptors do not give. */
	if (program->texture_count != 0U || program->image_count != 0U) {
		*reason = "reads textures or images";
		return false;
	}

	if (program->tls_bytes != 0U || program->shared_bytes != 0U) {
		*reason = "needs local or shared memory";
		return false;
	}

	if (program->fau_words > MALI_VK_META_FAU_WORDS) {
		*reason = "reads too many FAU words";
		return false;
	}

	/* The blit: a compute program of its workgroup, its push constants and its two buffers. */
	if (which == MALI_VK_META_BLIT) {
		fits = compute_fits(program, MALI_META_BLIT_PUSH_BYTES, MALI_META_BLIT_GROUP, reason);
		if (!fits)
			return false;
		return true;
	}

	/* The aspect copy: the same shape with its own push constants. */
	if (which == MALI_VK_META_ASPECT) {
		fits = compute_fits(program, MALI_META_ASPECT_PUSH_BYTES, MALI_META_ASPECT_GROUP, reason);
		if (!fits)
			return false;
		return true;
	}

	/* The clear's programs read nothing but their push constants. */
	if (program->buffer_count != 0U || program->push_bytes != MALI_META_CLEAR_PUSH_BYTES) {
		*reason = "reads buffers or other push constants";
		return false;
	}

	/* The clear's vertex program: the position program alone, from the vertex index, with the viewport words. */
	if (which == MALI_VK_META_CLEAR_VERTEX) {
		if (program->stage != MALI_SHADER_VERTEX) {
			*reason = "is not a vertex program";
			return false;
		}

		if (program->attribute_mask != 0U || program->varying_offset != 0U || program->writes_point_size != 0U) {
			*reason = "reads attributes or writes varyings";
			return false;
		}

		if (program->viewport_fau_word + 6U > program->fau_words) {
			*reason = "has its viewport outside its FAU words";
			return false;
		}

		return true;
	}

	/* The clear's fragment program: one colour at its location. */
	if (program->stage != MALI_SHADER_FRAGMENT) {
		*reason = "is not a fragment program";
		return false;
	}

	if (program->color_mask != 1U << (which - MALI_VK_META_CLEAR_COLOR)) {
		*reason = "does not write its colour location alone";
		return false;
	}

	if (program->input_mask != 0U || program->discards != 0U) {
		*reason = "reads varyings or discards";
		return false;
	}

	/* Succeeded: the work's descriptors fit it. */
	return true;
}

/*
 * Says whether a compute program has a workgroup of group by group
 * threads of one layer, push_bytes of push constants and two storage
 * buffers at set 0, the source's and the target's bindings (0 and 1),
 * inside its FAU words, whose lengths it does not read.
 */
static bool
compute_fits(
	const struct mali_shader_program *program,
	uint32_t push_bytes,
	uint32_t group,
	const char **reason)
{
	uint32_t index;

	/* A compute program of its workgroup. */
	if (program->stage != MALI_SHADER_COMPUTE) {
		*reason = "is not a compute program";
		return false;
	}

	if (program->local_size[0] != group || program->local_size[1] != group || program->local_size[2] != 1U) {
		*reason = "has another workgroup size";
		return false;
	}

	/* Its push constants and two buffers. */
	if (program->push_bytes != push_bytes || program->buffer_count != 2U) {
		*reason = "does not read its push constants and two buffers";
		return false;
	}

	/* Each buffer: set 0, the source's or the target's binding, inside the FAU words, its length not read. */
	for (index = 0; index < program->buffer_count; index++) {
		if (program->buffers[index].set != 0U || program->buffers[index].binding > TARGET_BINDING) {
			*reason = "binds another buffer";
			return false;
		}

		if (program->buffers[index].fau_word + 2U > program->fau_words || program->buffers[index].size_fau_word != MALI_SHADER_NO_FAU_WORD) {
			*reason = "reads a buffer's address outside its FAU words or its length";
			return false;
		}
	}

	if (program->buffers[0].binding == program->buffers[1].binding) {
		*reason = "binds one buffer twice";
		return false;
	}

	/* Succeeded: the run's descriptors fit it. */
	return true;
}

/*
 * Says whether one side of an aspect copy fits the program: a word
 * aligned first texel, and the last byte of its last texel at an offset
 * below ASPECT_OFFSET_MAX.  The extent is not zero.
 */
static bool
aspect_side_fits(
	uint64_t address,
	uint32_t width,
	uint32_t height,
	uint32_t layers,
	uint64_t pitch,
	uint64_t layer,
	uint32_t step)
{
	uint64_t last;

	/* The program reads and writes whole words from the first texel's. */
	if ((address & 3U) != 0U)
		return false;

	/* Each stride inside the program's range (a layer's only when there is a second), then the last texel's last byte. */
	if (pitch > ASPECT_OFFSET_MAX || step > 4U)
		return false;
	if (layers > 1U && layer > ASPECT_OFFSET_MAX)
		return false;
	last = (uint64_t)(layers - 1U) * layer + (uint64_t)(height - 1U) * pitch + (uint64_t)(width - 1U) * step + 3U;
	if (last > ASPECT_OFFSET_MAX)
		return false;

	/* Succeeded: the side fits. */
	return true;
}

/*
 * Gives the session's copy of a program's code, made the first time: a
 * buffer object of the session holding the code from its start (the
 * start of every object is aligned beyond the code's alignment), mapped
 * in the session's space, freed with the session's other resources.  A
 * failure is said once for the session.
 */
static int
session_code(
	struct mali_session *session,
	const struct mali_shader_program *program,
	unsigned which,
	uint64_t *address)
{
	struct mali_resource *resource;
	struct mali_bo *bo;
	size_t index;
	int error;

	/* Made already, or failed already. */
	*address = session->meta_code[which];
	if (session->meta_code[which] != 0U)
		return 0;
	if ((session->meta_failed & (1U << which)) != 0U)
		return ENOMEM;

	/* The object. */
	error = mali_bo_create(session->render, program->length, &bo);
	if (error != 0) {
		kern_logf("mali: vk: the code of the program %s has no memory (%d); this session's such work is not run\n", meta_name(which), error);
		session->meta_failed |= 1U << which;
		return error;
	}

	/* The code, copied in through the uncached view before the GPU sees it. */
	for (index = 0; index < program->length; index++)
		bo->buffer.view[index] = program->code[index];

	/* Mapped in the session's space; the resource holds the only reference left. */
	error = mali_resource_make(session, bo, false, &resource);
	mali_bo_put(session->render, bo);
	if (error != 0) {
		kern_logf("mali: vk: the code of the program %s is not mapped (%d); this session's such work is not run\n", meta_name(which), error);
		session->meta_failed |= 1U << which;
		return error;
	}

	/* Succeeded: the code is at the resource's address for the session's life. */
	session->meta_code[which] = resource->gpu_address;
	*address = session->meta_code[which];
	return 0;
}

/* Gives what a program is, for the lines about it. */
static const char *
meta_name(
	unsigned which)
{
	/* The aspect copy's own, after the clear's colour programs that share one name. */
	if (which == MALI_VK_META_ASPECT)
		return meta_names[ASPECT_NAME];
	if (which >= MALI_VK_META_CLEAR_COLOR)
		return meta_names[MALI_VK_META_CLEAR_COLOR];

	/* The program's own. */
	return meta_names[which];
}

/* Gives the component order of a format the blit takes: four 8-bit components, RGBA or BGRA, UNORM or sRGB. */
static int
blit_order(
	const struct mali_vk_format *format)
{
	/* One case a format. */
	switch (format->vk) {
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB:
		return ORDER_RGBA;
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB:
		return ORDER_BGRA;
	default:
		return ORDER_NONE;
	}
}

/* Gives the layers of a blit's side inside its image: zero when they or the level do not fit. */
static uint32_t
blit_layers(
	const VkImageSubresourceLayers *layers,
	const struct mali_vk_image *image)
{
	uint32_t count;

	/* A level and a first layer of the image. */
	if (layers->mipLevel >= image->levels || layers->baseArrayLayer >= image->layers)
		return 0;

	/* The rest of the image, or the count when it fits. */
	count = layers->layerCount;
	if (count == VK_REMAINING_ARRAY_LAYERS)
		count = image->layers - layers->baseArrayLayer;
	if (count > image->layers - layers->baseArrayLayer)
		return 0;

	/* Succeeded: the layers. */
	return count;
}

/* Stores words through an uncached view. */
static void
store(
	volatile uint8_t *view,
	const uint32_t *words,
	unsigned count)
{
	volatile uint32_t *destination;
	unsigned index;

	/* Every word, in order. */
	destination = (volatile uint32_t *)(volatile void *)view;
	for (index = 0; index < count; index++)
		destination[index] = words[index];
}
