/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The kernels of a pipeline: compiling a graphics pipeline's stages (vertex,
 * optional geometry, fragment) or a compute pipeline's one, and handing what
 * the compiler reports to the draw or the dispatch.
 *
 * The pipeline's SPIR-V goes through the executor's own compiler, and the
 * words of 3DSTATE_VS, GS, PS, PS_EXTRA, WM, SBE and SBE_SWIZ are packed
 * from what that compiler reports about the kernels.  The kernels must fit
 * the fixed slots of the instruction heap (vk-heap.h), each stage's inputs
 * must be outputs of the stage before it (a geometry kernel reads the
 * vertex kernel's VUE, the pixel kernel the last stage's), and no stage may
 * read more push constants than a command buffer carries.
 */

#include "vk-gfx.h"
#include "vk-heap.h"
#include "vk-internal.h"
#include "vk-state.h"
#include <kern/kcrt.h>

#include "../compiler/compiler.h"
#include "../i915.h"

#include <kern/klog.h>
#include <kern/kmem.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

static int i915_pipeline_compile_stage(const struct i915_gfx_shader *shader, enum drv_gpu_shader_stage stage, const struct i915_shader_binary *producer, unsigned graphics_ver, uint32_t features, uint32_t view, uint32_t *reads_view, struct i915_shader_binary **result);
static int i915_pipeline_prepare_view(struct i915_render_session *session, struct i915_gfx_pipeline *pipeline, uint32_t view, uint32_t *reads_view);
static uint32_t i915_pipeline_first_view(uint32_t view_mask);
static unsigned i915_pipeline_graphics_ver(const struct i915_render_session *session);
static int i915_pipeline_geometry_inputs(const struct drv_gpu_shader_ir *ir, const struct i915_shader_binary *producer);
static int i915_pipeline_kernels_fit(const struct i915_gfx_pipeline *pipeline);
static int i915_pipeline_geometry_fits(const struct i915_shader_binary *geometry);
static const struct i915_shader_binary *i915_pipeline_last_stage(const struct i915_gfx_pipeline *pipeline);
static int i915_pipeline_input_slot(const struct i915_shader_binary *writer, uint32_t location, uint32_t *slot);
static const char *i915_pipeline_stage_name(enum drv_gpu_shader_stage stage);
static int i915_pipeline_compute_fits(const struct i915_shader_binary *binary);
static int i915_pipeline_thread_ids(struct i915_gfx_pipeline *pipeline);

/*
 * Compiles a pipeline's vertex, geometry and fragment kernels.
 *
 * The pipeline needs the vertex and the fragment stage; the geometry stage
 * is optional and is compiled after the vertex stage, whose
 * VUE it reads.  Returns the parser's or the compiler's error, or ENOTSUP
 * for kernels the draw path cannot place or connect; the pipeline is then
 * left without kernels.
 * XXX: kernels of at most 16 KiB, 16 KiB and 32 KiB.
 */
int
drv_i915_gfx_pipeline_prepare(
	struct i915_render_session *session,
	struct i915_gfx_pipeline *pipeline)
{
	struct i915_gfx_pipeline *variant;
	uint32_t first;
	uint32_t view;
	uint32_t reads_view;
	uint32_t ignored;
	int error;

	/* The pipeline's own kernels render the subpass's first view (view 0 without multiview). */
	first = i915_pipeline_first_view(pipeline->view_mask);
	error = i915_pipeline_prepare_view(session, pipeline, first, &reads_view);
	if (error != 0)
		return error;

	/* Kernels that do not read gl_ViewIndex render every view. */
	if (reads_view == 0U)
		return 0;

	/* Each other view of the subpass gets a copy of the pipeline with kernels for that view. */
	for (view = first + 1U; view < I915_GFX_MAX_VIEWS; view++) {
		/* A view the subpass does not render needs no kernels. */
		if ((pipeline->view_mask & (1U << view)) == 0U)
			continue;

		/* The copy shares the pipeline's shaders and state and owns no kernels yet. */
		variant = kern_calloc(1U, sizeof(*variant));
		if (variant == NULL) {
			drv_i915_gfx_pipeline_release(pipeline);
			return ENOMEM;
		}
		*variant = *pipeline;
		kern_memset(variant->view_pipelines, 0, sizeof(variant->view_pipelines));
		variant->kernel_owner = NULL;
		variant->kernel_window = 0U;
		variant->kernel_generation = 0U;
		variant->vs_binary = NULL;
		variant->gs_binary = NULL;
		variant->fs_binary = NULL;
		variant->kernels_ready = 0;

		/* Compiles the copy's kernels for the view. */
		error = i915_pipeline_prepare_view(session, variant, view, &ignored);
		if (error != 0) {
			kern_free(variant);
			drv_i915_gfx_pipeline_release(pipeline);
			return error;
		}
		pipeline->view_pipelines[view] = variant;
	}

