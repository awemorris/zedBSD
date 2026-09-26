/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's pipeline layouts, shader modules and graphics pipelines
 * (see pipeline.h).
 *
 * Every command is decoded exactly as libvulkan encodes it: the records
 * through the generated codec, the framing around them as read from the
 * library's own sender (pipeline.c of libvulkan).
 */

#include "pipeline.h"
#include "codec.h"
#include "gfx.h"
#include "internal.h"
#include "object.h"
#include "reply.h"
#include <kern/kcrt.h>

#include <kern/klog.h>
#include <kern/kmem.h>

#include <libc/vulkan/vulkan_core.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "vulkan-codec.inc"

/* The largest SPIR-V module accepted, in bytes. */
#define I915_GFX_MAX_SHADER_BYTES	(1U << 20)

/* How many stages one pipeline may name. */
#define I915_GFX_MAX_PIPELINE_STAGES	8U

/* How many pipelines one vkCreateGraphicsPipelines may create. */
#define I915_GFX_MAX_CREATED_PIPELINES	4U

static int i915_gfx_decode_pipeline(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_gfx_pipeline *pipeline);
static int i915_gfx_decode_stages(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_gfx_pipeline *pipeline);
static int i915_gfx_decode_vertex_input(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_gfx_pipeline *pipeline);
static void i915_gfx_decode_viewport(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_gfx_pipeline *pipeline, int *viewport_given, int *scissor_given);
static void i915_gfx_decode_blend(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_gfx_pipeline *pipeline);
static void i915_gfx_decode_dynamic(struct i915_render_session *session, struct i915_wire_reader *reader, struct i915_gfx_pipeline *pipeline);
static void i915_gfx_float_bits(uint32_t *destination, const float *source);
static void i915_gfx_free_pipelines(struct i915_gfx_pipeline **pipelines, uint64_t count);

/*
 * Creates a VkPipelineLayout: vkCreatePipelineLayout, a generic create.
 *
 * A pipeline layout carries nothing a draw needs beyond what the pipeline
 * and the sets say; the layout keeps only its set count.
 */
int
drv_i915_gfx_create_pipeline_layout(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkPipelineLayoutCreateInfo info;
	uint64_t identity;
	uint32_t *layout;

	/* Decodes the create info behind the device and its presence marker. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkPipelineLayoutCreateInfo(reader, &session->arena, &info);
	identity = drv_i915_gfx_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Allocates the layout, which is its set count and nothing else. */
	layout = kern_calloc(1U, sizeof(*layout));
	if (layout != NULL)
		*layout = info.setLayoutCount;

	/* Publishes the layout and answers; a failed allocation is reported there. */
	drv_i915_gfx_create_reply(session, reply, I915_VK_OBJ_PIPELINE_LAYOUT, identity, layout, 0);

	/* Succeeded: the reply carries the result of the create. */
	return 0;
}

/*
 * Creates a VkShaderModule: vkCreateShaderModule, a generic create.
 *
 * The module keeps a copy of its SPIR-V words, allocated together with it.
 * A module that is absent, not whole words or larger than 1 MiB fails the
 * command.
 */
int
drv_i915_gfx_create_shader(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkShaderModuleCreateInfo info;
	struct i915_gfx_shader *shader;
	uint64_t identity;
	uint32_t words;

	/* Decodes the create info behind the device and its presence marker. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkShaderModuleCreateInfo(reader, &session->arena, &info);
	identity = drv_i915_gfx_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Refuses a module without code. */
	if (info.pCode == NULL)
		return EINVAL;

	/* Refuses code that is not whole 32-bit words. */
	if ((info.codeSize & 3U) != 0U)
		return EINVAL;

	/* Refuses a module larger than the executor accepts. */
	if (info.codeSize > I915_GFX_MAX_SHADER_BYTES)
		return EINVAL;

	/* Allocates the module with room for its words behind it. */
	words = (uint32_t)(info.codeSize / 4U);
	shader = kern_calloc(1U, sizeof(*shader) + (size_t)words * 4U);

	/* Copies the words out of the command's arena, which is emptied after the command. */
	if (shader != NULL) {
		shader->words = (uint32_t *)(shader + 1);
		shader->word_count = words;
		kern_memcpy(shader->words, info.pCode, (size_t)words * 4U);
	}

	/* Publishes the module and answers; a failed allocation is reported there. */
	drv_i915_gfx_create_reply(session, reply, I915_VK_OBJ_SHADER_MODULE, identity, shader, 0);

	/* Succeeded: the reply carries the result of the create. */
	return 0;
}

