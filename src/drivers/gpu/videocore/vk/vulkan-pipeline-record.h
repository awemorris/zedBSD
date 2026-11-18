/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* One finite graphics creation record owns its temporary nested fields until the complete batch is consumed and compiled. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_PIPELINE_RECORD_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_PIPELINE_RECORD_H

#include "drivers/gpu/videocore/vk/vulkan-pipeline.h"

/* Self-contained selected-state fields require no per-array arena allocation or borrowed wire/application pointer. */
struct bcm2711_vulkan_pipeline_record {
	VkGraphicsPipelineCreateInfo info;
	VkPipelineShaderStageCreateInfo stages[2];
	char entries[2][8];
	VkPipelineVertexInputStateCreateInfo input;
	VkVertexInputBindingDescription bindings[BCM2711_VULKAN_VERTEX_BINDINGS];
	VkVertexInputAttributeDescription attributes[BCM2711_VULKAN_VERTEX_ATTRIBUTES];
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineViewportStateCreateInfo viewport;
	/* A pipeline's static viewport and scissor, when they are not dynamic. */
	VkViewport viewports[1];
	VkRect2D scissors[1];
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo samples;
	VkSampleMask mask;
	VkPipelineDepthStencilStateCreateInfo depth;
	VkPipelineColorBlendStateCreateInfo blend;
	VkPipelineColorBlendAttachmentState colours[BCM2711_VULKAN_PASS_COLOURS];
	VkPipelineDynamicStateCreateInfo dynamic;
	VkDynamicState commands[BCM2711_VULKAN_DYNAMIC_STATES];
};

/* One compute creation record: its info, whose one stage names the copied entry string. */
struct bcm2711_vulkan_pipeline_compute_record {
	VkComputePipelineCreateInfo info;
	char entry[8];
};

int bcm2711_vulkan_pipeline_decode(struct i915_wire_reader *reader, struct bcm2711_vulkan_pipeline_record *record);
int bcm2711_vulkan_pipeline_decode_compute(struct i915_wire_reader *reader, struct bcm2711_vulkan_pipeline_compute_record *record);

#endif