	/* Succeeded: every view of the subpass has its kernels. */
	return 0;
}

/*
 * Compiles a pipeline's kernels for one view of its subpass (the value its
 * stages read as gl_ViewIndex) and says whether any stage reads it.
 */
static int
i915_pipeline_prepare_view(
	struct i915_render_session *session,
	struct i915_gfx_pipeline *pipeline,
	uint32_t view,
	uint32_t *reads_view)
{
	const struct i915_shader_binary *geometry;
	uint32_t stage_reads;
	int fits;
	int error;
	unsigned graphics_ver;

	/* No stage has read the view yet. */
	*reads_view = 0U;

	/* The kernels are compiled for the session's GT. */
	graphics_ver = i915_pipeline_graphics_ver(session);

	/* Refuses a pipeline whose create asked for what the draw path does not do (the decode logged it). */
	if (pipeline->refused != 0)
		return pipeline->refused;

	/* Refuses a pipeline without both the vertex and the fragment stage. */
	if (pipeline->vertex == NULL || pipeline->fragment == NULL)
		return EINVAL;

	/* Compiles the vertex stage. */
	error = i915_pipeline_compile_stage(pipeline->vertex, DRV_GPU_STAGE_VERTEX, NULL, graphics_ver, DRV_GPU_SPIRV_FEATURE_VIEW_INDEX, view, &stage_reads, &pipeline->vs_binary);
	if (error != 0) {
		drv_i915_gfx_pipeline_release(pipeline);
		return error;
	}
	*reads_view |= stage_reads;

	/* Compiles the geometry stage, when there is one, against the vertex kernel's VUE. */
	if (pipeline->geometry != NULL) {
		error = i915_pipeline_compile_stage(pipeline->geometry, DRV_GPU_STAGE_GEOMETRY, pipeline->vs_binary, graphics_ver, DRV_GPU_SPIRV_FEATURE_VIEW_INDEX, view, &stage_reads, &pipeline->gs_binary);
		if (error != 0) {
			drv_i915_gfx_pipeline_release(pipeline);
			return error;
		}
		*reads_view |= stage_reads;
	}

	/* Compiles the fragment stage. */
	error = i915_pipeline_compile_stage(pipeline->fragment, DRV_GPU_STAGE_FRAGMENT, NULL, graphics_ver, DRV_GPU_SPIRV_FEATURE_VIEW_INDEX, view, &stage_reads, &pipeline->fs_binary);
	if (error != 0) {
		drv_i915_gfx_pipeline_release(pipeline);
		return error;
	}
	*reads_view |= stage_reads;

	/* Refuses kernels the draw path cannot place or connect, naming the geometry kernel's numbers when there is one. */
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

		/* The geometry kernel's numbers, when there is one. */
		geometry = pipeline->gs_binary;
		if (geometry != NULL) {
			kern_logf("i915: vk: XXX unimplemented path: gs %u bytes / %u varyings / %u push registers (%u bytes of push constants) / %u sampled images\n",
				  geometry->code_bytes,
				  geometry->varying_count,
				  geometry->push_regs,
				  geometry->push_constant_bytes,
				  geometry->sampler_count);
		}

		/* The pipeline keeps no kernel. */
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

	/* Says what it made of the geometry stage, when there is one. */
	geometry = pipeline->gs_binary;
	if (geometry != NULL) {
		kern_logf("i915: vk: geometry stage compiled by the executor: gs %u bytes (%u inputs of %u vertices, %u push registers, "
			  "%u varyings, topology %u, vertex %u x 32 bytes, URB entry %u x 64 bytes, scratch %u bytes)\n",
			  geometry->code_bytes,
			  geometry->input_count,
			  geometry->vertices_in,
			  geometry->push_regs,
			  geometry->varying_count,
			  geometry->output_topology,
			  geometry->output_vertex_hwords,
			  geometry->urb_entry_size,
			  geometry->scratch_bytes);
	}

	/* Keeps whether the fragment kernel writes dual source, which the blend reads. */
	pipeline->dual_source = pipeline->fs_binary->dual_source;

	/* Marks the pipeline drawable. */
	pipeline->kernels_ready = 1;

	/* Succeeded: every stage's kernel is compiled and fits the draw path. */
	return 0;
}

