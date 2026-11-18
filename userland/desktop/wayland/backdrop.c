/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The backdrop of the glass (ws035-p057, compositing design D10): what is
 * under a window, blurred, for its title bar's and its panels' frosted
 * glass, in place of the blurred wallpaper alone.
 *
 * Before a window that has something under it is drawn, the frame's pass
 * on the output is ended; the scene under the window (the wallpaper and
 * the windows below, their own glass on the blurred wallpaper) is drawn
 * again into a small image an eighth of the output's size, blurred across
 * and down by more small passes, and the output's pass is taken up
 * again where it was (its pixels loaded).  The window's glass then samples
 * the blurred image instead of the blurred wallpaper (glass_shape_draw).
 *
 * The images are made the first time a frame needs them, not at start-up;
 * a device that cannot make them keeps the blurred wallpaper for good.
 */

#include "compose.h"
#include "glass.h"

#include <stdio.h>
#include <string.h>

/*
 * The backdrop is this many times smaller than the output; it is blurred
 * this many times across and down, a tap this many texels from the next
 * (together about as soft as the blurred wallpaper).
 */
#define BACKDROP_SCALE		8U
#define BACKDROP_ROUNDS		2U
#define BACKDROP_SPREAD		1.5f

/* The state of the backdrop: not tried yet, ready, or not possible on this device. */
#define BACKDROP_UNTRIED	0U
#define BACKDROP_READY		1U
#define BACKDROP_FAILED		2U

static VkResult backdrop_create(struct kwl_compose *compose, uint32_t width, uint32_t height);
static VkResult backdrop_pass(struct kwl_compose *compose);
static VkResult backdrop_target(struct kwl_compose *compose, struct kwl_backdrop_target *target);
static void backdrop_begin_small(struct kwl_server *server, struct kwl_list *command, const struct kwl_backdrop_target *target);
static void backdrop_blur(struct kwl_server *server, struct kwl_list *command, const struct kwl_backdrop_target *from, const struct kwl_backdrop_target *to, float step_x, float step_y);
static void backdrop_viewport(struct kwl_list *command, uint32_t width, uint32_t height);

/*
 * Starts drawing the scene under a window into the backdrop: the output's
 * pass ends, and the small image's begins.  Returns 1 when the caller
 * draws the scene (and then calls kwl_backdrop_end), 0 when there is no
 * backdrop (the glass keeps the blurred wallpaper).
 */
int
kwl_backdrop_begin(
	struct kwl_server *server,
	struct kwl_list *command)
{
	struct kwl_compose *compose;
	struct kwl_backdrop *backdrop;
	VkResult result;

	/* The images, made the first time (a failure is remembered). */
	compose = server->compose;
	backdrop = &compose->backdrop;
	if (backdrop->state == BACKDROP_UNTRIED) {
		result = backdrop_create(compose, compose->target_width / BACKDROP_SCALE, compose->target_height / BACKDROP_SCALE);
		if (result == VK_SUCCESS) {
			backdrop->state = BACKDROP_READY;
			printf("KWL BACKDROP ready width=%u height=%u\n", backdrop->width, backdrop->height);
		} else {
			backdrop->state = BACKDROP_FAILED;
			printf("KWL BACKDROP failed result=%d\n", (int)result);
		}
	}

	/* No backdrop on this device. */
	if (backdrop->state != BACKDROP_READY || compose->framebuffer_now == VK_NULL_HANDLE)
		return 0;

	/* The scene's glass samples the blurred wallpaper while the scene is drawn (not the image being drawn). */
	compose->backdrop_set = VK_NULL_HANDLE;

	/* The output's pass ends, and the scene is drawn small. */
	kwl_batch_flush(server, command);
	kwl_list_end_render_pass(command);
	backdrop_begin_small(server, command, &backdrop->targets[0]);

	/* Succeeded: the caller draws the scene. */
	return 1;
}

/*
 * Ends the scene under a window: it is blurred across and down, the
 * output's pass is taken up again with its pixels, and the glass drawn
 * from now on samples the blurred scene.
 */
void
kwl_backdrop_end(
	struct kwl_server *server,
	struct kwl_list *command)
{
	struct kwl_compose *compose;
	struct kwl_backdrop *backdrop;
	VkRenderPassBeginInfo pass;
	unsigned round;

