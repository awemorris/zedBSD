/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's graphics and compute pipelines.
 *
 * A pipeline's vertex and fragment modules are compiled for GFX10 by the
 * amdrdna compiler (compiler/gfx10-compile.c) when the pipeline is
 * created, and their code is copied into a run of the executor's GPU
 * memory the pipeline keeps, each program on a 256-byte boundary.  The
 * pipeline keeps the compiled programs too: their resource tables' layout
 * and their register values are what a draw writes.  It also keeps the
 * state a draw sends: the vertex input, the topology, the culling and the
 * depth bias, each colour attachment's blend and the constants, the
 * viewport and scissor, and the depth and stencil test, with which of them
 * a command buffer sets instead (the dynamic states a draw takes: the
 * viewport, scissor, line width, depth bias, blend constants and the
 * stencil's compare mask, write mask and reference).
 *
 * A pipeline that cannot be made says why: a state or a stage the
 * executor does not draw is a missing feature, a compile or a copy that
 * found no host memory is out of host memory, and a run of the executor's
 * GPU memory that could not be had for the programs is out of device
 * memory.
 *
 * The fragment program is compiled first: when it reads the primitive
 * number, the vertex program is compiled to export it.
 *
 * A compute pipeline has one program, its kernel, compiled and copied the
 * same way; a dispatch writes its registers and its resource table.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/amdrdna/vk/amdrdna-vk-objects.h"
#include "drivers/gpu/amdrdna/vk/amdrdna-vk-exec.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The most pipelines one create makes, and the most stages one pipeline lists. */
#define PIPELINE_CREATED_MAX		8U
#define PIPELINE_STAGES_MAX		4U

/* A program starts on this boundary of the pipeline's run (the code address's low 8 bits are not held). */
#define PIPELINE_PROGRAM_ALIGN		256U

/* The locations a vertex attribute may have. */
#define PIPELINE_LOCATIONS		16U

/* A sample mask of every sample: the 16 bits the rasteriser's mask of a pixel holds. */
#define PIPELINE_SAMPLE_MASK_ALL	0xffffU

/* The dynamic states a draw takes from its command buffer (a bit each, 1 << VkDynamicState). */
#define PIPELINE_DYNAMIC_TAKEN		((1U << VK_DYNAMIC_STATE_VIEWPORT) | (1U << VK_DYNAMIC_STATE_SCISSOR) | \
					 (1U << VK_DYNAMIC_STATE_LINE_WIDTH) | (1U << VK_DYNAMIC_STATE_DEPTH_BIAS) | \
					 (1U << VK_DYNAMIC_STATE_BLEND_CONSTANTS) | (1U << VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK) | \
					 (1U << VK_DYNAMIC_STATE_STENCIL_WRITE_MASK) | (1U << VK_DYNAMIC_STATE_STENCIL_REFERENCE))

/* The modules of a pipeline being made: the vertex and fragment modules, or the compute module. */
struct pipeline_build {
	const struct amdrdna_vk_shader *vertex_module;
	const struct amdrdna_vk_shader *fragment_module;
	const struct amdrdna_vk_shader *kernel_module;
	uint32_t colour_exports;
	uint32_t view_mask;
	uint32_t dispatch_base;
};

static int pipeline_create(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply, bool kernel);
static int pipeline_decode_kernel(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct amdrdna_vk_pipeline *pipeline, struct pipeline_build *build);
static int pipeline_compile_kernel(struct amdrdna_vk_session *session, struct amdrdna_vk_pipeline *pipeline, struct pipeline_build *build);
static int pipeline_decode(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct amdrdna_vk_pipeline *pipeline, struct pipeline_build *build);
static int pipeline_decode_stages(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct pipeline_build *build);
static int pipeline_decode_vertex_input(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct amdrdna_vk_pipeline *pipeline);
static void pipeline_decode_viewport(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct amdrdna_vk_pipeline *pipeline);
static void pipeline_decode_blend(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct amdrdna_vk_pipeline *pipeline);
static void pipeline_decode_dynamic(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct amdrdna_vk_pipeline *pipeline);
static int pipeline_compile(struct amdrdna_vk_session *session, struct amdrdna_vk_pipeline *pipeline, struct pipeline_build *build);
static int pipeline_compile_view(struct amdrdna_vk_session *session, struct amdrdna_vk_pipeline *pipeline, struct pipeline_build *build, uint32_t view);
static int pipeline_compile_stage(const struct amdrdna_vk_shader *module, enum drv_gpu_shader_stage stage, const struct amdrdna_gfx10_compile_options *options, struct amdrdna_vk_program *program);
static int pipeline_bindings(const struct amdrdna_gfx10_program *program);
static uint32_t pipeline_colour_exports(const struct amdrdna_vk_render_pass *pass, bool alpha_to_coverage);
static void pipeline_program(struct amdrdna_vk_program *program, uint8_t *cpu, uint64_t va);
static uint32_t pipeline_program_bytes(const struct amdrdna_gfx10_program *program);
static int pipeline_destroy(struct amdrdna_vk_session *session, void *payload);
static void pipeline_float(uint32_t *destination, const float *source);
static void pipeline_stencil_face(struct amdrdna_vk_stencil_face *face, const VkStencilOpState *state);