/*
 * Compiles a compute pipeline's kernel and makes its threads' ID table.
 *
 * The kernel must fit an instruction window, its push data the room a
 * dispatch gives it, and its workgroup the threads one group may have.
 * Returns the parser's or the compiler's error, ENOTSUP for a kernel the
 * dispatch cannot place, or ENOMEM; the pipeline is then left without a
 * kernel.
 */
int
drv_i915_gfx_compute_prepare(
	struct i915_render_session *session,
	struct i915_gfx_pipeline *pipeline)
{
	uint32_t features;
	uint32_t reads_view;
	int fits;
	int error;
	unsigned graphics_ver;

	/* The kernels are compiled for the session's GT. */
	graphics_ver = i915_pipeline_graphics_ver(session);

	/* Refuses a pipeline without its compute stage. */
	if (pipeline->compute == NULL)
		return EINVAL;

	/* A pipeline made for vkCmdDispatchBase numbers its groups from the dispatch's base. */
	features = 0U;
	if (pipeline->dispatch_base != 0U)
		features = DRV_GPU_SPIRV_FEATURE_DISPATCH_BASE;

	/* Compiles the compute stage. */
	error = i915_pipeline_compile_stage(pipeline->compute, DRV_GPU_STAGE_COMPUTE, NULL, graphics_ver, features, 0U, &reads_view, &pipeline->cs_binary);
	if (error != 0) {
		drv_i915_gfx_pipeline_release(pipeline);
		return error;
	}

	/* Refuses a kernel the dispatch cannot place. */
	fits = i915_pipeline_compute_fits(pipeline->cs_binary);
	if (fits == 0) {
		kern_logf("i915: vk: XXX unimplemented path: cs %u bytes / %u push registers (%u bytes of push constants) / group %u x %u x %u\n",
			  pipeline->cs_binary->code_bytes,
			  pipeline->cs_binary->cross_thread_regs,
			  pipeline->cs_binary->push_constant_bytes,
			  pipeline->cs_binary->local_size[0],
			  pipeline->cs_binary->local_size[1],
			  pipeline->cs_binary->local_size[2]);
		drv_i915_gfx_pipeline_release(pipeline);
		return ENOTSUP;
	}

	/* Makes the table of the threads' IDs every dispatch delivers. */
	error = i915_pipeline_thread_ids(pipeline);
	if (error != 0) {
		drv_i915_gfx_pipeline_release(pipeline);
		return error;
	}

	/* Says what the compiler made of the pipeline. */
	kern_logf("i915: vk: compute pipeline compiled by the executor: cs %u bytes, group %u x %u x %u in %u threads, %u push registers\n",
		  pipeline->cs_binary->code_bytes,
		  pipeline->cs_binary->local_size[0],
		  pipeline->cs_binary->local_size[1],
		  pipeline->cs_binary->local_size[2],
		  pipeline->threads,
		  pipeline->cs_binary->cross_thread_regs);

	/* Marks the pipeline dispatchable. */
	pipeline->kernels_ready = 1;

	/* Succeeded: the kernel is compiled and fits the dispatch. */
	return 0;
}

/*
 * Releases a pipeline's kernels and marks it not drawable.
 */
void
drv_i915_gfx_pipeline_release(
	struct i915_gfx_pipeline *pipeline)
{
	uint32_t view;

	/* Releases and frees each view's copy, whose kernels are its own. */
	for (view = 0U; view < I915_GFX_MAX_VIEWS; view++) {
		if (pipeline->view_pipelines[view] == NULL)
			continue;
		drv_i915_gfx_pipeline_release(pipeline->view_pipelines[view]);
		kern_free(pipeline->view_pipelines[view]);
		pipeline->view_pipelines[view] = NULL;
	}

	/* Frees every binary and the compute threads' table; a stage never compiled has none. */
	drv_i915_shader_binary_free(pipeline->vs_binary);
	drv_i915_shader_binary_free(pipeline->gs_binary);
	drv_i915_shader_binary_free(pipeline->fs_binary);
	drv_i915_shader_binary_free(pipeline->cs_binary);
	if (pipeline->thread_ids != NULL)
		kern_free(pipeline->thread_ids);

