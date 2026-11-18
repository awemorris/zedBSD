/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pre-frame draw that reads a loaded colour attachment back into
 * the tiles.  See vk-preload.h.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/lock.h>
#include <uapi/errno.h>

#include "drivers/gpu/mali/vk/vk-desc.h"
#include "drivers/gpu/mali/vk/vk-frame.h"
#include "drivers/gpu/mali/vk/vk-preload.h"
#include "drivers/gpu/mali/vk/vk-preload-spirv.h"

/* The resource tables of the program's table pointer: textures and samplers, rounded up to the hardware's multiple. */
#define PRELOAD_TABLES		MALI_DESC_TABLE_ALIGN

static const struct mali_shader_program *node_program(struct mali_render *render);
static bool program_fits(const struct mali_shader_program *program, const char **reason);
static int session_code(struct mali_session *session, const struct mali_shader_program *program, uint64_t *address);
static int write_resources(struct mali_vk_build *build, const struct mali_vk_image_view *view, uint64_t *table);
static void store(volatile uint8_t *view, const uint32_t *words, unsigned count);

/*
 * Writes a pass's frame shader draws: the pre-frame 0 draw that runs the
 * program on each tile, reading the loaded view, with its program
 * descriptor, its resource table, its blend and its depth and stencil
 * state; pre-frame 1 and the post-frame draw stay empty (never run).
 *
 * Procedure referenced:
 * Mesa 26.2.6 src/panfrost/vulkan/panvk_vX_cmd_frame_shaders.c
 * (cmd_preload_color_attachments, cmd_emit_dcd, alloc_pre_post_dcds:
 * three draws, the first the colour preload, its program descriptor of
 * the fragment stage, a resource table of one texture and one sampler,
 * the blend of each loaded target, a depth/stencil that writes nothing).
 */
int
mali_vk_preload_draws(
	struct mali_vk_build *build,
	const struct mali_vk_preload *preload,
	uint64_t *draws)
{
	const struct mali_shader_program *program;
	struct mali_session *session;
	struct mali_frame_draw draw;
	uint32_t words[32];
	volatile uint8_t *blend_view;
	volatile uint8_t *depth_view;
	volatile uint8_t *draws_view;
	uint64_t code;
	uint64_t table;
	uint64_t shader;
	uint64_t blend;
	uint64_t depth;
	uint64_t address;
	int error;

	/* The node's program; without it the pass draws on cleared tiles. */
	*draws = 0;
	session = build->session->owner;
	program = node_program(session->render);
	if (program == NULL)
		return ENOTSUP;

	/* The session's copy of its code. */
	error = session_code(session, program, &code);
	if (error != 0)
		return ENOTSUP;

	/* The texture of the loaded view and the sampler, in their tables. */
	error = write_resources(build, preload->view, &table);
	if (error != 0)
		return error;

	/* The program descriptor of the fragment stage. */
	shader = mali_vk_build_shader(build, MALI_DESC_STAGE_FRAGMENT, program->work_registers, program->preload, code);
	if (shader == 0U)
		return build->error;

	/* The blend of colour 0 into render target 0, as the caller made it. */
	blend_view = mali_vk_build_take(build, MALI_DESC_BLEND_BYTES, 64U, &blend);
	if (blend_view == NULL)
		return build->error;
	store(blend_view, preload->blend, 4);

	/* The depth and stencil state that passes every fragment. */
	depth_view = mali_vk_build_take(build, MALI_DESC_BYTES, 64U, &depth);
	if (depth_view == NULL)
		return build->error;
	mali_frame_depth(words);
	store(depth_view, words, 8);

	/* The three draws, cleared; the first is the pre-frame 0 draw. */
	draws_view = mali_vk_build_take(build, MALI_FRAME_DRAWS_BYTES, 64U, &address);
	if (draws_view == NULL)
		return build->error;
	kern_memset(&draw, 0, sizeof(draw));
	draw.blend = blend;
	draw.blend_count = 1U;
	draw.resources = table;
	draw.shader = shader;
	draw.storage = preload->storage;
	draw.depth = depth;
	draw.target = 0U;
	mali_frame_draw(words, &draw);
	store(draws_view, words, 32);

	/* Succeeded: the framebuffer names the draws, and the submission knows it runs one. */
	*draws = address;
	session->preload_used = true;
	return 0;
}

