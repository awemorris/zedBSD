/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Expose the real private client encoder with finite local object metadata; neither client source is modified. */
#include "userland/desktop/libvulkan/resources.c"
#include "userland/desktop/libvulkan/pipeline.c"
#include "userland/desktop/libvulkan/commands.c"

/*
 * Refuses host transport because this wrapper exercises finite real client recording without a kernel device descriptor.
 */
VkResult
vulkan_context_execute(
	struct vulkan_context *context,
	const struct vulkan_writer *writer,
	size_t reply_capacity,
	struct vulkan_reader *reader)
{
	/* Finite local records must never reach a transport flush in this explicit host fixture. */
	(void)context;
	(void)writer;
	(void)reply_capacity;
	vulkan_reader_init(reader, NULL, 0);

	/* Reports the fixture's absent native descriptor instead of fabricating successful device execution. */
	return VK_ERROR_DEVICE_LOST;
}

/*
 * Appends actual public vkCmd graphics records using matching finite local metadata and owned client writer storage.
 */
void
ws141_client_encode_recording(
	struct vulkan_writer *writer,
	uint64_t command_id)
{
	struct vulkan_context context;
	struct VkCommandBuffer_T command;
	struct vulkan_render_pass pass;
	struct vulkan_object framebuffer;
	struct vulkan_object pipeline;
	struct vulkan_object layout;
	struct vulkan_object set;
	struct vulkan_object buffer;
	VkAttachmentDescription attachment;
	VkRenderPassBeginInfo begin;
	VkClearValue clears[2];
	VkViewport viewport;
	VkRect2D scissor;
	VkDescriptorSet descriptor;
	VkBuffer vertex;
	VkDeviceSize offset;
	uint32_t push[8];
	uint32_t index;

	/* The explicit fixture mirrors the already-created native primary and single-colour attachment metadata. */
	memset(&context, 0, sizeof(context));
	context.max_resource_bytes = 1024U * 1024U;
	memset(&command, 0, sizeof(command));
	command.object.context = &context;
	command.object.wire_id = command_id;
	command.state = VULKAN_COMMAND_RECORDING;
	vulkan_writer_init(&command.recording);
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	memset(&pass, 0, sizeof(pass));
	pass.object.kind = VULKAN_OBJECT_RENDER_PASS;
	pass.object.wire_id = 120;
	pass.attachment_count = 1;
	pass.attachments = &attachment;

	/* Real client opaque handles translate to the exact preexisting native test identities. */
	memset(&framebuffer, 0, sizeof(framebuffer));
	framebuffer.wire_id = 122;
	memset(&pipeline, 0, sizeof(pipeline));
	pipeline.wire_id = 135;
	memset(&layout, 0, sizeof(layout));
	layout.wire_id = 131;
	memset(&set, 0, sizeof(set));
	set.wire_id = 163;
	memset(&buffer, 0, sizeof(buffer));
	buffer.wire_id = 170;
	descriptor = (VkDescriptorSet)(uintptr_t)&set;
	vertex = (VkBuffer)(uintptr_t)&buffer;
	offset = 0;

	/* The real selected clear encoder consumes one active union and canonicalizes the ignored extra entry. */
	memset(clears, 0, sizeof(clears));
	clears[0].color.float32[0] = 0.25f;
	clears[0].color.float32[3] = 1.0f;
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	begin.renderPass = (VkRenderPass)(uintptr_t)&pass;
	begin.framebuffer = (VkFramebuffer)(uintptr_t)&framebuffer;
	begin.renderArea.extent.width = 16;
	begin.renderArea.extent.height = 8;
	begin.clearValueCount = 2;
	begin.pClearValues = clears;
	vkCmdBeginRenderPass(&command, &begin, VK_SUBPASS_CONTENTS_INLINE);

	/* Public dynamic setters preserve real record headers, scalar codecs and exact selected array extents. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = 16.0f;
	viewport.height = 8.0f;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(&command, 0, 1, &viewport);
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent.width = 16;
	scissor.extent.height = 8;
	vkCmdSetScissor(&command, 0, 1, &scissor);
	vkCmdBindVertexBuffers(&command, 0, 1, &vertex, &offset);
	vkCmdBindPipeline(&command, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)(uintptr_t)&pipeline);
	vkCmdBindDescriptorSets(&command, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipelineLayout)(uintptr_t)&layout, 0, 1, &descriptor, 0, NULL);

	/* Copied push data and a complete triangle-list draw use the same API sequence as Keiland's ordinary quad path. */
	for (index = 0; index < 8; index++)
		push[index] = 0x3f000000U + index;
	vkCmdPushConstants(&command, (VkPipelineLayout)(uintptr_t)&layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), push);
	vkCmdDraw(&command, 6, 1, 0, 0);
	vkCmdEndRenderPass(&command);

	/* The bounded caller receives only complete actual recording bytes and their real local encoding outcome. */
	if (command.error != VK_SUCCESS)
		writer->error = command.error;
	vulkan_write_bytes(writer, command.recording.data, command.recording.bytes);
	vulkan_writer_finish(&command.recording);

	/* Succeeded: real client-owned recording bytes were copied before every fixture application pointer retired. */
	return;
}

