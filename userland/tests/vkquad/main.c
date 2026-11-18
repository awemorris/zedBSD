/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * vkquad: the compositor's textured quad drawn offscreen through the
 * public Vulkan 1.0 API, and read back (ws037-p007 stage D3, test images
 * only).
 *
 * It uses only what the Keiland compositor uses: a 32x32 R8G8B8A8 texture
 * of four coloured quadrants uploaded from a buffer, a 64x64 B8G8R8A8
 * target cleared to blue by its render pass, the compositor's quad
 * shaders (wayland/shaders.h) drawing the texture into the target's middle
 * 32x32 with blending, an image copy of the target's top-left corner into
 * a second image and a clear of that one, and the target read back into a
 * buffer.  The pixels are checked and one line says the result:
 *
 *   VKQUAD ok device=<name> pixels=4096
 *   VKQUAD fail <step> <VkResult or what differs>
 *
 * usage: vkquad [--device-index=N]   (default: the first NVIDIA device, else device 0)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "../../desktop/wayland/shaders.h"

/* The target, the texture and the fence's wait. */
#define QUAD_TARGET		64U
#define QUAD_TEXTURE		32U
#define QUAD_PIXEL		4U
#define QUAD_WAIT_NS		UINT64_C(10000000000)

/* NVIDIA's PCI vendor (the device asked for by default). */
#define QUAD_VENDOR_NVIDIA	0x10deU

/*
 * The colours as a B8G8R8A8 pixel read as a little-endian word (alpha in
 * the top byte, red below it, blue at the bottom): the clear (blue), the
 * texture's quadrants (red, green, white, black) and the corner's clear.
 */
#define QUAD_BLUE		0xff0000ffU
#define QUAD_RED		0xffff0000U
#define QUAD_GREEN		0xff00ff00U
#define QUAD_WHITE		0xffffffffU
#define QUAD_BLACK		0xff000000U

/* The objects of the run, freed together at its end. */
struct quad {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	VkDeviceMemory memory;
	uint8_t *mapping;
	VkImage target;
	VkImage texture;
	VkImage corner;
	VkImageView target_view;
	VkImageView texture_view;
	VkBuffer vertices;
	VkBuffer staging;
	VkBuffer readback;
	VkSampler sampler;
	VkShaderModule vertex_module;
	VkShaderModule fragment_module;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout pipeline_layout;
	VkDescriptorPool pool;
	VkDescriptorSet set;
	VkRenderPass pass;
	VkFramebuffer framebuffer;
	VkPipeline pipeline;
	VkCommandPool command_pool;
	VkCommandBuffer command;
	VkFence fence;
	char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
};

/* Where each resource lies in the one allocation (each on a 4 KiB boundary). */
#define QUAD_TARGET_AT		0x00000U
#define QUAD_TEXTURE_AT		0x10000U
#define QUAD_CORNER_AT		0x14000U
#define QUAD_VERTICES_AT	0x18000U
#define QUAD_STAGING_AT		0x19000U
#define QUAD_READBACK_AT	0x1a000U
#define QUAD_MEMORY_BYTES	0x30000U

static int quad_parse(int argc, char **argv, int *index);
static VkResult quad_device(struct quad *quad, int index);
static VkResult quad_resources(struct quad *quad);
static VkResult quad_image(struct quad *quad, VkFormat format, uint32_t side, VkImageUsageFlags usage, VkDeviceSize at, VkImage *image);
static VkResult quad_buffer(struct quad *quad, VkDeviceSize bytes, VkBufferUsageFlags usage, VkDeviceSize at, VkBuffer *buffer);
static VkResult quad_view(struct quad *quad, VkImage image, VkFormat format, VkImageView *view);
static VkResult quad_pipeline(struct quad *quad);
static VkResult quad_record(struct quad *quad);
static VkResult quad_submit(struct quad *quad);
static int quad_check(const struct quad *quad);
static uint32_t quad_expected(uint32_t x, uint32_t y);
static void quad_fill(struct quad *quad);
static void quad_free(struct quad *quad);

