/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The compositor's turn pass (WS217, ws217-p002b; the design is
 * plan/ws217/phase002b/phase.md).
 *
 * An output turned by 90 or 270 degrees (or by 180 where the display's
 * hardware cannot, under a test's policy) has its desktop drawn into an
 * image of the logical size, the turn image, and each frame then copies it
 * turned onto the swapchain's image in one pass: a quad over the whole
 * physical output whose texture coordinates are the logical image's
 * corners in another order (kwl_turn_coefficients), sampled pixel for
 * pixel.  Outside its passes the turn image is kept in the layout it is
 * sampled in.
 *
 * The two passes that draw into the turn image and the pipeline of the
 * turn pass are made the first time an output turns and kept for the
 * compositor's life, as the output's format is; the image, its view, its
 * framebuffer and its descriptor set live as long as the angle and the
 * output's opening.
 */

#include "kwl.h"
#include "compose.h"
#include "rotate.h"
#include "shaders.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/* The format features the turn image needs: drawn into, sampled, and copied from (the scene, and the mirror's linear blit). */
#define TURN_FEATURES	(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | \
			 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | \
			 VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)

static VkResult turn_shared(struct kwl_compose *compose);
static VkResult turn_pass(struct kwl_compose *compose, VkAttachmentLoadOp load, VkImageLayout initial, VkRenderPass *pass);
static VkResult turn_pipeline(struct kwl_compose *compose);
static VkResult turn_image(struct kwl_compose *compose, struct kwl_turn *turn);
static VkResult turn_set(struct kwl_compose *compose, struct kwl_turn *turn);

/*
 * Makes the turn image of an output's logical size for an angle, with its
 * view, framebuffer and descriptor set (and, the first time, the passes
 * and the pipeline they are drawn with).  Returns VK_SUCCESS, or the
 * error: VK_ERROR_FORMAT_NOT_SUPPORTED when the output's format cannot be
 * a turn image on this device.  On an error nothing is left made but the
 * shared passes and pipeline.
 */
VkResult
kwl_turn_open(
	struct kwl_compose *compose,
	uint32_t width,
	uint32_t height,
	unsigned rotation,
	struct kwl_turn *turn)
{
	VkFormatProperties properties;
	VkResult result;

	/* The image is drawn into, sampled and copied from in the output's format. */
	memset(turn, 0, sizeof(*turn));
	vkGetPhysicalDeviceFormatProperties(compose->physical, compose->format, &properties);
	if ((properties.optimalTilingFeatures & TURN_FEATURES) != TURN_FEATURES)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* The passes and the pipeline, made once. */
	result = turn_shared(compose);
	if (result != VK_SUCCESS)
		return result;

	/* The image of the logical size, for the angle. */
	turn->width = width;
	turn->height = height;
	turn->rotation = rotation;
	result = turn_image(compose, turn);
	if (result != VK_SUCCESS) {
		kwl_turn_close(compose, turn);
		return result;
	}

	/* Sampled pixel for pixel by the turn pass. */
	result = turn_set(compose, turn);
	if (result != VK_SUCCESS) {
		kwl_turn_close(compose, turn);
		return result;
	}

	/* Succeeded: nothing is drawn in it yet, so the first frame draws it whole. */
	turn->frame = 0U;
	return VK_SUCCESS;
}

/*
 * Destroys a turn image and what goes with it (the device does not use it
 * any more); a turn never made, or already closed, is left as it is.
 */
void
kwl_turn_close(
	struct kwl_compose *compose,
	struct kwl_turn *turn)
{
	/* The descriptor set back to the spares, then the framebuffer, the view, the image and its memory. */
	if (turn->set != VK_NULL_HANDLE)
		kwl_compose_set_put(compose, turn->set);
	if (turn->framebuffer != VK_NULL_HANDLE)
		vkDestroyFramebuffer(compose->device, turn->framebuffer, NULL);
	if (turn->view != VK_NULL_HANDLE)
		vkDestroyImageView(compose->device, turn->view, NULL);
	if (turn->image != VK_NULL_HANDLE)
		vkDestroyImage(compose->device, turn->image, NULL);
	if (turn->memory != VK_NULL_HANDLE)
		vkFreeMemory(compose->device, turn->memory, NULL);

