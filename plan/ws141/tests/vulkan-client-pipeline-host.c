/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Expose the real private client encoder with finite local object metadata; neither client source is modified. */
#include "userland/desktop/libvulkan/resources.c"
#include "userland/desktop/libvulkan/pipeline.c"

/*
 * Encodes one actual client graphics record using real selected-state and handle conversion functions.
 */
void
ws141_client_encode_graphics(
	struct vulkan_writer *writer,
	const VkGraphicsPipelineCreateInfo *source)
{
	struct VkDevice_T device;
	struct vulkan_render_pass pass;
	struct vulkan_object layout;
	struct vulkan_object modules[2];
	VkPipelineShaderStageCreateInfo stages[2];
	VkGraphicsPipelineCreateInfo info;
	VkBool32 colour;
	VkBool32 depth;
	uint32_t index;

	/* This explicit local metadata represents the same single-colour subpass already created in the native fixture. */
	memset(&device, 0, sizeof(device));
	memset(&pass, 0, sizeof(pass));
	pass.object.kind = VULKAN_OBJECT_RENDER_PASS;
	pass.object.wire_id = (uint64_t)(uintptr_t)source->renderPass;
	pass.subpass_count = 1;
	colour = VK_TRUE;
	depth = VK_FALSE;
	pass.color = &colour;
	pass.depth_stencil = &depth;
	memset(&layout, 0, sizeof(layout));
	layout.wire_id = (uint64_t)(uintptr_t)source->layout;

	/* Real nondispatchable client objects convert each selected native fixture identity to its ordinary wire handle. */
	memset(modules, 0, sizeof(modules));
	for (index = 0; index < 2; index++) {
		stages[index] = source->pStages[index];
		modules[index].wire_id = (uint64_t)(uintptr_t)stages[index].module;
		stages[index].module = (VkShaderModule)(uintptr_t)&modules[index];
	}

	/* The actual encoder owns state selection, ignored-field canonicalization, nested record widths and dynamic array omission. */
	info = *source;
	info.pStages = stages;
	info.layout = (VkPipelineLayout)(uintptr_t)&layout;
	info.renderPass = (VkRenderPass)(uintptr_t)&pass;
	pipeline_encode_graphics(writer, &device, 0, &info);

	/* Succeeded: writer status and exact bytes come from the unmodified actual client encoder. */
	return;
}