/*
 * Creates graphics pipelines: vkCreateGraphicsPipelines.
 *
 * The command is [device][cache][count][count]{create info}[pAllocator]
 * [count][identities] and the reply [result][count][identities].  A record
 * that does not decode fails the command without a reply body, because the
 * rest of the stream is unreadable.  Each pipeline's kernels are prepared
 * before any is published.
 */
int
drv_i915_gfx_create_pipelines(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct i915_gfx_pipeline *pipelines[I915_GFX_MAX_CREATED_PIPELINES];
	uint64_t identities[I915_GFX_MAX_CREATED_PIPELINES];
	uint64_t count;
	uint64_t identity_count;
	uint64_t index;
	uint64_t answered;
	uint32_t result;
	int error;

	/* Reads how many pipelines follow, behind the device, the cache and the count's first form. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (count == 0U || count > I915_GFX_MAX_CREATED_PIPELINES)
		return EINVAL;

	/* Allocates and decodes each pipeline, up to the first failure. */
	kern_memset(pipelines, 0, sizeof(pipelines));
	error = 0;
	for (index = 0U; index < count; index++) {
		pipelines[index] = kern_calloc(1U, sizeof(*pipelines[index]));
		if (pipelines[index] == NULL) {
			error = ENOMEM;
			break;
		}

		/* Each record decodes into an empty arena; one that fails leaves the rest unreadable. */
		session->arena.used = 0U;
		error = i915_gfx_decode_pipeline(session, reader, pipelines[index]);
		if (error != 0)
			break;
	}

	/* A pipeline that could not be made fails the whole command. */
	if (error != 0) {
		i915_gfx_free_pipelines(pipelines, count);
		return error;
	}

	/*
	 * Reads the identity count behind the allocator.  A count that is not
	 * one per pipeline fails the reader, so the stream decodes no further.
	 */
	(void)drv_i915_wire_read_u64(reader);
	identity_count = drv_i915_wire_read_u64(reader);
	if (identity_count != count)
		reader->error = 1;

	/* Reads the identity libvulkan chose for each pipeline. */
	for (index = 0U; index < count; index++)
		identities[index] = drv_i915_wire_read_u64(reader);
	if (reader->error != 0) {
		i915_gfx_free_pipelines(pipelines, count);
		return EINVAL;
	}

	/* Prepares each pipeline's kernels, up to the first failure. */
	for (index = 0U; index < count; index++) {
		error = drv_i915_gfx_pipeline_prepare(session, pipelines[index]);
		if (error != 0)
			break;
	}

	/* Publishes each pipeline once all of them are prepared, up to the first failure. */
	if (error == 0) {
		for (index = 0U; index < count; index++) {
			error = drv_i915_object_insert(session, I915_VK_OBJ_PIPELINE, identities[index], pipelines[index]);
			if (error != 0)
				break;
		}
	}

	/*
	 * XXX: pipelines already published by this batch stay published, and
	 * the ones that are not are neither released nor freed (happy path
	 * only).
	 */
	if (error != 0)
		kern_logf("i915: vk: vkCreateGraphicsPipelines failed: %d\n", error);

	/* Writes the result and the count of the create. */
	result = drv_i915_gfx_result(error);
	drv_i915_wire_reply_u32(reply, result);
	drv_i915_wire_reply_u64(reply, count);

	/* Answers each identity, or a null handle for each when the create failed. */
	for (index = 0U; index < count; index++) {
		answered = 0U;
		if (error == 0)
			answered = identities[index];
		drv_i915_wire_reply_u64(reply, answered);
	}

	/* Succeeded: the reply carries the result of the create. */
	return 0;
}

/*
 * Destroys a VkPipeline: vkDestroyPipeline, a generic destroy.
 *
 * The pipeline owns its compiled kernels, which are released with it.  An
 * unknown identity is not an error.
 */
int
drv_i915_gfx_destroy_pipeline(
	struct i915_render_session *session,
	struct i915_wire_reader *reader)
{
	struct i915_gfx_pipeline *pipeline;
	uint64_t identity;

	/* Reads the identity between the device and the allocator. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Unpublishes a known pipeline, releases its kernels and frees it. */
	pipeline = drv_i915_object_lookup(session, I915_VK_OBJ_PIPELINE, identity);
	if (pipeline != NULL) {
		drv_i915_object_remove(session, I915_VK_OBJ_PIPELINE, identity);
		drv_i915_gfx_pipeline_release(pipeline);
		kern_free(pipeline);
	}

	/* Succeeded: the pipeline is gone. */
	return 0;
}