	/* Nothing turns now. */
	memset(turn, 0, sizeof(*turn));
}

/*
 * Destroys the turn's passes and pipeline at the compositor's end (the
 * device is idle).
 */
void
kwl_turn_destroy_shared(
	struct kwl_compose *compose)
{
	/* The pipeline, then the two passes, those that were made. */
	if (compose->turn_pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(compose->device, compose->turn_pipeline, NULL);
	if (compose->turn_clear_pass != VK_NULL_HANDLE)
		vkDestroyRenderPass(compose->device, compose->turn_clear_pass, NULL);
	if (compose->turn_load_pass != VK_NULL_HANDLE)
		vkDestroyRenderPass(compose->device, compose->turn_load_pass, NULL);
	compose->turn_pipeline = VK_NULL_HANDLE;
	compose->turn_clear_pass = VK_NULL_HANDLE;
	compose->turn_load_pass = VK_NULL_HANDLE;
}

/*
 * Records the turn pass of a frame: the turn image copied turned onto the
 * swapchain's image, whole (count 0: cleared first, the image's earlier
 * contents unknown) or in the physical rectangles given (the rest of the
 * image kept from its last frame).  The scene's pass into the turn image
 * has ended.
 */
void
kwl_turn_record(
	struct kwl_server *server,
	struct kwl_list *command,
	uint32_t image,
	const VkRect2D *area,
	const VkRect2D *rects,
	unsigned count)
{
	struct kwl_compose *compose;
	VkRenderPassBeginInfo pass;
	VkClearValue clear;
	VkViewport viewport;
	VkRect2D whole;
	VkDeviceSize offset;
	float constants[8];
	float u[3];
	float v[3];
	unsigned index;

	/* The whole physical output. */
	compose = server->compose;
	memset(&whole, 0, sizeof(whole));
	whole.extent.width = compose->output.width;
	whole.extent.height = compose->output.height;

	/*
	 * The swapchain's own passes: the clearing one for a whole frame, the
	 * one that keeps the pixels over the rectangles' box otherwise.
	 */
	memset(&clear, 0, sizeof(clear));
	clear.color.float32[3] = 1.0f;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = compose->pass;
	pass.framebuffer = compose->framebuffers[image];
	pass.renderArea = whole;
	pass.clearValueCount = 1U;
	pass.pClearValues = &clear;
	if (count != 0U) {
		pass.renderPass = compose->pass_load;
		pass.renderArea = *area;
	}

	/* The pass begins; nothing is known as bound in it. */
	kwl_list_begin_render_pass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
	kwl_compose_bind_forget(compose);

	/* The whole output placed, the turn image bound, the quad's corners. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)compose->output.width;
	viewport.height = (float)compose->output.height;
	viewport.maxDepth = 1.0f;
	kwl_list_set_viewport(command, 0U, 1U, &viewport);
	kwl_compose_bind(server, command, compose->turn_pipeline, compose->layout, compose->turn.set);
	offset = 0U;
	kwl_list_bind_vertex_buffers(command, 0U, 1U, &compose->corners, &offset);

	/* The texture coordinates of the angle (turn.vert's two rows). */
	kwl_turn_coefficients(compose->turn.rotation, u, v);
	constants[0] = u[0];
	constants[1] = u[1];
	constants[2] = u[2];
	constants[3] = 0.0f;
	constants[4] = v[0];
	constants[5] = v[1];
	constants[6] = v[2];
	constants[7] = 0.0f;
	kwl_list_push_constants(command, compose->layout, VK_SHADER_STAGE_VERTEX_BIT, 0U, sizeof(constants), constants);

	/* A whole frame is one quad over everything. */
	if (count == 0U) {
		kwl_list_set_scissor(command, 0U, 1U, &whole);
		kwl_list_draw(command, 6U, 1U, 0U, 0U);
	}

	/* A partial one draws the quad once in each rectangle, cut to it. */
	for (index = 0U; index < count; index++) {
		kwl_list_set_scissor(command, 0U, 1U, &rects[index]);
		kwl_list_draw(command, 6U, 1U, 0U, 0U);
	}

	/* The swapchain's image is left for presentation; what follows binds its own. */
	kwl_list_end_render_pass(command);
	kwl_compose_bind_forget(compose);
}

/*
 * Makes the turn's two passes and its pipeline the first time an output
 * turns; they follow the output's format, which does not change.
 */
static VkResult
turn_shared(
	struct kwl_compose *compose)
{
	VkResult result;

