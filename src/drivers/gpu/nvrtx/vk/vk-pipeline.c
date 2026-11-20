/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's pipelines.
 *
 * A compute pipeline's module is compiled for the owner's shader model when
 * the pipeline is created and its code copied into a run of the executor's
 * GPU memory on a QMD's program boundary; the pipeline keeps what a launch
 * says of it (vk-execute.c): the registers, the workgroup, the shared
 * memory, the barriers, and where each storage and uniform buffer comes
 * from.
 *
 * A pipeline's vertex and fragment modules are compiled for SM75 by the
 * nvrtx compiler (compiler/sm75/) when the pipeline is created,
 * and their programs (each its header, then its code) are copied into a
 * run of the executor's GPU memory the pipeline keeps.  The pipeline also
 * keeps the state a draw sends: the vertex input, the topology, the
 * culling, the depth bias, the depth and stencil tests, the colour
 * attachment's blend and its constants, and the viewport and scissor when
 * they are not dynamic.  The depth bias's factors, the blend constants and
 * the stencil masks and references may come from the command buffer
 * instead (the dynamic state GL's pipelines declare); a line width is
 * dynamic or not alike, as every line is one pixel wide.
 *
 * The programs read push constants from constant buffer 0 at 0 and the
 * fragment program's texture handles from it at NVRTX_SM75_CB_TEXTURES,
 * in the order the compiler lists its sampled images; the vertex program
 * samples nothing, and neither uses storage or uniform buffers (the
 * compositor's shaders do not).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/nvrtx/vk/nvrtx-vk-objects.h"
#include "drivers/gpu/compiler/spirv.h"
#include "drivers/gpu/nvrtx/compiler/sm75/sm75.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The sample mask that keeps every sample (the hardware's 16 bits a pixel of the 2x2 quad). */
#define PIPELINE_SAMPLE_MASK_ALL	0xffffU

/* The most pipelines one create makes, and the most stages one pipeline lists. */
#define PIPELINE_CREATED_MAX		8U
#define PIPELINE_STAGES_MAX		4U

/* A program starts on this boundary of the pipeline's run (a QMD's program boundary too). */
#define PIPELINE_PROGRAM_ALIGN		256U

/* The invocations of a compute program's workgroup at most (the limit the device reports). */
#define PIPELINE_INVOCATIONS_MAX	128U

/* The shared memory of a compute program's workgroup at most (the limit the device reports). */
#define PIPELINE_SHARED_MAX		16384U

/* A program's header in front of its code. */
#define PIPELINE_HEADER_BYTES		(NVRTX_SM75_SPH_WORDS * 4U)

/* The locations a vertex attribute may have (the generic attributes). */
#define PIPELINE_LOCATIONS		16U

/*
 * The modules of a pipeline being made (the vertex, fragment and maybe
 * geometry ones of a graphics pipeline, the kernel one of a compute
 * pipeline), and their compiled programs until they are copied into the
 * pipeline's run.
 */
struct pipeline_build {
	const struct nvrtx_vk_shader *vertex_module;
	const struct nvrtx_vk_shader *fragment_module;
	const struct nvrtx_vk_shader *geometry_module;
	const struct nvrtx_vk_shader *kernel_module;
	struct nvrtx_sm75_binary *vertex;
	struct nvrtx_sm75_binary *fragment;
	struct nvrtx_sm75_binary *geometry;
	struct nvrtx_sm75_binary *kernel;
};

static int pipeline_create(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply, bool compute);
static int pipeline_decode_kernel(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct nvrtx_vk_pipeline *pipeline, struct pipeline_build *build);
static int pipeline_decode(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct nvrtx_vk_pipeline *pipeline, struct pipeline_build *build);
static int pipeline_refuse(const char *what);
static int pipeline_decode_stages(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct pipeline_build *build);
static int pipeline_decode_vertex_input(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct nvrtx_vk_pipeline *pipeline);
static void pipeline_decode_viewport(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct nvrtx_vk_pipeline *pipeline);
static void pipeline_decode_blend(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct nvrtx_vk_pipeline *pipeline);
static void pipeline_decode_dynamic(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct nvrtx_vk_pipeline *pipeline);
static void pipeline_stencil_face(const VkStencilOpState *state, struct nvrtx_exec_stencil_face *face);
static int pipeline_compile(struct nvrtx_vk_session *session, struct nvrtx_vk_pipeline *pipeline, struct pipeline_build *build);
static int pipeline_compile_kernel(struct nvrtx_vk_session *session, struct nvrtx_vk_pipeline *pipeline, struct pipeline_build *build);
static int pipeline_compile_stage(const struct nvrtx_vk_shader *module, enum drv_gpu_shader_stage stage, uint32_t shader_model, uint32_t features, uint32_t view, uint32_t *reads_view, struct nvrtx_sm75_binary **binary);
static int pipeline_compile_view(struct nvrtx_vk_session *session, struct nvrtx_vk_pipeline *pipeline, struct pipeline_build *build, uint32_t view, uint32_t *reads_view);
static uint32_t pipeline_first_view(uint32_t view_mask);
static int pipeline_kernel_sources(struct nvrtx_vk_kernel *kernel, const struct nvrtx_sm75_binary *binary);
static int pipeline_program(struct nvrtx_vk_program *program, const struct nvrtx_sm75_binary *binary, uint8_t *cpu, uint64_t va);
static uint32_t pipeline_program_bytes(const struct nvrtx_sm75_binary *binary);
static void pipeline_build_free(struct pipeline_build *build);
static int pipeline_destroy(struct nvrtx_vk_session *session, void *payload);
static void pipeline_float(uint32_t *destination, const float *source);
static uint32_t pipeline_topology_vertices(uint32_t topology);
static uint32_t pipeline_writes_point_size(const struct nvrtx_sm75_binary *binary);
static uint32_t pipeline_writes_attribute(const struct nvrtx_sm75_binary *binary, uint32_t attribute);

/*
 * Runs a pipeline command; *handled stays clear for an opcode of another
 * handler's.
 */
int
nvrtx_vk_pipeline_route(
	struct nvrtx_vk_session *session,
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
		error = nvrtx_vk_destroy_plain(session, reader, I915_VK_OBJ_PIPELINE);
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
 * vkCreateGraphicsPipelines and vkCreateComputePipelines (compute set):
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
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	bool compute)
{
	struct nvrtx_vk_pipeline *pipelines[PIPELINE_CREATED_MAX];
	struct pipeline_build builds[PIPELINE_CREATED_MAX];
	uint64_t identities[PIPELINE_CREATED_MAX];
	struct nvrtx_vk_object *object;
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
		if (compute)
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
	for (index = 0U; error == 0 && index < count; index++)
		error = pipeline_compile(session, pipelines[index], &builds[index]);

	/* Publishes each one, up to the first failure. */
	published = 0U;
	for (index = 0U; error == 0 && index < count; index++) {
		error = nvrtx_vk_object_publish(session, I915_VK_OBJ_PIPELINE, identities[index], pipelines[index], pipeline_destroy, &object);
		if (error != 0)
			break;
		pipelines[index] = NULL;
		published++;
	}

	/* A create that failed leaves no pipeline. */
	if (error != 0) {
		kern_logf("nvrtx: vk: vkCreate%sPipelines failed: %d\n", compute ? "Compute" : "Graphics", error);
		for (index = 0U; index < published; index++)
			(void)nvrtx_vk_object_remove(session, I915_VK_OBJ_PIPELINE, identities[index]);
	}

	/* What was not published goes. */
	for (index = 0U; index < count; index++) {
		pipeline_build_free(&builds[index]);
		if (pipelines[index] != NULL)
			(void)pipeline_destroy(session, pipelines[index]);
	}

	/* The result and the count, then each identity, or null for each when the create failed. */
	result = nvrtx_vk_result(error);
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
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_pipeline *pipeline,
	struct pipeline_build *build)
{
	VkComputePipelineCreateInfo info;

	/* The record. */
	kern_memset(&info, 0, sizeof(info));
	i915_vkc_dec_VkComputePipelineCreateInfo(reader, &session->arena, &info);
	if (reader->error != 0)
		return EINVAL;

	/* A compute pipeline (the push constants a program reads are its own business). */
	pipeline->compute = true;

	/* One made for vkCmdDispatchBase numbers its groups from the dispatch's base. */
	if ((info.flags & VK_PIPELINE_CREATE_DISPATCH_BASE_BIT) != 0U)
		pipeline->dispatch_base = 1U;

	/* The compute stage of a known module, without specialization constants. */
	if (info.stage.stage != VK_SHADER_STAGE_COMPUTE_BIT)
		return ENOTSUP;
	if (info.stage.pSpecializationInfo != NULL && info.stage.pSpecializationInfo->mapEntryCount != 0U)
		return ENOTSUP;
	build->kernel_module = nvrtx_vk_payload(session, I915_VK_OBJ_SHADER_MODULE, (uint64_t)(uintptr_t)info.stage.module);
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
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_pipeline *pipeline,
	struct pipeline_build *build)
{
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineTessellationStateCreateInfo tessellation;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineDepthStencilStateCreateInfo depth;
	const struct nvrtx_vk_pipeline_layout *layout;
	const struct nvrtx_vk_render_pass *pass;
	uint64_t present;
	uint64_t layout_id;
	uint64_t pass_id;
	uint32_t index;
	uint32_t full;
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
		if (error == ENOTSUP && refused == 0)
			refused = pipeline_refuse("vertex input");
		else if (error != 0 && refused == 0)
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

	/*
	 * The rasterization: the culling, whether primitives are discarded,
	 * the polygon mode (filled, lines or points; none other), whether the
	 * depth is clamped, the depth bias (its factors as float bits).
	 */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		kern_memset(&raster, 0, sizeof(raster));
		i915_vkc_dec_VkPipelineRasterizationStateCreateInfo(reader, &session->arena, &raster);
		pipeline->cull_mode = raster.cullMode;
		pipeline->front_face = raster.frontFace;
		pipeline->depth_bias_enable = 0U;
		if (raster.depthBiasEnable != VK_FALSE)
			pipeline->depth_bias_enable = 1U;
		pipeline_float(&pipeline->depth_bias[0], &raster.depthBiasConstantFactor);
		pipeline_float(&pipeline->depth_bias[1], &raster.depthBiasClamp);
		pipeline_float(&pipeline->depth_bias[2], &raster.depthBiasSlopeFactor);
		pipeline->raster_discard = 0U;
		if (raster.rasterizerDiscardEnable != VK_FALSE)
			pipeline->raster_discard = 1U;
		pipeline->polygon_mode = raster.polygonMode;
		if (raster.polygonMode > VK_POLYGON_MODE_POINT && refused == 0)
			refused = pipeline_refuse("polygon mode");
		pipeline->depth_clamp = 0U;
		if (raster.depthClampEnable != VK_FALSE)
			pipeline->depth_clamp = 1U;
	}

	/*
	 * The multisample state: one or four samples, the samples the mask
	 * keeps (every one without a mask; no sample shading, no alpha to
	 * one), alpha to coverage or not.
	 */
	pipeline->samples = VK_SAMPLE_COUNT_1_BIT;
	pipeline->sample_mask = PIPELINE_SAMPLE_MASK_ALL;
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		kern_memset(&multisample, 0, sizeof(multisample));
		i915_vkc_dec_VkPipelineMultisampleStateCreateInfo(reader, &session->arena, &multisample);
		pipeline->samples = multisample.rasterizationSamples;
		pipeline->alpha_coverage = 0U;
		if (multisample.alphaToCoverageEnable != VK_FALSE)
			pipeline->alpha_coverage = 1U;
		if (multisample.rasterizationSamples != VK_SAMPLE_COUNT_1_BIT && multisample.rasterizationSamples != VK_SAMPLE_COUNT_4_BIT &&
		    refused == 0)
			refused = pipeline_refuse("sample count");
		if ((multisample.sampleShadingEnable != VK_FALSE || multisample.alphaToOneEnable != VK_FALSE) && refused == 0)
			refused = pipeline_refuse("sample shading or alpha to one");
		full = 0x1U;
		if (pipeline->samples == VK_SAMPLE_COUNT_4_BIT)
			full = 0xfU;
		if (multisample.pSampleMask != NULL)
			pipeline->sample_mask = multisample.pSampleMask[0] & full;
	}

	/* The depth test and the stencil test of both faces (no depth bounds). */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		kern_memset(&depth, 0, sizeof(depth));
		i915_vkc_dec_VkPipelineDepthStencilStateCreateInfo(reader, &session->arena, &depth);
		pipeline->depth_test = 0U;
		if (depth.depthTestEnable != VK_FALSE)
			pipeline->depth_test = 1U;
		pipeline->depth_write = 0U;
		if (depth.depthWriteEnable != VK_FALSE)
			pipeline->depth_write = 1U;
		pipeline->depth_compare = depth.depthCompareOp;
		pipeline->stencil_test = 0U;
		if (depth.stencilTestEnable != VK_FALSE)
			pipeline->stencil_test = 1U;
		pipeline_stencil_face(&depth.front, &pipeline->stencil_front);
		pipeline_stencil_face(&depth.back, &pipeline->stencil_back);
		if (depth.depthBoundsTestEnable != VK_FALSE && refused == 0)
			refused = pipeline_refuse("depth bounds test");
	}

	/* The colour blend of each attachment (four at most); without it every channel is written unblended. */
	for (index = 0U; index < NVRTX_EXEC_COLOURS; index++)
		pipeline->blend[index].write_mask = 0xfU;
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		pipeline_decode_blend(session, reader, pipeline);
	if (pipeline->colour_count > NVRTX_EXEC_COLOURS && refused == 0)
		refused = pipeline_refuse("colour attachment count");

	/* The dynamic state. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		pipeline_decode_dynamic(session, reader, pipeline);

	/*
	 * The layout (its push constant bytes), the views of the render pass's
	 * subpass (the programs are compiled for each view that reads
	 * gl_ViewIndex); the subpass and the base pipeline are not used.
	 */
	layout_id = drv_i915_wire_read_u64(reader);
	pass_id = drv_i915_wire_read_u64(reader);
	pass = nvrtx_vk_payload(session, I915_VK_OBJ_RENDER_PASS, pass_id);
	if (pass != NULL)
		pipeline->view_mask = pass->view_mask;
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;
	layout = nvrtx_vk_payload(session, I915_VK_OBJ_PIPELINE_LAYOUT, layout_id);
	if (layout != NULL)
		pipeline->push_bytes = layout->push_bytes;

	/* Points, lines and triangles, with or without adjacency; no patches. */
	if (pipeline_topology_vertices(pipeline->topology) == 0U && refused == 0)
		refused = pipeline_refuse("topology");

	/* Reports state the executor does not draw. */
	if (refused != 0)
		return refused;

	/* Succeeded: the pipeline holds its state; its modules are in the build. */
	return 0;
}

