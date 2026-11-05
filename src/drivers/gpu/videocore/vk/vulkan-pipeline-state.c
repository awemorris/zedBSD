/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Finite Keiland graphics state is copied into immutable native fetch/raster metadata without retaining creation-record pointers. */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vk/vulkan-pipeline.h"

static int fetch_state(struct bcm2711_vulkan_pipeline *pipeline, const VkPipelineVertexInputStateCreateInfo *input);
static int raster_state(struct bcm2711_vulkan_pipeline *pipeline, const VkPipelineRasterizationStateCreateInfo *raster);
static int blend_state(struct bcm2711_vulkan_pipeline *pipeline, const VkPipelineColorBlendStateCreateInfo *blend);
static int dynamic_state(const VkPipelineDynamicStateCreateInfo *dynamic);
static int depth_state(struct bcm2711_vulkan_pipeline *pipeline, const VkPipelineDepthStencilStateCreateInfo *depth);
static int blend_equation(VkBlendFactor source, VkBlendFactor destination, VkBlendOp operation, uint32_t *fields);
static int stencil_face(const VkStencilOpState *face, VkStencilOpState *copy);

/*
 * Validates the admitted dynamic-viewport/scissor single-sample triangle state and copies its exact native fetch/raster semantics.
 */
int
bcm2711_vulkan_pipeline_state(
	struct bcm2711_vulkan_pipeline *pipeline,
	const VkGraphicsPipelineCreateInfo *info)
{
	const VkPipelineInputAssemblyStateCreateInfo *assembly;
	const VkPipelineViewportStateCreateInfo *viewport;
	const VkPipelineMultisampleStateCreateInfo *samples;
	int error;

	/* Unsupported derivative, optional-stage and rasterizer-discard contracts cannot enter this finite path implicitly. */
	if (info->sType != VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO ||
	    info->pNext != NULL ||
	    info->flags != 0 ||
	    info->subpass != 0)
		return ENOTSUP;
	if (info->pTessellationState != NULL ||
	    info->basePipelineHandle != VK_NULL_HANDLE ||
	    info->basePipelineIndex != -1)
		return ENOTSUP;
	if (info->pVertexInputState == NULL ||
	    info->pInputAssemblyState == NULL ||
	    info->pRasterizationState == NULL ||
	    info->pMultisampleState == NULL ||
	    info->pViewportState == NULL ||
	    info->pColorBlendState == NULL)
		return EINVAL;
	error = fetch_state(pipeline, info->pVertexInputState);
	if (error != 0)
		return error;
	assembly = info->pInputAssemblyState;
	if (assembly->sType != VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO ||
	    assembly->pNext != NULL ||
	    assembly->flags != 0)
		return ENOTSUP;
	if (assembly->topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || assembly->primitiveRestartEnable != VK_FALSE)
		return ENOTSUP;

	/* One viewport/scissor pair is supplied by each recorded draw, independently of ignored static arrays. */
	error = dynamic_state(info->pDynamicState);
	if (error != 0)
		return error;
	viewport = info->pViewportState;
	if (viewport->sType != VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO ||
	    viewport->pNext != NULL ||
	    viewport->flags != 0)
		return ENOTSUP;
	if (viewport->viewportCount != 1 || viewport->scissorCount != 1)
		return ENOTSUP;
	error = raster_state(pipeline, info->pRasterizationState);
	if (error != 0)
		return error;

	/* Single-sample operation ignores unused mask bits and minSampleShading while refusing enabled optional sampling effects. */
	samples = info->pMultisampleState;
	if (samples->sType != VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO ||
	    samples->pNext != NULL ||
	    samples->flags != 0)
		return ENOTSUP;
	if (samples->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT ||
	    samples->sampleShadingEnable != VK_FALSE ||
	    samples->alphaToCoverageEnable != VK_FALSE ||
	    samples->alphaToOneEnable != VK_FALSE)
		return ENOTSUP;
	if (samples->pSampleMask != NULL) {
		if ((samples->pSampleMask[0] & 1U) == 0)
			return ENOTSUP;
	}

	/* Exact colour state selects opaque replacement or a blend equation of the tile buffer. */
	error = blend_state(pipeline, info->pColorBlendState);
	if (error != 0)
		return error;

	/* The depth and stencil tests, when the pipeline states them, become native test state. */
	error = depth_state(pipeline, info->pDepthStencilState);
	if (error != 0)
		return error;

	/* Succeeded: immutable native state contains no temporary array or record pointer. */
	return 0;
}