	/* Forgets them, so the pipeline cannot be drawn or dispatched with. */
	pipeline->vs_binary = NULL;
	pipeline->gs_binary = NULL;
	pipeline->fs_binary = NULL;
	pipeline->cs_binary = NULL;
	pipeline->thread_ids = NULL;
	pipeline->threads = 0U;
	pipeline->right_mask = 0U;
	pipeline->kernels_ready = 0;
}

/*
 * Describes a prepared pipeline's kernels for the state and the batch of a
 * draw.
 *
 * The varyings, the point size and the fragment inputs' slots come from the
 * last stage before the fragment stage: the geometry kernel when there is
 * one, else the vertex kernel.  The code pointers borrow the
 * pipeline's binaries, which live until the pipeline is released.
 */
void
drv_i915_gfx_pipeline_kernels(
	const struct i915_gfx_pipeline *pipeline,
	struct i915_gfx_kernels *kernels)
{
	const struct i915_shader_binary *vertex;
	const struct i915_shader_binary *geometry;
	const struct i915_shader_binary *fragment;
	const struct i915_shader_binary *last;
	uint32_t index;
	uint32_t slot;
	int found;

	/* Starts from nothing. */
	kern_memset(kernels, 0, sizeof(*kernels));
	vertex = pipeline->vs_binary;
	geometry = pipeline->gs_binary;
	fragment = pipeline->fs_binary;
	last = i915_pipeline_last_stage(pipeline);

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

	/* Takes the vertex kernel's own varyings, which size its URB entries. */
	kernels->vs_varyings = vertex->varying_count;

	/* Takes the images the vertex kernel samples. */
	kernels->vs_samplers = vertex->sampler_count;
	kernels->vs_sampler_sets = vertex->sampler_set;
	kernels->vs_sampler_bindings = vertex->sampler_binding;
	kernels->vs_sampler_elements = vertex->sampler_element;

	/* Takes the geometry kernel's code, payload, push data and what 3DSTATE_GS and the URB are given. */
	if (geometry != NULL) {
		kernels->gs_code = geometry->code;
		kernels->gs_bytes = geometry->code_bytes;
		kernels->gs_grf_start = geometry->dispatch_grf_start;
		kernels->gs_push_regs = geometry->push_regs;
		kernels->gs_push.regs = geometry->push_regs;
		kernels->gs_push.constant_bytes = geometry->push_constant_bytes;
		kernels->gs_push.block_count = geometry->block_count;
		kernels->gs_push.blocks = geometry->blocks;
		kernels->gs_vertices_in = geometry->vertices_in;
		kernels->gs_output_topology = geometry->output_topology;
		kernels->gs_output_vertex_hwords = geometry->output_vertex_hwords;
		kernels->gs_control_hwords = geometry->control_data_hwords;
		kernels->gs_control_format = geometry->control_data_format;
		kernels->gs_urb_entry_size = geometry->urb_entry_size;
		kernels->gs_invocations = geometry->invocations;
		kernels->gs_primitive_id = geometry->uses_primitive_id;
		kernels->gs_writes_layer = geometry->writes_layer;
		kernels->gs_scratch_bytes = geometry->scratch_bytes;
		kernels->gs_samplers = geometry->sampler_count;
		kernels->gs_sampler_sets = geometry->sampler_set;
		kernels->gs_sampler_bindings = geometry->sampler_binding;
		kernels->gs_sampler_elements = geometry->sampler_element;
	}

	/* Takes the last stage's varyings, and whether it writes the point size. */
	kernels->varyings = last->varying_count;
	kernels->vs_point_size = last->writes_point_size;

	/*
	 * Finds the VUE slot of each fragment input; the fit check made sure the
	 * last stage writes every location the pixel kernel reads.
	 */
	kernels->ps_inputs_mapped = 1U;
	kernels->ps_input_count = fragment->input_count;
	kernels->ps_flat_mask = fragment->input_flat_mask;
	for (index = 0U; index < fragment->input_count && index < I915_GFX_MAX_VARYINGS; index++) {
		/* gl_PointCoord is the point sprite's coordinate, which the setup makes in place of a slot. */
		if (fragment->input_locations[index] == DRV_GPU_SHADER_LOCATION_POINT_COORD) {
			kernels->ps_point_sprite_mask |= 1U << index;
			kernels->ps_input_slots[index] = 0U;
			continue;
		}

		/* Any other input comes from the slot the last stage writes its location to. */
		slot = 0U;
		found = i915_pipeline_input_slot(last, fragment->input_locations[index], &slot);
		if (found != 0)
			slot = 0U;
		kernels->ps_input_slots[index] = slot;

		/* gl_PrimitiveID that no stage writes is the setup's (slot -1). */
		if (found != 0 && fragment->input_locations[index] == DRV_GPU_SHADER_LOCATION_PRIMITIVE_ID)
			kernels->ps_primitive_id_mask |= 1U << index;
	}