/* Runs the quad and says how it went. */
int
main(
	int argc,
	char **argv)
{
	struct quad quad;
	VkResult status;
	int index;
	int wrong;

	/* The device asked for. */
	index = -1;
	if (quad_parse(argc, argv, &index) != 0) {
		fprintf(stderr, "usage: vkquad [--device-index=N]\n");
		return 2;
	}

	/* The device, the resources, the pipeline, the recording and its submission. */
	memset(&quad, 0, sizeof(quad));
	status = quad_device(&quad, index);
	if (status == VK_SUCCESS)
		status = quad_resources(&quad);
	if (status == VK_SUCCESS)
		status = quad_pipeline(&quad);
	if (status == VK_SUCCESS)
		status = quad_record(&quad);
	if (status == VK_SUCCESS)
		status = quad_submit(&quad);
	if (status != VK_SUCCESS) {
		quad_free(&quad);
		return 1;
	}

	/* The pixels read back. */
	wrong = quad_check(&quad);
	quad_free(&quad);
	if (wrong != 0)
		return 1;

	/* Succeeded: the GPU drew the quad. */
	return 0;
}

/* Reads --device-index=N; -1 asks for the first NVIDIA device. */
static int
quad_parse(
	int argc,
	char **argv,
	int *index)
{
	int argument;

	/* Each argument. */
	for (argument = 1; argument < argc; argument++) {
		if (strncmp(argv[argument], "--device-index=", 15) != 0)
			return -1;
		*index = atoi(argv[argument] + 15);
	}

	/* Succeeded: the arguments are read. */
	return 0;
}

/* Creates the instance, picks the device and creates it with its queue. */
static VkResult
quad_device(
	struct quad *quad,
	int index)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo instance;
	VkPhysicalDevice physicals[8];
	VkPhysicalDeviceProperties properties;
	VkDeviceQueueCreateInfo queue;
	VkDeviceCreateInfo device;
	float priority;
	uint32_t count;
	uint32_t candidate;
	VkResult status;

	/* The instance. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "vkquad";
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&instance, 0, sizeof(instance));
	instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance.pApplicationInfo = &application;
	status = vkCreateInstance(&instance, NULL, &quad->instance);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail instance %d\n", (int)status);
		return status;
	}

	/* The devices. */
	count = 8U;
	status = vkEnumeratePhysicalDevices(quad->instance, &count, physicals);
	if ((status != VK_SUCCESS && status != VK_INCOMPLETE) || count == 0U) {
		printf("VKQUAD fail devices %d count=%u\n", (int)status, count);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* The one asked for, or the first NVIDIA one, or the first. */
	if (index >= 0) {
		if ((uint32_t)index >= count) {
			printf("VKQUAD fail device-index %d of %u\n", index, count);
			return VK_ERROR_INITIALIZATION_FAILED;
		}
		quad->physical = physicals[index];
	} else {
		quad->physical = physicals[0];
		for (candidate = 0U; candidate < count; candidate++) {
			vkGetPhysicalDeviceProperties(physicals[candidate], &properties);
			if (properties.vendorID == QUAD_VENDOR_NVIDIA) {
				quad->physical = physicals[candidate];
				break;
			}
		}
	}
	vkGetPhysicalDeviceProperties(quad->physical, &properties);
	snprintf(quad->name, sizeof(quad->name), "%s", properties.deviceName);

	/* The device with one queue of family 0. */
	priority = 1.0f;
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueCount = 1U;
	queue.pQueuePriorities = &priority;
	memset(&device, 0, sizeof(device));
	device.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device.queueCreateInfoCount = 1U;
	device.pQueueCreateInfos = &queue;
	status = vkCreateDevice(quad->physical, &device, NULL, &quad->device);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail device %d\n", (int)status);
		return status;
	}
	vkGetDeviceQueue(quad->device, 0U, 0U, &quad->queue);

	/* Succeeded: the device is ready. */
	return VK_SUCCESS;
}

/*
 * Allocates the one host-visible allocation and binds the images and
 * buffers into it; fills the texture's upload and the corners.
 */
static VkResult
quad_resources(
	struct quad *quad)
{
	VkMemoryAllocateInfo allocate;
	VkPhysicalDeviceMemoryProperties memory;
	VkSamplerCreateInfo sampler;
	void *mapping;
	uint32_t type;
	VkResult status;