/*
 * Runs a pipeline command; *handled stays clear for an opcode of another
 * handler's.
 */
int
amdrdna_vk_pipeline_route(
	struct amdrdna_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	UNUSED_PARAMETER(requested);

	/* Picks the command, or leaves it to another handler. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CREATE_GRAPHICS_PIPELINES:
		error = pipeline_create(session, reader, reply, false);
		break;
	case GPU_OP_CREATE_COMPUTE_PIPELINES:
		error = pipeline_create(session, reader, reply, true);
		break;
	case GPU_OP_DESTROY_PIPELINE:
		error = amdrdna_vk_destroy_plain(session, reader, I915_VK_OBJ_PIPELINE);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* Reports a command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran and wrote its reply. */
	return 0;
}

/*
 * vkCreateGraphicsPipelines and vkCreateComputePipelines (kernel set):
 * [device][cache][count][count]{create info}[allocator][count][identities]
 * -> [result][count][identities].
 *
 * Every record is decoded (one that does not decode leaves the stream
 * unreadable and fails the command); then each pipeline is compiled and
 * published, and on the first failure none is left and every identity is
 * answered as null.
 */
static int
pipeline_create(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	bool kernel)
{
	struct amdrdna_vk_pipeline *pipelines[PIPELINE_CREATED_MAX];
	struct pipeline_build builds[PIPELINE_CREATED_MAX];
	uint64_t identities[PIPELINE_CREATED_MAX];
	struct amdrdna_vk_object *object;
	uint64_t count;
	uint64_t index;
	uint64_t published;
	uint64_t answered;
	uint32_t result;
	int refused;
	int decoded;
	int error;

	/* How many pipelines follow, behind the device, the cache and the count's first form. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (count == 0U || count > PIPELINE_CREATED_MAX)
		return EINVAL;

	/*
	 * Decodes every record into its pipeline: one that does not decode
	 * ends the command (the stream is unreadable after it), one with state
	 * the executor does not draw is read to its end and remembered.
	 */
	kern_memset(pipelines, 0, sizeof(pipelines));
	kern_memset(builds, 0, sizeof(builds));
	error = 0;
	refused = 0;
	for (index = 0U; index < count; index++) {
		pipelines[index] = kern_calloc(1U, sizeof(*pipelines[index]));
		if (pipelines[index] == NULL) {
			error = ENOMEM;
			break;
		}

		/* Each record decodes into an emptied scratch. */
		session->arena.used = 0U;
		if (kernel)
			decoded = pipeline_decode_kernel(session, reader, pipelines[index], &builds[index]);
		else
			decoded = pipeline_decode(session, reader, pipelines[index], &builds[index]);
		if (decoded == EINVAL) {
			error = EINVAL;
			break;
		}
		if (decoded != 0 && refused == 0)
			refused = decoded;
	}

	/* The identities behind the allocator, one per pipeline. */
	if (error == 0) {
		(void)drv_i915_wire_read_u64(reader);
		answered = drv_i915_wire_read_u64(reader);
		if (answered != count)
			error = EINVAL;
		for (index = 0U; error == 0 && index < count; index++)
			identities[index] = drv_i915_wire_read_u64(reader);
		if (reader->error != 0)
			error = EINVAL;
	}

	/* A command that could not be read to its end fails the frame. */
	if (error != 0) {
		for (index = 0U; index < count; index++) {
			if (pipelines[index] != NULL)
				(void)pipeline_destroy(session, pipelines[index]);
		}
		return error;
	}

	/* A refused record fails the create with its result. */
	error = refused;

	/* Compiles each pipeline's programs into its run. */
	for (index = 0U; error == 0 && index < count; index++) {
		if (kernel)
			error = pipeline_compile_kernel(session, pipelines[index], &builds[index]);
		else
			error = pipeline_compile(session, pipelines[index], &builds[index]);
	}

	/* Publishes each one, up to the first failure. */
	published = 0U;
	for (index = 0U; error == 0 && index < count; index++) {
		error = amdrdna_vk_object_publish(session, I915_VK_OBJ_PIPELINE, identities[index], pipelines[index], pipeline_destroy, &object);
		if (error != 0)
			break;
		pipelines[index] = NULL;
		published++;
	}

	/* A create that failed leaves no pipeline. */
	if (error != 0) {
		kern_logf("amdrdna: vk: vkCreate%sPipelines failed: %d\n", kernel ? "Compute" : "Graphics", error);
		for (index = 0U; index < published; index++)
			(void)amdrdna_vk_object_remove(session, I915_VK_OBJ_PIPELINE, identities[index]);
	}

	/* What was not published goes. */
	for (index = 0U; index < count; index++) {
		if (pipelines[index] != NULL)
			(void)pipeline_destroy(session, pipelines[index]);
	}

	/* The result and the count, then each identity, or null for each when the create failed. */
	result = amdrdna_vk_result(error);
	drv_i915_wire_reply_u32(reply, result);
	drv_i915_wire_reply_u64(reply, count);
	for (index = 0U; index < count; index++) {
		answered = 0U;
		if (error == 0)
			answered = identities[index];
		drv_i915_wire_reply_u64(reply, answered);
	}

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * Decodes one VkComputePipelineCreateInfo as libvulkan's
 * pipeline_encode_compute() writes it: [sType][pNext][flags]{stage}
 * [layout][base pipeline][base index].  The compute stage of a known
 * module, without specialization, is taken.
 *
 * Returns EINVAL when the record did not decode (the stream is then
 * unreadable), ENOTSUP for a stage the executor does not run (the record
 * is still read to its end).
 */
static int
pipeline_decode_kernel(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_pipeline *pipeline,
	struct pipeline_build *build)
{
	VkComputePipelineCreateInfo info;
	const struct amdrdna_vk_pipeline_layout *layout;

	/* The record. */
	kern_memset(&info, 0, sizeof(info));
	i915_vkc_dec_VkComputePipelineCreateInfo(reader, &session->arena, &info);
	if (reader->error != 0)
		return EINVAL;

	/* A compute pipeline; its layout's push constant bytes. */
	pipeline->compute = 1U;
	layout = amdrdna_vk_payload(session, I915_VK_OBJ_PIPELINE_LAYOUT, (uint64_t)(uintptr_t)info.layout);
	if (layout != NULL)
		pipeline->push_bytes = layout->push_bytes;

	/* A pipeline for vkCmdDispatchBase counts its workgroups from the dispatch's base. */
	if ((info.flags & VK_PIPELINE_CREATE_DISPATCH_BASE_BIT) != 0U)
		build->dispatch_base = 1U;

	/* The compute stage of a known module, without specialization constants. */
	if (info.stage.stage != VK_SHADER_STAGE_COMPUTE_BIT)
		return ENOTSUP;
	if (info.stage.pSpecializationInfo != NULL && info.stage.pSpecializationInfo->mapEntryCount != 0U)
		return ENOTSUP;
	build->kernel_module = amdrdna_vk_payload(session, I915_VK_OBJ_SHADER_MODULE, (uint64_t)(uintptr_t)info.stage.module);
	if (build->kernel_module == NULL)
		return ENOTSUP;

	/* Succeeded: the pipeline's module is in the build. */
	return 0;
}

/*
 * Decodes one VkGraphicsPipelineCreateInfo as libvulkan's
 * pipeline_encode_graphics() writes it: the head and the stages, each state
 * record behind its presence word (rasterization always present), then
 * [layout][render pass][subpass][base pipeline][base index].
 *
 * Returns EINVAL when the record did not decode (the stream is then
 * unreadable), ENOTSUP for state the executor does not draw (the record is
 * still read to its end).
 */
static int
pipeline_decode(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_pipeline *pipeline,
	struct pipeline_build *build)
{
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineTessellationStateCreateInfo tessellation;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineDepthStencilStateCreateInfo depth;
	const struct amdrdna_vk_pipeline_layout *layout;
	const struct amdrdna_vk_render_pass *pass;
	uint64_t present;
	uint64_t layout_id;
	uint64_t pass_id;
	uint32_t index;
	int refused;
	int error;

	/* The stages. */
	refused = 0;
	error = pipeline_decode_stages(session, reader, build);
	if (error == EINVAL)
		return error;
	if (error != 0)
		refused = error;

	/* The vertex input. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		error = pipeline_decode_vertex_input(session, reader, pipeline);
		if (error != 0 && refused == 0)
			refused = error;
	}

	/* The input assembly: the topology (a triangle list unless said). */
	pipeline->topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		kern_memset(&assembly, 0, sizeof(assembly));
		i915_vkc_dec_VkPipelineInputAssemblyStateCreateInfo(reader, &session->arena, &assembly);
		pipeline->topology = assembly.topology;
	}

	/* The tessellation state, not used. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		kern_memset(&tessellation, 0, sizeof(tessellation));
		i915_vkc_dec_VkPipelineTessellationStateCreateInfo(reader, &session->arena, &tessellation);
	}

	/* The viewport and scissor. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		pipeline_decode_viewport(session, reader, pipeline);

	/* The rasterization: the culling and the depth bias. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		kern_memset(&raster, 0, sizeof(raster));
		i915_vkc_dec_VkPipelineRasterizationStateCreateInfo(reader, &session->arena, &raster);
		pipeline->cull_mode = raster.cullMode;
		pipeline->front_face = raster.frontFace;
		if (raster.depthBiasEnable != VK_FALSE)
			pipeline->depth_bias_enable = 1U;
		pipeline_float(&pipeline->depth_bias[0], &raster.depthBiasConstantFactor);
		pipeline_float(&pipeline->depth_bias[1], &raster.depthBiasClamp);
		pipeline_float(&pipeline->depth_bias[2], &raster.depthBiasSlopeFactor);
		if (raster.rasterizerDiscardEnable != VK_FALSE && refused == 0)
			refused = ENOTSUP;
	}

	/*
	 * The multisample state: one sample or AMDRDNA_VK_SAMPLES_MAX, the
	 * sample mask (every sample without one), the alpha to coverage; no
	 * sample shading and no alpha forced to one (features the device does
	 * not report).
	 *
	 * Values referenced: the Vulkan 1.3 specification
	 * (VkPipelineMultisampleStateCreateInfo: pSampleMask NULL as all ones;
	 * sampleShadingEnable and alphaToOneEnable need their features).
	 */
	pipeline->samples = 1U;
	pipeline->sample_mask = PIPELINE_SAMPLE_MASK_ALL;
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		kern_memset(&multisample, 0, sizeof(multisample));
		i915_vkc_dec_VkPipelineMultisampleStateCreateInfo(reader, &session->arena, &multisample);
		pipeline->samples = (uint32_t)multisample.rasterizationSamples;
		if (multisample.pSampleMask != NULL)
			pipeline->sample_mask = multisample.pSampleMask[0] & PIPELINE_SAMPLE_MASK_ALL;
		if (multisample.alphaToCoverageEnable != VK_FALSE)
			pipeline->alpha_to_coverage = 1U;
		if (pipeline->samples != 1U && pipeline->samples != AMDRDNA_VK_SAMPLES_MAX && refused == 0)
			refused = ENOTSUP;
		if ((multisample.sampleShadingEnable != VK_FALSE || multisample.alphaToOneEnable != VK_FALSE) && refused == 0)
			refused = ENOTSUP;
	}

	/*
	 * The depth and stencil state: the depth test, write and compare, and
	 * the stencil test of each face (a pass without a depth attachment, or
	 * without stencil, does not use them); no depth bounds test.
	 */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		kern_memset(&depth, 0, sizeof(depth));
		i915_vkc_dec_VkPipelineDepthStencilStateCreateInfo(reader, &session->arena, &depth);
		pipeline->depth_test = depth.depthTestEnable;
		pipeline->depth_write = depth.depthWriteEnable;
		pipeline->depth_compare = (uint32_t)depth.depthCompareOp;
		if (depth.stencilTestEnable != VK_FALSE)
			pipeline->stencil_test = 1U;
		pipeline_stencil_face(&pipeline->stencil[0], &depth.front);
		pipeline_stencil_face(&pipeline->stencil[1], &depth.back);
		if (depth.depthBoundsTestEnable != VK_FALSE && refused == 0)
			refused = ENOTSUP;
	}

	/* The colour blend; without it every channel of every target is written unblended. */
	for (index = 0U; index < AMDRDNA_VK_COLOURS_MAX; index++)
		pipeline->blend[index].write_mask = 0xfU;
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		pipeline_decode_blend(session, reader, pipeline);

	/* The dynamic state. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		pipeline_decode_dynamic(session, reader, pipeline);

	/*
	 * The layout (its push constant bytes) and the render pass (its colour
	 * attachments' formats, which say how the fragment program exports
	 * each colour); the subpass and the base pipeline are not used.
	 */
	layout_id = drv_i915_wire_read_u64(reader);
	pass_id = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;
	layout = amdrdna_vk_payload(session, I915_VK_OBJ_PIPELINE_LAYOUT, layout_id);
	if (layout != NULL)
		pipeline->push_bytes = layout->push_bytes;
	pass = amdrdna_vk_payload(session, I915_VK_OBJ_RENDER_PASS, pass_id);
	if (pass != NULL) {
		build->colour_exports = pipeline_colour_exports(pass, pipeline->alpha_to_coverage != 0U);
		build->view_mask = pass->view_mask;
	}

	/* Points, lines and triangles without adjacency or patches. */
	if (pipeline->topology > VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN && refused == 0)
		refused = ENOTSUP;

	/* Reports state the executor does not draw. */
	if (refused != 0)
		return refused;

	/* Succeeded: the pipeline holds its state; its modules are in the build. */
	return 0;
}