	/* The scene's pass ends. */
	compose = server->compose;
	backdrop = &compose->backdrop;
	kwl_batch_flush(server, command);
	kwl_list_end_render_pass(command);

	/* Blurred across into the second image, then down back into the first, a few times over. */
	for (round = 0; round < BACKDROP_ROUNDS; round++) {
		backdrop_blur(server, command, &backdrop->targets[0], &backdrop->targets[1], BACKDROP_SPREAD / (float)backdrop->width, 0.0f);
		backdrop_blur(server, command, &backdrop->targets[1], &backdrop->targets[0], 0.0f, BACKDROP_SPREAD / (float)backdrop->height);
	}

	/* The output's pass again, keeping what was drawn, over the frame's own area (its damage's box, ws211-p009). */
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = kwl_compose_target_pass(compose, KWL_PLANE_ANCHOR, 1U);
	pass.framebuffer = compose->framebuffer_now;
	pass.renderArea = compose->pass_area;
	kwl_list_begin_render_pass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
	kwl_compose_bind_forget(server->compose);
	backdrop_viewport(command, compose->target_width, compose->target_height);
	kwl_list_set_scissor(command, 0U, 1U, &compose->scissor_now);

	/* Output-coordinate clipping resumes only after every small-image pass has ended. */
	compose->backdrop_recording = 0U;

	/* The glass from now on is on the blurred scene. */
	compose->backdrop_set = backdrop->targets[0].set;
}

/*
 * Lets the glass drawn from now on (the system bar, the menus) sample the
 * blurred wallpaper again.
 */
void
kwl_backdrop_reset(
	struct kwl_server *server)
{
	/* No blurred scene. */
	server->compose->backdrop_set = VK_NULL_HANDLE;
}

/*
 * Releases the backdrop's images and its own pass (the device is idle).
 */
void
kwl_backdrop_destroy(
	struct kwl_compose *compose)
{
	struct kwl_backdrop *backdrop;
	unsigned index;

	/* Each image, its view, its framebuffer, its memory and its descriptor set. */
	backdrop = &compose->backdrop;
	for (index = 0; index < 2U; index++) {
		if (backdrop->targets[index].framebuffer != VK_NULL_HANDLE)
			vkDestroyFramebuffer(compose->device, backdrop->targets[index].framebuffer, NULL);
		if (backdrop->targets[index].view != VK_NULL_HANDLE)
			vkDestroyImageView(compose->device, backdrop->targets[index].view, NULL);
		if (backdrop->targets[index].image != VK_NULL_HANDLE)
			vkDestroyImage(compose->device, backdrop->targets[index].image, NULL);
		if (backdrop->targets[index].memory != VK_NULL_HANDLE)
			vkFreeMemory(compose->device, backdrop->targets[index].memory, NULL);
		if (backdrop->targets[index].set != VK_NULL_HANDLE)
			kwl_compose_set_put(compose, backdrop->targets[index].set);
	}

	/*
	 * The small images' pass.  The output's loading one is the damage's
	 * and the scene's too, and stays for the compositor's life (it goes
	 * with compose->pass, WS217): the output's format does not change.
	 */
	if (backdrop->pass != VK_NULL_HANDLE)
		vkDestroyRenderPass(compose->device, backdrop->pass, NULL);

	/* Nothing is left, and the next frame may try again. */
	memset(backdrop, 0, sizeof(*backdrop));
	compose->backdrop_set = VK_NULL_HANDLE;
}

/* Makes the passes and the two small images. */
static VkResult
backdrop_create(
	struct kwl_compose *compose,
	uint32_t width,
	uint32_t height)
{
	struct kwl_backdrop *backdrop;
	VkResult result;
	unsigned index;

	/* The size, never empty. */
	backdrop = &compose->backdrop;
	backdrop->width = width;
	backdrop->height = height;
	if (backdrop->width == 0U)
		backdrop->width = 1U;
	if (backdrop->height == 0U)
		backdrop->height = 1U;

	/* The small images' pass, and the output's pass that keeps its pixels. */
	result = backdrop_pass(compose);
	if (result != VK_SUCCESS)
		return result;
	result = kwl_compose_load_pass(compose);
	if (result != VK_SUCCESS)
		return result;

	/* The two images. */
	for (index = 0; index < 2U; index++) {
		result = backdrop_target(compose, &backdrop->targets[index]);
		if (result != VK_SUCCESS)
			return result;
	}

	/* Succeeded: the backdrop can be drawn. */
	return VK_SUCCESS;
}

