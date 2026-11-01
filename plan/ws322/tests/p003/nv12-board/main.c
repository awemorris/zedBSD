/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * nv12-board: the board check of the PowerVR executor's NV12 images, as a
 * video decoder and Video Player use them.
 *
 * Device A (the decoder's side) makes a 64x64 NV12 image (OPTIMAL,
 * SAMPLED | TRANSFER_SRC, MUTABLE_FORMAT | EXTENDED_USAGE, external
 * memory), binds exportable host visible memory, writes a pattern into
 * both planes at the offsets and rows vkGetImageSubresourceLayout gives,
 * samples the luma plane through an R8 view and the chroma plane through
 * an R8G8 view into an RGBA8 target (red = Y, green = Cb, blue = Cr) and
 * copies the target and both planes (PLANE_0, PLANE_1) out.  Device B, of
 * an instance of its own (another open of the render node, as Video
 * Player's beside libmedia's), imports A's memory by an opaque fd into an
 * NV12 image made as Video Player makes it (SAMPLED only) and samples it
 * the same way.  Every pixel and plane texel is compared with the pattern.
 * It also reports whether the device offers video (the queue family's
 * flags and the video extensions), which it does not without a video
 * engine.  It prints lines ending with one:
 *
 *   nv12-board: PASS
 *   nv12-board: FAIL <what> <value>
 *
 * The exit status is 0 only on PASS.
 */

#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "planes-spv.h"

/* The picture's edges, and the target's (the same). */
#define SIDE			64U
#define CHROMA_SIDE		(SIDE / 2U)

/* The Vulkan objects of one side (device) of the check; every handle starts null. */
struct side {
	const char *name;
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	VkCommandPool command_pool;
	VkCommandBuffer command;
	VkFence fence;
	VkImage picture;
	VkDeviceMemory picture_memory;
	VkImageView luma;
	VkImageView chroma;
	VkImage target;
	VkDeviceMemory target_memory;
	VkImageView target_view;
	VkBuffer readback;
	VkDeviceMemory readback_memory;
	uint8_t *readback_bytes;
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	VkSampler sampler;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout pipeline_layout;
	VkDescriptorPool pool;
	VkDescriptorSet set;
	VkRenderPass render_pass;
	VkFramebuffer framebuffer;
	VkShaderModule modules[2];
	VkPipeline pipeline;
};

/* One corner of the quad: its position and the picture's coordinate there. */
struct corner {
	float position[2];
	float coordinate[2];
};

/* The readback buffer: the target's RGBA8 pixels, the luma plane's texels, the chroma plane's pairs. */
#define READBACK_TARGET		0U
#define READBACK_LUMA		(SIDE * SIDE * 4U)
#define READBACK_CHROMA		(READBACK_LUMA + SIDE * SIDE)
#define READBACK_BYTES		(READBACK_CHROMA + CHROMA_SIDE * CHROMA_SIDE * 2U)

/* The first failure, reported by the last line. */
static const char *failed_what;
static long failed_value;

static int fail(const char *what, long value);
static int open_side(struct side *side, const char *name);
static int find_memory(struct side *side, uint32_t allowed, VkMemoryPropertyFlags wanted, uint32_t *type);
static int make_picture(struct side *side, VkImageUsageFlags usage);
static int make_views(struct side *side);
static int make_buffer(struct side *side, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buffer, VkDeviceMemory *memory);
static void picture_barrier(struct side *side, VkImageLayout from, VkImageLayout to, VkAccessFlags source_access, VkAccessFlags destination_access, VkPipelineStageFlags source_stage, VkPipelineStageFlags destination_stage);
static int fill_picture(struct side *side);
static int make_target(struct side *side);
static int make_pipeline(struct side *side);
static int draw(struct side *side, int copy_planes);
static int check_target(struct side *side);
static int check_planes(struct side *side);
static int check_no_video(struct side *side);
static void destroy(struct side *side);
static uint8_t pattern_luma(uint32_t x, uint32_t y);
static uint8_t pattern_cb(uint32_t x, uint32_t y);
static uint8_t pattern_cr(uint32_t x, uint32_t y);