/*
 * Decodes the head and the stages: [sType][pNext][flags][stageCount]
 * [count]{VkPipelineShaderStageCreateInfo}.  A vertex and a fragment stage
 * of known modules, without specialization, are taken; anything else is
 * ENOTSUP after the stages are read.
 */
static int
pipeline_decode_stages(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct pipeline_build *build)
{
	VkPipelineShaderStageCreateInfo stage;
	const struct amdrdna_vk_shader *module;
	uint64_t stages;
	uint64_t index;
	int refused;

	/* How many stages follow, behind sType, pNext, flags and the count's first form. */
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	stages = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (stages > PIPELINE_STAGES_MAX)
		return EINVAL;

	/* Each stage. */
	refused = 0;
	for (index = 0U; index < stages; index++) {
		kern_memset(&stage, 0, sizeof(stage));
		i915_vkc_dec_VkPipelineShaderStageCreateInfo(reader, &session->arena, &stage);
		if (reader->error != 0)
			return EINVAL;

		/* Specialization constants are not taken. */
		if (stage.pSpecializationInfo != NULL && stage.pSpecializationInfo->mapEntryCount != 0U) {
			refused = ENOTSUP;
			continue;
		}

		/* The vertex and the fragment stage keep their modules. */
		module = amdrdna_vk_payload(session, I915_VK_OBJ_SHADER_MODULE, (uint64_t)(uintptr_t)stage.module);
		if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT) {
			build->vertex_module = module;
		} else if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
			build->fragment_module = module;
		} else {
			kern_logf("amdrdna: vk: pipeline stage 0x%x is not run\n", (unsigned)stage.stage);
			refused = ENOTSUP;
		}
	}

	/* Both programs, of known modules. */
	if (build->vertex_module == NULL || build->fragment_module == NULL) {
		if (refused == 0)
			refused = ENOTSUP;
	}

	/* Reports stages the executor does not run. */
	if (refused != 0)
		return refused;

	/* Succeeded: the two modules are known. */
	return 0;
}