/*
 * Gives the node's program, compiling it the first time: NULL when it
 * does not compile or does not have the shape the draw writes for, said
 * once for the node, or after a submission that ran it failed.  Two
 * first passes may both compile; one program is kept and the other
 * freed.
 */
static const struct mali_shader_program *
node_program(
	struct mali_render *render)
{
	struct mali_shader_program *program;
	struct mali_shader_program *kept;
	struct mali_shader_diagnostic diagnostic;
	const char *reason;
	unsigned long enabled;
	bool refused;
	bool fits;
	int error;

	/* The program made already, or the refusal said already. */
	enabled = spin_lock_irqsave(&render->guard);

	kept = render->preload;
	refused = render->preload_refused;

	spin_unlock_irqrestore(&render->guard, enabled);

	if (refused)
		return NULL;
	if (kept != NULL)
		return kept;

	/* The fragment program from its SPIR-V. */
	program = NULL;
	kern_memset(&diagnostic, 0, sizeof(diagnostic));
	error = mali_shader_compile(mali_preload_spirv, sizeof(mali_preload_spirv) / sizeof(mali_preload_spirv[0]), MALI_SHADER_FRAGMENT, NULL, &program, &diagnostic);
	if (error != 0) {
		reason = diagnostic.reason;
		if (reason == NULL)
			reason = "no reason";
		kern_logf("mali: vk: the program that loads an attachment is not compiled (%d) at %u: %s; loads draw on cleared tiles\n",
			  error,
			  (unsigned)diagnostic.instruction,
			  reason);
		program = NULL;
	}

	/* The shape the draw writes descriptors for. */
	if (program != NULL) {
		reason = NULL;
		fits = program_fits(program, &reason);
		if (!fits) {
			kern_logf("mali: vk: the program that loads an attachment %s; loads draw on cleared tiles\n", reason);
			mali_shader_program_free(program);
			program = NULL;
		}
	}

	/* Keeps the first program made, or the refusal. */
	enabled = spin_lock_irqsave(&render->guard);

	if (program == NULL)
		render->preload_refused = true;
	else if (render->preload == NULL)
		render->preload = program;
	kept = render->preload;
	refused = render->preload_refused;

	spin_unlock_irqrestore(&render->guard, enabled);

	/* The program another pass kept first goes. */
	if (program != NULL && program != kept)
		mali_shader_program_free(program);

	/* A refusal, made here or by a failed submission meanwhile. */
	if (refused)
		return NULL;

	/* Succeeded: the kept program. */
	return kept;
}

/*
 * Says whether the compiled program has the shape the draw writes
 * descriptors for: a fragment program of one texture at entry 0 whose
 * size it does not query, no buffers and no FAU words, writing colour 0.
 */
static bool
program_fits(
	const struct mali_shader_program *program,
	const char **reason)
{
	/* A fragment program. */
	if (program->stage != MALI_SHADER_FRAGMENT) {
		*reason = "is not a fragment program";
		return false;
	}

	/* The one texture at entry 0 of its tables, without a size. */
	if (program->texture_count != 1U || program->textures[0].index != 0U) {
		*reason = "does not read one texture at entry 0";
		return false;
	}

	if (program->textures[0].size_fau_word != MALI_SHADER_NO_FAU_WORD) {
		*reason = "reads its texture's size";
		return false;
	}

	/* Nothing through FAU words. */
	if (program->buffer_count != 0U || program->fau_words != 0U) {
		*reason = "reads FAU words";
		return false;
	}

	/* Colour 0 written. */
	if ((program->color_mask & 1U) == 0U) {
		*reason = "does not write colour 0";
		return false;
	}

	/* Succeeded: the draw's descriptors fit it. */
	return true;
}

/*
 * Gives the session's copy of the program's code, made the first time:
 * a buffer object of the session holding the code from its start (the
 * start of every object is aligned beyond the code's alignment), mapped
 * in the session's space, freed with the session's other resources.  A
 * failure is said once for the session.
 */