/*
 * Makes the small images' pass: cleared, drawn, and left to be sampled;
 * it waits for the reads of the image's last use before writing, and its
 * writes are seen by the reads after it.
 */
static VkResult
backdrop_pass(
	struct kwl_compose *compose)
{
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkSubpassDependency dependencies[2];
	VkRenderPassCreateInfo pass;
	VkResult result;

	/* One color attachment of the output's format. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = compose->format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	memset(&reference, 0, sizeof(reference));
	reference.attachment = 0U;
	reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1U;
	subpass.pColorAttachments = &reference;

	/* Written after the last reads of the image. */
	memset(dependencies, 0, sizeof(dependencies));
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0U;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	/* Read after the writes. */
	dependencies[1].srcSubpass = 0U;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	/* The pass. */
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	pass.attachmentCount = 1U;
	pass.pAttachments = &attachment;
	pass.subpassCount = 1U;
	pass.pSubpasses = &subpass;
	pass.dependencyCount = 2U;
	pass.pDependencies = dependencies;
	result = vkCreateRenderPass(compose->device, &pass, NULL, &compose->backdrop.pass);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded. */
	return VK_SUCCESS;
}

/*
 * Makes, the first time, the output's pass that keeps the image's pixels:
 * a frame taken up again after the backdrop, and a frame drawn only in its
 * damage (compose.c).  The image is presented as before.
 */
VkResult
kwl_compose_load_pass(
	struct kwl_compose *compose)
{
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkSubpassDependency dependency;
	VkRenderPassCreateInfo pass;
	VkResult result;

	/* Made already. */
	if (compose->pass_load != VK_NULL_HANDLE)
		return VK_SUCCESS;

	/* The output's color attachment, loaded and presented. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = compose->format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	memset(&reference, 0, sizeof(reference));
	reference.attachment = 0U;
	reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1U;
	subpass.pColorAttachments = &reference;

	/* Written after the earlier writes of the frame. */
	memset(&dependency, 0, sizeof(dependency));
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0U;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	/* The pass. */
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	pass.attachmentCount = 1U;
	pass.pAttachments = &attachment;
	pass.subpassCount = 1U;
	pass.pSubpasses = &subpass;
	pass.dependencyCount = 1U;
	pass.pDependencies = &dependency;
	result = vkCreateRenderPass(compose->device, &pass, NULL, &compose->pass_load);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded. */
	return VK_SUCCESS;
}

/* Makes one small image: device memory, a view, a framebuffer of the small pass, and a linearly sampled descriptor set. */
static VkResult
backdrop_target(
	struct kwl_compose *compose,
	struct kwl_backdrop_target *target)
{
	VkPhysicalDeviceMemoryProperties memory;
	VkMemoryRequirements requirements;
	VkImageCreateInfo image;
	VkMemoryAllocateInfo allocate;
	VkImageViewCreateInfo view;
	VkFramebufferCreateInfo framebuffer;
	VkDescriptorImageInfo image_info;
	VkWriteDescriptorSet write;
	uint32_t index;
	VkResult result;

	/* The image: drawn into and sampled. */
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = compose->format;
	image.extent.width = compose->backdrop.width;
	image.extent.height = compose->backdrop.height;
	image.extent.depth = 1U;
	image.mipLevels = 1U;
	image.arrayLayers = 1U;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	result = vkCreateImage(compose->device, &image, NULL, &target->image);
	if (result != VK_SUCCESS)
		return result;