/*
 * Decodes one VkGraphicsPipelineCreateInfo into a pipeline.
 *
 * The record is sent as pipeline_encode_graphics of libvulkan sends it: the
 * head, the stages, then each state record behind its own presence marker
 * (the rasterization record is always present), then [layout][renderPass]
 * [subpass][base pipeline][base index].
 */
static int
i915_gfx_decode_pipeline(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_gfx_pipeline *pipeline)
{
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineTessellationStateCreateInfo tessellation;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineDepthStencilStateCreateInfo depth;
	uint64_t present;
	int viewport_given;
	int scissor_given;
	int error;

	/* Decodes the shader stages. */
	error = i915_gfx_decode_stages(session, reader, pipeline);
	if (error != 0)
		return error;

	/* Decodes the vertex input state when it is present. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		error = i915_gfx_decode_vertex_input(session, reader, pipeline);
		if (error != 0)
			return error;
	}

	/* Decodes the input assembly state, which gives the topology. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		kern_memset(&assembly, 0, sizeof(assembly));
		i915_vkc_dec_VkPipelineInputAssemblyStateCreateInfo(reader, &session->arena, &assembly);
		pipeline->topology = assembly.topology;
	}

	/* Decodes the tessellation state; nothing of it is used. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		kern_memset(&tessellation, 0, sizeof(tessellation));
		i915_vkc_dec_VkPipelineTessellationStateCreateInfo(reader, &session->arena, &tessellation);
	}

	/* Decodes the viewport state when it is present. */
	viewport_given = 0;
	scissor_given = 0;
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_gfx_decode_viewport(session, reader, pipeline, &viewport_given, &scissor_given);

	/* Decodes the rasterization state, which gives the culling. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		kern_memset(&raster, 0, sizeof(raster));
		i915_vkc_dec_VkPipelineRasterizationStateCreateInfo(reader, &session->arena, &raster);
		pipeline->cull_mode = raster.cullMode;
		pipeline->front_face = raster.frontFace;
	}

	/* Decodes the multisample state; nothing of it is used. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		kern_memset(&multisample, 0, sizeof(multisample));
		i915_vkc_dec_VkPipelineMultisampleStateCreateInfo(reader, &session->arena, &multisample);
	}

	/* Decodes the depth and stencil state, which gives the depth test. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U) {
		kern_memset(&depth, 0, sizeof(depth));
		i915_vkc_dec_VkPipelineDepthStencilStateCreateInfo(reader, &session->arena, &depth);
		pipeline->depth_test = depth.depthTestEnable;
		pipeline->depth_write = depth.depthWriteEnable;
		pipeline->depth_compare = depth.depthCompareOp;
	}

	/* Decodes the colour blend state when it is present. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_gfx_decode_blend(session, reader, pipeline);

	/* Decodes the dynamic state when it is present. */
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_gfx_decode_dynamic(session, reader, pipeline);

	/* Says when a draw would have no viewport: none given and none dynamic. */
	if (reader->error == 0 &&
	    viewport_given == 0 &&
	    pipeline->dynamic_viewport == 0) {
		kern_logf("i915: vk: XXX pipeline has neither a viewport nor a dynamic viewport; its draws use an empty one\n");
	}

	/* Says when a draw would have no scissor: none given and none dynamic. */
	if (reader->error == 0 &&
	    scissor_given == 0 &&
	    pipeline->dynamic_scissor == 0) {
		kern_logf("i915: vk: XXX pipeline has neither a scissor nor a dynamic scissor; its draws use an empty one\n");
	}

	/* Skips the layout, the render pass, the subpass and the base pipeline and index. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);

	/* A record that did not decode leaves the rest of the stream unreadable. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the pipeline holds its decoded state. */
	return 0;
}

/* Decodes the head and the shader stages of a pipeline record. */
static int
i915_gfx_decode_stages(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_gfx_pipeline *pipeline)
{
	VkPipelineShaderStageCreateInfo stage;
	struct i915_gfx_shader *shader;
	uint64_t stages;
	uint64_t index;
	uint64_t module_id;

	/* Reads how many stages follow, behind sType, pNext, flags and stageCount. */
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	stages = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	if (stages > I915_GFX_MAX_PIPELINE_STAGES)
		return EINVAL;

	/* Decodes each stage and keeps the modules of the stages that run. */
	for (index = 0U; index < stages; index++) {
		kern_memset(&stage, 0, sizeof(stage));
		i915_vkc_dec_VkPipelineShaderStageCreateInfo(reader, &session->arena, &stage);
		module_id = (uint64_t)(uintptr_t)stage.module;
		shader = drv_i915_object_lookup(session, I915_VK_OBJ_SHADER_MODULE, module_id);

		/* Only the vertex and the fragment stages are run. */
		if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT) {
			pipeline->vertex = shader;
		} else if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
			pipeline->fragment = shader;
		} else {
			kern_logf("i915: vk: XXX pipeline stage 0x%x is not run\n", (unsigned)stage.stage);
		}
	}

	/* Succeeded: the stages are read; a failed decode is latched in the reader. */
	return 0;
}