	/* A memory type that is host visible and coherent. */
	vkGetPhysicalDeviceMemoryProperties(quad->physical, &memory);
	for (type = 0U; type < memory.memoryTypeCount; type++) {
		if ((memory.memoryTypes[type].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
		    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
			break;
	}
	if (type == memory.memoryTypeCount) {
		printf("VKQUAD fail memory-type none of %u\n", memory.memoryTypeCount);
		return VK_ERROR_FEATURE_NOT_PRESENT;
	}

	/* The allocation, mapped. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = QUAD_MEMORY_BYTES;
	allocate.memoryTypeIndex = type;
	status = vkAllocateMemory(quad->device, &allocate, NULL, &quad->memory);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail allocate %d\n", (int)status);
		return status;
	}
	status = vkMapMemory(quad->device, quad->memory, 0U, VK_WHOLE_SIZE, 0U, &mapping);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail map %d\n", (int)status);
		return status;
	}
	quad->mapping = mapping;

	/* The images and the buffers. */
	status = quad_image(quad, VK_FORMAT_B8G8R8A8_UNORM, QUAD_TARGET,
			    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, QUAD_TARGET_AT, &quad->target);
	if (status == VK_SUCCESS)
		status = quad_image(quad, VK_FORMAT_R8G8B8A8_UNORM, QUAD_TEXTURE,
				    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, QUAD_TEXTURE_AT, &quad->texture);
	if (status == VK_SUCCESS)
		status = quad_image(quad, VK_FORMAT_B8G8R8A8_UNORM, QUAD_TEXTURE,
				    VK_IMAGE_USAGE_TRANSFER_DST_BIT, QUAD_CORNER_AT, &quad->corner);
	if (status == VK_SUCCESS)
		status = quad_buffer(quad, 48U, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, QUAD_VERTICES_AT, &quad->vertices);
	if (status == VK_SUCCESS)
		status = quad_buffer(quad, QUAD_TEXTURE * QUAD_TEXTURE * QUAD_PIXEL, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, QUAD_STAGING_AT,
				     &quad->staging);
	if (status == VK_SUCCESS)
		status = quad_buffer(quad, QUAD_TARGET * QUAD_TARGET * QUAD_PIXEL, VK_BUFFER_USAGE_TRANSFER_DST_BIT, QUAD_READBACK_AT,
				     &quad->readback);
	if (status == VK_SUCCESS)
		status = quad_view(quad, quad->target, VK_FORMAT_B8G8R8A8_UNORM, &quad->target_view);
	if (status == VK_SUCCESS)
		status = quad_view(quad, quad->texture, VK_FORMAT_R8G8B8A8_UNORM, &quad->texture_view);
	if (status != VK_SUCCESS)
		return status;

	/* A nearest sampler, clamped. */
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_NEAREST;
	sampler.minFilter = VK_FILTER_NEAREST;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	status = vkCreateSampler(quad->device, &sampler, NULL, &quad->sampler);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail sampler %d\n", (int)status);
		return status;
	}

	/* The texture's upload and the corners. */
	quad_fill(quad);

	/* Succeeded: the resources are bound. */
	return VK_SUCCESS;
}

/* Creates a 2D image of one level and binds it at its place. */
static VkResult
quad_image(
	struct quad *quad,
	VkFormat format,
	uint32_t side,
	VkImageUsageFlags usage,
	VkDeviceSize at,
	VkImage *image)
{
	VkImageCreateInfo create;
	VkMemoryRequirements requirements;
	VkResult status;

	/* The image. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.imageType = VK_IMAGE_TYPE_2D;
	create.format = format;
	create.extent.width = side;
	create.extent.height = side;
	create.extent.depth = 1U;
	create.mipLevels = 1U;
	create.arrayLayers = 1U;
	create.samples = VK_SAMPLE_COUNT_1_BIT;
	create.tiling = VK_IMAGE_TILING_OPTIMAL;
	create.usage = usage;
	status = vkCreateImage(quad->device, &create, NULL, image);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail image %d\n", (int)status);
		return status;
	}

	/* Bound at its place. */
	vkGetImageMemoryRequirements(quad->device, *image, &requirements);
	status = vkBindImageMemory(quad->device, *image, quad->memory, at);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail bind-image %d size=%llu\n", (int)status, (unsigned long long)requirements.size);
		return status;
	}

	/* Succeeded: the image has storage. */
	return VK_SUCCESS;
}

