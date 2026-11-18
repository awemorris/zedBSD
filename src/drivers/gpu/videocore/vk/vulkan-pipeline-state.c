/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Finite Keiland graphics state is copied into immutable native fetch/raster metadata without retaining creation-record pointers. */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/videocore/vk/vulkan-pipeline.h"

static int fetch_state(struct bcm2711_vulkan_pipeline *pipeline, const VkPipelineVertexInputStateCreateInfo *input);
static int raster_state(struct bcm2711_vulkan_pipeline *pipeline, const VkPipelineRasterizationStateCreateInfo *raster);
static int blend_state(struct bcm2711_vulkan_pipeline *pipeline, const VkPipelineColorBlendStateCreateInfo *blend);
static int dynamic_state(struct bcm2711_vulkan_pipeline *pipeline, const VkPipelineDynamicStateCreateInfo *dynamic);
static int static_area(struct bcm2711_vulkan_pipeline *pipeline, const VkPipelineViewportStateCreateInfo *viewport);
static int depth_state(struct bcm2711_vulkan_pipeline *pipeline, const VkPipelineDepthStencilStateCreateInfo *depth);
static int blend_equation(VkBlendFactor source, VkBlendFactor destination, VkBlendOp operation, uint32_t *fields);
static int stencil_face(const VkStencilOpState *face, VkStencilOpState *copy);

/*
 * Validates the admitted single-sample triangle state, whose viewport and scissor are dynamic, and copies its exact native fetch/raster semantics.
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
	    info->pRasterizationState == NULL)
		return EINVAL;

	/*
	 * A pipeline whose rasterizer discards every primitive (whose vertex
	 * shader only stores) needs no viewport, multisample or colour blend
	 * state; any other needs the first two, and colour blend state unless
	 * its subpass has no colour attachment (the pass's colour slots, which
	 * the pass's own check compares, say so).
	 */
	pipeline->discard = false;
	if (info->pRasterizationState->rasterizerDiscardEnable == VK_TRUE)
		pipeline->discard = true;
	if (!pipeline->discard &&
	    (info->pMultisampleState == NULL ||
	     info->pViewportState == NULL))
		return EINVAL;
	error = fetch_state(pipeline, info->pVertexInputState);
	if (error != 0)
		return error;
	assembly = info->pInputAssemblyState;
	if (assembly->sType != VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO ||
	    assembly->pNext != NULL ||
	    assembly->flags != 0)
		return ENOTSUP;
	/* The lists, strips and the fan the hardware draws; adjacency and patches need stages it does not have. */
	if (assembly->topology > VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN)
		return ENOTSUP;
	if (assembly->primitiveRestartEnable != VK_FALSE && assembly->primitiveRestartEnable != VK_TRUE)
		return EINVAL;

	/* A restart ends a strip or a fan; a list has none to end. */
	if (assembly->primitiveRestartEnable == VK_TRUE &&
	    (assembly->topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST ||
	     assembly->topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
	     assembly->topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST))
		return EINVAL;
	pipeline->topology = assembly->topology;
	pipeline->restart = false;
	if (assembly->primitiveRestartEnable == VK_TRUE)
		pipeline->restart = true;

	/* One viewport/scissor pair, each dynamic (supplied by the draws) or the pipeline's own. */
	error = dynamic_state(pipeline, info->pDynamicState);
	if (error != 0)
		return error;
	viewport = info->pViewportState;
	if (viewport != NULL) {
		if (viewport->sType != VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO ||
		    viewport->pNext != NULL ||
		    viewport->flags != 0)
			return ENOTSUP;
		if (viewport->viewportCount != 1 || viewport->scissorCount != 1)
			return ENOTSUP;
	}
	error = static_area(pipeline, viewport);
	if (error != 0)
		return error;
	error = raster_state(pipeline, info->pRasterizationState);
	if (error != 0)
		return error;

	/* A discarding pipeline's draws reach no fragment; its depth state, if any, still stands. */
	if (pipeline->discard && (info->pMultisampleState == NULL || info->pColorBlendState == NULL)) {
		error = depth_state(pipeline, info->pDepthStencilState);
		if (error != 0)
			return error;
		return 0;
	}

	/*
	 * One sample or four a pixel, all four of them covered unless the
	 * sample mask leaves some out; minSampleShading is ignored and the
	 * optional sampling effects are refused.  A single-sample pipeline's
	 * mask must keep its one sample.
	 */
	samples = info->pMultisampleState;
	if (samples->sType != VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO ||
	    samples->pNext != NULL ||
	    samples->flags != 0)
		return ENOTSUP;
	if ((samples->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT && samples->rasterizationSamples != VK_SAMPLE_COUNT_4_BIT) ||
	    samples->sampleShadingEnable != VK_FALSE ||
	    samples->alphaToCoverageEnable != VK_FALSE ||
	    samples->alphaToOneEnable != VK_FALSE)
		return ENOTSUP;
	pipeline->samples = samples->rasterizationSamples;
	pipeline->sample_mask = 0xfU;
	if (samples->pSampleMask != NULL) {
		if (samples->rasterizationSamples == VK_SAMPLE_COUNT_1_BIT && (samples->pSampleMask[0] & 1U) == 0)
			return ENOTSUP;
		if (samples->rasterizationSamples == VK_SAMPLE_COUNT_4_BIT)
			pipeline->sample_mask = samples->pSampleMask[0] & 0xfU;
	}

	/* Exact colour state selects opaque replacement or a blend equation of the tile buffer; without it no colour is written. */
	pipeline->colour_count = 0;
	if (info->pColorBlendState != NULL) {
		error = blend_state(pipeline, info->pColorBlendState);
		if (error != 0)
			return error;
	}

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
		if (bcm2711_vulkan_attribute_components(attribute->format) == 0)
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