/*
 * Appends one actual public barrier record using independently copied finite host object metadata.
 */
void
ws141_client_encode_barrier(
	struct vulkan_writer *writer,
	uint64_t command_id,
	uint64_t buffer_id,
	const uint64_t *image_ids,
	const VkImageLayout *layouts)
{
	struct vulkan_context context;
	struct VkCommandBuffer_T command;
	struct vulkan_object buffer;
	struct vulkan_object images[2];
	VkMemoryBarrier memory;
	VkBufferMemoryBarrier buffer_barrier;
	VkImageMemoryBarrier image_barriers[2];
	uint32_t index;

	/* The real recording wrapper owns its writer and the same native command identity used by the actual runtime fixture. */
	memset(&context, 0, sizeof(context));
	context.max_resource_bytes = 1024U * 1024U;
	memset(&command, 0, sizeof(command));
	command.object.context = &context;
	command.object.wire_id = command_id;
	command.state = VULKAN_COMMAND_RECORDING;
	vulkan_writer_init(&command.recording);

	/* One global memory dependency and one full logical buffer dependency use the actual public core encoder. */
	memset(&memory, 0, sizeof(memory));
	memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
	memory.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	memset(&buffer, 0, sizeof(buffer));
	buffer.wire_id = buffer_id;
	memset(&buffer_barrier, 0, sizeof(buffer_barrier));
	buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer_barrier.buffer = (VkBuffer)(uintptr_t)&buffer;
	buffer_barrier.size = VK_WHOLE_SIZE;

	/* Two colour images provide independent copied source layouts and owned handle translation, including duplicate-handle refusal cases. */
	memset(images, 0, sizeof(images));
	memset(image_barriers, 0, sizeof(image_barriers));
	for (index = 0; index < 2; index++) {
		images[index].wire_id = image_ids[index];
		image_barriers[index].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		image_barriers[index].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
		image_barriers[index].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		image_barriers[index].oldLayout = layouts[index];
		image_barriers[index].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		image_barriers[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		image_barriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		image_barriers[index].image = (VkImage)(uintptr_t)&images[index];
		image_barriers[index].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		image_barriers[index].subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
		image_barriers[index].subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
	}

	/* The public wrapper supplies exact opcode, command identity, array counts and copied Vulkan records. */
	vkCmdPipelineBarrier(&command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &memory, 1, &buffer_barrier, 2, image_barriers);
	if (command.error != VK_SUCCESS)
		writer->error = command.error;
	vulkan_write_bytes(writer, command.recording.data, command.recording.bytes);
	vulkan_writer_finish(&command.recording);

	/* Succeeded: no application structure or opaque client object survives the copied recording bytes. */
	return;
}

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