/* Creates a buffer and binds it at its place. */
static VkResult
quad_buffer(
	struct quad *quad,
	VkDeviceSize bytes,
	VkBufferUsageFlags usage,
	VkDeviceSize at,
	VkBuffer *buffer)
{
	VkBufferCreateInfo create;
	VkMemoryRequirements requirements;
	VkResult status;

	/* The buffer. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	create.size = bytes;
	create.usage = usage;
	status = vkCreateBuffer(quad->device, &create, NULL, buffer);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail buffer %d\n", (int)status);
		return status;
	}

	/* Bound at its place. */
	vkGetBufferMemoryRequirements(quad->device, *buffer, &requirements);
	status = vkBindBufferMemory(quad->device, *buffer, quad->memory, at);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail bind-buffer %d\n", (int)status);
		return status;
	}

	/* Succeeded: the buffer has storage. */
	return VK_SUCCESS;
}

/* Creates a 2D view of an image's one level. */
static VkResult
quad_view(
	struct quad *quad,
	VkImage image,
	VkFormat format,
	VkImageView *view)
{
	VkImageViewCreateInfo create;
	VkResult status;

	/* The view. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create.image = image;
	create.viewType = VK_IMAGE_VIEW_TYPE_2D;
	create.format = format;
	create.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	create.subresourceRange.levelCount = 1U;
	create.subresourceRange.layerCount = 1U;
	status = vkCreateImageView(quad->device, &create, NULL, view);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail view %d\n", (int)status);
		return status;
	}

	/* Succeeded: the view exists. */
	return VK_SUCCESS;
}

/*
 * Creates the compositor's quad pipeline: its two modules, a set of one
 * combined image sampler (the texture), the push constants, a pass that
 * clears its one attachment, the framebuffer and the pipeline (dynamic
 * viewport and scissor, blending one, one minus source alpha).
 */
static VkResult
quad_pipeline(
	struct quad *quad)
{
	VkShaderModuleCreateInfo module;
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo set_layout;
	VkPushConstantRange range;
	VkPipelineLayoutCreateInfo pipeline_layout;
	VkDescriptorPoolSize size;
	VkDescriptorPoolCreateInfo pool;
	VkDescriptorSetAllocateInfo allocate;
	VkDescriptorImageInfo image;
	VkWriteDescriptorSet write;
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkRenderPassCreateInfo pass;
	VkFramebufferCreateInfo framebuffer;
	VkPipelineShaderStageCreateInfo stages[2];
	VkVertexInputBindingDescription vertex_binding;
	VkVertexInputAttributeDescription vertex_attribute;
	VkPipelineVertexInputStateCreateInfo vertex_input;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineColorBlendAttachmentState blend_attachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkDynamicState states[2];
	VkPipelineDynamicStateCreateInfo dynamic;
	VkGraphicsPipelineCreateInfo create;
	VkResult status;