/* Copies fill/cull/winding and depth bias state while rejecting depth clamping and nonunit line semantics. */
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
	    (raster->rasterizerDiscardEnable != VK_FALSE && raster->rasterizerDiscardEnable != VK_TRUE) ||
	    raster->polygonMode != VK_POLYGON_MODE_FILL)
		return ENOTSUP;
	if (raster->depthBiasEnable != VK_FALSE && raster->depthBiasEnable != VK_TRUE)
		return EINVAL;

	/* Depth bias offsets each polygon's depths by its constant and slope factors, within its clamp. */
	pipeline->depth_bias = false;
	if (raster->depthBiasEnable == VK_TRUE)
		pipeline->depth_bias = true;
	kern_memcpy(&pipeline->depth_bias_constant, &raster->depthBiasConstantFactor, sizeof(pipeline->depth_bias_constant));
	kern_memcpy(&pipeline->depth_bias_clamp, &raster->depthBiasClamp, sizeof(pipeline->depth_bias_clamp));
	kern_memcpy(&pipeline->depth_bias_slope, &raster->depthBiasSlopeFactor, sizeof(pipeline->depth_bias_slope));
	if ((raster->cullMode & ~VK_CULL_MODE_FRONT_AND_BACK) != 0)
		return ENOTSUP;
	if (raster->frontFace != VK_FRONT_FACE_CLOCKWISE && raster->frontFace != VK_FRONT_FACE_COUNTER_CLOCKWISE)
		return ENOTSUP;

	/* A static line width is 1 to 32 pixels (NaN fails); a dynamic one ignores the static field. */
	kern_memcpy(&line, &raster->lineWidth, sizeof(line));
	if ((pipeline->dynamic & (1U << VK_DYNAMIC_STATE_LINE_WIDTH)) == 0 && (line < 0x3f800000U || line > 0x42000000U))
		return ENOTSUP;
	pipeline->line_width = line;

	/* Immutable culling and winding are lowered into native CL state with the draw's actual viewport orientation. */
	pipeline->cull = raster->cullMode;
	pipeline->front = raster->frontFace;

	/* Succeeded: the admitted raster semantics are entirely represented by native immutable metadata. */
	return 0;
}

/*
 * Selects each colour attachment's opaque replacement or blend equation,
 * and the channels it writes.
 *
 * Every attachment blends and writes as its own state says (the tile
 * buffer has a blend configuration and a channel mask for each target);
 * the build moves them to the pass's colour slots.
 */