/* Runs the check on both sides and prints the result line. */
int
main(
	void)
{
	PFN_vkGetMemoryFdKHR get_memory_fd;
	VkExportMemoryAllocateInfo export_info;
	VkMemoryGetFdInfoKHR get;
	VkImportMemoryFdInfoKHR import;
	VkMemoryAllocateInfo allocate;
	VkMemoryRequirements requirements;
	VkMemoryRequirements exported_requirements;
	struct side a;
	struct side b;
	uint32_t type;
	VkResult status;
	int fd;
	int error;

	/* Side A: the decoder's image, filled by the host, sampled and copied out. */
	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	error = open_side(&a, "A");
	if (error == 0)
		error = check_no_video(&a);
	if (error == 0)
		error = make_picture(&a, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	if (error == 0) {
		vkGetImageMemoryRequirements(a.device, a.picture, &exported_requirements);
		error = find_memory(&a, exported_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &type);
	}
	if (error == 0) {
		memset(&export_info, 0, sizeof(export_info));
		export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
		export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		memset(&allocate, 0, sizeof(allocate));
		allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocate.pNext = &export_info;
		allocate.allocationSize = exported_requirements.size;
		allocate.memoryTypeIndex = type;
		status = vkAllocateMemory(a.device, &allocate, NULL, &a.picture_memory);
		if (status != VK_SUCCESS)
			error = fail("A vkAllocateMemory export", status);
	}
	if (error == 0) {
		status = vkBindImageMemory(a.device, a.picture, a.picture_memory, 0);
		if (status != VK_SUCCESS)
			error = fail("A vkBindImageMemory", status);
	}
	if (error == 0)
		error = fill_picture(&a);
	if (error == 0)
		error = make_target(&a);
	if (error == 0)
		error = make_pipeline(&a);
	if (error == 0)
		error = draw(&a, 1);
	if (error == 0)
		error = check_target(&a);
	if (error == 0)
		error = check_planes(&a);
	if (error == 0)
		printf("nv12-board: A sampled and copied out: ok\n");

	/* The memory's opaque fd. */
	fd = -1;
	if (error == 0) {
		get_memory_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(a.device, "vkGetMemoryFdKHR");
		if (get_memory_fd == NULL)
			error = fail("vkGetMemoryFdKHR missing", 0);
	}
	if (error == 0) {
		memset(&get, 0, sizeof(get));
		get.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
		get.memory = a.picture_memory;
		get.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		status = get_memory_fd(a.device, &get, &fd);
		if (status != VK_SUCCESS)
			error = fail("A vkGetMemoryFdKHR", status);
	}

	/* Side B: Video Player's import of the same memory, sampled. */
	if (error == 0)
		error = open_side(&b, "B");
	if (error == 0)
		error = make_picture(&b, VK_IMAGE_USAGE_SAMPLED_BIT);
	if (error == 0) {
		vkGetImageMemoryRequirements(b.device, b.picture, &requirements);
		if (requirements.size != exported_requirements.size)
			error = fail("B's import is laid out as A's image", (long)requirements.size);
	}
	if (error == 0) {
		memset(&import, 0, sizeof(import));
		import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
		import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		import.fd = fd;
		memset(&allocate, 0, sizeof(allocate));
		allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocate.pNext = &import;
		allocate.allocationSize = exported_requirements.size;
		allocate.memoryTypeIndex = type;
		status = vkAllocateMemory(b.device, &allocate, NULL, &b.picture_memory);
		if (status != VK_SUCCESS)
			error = fail("B vkAllocateMemory import", status);
		else
			fd = -1;
	}
	if (error == 0) {
		status = vkBindImageMemory(b.device, b.picture, b.picture_memory, 0);
		if (status != VK_SUCCESS)
			error = fail("B vkBindImageMemory", status);
	}
	if (error == 0)
		error = make_target(&b);
	if (error == 0)
		error = make_pipeline(&b);
	if (error == 0)
		error = draw(&b, 0);
	if (error == 0)
		error = check_target(&b);
	if (error == 0)
		printf("nv12-board: B imported and sampled: ok\n");

	/* Everything goes, then the verdict. */
	if (fd >= 0)
		close(fd);
	destroy(&b);
	destroy(&a);
	if (error != 0) {
		printf("nv12-board: FAIL %s %ld\n", failed_what, failed_value);
		return 1;
	}
	printf("nv12-board: PASS\n");
	return 0;
}

/* Records the first failure; returns 1. */
static int
fail(
	const char *what,
	long value)
{
	if (failed_what == NULL) {
		failed_what = what;
		failed_value = value;
	}
	return 1;
}

/* The pattern's luma at a texel. */
static uint8_t
pattern_luma(
	uint32_t x,
	uint32_t y)
{
	return (uint8_t)(x * 3U + y * 5U + 7U);
}

/* The pattern's Cb at a chroma texel. */
static uint8_t
pattern_cb(
	uint32_t x,
	uint32_t y)
{
	return (uint8_t)(x * 7U + y + 11U);
}

/* The pattern's Cr at a chroma texel. */
static uint8_t
pattern_cr(
	uint32_t x,
	uint32_t y)
{
	return (uint8_t)(255U - x - y * 6U);
}

/* Opens an instance of its own, the first device, its graphics family's queue, a command buffer and a fence. */
static int
open_side(
	struct side *side,
	const char *name)
{
	static const char *const extensions[] = { "VK_KHR_external_memory", "VK_KHR_external_memory_fd" };
	VkApplicationInfo application;
	VkInstanceCreateInfo instance_info;
	VkQueueFamilyProperties families[8];
	VkDeviceQueueCreateInfo queue_info;
	VkDeviceCreateInfo device_info;
	VkCommandPoolCreateInfo pool_info;
	VkCommandBufferAllocateInfo command_info;
	VkFenceCreateInfo fence_info;
	uint32_t count;
	uint32_t index;
	float priority;
	VkResult status;

	side->name = name;
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "nv12-board";
	application.apiVersion = VK_MAKE_VERSION(1, 1, 0);
	memset(&instance_info, 0, sizeof(instance_info));
	instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info.pApplicationInfo = &application;
	status = vkCreateInstance(&instance_info, NULL, &side->instance);
	if (status != VK_SUCCESS)
		return fail("vkCreateInstance", status);
	count = 1;
	status = vkEnumeratePhysicalDevices(side->instance, &count, &side->physical);
	if ((status != VK_SUCCESS && status != VK_INCOMPLETE) || count == 0)
		return fail("vkEnumeratePhysicalDevices", status);

	/* The first family with graphics. */
	count = sizeof(families) / sizeof(families[0]);
	vkGetPhysicalDeviceQueueFamilyProperties(side->physical, &count, families);
	for (index = 0; index < count; index++) {
		if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
			break;
	}
	if (index == count)
		return fail("graphics family", (long)count);
	side->family = index;

	/* The device with external memory by fd. */
	priority = 1.0f;
	memset(&queue_info, 0, sizeof(queue_info));
	queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = side->family;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &priority;
	memset(&device_info, 0, sizeof(device_info));
	device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue_info;
	device_info.enabledExtensionCount = 2;
	device_info.ppEnabledExtensionNames = extensions;
	status = vkCreateDevice(side->physical, &device_info, NULL, &side->device);
	if (status != VK_SUCCESS)
		return fail("vkCreateDevice with external memory fd", status);
	vkGetDeviceQueue(side->device, side->family, 0, &side->queue);

	/* The command buffer and the fence. */
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = side->family;
	status = vkCreateCommandPool(side->device, &pool_info, NULL, &side->command_pool);
	if (status != VK_SUCCESS)
		return fail("vkCreateCommandPool", status);
	memset(&command_info, 0, sizeof(command_info));
	command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command_info.commandPool = side->command_pool;
	command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command_info.commandBufferCount = 1;
	status = vkAllocateCommandBuffers(side->device, &command_info, &side->command);
	if (status != VK_SUCCESS)
		return fail("vkAllocateCommandBuffers", status);
	memset(&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	status = vkCreateFence(side->device, &fence_info, NULL, &side->fence);
	if (status != VK_SUCCESS)
		return fail("vkCreateFence", status);
	return 0;
}

/* Reports (and requires) that the device offers no video: no video queue flag, no video extension. */
static int
check_no_video(
	struct side *side)
{
	VkQueueFamilyProperties families[8];
	VkExtensionProperties extensions[64];
	uint32_t count;
	uint32_t family_count;
	uint32_t index;
	uint32_t video_flags;
	int video_extensions;

	count = sizeof(families) / sizeof(families[0]);
	vkGetPhysicalDeviceQueueFamilyProperties(side->physical, &count, families);
	video_flags = 0;
	for (index = 0; index < count; index++)
		video_flags |= families[index].queueFlags & (VK_QUEUE_VIDEO_DECODE_BIT_KHR | 0x40U);
	family_count = count;
	count = sizeof(extensions) / sizeof(extensions[0]);
	(void)vkEnumerateDeviceExtensionProperties(side->physical, NULL, &count, extensions);
	video_extensions = 0;
	for (index = 0; index < count; index++) {
		if (strncmp(extensions[index].extensionName, "VK_KHR_video", 12) == 0)
			video_extensions++;
	}
	printf("nv12-board: families %u, video queue flags 0x%x, video extensions %d\n", (unsigned)family_count, (unsigned)video_flags, video_extensions);
	if (video_flags != 0 || video_extensions != 0)
		return fail("video offered without an engine", (long)video_flags);
	return 0;
}

/* Finds a memory type in allowed with every wanted property. */
static int
find_memory(
	struct side *side,
	uint32_t allowed,
	VkMemoryPropertyFlags wanted,
	uint32_t *type)
{
	VkPhysicalDeviceMemoryProperties memory;
	uint32_t index;

	vkGetPhysicalDeviceMemoryProperties(side->physical, &memory);
	for (index = 0; index < memory.memoryTypeCount; index++) {
		if ((allowed & (1U << index)) != 0 && (memory.memoryTypes[index].propertyFlags & wanted) == wanted) {
			*type = index;
			return 0;
		}
	}
	return fail("memory type", (long)allowed);
}

/* Makes the side's NV12 image as Video Player and the decoder do: OPTIMAL, the plane views' flags, external memory. */
static int
make_picture(
	struct side *side,
	VkImageUsageFlags usage)
{
	VkExternalMemoryImageCreateInfo external;
	VkImageCreateInfo image;
	VkResult status;

	memset(&external, 0, sizeof(external));
	external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.pNext = &external;
	image.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	image.extent.width = SIDE;
	image.extent.height = SIDE;
	image.extent.depth = 1;
	image.mipLevels = 1;
	image.arrayLayers = 1;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = usage;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	status = vkCreateImage(side->device, &image, NULL, &side->picture);
	if (status != VK_SUCCESS)
		return fail("vkCreateImage NV12", status);
	return 0;
}

/* Makes the side's plane views (the image is bound by then). */
static int
make_views(
	struct side *side)
{
	VkImageViewCreateInfo view;
	VkResult status;

	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = side->picture;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_R8_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = 1;
	status = vkCreateImageView(side->device, &view, NULL, &side->luma);
	if (status != VK_SUCCESS)
		return fail("vkCreateImageView PLANE_0 R8", status);
	view.format = VK_FORMAT_R8G8_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
	status = vkCreateImageView(side->device, &view, NULL, &side->chroma);
	if (status != VK_SUCCESS)
		return fail("vkCreateImageView PLANE_1 R8G8", status);
	return 0;
}

/* Writes the pattern into both planes through the memory's mapping, at the planes' offsets and rows. */
static int
fill_picture(
	struct side *side)
{
	VkImageSubresource subresource;
	VkSubresourceLayout luma;
	VkSubresourceLayout chroma;
	uint8_t *bytes;
	uint32_t x;
	uint32_t y;
	VkResult status;

	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
	vkGetImageSubresourceLayout(side->device, side->picture, &subresource, &luma);
	subresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
	vkGetImageSubresourceLayout(side->device, side->picture, &subresource, &chroma);
	printf("nv12-board: planes at %llu (row %llu, %llu bytes) and %llu (row %llu, %llu bytes)\n",
	       (unsigned long long)luma.offset, (unsigned long long)luma.rowPitch, (unsigned long long)luma.size,
	       (unsigned long long)chroma.offset, (unsigned long long)chroma.rowPitch, (unsigned long long)chroma.size);
	if (luma.rowPitch < SIDE || chroma.rowPitch < SIDE || chroma.offset < luma.offset + luma.rowPitch * SIDE)
		return fail("plane layouts", (long)chroma.offset);
	status = vkMapMemory(side->device, side->picture_memory, 0, VK_WHOLE_SIZE, 0, (void **)&bytes);
	if (status != VK_SUCCESS)
		return fail("vkMapMemory picture", status);
	for (y = 0; y < SIDE; y++) {
		for (x = 0; x < SIDE; x++)
			bytes[luma.offset + y * luma.rowPitch + x] = pattern_luma(x, y);
	}
	for (y = 0; y < CHROMA_SIDE; y++) {
		for (x = 0; x < CHROMA_SIDE; x++) {
			bytes[chroma.offset + y * chroma.rowPitch + 2U * x] = pattern_cb(x, y);
			bytes[chroma.offset + y * chroma.rowPitch + 2U * x + 1U] = pattern_cr(x, y);
		}
	}
	vkUnmapMemory(side->device, side->picture_memory);
	return 0;
}

/* Makes a buffer bound to new memory with the wanted properties. */
static int
make_buffer(
	struct side *side,
	VkDeviceSize size,
	VkBufferUsageFlags usage,
	VkBuffer *buffer,
	VkDeviceMemory *memory)
{
	VkBufferCreateInfo info;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	uint32_t type;
	VkResult status;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = size;
	info.usage = usage;
	status = vkCreateBuffer(side->device, &info, NULL, buffer);
	if (status != VK_SUCCESS)
		return fail("vkCreateBuffer", status);
	vkGetBufferMemoryRequirements(side->device, *buffer, &requirements);
	if (find_memory(side, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type) != 0)
		return 1;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	status = vkAllocateMemory(side->device, &allocate, NULL, memory);
	if (status != VK_SUCCESS)
		return fail("vkAllocateMemory buffer", status);
	status = vkBindBufferMemory(side->device, *buffer, *memory, 0);
	if (status != VK_SUCCESS)
		return fail("vkBindBufferMemory", status);
	return 0;
}

/* Makes the RGBA8 target, its view, the readback buffer and the quad's vertices, and the plane views. */
static int
make_target(
	struct side *side)
{
	static const struct corner corners[6] = {
		{ { -1.0f, -1.0f }, { 0.0f, 0.0f } }, { { 1.0f, -1.0f }, { 1.0f, 0.0f } }, { { 1.0f, 1.0f }, { 1.0f, 1.0f } },
		{ { -1.0f, -1.0f }, { 0.0f, 0.0f } }, { { 1.0f, 1.0f }, { 1.0f, 1.0f } }, { { -1.0f, 1.0f }, { 0.0f, 1.0f } }
	};
	VkImageCreateInfo image;
	VkImageViewCreateInfo view;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	uint32_t type;
	void *mapped;
	VkResult status;

	if (make_views(side) != 0)
		return 1;
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = VK_FORMAT_R8G8B8A8_UNORM;
	image.extent.width = SIDE;
	image.extent.height = SIDE;
	image.extent.depth = 1;
	image.mipLevels = 1;
	image.arrayLayers = 1;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	status = vkCreateImage(side->device, &image, NULL, &side->target);
	if (status != VK_SUCCESS)
		return fail("vkCreateImage target", status);
	vkGetImageMemoryRequirements(side->device, side->target, &requirements);
	if (find_memory(side, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) != 0)
		return 1;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	status = vkAllocateMemory(side->device, &allocate, NULL, &side->target_memory);
	if (status != VK_SUCCESS)
		return fail("vkAllocateMemory target", status);
	status = vkBindImageMemory(side->device, side->target, side->target_memory, 0);
	if (status != VK_SUCCESS)
		return fail("vkBindImageMemory target", status);
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = side->target;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_R8G8B8A8_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = 1;
	status = vkCreateImageView(side->device, &view, NULL, &side->target_view);
	if (status != VK_SUCCESS)
		return fail("vkCreateImageView target", status);

	/* The readback buffer, mapped; the vertices, written. */
	if (make_buffer(side, READBACK_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &side->readback, &side->readback_memory) != 0)
		return 1;
	status = vkMapMemory(side->device, side->readback_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
	if (status != VK_SUCCESS)
		return fail("vkMapMemory readback", status);
	side->readback_bytes = mapped;
	if (make_buffer(side, sizeof(corners), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &side->vertices, &side->vertex_memory) != 0)
		return 1;
	status = vkMapMemory(side->device, side->vertex_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
	if (status != VK_SUCCESS)
		return fail("vkMapMemory vertices", status);
	memcpy(mapped, corners, sizeof(corners));
	vkUnmapMemory(side->device, side->vertex_memory);
	return 0;
}

/* Makes the sampler, the set of the two planes, the pass, the framebuffer and the pipeline. */
static int
make_pipeline(
	struct side *side)
{
	VkSamplerCreateInfo sampler;
	VkDescriptorSetLayoutBinding bindings[2];
	VkDescriptorSetLayoutCreateInfo set_info;
	VkPipelineLayoutCreateInfo layout_info;
	VkDescriptorPoolSize pool_size;
	VkDescriptorPoolCreateInfo pool_info;
	VkDescriptorSetAllocateInfo set_allocate;
	VkDescriptorImageInfo images[2];
	VkWriteDescriptorSet writes[2];
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkSubpassDependency dependency;
	VkRenderPassCreateInfo pass;
	VkFramebufferCreateInfo framebuffer;
	VkShaderModuleCreateInfo module;
	VkPipelineShaderStageCreateInfo stages[2];
	VkVertexInputBindingDescription vertex_binding;
	VkVertexInputAttributeDescription attributes[2];
	VkPipelineVertexInputStateCreateInfo vertex_input;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkViewport viewport;
	VkRect2D scissor;
	VkPipelineViewportStateCreateInfo viewport_state;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineColorBlendAttachmentState blend_attachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkGraphicsPipelineCreateInfo pipeline;
	uint32_t index;
	VkResult status;

	/* A nearest sampler: each pixel reads its own luma texel and its chroma texel. */
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_NEAREST;
	sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	status = vkCreateSampler(side->device, &sampler, NULL, &side->sampler);
	if (status != VK_SUCCESS)
		return fail("vkCreateSampler", status);

	/* The set: the luma plane at binding 0, the chroma plane at binding 1. */
	memset(bindings, 0, sizeof(bindings));
	for (index = 0; index < 2U; index++) {
		bindings[index].binding = index;
		bindings[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bindings[index].descriptorCount = 1;
		bindings[index].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	}
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_info.bindingCount = 2;
	set_info.pBindings = bindings;
	status = vkCreateDescriptorSetLayout(side->device, &set_info, NULL, &side->set_layout);
	if (status != VK_SUCCESS)
		return fail("vkCreateDescriptorSetLayout", status);
	memset(&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &side->set_layout;
	status = vkCreatePipelineLayout(side->device, &layout_info, NULL, &side->pipeline_layout);
	if (status != VK_SUCCESS)
		return fail("vkCreatePipelineLayout", status);
	memset(&pool_size, 0, sizeof(pool_size));
	pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_size.descriptorCount = 2;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &pool_size;
	status = vkCreateDescriptorPool(side->device, &pool_info, NULL, &side->pool);
	if (status != VK_SUCCESS)
		return fail("vkCreateDescriptorPool", status);
	memset(&set_allocate, 0, sizeof(set_allocate));
	set_allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	set_allocate.descriptorPool = side->pool;
	set_allocate.descriptorSetCount = 1;
	set_allocate.pSetLayouts = &side->set_layout;
	status = vkAllocateDescriptorSets(side->device, &set_allocate, &side->set);
	if (status != VK_SUCCESS)
		return fail("vkAllocateDescriptorSets", status);
	memset(images, 0, sizeof(images));
	memset(writes, 0, sizeof(writes));
	for (index = 0; index < 2U; index++) {
		images[index].sampler = side->sampler;
		images[index].imageView = index == 0U ? side->luma : side->chroma;
		images[index].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[index].dstSet = side->set;
		writes[index].dstBinding = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[index].pImageInfo = &images[index];
	}
	vkUpdateDescriptorSets(side->device, 2, writes, 0, NULL);

	/* The pass: the target cleared, stored and left for the copy. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
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
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &reference;
	memset(&dependency, 0, sizeof(dependency));
	dependency.srcSubpass = 0;
	dependency.dstSubpass = VK_SUBPASS_EXTERNAL;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependency.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	pass.attachmentCount = 1;
	pass.pAttachments = &attachment;
	pass.subpassCount = 1;
	pass.pSubpasses = &subpass;
	pass.dependencyCount = 1;
	pass.pDependencies = &dependency;
	status = vkCreateRenderPass(side->device, &pass, NULL, &side->render_pass);
	if (status != VK_SUCCESS)
		return fail("vkCreateRenderPass", status);
	memset(&framebuffer, 0, sizeof(framebuffer));
	framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer.renderPass = side->render_pass;
	framebuffer.attachmentCount = 1;
	framebuffer.pAttachments = &side->target_view;
	framebuffer.width = SIDE;
	framebuffer.height = SIDE;
	framebuffer.layers = 1;
	status = vkCreateFramebuffer(side->device, &framebuffer, NULL, &side->framebuffer);
	if (status != VK_SUCCESS)
		return fail("vkCreateFramebuffer", status);

	/* The shaders. */
	memset(&module, 0, sizeof(module));
	module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module.codeSize = sizeof(nv12_planes_vert);
	module.pCode = nv12_planes_vert;
	status = vkCreateShaderModule(side->device, &module, NULL, &side->modules[0]);
	if (status != VK_SUCCESS)
		return fail("vkCreateShaderModule vertex", status);
	module.codeSize = sizeof(nv12_planes_frag);
	module.pCode = nv12_planes_frag;
	status = vkCreateShaderModule(side->device, &module, NULL, &side->modules[1]);
	if (status != VK_SUCCESS)
		return fail("vkCreateShaderModule fragment", status);
	memset(stages, 0, sizeof(stages));
	for (index = 0; index < 2U; index++) {
		stages[index].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[index].stage = index == 0U ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[index].module = side->modules[index];
		stages[index].pName = "main";
	}

	/* The quad's corners, the whole target, filled, no blending. */
	memset(&vertex_binding, 0, sizeof(vertex_binding));
	vertex_binding.stride = sizeof(struct corner);
	vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	memset(attributes, 0, sizeof(attributes));
	attributes[0].location = 0;
	attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
	attributes[0].offset = 0;
	attributes[1].location = 1;
	attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
	attributes[1].offset = 8;
	memset(&vertex_input, 0, sizeof(vertex_input));
	vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input.vertexBindingDescriptionCount = 1;
	vertex_input.pVertexBindingDescriptions = &vertex_binding;
	vertex_input.vertexAttributeDescriptionCount = 2;
	vertex_input.pVertexAttributeDescriptions = attributes;
	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)SIDE;
	viewport.height = (float)SIDE;
	viewport.maxDepth = 1.0f;
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent.width = SIDE;
	scissor.extent.height = SIDE;
	memset(&viewport_state, 0, sizeof(viewport_state));
	viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport_state.viewportCount = 1;
	viewport_state.pViewports = &viewport;
	viewport_state.scissorCount = 1;
	viewport_state.pScissors = &scissor;
	memset(&raster, 0, sizeof(raster));
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;
	memset(&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	memset(&blend_attachment, 0, sizeof(blend_attachment));
	blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	memset(&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments = &blend_attachment;
	memset(&pipeline, 0, sizeof(pipeline));
	pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline.stageCount = 2;
	pipeline.pStages = stages;
	pipeline.pVertexInputState = &vertex_input;
	pipeline.pInputAssemblyState = &assembly;
	pipeline.pViewportState = &viewport_state;
	pipeline.pRasterizationState = &raster;
	pipeline.pMultisampleState = &multisample;
	pipeline.pColorBlendState = &blend;
	pipeline.layout = side->pipeline_layout;
	pipeline.renderPass = side->render_pass;
	status = vkCreateGraphicsPipelines(side->device, VK_NULL_HANDLE, 1, &pipeline, NULL, &side->pipeline);
	if (status != VK_SUCCESS)
		return fail("vkCreateGraphicsPipelines", status);
	return 0;
}

/* Moves the picture between layouts around the stages that use it. */
static void
picture_barrier(
	struct side *side,
	VkImageLayout from,
	VkImageLayout to,
	VkAccessFlags source_access,
	VkAccessFlags destination_access,
	VkPipelineStageFlags source_stage,
	VkPipelineStageFlags destination_stage)
{
	VkImageMemoryBarrier barrier;

	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = source_access;
	barrier.dstAccessMask = destination_access;
	barrier.oldLayout = from;
	barrier.newLayout = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = side->picture;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(side->command, source_stage, destination_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

/* Draws the planes into the target, copies it (and with copy_planes the two planes) out, and waits. */
static int
draw(
	struct side *side,
	int copy_planes)
{
	VkCommandBufferBeginInfo begin;
	VkRenderPassBeginInfo pass;
	VkClearValue clear;
	VkBufferImageCopy copies[2];
	VkMemoryBarrier host;
	VkSubmitInfo submit;
	VkDeviceSize offset;
	VkResult status;

	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	status = vkBeginCommandBuffer(side->command, &begin);
	if (status != VK_SUCCESS)
		return fail("vkBeginCommandBuffer", status);

	/* The host's writes, then the planes sampled. */
	picture_barrier(side, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
			VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	memset(&clear, 0, sizeof(clear));
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = side->render_pass;
	pass.framebuffer = side->framebuffer;
	pass.renderArea.extent.width = SIDE;
	pass.renderArea.extent.height = SIDE;
	pass.clearValueCount = 1;
	pass.pClearValues = &clear;
	vkCmdBeginRenderPass(side->command, &pass, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdBindPipeline(side->command, VK_PIPELINE_BIND_POINT_GRAPHICS, side->pipeline);
	vkCmdBindDescriptorSets(side->command, VK_PIPELINE_BIND_POINT_GRAPHICS, side->pipeline_layout, 0, 1, &side->set, 0, NULL);
	offset = 0;
	vkCmdBindVertexBuffers(side->command, 0, 1, &side->vertices, &offset);
	vkCmdDraw(side->command, 6, 1, 0, 0);
	vkCmdEndRenderPass(side->command);

	/* The target out. */
	memset(copies, 0, sizeof(copies));
	copies[0].bufferOffset = READBACK_TARGET;
	copies[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copies[0].imageSubresource.layerCount = 1;
	copies[0].imageExtent.width = SIDE;
	copies[0].imageExtent.height = SIDE;
	copies[0].imageExtent.depth = 1;
	vkCmdCopyImageToBuffer(side->command, side->target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, side->readback, 1, copies);

	/* The planes out, as libmedia's readback copies them. */
	if (copy_planes) {
		picture_barrier(side, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		memset(copies, 0, sizeof(copies));
		copies[0].bufferOffset = READBACK_LUMA;
		copies[0].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
		copies[0].imageSubresource.layerCount = 1;
		copies[0].imageExtent.width = SIDE;
		copies[0].imageExtent.height = SIDE;
		copies[0].imageExtent.depth = 1;
		copies[1].bufferOffset = READBACK_CHROMA;
		copies[1].imageSubresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
		copies[1].imageSubresource.layerCount = 1;
		copies[1].imageExtent.width = CHROMA_SIDE;
		copies[1].imageExtent.height = CHROMA_SIDE;
		copies[1].imageExtent.depth = 1;
		vkCmdCopyImageToBuffer(side->command, side->picture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, side->readback, 2, copies);
	}

	/* The copies before the host reads. */
	memset(&host, 0, sizeof(host));
	host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(side->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
	status = vkEndCommandBuffer(side->command);
	if (status != VK_SUCCESS)
		return fail("vkEndCommandBuffer", status);

	/* Runs it and waits up to ten seconds. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &side->command;
	status = vkQueueSubmit(side->queue, 1, &submit, side->fence);
	if (status != VK_SUCCESS)
		return fail("vkQueueSubmit", status);
	status = vkWaitForFences(side->device, 1, &side->fence, VK_TRUE, 10000000000ULL);
	if (status != VK_SUCCESS)
		return fail("vkWaitForFences", status);
	return 0;
}

/* Compares every target pixel with the pattern: red the luma, green and blue the chroma pair of its 2x2 block. */
static int
check_target(
	struct side *side)
{
	const uint8_t *pixel;
	uint32_t x;
	uint32_t y;
	uint32_t wrong;
	long first;

	wrong = 0;
	first = -1;
	for (y = 0; y < SIDE; y++) {
		for (x = 0; x < SIDE; x++) {
			pixel = side->readback_bytes + READBACK_TARGET + (y * SIDE + x) * 4U;
			if (pixel[0] != pattern_luma(x, y) ||
			    pixel[1] != pattern_cb(x / 2U, y / 2U) ||
			    pixel[2] != pattern_cr(x / 2U, y / 2U) ||
			    pixel[3] != 255U) {
				if (first < 0) {
					first = (long)(y * SIDE + x);
					printf("nv12-board: %s pixel %u,%u is %02x%02x%02x%02x, wanted %02x%02x%02xff\n", side->name, x, y,
					       pixel[0], pixel[1], pixel[2], pixel[3], pattern_luma(x, y), pattern_cb(x / 2U, y / 2U), pattern_cr(x / 2U, y / 2U));
				}
				wrong++;
			}
		}
	}
	printf("nv12-board: %s sampled planes: %u of %u pixels wrong\n", side->name, (unsigned)wrong, SIDE * SIDE);
	if (wrong != 0)
		return fail(side->name[0] == 'A' ? "A's sampled planes" : "B's sampled import", first);
	return 0;
}

/* Compares the planes the copies wrote with the pattern. */
static int
check_planes(
	struct side *side)
{
	const uint8_t *luma;
	const uint8_t *chroma;
	uint32_t x;
	uint32_t y;
	uint32_t wrong;

	luma = side->readback_bytes + READBACK_LUMA;
	chroma = side->readback_bytes + READBACK_CHROMA;
	wrong = 0;
	for (y = 0; y < SIDE; y++) {
		for (x = 0; x < SIDE; x++) {
			if (luma[y * SIDE + x] != pattern_luma(x, y))
				wrong++;
		}
	}
	for (y = 0; y < CHROMA_SIDE; y++) {
		for (x = 0; x < CHROMA_SIDE; x++) {
			if (chroma[(y * CHROMA_SIDE + x) * 2U] != pattern_cb(x, y) || chroma[(y * CHROMA_SIDE + x) * 2U + 1U] != pattern_cr(x, y))
				wrong++;
		}
	}
	printf("nv12-board: %s plane copies: %u of %u texels wrong\n", side->name, (unsigned)wrong, SIDE * SIDE + CHROMA_SIDE * CHROMA_SIDE);
	if (wrong != 0)
		return fail("plane copies", (long)wrong);
	return 0;
}

/* Destroys what a side made, newest first. */
static void
destroy(
	struct side *side)
{
	if (side->device != VK_NULL_HANDLE) {
		(void)vkDeviceWaitIdle(side->device);
		vkDestroyPipeline(side->device, side->pipeline, NULL);
		vkDestroyShaderModule(side->device, side->modules[1], NULL);
		vkDestroyShaderModule(side->device, side->modules[0], NULL);
		vkDestroyFramebuffer(side->device, side->framebuffer, NULL);
		vkDestroyRenderPass(side->device, side->render_pass, NULL);
		vkDestroyDescriptorPool(side->device, side->pool, NULL);
		vkDestroyPipelineLayout(side->device, side->pipeline_layout, NULL);
		vkDestroyDescriptorSetLayout(side->device, side->set_layout, NULL);
		vkDestroySampler(side->device, side->sampler, NULL);
		vkDestroyBuffer(side->device, side->vertices, NULL);
		vkFreeMemory(side->device, side->vertex_memory, NULL);
		if (side->readback_bytes != NULL)
			vkUnmapMemory(side->device, side->readback_memory);
		vkDestroyBuffer(side->device, side->readback, NULL);
		vkFreeMemory(side->device, side->readback_memory, NULL);
		vkDestroyImageView(side->device, side->target_view, NULL);
		vkDestroyImage(side->device, side->target, NULL);
		vkFreeMemory(side->device, side->target_memory, NULL);
		vkDestroyImageView(side->device, side->chroma, NULL);
		vkDestroyImageView(side->device, side->luma, NULL);
		vkDestroyImage(side->device, side->picture, NULL);
		vkFreeMemory(side->device, side->picture_memory, NULL);
		vkDestroyFence(side->device, side->fence, NULL);
		vkDestroyCommandPool(side->device, side->command_pool, NULL);
		vkDestroyDevice(side->device, NULL);
	}
	if (side->instance != VK_NULL_HANDLE)
		vkDestroyInstance(side->instance, NULL);
}