	/* Takes what the pixel kernel's payload carries beyond the perspective barycentrics. */
	kernels->ps_linear_barycentrics = fragment->uses_linear_barycentrics;
	kernels->ps_source_depth = fragment->uses_source_depth;
	kernels->ps_source_w = fragment->uses_source_w;
	kernels->ps_coverage_mask = fragment->uses_coverage_mask;

	/* Takes whether every write of the pixel kernel carries a computed depth and an oMask. */
	kernels->ps_computed_depth = fragment->writes_depth;
	kernels->ps_omask = fragment->writes_sample_mask;

	/* Takes the pixel kernel's first payload register and the images it samples. */
	kernels->ps_grf_start = fragment->dispatch_grf_start;
	kernels->ps_samplers = fragment->sampler_count;
	kernels->ps_sampler_sets = fragment->sampler_set;
	kernels->ps_sampler_bindings = fragment->sampler_binding;
	kernels->ps_sampler_elements = fragment->sampler_element;

	/* Takes the pixel kernel's push data. */
	kernels->ps_push_regs = fragment->push_regs;
	kernels->ps_push.regs = fragment->push_regs;
	kernels->ps_push.constant_bytes = fragment->push_constant_bytes;
	kernels->ps_push.block_count = fragment->block_count;
	kernels->ps_push.blocks = fragment->blocks;

	/* Takes whether the pixel kernel discards, which the pixel stage must be told. */
	kernels->ps_kills = fragment->uses_kill;

	/* Takes whether it writes memory and whether it asks for its tests first, which decide when depth is tested. */
	kernels->ps_writes_memory = fragment->writes_memory;
	kernels->ps_early_tests = fragment->early_fragment_tests;

	/* Takes the scratch memory each kernel spills to; the draw places the buffers. */
	kernels->vs_scratch_bytes = vertex->scratch_bytes;
	kernels->ps_scratch_bytes = fragment->scratch_bytes;
}

/*
 * Parses and compiles one stage of a pipeline; `producer` is the vertex
 * kernel a geometry stage reads, NULL for any other stage.
 *
 * A refusal is logged with the stage and, for the parser, the refused
 * instruction; a geometry shader that reads a location the vertex kernel
 * does not write is refused before the compiler with that location.
 */
static int
i915_pipeline_compile_stage(
	const struct i915_gfx_shader *shader,
	enum drv_gpu_shader_stage stage,
	const struct i915_shader_binary *producer,
	unsigned graphics_ver,
	uint32_t features,
	uint32_t view,
	uint32_t *reads_view,
	struct i915_shader_binary **result)
{
	struct drv_gpu_shader_ir *ir;
	struct drv_gpu_compile_diagnostic diagnostic;
	const char *reason;
	int error;

	/*
	 * Parses the SPIR-V into the compiler's IR, with the built-ins this
	 * compiler lowers beyond the shared profile and the arrays of
	 * descriptors, each element of which the draw and the dispatch bind.
	 */
	*reads_view = 0U;
	kern_memset(&diagnostic, 0, sizeof(diagnostic));
	error = drv_gpu_shader_parse_features(shader->words,
					      shader->word_count,
					      stage,
					      I915_SHADER_SPIRV_FEATURES | I915_SHADER_SPIRV_FEATURES_ARRAYS | features,
					      &ir,
					      &diagnostic);
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

	/*
	 * Refuses a module whose entry point is of another stage than the one it
	 * is used for: a compute shader named as a vertex stage, or the reverse,
	 * would be compiled as what it is and run as what it is not.
	 */
	if (ir->stage != stage) {
		kern_logf("i915: vk: %s stage given a %s shader\n",
			  i915_pipeline_stage_name(stage),
			  i915_pipeline_stage_name(ir->stage));
		drv_gpu_shader_ir_free(ir);
		return EINVAL;
	}

	/*
	 * gl_ViewIndex reads the view the kernel is compiled for (0 outside a
	 * multiview subpass, as the API says).
	 */
	*reads_view = ir->uses_view_index;
	drv_gpu_shader_ir_bind_view(ir, view);

	/* A geometry shader reads only locations the vertex kernel writes. */
	if (stage == DRV_GPU_STAGE_GEOMETRY) {
		error = i915_pipeline_geometry_inputs(ir, producer);
		if (error != 0) {
			drv_gpu_shader_ir_free(ir);
			return error;
		}
	}

