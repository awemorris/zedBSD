/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernels of a graphics pipeline: compiling its two stages and handing
 * what the compiler reports to the draw.
 *
 * The pipeline's SPIR-V goes through the executor's own compiler, and the
 * words of 3DSTATE_VS, PS, PS_EXTRA, WM, SBE and SBE_SWIZ are packed from
 * what that compiler reports about the kernels.  The kernels must fit the
 * fixed slots of the instruction heap (heap.h), the two stages' interfaces
 * must agree, and neither stage may read more push constants than a command
 * buffer carries.
 */

#include "gfx.h"
#include "heap.h"
#include "internal.h"
#include "state.h"
#include <kern/kcrt.h>

#include "../compiler/compiler.h"
#include "../i915.h"

#include <kern/klog.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

static int i915_pipeline_compile_stage(const struct i915_gfx_shader *shader, enum i915_shader_stage stage, struct i915_shader_binary **result);
static int i915_pipeline_kernels_fit(const struct i915_gfx_pipeline *pipeline);
static int i915_pipeline_input_slot(const struct i915_shader_binary *vertex, uint32_t location, uint32_t *slot);
static const char *i915_pipeline_stage_name(enum i915_shader_stage stage);

/*
 * Compiles a pipeline's vertex and fragment kernels.
 *
 * The pipeline needs both stages.  Returns the parser's or the compiler's
 * error, or ENOTSUP for kernels the draw path cannot place; the pipeline is
 * then left without kernels.
 * XXX: kernels of at most 16 KiB and 32 KiB.
 */
int
drv_i915_gfx_pipeline_prepare(
	struct i915_render_session *session,
	struct i915_gfx_pipeline *pipeline)
{
	int fits;
	int error;

	UNUSED_PARAMETER(session);

	/* Refuses a pipeline without both stages. */
	if (pipeline->vertex == NULL || pipeline->fragment == NULL)
		return EINVAL;

	/* Compiles the vertex stage. */
	error = i915_pipeline_compile_stage(pipeline->vertex, I915_STAGE_VERTEX, &pipeline->vs_binary);
	if (error != 0) {
		drv_i915_gfx_pipeline_release(pipeline);
		return error;
	}

	/* Compiles the fragment stage. */
	error = i915_pipeline_compile_stage(pipeline->fragment, I915_STAGE_FRAGMENT, &pipeline->fs_binary);
	if (error != 0) {
		drv_i915_gfx_pipeline_release(pipeline);
		return error;
	}

	/* Refuses kernels the draw path cannot place or connect. */
	fits = i915_pipeline_kernels_fit(pipeline);
	if (fits == 0) {
		kern_logf("i915: vk: XXX unimplemented path: vs %u bytes / %u varyings / %u push registers, "
			  "fs %u bytes / %u inputs / %u push registers\n",
			  pipeline->vs_binary->code_bytes,
			  pipeline->vs_binary->varying_count,
			  pipeline->vs_binary->push_regs,
			  pipeline->fs_binary->code_bytes,
			  pipeline->fs_binary->input_count,
			  pipeline->fs_binary->push_regs);
		drv_i915_gfx_pipeline_release(pipeline);
		return ENOTSUP;
	}

	/* Says what the compiler made of the pipeline. */
	kern_logf("i915: vk: pipeline compiled by the executor: vs %u bytes (%u attributes, %u push registers, %u varyings), "
		  "fs %u bytes (%u inputs, %u push registers, %u sampled images)\n",
		  pipeline->vs_binary->code_bytes,
		  pipeline->vs_binary->input_count,
		  pipeline->vs_binary->push_regs,
		  pipeline->vs_binary->varying_count,
		  pipeline->fs_binary->code_bytes,
		  pipeline->fs_binary->input_count,
		  pipeline->fs_binary->push_regs,
		  pipeline->fs_binary->sampler_count);

	/* Marks the pipeline drawable. */
	pipeline->kernels_ready = 1;

	/* Succeeded: both kernels are compiled and fit the draw path. */
	return 0;
}

/*
 * Releases a pipeline's kernels and marks it not drawable.
 */
void
drv_i915_gfx_pipeline_release(
	struct i915_gfx_pipeline *pipeline)
{
	/* Frees both binaries; a stage never compiled has none. */
	drv_i915_shader_binary_free(pipeline->vs_binary);
	drv_i915_shader_binary_free(pipeline->fs_binary);