	/* The pass a whole frame of the turn image is drawn with: cleared, its earlier contents not needed. */
	if (compose->turn_clear_pass == VK_NULL_HANDLE) {
		result = turn_pass(compose, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED, &compose->turn_clear_pass);
		if (result != VK_SUCCESS)
			return result;
	}

	/* The pass a partial frame (or the drawing taken up again) keeps the image's pixels with. */
	if (compose->turn_load_pass == VK_NULL_HANDLE) {
		result = turn_pass(compose, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &compose->turn_load_pass);
		if (result != VK_SUCCESS)
			return result;
	}

	/* The pipeline of the turn pass. */
	if (compose->turn_pipeline == VK_NULL_HANDLE) {
		result = turn_pipeline(compose);
		if (result != VK_SUCCESS)
			return result;
	}

	/* Succeeded: the turn can be drawn. */
	return VK_SUCCESS;
}

/*
 * Makes one of the passes that draw into the turn image: the image is
 * left in the layout the turn pass samples it in, after the turn pass of
 * the frame before has read it, the scene's copies have written it and an
 * earlier pass of the frame has drawn it (the same queue, so they are
 * ordered), and its writes are seen by the turn pass, the scene's copies
 * and the mirror's blit after it.
 */
static VkResult
turn_pass(
	struct kwl_compose *compose,
	VkAttachmentLoadOp load,
	VkImageLayout initial,
	VkRenderPass *pass)
{
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkSubpassDependency dependencies[2];
	VkRenderPassCreateInfo info;
	VkResult result;

	/* One color attachment of the output's format, left to be sampled. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = compose->format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = load;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = initial;
	attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	memset(&reference, 0, sizeof(reference));
	reference.attachment = 0U;
	reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1U;
	subpass.pColorAttachments = &reference;

	/* Written after the last reads and copies of the image. */
	memset(dependencies, 0, sizeof(dependencies));
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0U;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT |
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
	    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	/* Read and copied after the writes. */
	dependencies[1].srcSubpass = 0U;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;

	/* The pass. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	info.attachmentCount = 1U;
	info.pAttachments = &attachment;
	info.subpassCount = 1U;
	info.pSubpasses = &subpass;
	info.dependencyCount = 2U;
	info.pDependencies = dependencies;
	result = vkCreateRenderPass(compose->device, &info, NULL, pass);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the pass is compatible with the output's (same format, one sample). */
	return VK_SUCCESS;
}

/*
 * Makes the turn pass's pipeline: turn.vert places the quad over the
 * whole output and gives it the turned texture coordinates; the window
 * quads' own fragment shader samples the turn image (nearest), with no
 * blending, in the output's pass and the quads' layout.
 */
static VkResult
turn_pipeline(
	struct kwl_compose *compose)
{
	VkShaderModuleCreateInfo module;
	VkShaderModule vertex;
	VkShaderModule fragment;
	VkResult result;

	/* The turn's vertex shader. */
	memset(&module, 0, sizeof(module));
	module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module.codeSize = sizeof(kwl_turn_vert);
	module.pCode = kwl_turn_vert;
	result = vkCreateShaderModule(compose->device, &module, NULL, &vertex);
	if (result != VK_SUCCESS)
		return result;

	/* The quads' fragment shader. */
	module.codeSize = sizeof(kwl_quad_frag);
	module.pCode = kwl_quad_frag;
	result = vkCreateShaderModule(compose->device, &module, NULL, &fragment);
	if (result != VK_SUCCESS) {
		vkDestroyShaderModule(compose->device, vertex, NULL);
		return result;
	}