/* Validates independent vertex binding and attribute declarations before copying their exact immutable metadata. */
static int
fetch_state(
	struct bcm2711_vulkan_pipeline *pipeline,
	const VkPipelineVertexInputStateCreateInfo *input)
{
	const VkVertexInputBindingDescription *binding;
	const VkVertexInputAttributeDescription *attribute;
	uint32_t index;
	uint32_t previous;
	uint32_t found;

	/* The reported sixteen-binding/attribute and offset/stride limits bound exact native fetch declarations. */
	if (input->sType != VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO ||
	    input->pNext != NULL ||
	    input->flags != 0)
		return ENOTSUP;
	if (input->vertexBindingDescriptionCount > BCM2711_VULKAN_VERTEX_BINDINGS || input->vertexAttributeDescriptionCount > BCM2711_VULKAN_VERTEX_ATTRIBUTES)
		return ENOTSUP;
	if (input->vertexBindingDescriptionCount != 0 && input->pVertexBindingDescriptions == NULL)
		return EINVAL;
	if (input->vertexAttributeDescriptionCount != 0 && input->pVertexAttributeDescriptions == NULL)
		return EINVAL;
	for (index = 0; index < input->vertexBindingDescriptionCount; index++) {
		binding = &input->pVertexBindingDescriptions[index];
		if (binding->binding >= BCM2711_VULKAN_VERTEX_BINDINGS || binding->stride > 2048U)
			return ENOTSUP;

		/* A binding advances per vertex or, one element per instance, per instance. */
		if (binding->inputRate != VK_VERTEX_INPUT_RATE_VERTEX && binding->inputRate != VK_VERTEX_INPUT_RATE_INSTANCE)
			return ENOTSUP;
		for (previous = 0; previous < index; previous++) {
			if (input->pVertexBindingDescriptions[previous].binding == binding->binding)
				return EINVAL;
		}

		/* Complete validated declarations are ordinary scalar metadata, independent of client order. */
		pipeline->bindings[index] = *binding;
	}

	/* Every declared attribute has one unique finite location, supported raw float representation and declared binding. */
	for (index = 0; index < input->vertexAttributeDescriptionCount; index++) {
		attribute = &input->pVertexAttributeDescriptions[index];
		if (attribute->location >= BCM2711_VULKAN_VERTEX_ATTRIBUTES ||
		    attribute->offset > 2047U ||
		    (attribute->offset & 3U) != 0)
			return ENOTSUP;
		if (attribute->format != VK_FORMAT_R32_SFLOAT &&
		    attribute->format != VK_FORMAT_R32G32_SFLOAT &&
		    attribute->format != VK_FORMAT_R32G32B32_SFLOAT &&
		    attribute->format != VK_FORMAT_R32G32B32A32_SFLOAT)
			return ENOTSUP;
		for (previous = 0; previous < index; previous++) {
			if (input->pVertexAttributeDescriptions[previous].location == attribute->location)
				return EINVAL;
		}

		/* Binding IDs are declarations rather than array indices; sparse and unsorted bindings remain exact. */
		found = 0;
		for (previous = 0; previous < input->vertexBindingDescriptionCount; previous++) {
			if (input->pVertexBindingDescriptions[previous].binding == attribute->binding)
				found++;
		}

		/* Missing or repeated binding owners cannot produce a trusted fetch interval. */
		if (found != 1)
			return EINVAL;
		pipeline->attributes[index] = *attribute;
	}

	/* Publish exact interface extents only after all declarations have been validated. */
	pipeline->binding_count = input->vertexBindingDescriptionCount;
	pipeline->attribute_count = input->vertexAttributeDescriptionCount;

	/* Succeeded: draw preparation can address exact declared bindings and active scalar attributes. */
	return 0;
}