/*
 * Decodes the vertex input: its bindings and attributes.  Float
 * attributes of one to four components at the generic locations, from as
 * many buffers as a draw binds; anything else is ENOTSUP.
 */
static int
pipeline_decode_vertex_input(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_pipeline *pipeline)
{
	VkPipelineVertexInputStateCreateInfo input;
	const VkVertexInputAttributeDescription *attribute;
	uint32_t components;
	uint32_t bytes;
	uint32_t index;
	uint32_t code;

	/* The record. */
	kern_memset(&input, 0, sizeof(input));
	i915_vkc_dec_VkPipelineVertexInputStateCreateInfo(reader, &session->arena, &input);
	if (reader->error != 0)
		return EINVAL;

	/* No more bindings and attributes than a draw sends. */
	if (input.vertexBindingDescriptionCount > AMDRDNA_VK_VERTEX_BINDINGS_MAX)
		return ENOTSUP;
	if (input.vertexAttributeDescriptionCount > AMDRDNA_VK_VERTEX_ATTRIBUTES_MAX)
		return ENOTSUP;

	/* Each binding's number, stride and rate. */
	pipeline->binding_count = input.vertexBindingDescriptionCount;
	for (index = 0U; index < pipeline->binding_count; index++) {
		if (input.pVertexBindingDescriptions[index].binding >= AMDRDNA_VK_VERTEX_BINDINGS_MAX)
			return ENOTSUP;
		pipeline->bindings[index].binding = input.pVertexBindingDescriptions[index].binding;
		pipeline->bindings[index].stride = input.pVertexBindingDescriptions[index].stride;
		pipeline->bindings[index].instance = 0U;
		if (input.pVertexBindingDescriptions[index].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE)
			pipeline->bindings[index].instance = 1U;
	}

	/* Each attribute: a location, a bound buffer, a format the vertex fetch reads. */
	pipeline->attribute_count = input.vertexAttributeDescriptionCount;
	for (index = 0U; index < pipeline->attribute_count; index++) {
		attribute = &input.pVertexAttributeDescriptions[index];
		if (attribute->location >= PIPELINE_LOCATIONS)
			return ENOTSUP;
		if (attribute->binding >= AMDRDNA_VK_VERTEX_BINDINGS_MAX)
			return ENOTSUP;
		if (!amdrdna_vk_vertex_format(attribute->format, &code, &components, &bytes))
			return ENOTSUP;

		pipeline->attributes[index].location = attribute->location;
		pipeline->attributes[index].binding = attribute->binding;
		pipeline->attributes[index].format = attribute->format;
		pipeline->attributes[index].offset = attribute->offset;
	}

	/* Succeeded: the vertex input is kept. */
	return 0;
}

