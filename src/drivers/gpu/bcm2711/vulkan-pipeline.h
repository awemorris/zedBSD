/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Compiled immutable graphics pipelines retain exact shader interfaces independently of module and layout identities. */
#ifndef KERN_DRIVERS_GPU_BCM2711_VULKAN_PIPELINE_H
#define KERN_DRIVERS_GPU_BCM2711_VULKAN_PIPELINE_H

#include "drivers/gpu/bcm2711/shader.h"
#include "drivers/gpu/bcm2711/vulkan-layout.h"
#include "drivers/gpu/bcm2711/vulkan-target.h"

#define BCM2711_VULKAN_VERTEX_BINDINGS 16U
#define BCM2711_VULKAN_VERTEX_ATTRIBUTES 16U

/* One pipeline owns three native programs and immutable fetch/raster state until its final recorded/prepared draw owner retires. */
struct bcm2711_vulkan_pipeline {
	struct bcm2711_vulkan_input_owner owner;
	struct bcm2711_vulkan_object *pass;
	struct bcm2711_shader_binary *programs[3];
	VkVertexInputBindingDescription bindings[BCM2711_VULKAN_VERTEX_BINDINGS];
	VkVertexInputAttributeDescription attributes[BCM2711_VULKAN_VERTEX_ATTRIBUTES];
	uint32_t binding_count;
	uint32_t attribute_count;
	VkCullModeFlags cull;
	VkFrontFace front;
	bool blend;
};

int bcm2711_vulkan_pipeline_state(struct bcm2711_vulkan_pipeline *pipeline, const VkGraphicsPipelineCreateInfo *info);
int bcm2711_vulkan_pipeline_build(struct bcm2711_vulkan_session *session, struct bcm2711_vulkan_object *device, const VkGraphicsPipelineCreateInfo *info, struct bcm2711_vulkan_pipeline **pipeline);
int bcm2711_vulkan_pipeline_release(struct bcm2711_vulkan_session *session, void *payload);

#endif