/* Logs which part of a graphics pipeline's state the executor does not draw; returns ENOTSUP. */
static int
pipeline_refuse(
	const char *what)
{
	/* The reason, for the create's failure line that follows. */
	kern_logf("nvrtx: vk: a graphics pipeline's %s is not drawn\n", what);

	/* The refusal. */
	return ENOTSUP;
}

/*
 * Decodes the head and the stages: [sType][pNext][flags][stageCount]
 * [count]{VkPipelineShaderStageCreateInfo}.  A vertex and a fragment stage
 * and maybe a geometry stage, of known modules, without specialization,
 * are taken; anything else is ENOTSUP after the stages are read.
 */
static int
pipeline_decode_stages(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct pipeline_build *build)
{
	VkPipelineShaderStageCreateInfo stage;
	const struct nvrtx_vk_shader *module;
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
			refused = pipeline_refuse("specialization");
			continue;
		}

		/* The vertex and the fragment stage keep their modules. */
		module = nvrtx_vk_payload(session, I915_VK_OBJ_SHADER_MODULE, (uint64_t)(uintptr_t)stage.module);
		if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT) {
			build->vertex_module = module;
		} else if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
			build->fragment_module = module;
		} else if (stage.stage == VK_SHADER_STAGE_GEOMETRY_BIT && module != NULL) {
			build->geometry_module = module;
		} else {
			kern_logf("nvrtx: vk: pipeline stage 0x%x is not run\n", (unsigned)stage.stage);
			refused = ENOTSUP;
		}
	}

	/* Both programs, of known modules. */
	if (build->vertex_module == NULL || build->fragment_module == NULL) {
		if (refused == 0)
			refused = pipeline_refuse("vertex or fragment module");
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
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_pipeline *pipeline)
{
	VkPipelineVertexInputStateCreateInfo input;
	const VkVertexInputAttributeDescription *attribute;
	uint32_t index;

	/* The record. */
	kern_memset(&input, 0, sizeof(input));
	i915_vkc_dec_VkPipelineVertexInputStateCreateInfo(reader, &session->arena, &input);
	if (reader->error != 0)
		return EINVAL;

	/* No more bindings and attributes than a draw sends. */
	if (input.vertexBindingDescriptionCount > NVRTX_VK_VERTEX_BINDINGS_MAX)
		return ENOTSUP;
	if (input.vertexAttributeDescriptionCount > NVRTX_VK_VERTEX_ATTRIBUTES_MAX)
		return ENOTSUP;

	/* Each binding's number, stride and rate. */
	pipeline->binding_count = input.vertexBindingDescriptionCount;
	for (index = 0U; index < pipeline->binding_count; index++) {
		if (input.pVertexBindingDescriptions[index].binding >= NVRTX_VK_VERTEX_BINDINGS_MAX)
			return ENOTSUP;
		pipeline->bindings[index].binding = input.pVertexBindingDescriptions[index].binding;
		pipeline->bindings[index].stride = input.pVertexBindingDescriptions[index].stride;
		pipeline->bindings[index].instance = 0U;
		if (input.pVertexBindingDescriptions[index].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE)
			pipeline->bindings[index].instance = 1U;
	}

	/* Each attribute: a generic location, a bound buffer, a format the attributes fetch. */
	pipeline->attribute_count = input.vertexAttributeDescriptionCount;
	for (index = 0U; index < pipeline->attribute_count; index++) {
		attribute = &input.pVertexAttributeDescriptions[index];
		if (attribute->location >= PIPELINE_LOCATIONS)
			return ENOTSUP;
		if (attribute->binding >= NVRTX_VK_VERTEX_BINDINGS_MAX)
			return ENOTSUP;
		if (!nvrtx_exec_vertex_format(attribute->format))
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
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_pipeline *pipeline)
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

/* Decodes the colour blend state: the blend constants, and the blend and write mask of attachment 0. */
static void
pipeline_decode_blend(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_pipeline *pipeline)
{
	VkPipelineColorBlendStateCreateInfo blend;
	const VkPipelineColorBlendAttachmentState *attachment;
	struct nvrtx_exec_blend *target;
	uint32_t index;

	/* The record; one that did not decode, or without attachments, blends nothing. */
	kern_memset(&blend, 0, sizeof(blend));
	i915_vkc_dec_VkPipelineColorBlendStateCreateInfo(reader, &session->arena, &blend);
	if (reader->error != 0)
		return;

	/* The logic operation, on or off. */
	pipeline->logic_op_enable = 0U;
	if (blend.logicOpEnable != VK_FALSE)
		pipeline->logic_op_enable = 1U;
	pipeline->logic_op = blend.logicOp;

	/* The constants, as float bits. */
	pipeline_float(&pipeline->blend_constants[0], &blend.blendConstants[0]);
	pipeline_float(&pipeline->blend_constants[1], &blend.blendConstants[1]);
	pipeline_float(&pipeline->blend_constants[2], &blend.blendConstants[2]);
	pipeline_float(&pipeline->blend_constants[3], &blend.blendConstants[3]);
	if (blend.attachmentCount == 0U || blend.pAttachments == NULL)
		return;

	/* Each attachment's blend, factors, operations and write mask (past the fourth the pipeline is refused). */
	pipeline->colour_count = blend.attachmentCount;
	for (index = 0U; index < blend.attachmentCount && index < NVRTX_EXEC_COLOURS; index++) {
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

/* Keeps a face's stencil test: its operations, function, masks and reference (the masks and reference 8 bits). */
static void
pipeline_stencil_face(
	const VkStencilOpState *state,
	struct nvrtx_exec_stencil_face *face)
{
	/* The operations and the function, as Vulkan numbers them. */
	face->fail = state->failOp;
	face->pass = state->passOp;
	face->depth_fail = state->depthFailOp;
	face->compare = state->compareOp;

	/* The masks and the reference, of the 8-bit stencil. */
	face->compare_mask = state->compareMask & 0xffU;
	face->write_mask = state->writeMask & 0xffU;
	face->reference = state->reference & 0xffU;
}

/*
 * Decodes the dynamic state: whether the viewport and the scissor come
 * from the command buffer, and which of the depth bias's factors, the
 * blend constants and the stencil masks and references do.
 */
static void
pipeline_decode_dynamic(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct nvrtx_vk_pipeline *pipeline)
{
	VkPipelineDynamicStateCreateInfo dynamic;
	uint32_t index;
	uint32_t state;

	/* The record; one that did not decode, or without a list, declares nothing. */
	kern_memset(&dynamic, 0, sizeof(dynamic));
	i915_vkc_dec_VkPipelineDynamicStateCreateInfo(reader, &session->arena, &dynamic);
	if (reader->error != 0 || dynamic.pDynamicStates == NULL)
		return;

	/* Each state the list names; a line width needs nothing (every line is one pixel wide), any other keeps the pipeline's value. */
	for (index = 0U; index < dynamic.dynamicStateCount; index++) {
		state = (uint32_t)dynamic.pDynamicStates[index];

		/* What the pipeline takes from the command buffer for it. */
		switch (state) {
		case VK_DYNAMIC_STATE_VIEWPORT:
			pipeline->dynamic_viewport = 1U;
			break;
		case VK_DYNAMIC_STATE_SCISSOR:
			pipeline->dynamic_scissor = 1U;
			break;
		case VK_DYNAMIC_STATE_LINE_WIDTH:
			break;
		case VK_DYNAMIC_STATE_DEPTH_BIAS:
			pipeline->dynamic |= NVRTX_VK_DYNAMIC_DEPTH_BIAS;
			break;
		case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
			pipeline->dynamic |= NVRTX_VK_DYNAMIC_BLEND_CONSTANTS;
			break;
		case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
			pipeline->dynamic |= NVRTX_VK_DYNAMIC_STENCIL_COMPARE;
			break;
		case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
			pipeline->dynamic |= NVRTX_VK_DYNAMIC_STENCIL_WRITE;
			break;
		case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
			pipeline->dynamic |= NVRTX_VK_DYNAMIC_STENCIL_REFERENCE;
			break;
		default:
			kern_logf("nvrtx: vk: dynamic state %u is not implemented\n", state);
			break;
		}
	}
}

/*
 * Compiles a pipeline's two modules (three with a geometry module) and
 * copies the programs into a run of the executor's GPU memory: the vertex
 * program first, the fragment program on the next boundary, the geometry
 * program on the one after.  A geometry program's input primitive is the
 * one the pipeline's topology gives (its vertices).  The point size comes
 * from the last program before the rasterizer when that writes it.
 */
static int
pipeline_compile(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_pipeline *pipeline,
	struct pipeline_build *build)
{
	struct nvrtx_vk_pipeline *variant;
	struct pipeline_build variant_build;
	uint32_t first;
	uint32_t view;
	uint32_t reads_view;
	uint32_t ignored;
	int error;

	/* A compute pipeline has its one program. */
	if (pipeline->compute) {
		error = pipeline_compile_kernel(session, pipeline, build);
		return error;
	}

	/* The pipeline's own programs render the subpass's first view (view 0 without multiview). */
	first = pipeline_first_view(pipeline->view_mask);
	error = pipeline_compile_view(session, pipeline, build, first, &reads_view);
	if (error != 0)
		return error;

	/* Programs that do not read gl_ViewIndex render every view. */
	if (reads_view == 0U)
		return 0;

	/* Each other view of the subpass gets a pipeline of the same state with programs for that view. */
	for (view = first + 1U; view < NVRTX_VK_VIEWS_MAX; view++) {
		/* A view the subpass does not render needs no programs. */
		if ((pipeline->view_mask & (1U << view)) == 0U)
			continue;

		/* The copy owns no run and no copies yet. */
		variant = kern_calloc(1U, sizeof(*variant));
		if (variant == NULL)
			return ENOMEM;
		*variant = *pipeline;
		variant->code_token = NULL;
		kern_memset(variant->views, 0, sizeof(variant->views));

		/* Its programs for the view, from the same modules. */
		kern_memset(&variant_build, 0, sizeof(variant_build));
		variant_build.vertex_module = build->vertex_module;
		variant_build.fragment_module = build->fragment_module;
		variant_build.geometry_module = build->geometry_module;
		error = pipeline_compile_view(session, variant, &variant_build, view, &ignored);
		pipeline_build_free(&variant_build);
		if (error != 0) {
			(void)pipeline_destroy(session, variant);
			return error;
		}
		pipeline->views[view] = variant;
	}

	/* Succeeded: every view of the subpass has its programs. */
	return 0;
}

/*
 * Compiles a graphics pipeline's programs for one view of its subpass (the
 * value they read as gl_ViewIndex) into a run of the executor's GPU memory,
 * and says whether any of them reads it.
 */
static int
pipeline_compile_view(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_pipeline *pipeline,
	struct pipeline_build *build,
	uint32_t view,
	uint32_t *reads_view)
{
	uint8_t *cpu;
	uint64_t va;
	uint32_t vertex_bytes;
	uint32_t fragment_bytes;
	uint32_t geometry_bytes;
	uint32_t shader_model;
	uint32_t stage_reads;
	int error;

	/* Compiles the two programs for the owner's shader model. */
	*reads_view = 0U;
	shader_model = nvrtx_vk_shader_model(session->owner);
	error = pipeline_compile_stage(build->vertex_module, DRV_GPU_STAGE_VERTEX, shader_model, DRV_GPU_SPIRV_FEATURE_VIEW_INDEX, view, &stage_reads, &build->vertex);
	if (error != 0)
		return error;
	*reads_view |= stage_reads;

	error = pipeline_compile_stage(build->fragment_module, DRV_GPU_STAGE_FRAGMENT, shader_model, DRV_GPU_SPIRV_FEATURE_VIEW_INDEX, view, &stage_reads, &build->fragment);
	if (error != 0)
		return error;
	*reads_view |= stage_reads;

	/* A geometry program, of the primitives the topology gives. */
	geometry_bytes = 0U;
	pipeline->point_size = pipeline_writes_point_size(build->vertex);
	pipeline->layer = pipeline_writes_attribute(build->vertex, NVRTX_SM75_ATTR_LAYER);
	if (build->geometry_module != NULL) {
		error = pipeline_compile_stage(build->geometry_module, DRV_GPU_STAGE_GEOMETRY, shader_model, DRV_GPU_SPIRV_FEATURE_VIEW_INDEX, view, &stage_reads, &build->geometry);
		if (error != 0)
			return error;
		*reads_view |= stage_reads;
		if (build->geometry->vertices_in != pipeline_topology_vertices(pipeline->topology)) {
			kern_logf("nvrtx: vk: a geometry program of %u input vertices (the topology %u gives %u)\n",
				  build->geometry->vertices_in, pipeline->topology, pipeline_topology_vertices(pipeline->topology));
			return EINVAL;
		}
		geometry_bytes = pipeline_program_bytes(build->geometry);
		pipeline->point_size = pipeline_writes_point_size(build->geometry);
		pipeline->layer = pipeline_writes_attribute(build->geometry, NVRTX_SM75_ATTR_LAYER);
	}

	/* Each samples, reads storage buffers and reads uniform buffers as its constant buffer 0 and its slots hold (pipeline_program() checks). */

	/* The run holding them, each on the boundary. */
	vertex_bytes = pipeline_program_bytes(build->vertex);
	fragment_bytes = pipeline_program_bytes(build->fragment);
	error = nvrtx_vk_gpu_alloc(session->owner, (uint64_t)vertex_bytes + fragment_bytes + geometry_bytes, &pipeline->code_token, &cpu, &va);
	if (error != 0)
		return error;

	/* Copies the programs and keeps what a draw needs of them. */
	error = pipeline_program(&pipeline->vertex, build->vertex, cpu, va);
	if (error == 0)
		error = pipeline_program(&pipeline->fragment, build->fragment, cpu + vertex_bytes, va + vertex_bytes);
	if (error == 0 && build->geometry != NULL)
		error = pipeline_program(&pipeline->geometry, build->geometry, cpu + vertex_bytes + fragment_bytes,
					 va + vertex_bytes + fragment_bytes);
	if (error != 0)
		return error;

	/* Succeeded: the pipeline's programs are in the GPU's memory. */
	return 0;
}

/*
 * Compiles a compute pipeline's program for the owner's shader model and
 * copies its code into a run of the executor's GPU memory: a workgroup of
 * the size and shared memory the device reports at most, storage and
 * uniform buffers the executor binds, no sampled image.
 */
static int
pipeline_compile_kernel(
	struct nvrtx_vk_session *session,
	struct nvrtx_vk_pipeline *pipeline,
	struct pipeline_build *build)
{
	struct nvrtx_sm75_binary *binary;
	struct nvrtx_vk_kernel *kernel;
	uint32_t shader_model;
	uint32_t features;
	uint32_t reads_view;
	uint32_t invocations;
	uint32_t bytes;
	uint8_t *cpu;
	uint64_t va;
	int error;

	/* The program; one made for vkCmdDispatchBase counts its groups from the base. */
	features = 0U;
	if (pipeline->dispatch_base != 0U)
		features = DRV_GPU_SPIRV_FEATURE_DISPATCH_BASE;
	shader_model = nvrtx_vk_shader_model(session->owner);
	error = pipeline_compile_stage(build->kernel_module, DRV_GPU_STAGE_COMPUTE, shader_model, features, 0U, &reads_view, &build->kernel);
	if (error != 0)
		return error;
	binary = build->kernel;

	/* A workgroup the device reports it runs, of the buffers the executor binds. */
	invocations = binary->local_size[0] * binary->local_size[1] * binary->local_size[2];
	if (invocations == 0U || invocations > PIPELINE_INVOCATIONS_MAX || binary->shared_bytes > PIPELINE_SHARED_MAX)
		return ENOTSUP;
	if (binary->texture_count != 0U)
		return ENOTSUP;
	kernel = &pipeline->kernel;
	error = pipeline_kernel_sources(kernel, binary);
	if (error != 0)
		return error;

	/* The code in a run of its own, from its start (a QMD's program boundary). */
	bytes = (binary->code_words * 4U + PIPELINE_PROGRAM_ALIGN - 1U) & ~(PIPELINE_PROGRAM_ALIGN - 1U);
	error = nvrtx_vk_gpu_alloc(session->owner, bytes, &pipeline->code_token, &cpu, &va);
	if (error != 0)
		return error;
	if ((va & (PIPELINE_PROGRAM_ALIGN - 1U)) != 0U)
		return EINVAL;
	kern_memcpy(cpu, binary->code, (size_t)binary->code_words * 4U);

	/* What a launch says of it. */
	kernel->va = va;
	kernel->gprs = binary->gpr_count;
	kernel->local_size[0] = binary->local_size[0];
	kernel->local_size[1] = binary->local_size[1];
	kernel->local_size[2] = binary->local_size[2];
	kernel->shared_bytes = binary->shared_bytes;
	kernel->barrier_count = binary->barrier_count;

	/* Succeeded: the program is in the GPU's memory. */
	return 0;
}

/*
 * Keeps where each storage buffer and uniform buffer of a compute program
 * comes from, in the order of its storage table's entries and of its
 * constant buffers; the group counts' buffer keeps its set.
 */
static int
pipeline_kernel_sources(
	struct nvrtx_vk_kernel *kernel,
	const struct nvrtx_sm75_binary *binary)
{
	uint32_t index;
	uint32_t set;

	/* The storage buffers, from a set a layout may have or the group counts' set. */
	if (binary->storage_count > NVRTX_VK_STORAGE_MAX || binary->uniform_count > NVRTX_VK_UNIFORMS_MAX)
		return ENOTSUP;
	kernel->storage_count = binary->storage_count;
	for (index = 0U; index < binary->storage_count; index++) {
		set = binary->storage[index].set;
		if (set == DRV_GPU_IR_SYSTEM_SET)
			set = NVRTX_VK_SYSTEM_SET;
		else if (set >= NVRTX_VK_SETS_MAX || binary->storage[index].binding >= NVRTX_VK_BINDINGS_MAX ||
			 binary->storage[index].element >= NVRTX_VK_ELEMENTS_MAX)
			return ENOTSUP;
		kernel->storage[index].set = set;
		kernel->storage[index].binding = binary->storage[index].binding;
		kernel->storage[index].element = binary->storage[index].element;
	}

	/* The uniform buffers, in the order of their constant buffers. */
	kernel->uniform_count = binary->uniform_count;
	for (index = 0U; index < binary->uniform_count; index++) {
		if (binary->uniforms[index].set >= NVRTX_VK_SETS_MAX || binary->uniforms[index].binding >= NVRTX_VK_BINDINGS_MAX ||
		    binary->uniforms[index].element >= NVRTX_VK_ELEMENTS_MAX)
			return ENOTSUP;
		kernel->uniforms[index].set = binary->uniforms[index].set;
		kernel->uniforms[index].binding = binary->uniforms[index].binding;
		kernel->uniforms[index].element = binary->uniforms[index].element;
	}

	/* Succeeded: every buffer's descriptor is known. */
	return 0;
}

/*
 * Compiles one module for a stage, with the frontend's features beyond the
 * compiler's own and gl_ViewIndex read as `view`; the reason of a refusal
 * is shown.
 */
static int
pipeline_compile_stage(
	const struct nvrtx_vk_shader *module,
	enum drv_gpu_shader_stage stage,
	uint32_t shader_model,
	uint32_t features,
	uint32_t view,
	uint32_t *reads_view,
	struct nvrtx_sm75_binary **binary)
{
	struct nvrtx_sm75_diagnostic diagnostic;
	int error;

	/* Compiled for the shader model asked for. */
	kern_memset(&diagnostic, 0, sizeof(diagnostic));
	error = nvrtx_sm75_compile_stage_view(module->words, module->word_count, stage, shader_model, features, view, reads_view, binary, &diagnostic);
	if (error != 0) {
		kern_logf("nvrtx: vk: a stage %u module of %u words did not compile (%d at %u: %s)\n",
			  (unsigned)stage,
			  (unsigned)module->word_count,
			  error,
			  (unsigned)diagnostic.instruction,
			  (diagnostic.reason != NULL) ? diagnostic.reason : "?");
		return error;
	}

	/* A graphics program has its header, a compute one none. */
	if (((*binary)->graphics == 0U) != (stage == DRV_GPU_STAGE_COMPUTE))
		return EINVAL;

	/* Succeeded: the program is compiled. */
	return 0;
}

/*
 * Copies a compiled program to its place (its header, then its code) and
 * keeps its address, registers, texture sources and uniform buffer sources.
 */
static int
pipeline_program(
	struct nvrtx_vk_program *program,
	const struct nvrtx_sm75_binary *binary,
	uint8_t *cpu,
	uint64_t va)
{
	uint32_t index;

	/* The header where the address points, the code behind it. */
	kern_memcpy(cpu, binary->sph, PIPELINE_HEADER_BYTES);
	kern_memcpy(cpu + PIPELINE_HEADER_BYTES, binary->code, (size_t)binary->code_words * 4U);

	/* As many textures, storage and uniform buffers as its constant buffer 0 and its slots hold. */
	if (binary->texture_count > NVRTX_VK_TEXTURES_MAX || binary->storage_count > NVRTX_VK_STORAGE_MAX || binary->uniform_count > NVRTX_VK_UNIFORMS_MAX)
		return ENOTSUP;

	/* The address, the registers and where each texture handle comes from. */
	program->va = va;
	program->gprs = binary->gpr_count;
	program->texture_count = binary->texture_count;
	for (index = 0U; index < binary->texture_count; index++) {
		if (binary->textures[index].set >= NVRTX_VK_SETS_MAX || binary->textures[index].binding >= NVRTX_VK_BINDINGS_MAX ||
		    binary->textures[index].element >= NVRTX_VK_ELEMENTS_MAX)
			return ENOTSUP;
		program->textures[index].set = binary->textures[index].set;
		program->textures[index].binding = binary->textures[index].binding;
		program->textures[index].element = binary->textures[index].element;
	}

	/* Where each entry of its storage table comes from (a storage buffer, or a uniform buffer past the constant buffers). */
	program->storage_count = binary->storage_count;
	for (index = 0U; index < binary->storage_count; index++) {
		if (binary->storage[index].set >= NVRTX_VK_SETS_MAX || binary->storage[index].binding >= NVRTX_VK_BINDINGS_MAX ||
		    binary->storage[index].element >= NVRTX_VK_ELEMENTS_MAX)
			return ENOTSUP;
		program->storage[index].set = binary->storage[index].set;
		program->storage[index].binding = binary->storage[index].binding;
		program->storage[index].element = binary->storage[index].element;
	}

	/* Where each uniform buffer comes from, in the order of its constant buffer. */
	program->uniform_count = binary->uniform_count;
	for (index = 0U; index < binary->uniform_count; index++) {
		if (binary->uniforms[index].set >= NVRTX_VK_SETS_MAX || binary->uniforms[index].binding >= NVRTX_VK_BINDINGS_MAX ||
		    binary->uniforms[index].element >= NVRTX_VK_ELEMENTS_MAX)
			return ENOTSUP;
		program->uniforms[index].set = binary->uniforms[index].set;
		program->uniforms[index].binding = binary->uniforms[index].binding;
		program->uniforms[index].element = binary->uniforms[index].element;
	}

	/* Succeeded: the program is in place. */
	return 0;
}

/* Reports the bytes a program takes in the run: its header and code, on the boundary. */
static uint32_t
pipeline_program_bytes(
	const struct nvrtx_sm75_binary *binary)
{
	uint32_t bytes;

	/* The header and the code, rounded up. */
	bytes = PIPELINE_HEADER_BYTES + binary->code_words * 4U;
	bytes = (bytes + PIPELINE_PROGRAM_ALIGN - 1U) & ~(PIPELINE_PROGRAM_ALIGN - 1U);

	/* Succeeded: the program's share of the run. */
	return bytes;
}

/* Frees the compiled programs of a build (the run keeps the copies). */
static void
pipeline_build_free(
	struct pipeline_build *build)
{
	/* Each compiled program. */
	if (build->vertex != NULL)
		nvrtx_sm75_binary_free(build->vertex);
	if (build->fragment != NULL)
		nvrtx_sm75_binary_free(build->fragment);
	if (build->geometry != NULL)
		nvrtx_sm75_binary_free(build->geometry);
	if (build->kernel != NULL)
		nvrtx_sm75_binary_free(build->kernel);
	build->vertex = NULL;
	build->fragment = NULL;
	build->geometry = NULL;
	build->kernel = NULL;
}

/* Destroys a pipeline nothing holds: its programs' run goes back to the node. */
static int
pipeline_destroy(
	struct nvrtx_vk_session *session,
	void *payload)
{
	struct nvrtx_vk_pipeline *pipeline;
	uint32_t view;

	pipeline = payload;

	/* The pipelines of the other views, each with its run. */
	for (view = 0U; view < NVRTX_VK_VIEWS_MAX; view++) {
		if (pipeline->views[view] != NULL)
			(void)pipeline_destroy(session, pipeline->views[view]);
	}

	/* The run, when the programs were copied. */
	if (pipeline->code_token != NULL)
		nvrtx_vk_gpu_free(session->owner, pipeline->code_token);

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

/*
 * Reports the vertices of one input primitive of a VkPrimitiveTopology the
 * executor draws (what a geometry program takes), or 0 for one it does not
 * (the patches of tessellation).
 *
 * Values referenced: the Vulkan 1.3 specification, 21.1 (Primitive
 * Topologies) and 28.2 (Geometry Shader Input Primitives).
 */
static uint32_t
pipeline_topology_vertices(
	uint32_t topology)
{
	/* Each topology's primitive. */
	switch (topology) {
	case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
		return 1U;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
		return 2U;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
		return 3U;
	case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
	case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
		return 4U;
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
	case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
		return 6U;
	default:
		break;
	}

	/* Not drawn. */
	return 0U;
}

/*
 * Reports whether a vertex or geometry program writes the point size: its
 * header's output map has the point size's attribute.
 *
 * Value referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sph.rs
 * (ShaderProgramHeader::set_omap_system_values_ab, the attributes below
 * the generic ones a bit for each word).
 */
static uint32_t
pipeline_writes_point_size(
	const struct nvrtx_sm75_binary *binary)
{
	uint32_t written;

	/* The point size's bit of the output map. */
	written = pipeline_writes_attribute(binary, NVRTX_SM75_ATTR_POINT_SIZE);

	/* Succeeded: whether it is set. */
	return written;
}

/*
 * Reports whether a vertex or geometry program writes one of the system
 * attributes below the generic ones (the point size, the layer): its
 * header's output map has the attribute's word.
 *
 * Value referenced: Mesa 25.3.6 src/nouveau/compiler/nak/sph.rs
 * (ShaderProgramHeader::set_omap_system_values_ab, a bit for each word).
 */
static uint32_t
pipeline_writes_attribute(
	const struct nvrtx_sm75_binary *binary,
	uint32_t attribute)
{
	uint32_t bit;

	/* The attribute's bit of the output map. */
	bit = NVRTX_SM75_SPH_OMAP_AB_LOW + attribute / 4U;

	/* Succeeded: whether it is set. */
	return (binary->sph[bit / 32U] >> (bit % 32U)) & 1U;
}

/* Returns the lowest view of a subpass's view mask; view 0 for a pass of one view. */
static uint32_t
pipeline_first_view(
	uint32_t view_mask)
{
	uint32_t view;

	/* The lowest bit set. */
	for (view = 0U; view < NVRTX_VK_VIEWS_MAX; view++) {
		if ((view_mask & (1U << view)) != 0U)
			return view;
	}

	/* A pass of one view renders view 0. */
	return 0U;
}
