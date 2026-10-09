/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Actual client selected-state framing is decoded independently into finite self-owned temporary graphics fields. */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/vulkan-pipeline-record.h"

static int structure_header(struct i915_wire_reader *reader, VkStructureType expected);
static int shader_stages(struct i915_wire_reader *reader, struct bcm2711_vulkan_pipeline_record *record);
static int vertex_state(struct i915_wire_reader *reader, struct bcm2711_vulkan_pipeline_record *record);
static int assembly_viewport(struct i915_wire_reader *reader, struct bcm2711_vulkan_pipeline_record *record);
static int raster_samples(struct i915_wire_reader *reader, struct bcm2711_vulkan_pipeline_record *record);
static int colour_dynamic(struct i915_wire_reader *reader, struct bcm2711_vulkan_pipeline_record *record);

/*
 * Decodes one complete admitted graphics creation record without allocating arrays or retaining pointers into the stream.
 */
int
bcm2711_vulkan_pipeline_decode(
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_pipeline_record *record)
{
	int error;

	/* The selected client record begins with an ordinary chainless zero-flag graphics header. */
	kern_memset(record, 0, sizeof(*record));
	error = structure_header(reader, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
	if (error != 0)
		return error;
	record->info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	error = shader_stages(reader, record);
	if (error != 0)
		return error;
	error = vertex_state(reader, record);
	if (error != 0)
		return error;
	error = assembly_viewport(reader, record);
	if (error != 0)
		return error;
	error = raster_samples(reader, record);
	if (error != 0)
		return error;
	error = colour_dynamic(reader, record);
	if (error != 0)
		return error;

	/* Exact same-open typed identities are resolved by the builder only after the whole batch/output tail is consumed. */
	record->info.layout = (VkPipelineLayout)(uintptr_t)drv_i915_wire_read_u64(reader);
	record->info.renderPass = (VkRenderPass)(uintptr_t)drv_i915_wire_read_u64(reader);
	record->info.subpass = drv_i915_wire_read_u32(reader);
	record->info.basePipelineHandle = (VkPipeline)(uintptr_t)drv_i915_wire_read_u64(reader);
	record->info.basePipelineIndex = (int32_t)drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: every nested field belongs to this finite temporary record rather than a command arena or client address. */
	return 0;
}

/* Reads exact standard structure identity, absent chain and zero flags without skipping unsupported extension meaning. */
static int
structure_header(
	struct i915_wire_reader *reader,
	VkStructureType expected)
{
	uint32_t structure;
	uint32_t flags;
	uint64_t chain;

	/* Every admitted nested state uses the client's standard fixed-width header, with no advertised extension chain. */
	structure = drv_i915_wire_read_u32(reader);
	chain = drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;
	if (structure != (uint32_t)expected ||
	    chain != 0 ||
	    flags != 0)
		return ENOTSUP;

	/* Succeeded: the expected ordinary structure body follows. */
	return 0;
}

/* Copies exact vertex/fragment stage identities and the only supported main string before native module/interface compilation. */
static int
shader_stages(
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_pipeline_record *record)
{
	VkPipelineShaderStageCreateInfo *stage;
	uint64_t array;
	uint64_t bytes;
	uint64_t present;
	uint32_t count;
	uint32_t index;
	int error;
	int name;

	/* Count-selected stages are exactly the two roles supported by native graphics lowering. */
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    count != 2 ||
	    array != count)
		return ENOTSUP;
	record->info.stageCount = count;
	record->info.pStages = record->stages;
	for (index = 0; index < count; index++) {
		error = structure_header(reader, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
		if (error != 0)
			return error;
		stage = &record->stages[index];
		stage->sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stage->stage = drv_i915_wire_read_u32(reader);
		stage->module = (VkShaderModule)(uintptr_t)drv_i915_wire_read_u64(reader);
		bytes = drv_i915_wire_read_u64(reader);
		if (reader->error != 0 || bytes != 5)
			return ENOTSUP;
		i915_vkc_read_bytes(reader, record->entries[index], 5);
		present = drv_i915_wire_read_u64(reader);
		if (reader->error != 0)
			return EINVAL;
		if (present != 0)
			return ENOTSUP;
		name = kern_memcmp(record->entries[index], "main", 5);
		if (name != 0)
			return ENOTSUP;
		stage->pName = record->entries[index];
	}

	/* Succeeded: stages and names remain available through complete batch validation without a new heap owner. */
	return 0;
}

/* Decodes exact finite binding and attribute arrays into native record-owned fetch declarations. */
static int
vertex_state(
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_pipeline_record *record)
{
	VkVertexInputBindingDescription *binding;
	VkVertexInputAttributeDescription *attribute;
	uint64_t present;
	uint64_t array;
	uint32_t count;
	uint32_t index;
	int error;

	/* Vertex input is a mandatory selected state for the admitted non-discarding triangle pipeline. */
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1)
		return EINVAL;
	error = structure_header(reader, VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
	if (error != 0)
		return error;
	record->input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	record->info.pVertexInputState = &record->input;
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    count > BCM2711_VULKAN_VERTEX_BINDINGS ||
	    array != count)
		return ENOTSUP;
	record->input.vertexBindingDescriptionCount = count;
	if (count != 0)
		record->input.pVertexBindingDescriptions = record->bindings;
	for (index = 0; index < count; index++) {
		binding = &record->bindings[index];
		binding->binding = drv_i915_wire_read_u32(reader);
		binding->stride = drv_i915_wire_read_u32(reader);
		binding->inputRate = drv_i915_wire_read_u32(reader);
	}

	/* Attributes have an independent exact declared count; a larger encoded array cannot hide ignored declarations. */
	count = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    count > BCM2711_VULKAN_VERTEX_ATTRIBUTES ||
	    array != count)
		return ENOTSUP;
	record->input.vertexAttributeDescriptionCount = count;
	if (count != 0)
		record->input.pVertexAttributeDescriptions = record->attributes;
	for (index = 0; index < count; index++) {
		attribute = &record->attributes[index];
		attribute->location = drv_i915_wire_read_u32(reader);
		attribute->binding = drv_i915_wire_read_u32(reader);
		attribute->format = drv_i915_wire_read_u32(reader);
		attribute->offset = drv_i915_wire_read_u32(reader);
	}

	/* Complete count-selected arrays must fit before later state records may begin. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: exact vertex declarations have no borrowed wire or arena storage. */
	return 0;
}