	/* Forgets them, so the pipeline cannot be drawn with. */
	pipeline->vs_binary = NULL;
	pipeline->fs_binary = NULL;
	pipeline->kernels_ready = 0;
}

/*
 * Describes a prepared pipeline's two kernels for the state and the batch
 * of a draw.
 *
 * The code pointers borrow the pipeline's binaries, which live until the
 * pipeline is released.
 */
void
drv_i915_gfx_pipeline_kernels(
	const struct i915_gfx_pipeline *pipeline,
	struct i915_gfx_kernels *kernels)
{
	const struct i915_shader_binary *vertex;
	const struct i915_shader_binary *fragment;
	uint32_t index;
	uint32_t slot;
	int found;

	/* Starts from nothing. */
	kern_memset(kernels, 0, sizeof(*kernels));
	vertex = pipeline->vs_binary;
	fragment = pipeline->fs_binary;

	/* Takes the code of both kernels. */
	kernels->vs_code = vertex->code;
	kernels->vs_bytes = vertex->code_bytes;
	kernels->ps_code = fragment->code;
	kernels->ps_bytes = fragment->code_bytes;

	/* Takes the vertex kernel's payload start, push data and attribute locations. */
	kernels->vs_grf_start = vertex->dispatch_grf_start;
	kernels->vs_push_regs = vertex->push_regs;
	kernels->vs_push.regs = vertex->push_regs;
	kernels->vs_push.constant_bytes = vertex->push_constant_bytes;
	kernels->vs_push.block_count = vertex->block_count;
	kernels->vs_push.blocks = vertex->blocks;
	kernels->vs_input_count = vertex->input_count;
	for (index = 0U; index < kernels->vs_input_count && index < I915_GFX_MAX_VERTEX_ATTRIBUTES; index++)
		kernels->vs_inputs[index] = vertex->input_locations[index];

	/* Takes the varyings and the pixel kernel's payload start and sampled images. */
	kernels->varyings = vertex->varying_count;

	/*
	 * Finds the VUE slot of each fragment input; the fit check made sure the
	 * vertex kernel writes every location the pixel kernel reads.
	 */
	kernels->ps_inputs_mapped = 1U;
	kernels->ps_input_count = fragment->input_count;
	for (index = 0U; index < fragment->input_count && index < I915_GFX_MAX_VARYINGS; index++) {
		slot = 0U;
		found = i915_pipeline_input_slot(vertex, fragment->input_locations[index], &slot);
		if (found != 0)
			slot = 0U;
		kernels->ps_input_slots[index] = slot;
	}

	kernels->ps_grf_start = fragment->dispatch_grf_start;
	kernels->ps_samplers = fragment->sampler_count;
	kernels->ps_sampler_sets = fragment->sampler_set;
	kernels->ps_sampler_bindings = fragment->sampler_binding;

	/* Takes the pixel kernel's push data. */
	kernels->ps_push_regs = fragment->push_regs;
	kernels->ps_push.regs = fragment->push_regs;
	kernels->ps_push.constant_bytes = fragment->push_constant_bytes;
	kernels->ps_push.block_count = fragment->block_count;
	kernels->ps_push.blocks = fragment->blocks;

	/* Takes whether the pixel kernel discards, which the pixel stage must be told. */
	kernels->ps_kills = fragment->uses_kill;

	/* Takes the scratch memory each kernel spills to; the draw places the buffers. */
	kernels->vs_scratch_bytes = vertex->scratch_bytes;
	kernels->ps_scratch_bytes = fragment->scratch_bytes;
}

/*
 * Parses and compiles one stage of a pipeline.
 *
 * A refusal is logged with the stage and, for the parser, the refused
 * instruction.
 */
static int
i915_pipeline_compile_stage(
	const struct i915_gfx_shader *shader,
	enum i915_shader_stage stage,
	struct i915_shader_binary **result)
{
	struct i915_shader_ir *ir;
	struct i915_compile_diagnostic diagnostic;
	const char *reason;
	int error;

	/* Parses the SPIR-V into the compiler's IR. */
	kern_memset(&diagnostic, 0, sizeof(diagnostic));
	error = drv_i915_shader_parse(shader->words, shader->word_count, stage, &ir, &diagnostic);
	if (error != 0) {
		reason = "?";
		if (diagnostic.reason != NULL)
			reason = diagnostic.reason;
		kern_logf("i915: vk: %s shader refused by the SPIR-V parser: error %d (%s; opcode %u at word %u)\n",
			  i915_pipeline_stage_name(stage),
			  error,
			  reason,
			  diagnostic.opcode,
			  diagnostic.word_offset);
		return error;
	}