	/*
	 * Compiles the IR to EU code after the producer.  The compiler gives no
	 * reason of its own: for a geometry shader the log names the input
	 * primitive and the vertices it may emit, which size its output URB
	 * entry (refused past 32 KiB).  The IR is freed either way.
	 */
	error = drv_i915_shader_compile_gen(ir, producer, graphics_ver, result);
	if (error != 0) {
		kern_logf("i915: vk: %s shader refused by the compiler: error %d\n", i915_pipeline_stage_name(stage), error);

		/* A geometry shader's input primitive and output vertices, which size its URB entry. */
		if (stage == DRV_GPU_STAGE_GEOMETRY) {
			kern_logf("i915: vk: the refused geometry shader: %u vertices in, at most %u vertices out (topology %u)\n",
				  ir->vertices_in,
				  ir->max_vertices,
				  ir->output_topology);
		}

		/* The IR goes with the refusal. */
		drv_gpu_shader_ir_free(ir);
		return error;
	}

	/* The IR is not needed once the binary is made. */
	drv_gpu_shader_ir_free(ir);

	/* Succeeded: the stage has its binary. */
	return 0;
}

/*
 * Reports whether a pipeline's compiled kernels fit the draw path.
 *
 * The vertex kernel must fit below the geometry kernel's slot, the geometry
 * kernel below the pixel kernel's, and the pixel kernel in the rest of the
 * instruction window; the last stage before the fragment stage must write
 * every location the fragment stage reads; no stage may read more push
 * constants than a command buffer carries, nor more push data than its
 * buffer holds; no kernel may sample more textures than its stage's
 * binding table has room for.
 */