/* Decodes the vertex input state of a pipeline record into its bindings and attributes. */
static int
i915_gfx_decode_vertex_input(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_gfx_pipeline *pipeline)
{
	VkPipelineVertexInputStateCreateInfo vertex_input;
	uint32_t index;

	/* Decodes the record. */
	kern_memset(&vertex_input, 0, sizeof(vertex_input));
	i915_vkc_dec_VkPipelineVertexInputStateCreateInfo(reader, &session->arena, &vertex_input);

	/* Refuses more bindings or attributes than a pipeline holds. */
	if (reader->error == 0) {
		if (vertex_input.vertexBindingDescriptionCount > I915_GFX_MAX_VERTEX_BINDINGS)
			return ENOTSUP;
		if (vertex_input.vertexAttributeDescriptionCount > I915_GFX_MAX_VERTEX_ATTRIBUTES)
			return ENOTSUP;
	}

	/* Keeps each binding's number and stride; a failed decode keeps none. */
	pipeline->binding_count = vertex_input.vertexBindingDescriptionCount;
	if (reader->error == 0) {
		for (index = 0U; index < pipeline->binding_count; index++) {
			pipeline->bindings[index].binding = vertex_input.pVertexBindingDescriptions[index].binding;
			pipeline->bindings[index].stride = vertex_input.pVertexBindingDescriptions[index].stride;
		}
	}

	/* Keeps each attribute's location, binding, format and offset; a failed decode keeps none. */
	pipeline->attribute_count = vertex_input.vertexAttributeDescriptionCount;
	if (reader->error == 0) {
		for (index = 0U; index < pipeline->attribute_count; index++) {
			pipeline->attributes[index].location = vertex_input.pVertexAttributeDescriptions[index].location;
			pipeline->attributes[index].binding = vertex_input.pVertexAttributeDescriptions[index].binding;
			pipeline->attributes[index].format = vertex_input.pVertexAttributeDescriptions[index].format;
			pipeline->attributes[index].offset = vertex_input.pVertexAttributeDescriptions[index].offset;
		}
	}

	/* Succeeded: the vertex input is kept; a failed decode is latched in the reader. */
	return 0;
}

/*
 * Decodes the viewport state of a pipeline record into its first viewport
 * and scissor.
 *
 * A dynamic viewport or scissor comes without its array; the flags say
 * which of the two the record gave.
 */
static void
i915_gfx_decode_viewport(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_gfx_pipeline *pipeline,
	int *viewport_given,
	int *scissor_given)
{
	VkPipelineViewportStateCreateInfo viewport;

	/* Decodes the record. */
	kern_memset(&viewport, 0, sizeof(viewport));
	i915_vkc_dec_VkPipelineViewportStateCreateInfo(reader, &session->arena, &viewport);

	/* A record that did not decode keeps nothing. */
	if (reader->error != 0)
		return;

	/* Keeps the first viewport as float bits. */
	if (viewport.pViewports != NULL && viewport.viewportCount != 0U) {
		i915_gfx_float_bits(&pipeline->viewport[0], &viewport.pViewports[0].x);
		i915_gfx_float_bits(&pipeline->viewport[1], &viewport.pViewports[0].y);
		i915_gfx_float_bits(&pipeline->viewport[2], &viewport.pViewports[0].width);
		i915_gfx_float_bits(&pipeline->viewport[3], &viewport.pViewports[0].height);
		i915_gfx_float_bits(&pipeline->viewport[4], &viewport.pViewports[0].minDepth);
		i915_gfx_float_bits(&pipeline->viewport[5], &viewport.pViewports[0].maxDepth);
		*viewport_given = 1;
	}

	/* Keeps the first scissor rectangle. */
	if (viewport.pScissors != NULL && viewport.scissorCount != 0U) {
		pipeline->scissor = viewport.pScissors[0];
		*scissor_given = 1;
	}
}

/*
 * Decodes the colour blend state of a pipeline record into the blend of
 * attachment 0: the enable, the factors and operations of the colour and
 * of the alpha, the write mask and the blend constants.
 *
 * XXX: one colour attachment is drawn, so only attachment 0 is kept; a
 * logic operation is named and not applied.
 */