/* Decodes the viewport state: its first viewport and scissor when it gives them. */
static void
pipeline_decode_viewport(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_pipeline *pipeline)
{
	VkPipelineViewportStateCreateInfo viewport;

	/* The record; one that did not decode keeps nothing. */
	kern_memset(&viewport, 0, sizeof(viewport));
	i915_vkc_dec_VkPipelineViewportStateCreateInfo(reader, &session->arena, &viewport);
	if (reader->error != 0)
		return;

	/* The first viewport, as float bits. */
	if (viewport.pViewports != NULL && viewport.viewportCount != 0U) {
		pipeline_float(&pipeline->viewport[0], &viewport.pViewports[0].x);
		pipeline_float(&pipeline->viewport[1], &viewport.pViewports[0].y);
		pipeline_float(&pipeline->viewport[2], &viewport.pViewports[0].width);
		pipeline_float(&pipeline->viewport[3], &viewport.pViewports[0].height);
		pipeline_float(&pipeline->viewport[4], &viewport.pViewports[0].minDepth);
		pipeline_float(&pipeline->viewport[5], &viewport.pViewports[0].maxDepth);
	}

	/* The first scissor. */
	if (viewport.pScissors != NULL && viewport.scissorCount != 0U)
		pipeline->scissor = viewport.pScissors[0];
}

/*
 * Decodes the colour blend state: the blend and write mask of each
 * attachment (each its own: the targets blend independently), and the
 * blend constants.  Attachments past the colour targets are not read.
 */
