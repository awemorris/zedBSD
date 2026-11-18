/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's pipelines: a compute pipeline's program, a graphics
 * pipeline's vertex and fragment programs and the fixed state its draws
 * take.  The shader compiler (compiler/shader.h) compiles each stage of
 * the SPIR-V module; the code goes into a buffer object of the session,
 * mapped where the shader cores can run it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/mali/vk/vk.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The most pipelines of one batch and stages of one pipeline. */
#define PIPELINE_BATCH_MAX	64U
#define PIPELINE_STAGES_MAX	4U

static int create_pipelines(struct mali_vk_session *session, bool compute, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int compute_pipeline(struct mali_vk_session *session, struct i915_wire_reader *reader, struct mali_vk_pipeline **result);
static int graphics_pipeline(struct mali_vk_session *session, struct i915_wire_reader *reader, struct mali_vk_pipeline **result);
static int graphics_decode(struct mali_vk_session *session, struct i915_wire_reader *reader, VkGraphicsPipelineCreateInfo *info, const struct mali_vk_shader **vertex, const struct mali_vk_shader **fragment);
static int graphics_state(const VkGraphicsPipelineCreateInfo *info, struct mali_vk_graphics *graphics);
static void depth_state(const VkGraphicsPipelineCreateInfo *info, struct mali_vk_graphics *graphics);
static void stencil_face(const VkStencilOpState *state, struct mali_surface_stencil_face *face);
static int compile(const struct mali_vk_shader *shader, uint32_t stage, struct mali_shader_program **program);
static int upload(struct mali_vk_session *session, struct mali_vk_pipeline *pipeline);
static int pipeline_destroy(struct mali_vk_session *session, void *payload);
static void *decode_state(struct mali_vk_session *session, struct i915_wire_reader *reader, size_t bytes, int *error);

/* Runs the pipeline commands. */
int
mali_vk_pipeline_route(
	struct mali_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	/* Picks the command, or leaves it to another handler. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CREATE_COMPUTE_PIPELINES:
		error = create_pipelines(session, true, reader, reply);
		break;
	case GPU_OP_CREATE_GRAPHICS_PIPELINES:
		error = create_pipelines(session, false, reader, reply);
		break;
	case GPU_OP_DESTROY_PIPELINE:
		error = mali_vk_destroy_plain(session, reader, I915_VK_OBJ_PIPELINE);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* A command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran. */
	return 0;
}

/*
 * vkCreate{Compute,Graphics}Pipelines: [device][cache][count][count]
 * [infos][allocator][count][identities] -> [result][count][identities],
 * 0 for a pipeline not made.
 */
static int
create_pipelines(
	struct mali_vk_session *session,
	bool compute,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct mali_vk_pipeline **made;
	uint64_t *identities;
	uint64_t count;
	uint64_t index;
	int first_error;
	int error;

	/* The device, the cache and the count. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count == 0U || count > PIPELINE_BATCH_MAX)
		return EINVAL;
	made = mali_vk_arena_take(session, (size_t)count * sizeof(*made));
	identities = mali_vk_arena_take(session, (size_t)count * sizeof(*identities));
	if (made == NULL || identities == NULL)
		return EINVAL;

	/* Each info, compiled as it is read. */
	first_error = 0;
	for (index = 0; index < count; index++) {
		if (compute) {
			error = compute_pipeline(session, reader, &made[index]);
		} else {
			error = graphics_pipeline(session, reader, &made[index]);
		}

		if (error == EINVAL || reader->error != 0)
			return EINVAL;
		if (error != 0 && first_error == 0)
			first_error = error;
	}

	/* The allocator and the identities. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	for (index = 0; index < count; index++)
		identities[index] = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Each pipeline made is published, or goes. */
	for (index = 0; index < count; index++) {
		if (made[index] == NULL)
			continue;
		error = mali_vk_object_publish(session, I915_VK_OBJ_PIPELINE, identities[index], made[index], pipeline_destroy);
		if (error != 0) {
			(void)pipeline_destroy(session, made[index]);
			made[index] = NULL;
			if (first_error == 0)
				first_error = error;
		}
	}

	/* Succeeded: the result and the identities of the pipelines made. */
	drv_i915_wire_reply_u32(reply, mali_vk_result(first_error));
	drv_i915_wire_reply_u64(reply, count);
	for (index = 0; index < count; index++) {
		if (made[index] == NULL) {
			drv_i915_wire_reply_u64(reply, 0);
		} else {
			drv_i915_wire_reply_u64(reply, identities[index]);
		}
	}

	return 0;
}

/*
 * One VkComputePipelineCreateInfo: its stage's module compiled for
 * compute, uploaded.
 */
static int
compute_pipeline(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct mali_vk_pipeline **result)
{
	VkComputePipelineCreateInfo info;
	const struct mali_vk_shader *shader;
	struct mali_vk_pipeline *pipeline;
	int error;

	/* The info. */
	*result = NULL;
	kern_memset(&info, 0, sizeof(info));
	i915_vkc_dec_VkComputePipelineCreateInfo(reader, &session->arena, &info);
	if (reader->error != 0)
		return EINVAL;

	/* A known module with no specialization. */
	shader = mali_vk_payload(session, I915_VK_OBJ_SHADER_MODULE, mali_vk_handle(info.stage.module));
	if (shader == NULL)
		return ENOTSUP;
	if (info.stage.pSpecializationInfo != NULL && info.stage.pSpecializationInfo->mapEntryCount != 0U)
		return ENOTSUP;

	/* The pipeline and its program. */
	pipeline = kern_calloc(1, sizeof(*pipeline));
	if (pipeline == NULL)
		return ENOMEM;
	pipeline->compute = true;
	error = compile(shader, MALI_SHADER_COMPUTE, &pipeline->program);
	if (error == 0)
		error = upload(session, pipeline);
	if (error != 0) {
		(void)pipeline_destroy(session, pipeline);
		return error;
	}

	/* Succeeded: the pipeline is made. */
	*result = pipeline;
	return 0;
}

/*
 * One VkGraphicsPipelineCreateInfo: its vertex and fragment stages
 * compiled, its fixed state kept, the code uploaded.
 */
static int
graphics_pipeline(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	struct mali_vk_pipeline **result)
{
	VkGraphicsPipelineCreateInfo info;
	const struct mali_vk_shader *vertex;
	const struct mali_vk_shader *fragment;
	struct mali_vk_pipeline *pipeline;
	struct mali_vk_graphics *graphics;
	int error;

	/* The info. */
	*result = NULL;
	error = graphics_decode(session, reader, &info, &vertex, &fragment);
	if (error != 0)
		return error;
	if (vertex == NULL || fragment == NULL)
		return ENOTSUP;

	/* The pipeline and its fixed state. */
	pipeline = kern_calloc(1, sizeof(*pipeline));
	if (pipeline == NULL)
		return ENOMEM;
	graphics = kern_calloc(1, sizeof(*graphics));
	if (graphics == NULL) {
		kern_free(pipeline);
		return ENOMEM;
	}

	pipeline->graphics = graphics;
	error = graphics_state(&info, graphics);

	/* The two programs and their code. */
	if (error == 0)
		error = compile(vertex, MALI_SHADER_VERTEX, &graphics->vertex);
	if (error == 0)
		error = compile(fragment, MALI_SHADER_FRAGMENT, &graphics->fragment);

	if (error == 0)
		error = upload(session, pipeline);
	if (error != 0) {
		(void)pipeline_destroy(session, pipeline);
		return error;
	}

	/* Succeeded: the pipeline is made. */
	*result = pipeline;
	return 0;
}

/*
 * Reads a graphics create info: its head, its stages, each state behind
 * its presence word, the layout, the pass, the subpass and the base.
 */
static int
graphics_decode(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	VkGraphicsPipelineCreateInfo *info,
	const struct mali_vk_shader **vertex,
	const struct mali_vk_shader **fragment)
{
	VkPipelineShaderStageCreateInfo *stages;
	uint64_t count;
	uint64_t index;
	int refused;
	int error;

	/* The head and the stages. */
	kern_memset(info, 0, sizeof(*info));
	*vertex = NULL;
	*fragment = NULL;
	refused = 0;
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	info->flags = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > PIPELINE_STAGES_MAX)
		return EINVAL;
	stages = mali_vk_arena_take(session, (size_t)count * sizeof(*stages) + 8U);
	if (stages == NULL)
		return EINVAL;
	for (index = 0; index < count; index++) {
		i915_vkc_dec_VkPipelineShaderStageCreateInfo(reader, &session->arena, &stages[index]);
		if (stages[index].pSpecializationInfo != NULL && stages[index].pSpecializationInfo->mapEntryCount != 0U)
			refused = ENOTSUP;
		if (stages[index].stage == VK_SHADER_STAGE_VERTEX_BIT)
			*vertex = mali_vk_payload(session, I915_VK_OBJ_SHADER_MODULE, mali_vk_handle(stages[index].module));
		if (stages[index].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
			*fragment = mali_vk_payload(session, I915_VK_OBJ_SHADER_MODULE, mali_vk_handle(stages[index].module));
	}
	info->stageCount = (uint32_t)count;
	info->pStages = stages;

	/* The vertex input, the assembly and the tessellation states. */
	error = 0;
	info->pVertexInputState = decode_state(session, reader, sizeof(VkPipelineVertexInputStateCreateInfo), &error);
	if (info->pVertexInputState != NULL)
		i915_vkc_dec_VkPipelineVertexInputStateCreateInfo(reader, &session->arena, (VkPipelineVertexInputStateCreateInfo *)info->pVertexInputState);
	info->pInputAssemblyState = decode_state(session, reader, sizeof(VkPipelineInputAssemblyStateCreateInfo), &error);
	if (info->pInputAssemblyState != NULL)
		i915_vkc_dec_VkPipelineInputAssemblyStateCreateInfo(reader, &session->arena, (VkPipelineInputAssemblyStateCreateInfo *)info->pInputAssemblyState);
	info->pTessellationState = decode_state(session, reader, sizeof(VkPipelineTessellationStateCreateInfo), &error);
	if (info->pTessellationState != NULL)
		i915_vkc_dec_VkPipelineTessellationStateCreateInfo(reader, &session->arena, (VkPipelineTessellationStateCreateInfo *)info->pTessellationState);

	/* The viewport, the rasterization and the multisample states. */
	info->pViewportState = decode_state(session, reader, sizeof(VkPipelineViewportStateCreateInfo), &error);
	if (info->pViewportState != NULL)
		i915_vkc_dec_VkPipelineViewportStateCreateInfo(reader, &session->arena, (VkPipelineViewportStateCreateInfo *)info->pViewportState);
	info->pRasterizationState = decode_state(session, reader, sizeof(VkPipelineRasterizationStateCreateInfo), &error);
	if (info->pRasterizationState != NULL)
		i915_vkc_dec_VkPipelineRasterizationStateCreateInfo(reader, &session->arena, (VkPipelineRasterizationStateCreateInfo *)info->pRasterizationState);
	info->pMultisampleState = decode_state(session, reader, sizeof(VkPipelineMultisampleStateCreateInfo), &error);
	if (info->pMultisampleState != NULL)
		i915_vkc_dec_VkPipelineMultisampleStateCreateInfo(reader, &session->arena, (VkPipelineMultisampleStateCreateInfo *)info->pMultisampleState);

	/* The depth, the blend and the dynamic states. */
	info->pDepthStencilState = decode_state(session, reader, sizeof(VkPipelineDepthStencilStateCreateInfo), &error);
	if (info->pDepthStencilState != NULL)
		i915_vkc_dec_VkPipelineDepthStencilStateCreateInfo(reader, &session->arena, (VkPipelineDepthStencilStateCreateInfo *)info->pDepthStencilState);
	info->pColorBlendState = decode_state(session, reader, sizeof(VkPipelineColorBlendStateCreateInfo), &error);
	if (info->pColorBlendState != NULL)
		i915_vkc_dec_VkPipelineColorBlendStateCreateInfo(reader, &session->arena, (VkPipelineColorBlendStateCreateInfo *)info->pColorBlendState);
	info->pDynamicState = decode_state(session, reader, sizeof(VkPipelineDynamicStateCreateInfo), &error);
	if (info->pDynamicState != NULL)
		i915_vkc_dec_VkPipelineDynamicStateCreateInfo(reader, &session->arena, (VkPipelineDynamicStateCreateInfo *)info->pDynamicState);
	if (error != 0)
		return error;

	/* The layout, the render pass, the subpass and the base pipeline and index. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	info->subpass = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded unless a stage was refused. */
	if (refused != 0)
		return refused;
	return 0;
}

/* Keeps a graphics pipeline's fixed state: vertex input, topology, culling, blending and the static viewport. */
static int
graphics_state(
	const VkGraphicsPipelineCreateInfo *info,
	struct mali_vk_graphics *graphics)
{
	const VkPipelineVertexInputStateCreateInfo *input;
	const VkPipelineColorBlendStateCreateInfo *blend;
	const VkPipelineColorBlendAttachmentState *attachment;
	uint32_t index;
	uint32_t binding;

	/* The vertex bindings and attributes. */
	input = info->pVertexInputState;
	if (input != NULL) {
		if (input->vertexBindingDescriptionCount > MALI_VK_VERTEX_BINDINGS_MAX || input->vertexAttributeDescriptionCount > MALI_VK_VERTEX_ATTRIBUTES_MAX)
			return ENOTSUP;
		for (index = 0; index < input->vertexBindingDescriptionCount; index++) {
			binding = input->pVertexBindingDescriptions[index].binding;
			if (binding >= MALI_VK_VERTEX_BINDINGS_MAX)
				return ENOTSUP;
			graphics->binding_stride[binding] = input->pVertexBindingDescriptions[index].stride;
			graphics->binding_rate[binding] = input->pVertexBindingDescriptions[index].inputRate;
			if (binding + 1U > graphics->binding_count)
				graphics->binding_count = binding + 1U;
		}

		graphics->attribute_count = input->vertexAttributeDescriptionCount;
		for (index = 0; index < input->vertexAttributeDescriptionCount; index++) {
			graphics->attribute_location[index] = input->pVertexAttributeDescriptions[index].location;
			graphics->attribute_binding[index] = input->pVertexAttributeDescriptions[index].binding;
			graphics->attribute_offset[index] = input->pVertexAttributeDescriptions[index].offset;
			graphics->attribute_format[index] = mali_vk_format_find(input->pVertexAttributeDescriptions[index].format);
			if (graphics->attribute_format[index] == NULL || !graphics->attribute_format[index]->vertex)
				return ENOTSUP;
		}
	}

	/* The topology; lists and strips of triangles and lists of points and lines. */
	graphics->topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	if (info->pInputAssemblyState != NULL)
		graphics->topology = info->pInputAssemblyState->topology;
	if (graphics->topology > VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN)
		return ENOTSUP;

	/* The culling and the front face. */
	if (info->pRasterizationState != NULL) {
		graphics->cull_mode = info->pRasterizationState->cullMode;
		graphics->front_face = info->pRasterizationState->frontFace;
	}

	/* The blend of each colour attachment. */
	blend = info->pColorBlendState;
	if (blend != NULL) {
		if (blend->attachmentCount > MALI_VK_ATTACHMENTS_MAX)
			return ENOTSUP;
		graphics->blend_count = blend->attachmentCount;
		for (index = 0; index < blend->attachmentCount; index++) {
			attachment = &blend->pAttachments[index];
			graphics->blend_enable[index] = attachment->blendEnable != VK_FALSE;
			graphics->write_mask[index] = attachment->colorWriteMask;
			graphics->blend_equation[index] = attachment->srcColorBlendFactor;
			graphics->blend_equation[index] |= attachment->dstColorBlendFactor << 5;
			graphics->blend_equation[index] |= attachment->colorBlendOp << 10;
			graphics->blend_equation[index] |= attachment->srcAlphaBlendFactor << 13;
			graphics->blend_equation[index] |= attachment->dstAlphaBlendFactor << 18;
			graphics->blend_equation[index] |= attachment->alphaBlendOp << 23;
		}
	}

	/* The depth and stencil tests, their faces, the depth bias, and which of them the commands set. */
	depth_state(info, graphics);

	/* A viewport and a scissor the pipeline fixes (not dynamic). */
	if (info->pViewportState != NULL && info->pViewportState->pViewports != NULL) {
		graphics->viewport_static = true;
		graphics->viewport = info->pViewportState->pViewports[0];
	}

	if (info->pViewportState != NULL && info->pViewportState->pScissors != NULL) {
		graphics->scissor_static = true;
		graphics->scissor = info->pViewportState->pScissors[0];
	}

	/* Succeeded: the state is kept. */
	return 0;
}

/*
 * Keeps a graphics pipeline's depth and stencil state: the depth test,
 * write and compare, the stencil test and its two faces, the depth bias
 * (its constant, clamp and slope factors' float bits), and which of the
 * bias, the stencil masks and the reference the commands set instead.
 * The pass's aspects and the program's own writes are the draw's.
 */
static void
depth_state(
	const VkGraphicsPipelineCreateInfo *info,
	struct mali_vk_graphics *graphics)
{
	const VkPipelineDepthStencilStateCreateInfo *depth;
	const VkPipelineRasterizationStateCreateInfo *raster;
	const VkPipelineDynamicStateCreateInfo *dynamic;
	struct mali_surface_zs_state *zs;
	uint32_t index;

	/* The tests and the faces. */
	zs = &graphics->zs;
	depth = info->pDepthStencilState;
	if (depth != NULL) {
		zs->depth_test = depth->depthTestEnable != VK_FALSE;
		zs->depth_write = depth->depthWriteEnable != VK_FALSE;
		zs->depth_compare = depth->depthCompareOp;
		zs->stencil_test = depth->stencilTestEnable != VK_FALSE;
		stencil_face(&depth->front, &zs->front);
		stencil_face(&depth->back, &zs->back);
	}

	/* The depth bias, as the rasterization fixes it. */
	raster = info->pRasterizationState;
	if (raster != NULL && raster->depthBiasEnable != VK_FALSE) {
		zs->bias = true;
		kern_memcpy(&zs->bias_constant, &raster->depthBiasConstantFactor, sizeof(zs->bias_constant));
		kern_memcpy(&zs->bias_clamp, &raster->depthBiasClamp, sizeof(zs->bias_clamp));
		kern_memcpy(&zs->bias_slope, &raster->depthBiasSlopeFactor, sizeof(zs->bias_slope));
	}

	/* The states the commands set. */
	dynamic = info->pDynamicState;
	if (dynamic == NULL || dynamic->pDynamicStates == NULL)
		return;
	for (index = 0; index < dynamic->dynamicStateCount; index++) {
		switch (dynamic->pDynamicStates[index]) {
		case VK_DYNAMIC_STATE_DEPTH_BIAS:
			graphics->dynamic_bias = true;
			break;
		case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK:
			graphics->dynamic_compare_mask = true;
			break;
		case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK:
			graphics->dynamic_write_mask = true;
			break;
		case VK_DYNAMIC_STATE_STENCIL_REFERENCE:
			graphics->dynamic_reference = true;
			break;
		default:
			break;
		}
	}
}

/* Copies one face's stencil state. */
static void
stencil_face(
	const VkStencilOpState *state,
	struct mali_surface_stencil_face *face)
{
	/* The ops, the compare, the masks and the reference. */
	face->fail_op = state->failOp;
	face->pass_op = state->passOp;
	face->depth_fail_op = state->depthFailOp;
	face->compare_op = state->compareOp;
	face->compare_mask = state->compareMask;
	face->write_mask = state->writeMask;
	face->reference = state->reference;
}

/* Compiles one stage of a module; a refusal is written once with its reason. */
static int
compile(
	const struct mali_vk_shader *shader,
	uint32_t stage,
	struct mali_shader_program **program)
{
	struct mali_shader_diagnostic diagnostic;
	int error;

	/* The compile. */
	kern_memset(&diagnostic, 0, sizeof(diagnostic));
	error = mali_shader_compile(shader->words, shader->word_count, stage, NULL, program, &diagnostic);
	if (error != 0) {
		kern_logf("mali: vk: stage %u not compiled (%d) at %u: %s\n",
			  (unsigned)stage,
			  error,
			  (unsigned)diagnostic.instruction,
			  diagnostic.reason != NULL ? diagnostic.reason : "no reason");
		*program = NULL;
		if (error == ENOMEM)
			return ENOMEM;
		return ENOTSUP;
	}

	/* Succeeded: the program is the pipeline's. */
	return 0;
}

/*
 * Copies a pipeline's code into a buffer object of the session, each
 * program at a 128-byte boundary, and keeps the programs' addresses.
 */
static int
upload(
	struct mali_vk_session *session,
	struct mali_vk_pipeline *pipeline)
{
	const struct mali_shader_program *first;
	const struct mali_shader_program *second;
	uint64_t first_bytes;
	uint64_t bytes;
	int error;

	/* The programs and the room they take. */
	if (pipeline->compute) {
		first = pipeline->program;
		second = NULL;
	} else {
		first = pipeline->graphics->vertex;
		second = pipeline->graphics->fragment;
	}

	first_bytes = (first->length + MALI_SHADER_CODE_ALIGNMENT - 1U) & ~(uint64_t)(MALI_SHADER_CODE_ALIGNMENT - 1U);
	bytes = first_bytes;
	if (second != NULL)
		bytes += second->length;

	/* The object, mapped. */
	error = mali_bo_create(session->owner->render, bytes, &pipeline->code);
	if (error != 0)
		return error;
	error = mali_resource_make(session->owner, pipeline->code, false, &pipeline->code_resource);
	if (error != 0)
		return error;
	pipeline->code_address = pipeline->code_resource->gpu_address;

	/* The code through the uncached view. */
	kern_memcpy((void *)(uintptr_t)pipeline->code->buffer.view, first->code, first->length);
	if (second != NULL)
		kern_memcpy((void *)(uintptr_t)(pipeline->code->buffer.view + first_bytes), second->code, second->length);

	/* The programs' addresses: a vertex program's two programs share one code. */
	if (pipeline->compute) {
		pipeline->vertex_address = pipeline->code_address;
	} else {
		pipeline->vertex_address = pipeline->code_address;
		pipeline->varying_address = 0;
		if (first->varying_offset != 0U)
			pipeline->varying_address = pipeline->code_address + first->varying_offset;
		pipeline->fragment_address = pipeline->code_address + first_bytes;
	}

	/* Succeeded: the code is where the cores read it. */
	return 0;
}

/* Destroys a pipeline: its code's mapping and object, then its programs. */
static int
pipeline_destroy(
	struct mali_vk_session *session,
	void *payload)
{
	struct mali_vk_pipeline *pipeline;

	/* The code. */
	pipeline = payload;
	if (pipeline->code_resource != NULL)
		mali_resource_unmake(session->owner, pipeline->code_resource);
	if (pipeline->code != NULL)
		mali_bo_put(session->owner->render, pipeline->code);

	/* The programs. */
	if (pipeline->program != NULL)
		mali_shader_program_free(pipeline->program);
	if (pipeline->graphics != NULL) {
		if (pipeline->graphics->vertex != NULL)
			mali_shader_program_free(pipeline->graphics->vertex);
		if (pipeline->graphics->fragment != NULL)
			mali_shader_program_free(pipeline->graphics->fragment);
		kern_free(pipeline->graphics);
	}

	kern_free(pipeline);
	return 0;
}

/* Reads a state's presence word and takes room for it when present; NULL when absent or out of room. */
static void *
decode_state(
	struct mali_vk_session *session,
	struct i915_wire_reader *reader,
	size_t bytes,
	int *error)
{
	uint64_t present;
	void *state;

	/* The presence word. */
	present = drv_i915_wire_read_u64(reader);
	if (present == 0U || reader->error != 0)
		return NULL;

	/* Cleared room in the scratch. */
	state = mali_vk_arena_take(session, bytes);
	if (state == NULL)
		*error = EINVAL;

	/* Succeeded: the room, decoded into by the caller. */
	return state;
}