static int
session_code(
	struct mali_session *session,
	const struct mali_shader_program *program,
	uint64_t *address)
{
	struct mali_resource *resource;
	struct mali_bo *bo;
	size_t index;
	int error;

	/* Made already, or failed already. */
	*address = session->preload_code;
	if (session->preload_code != 0U)
		return 0;
	if (session->preload_failed)
		return ENOMEM;

	/* The object. */
	error = mali_bo_create(session->render, program->length, &bo);
	if (error != 0) {
		kern_logf("mali: vk: the code that loads an attachment has no memory (%d); this session's loads draw on cleared tiles\n", error);
		session->preload_failed = true;
		return error;
	}

	/* The code, copied in through the uncached view before the GPU sees it. */
	for (index = 0; index < program->length; index++)
		bo->buffer.view[index] = program->code[index];

	/* Mapped in the session's space; the resource holds the only reference left. */
	error = mali_resource_make(session, bo, false, &resource);
	mali_bo_put(session->render, bo);
	if (error != 0) {
		kern_logf("mali: vk: the code that loads an attachment is not mapped (%d); this session's loads draw on cleared tiles\n", error);
		session->preload_failed = true;
		return error;
	}

	/* Succeeded: the code is at the resource's address for the session's life. */
	session->preload_code = resource->gpu_address;
	*address = session->preload_code;
	return 0;
}

/*
 * Writes the program's resource table into the stage: table 0 holds the
 * loaded view's texture (with its plane), table 1 a sampler of exact
 * texels, the others nothing.  Reports the table pointer with its count,
 * ENOTSUP for a view of an image without memory, or the stage's error.
 */
static int
write_resources(
	struct mali_vk_build *build,
	const struct mali_vk_image_view *view,
	uint64_t *table)
{
	const struct mali_vk_image *image;
	const struct mali_surface_level *level;
	uint32_t words[8];
	volatile uint8_t *tables_view;
	volatile uint8_t *texture_view;
	volatile uint8_t *plane_view;
	volatile uint8_t *sampler_view;
	uint64_t tables;
	uint64_t texture;
	uint64_t plane;
	uint64_t sampler;
	uint64_t memory;

	/* The view's first level and layer in memory; an image not bound draws on cleared tiles. */
	*table = 0;
	image = view->image->payload;
	level = &image->level[0];
	if (view->base_level < image->levels)
		level = &image->level[view->base_level];
	memory = mali_vk_image_level_address(image, view->base_level, view->base_layer);
	if (memory == 0U)
		return ENOTSUP;

	/* The tables, cleared: those past the sampler's hold nothing. */
	tables_view = mali_vk_build_take(build, (uint64_t)PRELOAD_TABLES * MALI_DESC_RESOURCE_BYTES, 64U, &tables);
	if (tables_view == NULL)
		return build->error;

	/* The texture descriptor. */
	texture_view = mali_vk_build_take(build, MALI_DESC_BYTES, 64U, &texture);
	if (texture_view == NULL)
		return build->error;

	/* Its plane descriptor. */
	plane_view = mali_vk_build_take(build, MALI_DESC_BYTES, MALI_DESC_BYTES, &plane);
	if (plane_view == NULL)
		return build->error;

	/* The sampler descriptor. */
	sampler_view = mali_vk_build_take(build, MALI_DESC_BYTES, 64U, &sampler);
	if (sampler_view == NULL)
		return build->error;

	/* The plane of the view's first level and layer, linear. */
	mali_desc_plane(words, view->format->bytes, memory, level->pitch, level->bytes, level->width, level->height);
	store(plane_view, words, 8);

	/* The texture of the view's format and swizzle, naming the plane. */
	mali_desc_texture(words, view->format->pixel, level->width, level->height, view->swizzle, plane);
	store(texture_view, words, 8);

	/* The sampler. */
	mali_frame_sampler(words);
	store(sampler_view, words, 8);

	/* The texture table and the sampler table. */
	mali_desc_resource(words, texture, MALI_DESC_BYTES);
	store(tables_view + MALI_SHADER_TABLE_TEXTURES * MALI_DESC_RESOURCE_BYTES, words, 4);
	mali_desc_resource(words, sampler, MALI_DESC_BYTES);
	store(tables_view + MALI_SHADER_TABLE_SAMPLERS * MALI_DESC_RESOURCE_BYTES, words, 4);

	/* Succeeded: the table pointer with its count. */
	*table = tables | PRELOAD_TABLES;
	return 0;
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