	/* The two modules. */
	memset(&module, 0, sizeof(module));
	module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module.codeSize = sizeof(kwl_quad_vert);
	module.pCode = kwl_quad_vert;
	status = vkCreateShaderModule(quad->device, &module, NULL, &quad->vertex_module);
	if (status == VK_SUCCESS) {
		module.codeSize = sizeof(kwl_quad_frag);
		module.pCode = kwl_quad_frag;
		status = vkCreateShaderModule(quad->device, &module, NULL, &quad->fragment_module);
	}
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail module %d\n", (int)status);
		return status;
	}

	/* The set's layout, the pipeline's layout (two vec4 of push constants), the pool and the set. */
	memset(&binding, 0, sizeof(binding));
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1U;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	memset(&set_layout, 0, sizeof(set_layout));
	set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout.bindingCount = 1U;
	set_layout.pBindings = &binding;
	status = vkCreateDescriptorSetLayout(quad->device, &set_layout, NULL, &quad->set_layout);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail set-layout %d\n", (int)status);
		return status;
	}
	memset(&range, 0, sizeof(range));
	range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	range.size = 32U;
	memset(&pipeline_layout, 0, sizeof(pipeline_layout));
	pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout.setLayoutCount = 1U;
	pipeline_layout.pSetLayouts = &quad->set_layout;
	pipeline_layout.pushConstantRangeCount = 1U;
	pipeline_layout.pPushConstantRanges = &range;
	status = vkCreatePipelineLayout(quad->device, &pipeline_layout, NULL, &quad->pipeline_layout);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail pipeline-layout %d\n", (int)status);
		return status;
	}
	memset(&size, 0, sizeof(size));
	size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	size.descriptorCount = 1U;
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.maxSets = 1U;
	pool.poolSizeCount = 1U;
	pool.pPoolSizes = &size;
	status = vkCreateDescriptorPool(quad->device, &pool, NULL, &quad->pool);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail pool %d\n", (int)status);
		return status;
	}
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = quad->pool;
	allocate.descriptorSetCount = 1U;
	allocate.pSetLayouts = &quad->set_layout;
	status = vkAllocateDescriptorSets(quad->device, &allocate, &quad->set);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail set %d\n", (int)status);
		return status;
	}
	memset(&image, 0, sizeof(image));
	image.sampler = quad->sampler;
	image.imageView = quad->texture_view;
	image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = quad->set;
	write.descriptorCount = 1U;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image;
	vkUpdateDescriptorSets(quad->device, 1U, &write, 0U, NULL);

	/* The pass: one B8G8R8A8 attachment cleared and kept. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = VK_FORMAT_B8G8R8A8_UNORM;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	memset(&reference, 0, sizeof(reference));
	reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1U;
	subpass.pColorAttachments = &reference;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	pass.attachmentCount = 1U;
	pass.pAttachments = &attachment;
	pass.subpassCount = 1U;
	pass.pSubpasses = &subpass;
	status = vkCreateRenderPass(quad->device, &pass, NULL, &quad->pass);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail render-pass %d\n", (int)status);
		return status;
	}
	memset(&framebuffer, 0, sizeof(framebuffer));
	framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer.renderPass = quad->pass;
	framebuffer.attachmentCount = 1U;
	framebuffer.pAttachments = &quad->target_view;
	framebuffer.width = QUAD_TARGET;
	framebuffer.height = QUAD_TARGET;
	framebuffer.layers = 1U;
	status = vkCreateFramebuffer(quad->device, &framebuffer, NULL, &quad->framebuffer);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail framebuffer %d\n", (int)status);
		return status;
	}

	/* The pipeline. */
	memset(stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = quad->vertex_module;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = quad->fragment_module;
	stages[1].pName = "main";
	memset(&vertex_binding, 0, sizeof(vertex_binding));
	vertex_binding.stride = 8U;
	memset(&vertex_attribute, 0, sizeof(vertex_attribute));
	vertex_attribute.format = VK_FORMAT_R32G32_SFLOAT;
	memset(&vertex_input, 0, sizeof(vertex_input));
	vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input.vertexBindingDescriptionCount = 1U;
	vertex_input.pVertexBindingDescriptions = &vertex_binding;
	vertex_input.vertexAttributeDescriptionCount = 1U;
	vertex_input.pVertexAttributeDescriptions = &vertex_attribute;
	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	memset(&viewport, 0, sizeof(viewport));
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1U;
	viewport.scissorCount = 1U;
	memset(&raster, 0, sizeof(raster));
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.lineWidth = 1.0f;
	memset(&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	memset(&blend_attachment, 0, sizeof(blend_attachment));
	blend_attachment.blendEnable = VK_TRUE;
	blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
	blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
	blend_attachment.colorWriteMask = 0xfU;
	memset(&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1U;
	blend.pAttachments = &blend_attachment;
	states[0] = VK_DYNAMIC_STATE_VIEWPORT;
	states[1] = VK_DYNAMIC_STATE_SCISSOR;
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = 2U;
	dynamic.pDynamicStates = states;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	create.stageCount = 2U;
	create.pStages = stages;
	create.pVertexInputState = &vertex_input;
	create.pInputAssemblyState = &assembly;
	create.pViewportState = &viewport;
	create.pRasterizationState = &raster;
	create.pMultisampleState = &multisample;
	create.pColorBlendState = &blend;
	create.pDynamicState = &dynamic;
	create.layout = quad->pipeline_layout;
	create.renderPass = quad->pass;
	create.basePipelineIndex = -1;
	status = vkCreateGraphicsPipelines(quad->device, VK_NULL_HANDLE, 1U, &create, NULL, &quad->pipeline);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail pipeline %d\n", (int)status);
		return status;
	}

	/* Succeeded: the pipeline is compiled. */
	return VK_SUCCESS;
}

/*
 * Records the command buffer: the texture's upload, the pass drawing the
 * quad over the middle, the corner's copy and clear, the readback.
 */
static VkResult
quad_record(
	struct quad *quad)
{
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo allocate;
	VkCommandBufferBeginInfo begin;
	VkBufferImageCopy upload;
	VkRenderPassBeginInfo pass;
	VkClearValue clear;
	VkClearColorValue corner_clear;
	VkImageSubresourceRange whole;
	VkViewport viewport;
	VkRect2D scissor;
	VkImageCopy copy;
	VkDeviceSize offset;
	float push[8];
	VkResult status;

	/* The pool and the command buffer. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	status = vkCreateCommandPool(quad->device, &pool, NULL, &quad->command_pool);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail command-pool %d\n", (int)status);
		return status;
	}
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate.commandPool = quad->command_pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1U;
	status = vkAllocateCommandBuffers(quad->device, &allocate, &quad->command);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail command-buffer %d\n", (int)status);
		return status;
	}
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	status = vkBeginCommandBuffer(quad->command, &begin);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail begin %d\n", (int)status);
		return status;
	}

	/* The texture from the staging buffer. */
	memset(&upload, 0, sizeof(upload));
	upload.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	upload.imageSubresource.layerCount = 1U;
	upload.imageExtent.width = QUAD_TEXTURE;
	upload.imageExtent.height = QUAD_TEXTURE;
	upload.imageExtent.depth = 1U;
	vkCmdCopyBufferToImage(quad->command, quad->staging, quad->texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1U, &upload);

	/* The pass: cleared blue, the quad over the middle 32x32 showing the whole texture. */
	memset(&clear, 0, sizeof(clear));
	clear.color.float32[2] = 1.0f;
	clear.color.float32[3] = 1.0f;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = quad->pass;
	pass.framebuffer = quad->framebuffer;
	pass.renderArea.extent.width = QUAD_TARGET;
	pass.renderArea.extent.height = QUAD_TARGET;
	pass.clearValueCount = 1U;
	pass.pClearValues = &clear;
	vkCmdBeginRenderPass(quad->command, &pass, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(quad->command, VK_PIPELINE_BIND_POINT_GRAPHICS, quad->pipeline);
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)QUAD_TARGET;
	viewport.height = (float)QUAD_TARGET;
	viewport.maxDepth = 1.0f;
	vkCmdSetViewport(quad->command, 0U, 1U, &viewport);
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent.width = QUAD_TARGET;
	scissor.extent.height = QUAD_TARGET;
	vkCmdSetScissor(quad->command, 0U, 1U, &scissor);
	vkCmdBindDescriptorSets(quad->command, VK_PIPELINE_BIND_POINT_GRAPHICS, quad->pipeline_layout, 0U, 1U, &quad->set, 0U, NULL);
	offset = 0U;
	vkCmdBindVertexBuffers(quad->command, 0U, 1U, &quad->vertices, &offset);
	push[0] = -0.5f;
	push[1] = -0.5f;
	push[2] = 0.5f;
	push[3] = 0.5f;
	push[4] = 0.0f;
	push[5] = 0.0f;
	push[6] = 1.0f;
	push[7] = 1.0f;
	vkCmdPushConstants(quad->command, quad->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0U, sizeof(push), push);
	vkCmdDraw(quad->command, 6U, 1U, 0U, 0U);
	vkCmdEndRenderPass(quad->command);

	/* The target's top-left 16x16 into the corner image, after the corner is cleared black. */
	memset(&corner_clear, 0, sizeof(corner_clear));
	corner_clear.float32[3] = 1.0f;
	memset(&whole, 0, sizeof(whole));
	whole.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	whole.levelCount = 1U;
	whole.layerCount = 1U;
	vkCmdClearColorImage(quad->command, quad->corner, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &corner_clear, 1U, &whole);
	memset(&copy, 0, sizeof(copy));
	copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.srcSubresource.layerCount = 1U;
	copy.dstSubresource = copy.srcSubresource;
	copy.extent.width = 16U;
	copy.extent.height = 16U;
	copy.extent.depth = 1U;
	vkCmdCopyImage(quad->command, quad->target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, quad->corner, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		       1U, &copy);

	/* The target read back, rows of 64 pixels. */
	upload.imageExtent.width = QUAD_TARGET;
	upload.imageExtent.height = QUAD_TARGET;
	vkCmdCopyImageToBuffer(quad->command, quad->target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, quad->readback, 1U, &upload);

	/* The end. */
	status = vkEndCommandBuffer(quad->command);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail end %d\n", (int)status);
		return status;
	}

	/* Succeeded: the command buffer is recorded. */
	return VK_SUCCESS;
}