/* Copies fill/cull/winding state while rejecting unsupported depth bias, clamping and nonunit line semantics. */
static int
raster_state(
	struct bcm2711_vulkan_pipeline *pipeline,
	const VkPipelineRasterizationStateCreateInfo *raster)
{
	uint32_t line;

	/* No enabled raster feature may disappear behind a triangle-only native lowering. */
	if (raster->sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO ||
	    raster->pNext != NULL ||
	    raster->flags != 0)
		return ENOTSUP;
	if (raster->depthClampEnable != VK_FALSE ||
	    raster->rasterizerDiscardEnable != VK_FALSE ||
	    raster->depthBiasEnable != VK_FALSE ||
	    raster->polygonMode != VK_POLYGON_MODE_FILL)
		return ENOTSUP;
	if ((raster->cullMode & ~VK_CULL_MODE_FRONT_AND_BACK) != 0)
		return ENOTSUP;
	if (raster->frontFace != VK_FRONT_FACE_CLOCKWISE && raster->frontFace != VK_FRONT_FACE_COUNTER_CLOCKWISE)
		return ENOTSUP;
	kern_memcpy(&line, &raster->lineWidth, sizeof(line));
	if (line != 0x3f800000U)
		return ENOTSUP;

	/* Immutable culling and winding are lowered into native CL state with the draw's actual viewport orientation. */
	pipeline->cull = raster->cullMode;
	pipeline->front = raster->frontFace;

	/* Succeeded: the admitted raster semantics are entirely represented by native immutable metadata. */
	return 0;
}

/* Selects opaque replacement or the blend equation, and the channels written. */
static int
blend_state(
	struct bcm2711_vulkan_pipeline *pipeline,
	const VkPipelineColorBlendStateCreateInfo *blend)
{
	const VkPipelineColorBlendAttachmentState *attachment;
	uint32_t colour;
	uint32_t alpha;
	int error;

	/* One full colour attachment and disabled logic operation match the finite fragment tile-output ABI. */
	if (blend->sType != VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO ||
	    blend->pNext != NULL ||
	    blend->flags != 0 ||
	    blend->logicOpEnable != VK_FALSE)
		return ENOTSUP;
	if (blend->attachmentCount != 1 || blend->pAttachments == NULL)
		return ENOTSUP;
	attachment = &blend->pAttachments[0];
	if ((attachment->colorWriteMask & ~(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)) != 0)
		return EINVAL;

	/*
	 * The channels not written are masked in the tile buffer; Vulkan's R, G,
	 * B and A bits are the native mask's bits 0 to 3 (set: not written).
	 */
	pipeline->colour_skip = ~attachment->colorWriteMask & 0xfU;
	if (attachment->blendEnable != VK_FALSE && attachment->blendEnable != VK_TRUE)
		return EINVAL;

	/* A pipeline that does not blend keeps premultiplied source-over in the unused equation. */
	pipeline->blend = false;
	pipeline->blend_factors = (7U << 20) | (1U << 16) | (7U << 8) | (1U << 4);
	if (attachment->blendEnable == VK_FALSE)
		return 0;

	/* The colour and the alpha equations, each its destination factor, source factor and mode. */
	error = blend_equation(attachment->srcColorBlendFactor, attachment->dstColorBlendFactor, attachment->colorBlendOp, &colour);
	if (error != 0)
		return error;
	error = blend_equation(attachment->srcAlphaBlendFactor, attachment->dstAlphaBlendFactor, attachment->alphaBlendOp, &alpha);
	if (error != 0)
		return error;
	pipeline->blend = true;
	pipeline->blend_factors = colour << 12 | alpha;

	/* Succeeded: ignored blend constants and disabled-logic fields do not become native state. */
	return 0;
}

/*
 * Encodes one blend equation as the native fields: the destination factor
 * in bits 8 to 11, the source factor in bits 4 to 7 and the mode in bits 0
 * to 3.
 *
 * Vulkan's factors from ZERO to ONE_MINUS_DST_ALPHA and SRC_ALPHA_SATURATE,
 * and its operations from ADD to MAX, have the native codes' values (Mesa
 * src/broadcom/cle/v3d_packet.xml, the Blend Factor and Blend Mode enums).
 * The constant factors are refused: the pass sets no blend constant.
 */
static int
blend_equation(
	VkBlendFactor source,
	VkBlendFactor destination,
	VkBlendOp operation,
	uint32_t *fields)
{
	/* Each factor is a non-constant one. */
	if ((uint32_t)source > (uint32_t)VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA && source != VK_BLEND_FACTOR_SRC_ALPHA_SATURATE)
		return ENOTSUP;
	if ((uint32_t)destination > (uint32_t)VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA && destination != VK_BLEND_FACTOR_SRC_ALPHA_SATURATE)
		return ENOTSUP;

	/* The operation is one of the five basic ones. */
	if ((uint32_t)operation > (uint32_t)VK_BLEND_OP_MAX)
		return ENOTSUP;

	/* Succeeded: the codes are the native ones. */
	*fields = (uint32_t)destination << 8 | (uint32_t)source << 4 | (uint32_t)operation;
	return 0;
}