/* Decodes mandatory triangle assembly and a single dynamically supplied viewport/scissor pair. */
static int
assembly_viewport(
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_pipeline_record *record)
{
	uint64_t present;
	uint64_t array;
	int error;

	/* Input assembly carries the actual primitive topology and restart choice before any optional tessellation record. */
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1)
		return EINVAL;
	error = structure_header(reader, VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
	if (error != 0)
		return error;
	record->assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	record->assembly.topology = drv_i915_wire_read_u32(reader);
	record->assembly.primitiveRestartEnable = drv_i915_wire_read_u32(reader);
	record->info.pInputAssemblyState = &record->assembly;
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 0)
		return ENOTSUP;

	/* The actual client omits dynamically selected static arrays while preserving their declared one-element counts. */
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1)
		return EINVAL;
	error = structure_header(reader, VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
	if (error != 0)
		return error;
	record->viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	record->viewport.viewportCount = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    record->viewport.viewportCount != 1 ||
	    array != 0)
		return ENOTSUP;
	record->viewport.scissorCount = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    record->viewport.scissorCount != 1 ||
	    array != 0)
		return ENOTSUP;
	record->info.pViewportState = &record->viewport;

	/* Succeeded: draw-time viewport/scissor data is represented by declarations rather than ignored client arrays. */
	return 0;
}