/* Submits the command buffer with a fence and waits for it. */
static VkResult
quad_submit(
	struct quad *quad)
{
	VkFenceCreateInfo fence;
	VkSubmitInfo submit;
	VkResult status;

	/* The fence. */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	status = vkCreateFence(quad->device, &fence, NULL, &quad->fence);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail fence %d\n", (int)status);
		return status;
	}

	/* The submission and the wait. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &quad->command;
	status = vkQueueSubmit(quad->queue, 1U, &submit, quad->fence);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail submit %d\n", (int)status);
		return status;
	}
	status = vkWaitForFences(quad->device, 1U, &quad->fence, VK_TRUE, QUAD_WAIT_NS);
	if (status != VK_SUCCESS) {
		printf("VKQUAD fail wait %d\n", (int)status);
		return status;
	}

	/* Succeeded: the GPU has done the work. */
	return VK_SUCCESS;
}

/*
 * Checks every read-back pixel against the picture (and the corner
 * image's 16x16 against the target's); says the first wrong one.
 */
static int
quad_check(
	const struct quad *quad)
{
	const uint8_t *readback;
	const uint8_t *corner;
	uint32_t pixel;
	uint32_t want;
	uint32_t wrong;
	uint32_t x;
	uint32_t y;

	/* The target. */
	readback = quad->mapping + QUAD_READBACK_AT;
	wrong = 0U;
	for (y = 0U; y < QUAD_TARGET; y++) {
		for (x = 0U; x < QUAD_TARGET; x++) {
			memcpy(&pixel, readback + (y * QUAD_TARGET + x) * QUAD_PIXEL, sizeof(pixel));
			want = quad_expected(x, y);
			if (pixel != want) {
				if (wrong == 0U)
					printf("VKQUAD fail pixel (%u,%u) is %08x want %08x\n", x, y, pixel, want);
				wrong++;
			}
		}
	}

	/* The corner image (rows of 256 bytes: 16 pixels of 4 bytes on the 256 boundary at least) is the target's corner, blue. */
	corner = quad->mapping + QUAD_CORNER_AT;
	memcpy(&pixel, corner, sizeof(pixel));
	if (wrong == 0U && pixel != QUAD_BLUE) {
		printf("VKQUAD fail corner (0,0) is %08x want %08x\n", pixel, QUAD_BLUE);
		wrong++;
	}
	memcpy(&pixel, corner + 20U * 4U, sizeof(pixel));
	if (wrong == 0U && pixel != QUAD_BLACK) {
		printf("VKQUAD fail corner (20,0) is %08x want %08x (the clear)\n", pixel, QUAD_BLACK);
		wrong++;
	}

	/* Reports the wrong pixels. */
	if (wrong != 0U) {
		printf("VKQUAD fail pixels wrong=%u of %u\n", wrong, QUAD_TARGET * QUAD_TARGET);
		return 1;
	}

	/* Succeeded: every pixel is right. */
	printf("VKQUAD ok device=%s pixels=%u\n", quad->name, QUAD_TARGET * QUAD_TARGET);
	return 0;
}