	/* The opaque pipeline; the modules are needed only while it is made. */
	result = kwl_compose_pipeline_opaque(compose, vertex, fragment, &compose->turn_pipeline);
	vkDestroyShaderModule(compose->device, vertex, NULL);
	vkDestroyShaderModule(compose->device, fragment, NULL);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the turn pass can be drawn. */
	return VK_SUCCESS;
}

/* Makes the turn image in device memory, its view and its framebuffer. */
static VkResult
turn_image(
	struct kwl_compose *compose,
	struct kwl_turn *turn)
{
	VkPhysicalDeviceMemoryProperties memory;
	VkMemoryRequirements requirements;
	VkImageCreateInfo image;
	VkMemoryAllocateInfo allocate;
	VkImageViewCreateInfo view;
	VkFramebufferCreateInfo framebuffer;
	uint32_t index;
	VkResult result;

	/* The image: drawn into, sampled, copied from and into (the scene's cache, the mirror). */
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = compose->format;
	image.extent.width = turn->width;
	image.extent.height = turn->height;
	image.extent.depth = 1U;
	image.mipLevels = 1U;
	image.arrayLayers = 1U;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	result = vkCreateImage(compose->device, &image, NULL, &turn->image);
	if (result != VK_SUCCESS)
		return result;

	/* The device's own memory for it when it has some. */
	vkGetImageMemoryRequirements(compose->device, turn->image, &requirements);
	vkGetPhysicalDeviceMemoryProperties(compose->physical, &memory);
	for (index = 0; index < memory.memoryTypeCount; index++) {
		if ((requirements.memoryTypeBits & (1U << index)) == 0U)
			continue;
		if ((memory.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U)
			break;
	}

	/* Otherwise any type the image takes. */
	if (index == memory.memoryTypeCount) {
		for (index = 0; index < memory.memoryTypeCount; index++) {
			if ((requirements.memoryTypeBits & (1U << index)) != 0U)
				break;
		}
	}

	/* No memory the image takes. */
	if (index == memory.memoryTypeCount)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* The memory, bound. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = index;
	result = vkAllocateMemory(compose->device, &allocate, NULL, &turn->memory);
	if (result != VK_SUCCESS)
		return result;
	result = vkBindImageMemory(compose->device, turn->image, turn->memory, 0U);
	if (result != VK_SUCCESS)
		return result;

	/* Its view. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = turn->image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = compose->format;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	result = vkCreateImageView(compose->device, &view, NULL, &turn->view);
	if (result != VK_SUCCESS)
		return result;

	/* The framebuffer the scene's passes draw into, of the logical size. */
	memset(&framebuffer, 0, sizeof(framebuffer));
	framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer.renderPass = compose->turn_clear_pass;
	framebuffer.attachmentCount = 1U;
	framebuffer.pAttachments = &turn->view;
	framebuffer.width = turn->width;
	framebuffer.height = turn->height;
	framebuffer.layers = 1U;
	result = vkCreateFramebuffer(compose->device, &framebuffer, NULL, &turn->framebuffer);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the image can be drawn into. */
	return VK_SUCCESS;
}

/* Makes the turn image's descriptor set, sampled nearest: each physical pixel takes one logical pixel. */
static VkResult
turn_set(
	struct kwl_compose *compose,
	struct kwl_turn *turn)
{
	VkDescriptorImageInfo image_info;
	VkWriteDescriptorSet write;
	VkResult result;

	/* The set (a spare one when there is one). */
	result = kwl_compose_set_get(compose, &turn->set);
	if (result != VK_SUCCESS)
		return result;

	/* The image, in the layout it is kept in outside its passes, with the nearest sampler. */
	memset(&image_info, 0, sizeof(image_info));
	image_info.sampler = compose->sampler;
	image_info.imageView = turn->view;
	image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = turn->set;
	write.dstBinding = 0U;
	write.descriptorCount = 1U;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image_info;
	vkUpdateDescriptorSets(compose->device, 1U, &write, 0U, NULL);

	/* Succeeded: the turn pass can sample the image. */
	return VK_SUCCESS;
}
