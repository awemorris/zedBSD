/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * vkstage: the gpu-selftest stages after image, one stage a run, named by
 * the only argument:
 *
 *   clear    vkCmdClearColorImage on an OPTIMAL image, copied to a buffer
 *   copy     buffer -> OPTIMAL image -> buffer, compared word by word
 *   draw     an offscreen render pass: clear and one triangle
 *   sample   a quad that samples a 4x4 texture in its four quadrants
 *   depth    two triangles in the wrong order, sorted by a D32 depth test
 *   present  one cleared frame on the first display (VK_KHR_display), held
 *            three seconds; the acquire, the clear and the present are
 *            ordered by semaphores and the clear's submit by a fence
 *
 * Like vkcompute it opens an instance and a device on the first physical
 * device, does the stage's work, reads the result back and prints one line:
 *
 *   <stage>: <device>: PASS, <what was seen>
 *   <stage>: <device>: FAIL, <call> <VkResult>
 *   <stage>: <device>: FAIL, pixel (<x>,<y>) 0x<seen>, expected 0x<wanted>
 *   <stage>: <device>: SKIP, <what the device lacks>
 *   <stage>: no Vulkan device (<call> <VkResult>)
 *
 * The exit status is 0 only on PASS, and 3 on SKIP (the device does not
 * offer what the stage needs, such as depth's D32_SFLOAT attachment).
 */

#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "shader.h"

/* The offscreen images' side in pixels. */
#define VKSTAGE_SIZE		64U

/* The pixels of one offscreen image. */
#define VKSTAGE_PIXELS		(VKSTAGE_SIZE * VKSTAGE_SIZE)

/* The texture's side in texels. */
#define VKSTAGE_TEXTURE		4U

/* The fence wait's limit in nanoseconds (5 s, above the driver's 2 s). */
#define VKSTAGE_WAIT_NS		5000000000ULL

/* The seconds the present stage leaves its frame on the display. */
#define VKSTAGE_PRESENT_SECONDS	3U

/* The most bytes of a failure's reason. */
#define VKSTAGE_REASON_MAX	160

/* The most buffers, images and memories one stage makes. */
#define VKSTAGE_OBJECTS_MAX	4

/* The colour channel tolerance of the UNORM8 comparisons. */
#define VKSTAGE_TOLERANCE	1

/* The exit status of a stage the device cannot run. */
#define VKSTAGE_EXIT_SKIP	3

/* One vertex of draw and depth: its position (z the depth) and its colour. */
struct colour_vertex {
	float position[3];
	float colour[4];
};

/* One vertex of sample: its position and its texture coordinate. */
struct texture_vertex {
	float position[2];
	float coordinate[2];
};

/* What a graphics pipeline is built from. */
struct pipeline_shape {
	const uint32_t *vertex;
	size_t vertex_size;
	const uint32_t *fragment;
	size_t fragment_size;
	const VkVertexInputAttributeDescription *attributes;
	uint32_t attribute_count;
	uint32_t stride;
	VkPrimitiveTopology topology;
	int depth;
	int texture;
};

/*
 * The Vulkan objects of one run.
 *
 * Every handle starts as VK_NULL_HANDLE and is destroyed by
 * vkstage_destroy() whether or not the run got as far as creating it.
 */
struct vkstage {
	const char *stage;
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	VkCommandPool command_pool;
	VkCommandBuffer command;
	VkFence fence;
	VkBuffer buffers[VKSTAGE_OBJECTS_MAX];
	VkDeviceMemory buffer_memories[VKSTAGE_OBJECTS_MAX];
	void *mapped[VKSTAGE_OBJECTS_MAX];
	uint32_t buffer_count;
	VkImage images[VKSTAGE_OBJECTS_MAX];
	VkDeviceMemory image_memories[VKSTAGE_OBJECTS_MAX];
	VkImageView views[VKSTAGE_OBJECTS_MAX];
	uint32_t image_count;
	VkRenderPass render_pass;
	VkFramebuffer framebuffer;
	VkShaderModule modules[2];
	VkSampler sampler;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline;
	VkDescriptorPool pool;
	VkDescriptorSet set;
	VkSurfaceKHR surface;
	VkSwapchainKHR swapchain;
	VkSemaphore acquired;
	VkSemaphore rendered;
	char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
	const char *failed_call;
	char reason[VKSTAGE_REASON_MAX];
	char seen[VKSTAGE_REASON_MAX];
	char skipped[VKSTAGE_REASON_MAX];
};

/* One stage: its name, whether it needs the display extensions, and its work. */
struct stage {
	const char *name;
	int wsi;
	VkResult (*run)(struct vkstage *run);
};

static VkResult vkstage_open_device(struct vkstage *run, int wsi);
static VkResult vkstage_find_memory(struct vkstage *run, uint32_t allowed, VkMemoryPropertyFlags wanted, uint32_t *type);
static VkResult vkstage_create_buffer(struct vkstage *run, VkDeviceSize size, VkBufferUsageFlags usage, uint32_t *index);
static VkResult vkstage_create_image(struct vkstage *run, VkFormat format, uint32_t side, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t *index);
static VkResult vkstage_begin(struct vkstage *run);
static VkResult vkstage_submit(struct vkstage *run);
static void vkstage_image_barrier(struct vkstage *run, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags source_access, VkAccessFlags destination_access, VkPipelineStageFlags source_stage, VkPipelineStageFlags destination_stage);
static void vkstage_copy_out(struct vkstage *run, VkImage image, VkBuffer buffer, uint32_t side);
static void vkstage_host_barrier(struct vkstage *run);
static VkResult vkstage_create_render_pass(struct vkstage *run, int depth, uint32_t colour, uint32_t depth_image);
static VkResult vkstage_create_pipeline(struct vkstage *run, const struct pipeline_shape *shape);
static VkResult vkstage_draw(struct vkstage *run, uint32_t vertices, uint32_t vertex_count, int depth, uint32_t colour);
static int vkstage_check_pixel(struct vkstage *run, const uint32_t *pixels, uint32_t x, uint32_t y, uint32_t wanted);
static uint32_t vkstage_rgba(uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha);
static VkResult stage_clear(struct vkstage *run);
static VkResult stage_copy(struct vkstage *run);
static VkResult stage_draw(struct vkstage *run);
static VkResult stage_sample(struct vkstage *run);
static VkResult stage_depth(struct vkstage *run);
static VkResult stage_present(struct vkstage *run);
static VkResult vkstage_open_surface(struct vkstage *run, VkExtent2D *extent);
static void vkstage_destroy(struct vkstage *run);

/* The stages, by name. */
static const struct stage stages[] = {
	{ "clear", 0, stage_clear },
	{ "copy", 0, stage_copy },
	{ "draw", 0, stage_draw },
	{ "sample", 0, stage_sample },
	{ "depth", 0, stage_depth },
	{ "present", 1, stage_present },
};

/* The clear colour of clear: 0.25, 0.5, 0.75, 1.0. */
static const float clear_colour[4] = { 0.25f, 0.5f, 0.75f, 1.0f };

/* The background of draw and depth: opaque blue. */
static const float background[4] = { 0.0f, 0.0f, 1.0f, 1.0f };

/*
 * Runs the stage named by the argument and prints the result line.
 */
int
main(
	int argc,
	char **argv)
{
	const struct stage *stage;
	struct vkstage run;
	VkResult status;
	size_t i;
	int same;

	/* Finds the stage. */
	stage = NULL;
	for (i = 0; argc == 2 && i < sizeof(stages) / sizeof(stages[0]); i++) {
		same = strcmp(argv[1], stages[i].name);
		if (same == 0)
			stage = &stages[i];
	}

	/* Refuses an unknown or missing stage. */
	if (stage == NULL) {
		fprintf(stderr, "usage: vkstage clear|copy|draw|sample|depth|present\n");
		return 2;
	}

	/* Starts with no objects, so the teardown can run from any point. */
	memset(&run, 0, sizeof(run));
	run.stage = stage->name;

	/* Opens the instance and the device; with none, nothing else can run. */
	status = vkstage_open_device(&run, stage->wsi);
	if (status != VK_SUCCESS && run.name[0] == '\0') {
		printf("%s: no Vulkan device (%s %d)\n", run.stage, run.failed_call, (int)status);
		vkstage_destroy(&run);
		return 1;
	}

	/* Does the stage's work. */
	if (status == VK_SUCCESS)
		status = stage->run(&run);

	/* Reports the call that failed, or the pixel that was wrong. */
	if (status != VK_SUCCESS) {
		if (run.reason[0] != '\0') {
			printf("%s: %s: FAIL, %s\n", run.stage, run.name, run.reason);
		} else {
			printf("%s: %s: FAIL, %s %d\n", run.stage, run.name, run.failed_call, (int)status);
		}
		vkstage_destroy(&run);
		return 1;
	}

	/* Reports a stage the device cannot run, and what it lacks. */
	if (run.skipped[0] != '\0') {
		printf("%s: %s: SKIP, %s\n", run.stage, run.name, run.skipped);
		vkstage_destroy(&run);
		return VKSTAGE_EXIT_SKIP;
	}

	/* Prints what the stage saw. */
	printf("%s: %s: PASS, %s\n", run.stage, run.name, run.seen);
	vkstage_destroy(&run);

	/* Succeeded: the stage's result matched. */
	return 0;
}

/* Creates the instance and the device on the first physical device's graphics family. */
static VkResult
vkstage_open_device(
	struct vkstage *run,
	int wsi)
{
	static const char *const instance_extensions[] = {
		VK_KHR_SURFACE_EXTENSION_NAME,
		VK_KHR_DISPLAY_EXTENSION_NAME
	};
	static const char *const device_extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkApplicationInfo application;
	VkInstanceCreateInfo instance_info;
	VkPhysicalDeviceProperties properties;
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

	/* Creates a Vulkan 1.0 instance, with the display extensions for present. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "vkstage";
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&instance_info, 0, sizeof(instance_info));
	instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info.pApplicationInfo = &application;
	if (wsi) {
		instance_info.enabledExtensionCount = 2;
		instance_info.ppEnabledExtensionNames = instance_extensions;
	}
	run->failed_call = "vkCreateInstance";
	status = vkCreateInstance(&instance_info, NULL, &run->instance);
	if (status != VK_SUCCESS)
		return status;

	/* Takes the first physical device; VK_INCOMPLETE only means there are more. */
	count = 1;
	run->failed_call = "vkEnumeratePhysicalDevices";
	status = vkEnumeratePhysicalDevices(run->instance, &count, &run->physical);
	if (status != VK_SUCCESS && status != VK_INCOMPLETE)
		return status;
	if (count == 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Names the device in every later line. */
	vkGetPhysicalDeviceProperties(run->physical, &properties);
	memcpy(run->name, properties.deviceName, sizeof(run->name));
	run->name[sizeof(run->name) - 1] = '\0';

	/* Finds the first family with graphics (which carries transfer too). */
	count = sizeof(families) / sizeof(families[0]);
	vkGetPhysicalDeviceQueueFamilyProperties(run->physical, &count, families);
	for (index = 0; index < count; index++) {
		if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
			break;
	}

	/* Refuses a device without graphics. */
	run->failed_call = "graphics queue family";
	if (index == count)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* Creates the device with one queue of that family, and the swapchain for present. */
	run->family = index;
	priority = 1.0f;
	memset(&queue_info, 0, sizeof(queue_info));
	queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = run->family;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &priority;
	memset(&device_info, 0, sizeof(device_info));
	device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue_info;
	if (wsi) {
		device_info.enabledExtensionCount = 1;
		device_info.ppEnabledExtensionNames = device_extensions;
	}
	run->failed_call = "vkCreateDevice";
	status = vkCreateDevice(run->physical, &device_info, NULL, &run->device);
	if (status != VK_SUCCESS)
		return status;
	vkGetDeviceQueue(run->device, run->family, 0, &run->queue);

	/* Creates the command pool and the one primary command buffer every stage records into. */
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = run->family;
	run->failed_call = "vkCreateCommandPool";
	status = vkCreateCommandPool(run->device, &pool_info, NULL, &run->command_pool);
	if (status != VK_SUCCESS)
		return status;
	memset(&command_info, 0, sizeof(command_info));
	command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command_info.commandPool = run->command_pool;
	command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command_info.commandBufferCount = 1;
	run->failed_call = "vkAllocateCommandBuffers";
	status = vkAllocateCommandBuffers(run->device, &command_info, &run->command);
	if (status != VK_SUCCESS)
		return status;

	/* Creates the fence every submit signals. */
	memset(&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	run->failed_call = "vkCreateFence";
	status = vkCreateFence(run->device, &fence_info, NULL, &run->fence);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the device, its queue and the command buffer are ready. */
	return VK_SUCCESS;
}

/* Finds a memory type in allowed with every wanted property. */
static VkResult
vkstage_find_memory(
	struct vkstage *run,
	uint32_t allowed,
	VkMemoryPropertyFlags wanted,
	uint32_t *type)
{
	VkPhysicalDeviceMemoryProperties memory;
	uint32_t index;

	/* Takes the first allowed type that has the properties. */
	vkGetPhysicalDeviceMemoryProperties(run->physical, &memory);
	for (index = 0; index < memory.memoryTypeCount; index++) {
		if ((allowed & (1U << index)) == 0)
			continue;
		if ((memory.memoryTypes[index].propertyFlags & wanted) == wanted)
			break;
	}

	/* Refuses a device with no such type. */
	run->failed_call = "memory type";
	if (index == memory.memoryTypeCount)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* Succeeded: the type's index. */
	*type = index;
	return VK_SUCCESS;
}

/* Creates a buffer in host-visible, host-coherent memory, mapped and filled with 0xa5. */
static VkResult
vkstage_create_buffer(
	struct vkstage *run,
	VkDeviceSize size,
	VkBufferUsageFlags usage,
	uint32_t *index)
{
	VkBufferCreateInfo buffer_info;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate_info;
	uint32_t slot;
	uint32_t type;
	VkResult status;

	/* Takes the next slot. */
	slot = run->buffer_count;
	run->failed_call = "buffer slots";
	if (slot == VKSTAGE_OBJECTS_MAX)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	run->buffer_count++;

	/* Creates the buffer. */
	memset(&buffer_info, 0, sizeof(buffer_info));
	buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer_info.size = size;
	buffer_info.usage = usage;
	buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	run->failed_call = "vkCreateBuffer";
	status = vkCreateBuffer(run->device, &buffer_info, NULL, &run->buffers[slot]);
	if (status != VK_SUCCESS)
		return status;

	/* Allocates memory the CPU sees without flushes and binds it. */
	vkGetBufferMemoryRequirements(run->device, run->buffers[slot], &requirements);
	status = vkstage_find_memory(
		run,
		requirements.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		&type);
	if (status != VK_SUCCESS)
		return status;
	memset(&allocate_info, 0, sizeof(allocate_info));
	allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate_info.allocationSize = requirements.size;
	allocate_info.memoryTypeIndex = type;
	run->failed_call = "vkAllocateMemory";
	status = vkAllocateMemory(run->device, &allocate_info, NULL, &run->buffer_memories[slot]);
	if (status != VK_SUCCESS)
		return status;
	run->failed_call = "vkBindBufferMemory";
	status = vkBindBufferMemory(run->device, run->buffers[slot], run->buffer_memories[slot], 0);
	if (status != VK_SUCCESS)
		return status;

	/* Maps it for the run and fills it with a value no stage writes, so stale memory fails. */
	run->failed_call = "vkMapMemory";
	status = vkMapMemory(run->device, run->buffer_memories[slot], 0, VK_WHOLE_SIZE, 0, &run->mapped[slot]);
	if (status != VK_SUCCESS)
		return status;
	memset(run->mapped[slot], 0xa5, (size_t)size);

	/* Succeeded: the buffer's slot. */
	*index = slot;
	return VK_SUCCESS;
}

/* Creates a 2D OPTIMAL image of side x side with one level, its memory and a view. */
static VkResult
vkstage_create_image(
	struct vkstage *run,
	VkFormat format,
	uint32_t side,
	VkImageUsageFlags usage,
	VkImageAspectFlags aspect,
	uint32_t *index)
{
	VkImageCreateInfo image_info;
	VkImageViewCreateInfo view_info;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate_info;
	uint32_t slot;
	uint32_t type;
	VkResult status;

	/* Takes the next slot. */
	slot = run->image_count;
	run->failed_call = "image slots";
	if (slot == VKSTAGE_OBJECTS_MAX)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	run->image_count++;

	/* Creates the image. */
	memset(&image_info, 0, sizeof(image_info));
	image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = format;
	image_info.extent.width = side;
	image_info.extent.height = side;
	image_info.extent.depth = 1;
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
	image_info.usage = usage;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	run->failed_call = "vkCreateImage";
	status = vkCreateImage(run->device, &image_info, NULL, &run->images[slot]);
	if (status != VK_SUCCESS)
		return status;

	/* Allocates device-local memory when there is some, any allowed type otherwise. */
	vkGetImageMemoryRequirements(run->device, run->images[slot], &requirements);
	status = vkstage_find_memory(run, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type);
	if (status != VK_SUCCESS)
		status = vkstage_find_memory(run, requirements.memoryTypeBits, 0, &type);
	if (status != VK_SUCCESS)
		return status;
	memset(&allocate_info, 0, sizeof(allocate_info));
	allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate_info.allocationSize = requirements.size;
	allocate_info.memoryTypeIndex = type;
	run->failed_call = "vkAllocateMemory";
	status = vkAllocateMemory(run->device, &allocate_info, NULL, &run->image_memories[slot]);
	if (status != VK_SUCCESS)
		return status;
	run->failed_call = "vkBindImageMemory";
	status = vkBindImageMemory(run->device, run->images[slot], run->image_memories[slot], 0);
	if (status != VK_SUCCESS)
		return status;

	/* Creates the view of the whole image. */
	memset(&view_info, 0, sizeof(view_info));
	view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view_info.image = run->images[slot];
	view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view_info.format = format;
	view_info.subresourceRange.aspectMask = aspect;
	view_info.subresourceRange.levelCount = 1;
	view_info.subresourceRange.layerCount = 1;
	run->failed_call = "vkCreateImageView";
	status = vkCreateImageView(run->device, &view_info, NULL, &run->views[slot]);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the image's slot. */
	*index = slot;
	return VK_SUCCESS;
}

/* Starts recording the command buffer for one submit. */
static VkResult
vkstage_begin(
	struct vkstage *run)
{
	VkCommandBufferBeginInfo begin;
	VkResult status;

	/* Begins a one-time recording. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	run->failed_call = "vkBeginCommandBuffer";
	status = vkBeginCommandBuffer(run->command, &begin);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the command buffer is recording. */
	return VK_SUCCESS;
}

/* Ends the recording, submits it with the fence and waits for the fence. */
static VkResult
vkstage_submit(
	struct vkstage *run)
{
	VkSubmitInfo submit;
	VkResult status;

	/* Ends the recording. */
	run->failed_call = "vkEndCommandBuffer";
	status = vkEndCommandBuffer(run->command);
	if (status != VK_SUCCESS)
		return status;

	/* Submits the command buffer. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &run->command;
	run->failed_call = "vkQueueSubmit";
	status = vkQueueSubmit(run->queue, 1, &submit, run->fence);
	if (status != VK_SUCCESS)
		return status;

	/* Waits for it; VK_TIMEOUT is reported like any failure. */
	run->failed_call = "vkWaitForFences";
	status = vkWaitForFences(run->device, 1, &run->fence, VK_TRUE, VKSTAGE_WAIT_NS);
	if (status != VK_SUCCESS)
		return status;

	/* Makes the fence and the command buffer ready for the next submit. */
	run->failed_call = "vkResetFences";
	status = vkResetFences(run->device, 1, &run->fence);
	if (status != VK_SUCCESS)
		return status;
	run->failed_call = "vkResetCommandBuffer";
	status = vkResetCommandBuffer(run->command, 0);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the work has finished. */
	return VK_SUCCESS;
}

/* Records a layout change of a whole single-level colour or depth image. */
static void
vkstage_image_barrier(
	struct vkstage *run,
	VkImage image,
	VkImageLayout from,
	VkImageLayout to,
	VkAccessFlags source_access,
	VkAccessFlags destination_access,
	VkPipelineStageFlags source_stage,
	VkPipelineStageFlags destination_stage)
{
	VkImageMemoryBarrier barrier;

	/* Describes the change on the colour aspect of level 0, layer 0. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = source_access;
	barrier.dstAccessMask = destination_access;
	barrier.oldLayout = from;
	barrier.newLayout = to;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;

	/* Records it. */
	vkCmdPipelineBarrier(
		run->command,
		source_stage,
		destination_stage,
		0,
		0,
		NULL,
		0,
		NULL,
		1,
		&barrier);
}

/* Records the copy of a whole TRANSFER_SRC colour image into a tightly packed buffer. */
static void
vkstage_copy_out(
	struct vkstage *run,
	VkImage image,
	VkBuffer buffer,
	uint32_t side)
{
	VkBufferImageCopy region;

	/* Copies level 0 with rows of side pixels. */
	memset(&region, 0, sizeof(region));
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent.width = side;
	region.imageExtent.height = side;
	region.imageExtent.depth = 1;
	vkCmdCopyImageToBuffer(run->command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
}

/* Records the barrier that makes the transfer writes visible to the host. */
static void
vkstage_host_barrier(
	struct vkstage *run)
{
	VkMemoryBarrier barrier;

	/* Transfer writes, then host reads. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	vkCmdPipelineBarrier(
		run->command,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_HOST_BIT,
		0,
		1,
		&barrier,
		0,
		NULL,
		0,
		NULL);
}

/*
 * Creates the one-subpass render pass (a cleared R8G8B8A8 colour left in
 * TRANSFER_SRC, and with depth a cleared D32 not stored) and its framebuffer
 * on the colour image and, with depth, the depth image.
 */
static VkResult
vkstage_create_render_pass(
	struct vkstage *run,
	int depth,
	uint32_t colour,
	uint32_t depth_image)
{
	VkAttachmentDescription attachments[2];
	VkAttachmentReference colour_reference;
	VkAttachmentReference depth_reference;
	VkSubpassDescription subpass;
	VkSubpassDependency dependency;
	VkRenderPassCreateInfo pass_info;
	VkFramebufferCreateInfo framebuffer_info;
	VkImageView views[2];
	VkResult status;

	/* The colour attachment: cleared, stored, left ready for the copy out. */
	memset(attachments, 0, sizeof(attachments));
	attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

	/* The depth attachment: cleared to 1.0, not stored. */
	attachments[1].format = VK_FORMAT_D32_SFLOAT;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	/* The one subpass writes the colour and, with depth, tests the depth. */
	memset(&colour_reference, 0, sizeof(colour_reference));
	colour_reference.attachment = 0;
	colour_reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&depth_reference, 0, sizeof(depth_reference));
	depth_reference.attachment = 1;
	depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colour_reference;
	if (depth)
		subpass.pDepthStencilAttachment = &depth_reference;

	/* The colour writes come before the copy that follows the pass. */
	memset(&dependency, 0, sizeof(dependency));
	dependency.srcSubpass = 0;
	dependency.dstSubpass = VK_SUBPASS_EXTERNAL;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependency.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

	/* Creates the render pass. */
	memset(&pass_info, 0, sizeof(pass_info));
	pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	pass_info.attachmentCount = 1;
	if (depth)
		pass_info.attachmentCount = 2;
	pass_info.pAttachments = attachments;
	pass_info.subpassCount = 1;
	pass_info.pSubpasses = &subpass;
	pass_info.dependencyCount = 1;
	pass_info.pDependencies = &dependency;
	run->failed_call = "vkCreateRenderPass";
	status = vkCreateRenderPass(run->device, &pass_info, NULL, &run->render_pass);
	if (status != VK_SUCCESS)
		return status;

	/* Creates the framebuffer on the images' views. */
	views[0] = run->views[colour];
	views[1] = VK_NULL_HANDLE;
	if (depth)
		views[1] = run->views[depth_image];
	memset(&framebuffer_info, 0, sizeof(framebuffer_info));
	framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer_info.renderPass = run->render_pass;
	framebuffer_info.attachmentCount = pass_info.attachmentCount;
	framebuffer_info.pAttachments = views;
	framebuffer_info.width = VKSTAGE_SIZE;
	framebuffer_info.height = VKSTAGE_SIZE;
	framebuffer_info.layers = 1;
	run->failed_call = "vkCreateFramebuffer";
	status = vkCreateFramebuffer(run->device, &framebuffer_info, NULL, &run->framebuffer);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the render pass and its framebuffer exist. */
	return VK_SUCCESS;
}

/*
 * Creates the shader modules, the layouts (with texture, one combined image
 * sampler at set 0 binding 0) and the graphics pipeline of shape, with a
 * fixed 64 x 64 viewport and no culling.
 */
static VkResult
vkstage_create_pipeline(
	struct vkstage *run,
	const struct pipeline_shape *shape)
{
	VkShaderModuleCreateInfo module_info;
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo set_info;
	VkPipelineLayoutCreateInfo layout_info;
	VkPipelineShaderStageCreateInfo shader_stages[2];
	VkVertexInputBindingDescription vertex_binding;
	VkPipelineVertexInputStateCreateInfo vertex_input;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkViewport viewport;
	VkRect2D scissor;
	VkPipelineViewportStateCreateInfo viewport_state;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineDepthStencilStateCreateInfo depth;
	VkPipelineColorBlendAttachmentState blend_attachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkGraphicsPipelineCreateInfo pipeline_info;
	VkResult status;

	/* Creates the two modules from the embedded SPIR-V. */
	memset(&module_info, 0, sizeof(module_info));
	module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module_info.codeSize = shape->vertex_size;
	module_info.pCode = shape->vertex;
	run->failed_call = "vkCreateShaderModule";
	status = vkCreateShaderModule(run->device, &module_info, NULL, &run->modules[0]);
	if (status != VK_SUCCESS)
		return status;
	module_info.codeSize = shape->fragment_size;
	module_info.pCode = shape->fragment;
	status = vkCreateShaderModule(run->device, &module_info, NULL, &run->modules[1]);
	if (status != VK_SUCCESS)
		return status;

	/* Describes set 0: the texture for sample, nothing otherwise. */
	memset(&binding, 0, sizeof(binding));
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	if (shape->texture) {
		set_info.bindingCount = 1;
		set_info.pBindings = &binding;
	}
	run->failed_call = "vkCreateDescriptorSetLayout";
	status = vkCreateDescriptorSetLayout(run->device, &set_info, NULL, &run->set_layout);
	if (status != VK_SUCCESS)
		return status;
	memset(&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &run->set_layout;
	run->failed_call = "vkCreatePipelineLayout";
	status = vkCreatePipelineLayout(run->device, &layout_info, NULL, &run->pipeline_layout);
	if (status != VK_SUCCESS)
		return status;

	/* The vertex and fragment stages from each module's main. */
	memset(shader_stages, 0, sizeof(shader_stages));
	shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	shader_stages[0].module = run->modules[0];
	shader_stages[0].pName = "main";
	shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	shader_stages[1].module = run->modules[1];
	shader_stages[1].pName = "main";

	/* One interleaved vertex buffer at binding 0. */
	memset(&vertex_binding, 0, sizeof(vertex_binding));
	vertex_binding.binding = 0;
	vertex_binding.stride = shape->stride;
	vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	memset(&vertex_input, 0, sizeof(vertex_input));
	vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertex_input.vertexBindingDescriptionCount = 1;
	vertex_input.pVertexBindingDescriptions = &vertex_binding;
	vertex_input.vertexAttributeDescriptionCount = shape->attribute_count;
	vertex_input.pVertexAttributeDescriptions = shape->attributes;
	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = shape->topology;

	/* The whole image, fixed. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)VKSTAGE_SIZE;
	viewport.height = (float)VKSTAGE_SIZE;
	viewport.maxDepth = 1.0f;
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent.width = VKSTAGE_SIZE;
	scissor.extent.height = VKSTAGE_SIZE;
	memset(&viewport_state, 0, sizeof(viewport_state));
	viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport_state.viewportCount = 1;
	viewport_state.pViewports = &viewport;
	viewport_state.scissorCount = 1;
	viewport_state.pScissors = &scissor;

	/* Filled triangles, both faces, one sample. */
	memset(&raster, 0, sizeof(raster));
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;
	memset(&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	/* With depth, the nearer fragment wins and writes its depth. */
	memset(&depth, 0, sizeof(depth));
	depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	if (shape->depth) {
		depth.depthTestEnable = VK_TRUE;
		depth.depthWriteEnable = VK_TRUE;
		depth.depthCompareOp = VK_COMPARE_OP_LESS;
		depth.maxDepthBounds = 1.0f;
	}

	/* No blending, every channel written. */
	memset(&blend_attachment, 0, sizeof(blend_attachment));
	blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	memset(&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1;
	blend.pAttachments = &blend_attachment;

	/* Creates the pipeline for subpass 0. */
	memset(&pipeline_info, 0, sizeof(pipeline_info));
	pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline_info.stageCount = 2;
	pipeline_info.pStages = shader_stages;
	pipeline_info.pVertexInputState = &vertex_input;
	pipeline_info.pInputAssemblyState = &assembly;
	pipeline_info.pViewportState = &viewport_state;
	pipeline_info.pRasterizationState = &raster;
	pipeline_info.pMultisampleState = &multisample;
	if (shape->depth)
		pipeline_info.pDepthStencilState = &depth;
	pipeline_info.pColorBlendState = &blend;
	pipeline_info.layout = run->pipeline_layout;
	pipeline_info.renderPass = run->render_pass;
	run->failed_call = "vkCreateGraphicsPipelines";
	status = vkCreateGraphicsPipelines(run->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &run->pipeline);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the pipeline is ready to bind. */
	return VK_SUCCESS;
}

/*
 * Records and runs one render pass that clears to the background (and the
 * depth to 1.0), draws vertex_count vertices from buffer slot vertices and
 * copies the colour image to buffer slot 0.
 */
static VkResult
vkstage_draw(
	struct vkstage *run,
	uint32_t vertices,
	uint32_t vertex_count,
	int depth,
	uint32_t colour)
{
	VkRenderPassBeginInfo pass_begin;
	VkClearValue clears[2];
	VkDeviceSize offset;
	VkResult status;

	/* Starts recording. */
	status = vkstage_begin(run);
	if (status != VK_SUCCESS)
		return status;

	/* The background and the far depth. */
	memset(clears, 0, sizeof(clears));
	memcpy(clears[0].color.float32, background, sizeof(background));
	clears[1].depthStencil.depth = 1.0f;

	/* The pass over the whole image. */
	memset(&pass_begin, 0, sizeof(pass_begin));
	pass_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass_begin.renderPass = run->render_pass;
	pass_begin.framebuffer = run->framebuffer;
	pass_begin.renderArea.extent.width = VKSTAGE_SIZE;
	pass_begin.renderArea.extent.height = VKSTAGE_SIZE;
	pass_begin.clearValueCount = 1;
	if (depth)
		pass_begin.clearValueCount = 2;
	pass_begin.pClearValues = clears;
	vkCmdBeginRenderPass(run->command, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);

	/* The draw, with the texture's set when there is one. */
	vkCmdBindPipeline(run->command, VK_PIPELINE_BIND_POINT_GRAPHICS, run->pipeline);
	if (run->set != VK_NULL_HANDLE)
		vkCmdBindDescriptorSets(run->command, VK_PIPELINE_BIND_POINT_GRAPHICS, run->pipeline_layout, 0, 1, &run->set, 0, NULL);
	offset = 0;
	vkCmdBindVertexBuffers(run->command, 0, 1, &run->buffers[vertices], &offset);
	vkCmdDraw(run->command, vertex_count, 1, 0, 0);
	vkCmdEndRenderPass(run->command);

	/* The colour image, already TRANSFER_SRC, into buffer 0 for the host. */
	vkstage_copy_out(run, run->images[colour], run->buffers[0], VKSTAGE_SIZE);
	vkstage_host_barrier(run);

	/* Runs it. */
	status = vkstage_submit(run);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: buffer 0 holds the rendered image. */
	return VK_SUCCESS;
}

/* Says whether every channel of a pixel is within the tolerance of wanted; writes the reason when not. */
static int
vkstage_check_pixel(
	struct vkstage *run,
	const uint32_t *pixels,
	uint32_t x,
	uint32_t y,
	uint32_t wanted)
{
	uint32_t seen;
	uint32_t shift;
	int difference;

	/* Compares each 8-bit channel. */
	seen = pixels[y * VKSTAGE_SIZE + x];
	for (shift = 0; shift < 32; shift += 8) {
		difference = (int)((seen >> shift) & 0xffU) - (int)((wanted >> shift) & 0xffU);
		if (difference > VKSTAGE_TOLERANCE || difference < -VKSTAGE_TOLERANCE)
			break;
	}

	/* A channel out of the tolerance fails with the pixel. */
	if (shift < 32) {
		snprintf(run->reason, sizeof(run->reason), "pixel (%u,%u) 0x%08x, expected 0x%08x", x, y, seen, wanted);
		return 0;
	}

	/* Succeeded: the pixel matches. */
	return 1;
}

/* The word of an R8G8B8A8 pixel as the host reads it (little endian). */
static uint32_t
vkstage_rgba(
	uint32_t red,
	uint32_t green,
	uint32_t blue,
	uint32_t alpha)
{
	/* Red is the lowest byte. */
	return red | (green << 8) | (blue << 16) | (alpha << 24);
}

/* clear: vkCmdClearColorImage on an OPTIMAL image, every pixel read back. */
static VkResult
stage_clear(
	struct vkstage *run)
{
	VkClearColorValue colour;
	VkImageSubresourceRange range;
	const uint32_t *pixels;
	uint32_t buffer;
	uint32_t image;
	uint32_t wanted;
	uint32_t i;
	VkResult status;
	int matches;

	/* The image and the buffer it is copied into. */
	status = vkstage_create_image(run, VK_FORMAT_R8G8B8A8_UNORM, VKSTAGE_SIZE, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &image);
	if (status != VK_SUCCESS)
		return status;
	status = vkstage_create_buffer(run, VKSTAGE_PIXELS * 4U, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &buffer);
	if (status != VK_SUCCESS)
		return status;

	/* Records the clear and the copy out. */
	status = vkstage_begin(run);
	if (status != VK_SUCCESS)
		return status;
	vkstage_image_barrier(run, run->images[image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	memset(&colour, 0, sizeof(colour));
	memcpy(colour.float32, clear_colour, sizeof(clear_colour));
	memset(&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1;
	range.layerCount = 1;
	vkCmdClearColorImage(run->command, run->images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
	vkstage_image_barrier(run, run->images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkstage_copy_out(run, run->images[image], run->buffers[buffer], VKSTAGE_SIZE);
	vkstage_host_barrier(run);
	status = vkstage_submit(run);
	if (status != VK_SUCCESS)
		return status;

	/* Every pixel is the clear colour (0.75 rounds to 191). */
	pixels = run->mapped[buffer];
	wanted = vkstage_rgba(64, 128, 191, 255);
	for (i = 0; i < VKSTAGE_PIXELS; i++) {
		matches = vkstage_check_pixel(run, pixels, i % VKSTAGE_SIZE, i / VKSTAGE_SIZE, wanted);
		if (!matches)
			return VK_ERROR_UNKNOWN;
	}

	/* Succeeded: the whole image holds the colour. */
	snprintf(run->seen, sizeof(run->seen), "%u/%u pixels 0x%08x", VKSTAGE_PIXELS, VKSTAGE_PIXELS, wanted);
	return VK_SUCCESS;
}

/* copy: a pattern from a buffer into an OPTIMAL image and back to another buffer. */
static VkResult
stage_copy(
	struct vkstage *run)
{
	VkBufferImageCopy region;
	uint32_t *source;
	const uint32_t *back;
	uint32_t in;
	uint32_t out;
	uint32_t image;
	uint32_t i;
	VkResult status;

	/* The two buffers and the image between them. */
	status = vkstage_create_buffer(run, VKSTAGE_PIXELS * 4U, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &out);
	if (status != VK_SUCCESS)
		return status;
	status = vkstage_create_buffer(run, VKSTAGE_PIXELS * 4U, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &in);
	if (status != VK_SUCCESS)
		return status;
	status = vkstage_create_image(run, VK_FORMAT_R8G8B8A8_UNORM, VKSTAGE_SIZE, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &image);
	if (status != VK_SUCCESS)
		return status;

	/* A pattern in which every word differs from its neighbours. */
	source = run->mapped[in];
	for (i = 0; i < VKSTAGE_PIXELS; i++)
		source[i] = i * 0x9e3779b1U + 0x01234567U;

	/* Records the copy in and the copy out. */
	status = vkstage_begin(run);
	if (status != VK_SUCCESS)
		return status;
	vkstage_image_barrier(run, run->images[image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	memset(&region, 0, sizeof(region));
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent.width = VKSTAGE_SIZE;
	region.imageExtent.height = VKSTAGE_SIZE;
	region.imageExtent.depth = 1;
	vkCmdCopyBufferToImage(run->command, run->buffers[in], run->images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	vkstage_image_barrier(run, run->images[image], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	vkstage_copy_out(run, run->images[image], run->buffers[out], VKSTAGE_SIZE);
	vkstage_host_barrier(run);
	status = vkstage_submit(run);
	if (status != VK_SUCCESS)
		return status;

	/* Every word came back unchanged. */
	back = run->mapped[out];
	for (i = 0; i < VKSTAGE_PIXELS; i++) {
		if (back[i] != source[i])
			break;
	}

	/* A changed word fails with its place. */
	if (i < VKSTAGE_PIXELS) {
		snprintf(run->reason, sizeof(run->reason), "word %u 0x%08x, expected 0x%08x", i, back[i], source[i]);
		return VK_ERROR_UNKNOWN;
	}

	/* Succeeded: the round trip kept every word. */
	snprintf(run->seen, sizeof(run->seen), "%u/%u words", VKSTAGE_PIXELS, VKSTAGE_PIXELS);
	return VK_SUCCESS;
}

/* draw: one green triangle on the blue background; its middle and a corner. */
static VkResult
stage_draw(
	struct vkstage *run)
{
	static const struct colour_vertex triangle[3] = {
		{ { -0.5f, -0.5f, 0.5f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
		{ { 0.5f, -0.5f, 0.5f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
		{ { 0.0f, 0.5f, 0.5f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
	};
	static const VkVertexInputAttributeDescription attributes[2] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12 },
	};
	struct pipeline_shape shape;
	const uint32_t *pixels;
	uint32_t readback;
	uint32_t vertices;
	uint32_t colour;
	VkResult status;
	int matches;

	/* The readback, the vertices and the colour image. */
	status = vkstage_create_buffer(run, VKSTAGE_PIXELS * 4U, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback);
	if (status != VK_SUCCESS)
		return status;
	status = vkstage_create_buffer(run, sizeof(triangle), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vertices);
	if (status != VK_SUCCESS)
		return status;
	memcpy(run->mapped[vertices], triangle, sizeof(triangle));
	status = vkstage_create_image(run, VK_FORMAT_R8G8B8A8_UNORM, VKSTAGE_SIZE, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &colour);
	if (status != VK_SUCCESS)
		return status;

	/* The pass and the pipeline. */
	status = vkstage_create_render_pass(run, 0, colour, 0);
	if (status != VK_SUCCESS)
		return status;
	memset(&shape, 0, sizeof(shape));
	shape.vertex = vkstage_colour_vert;
	shape.vertex_size = sizeof(vkstage_colour_vert);
	shape.fragment = vkstage_colour_frag;
	shape.fragment_size = sizeof(vkstage_colour_frag);
	shape.attributes = attributes;
	shape.attribute_count = 2;
	shape.stride = sizeof(struct colour_vertex);
	shape.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	status = vkstage_create_pipeline(run, &shape);
	if (status != VK_SUCCESS)
		return status;

	/* Draws. */
	status = vkstage_draw(run, vertices, 3, 0, colour);
	if (status != VK_SUCCESS)
		return status;

	/* The middle is the triangle's green, the corners and the top edge the background. */
	pixels = run->mapped[readback];
	matches = vkstage_check_pixel(run, pixels, VKSTAGE_SIZE / 2U, VKSTAGE_SIZE / 2U, vkstage_rgba(0, 255, 0, 255));
	if (!matches)
		return VK_ERROR_UNKNOWN;
	matches = vkstage_check_pixel(run, pixels, 2, 2, vkstage_rgba(0, 0, 255, 255));
	if (!matches)
		return VK_ERROR_UNKNOWN;
	matches = vkstage_check_pixel(run, pixels, VKSTAGE_SIZE - 3U, VKSTAGE_SIZE - 3U, vkstage_rgba(0, 0, 255, 255));
	if (!matches)
		return VK_ERROR_UNKNOWN;
	matches = vkstage_check_pixel(run, pixels, VKSTAGE_SIZE / 2U, 4, vkstage_rgba(0, 0, 255, 255));
	if (!matches)
		return VK_ERROR_UNKNOWN;

	/* Succeeded: the triangle is where it was drawn and nowhere else. */
	snprintf(run->seen, sizeof(run->seen), "triangle 0x%08x, background 0x%08x", pixels[(VKSTAGE_SIZE / 2U) * VKSTAGE_SIZE + VKSTAGE_SIZE / 2U], pixels[2 * VKSTAGE_SIZE + 2]);
	return VK_SUCCESS;
}

/* sample: a full-image quad that samples a 4x4 texture of four coloured quadrants. */
static VkResult
stage_sample(
	struct vkstage *run)
{
	static const struct texture_vertex quad[4] = {
		{ { -1.0f, -1.0f }, { 0.0f, 0.0f } },
		{ { 1.0f, -1.0f }, { 1.0f, 0.0f } },
		{ { -1.0f, 1.0f }, { 0.0f, 1.0f } },
		{ { 1.0f, 1.0f }, { 1.0f, 1.0f } },
	};
	static const VkVertexInputAttributeDescription attributes[2] = {
		{ 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R32G32_SFLOAT, 8 },
	};
	uint32_t quadrants[4];
	struct pipeline_shape shape;
	VkSamplerCreateInfo sampler_info;
	VkDescriptorPoolSize size;
	VkDescriptorPoolCreateInfo pool_info;
	VkDescriptorSetAllocateInfo set_info;
	VkDescriptorImageInfo image_info;
	VkWriteDescriptorSet write;
	VkBufferImageCopy region;
	const uint32_t *pixels;
	uint32_t *texels;
	uint32_t readback;
	uint32_t vertices;
	uint32_t upload;
	uint32_t texture;
	uint32_t colour;
	uint32_t x;
	uint32_t y;
	uint32_t quadrant;
	VkResult status;
	int matches;

	/* Red, green, blue and white: top left, top right, bottom left, bottom right. */
	quadrants[0] = vkstage_rgba(255, 0, 0, 255);
	quadrants[1] = vkstage_rgba(0, 255, 0, 255);
	quadrants[2] = vkstage_rgba(0, 0, 255, 255);
	quadrants[3] = vkstage_rgba(255, 255, 255, 255);

	/* The readback, the vertices, the texture's upload, the texture and the colour image. */
	status = vkstage_create_buffer(run, VKSTAGE_PIXELS * 4U, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback);
	if (status != VK_SUCCESS)
		return status;
	status = vkstage_create_buffer(run, sizeof(quad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vertices);
	if (status != VK_SUCCESS)
		return status;
	memcpy(run->mapped[vertices], quad, sizeof(quad));
	status = vkstage_create_buffer(run, VKSTAGE_TEXTURE * VKSTAGE_TEXTURE * 4U, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &upload);
	if (status != VK_SUCCESS)
		return status;
	status = vkstage_create_image(run, VK_FORMAT_R8G8B8A8_UNORM, VKSTAGE_TEXTURE, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &texture);
	if (status != VK_SUCCESS)
		return status;
	status = vkstage_create_image(run, VK_FORMAT_R8G8B8A8_UNORM, VKSTAGE_SIZE, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &colour);
	if (status != VK_SUCCESS)
		return status;

	/* The texels: each 2x2 quadrant one colour. */
	texels = run->mapped[upload];
	for (y = 0; y < VKSTAGE_TEXTURE; y++) {
		for (x = 0; x < VKSTAGE_TEXTURE; x++) {
			quadrant = (y / 2U) * 2U + x / 2U;
			texels[y * VKSTAGE_TEXTURE + x] = quadrants[quadrant];
		}
	}

	/* Uploads the texture and leaves it ready for the fragment shader. */
	status = vkstage_begin(run);
	if (status != VK_SUCCESS)
		return status;
	vkstage_image_barrier(run, run->images[texture], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	memset(&region, 0, sizeof(region));
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent.width = VKSTAGE_TEXTURE;
	region.imageExtent.height = VKSTAGE_TEXTURE;
	region.imageExtent.depth = 1;
	vkCmdCopyBufferToImage(run->command, run->buffers[upload], run->images[texture], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
	vkstage_image_barrier(run, run->images[texture], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
	status = vkstage_submit(run);
	if (status != VK_SUCCESS)
		return status;

	/* A nearest, clamped sampler. */
	memset(&sampler_info, 0, sizeof(sampler_info));
	sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler_info.magFilter = VK_FILTER_NEAREST;
	sampler_info.minFilter = VK_FILTER_NEAREST;
	sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	run->failed_call = "vkCreateSampler";
	status = vkCreateSampler(run->device, &sampler_info, NULL, &run->sampler);
	if (status != VK_SUCCESS)
		return status;

	/* The pass and the pipeline with the texture's set. */
	status = vkstage_create_render_pass(run, 0, colour, 0);
	if (status != VK_SUCCESS)
		return status;
	memset(&shape, 0, sizeof(shape));
	shape.vertex = vkstage_texture_vert;
	shape.vertex_size = sizeof(vkstage_texture_vert);
	shape.fragment = vkstage_texture_frag;
	shape.fragment_size = sizeof(vkstage_texture_frag);
	shape.attributes = attributes;
	shape.attribute_count = 2;
	shape.stride = sizeof(struct texture_vertex);
	shape.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	shape.texture = 1;
	status = vkstage_create_pipeline(run, &shape);
	if (status != VK_SUCCESS)
		return status;

	/* The set that names the texture and the sampler. */
	memset(&size, 0, sizeof(size));
	size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	size.descriptorCount = 1;
	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.maxSets = 1;
	pool_info.poolSizeCount = 1;
	pool_info.pPoolSizes = &size;
	run->failed_call = "vkCreateDescriptorPool";
	status = vkCreateDescriptorPool(run->device, &pool_info, NULL, &run->pool);
	if (status != VK_SUCCESS)
		return status;
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	set_info.descriptorPool = run->pool;
	set_info.descriptorSetCount = 1;
	set_info.pSetLayouts = &run->set_layout;
	run->failed_call = "vkAllocateDescriptorSets";
	status = vkAllocateDescriptorSets(run->device, &set_info, &run->set);
	if (status != VK_SUCCESS)
		return status;
	memset(&image_info, 0, sizeof(image_info));
	image_info.sampler = run->sampler;
	image_info.imageView = run->views[texture];
	image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = run->set;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image_info;
	vkUpdateDescriptorSets(run->device, 1, &write, 0, NULL);

	/* Draws the quad as a strip. */
	status = vkstage_draw(run, vertices, 4, 0, colour);
	if (status != VK_SUCCESS)
		return status;

	/* The middle of each quarter of the image is that quadrant's colour. */
	pixels = run->mapped[readback];
	for (quadrant = 0; quadrant < 4; quadrant++) {
		x = VKSTAGE_SIZE / 4U + (quadrant % 2U) * (VKSTAGE_SIZE / 2U);
		y = VKSTAGE_SIZE / 4U + (quadrant / 2U) * (VKSTAGE_SIZE / 2U);
		matches = vkstage_check_pixel(run, pixels, x, y, quadrants[quadrant]);
		if (!matches)
			return VK_ERROR_UNKNOWN;
	}

	/* Succeeded: the texture landed the right way up and round. */
	snprintf(run->seen, sizeof(run->seen), "4/4 quadrants");
	return VK_SUCCESS;
}

/* depth: a near red triangle drawn before a far green one that covers the image. */
static VkResult
stage_depth(
	struct vkstage *run)
{
	static const struct colour_vertex triangles[6] = {
		{ { -0.5f, -0.5f, 0.25f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
		{ { 0.5f, -0.5f, 0.25f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
		{ { 0.0f, 0.5f, 0.25f }, { 1.0f, 0.0f, 0.0f, 1.0f } },
		{ { -1.0f, -1.0f, 0.75f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
		{ { 3.0f, -1.0f, 0.75f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
		{ { -1.0f, 3.0f, 0.75f }, { 0.0f, 1.0f, 0.0f, 1.0f } },
	};
	static const VkVertexInputAttributeDescription attributes[2] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
		{ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12 },
	};
	VkImageFormatProperties depth_properties;
	struct pipeline_shape shape;
	const uint32_t *pixels;
	uint32_t readback;
	uint32_t vertices;
	uint32_t colour;
	uint32_t depth;
	VkResult status;
	int matches;

	/*
	 * The depth attachment is D32_SFLOAT, which a device need not offer
	 * (a GPU reports each format's features on its own): without it the
	 * stage is skipped rather than failed.
	 */
	status = vkGetPhysicalDeviceImageFormatProperties(run->physical, VK_FORMAT_D32_SFLOAT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
	    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, &depth_properties);
	if (status == VK_ERROR_FORMAT_NOT_SUPPORTED) {
		snprintf(run->skipped, sizeof(run->skipped), "no D32_SFLOAT depth attachment");
		return VK_SUCCESS;
	}

	/* Another failure of the query is the device's. */
	if (status != VK_SUCCESS) {
		run->failed_call = "vkGetPhysicalDeviceImageFormatProperties";
		return status;
	}

	/* The readback, the vertices, the colour image and the depth image. */
	status = vkstage_create_buffer(run, VKSTAGE_PIXELS * 4U, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback);
	if (status != VK_SUCCESS)
		return status;
	status = vkstage_create_buffer(run, sizeof(triangles), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vertices);
	if (status != VK_SUCCESS)
		return status;
	memcpy(run->mapped[vertices], triangles, sizeof(triangles));
	status = vkstage_create_image(run, VK_FORMAT_R8G8B8A8_UNORM, VKSTAGE_SIZE, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &colour);
	if (status != VK_SUCCESS)
		return status;
	status = vkstage_create_image(run, VK_FORMAT_D32_SFLOAT, VKSTAGE_SIZE, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, &depth);
	if (status != VK_SUCCESS)
		return status;

	/* The pass with depth and the pipeline that tests it. */
	status = vkstage_create_render_pass(run, 1, colour, depth);
	if (status != VK_SUCCESS)
		return status;
	memset(&shape, 0, sizeof(shape));
	shape.vertex = vkstage_colour_vert;
	shape.vertex_size = sizeof(vkstage_colour_vert);
	shape.fragment = vkstage_colour_frag;
	shape.fragment_size = sizeof(vkstage_colour_frag);
	shape.attributes = attributes;
	shape.attribute_count = 2;
	shape.stride = sizeof(struct colour_vertex);
	shape.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	shape.depth = 1;
	status = vkstage_create_pipeline(run, &shape);
	if (status != VK_SUCCESS)
		return status;

	/* Draws both, the near one first. */
	status = vkstage_draw(run, vertices, 6, 1, colour);
	if (status != VK_SUCCESS)
		return status;

	/* The middle keeps the near red; a corner only the far green covers is green. */
	pixels = run->mapped[readback];
	matches = vkstage_check_pixel(run, pixels, VKSTAGE_SIZE / 2U, VKSTAGE_SIZE / 2U, vkstage_rgba(255, 0, 0, 255));
	if (!matches)
		return VK_ERROR_UNKNOWN;
	matches = vkstage_check_pixel(run, pixels, 2, 2, vkstage_rgba(0, 255, 0, 255));
	if (!matches)
		return VK_ERROR_UNKNOWN;

	/* What was seen, for the stage's line. */
	snprintf(run->seen, sizeof(run->seen), "near 0x%08x over far 0x%08x", pixels[(VKSTAGE_SIZE / 2U) * VKSTAGE_SIZE + VKSTAGE_SIZE / 2U], pixels[2 * VKSTAGE_SIZE + 2]);

	/* Succeeded: the depth test kept the nearer triangle. */
	return VK_SUCCESS;
}

/*
 * Opens a surface on the first display's first mode and the first plane
 * that can show that display; reports the mode's size.
 */
static VkResult
vkstage_open_surface(
	struct vkstage *run,
	VkExtent2D *extent)
{
	VkDisplayPropertiesKHR displays[4];
	VkDisplayModePropertiesKHR modes[8];
	VkDisplayPlanePropertiesKHR planes[8];
	VkDisplayKHR supported[4];
	VkDisplayPlaneCapabilitiesKHR capabilities;
	VkDisplaySurfaceCreateInfoKHR surface_info;
	uint32_t count;
	uint32_t plane_count;
	uint32_t plane;
	uint32_t i;
	VkResult status;

	/* The first display. */
	count = sizeof(displays) / sizeof(displays[0]);
	run->failed_call = "vkGetPhysicalDeviceDisplayPropertiesKHR";
	status = vkGetPhysicalDeviceDisplayPropertiesKHR(run->physical, &count, displays);
	if (status != VK_SUCCESS && status != VK_INCOMPLETE)
		return status;
	run->failed_call = "no display";
	if (count == 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Its first mode. */
	count = sizeof(modes) / sizeof(modes[0]);
	run->failed_call = "vkGetDisplayModePropertiesKHR";
	status = vkGetDisplayModePropertiesKHR(run->physical, displays[0].display, &count, modes);
	if (status != VK_SUCCESS && status != VK_INCOMPLETE)
		return status;
	run->failed_call = "no display mode";
	if (count == 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The planes. */
	plane_count = sizeof(planes) / sizeof(planes[0]);
	run->failed_call = "vkGetPhysicalDeviceDisplayPlanePropertiesKHR";
	status = vkGetPhysicalDeviceDisplayPlanePropertiesKHR(run->physical, &plane_count, planes);
	if (status != VK_SUCCESS && status != VK_INCOMPLETE)
		return status;

	/* The first plane that names the display among those it can show. */
	for (plane = 0; plane < plane_count; plane++) {
		count = sizeof(supported) / sizeof(supported[0]);
		status = vkGetDisplayPlaneSupportedDisplaysKHR(run->physical, plane, &count, supported);
		if (status != VK_SUCCESS && status != VK_INCOMPLETE)
			continue;
		for (i = 0; i < count; i++) {
			if (supported[i] == displays[0].display)
				break;
		}
		if (i < count)
			break;
	}
	run->failed_call = "no plane for the display";
	if (plane == plane_count)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The plane's alpha modes; opaque when it has it, else its first. */
	run->failed_call = "vkGetDisplayPlaneCapabilitiesKHR";
	status = vkGetDisplayPlaneCapabilitiesKHR(run->physical, modes[0].displayMode, plane, &capabilities);
	if (status != VK_SUCCESS)
		return status;

	/* Creates the surface over the whole mode. */
	memset(&surface_info, 0, sizeof(surface_info));
	surface_info.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
	surface_info.displayMode = modes[0].displayMode;
	surface_info.planeIndex = plane;
	surface_info.planeStackIndex = planes[plane].currentStackIndex;
	surface_info.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	surface_info.globalAlpha = 1.0f;
	surface_info.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
	if ((capabilities.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR) == 0) {
		for (i = 0; i < 4 && (capabilities.supportedAlpha & (1U << i)) == 0; i++)
			continue;
		surface_info.alphaMode = (VkDisplayPlaneAlphaFlagBitsKHR)(1U << i);
	}
	surface_info.imageExtent = modes[0].parameters.visibleRegion;
	run->failed_call = "vkCreateDisplayPlaneSurfaceKHR";
	status = vkCreateDisplayPlaneSurfaceKHR(run->instance, &surface_info, NULL, &run->surface);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the surface and its size. */
	*extent = modes[0].parameters.visibleRegion;
	return VK_SUCCESS;
}

/*
 * present: one frame cleared to the clear colour on the first display,
 * held three seconds, then back.  On PowerVR the swapchain images are
 * OPTIMAL and libvulkan's WSI presents them by copying each to a
 * host-visible buffer and the display (the readback route).
 */
static VkResult
stage_present(
	struct vkstage *run)
{
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR formats[16];
	VkSwapchainCreateInfoKHR swapchain_info;
	VkImage images[8];
	VkClearColorValue colour;
	VkImageSubresourceRange range;
	VkPresentInfoKHR present;
	VkSemaphoreCreateInfo semaphore_info;
	VkSubmitInfo submit;
	VkPipelineStageFlags wait_stage;
	VkExtent2D extent;
	struct timespec pause;
	VkBool32 supported;
	uint32_t format_count;
	uint32_t image_count;
	uint32_t index;
	VkResult status;

	/* The surface on the display. */
	status = vkstage_open_surface(run, &extent);
	if (status != VK_SUCCESS)
		return status;

	/* The queue must be able to present to it. */
	supported = VK_FALSE;
	run->failed_call = "vkGetPhysicalDeviceSurfaceSupportKHR";
	status = vkGetPhysicalDeviceSurfaceSupportKHR(run->physical, run->family, run->surface, &supported);
	if (status != VK_SUCCESS)
		return status;
	run->failed_call = "present support";
	if (!supported)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* Its capabilities: the images must take a transfer clear. */
	run->failed_call = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR";
	status = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(run->physical, run->surface, &capabilities);
	if (status != VK_SUCCESS)
		return status;
	run->failed_call = "transfer-destination swapchain images";
	if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
		return VK_ERROR_FEATURE_NOT_PRESENT;
	if (capabilities.currentExtent.width != UINT32_MAX)
		extent = capabilities.currentExtent;

	/* Its first format. */
	format_count = sizeof(formats) / sizeof(formats[0]);
	run->failed_call = "vkGetPhysicalDeviceSurfaceFormatsKHR";
	status = vkGetPhysicalDeviceSurfaceFormatsKHR(run->physical, run->surface, &format_count, formats);
	if (status != VK_SUCCESS && status != VK_INCOMPLETE)
		return status;
	run->failed_call = "no surface format";
	if (format_count == 0)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* The smallest FIFO swapchain. */
	memset(&swapchain_info, 0, sizeof(swapchain_info));
	swapchain_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	swapchain_info.surface = run->surface;
	swapchain_info.minImageCount = capabilities.minImageCount;
	swapchain_info.imageFormat = formats[0].format;
	swapchain_info.imageColorSpace = formats[0].colorSpace;
	swapchain_info.imageExtent = extent;
	swapchain_info.imageArrayLayers = 1;
	swapchain_info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	swapchain_info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	swapchain_info.clipped = VK_TRUE;
	run->failed_call = "vkCreateSwapchainKHR";
	status = vkCreateSwapchainKHR(run->device, &swapchain_info, NULL, &run->swapchain);
	if (status != VK_SUCCESS)
		return status;
	image_count = sizeof(images) / sizeof(images[0]);
	run->failed_call = "vkGetSwapchainImagesKHR";
	status = vkGetSwapchainImagesKHR(run->device, run->swapchain, &image_count, images);
	if (status != VK_SUCCESS && status != VK_INCOMPLETE)
		return status;

	/* The two semaphores: the image acquired, the clear done. */
	memset(&semaphore_info, 0, sizeof(semaphore_info));
	semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	run->failed_call = "vkCreateSemaphore";
	status = vkCreateSemaphore(run->device, &semaphore_info, NULL, &run->acquired);
	if (status != VK_SUCCESS)
		return status;
	status = vkCreateSemaphore(run->device, &semaphore_info, NULL, &run->rendered);
	if (status != VK_SUCCESS)
		return status;

	/* Acquires an image, signalling the semaphore and the fence. */
	run->failed_call = "vkAcquireNextImageKHR";
	status = vkAcquireNextImageKHR(run->device, run->swapchain, VKSTAGE_WAIT_NS, run->acquired, run->fence, &index);
	if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR)
		return status;
	run->failed_call = "vkWaitForFences (acquire)";
	status = vkWaitForFences(run->device, 1, &run->fence, VK_TRUE, VKSTAGE_WAIT_NS);
	if (status != VK_SUCCESS)
		return status;
	run->failed_call = "vkResetFences";
	status = vkResetFences(run->device, 1, &run->fence);
	if (status != VK_SUCCESS)
		return status;
	run->failed_call = "swapchain image index";
	if (index >= image_count)
		return VK_ERROR_UNKNOWN;

	/* Clears it and hands it to the display. */
	status = vkstage_begin(run);
	if (status != VK_SUCCESS)
		return status;
	vkstage_image_barrier(run, images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	memset(&colour, 0, sizeof(colour));
	memcpy(colour.float32, clear_colour, sizeof(clear_colour));
	memset(&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1;
	range.layerCount = 1;
	vkCmdClearColorImage(run->command, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
	vkstage_image_barrier(run, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	run->failed_call = "vkEndCommandBuffer";
	status = vkEndCommandBuffer(run->command);
	if (status != VK_SUCCESS)
		return status;

	/* Submits it after the acquire, signalling the clear's semaphore and the fence. */
	wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores = &run->acquired;
	submit.pWaitDstStageMask = &wait_stage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &run->command;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &run->rendered;
	run->failed_call = "vkQueueSubmit";
	status = vkQueueSubmit(run->queue, 1, &submit, run->fence);
	if (status != VK_SUCCESS)
		return status;
	run->failed_call = "vkWaitForFences";
	status = vkWaitForFences(run->device, 1, &run->fence, VK_TRUE, VKSTAGE_WAIT_NS);
	if (status != VK_SUCCESS)
		return status;

	/* Presents it after the clear: the WSI copies it out to the display (the readback route). */
	memset(&present, 0, sizeof(present));
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &run->rendered;
	present.swapchainCount = 1;
	present.pSwapchains = &run->swapchain;
	present.pImageIndices = &index;
	run->failed_call = "vkQueuePresentKHR";
	status = vkQueuePresentKHR(run->queue, &present);
	if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR)
		return status;

	/* Leaves the frame up for three seconds, then lets the queue drain. */
	pause.tv_sec = VKSTAGE_PRESENT_SECONDS;
	pause.tv_nsec = 0;
	nanosleep(&pause, NULL);
	run->failed_call = "vkQueueWaitIdle";
	status = vkQueueWaitIdle(run->queue);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: one frame went to the display. */
	snprintf(run->seen, sizeof(run->seen), "1 frame %ux%u format %d, %u s", extent.width, extent.height, (int)formats[0].format, VKSTAGE_PRESENT_SECONDS);
	return VK_SUCCESS;
}

/* Destroys every object the run created, newest first. */
static void
vkstage_destroy(
	struct vkstage *run)
{
	uint32_t i;

	/* Leaves the instance alone when no device was created; the WSI calls only with their objects. */
	if (run->device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(run->device);
		if (run->swapchain != VK_NULL_HANDLE)
			vkDestroySwapchainKHR(run->device, run->swapchain, NULL);
		vkDestroySemaphore(run->device, run->rendered, NULL);
		vkDestroySemaphore(run->device, run->acquired, NULL);
		vkDestroyDescriptorPool(run->device, run->pool, NULL);
		vkDestroyPipeline(run->device, run->pipeline, NULL);
		vkDestroyPipelineLayout(run->device, run->pipeline_layout, NULL);
		vkDestroyDescriptorSetLayout(run->device, run->set_layout, NULL);
		vkDestroySampler(run->device, run->sampler, NULL);
		vkDestroyShaderModule(run->device, run->modules[1], NULL);
		vkDestroyShaderModule(run->device, run->modules[0], NULL);
		vkDestroyFramebuffer(run->device, run->framebuffer, NULL);
		vkDestroyRenderPass(run->device, run->render_pass, NULL);
		for (i = 0; i < run->image_count; i++) {
			vkDestroyImageView(run->device, run->views[i], NULL);
			vkDestroyImage(run->device, run->images[i], NULL);
			vkFreeMemory(run->device, run->image_memories[i], NULL);
		}
		for (i = 0; i < run->buffer_count; i++) {
			vkDestroyBuffer(run->device, run->buffers[i], NULL);
			vkFreeMemory(run->device, run->buffer_memories[i], NULL);
		}
		vkDestroyFence(run->device, run->fence, NULL);
		vkDestroyCommandPool(run->device, run->command_pool, NULL);
		vkDestroyDevice(run->device, NULL);
	}

	/* Destroys the surface and the instance last. */
	if (run->instance != VK_NULL_HANDLE) {
		if (run->surface != VK_NULL_HANDLE)
			vkDestroySurfaceKHR(run->instance, run->surface, NULL);
		vkDestroyInstance(run->instance, NULL);
	}
}