/*
 * Reports the pixel the picture has at (x, y) as B8G8R8A8 read as a
 * little-endian word: blue outside the middle 32x32, the texture's
 * quadrant (red, green, white, black) inside.
 */
static uint32_t
quad_expected(
	uint32_t x,
	uint32_t y)
{
	uint32_t u;
	uint32_t v;

	/* Outside the quad: the clear. */
	if (x < 16U || x >= 48U || y < 16U || y >= 48U)
		return QUAD_BLUE;

	/* Inside: the quadrant of the texture the pixel shows. */
	u = x - 16U;
	v = y - 16U;
	if (u < 16U && v < 16U)
		return QUAD_RED;
	if (u >= 16U && v < 16U)
		return QUAD_GREEN;
	if (u < 16U)
		return QUAD_WHITE;

	/* Succeeded: the last quadrant. */
	return QUAD_BLACK;
}

/*
 * Fills the staging buffer with the texture (R8G8B8A8: red, green, white
 * and black quadrants) and the vertex buffer with the quad's corners.
 */
static void
quad_fill(
	struct quad *quad)
{
	static const float corners[12] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
	uint8_t *texel;
	uint32_t x;
	uint32_t y;

	/* The quadrants, byte by byte: R, G, B, A. */
	for (y = 0U; y < QUAD_TEXTURE; y++) {
		for (x = 0U; x < QUAD_TEXTURE; x++) {
			texel = quad->mapping + QUAD_STAGING_AT + (y * QUAD_TEXTURE + x) * QUAD_PIXEL;
			texel[0] = 0x00U;
			texel[1] = 0x00U;
			texel[2] = 0x00U;
			texel[3] = 0xffU;
			if (x < 16U && y < 16U) {
				texel[0] = 0xffU;
			} else if (y < 16U) {
				texel[1] = 0xffU;
			} else if (x < 16U) {
				texel[0] = 0xffU;
				texel[1] = 0xffU;
				texel[2] = 0xffU;
			}
		}
	}