/* Decodes exact fixed raster and single-sample state while refusing unimplemented depth/stencil attachments. */
static int
raster_samples(
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_pipeline_record *record)
{
	uint64_t present;
	uint64_t array;
	int error;

	/* Every meaningful fixed raster field has an explicit fixed-width scalar or raw IEEE float representation. */
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1)
		return EINVAL;
	error = structure_header(reader, VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
	if (error != 0)
		return error;
	record->raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	record->raster.depthClampEnable = drv_i915_wire_read_u32(reader);
	record->raster.rasterizerDiscardEnable = drv_i915_wire_read_u32(reader);
	record->raster.polygonMode = drv_i915_wire_read_u32(reader);
	record->raster.cullMode = drv_i915_wire_read_u32(reader);
	record->raster.frontFace = drv_i915_wire_read_u32(reader);
	record->raster.depthBiasEnable = drv_i915_wire_read_u32(reader);
	i915_vkc_read_float(reader, &record->raster.depthBiasConstantFactor);
	i915_vkc_read_float(reader, &record->raster.depthBiasClamp);
	i915_vkc_read_float(reader, &record->raster.depthBiasSlopeFactor);
	i915_vkc_read_float(reader, &record->raster.lineWidth);
	record->info.pRasterizationState = &record->raster;

	/* Native single-sample state admits an absent mask or exactly one full-width mask word. */
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1)
		return EINVAL;
	error = structure_header(reader, VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
	if (error != 0)
		return error;
	record->samples.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	record->samples.rasterizationSamples = drv_i915_wire_read_u32(reader);
	record->samples.sampleShadingEnable = drv_i915_wire_read_u32(reader);
	i915_vkc_read_float(reader, &record->samples.minSampleShading);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || array > 1)
		return ENOTSUP;
	if (array != 0) {
		record->mask = drv_i915_wire_read_u32(reader);
		record->samples.pSampleMask = &record->mask;
	}

	/* Optional per-sample alpha effects and any following depth record remain explicit native validation inputs. */
	record->samples.alphaToCoverageEnable = drv_i915_wire_read_u32(reader);
	record->samples.alphaToOneEnable = drv_i915_wire_read_u32(reader);
	record->info.pMultisampleState = &record->samples;
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 0)
		return ENOTSUP;

	/* Succeeded: raster and sample semantics are entirely represented in finite temporary storage. */
	return 0;
}

/* Decodes one exact colour blend attachment and the native viewport/scissor dynamic-state declaration. */
static int
colour_dynamic(
	struct i915_wire_reader *reader,
	struct bcm2711_vulkan_pipeline_record *record)
{
	uint64_t present;
	uint64_t array;
	uint32_t index;
	int error;

	/* The sole colour attachment selects full replacement or supported premultiplied blending at native compilation. */
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1)
		return EINVAL;
	error = structure_header(reader, VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
	if (error != 0)
		return error;
	record->blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	record->blend.logicOpEnable = drv_i915_wire_read_u32(reader);
	record->blend.logicOp = drv_i915_wire_read_u32(reader);
	record->blend.attachmentCount = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    record->blend.attachmentCount != 1 ||
	    array != 1)
		return ENOTSUP;
	record->colour.blendEnable = drv_i915_wire_read_u32(reader);
	record->colour.srcColorBlendFactor = drv_i915_wire_read_u32(reader);
	record->colour.dstColorBlendFactor = drv_i915_wire_read_u32(reader);
	record->colour.colorBlendOp = drv_i915_wire_read_u32(reader);
	record->colour.srcAlphaBlendFactor = drv_i915_wire_read_u32(reader);
	record->colour.dstAlphaBlendFactor = drv_i915_wire_read_u32(reader);
	record->colour.alphaBlendOp = drv_i915_wire_read_u32(reader);
	record->colour.colorWriteMask = drv_i915_wire_read_u32(reader);
	record->blend.pAttachments = &record->colour;
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || array != 4)
		return EINVAL;
	for (index = 0; index < 4; index++)
		i915_vkc_read_float(reader, &record->blend.blendConstants[index]);
	record->info.pColorBlendState = &record->blend;

	/* Dynamic state arrays are exact two-element declarations, with semantic duplicate/state checks in the native builder. */
	present = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || present != 1)
		return EINVAL;
	error = structure_header(reader, VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
	if (error != 0)
		return error;
	record->dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	record->dynamic.dynamicStateCount = drv_i915_wire_read_u32(reader);
	array = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 ||
	    record->dynamic.dynamicStateCount != 2 ||
	    array != 2)
		return ENOTSUP;
	for (index = 0; index < 2; index++)
		record->commands[index] = drv_i915_wire_read_u32(reader);
	record->dynamic.pDynamicStates = record->commands;
	record->info.pDynamicState = &record->dynamic;
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: selected graphics state is self-contained and ready for exact same-device/native compiler validation. */
	return 0;
}