	/* Compiles the IR to EU code, and frees the IR either way. */
	error = drv_i915_shader_compile(ir, result);
	drv_i915_shader_ir_free(ir);
	if (error != 0) {
		kern_logf("i915: vk: %s shader refused by the compiler: error %d\n", i915_pipeline_stage_name(stage), error);
		return error;
	}

	/* Succeeded: the stage has its binary. */
	return 0;
}

/*
 * Reports whether a pipeline's compiled kernels fit the draw path.
 *
 * The vertex kernel must fit below the pixel kernel's slot and the pixel
 * kernel in the rest of the instruction heap; the vertex stage must write
 * every location the fragment stage reads; neither stage may read more push
 * constants than a command buffer carries, nor more push data than its
 * buffer holds; the pixel kernel may sample no more textures than the
 * binding table has room for, and the vertex kernel none (it has no
 * binding table).
 */
static int
i915_pipeline_kernels_fit(
	const struct i915_gfx_pipeline *pipeline)
{
	uint32_t index;
	uint32_t slot;
	int found;

	/* The vertex kernel must fit its slot. */
	if (pipeline->vs_binary->code_bytes > I915_GFX_PS_KERNEL - I915_GFX_VS_KERNEL)
		return 0;

	/* The pixel kernel must fit the rest of the heap. */
	if (pipeline->fs_binary->code_bytes > I915_GFX_INSTRUCTION_BYTES - I915_GFX_PS_KERNEL)
		return 0;

	/* The pixel kernel reads no more inputs than the setup can route. */
	if (pipeline->fs_binary->input_count > I915_GFX_MAX_VARYINGS)
		return 0;

	/*
	 * The stages' interfaces must agree: every location the fragment kernel
	 * reads is one the vertex kernel writes (it may read only some of them,
	 * in any order).
	 */
	for (index = 0U; index < pipeline->fs_binary->input_count; index++) {
		found = i915_pipeline_input_slot(pipeline->vs_binary, pipeline->fs_binary->input_locations[index], &slot);
		if (found != 0) {
			kern_logf("i915: vk: the fragment shader reads location %u, which the vertex shader does not write\n",
				  pipeline->fs_binary->input_locations[index]);
			return 0;
		}
	}

	/* The vertex stage may read no more push constants than a command buffer carries. */
	if (pipeline->vs_binary->push_constant_bytes > I915_GFX_PUSH_BYTES)
		return 0;

	/* Nor may the fragment stage, which reads the same block. */
	if (pipeline->fs_binary->push_constant_bytes > I915_GFX_PUSH_BYTES)
		return 0;

	/* The vertex stage's push data must fit its buffer. */
	if (pipeline->vs_binary->push_regs * 32U > I915_GFX_PUSH_DATA_BYTES)
		return 0;

	/* So must the pixel stage's. */
	if (pipeline->fs_binary->push_regs * 32U > I915_GFX_PUSH_DATA_BYTES)
		return 0;

	/* XXX: the vertex stage has no binding table, so a vertex kernel does not sample. */
	if (pipeline->vs_binary->sampler_count != 0U)
		return 0;

	/* The pixel kernel's textures must fit the binding table. */
	if (pipeline->fs_binary->sampler_count > I915_GFX_MAX_TEXTURES)
		return 0;

	/* Succeeded: the kernels fit. */
	return 1;
}

/*
 * Finds the VUE slot after the position in which a vertex kernel writes a
 * location; ENOENT when it writes none there.
 */
static int
i915_pipeline_input_slot(
	const struct i915_shader_binary *vertex,
	uint32_t location,
	uint32_t *slot)
{
	uint32_t index;

	/* Looks the location up among the slots the vertex kernel writes. */
	for (index = 0U; index < vertex->varying_count && index < I915_SHADER_MAX_INPUTS; index++) {
		if (vertex->varying_locations[index] == location) {
			/* Succeeded: the location is this slot. */
			*slot = index;
			return 0;
		}
	}

	/* The vertex kernel does not write the location. */
	return ENOENT;
}

/* Names a stage in the log lines. */
static const char *
i915_pipeline_stage_name(
	enum i915_shader_stage stage)
{
	/* Only the vertex stage is not a fragment stage here. */
	if (stage == I915_STAGE_VERTEX)
		return "vertex";

	/* Succeeded: every other stage is the fragment stage. */
	return "fragment";
}