static void
pipeline_decode_blend(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_pipeline *pipeline)
{
	VkPipelineColorBlendStateCreateInfo blend;
	const VkPipelineColorBlendAttachmentState *attachment;
	struct amdrdna_vk_blend *target;
	uint32_t count;
	uint32_t index;

	/* The record; one that did not decode, or without attachments, blends nothing. */
	kern_memset(&blend, 0, sizeof(blend));
	i915_vkc_dec_VkPipelineColorBlendStateCreateInfo(reader, &session->arena, &blend);
	if (reader->error != 0)
		return;

	/* The constants (floats as their bits). */
	pipeline_float(&pipeline->blend_constants[0], &blend.blendConstants[0]);
	pipeline_float(&pipeline->blend_constants[1], &blend.blendConstants[1]);
	pipeline_float(&pipeline->blend_constants[2], &blend.blendConstants[2]);
	pipeline_float(&pipeline->blend_constants[3], &blend.blendConstants[3]);
	if (blend.attachmentCount == 0U || blend.pAttachments == NULL)
		return;

	/* Each attachment's blend, factors, operations and write mask. */
	count = blend.attachmentCount;
	if (count > AMDRDNA_VK_COLOURS_MAX)
		count = AMDRDNA_VK_COLOURS_MAX;
	for (index = 0U; index < count; index++) {
		attachment = &blend.pAttachments[index];
		target = &pipeline->blend[index];
		target->enable = 0U;
		if (attachment->blendEnable != VK_FALSE)
			target->enable = 1U;
		target->colour_source = attachment->srcColorBlendFactor;
		target->colour_destination = attachment->dstColorBlendFactor;
		target->colour_op = attachment->colorBlendOp;
		target->alpha_source = attachment->srcAlphaBlendFactor;
		target->alpha_destination = attachment->dstAlphaBlendFactor;
		target->alpha_op = attachment->alphaBlendOp;
		target->write_mask = attachment->colorWriteMask & 0xfU;
	}
}

/*
 * Decodes the dynamic state: which of the states a draw takes come from
 * the command buffer; any other keeps the pipeline's value.
 */
static void
pipeline_decode_dynamic(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct amdrdna_vk_pipeline *pipeline)
{
	VkPipelineDynamicStateCreateInfo dynamic;
	uint32_t index;
	uint32_t state;

	/* The record; one that did not decode, or without a list, declares nothing. */
	kern_memset(&dynamic, 0, sizeof(dynamic));
	i915_vkc_dec_VkPipelineDynamicStateCreateInfo(reader, &session->arena, &dynamic);
	if (reader->error != 0 || dynamic.pDynamicStates == NULL)
		return;

	/* Each state a draw takes; any other is logged and keeps the pipeline's value. */
	for (index = 0U; index < dynamic.dynamicStateCount; index++) {
		state = (uint32_t)dynamic.pDynamicStates[index];
		if (state < 32U && ((1U << state) & PIPELINE_DYNAMIC_TAKEN) != 0U) {
			pipeline->dynamic |= 1U << state;
			continue;
		}
		kern_logf("amdrdna: vk: dynamic state %u is not implemented\n", state);
	}
}

/*
 * Compiles a pipeline's two modules and copies the programs into a run of
 * the executor's GPU memory: the vertex program first, the fragment
 * program on the next boundary.  The fragment program is compiled first,
 * so that the vertex program exports the primitive number it reads; the
 * vertex program is a primitive shader as the node asks, unless it exports
 * the primitive number.
 */
static int
pipeline_compile(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_pipeline *pipeline,
	struct pipeline_build *build)
{
	struct amdrdna_vk_pipeline *variant;
	uint32_t view;
	int error;

	/* The programs for view 0 (or without multiview). */
	error = pipeline_compile_view(session, pipeline, build, 0U);
	if (error != 0)
		return error;

	/* A pipeline of a multiview pass whose programs read gl_ViewIndex has the programs of each further view. */
	if (pipeline->vertex.compiled->reads_view == 0U && pipeline->fragment.compiled->reads_view == 0U)
		return 0;
	for (view = 1U; view < AMDRDNA_VK_VIEWS_MAX; view++) {
		if ((build->view_mask & (1U << view)) == 0U)
			continue;
		variant = kern_malloc(sizeof(*variant));
		if (variant == NULL)
			return ENOMEM;
		*variant = *pipeline;
		variant->code_token = NULL;
		variant->vertex.compiled = NULL;
		variant->fragment.compiled = NULL;
		kern_memset(variant->views, 0, sizeof(variant->views));
		pipeline->views[view] = variant;
		error = pipeline_compile_view(session, variant, build, view);
		if (error != 0)
			return error;
	}

	/* Succeeded: every view has its programs. */
	return 0;
}

/*
 * Compiles a graphics pipeline's programs for one view (its gl_ViewIndex)
 * and copies them into a run of the executor's GPU memory of their own.
 */