/* Requires exactly the dynamic viewport and scissor commands that the native graphics draw state implements. */
static int
dynamic_state(
	const VkPipelineDynamicStateCreateInfo *dynamic)
{
	uint32_t index;
	uint32_t viewport;
	uint32_t scissor;

	/* Static or other dynamic-state variants remain explicit unsupported contracts until their native lowering exists. */
	if (dynamic == NULL)
		return ENOTSUP;
	if (dynamic->sType != VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO ||
	    dynamic->pNext != NULL ||
	    dynamic->flags != 0 ||
	    dynamic->dynamicStateCount != 2 ||
	    dynamic->pDynamicStates == NULL)
		return ENOTSUP;
	viewport = 0;
	scissor = 0;
	for (index = 0; index < dynamic->dynamicStateCount; index++) {
		if (dynamic->pDynamicStates[index] == VK_DYNAMIC_STATE_VIEWPORT)
			viewport++;
		else if (dynamic->pDynamicStates[index] == VK_DYNAMIC_STATE_SCISSOR)
			scissor++;
		else
			return ENOTSUP;
	}

	/* Duplicate declarations cannot replace a missing required draw-time state. */
	if (viewport != 1 || scissor != 1)
		return EINVAL;

	/* Succeeded: every admitted pipeline has the exact two dynamic commands used by Keiland. */
	return 0;
}

/*
 * Copies the depth test and both faces' stencil tests.
 *
 * An absent state disables both tests.  Depth bounds are refused; the
 * compare masks, write masks and references keep their low eight bits, the
 * stencil buffer's width, as Vulkan defines the comparison.
 */
static int
depth_state(
	struct bcm2711_vulkan_pipeline *pipeline,
	const VkPipelineDepthStencilStateCreateInfo *depth)
{
	int error;

	/* A pipeline without the state tests nothing. */
	pipeline->depth_test = false;
	pipeline->depth_write = false;
	pipeline->depth_compare = VK_COMPARE_OP_ALWAYS;
	pipeline->stencil = false;
	if (depth == NULL)
		return 0;

	/* A chainless zero-flag record without depth bounds, whose enables are Booleans. */
	if (depth->sType != VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO ||
	    depth->pNext != NULL ||
	    depth->flags != 0)
		return ENOTSUP;
	if (depth->depthBoundsTestEnable != VK_FALSE)
		return ENOTSUP;
	if (depth->depthTestEnable > VK_TRUE ||
	    depth->depthWriteEnable > VK_TRUE ||
	    depth->stencilTestEnable > VK_TRUE)
		return EINVAL;

	/* The depth test's function is one of the eight shared Vulkan and V3D codes. */
	if (depth->depthTestEnable == VK_TRUE) {
		if ((uint32_t)depth->depthCompareOp > (uint32_t)VK_COMPARE_OP_ALWAYS)
			return EINVAL;
		pipeline->depth_test = true;
		pipeline->depth_compare = depth->depthCompareOp;
		if (depth->depthWriteEnable == VK_TRUE)
			pipeline->depth_write = true;
	}

	/* Each face's stencil test is copied with eight-bit masks and reference. */
	if (depth->stencilTestEnable == VK_TRUE) {
		error = stencil_face(&depth->front, &pipeline->stencil_front);
		if (error != 0)
			return error;
		error = stencil_face(&depth->back, &pipeline->stencil_back);
		if (error != 0)
			return error;
		pipeline->stencil = true;
	}

	/* Succeeded: the tests are immutable native pipeline state. */
	return 0;
}

/* Checks one face's operations and compare function and copies it with eight-bit masks and reference. */
static int
stencil_face(
	const VkStencilOpState *face,
	VkStencilOpState *copy)
{
	/* The operations are the eight core ones, the compare function one of the eight codes. */
	if ((uint32_t)face->failOp > (uint32_t)VK_STENCIL_OP_DECREMENT_AND_WRAP ||
	    (uint32_t)face->passOp > (uint32_t)VK_STENCIL_OP_DECREMENT_AND_WRAP ||
	    (uint32_t)face->depthFailOp > (uint32_t)VK_STENCIL_OP_DECREMENT_AND_WRAP ||
	    (uint32_t)face->compareOp > (uint32_t)VK_COMPARE_OP_ALWAYS)
		return EINVAL;

	/* The stencil buffer has eight bits, so only those of the masks and reference matter. */
	*copy = *face;
	copy->compareMask = face->compareMask & 0xffU;
	copy->writeMask = face->writeMask & 0xffU;
	copy->reference = face->reference & 0xffU;

	/* Succeeded: the face is native stencil state. */
	return 0;
}