	/* The corners. */
	memcpy(quad->mapping + QUAD_VERTICES_AT, corners, sizeof(corners));
}

/* Destroys what was made, in reverse. */
static void
quad_free(
	struct quad *quad)
{
	/* Waits for the device before anything goes. */
	if (quad->device != VK_NULL_HANDLE)
		(void)vkDeviceWaitIdle(quad->device);

	/* Each object made. */
	if (quad->fence != VK_NULL_HANDLE)
		vkDestroyFence(quad->device, quad->fence, NULL);
	if (quad->command_pool != VK_NULL_HANDLE)
		vkDestroyCommandPool(quad->device, quad->command_pool, NULL);
	if (quad->pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(quad->device, quad->pipeline, NULL);
	if (quad->framebuffer != VK_NULL_HANDLE)
		vkDestroyFramebuffer(quad->device, quad->framebuffer, NULL);
	if (quad->pass != VK_NULL_HANDLE)
		vkDestroyRenderPass(quad->device, quad->pass, NULL);
	if (quad->pool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(quad->device, quad->pool, NULL);
	if (quad->pipeline_layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(quad->device, quad->pipeline_layout, NULL);
	if (quad->set_layout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(quad->device, quad->set_layout, NULL);
	if (quad->fragment_module != VK_NULL_HANDLE)
		vkDestroyShaderModule(quad->device, quad->fragment_module, NULL);
	if (quad->vertex_module != VK_NULL_HANDLE)
		vkDestroyShaderModule(quad->device, quad->vertex_module, NULL);
	if (quad->sampler != VK_NULL_HANDLE)
		vkDestroySampler(quad->device, quad->sampler, NULL);
	if (quad->texture_view != VK_NULL_HANDLE)
		vkDestroyImageView(quad->device, quad->texture_view, NULL);
	if (quad->target_view != VK_NULL_HANDLE)
		vkDestroyImageView(quad->device, quad->target_view, NULL);
	if (quad->readback != VK_NULL_HANDLE)
		vkDestroyBuffer(quad->device, quad->readback, NULL);
	if (quad->staging != VK_NULL_HANDLE)
		vkDestroyBuffer(quad->device, quad->staging, NULL);
	if (quad->vertices != VK_NULL_HANDLE)
		vkDestroyBuffer(quad->device, quad->vertices, NULL);
	if (quad->corner != VK_NULL_HANDLE)
		vkDestroyImage(quad->device, quad->corner, NULL);
	if (quad->texture != VK_NULL_HANDLE)
		vkDestroyImage(quad->device, quad->texture, NULL);
	if (quad->target != VK_NULL_HANDLE)
		vkDestroyImage(quad->device, quad->target, NULL);
	if (quad->memory != VK_NULL_HANDLE)
		vkFreeMemory(quad->device, quad->memory, NULL);
	if (quad->device != VK_NULL_HANDLE)
		vkDestroyDevice(quad->device, NULL);
	if (quad->instance != VK_NULL_HANDLE)
		vkDestroyInstance(quad->instance, NULL);
}