static int
pipeline_compile_view(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_pipeline *pipeline,
	struct pipeline_build *build,
	uint32_t view)
{
	struct amdrdna_gfx10_compile_options options;
	uint8_t *cpu;
	uint64_t va;
	uint32_t geometry;
	uint32_t vertex_bytes;
	uint32_t fragment_bytes;
	int error;

	/*
	 * The fragment program, then the vertex program with what the
	 * fragment program reads of it; both with arrays of descriptors (a
	 * draw writes the element each binding names), for the view.
	 */
	kern_memset(&options, 0, sizeof(options));
	options.descriptor_arrays = 1U;
	options.color_exports = build->colour_exports;
	options.view = view;
	error = pipeline_compile_stage(build->fragment_module, DRV_GPU_STAGE_FRAGMENT, &options, &pipeline->fragment);
	if (error != 0)
		return error;
	if (pipeline->fragment.compiled->primitive_id_attribute != AMDRDNA_GFX10_NO_ATTRIBUTE)
		options.pass_primitive_number = 1U;

	/* The vertex program runs as a primitive shader when the node says so and no vertex exports the primitive number. */
	geometry = amdrdna_vk_geometry_mode(session->owner);
	if (geometry != 0U && options.pass_primitive_number == 0U)
		options.primitive_shader = 1U;
	if (geometry == AMDRDNA_VK_GEOMETRY_CULLING && options.primitive_shader != 0U)
		options.primitive_culling = 1U;
	error = pipeline_compile_stage(build->vertex_module, DRV_GPU_STAGE_VERTEX, &options, &pipeline->vertex);
	if (error != 0)
		return error;

	/* Each binding the programs read lies in the sets and bindings a draw binds. */
	error = pipeline_bindings(pipeline->vertex.compiled);
	if (error == 0)
		error = pipeline_bindings(pipeline->fragment.compiled);
	if (error != 0)
		return error;

	/* The run holding both, each on the boundary (ENOSPC when the GPU's memory ran out). */
	vertex_bytes = pipeline_program_bytes(pipeline->vertex.compiled);
	fragment_bytes = pipeline_program_bytes(pipeline->fragment.compiled);
	error = amdrdna_vk_gpu_alloc(session->owner, (uint64_t)vertex_bytes + fragment_bytes, &pipeline->code_token, &cpu, &va);
	if (error != 0)
		return error;

	/* Copies the programs and keeps their addresses. */
	pipeline_program(&pipeline->vertex, cpu, va);
	pipeline_program(&pipeline->fragment, cpu + vertex_bytes, va + vertex_bytes);

	/* Succeeded: the pipeline's programs are in the GPU's memory. */
	return 0;
}

/*
 * Reports how a pipeline's fragment program exports its colours to the
 * render pass's colour attachments (AMDRDNA_GFX10_COLOUR_*, 4 bits a
 * location: location L goes to colour attachment L): by each attachment's
 * format, the first with its alpha when the alpha covers samples; four
 * 16-bit floats where there is no attachment.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_pipeline_graphics.c
 * (radv_pipeline_compute_spi_color_formats: each colour attachment's
 * format chosen by ac_choose_spi_color_formats, the alpha's variant for
 * MRT0 with alpha to coverage).
 */
static uint32_t
pipeline_colour_exports(
	const struct amdrdna_vk_render_pass *pass,
	bool alpha_to_coverage)
{
	uint32_t exports;
	uint32_t index;
	uint32_t kind;

	/* Each colour attachment's format. */
	exports = 0U;
	for (index = 0U; index < pass->colour_count && index < AMDRDNA_VK_COLOURS_MAX; index++) {
		if (pass->colour_attachments[index] == VK_ATTACHMENT_UNUSED)
			continue;
		kind = amdrdna_vk_target_export(pass->formats[index], alpha_to_coverage && index == 0U);
		exports |= kind << (AMDRDNA_GFX10_COLOUR_BITS * index);
	}

	/* Succeeded: the exports. */
	return exports;
}

/*
 * Compiles a compute pipeline's module for GFX10 and copies the program
 * into a run of the executor's GPU memory of its own.
 */
static int
pipeline_compile_kernel(
	struct amdrdna_vk_session *session,
	struct amdrdna_vk_pipeline *pipeline,
	struct pipeline_build *build)
{
	struct amdrdna_gfx10_compile_options options;
	uint8_t *cpu;
	uint64_t va;
	int error;

	/* The program, with arrays of descriptors, whose bindings a dispatch can bind (and for vkCmdDispatchBase). */
	kern_memset(&options, 0, sizeof(options));
	options.descriptor_arrays = 1U;
	options.dispatch_base = build->dispatch_base;
	error = pipeline_compile_stage(build->kernel_module, DRV_GPU_STAGE_COMPUTE, &options, &pipeline->kernel);
	if (error != 0)
		return error;
	error = pipeline_bindings(pipeline->kernel.compiled);
	if (error != 0)
		return error;

	/* Its run (ENOSPC when the GPU's memory ran out), then the code copied and its address kept. */
	error = amdrdna_vk_gpu_alloc(session->owner, pipeline_program_bytes(pipeline->kernel.compiled), &pipeline->code_token, &cpu, &va);
	if (error != 0)
		return error;
	pipeline_program(&pipeline->kernel, cpu, va);

	/* Succeeded: the kernel is in the GPU's memory. */
	return 0;
}

/*
 * Compiles one module for a stage into a program, with its register
 * values; the reason of a refusal is shown.
 */
static int
pipeline_compile_stage(
	const struct amdrdna_vk_shader *module,
	enum drv_gpu_shader_stage stage,
	const struct amdrdna_gfx10_compile_options *options,
	struct amdrdna_vk_program *program)
{
	struct amdrdna_gfx10_diagnostic diagnostic;
	int error;

	/* Compiled for GFX10. */
	kern_memset(&diagnostic, 0, sizeof(diagnostic));
	error = amdrdna_gfx10_compile(module->words, module->word_count, stage, options, &program->compiled, &diagnostic);
	if (error != 0) {
		kern_logf("amdrdna: vk: a stage %u module of %u words did not compile (%d at %u: %s)\n",
			  (unsigned)stage,
			  (unsigned)module->word_count,
			  error,
			  (unsigned)diagnostic.instruction,
			  (diagnostic.reason != NULL) ? diagnostic.reason : "?");
		program->compiled = NULL;
		return error;
	}