static void
i915_gfx_decode_blend(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_gfx_pipeline *pipeline)
{
	VkPipelineColorBlendStateCreateInfo blend;
	const VkPipelineColorBlendAttachmentState *attachment;

	/* Decodes the record. */
	kern_memset(&blend, 0, sizeof(blend));
	i915_vkc_dec_VkPipelineColorBlendStateCreateInfo(reader, &session->arena, &blend);

	/* A record that did not decode says nothing. */
	if (reader->error != 0)
		return;

	/* Keeps the blend constants as float bits, which a constant factor reads. */
	i915_gfx_float_bits(&pipeline->blend_constants[0], &blend.blendConstants[0]);
	i915_gfx_float_bits(&pipeline->blend_constants[1], &blend.blendConstants[1]);
	i915_gfx_float_bits(&pipeline->blend_constants[2], &blend.blendConstants[2]);
	i915_gfx_float_bits(&pipeline->blend_constants[3], &blend.blendConstants[3]);

	/* Names a logic operation the draw does not apply. */
	if (blend.logicOpEnable != VK_FALSE)
		kern_logf("i915: vk: XXX pipeline asks for logic operation %u; the draw does not apply it\n", blend.logicOp);

	/* A record without attachments asks for no blending and writes nothing it could mask. */
	if (blend.attachmentCount == 0U || blend.pAttachments == NULL)
		return;

	/* Blending is on only when attachment 0 asks for it. */
	attachment = &blend.pAttachments[0];
	pipeline->blend_enable = 0U;
	if (attachment->blendEnable != VK_FALSE)
		pipeline->blend_enable = 1U;

	/* Keeps the factors and the operations of the colour and of the alpha. */
	pipeline->blend_src_color = attachment->srcColorBlendFactor;
	pipeline->blend_dst_color = attachment->dstColorBlendFactor;
	pipeline->blend_color_op = attachment->colorBlendOp;
	pipeline->blend_src_alpha = attachment->srcAlphaBlendFactor;
	pipeline->blend_dst_alpha = attachment->dstAlphaBlendFactor;
	pipeline->blend_alpha_op = attachment->alphaBlendOp;

	/* Keeps the components the attachment does not write. */
	pipeline->color_write_disable = ~(uint32_t)attachment->colorWriteMask & 0xfU;
}

/*
 * Decodes the dynamic state of a pipeline record.
 *
 * The viewport, the scissor and the blend constants may be dynamic; any
 * other dynamic state is named as not implemented, and a draw then uses the
 * pipeline's value.
 */
static void
i915_gfx_decode_dynamic(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_gfx_pipeline *pipeline)
{
	VkPipelineDynamicStateCreateInfo dynamic;
	uint32_t index;
	uint32_t state;

	/* Decodes the record. */
	kern_memset(&dynamic, 0, sizeof(dynamic));
	i915_vkc_dec_VkPipelineDynamicStateCreateInfo(reader, &session->arena, &dynamic);

	/* A record that did not decode says nothing. */
	if (reader->error != 0)
		return;

	/* A record whose list did not arrive declares nothing. */
	if (dynamic.pDynamicStates == NULL)
		return;

	/* Marks each dynamic state the draws take from the command buffer. */
	for (index = 0U; index < dynamic.dynamicStateCount; index++) {
		state = (uint32_t)dynamic.pDynamicStates[index];

		/* The viewport, the scissor and the blend constants come from their vkCmdSet* commands. */
		if (state == VK_DYNAMIC_STATE_VIEWPORT) {
			pipeline->dynamic_viewport = 1;
		} else if (state == VK_DYNAMIC_STATE_SCISSOR) {
			pipeline->dynamic_scissor = 1;
		} else if (state == VK_DYNAMIC_STATE_BLEND_CONSTANTS) {
			pipeline->dynamic_blend_constants = 1;
		} else {
			kern_logf("i915: vk: XXX pipeline declares dynamic state %u; no vkCmdSet* command for it is implemented\n", state);
		}
	}
}

/* Copies a float as its 32 bits; no floating-point register is involved. */
static void
i915_gfx_float_bits(
	uint32_t *destination,
	const float *source)
{
	/* Copies the bits without converting them. */
	kern_memcpy(destination, source, sizeof(*destination));
}

/* Frees the pipelines of a create that failed before any was prepared. */
static void
i915_gfx_free_pipelines(
	struct i915_gfx_pipeline **pipelines,
	uint64_t count)
{
	uint64_t index;

	/* Frees every slot; a slot that was never allocated is NULL. */
	for (index = 0U; index < count; index++)
		kern_free(pipelines[index]);
}