static int
blend_state(
	struct bcm2711_vulkan_pipeline *pipeline,
	const VkPipelineColorBlendStateCreateInfo *blend)
{
	const VkPipelineColorBlendAttachmentState *attachment;
	uint32_t colour;
	uint32_t alpha;
	uint32_t index;
	int error;

	/* Up to four colour attachments and disabled logic operation match the finite fragment tile-output ABI. */
	if (blend->sType != VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO ||
	    blend->pNext != NULL ||
	    blend->flags != 0 ||
	    blend->logicOpEnable != VK_FALSE)
		return ENOTSUP;
	if (blend->attachmentCount > BCM2711_VULKAN_PASS_COLOURS ||
	    (blend->attachmentCount != 0 && blend->pAttachments == NULL))
		return ENOTSUP;
	pipeline->colour_count = blend->attachmentCount;

	/* The static blend constants, which a constant blend factor reads unless they are dynamic. */
	kern_memcpy(pipeline->blend_constants, blend->blendConstants, sizeof(pipeline->blend_constants));

	/* Each attachment: no blending keeps premultiplied source-over in the unused equation. */
	pipeline->blend = 0;
	pipeline->colour_skip = 0;
	for (index = 0; index < blend->attachmentCount; index++) {
		attachment = &blend->pAttachments[index];
		pipeline->blend_factors[index] = (7U << 20) | (1U << 16) | (7U << 8) | (1U << 4);
		if ((attachment->colorWriteMask & ~(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)) != 0)
			return EINVAL;
		if (attachment->blendEnable != VK_FALSE && attachment->blendEnable != VK_TRUE)
			return EINVAL;

		/*
		 * The channels not written are masked in the tile buffer; Vulkan's
		 * R, G, B and A bits are the native mask's bits 0 to 3 (set: not
		 * written).
		 */
		pipeline->colour_skip |= (~attachment->colorWriteMask & 0xfU) << (index * 4U);
		if (attachment->blendEnable == VK_FALSE)
			continue;

		/* The colour and the alpha equations, each its destination factor, source factor and mode. */
		error = blend_equation(attachment->srcColorBlendFactor, attachment->dstColorBlendFactor, attachment->colorBlendOp, &colour);
		if (error != 0)
			return error;
		error = blend_equation(attachment->srcAlphaBlendFactor, attachment->dstAlphaBlendFactor, attachment->alphaBlendOp, &alpha);
		if (error != 0)
			return error;
		pipeline->blend |= 1U << index;
		pipeline->blend_factors[index] = colour << 12 | alpha;
	}

	/* Succeeded: disabled-logic fields do not become native state. */
	return 0;
}

/*
 * Encodes one blend equation as the native fields: the destination factor
 * in bits 8 to 11, the source factor in bits 4 to 7 and the mode in bits 0
 * to 3.
 *
 * Vulkan's factors from ZERO to SRC_ALPHA_SATURATE, the constant ones
 * included, and its operations from ADD to MAX, have the native codes'
 * values (Mesa src/broadcom/cle/v3d_packet.xml, the Blend Factor and Blend
 * Mode enums); each draw sets the Blend Constant Color the constant factors
 * read.  The dual-source factors are refused.
 */
static int
blend_equation(
	VkBlendFactor source,
	VkBlendFactor destination,
	VkBlendOp operation,
	uint32_t *fields)
{
	/* Each factor reads the source, the destination or the constant, not a second source. */
	if ((uint32_t)source > (uint32_t)VK_BLEND_FACTOR_SRC_ALPHA_SATURATE)
		return ENOTSUP;
	if ((uint32_t)destination > (uint32_t)VK_BLEND_FACTOR_SRC_ALPHA_SATURATE)
		return ENOTSUP;

	/* The operation is one of the five basic ones. */
	if ((uint32_t)operation > (uint32_t)VK_BLEND_OP_MAX)
		return ENOTSUP;

	/* Succeeded: the codes are the native ones. */
	*fields = (uint32_t)destination << 8 | (uint32_t)source << 4 | (uint32_t)operation;
	return 0;
}

/*
 * Records which states the draws supply instead of the pipeline.
 *
 * The viewport and the scissor must be dynamic: the native draw state has no
 * static viewport.  The line width, depth bias, blend constants and the
 * stencil masks and reference may be.  Depth bounds are refused, as their
 * test is.
 */
static int
dynamic_state(
	struct bcm2711_vulkan_pipeline *pipeline,
	const VkPipelineDynamicStateCreateInfo *dynamic)
{
	uint32_t index;
	uint32_t state;
	uint32_t declared;

	/* A pipeline without the record has every state static. */
	pipeline->dynamic = 0;
	if (dynamic == NULL)
		return 0;
	if (dynamic->sType != VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO ||
	    dynamic->pNext != NULL ||
	    dynamic->flags != 0 ||
	    dynamic->dynamicStateCount > BCM2711_VULKAN_DYNAMIC_STATES ||
	    dynamic->pDynamicStates == NULL)
		return ENOTSUP;

	/* Each named state is a core one other than depth bounds, named once. */
	declared = 0;
	for (index = 0; index < dynamic->dynamicStateCount; index++) {
		state = (uint32_t)dynamic->pDynamicStates[index];
		if (state >= BCM2711_VULKAN_DYNAMIC_STATES || state == (uint32_t)VK_DYNAMIC_STATE_DEPTH_BOUNDS)
			return ENOTSUP;
		if ((declared & (1U << state)) != 0)
			return EINVAL;
		declared |= 1U << state;
	}

	/* The draws supply the declared states; the pipeline has the others. */
	pipeline->dynamic = declared;

	/* Succeeded: the draws supply the declared states. */
	return 0;
}