	/* The register values a draw writes for it. */
	amdrdna_gfx10_program_registers(program->compiled, &program->registers);

	/* Succeeded: the program is compiled. */
	return 0;
}

/*
 * Checks the bindings a program reads: each buffer, sampled image and
 * storage image in a set and at a binding a descriptor set has (or a kernel's workgroup
 * counts, which the dispatch places), and no more sampled images than a
 * draw writes.  Returns 0 or ENOTSUP.
 */
static int
pipeline_bindings(
	const struct amdrdna_gfx10_program *program)
{
	uint32_t index;

	/* The storage buffers (a kernel's workgroup counts are the dispatch's), the uniform buffers, the sampled images. */
	for (index = 0U; index < program->storage_used; index++) {
		if (program->stage == DRV_GPU_STAGE_COMPUTE && program->storage[index].set == DRV_GPU_IR_SYSTEM_SET)
			continue;
		if (program->storage[index].set >= AMDRDNA_VK_SETS_MAX || program->storage[index].binding >= AMDRDNA_VK_BINDINGS_MAX)
			return ENOTSUP;
	}
	for (index = 0U; index < program->uniform_used; index++) {
		if (program->uniforms[index].set >= AMDRDNA_VK_SETS_MAX || program->uniforms[index].binding >= AMDRDNA_VK_BINDINGS_MAX)
			return ENOTSUP;
	}
	if (program->texture_used > AMDRDNA_VK_TEXTURES_MAX)
		return ENOTSUP;
	for (index = 0U; index < program->texture_used; index++) {
		if (program->textures[index].set >= AMDRDNA_VK_SETS_MAX || program->textures[index].binding >= AMDRDNA_VK_BINDINGS_MAX)
			return ENOTSUP;
	}

	/* The storage images and storage texel buffers. */
	if (program->image_used > AMDRDNA_VK_STORAGE_IMAGES_MAX)
		return ENOTSUP;
	for (index = 0U; index < program->image_used; index++) {
		if (program->images[index].set >= AMDRDNA_VK_SETS_MAX || program->images[index].binding >= AMDRDNA_VK_BINDINGS_MAX)
			return ENOTSUP;
	}

	/* The push constants a draw holds. */
	if (program->push_bytes > AMDRDNA_VK_PUSH_BYTES)
		return ENOTSUP;

	/* Succeeded: a draw can write the program's table. */
	return 0;
}

/* Copies a compiled program's code to its place and keeps its address. */
static void
pipeline_program(
	struct amdrdna_vk_program *program,
	uint8_t *cpu,
	uint64_t va)
{
	/* The code, with its padding. */
	kern_memcpy(cpu, program->compiled->code, (size_t)program->compiled->code_words * 4U);
	program->va = va;
}

/* Reports the bytes a program takes in the run: its code, on the boundary. */
static uint32_t
pipeline_program_bytes(
	const struct amdrdna_gfx10_program *program)
{
	uint32_t bytes;

	/* The code, rounded up. */
	bytes = program->code_words * 4U;
	bytes = (bytes + PIPELINE_PROGRAM_ALIGN - 1U) & ~(PIPELINE_PROGRAM_ALIGN - 1U);

	/* Succeeded: the program's share of the run. */
	return bytes;
}

/* Destroys a pipeline nothing holds: its programs' run goes back to the node. */
static int
pipeline_destroy(
	struct amdrdna_vk_session *session,
	void *payload)
{
	struct amdrdna_vk_pipeline *pipeline;
	uint32_t view;

	pipeline = payload;

	/* The programs of the further views of a multiview pass. */
	for (view = 0U; view < AMDRDNA_VK_VIEWS_MAX; view++) {
		if (pipeline->views[view] != NULL)
			(void)pipeline_destroy(session, pipeline->views[view]);
	}

	/* The run, when the programs were copied, and the compiled programs. */
	if (pipeline->code_token != NULL)
		amdrdna_vk_gpu_free(session->owner, pipeline->code_token);
	if (pipeline->vertex.compiled != NULL)
		amdrdna_gfx10_program_free(pipeline->vertex.compiled);
	if (pipeline->fragment.compiled != NULL)
		amdrdna_gfx10_program_free(pipeline->fragment.compiled);
	if (pipeline->kernel.compiled != NULL)
		amdrdna_gfx10_program_free(pipeline->kernel.compiled);

	kern_free(pipeline);

	/* Succeeded: the pipeline is gone. */
	return 0;
}

/* Copies a float as its 32 bits (no floating point in the kernel). */
static void
pipeline_float(
	uint32_t *destination,
	const float *source)
{
	/* The bits, unconverted. */
	kern_memcpy(destination, source, sizeof(*destination));
}

/* Keeps one face's stencil test of a pipeline's depth and stencil state. */
static void
pipeline_stencil_face(
	struct amdrdna_vk_stencil_face *face,
	const VkStencilOpState *state)
{
	/* The operations, the compare, the masks and the reference. */
	face->fail = (uint32_t)state->failOp;
	face->pass = (uint32_t)state->passOp;
	face->depth_fail = (uint32_t)state->depthFailOp;
	face->compare = (uint32_t)state->compareOp;
	face->compare_mask = state->compareMask;
	face->write_mask = state->writeMask;
	face->reference = state->reference;
}