static int
i915_pipeline_kernels_fit(
	const struct i915_gfx_pipeline *pipeline)
{
	const struct i915_shader_binary *last;
	uint32_t index;
	uint32_t slot;
	int fits;
	int found;

	/* The vertex kernel must fit its slot, below the geometry kernel's. */
	if (pipeline->vs_binary->code_bytes > I915_GFX_GS_KERNEL - I915_GFX_VS_KERNEL)
		return 0;

	/* The geometry kernel, when there is one, must fit its slot and its stage's limits. */
	if (pipeline->gs_binary != NULL) {
		fits = i915_pipeline_geometry_fits(pipeline->gs_binary);
		if (fits == 0)
			return 0;
	}

	/* The pixel kernel must fit the rest of the heap. */
	if (pipeline->fs_binary->code_bytes > I915_GFX_INSTRUCTION_BYTES - I915_GFX_PS_KERNEL)
		return 0;

	/* The pixel kernel reads no more inputs than the setup can route. */
	if (pipeline->fs_binary->input_count > I915_GFX_MAX_VARYINGS)
		return 0;

	/*
	 * The stages' interfaces must agree: every location the fragment kernel
	 * reads is one the last stage before it writes (it may read only some of
	 * them, in any order), but for gl_PointCoord, which the setup makes, and
	 * gl_PrimitiveID, which the setup makes when no stage writes it.
	 */
	last = i915_pipeline_last_stage(pipeline);
	for (index = 0U; index < pipeline->fs_binary->input_count; index++) {
		if (pipeline->fs_binary->input_locations[index] == DRV_GPU_SHADER_LOCATION_POINT_COORD)
			continue;
		found = i915_pipeline_input_slot(last, pipeline->fs_binary->input_locations[index], &slot);
		if (found != 0 && pipeline->fs_binary->input_locations[index] == DRV_GPU_SHADER_LOCATION_PRIMITIVE_ID)
			continue;
		if (found != 0) {
			kern_logf("i915: vk: the fragment shader reads location %u, which the %s shader does not write\n",
				  pipeline->fs_binary->input_locations[index],
				  i915_pipeline_stage_name(last->stage));
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

	/* The vertex and the pixel kernel's textures must fit their binding tables. */
	if (pipeline->vs_binary->sampler_count > I915_GFX_MAX_TEXTURES)
		return 0;
	if (pipeline->fs_binary->sampler_count > I915_GFX_MAX_TEXTURES)
		return 0;

	/* Succeeded: the kernels fit. */
	return 1;
}

/*
 * Checks a geometry shader's inputs against the vertex kernel it reads:
 * every located per-vertex input must be a varying of the vertex kernel
 * (gl_in's gl_Position and gl_PointSize are in every VUE).  Returns 0, or
 * ENOTSUP with the location logged.
 */
static int
i915_pipeline_geometry_inputs(
	const struct drv_gpu_shader_ir *ir,
	const struct i915_shader_binary *producer)
{
	const struct drv_gpu_shader_ir_inst *inst;
	uint32_t index;
	uint32_t slot;
	int found;

	/* A geometry stage is compiled after the vertex stage, whose kernel it is given. */
	if (producer == NULL)
		return EINVAL;

	/* Looks at every read of an input vertex's value. */
	for (index = 0U; index < ir->instruction_count; index++) {
		inst = &ir->instructions[index];
		if (inst->op != DRV_GPU_IR_LOAD_VERTEX_INPUT)
			continue;

		/* The position and the point size are in the header and the slot after it. */
		if (inst->location == DRV_GPU_IR_LOCATION_POSITION || inst->location == DRV_GPU_IR_LOCATION_POINT_SIZE)
			continue;

		/* A located input is one of the vertex kernel's varyings. */
		found = i915_pipeline_input_slot(producer, inst->location, &slot);
		if (found != 0) {
			kern_logf("i915: vk: the geometry shader reads location %u, which the vertex shader does not write\n", inst->location);
			return ENOTSUP;
		}
	}

	/* Succeeded: the vertex kernel writes every location the geometry shader reads. */
	return 0;
}

/*
 * Reports whether a geometry kernel fits the draw path: its slot of the
 * instruction window, the push constants a command buffer carries, the push
 * data room of its stage (as the other stages'), and no more sampled
 * images than its binding table holds.
 */
static int
i915_pipeline_geometry_fits(
	const struct i915_shader_binary *geometry)
{
	/* The geometry kernel must fit its slot, below the pixel kernel's. */
	if (geometry->code_bytes > I915_GFX_PS_KERNEL - I915_GFX_GS_KERNEL)
		return 0;

	/* Its push constants come from the command buffer's block. */
	if (geometry->push_constant_bytes > I915_GFX_PUSH_BYTES)
		return 0;

	/* Its push data must fit its buffer. */
	if (geometry->push_regs * 32U > I915_GFX_PUSH_DATA_BYTES)
		return 0;

	/* Its textures must fit its binding table. */
	if (geometry->sampler_count > I915_GFX_MAX_TEXTURES) {
		kern_logf("i915: vk: the geometry shader samples %u images, more than its binding table holds\n",
			  geometry->sampler_count);
		return 0;
	}

	/* Succeeded: the draw path can place the geometry kernel. */
	return 1;
}

/* Gives the last stage before a pipeline's fragment stage: its geometry kernel, or its vertex kernel. */
static const struct i915_shader_binary *
i915_pipeline_last_stage(
	const struct i915_gfx_pipeline *pipeline)
{
	/* A geometry kernel comes after the vertex kernel. */
	if (pipeline->gs_binary != NULL)
		return pipeline->gs_binary;

	/* Succeeded: the vertex kernel is the last. */
	return pipeline->vs_binary;
}

/*
 * Finds the VUE slot after the position in which a vertex or geometry
 * kernel writes a location; ENOENT when it writes none there.
 */
static int
i915_pipeline_input_slot(
	const struct i915_shader_binary *writer,
	uint32_t location,
	uint32_t *slot)
{
	uint32_t index;

	/* Looks the location up among the slots the kernel writes. */
	for (index = 0U; index < writer->varying_count && index < I915_SHADER_MAX_INPUTS; index++) {
		if (writer->varying_locations[index] == location) {
			/* Succeeded: the location is this slot. */
			*slot = index;
			return 0;
		}
	}

	/* The kernel does not write the location. */
	return ENOENT;
}

/* Names a stage in the log lines. */
static const char *
i915_pipeline_stage_name(
	enum drv_gpu_shader_stage stage)
{
	/* The vertex, the compute and the geometry stage by name. */
	if (stage == DRV_GPU_STAGE_VERTEX)
		return "vertex";
	if (stage == DRV_GPU_STAGE_COMPUTE)
		return "compute";
	if (stage == DRV_GPU_STAGE_GEOMETRY)
		return "geometry";

	/* Succeeded: every other stage is the fragment stage. */
	return "fragment";
}

/*
 * Reports whether a compute kernel fits the dispatch: the kernel an
 * instruction window, its push constants what a command buffer carries, its
 * cross-thread data the push data room of a slot, and its workgroup the
 * threads one group may have.
 */
static int
i915_pipeline_compute_fits(
	const struct i915_shader_binary *binary)
{
	uint32_t invocations;

	/* The kernel takes a whole instruction window: no vertex and pixel slots in a dispatch. */
	if (binary->code_bytes > I915_GFX_INSTRUCTION_BYTES)
		return 0;

	/* The push constants come from the command buffer's block. */
	if (binary->push_constant_bytes > I915_GFX_PUSH_BYTES)
		return 0;

	/* The cross-thread data takes no more room than one stage's push data. */
	if (binary->cross_thread_regs * 32U > I915_GFX_PUSH_DATA_BYTES)
		return 0;

	/* The compiler refused larger groups; one past the device's limit is inconsistent. */
	invocations = binary->local_size[0] * binary->local_size[1] * binary->local_size[2];
	if (invocations == 0U || invocations > DRV_GPU_SPIRV_MAX_GROUP_INVOCATIONS)
		return 0;

	/* Succeeded: the dispatch can place the kernel. */
	return 1;
}

/*
 * Makes a compute pipeline's table of its threads' IDs: for SIMD8 thread t
 * of a group, channel c is the group's invocation 8 t + c, whose local ID
 * along x, y and z and linear index go into the thread's four registers.
 * A channel past the group's invocations gets zeros; the dispatch does not
 * run it (the walker's right execution mask).  Returns 0 or ENOMEM.
 */
static int
i915_pipeline_thread_ids(
	struct i915_gfx_pipeline *pipeline)
{
	const struct i915_shader_binary *binary;
	uint32_t *table;
	uint32_t invocations;
	uint32_t threads;
	uint32_t thread;
	uint32_t channel;
	uint32_t linear;
	uint32_t base;
	uint32_t size_x;
	uint32_t size_y;
	uint32_t remainder;

	/* The group's invocations in SIMD8 threads. */
	binary = pipeline->cs_binary;
	size_x = binary->local_size[0];
	size_y = binary->local_size[1];
	invocations = size_x * size_y * binary->local_size[2];
	threads = (invocations + 7U) / 8U;

	/* Allocates the table: four registers of eight dwords to a thread, zeros for the channels that do not run. */
	table = kern_calloc((size_t)threads * I915_SHADER_PER_THREAD_REGS * 8U, sizeof(uint32_t));
	if (table == NULL)
		return ENOMEM;

	/* Fills each running channel's local IDs and linear index. */
	for (thread = 0U; thread < threads; thread++) {
		base = thread * I915_SHADER_PER_THREAD_REGS * 8U;
		for (channel = 0U; channel < 8U; channel++) {
			linear = thread * 8U + channel;
			if (linear >= invocations)
				break;
			table[base + channel] = linear % size_x;
			table[base + 8U + channel] = (linear / size_x) % size_y;
			table[base + 16U + channel] = linear / (size_x * size_y);
			table[base + 24U + channel] = linear;
		}
	}

	/* The last thread runs the channels of the group's remaining invocations, or all eight. */
	remainder = invocations % 8U;
	pipeline->right_mask = 0xffU;
	if (remainder != 0U)
		pipeline->right_mask = (1U << remainder) - 1U;

	/* Succeeded: the pipeline owns the table. */
	pipeline->thread_ids = table;
	pipeline->threads = threads;
	return 0;
}

/*
 * Returns the graphics version a session's kernels are compiled for: 9 on
 * a Gen9 GT, 11 on an Ice Lake GT, 12 on any other, and 12 without a session (the host tests
 * prepare pipelines with none).
 */
static unsigned
i915_pipeline_graphics_ver(
	const struct i915_render_session *session)
{
	/* No session: the Gen12 kernels the host tests compare. */
	if (session == NULL)
		return 12U;

	/* A Gen9 GT. */
	if (session->vk->i915->gt.graphics_ver == 9U)
		return 9U;

	/* An Ice Lake GT. */
	if (session->vk->i915->gt.graphics_ver == 11U)
		return 11U;

	/* Every other GT the driver runs is Gen12. */
	return 12U;
}

/* Returns the lowest view of a subpass's view mask; view 0 for a pass of one view. */
static uint32_t
i915_pipeline_first_view(
	uint32_t view_mask)
{
	uint32_t view;

	/* The lowest bit set. */
	for (view = 0U; view < I915_GFX_MAX_VIEWS; view++) {
		if ((view_mask & (1U << view)) != 0U)
			return view;
	}

	/* A pass of one view renders view 0. */
	return 0U;
}