	/* Device memory for it (any type it takes, the device's own first). */
	vkGetImageMemoryRequirements(compose->device, target->image, &requirements);
	vkGetPhysicalDeviceMemoryProperties(compose->physical, &memory);
	for (index = 0; index < memory.memoryTypeCount; index++) {
		if ((requirements.memoryTypeBits & (1U << index)) != 0U &&
		    (memory.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U)
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
	result = vkAllocateMemory(compose->device, &allocate, NULL, &target->memory);
	if (result != VK_SUCCESS)
		return result;
	result = vkBindImageMemory(compose->device, target->image, target->memory, 0U);
	if (result != VK_SUCCESS)
		return result;

	/* Its view. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = target->image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = compose->format;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	result = vkCreateImageView(compose->device, &view, NULL, &target->view);
	if (result != VK_SUCCESS)
		return result;

	/* The framebuffer of the small pass. */
	memset(&framebuffer, 0, sizeof(framebuffer));
	framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer.renderPass = compose->backdrop.pass;
	framebuffer.attachmentCount = 1U;
	framebuffer.pAttachments = &target->view;
	framebuffer.width = compose->backdrop.width;
	framebuffer.height = compose->backdrop.height;
	framebuffer.layers = 1U;
	result = vkCreateFramebuffer(compose->device, &framebuffer, NULL, &target->framebuffer);
	if (result != VK_SUCCESS)
		return result;

	/* The descriptor set, sampled linearly. */
	result = kwl_compose_set_get(compose, &target->set);
	if (result != VK_SUCCESS)
		return result;
	memset(&image_info, 0, sizeof(image_info));
	image_info.sampler = compose->linear_sampler;
	image_info.imageView = target->view;
	image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = target->set;
	write.dstBinding = 0U;
	write.descriptorCount = 1U;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image_info;
	vkUpdateDescriptorSets(compose->device, 1U, &write, 0U, NULL);

	/* Succeeded. */
	return VK_SUCCESS;
}

/* Begins the small pass on an image, cleared to the background, with a viewport of its size. */
static void
backdrop_begin_small(
	struct kwl_server *server,
	struct kwl_list *command,
	const struct kwl_backdrop_target *target)
{
	struct kwl_backdrop *backdrop;
	VkRenderPassBeginInfo pass;
	VkClearValue clear;
	VkRect2D scissor;

	/* The pass, cleared to the output's background. */
	backdrop = &server->compose->backdrop;
	memset(&clear, 0, sizeof(clear));
	clear.color.float32[0] = KWL_BACKGROUND_RED;
	clear.color.float32[1] = KWL_BACKGROUND_GREEN;
	clear.color.float32[2] = KWL_BACKGROUND_BLUE;
	clear.color.float32[3] = 1.0f;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = backdrop->pass;
	pass.framebuffer = target->framebuffer;
	pass.renderArea.extent.width = backdrop->width;
	pass.renderArea.extent.height = backdrop->height;
	pass.clearValueCount = 1U;
	pass.pClearValues = &clear;
	kwl_list_begin_render_pass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
	kwl_compose_bind_forget(server->compose);

	/* The scene needs all source quads, including those outside the output damage. */
	server->compose->backdrop_recording = 1U;

	/* The output's coordinates land on the small image. */
	backdrop_viewport(command, backdrop->width, backdrop->height);
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent.width = backdrop->width;
	scissor.extent.height = backdrop->height;
	kwl_list_set_scissor(command, 0U, 1U, &scissor);
}

/* Blurs one small image into the other along a direction (a step of one texel across or down). */
static void
backdrop_blur(
	struct kwl_server *server,
	struct kwl_list *command,
	const struct kwl_backdrop_target *from,
	const struct kwl_backdrop_target *to,
	float step_x,
	float step_y)
{
	struct glass_shape shape;
	unsigned layer;

	/* The pass on the target image. */
	backdrop_begin_small(server, command, to);

	/* One shape over all of it, sampling the other image along the step (not moved with a desktop that slides). */
	layer = server->layer_on;
	server->layer_on = 0;
	glass_shape_init(&shape, 0.0f, 0.0f, (float)server->width, (float)server->height);
	shape.mode = MODE_BLUR;
	shape.set = from->set;
	shape.color[0] = step_x;
	shape.color[1] = step_y;
	glass_shape_draw(server, command, &shape);
	server->layer_on = layer;
	kwl_batch_flush(server, command);
	kwl_list_end_render_pass(command);
}

/* Sets a viewport of a size. */
static void
backdrop_viewport(
	struct kwl_list *command,
	uint32_t width,
	uint32_t height)
{
	VkViewport viewport;

	/* The whole target. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)width;
	viewport.height = (float)height;
	viewport.maxDepth = 1.0f;
	kwl_list_set_viewport(command, 0U, 1U, &viewport);
}