/*
 * Copies a static viewport and scissor, which a pipeline whose dynamic
 * states leave them out has in its viewport state.  A discarding pipeline
 * may have no viewport state: its draws then take a one-pixel viewport and
 * scissor, which nothing reaches.
 */
static int
static_area(
	struct bcm2711_vulkan_pipeline *pipeline,
	const VkPipelineViewportStateCreateInfo *viewport)
{
	/* The one-pixel viewport's IEEE words: x 0, y 0, width 1, height 1, depth 0 to 1. */
	static const uint32_t pixel[6] = { 0, 0, 0x3f800000U, 0x3f800000U, 0, 0x3f800000U };
	static const VkRect2D square = { { 0, 0 }, { 1, 1 } };
	const VkRect2D *scissor;

	/* The viewport, unless the draws supply it: the state's, or the one-pixel one of a discarding pipeline. */
	pipeline->static_viewport = false;
	if ((pipeline->dynamic & (1U << VK_DYNAMIC_STATE_VIEWPORT)) == 0) {
		if (viewport != NULL && viewport->pViewports != NULL)
			kern_memcpy(pipeline->viewport, viewport->pViewports, sizeof(pipeline->viewport));
		else if (pipeline->discard)
			kern_memcpy(pipeline->viewport, pixel, sizeof(pipeline->viewport));
		else
			return EINVAL;
		pipeline->static_viewport = true;
	}

	/* The scissor likewise. */
	pipeline->static_scissor = false;
	if ((pipeline->dynamic & (1U << VK_DYNAMIC_STATE_SCISSOR)) == 0) {
		scissor = &square;
		if (viewport != NULL && viewport->pScissors != NULL)
			scissor = viewport->pScissors;
		else if (!pipeline->discard)
			return EINVAL;
		pipeline->scissor = *scissor;
		pipeline->static_scissor = true;
	}

	/* Succeeded: the pipeline has what its draws do not supply. */
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

/*
 * Tells how many four-byte words a vertex attribute of a format is: the
 * 32-bit float and integer formats, whose words the hardware's fetch passes
 * to the VPM unchanged (as a float attribute's are; an integer shader input
 * reads them as integers).  Zero for any other format.
 */
uint32_t
bcm2711_vulkan_attribute_components(
	VkFormat format)
{
	/* Each format's components. */
	switch (format) {
	case VK_FORMAT_R32_SFLOAT:
	case VK_FORMAT_R32_SINT:
	case VK_FORMAT_R32_UINT:
		return 1;
	case VK_FORMAT_R32G32_SFLOAT:
	case VK_FORMAT_R32G32_SINT:
	case VK_FORMAT_R32G32_UINT:
		return 2;
	case VK_FORMAT_R32G32B32_SFLOAT:
	case VK_FORMAT_R32G32B32_SINT:
	case VK_FORMAT_R32G32B32_UINT:
		return 3;
	case VK_FORMAT_R32G32B32A32_SFLOAT:
	case VK_FORMAT_R32G32B32A32_SINT:
	case VK_FORMAT_R32G32B32A32_UINT:
		return 4;
	default:
		return 0;
	}
}

/* Tells the bits of the one a missing fourth component of an attribute format reads, as Vulkan's defaults have it. */
uint32_t
bcm2711_vulkan_attribute_one(
	VkFormat format)
{
	/* The integer formats read the integer one. */
	switch (format) {
	case VK_FORMAT_R32_SINT:
	case VK_FORMAT_R32_UINT:
	case VK_FORMAT_R32G32_SINT:
	case VK_FORMAT_R32G32_UINT:
	case VK_FORMAT_R32G32B32_SINT:
	case VK_FORMAT_R32G32B32_UINT:
		return 1U;
	default:
		return 0x3f800000U;
	}
}
