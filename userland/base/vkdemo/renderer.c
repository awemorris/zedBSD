/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Render the original textured cuboid through the public Vulkan 1.0 API.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "../common/sha256.h"
#include "display.h"
#include "renderer.h"
#include "shaders.h"

#define TEXTURE_WIDTH 64U
#define TEXTURE_BYTES (TEXTURE_WIDTH * TEXTURE_WIDTH * 4U)
#define TEXTURE_OFFSET 4096U
#define UPLOAD_BYTES (TEXTURE_OFFSET + TEXTURE_BYTES)
#define VERTEX_COUNT 36U
#define VERTEX_STRIDE 20U
#define GPU_WAIT_NS UINT64_C(10000000000)

/* One application image retains its allocation until all GPU use has ended. */
struct demo_image {
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
};

/* One mapped buffer retains the actual memory attributes used for visibility. */
struct demo_buffer {
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkMemoryPropertyFlags properties;
	void *mapping;
};

/* One color target owns a view, framebuffer and image-specific present signal. */
struct demo_target {
	VkImage image;
	VkImageView view;
	VkFramebuffer framebuffer;
	VkSemaphore rendered;
};

/* Main serializes one complete scene, its Vulkan objects and its last failure. */
struct demo_renderer {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	VkPhysicalDeviceMemoryProperties memory;
	uint32_t family;
	VkFormat color_format;
	struct vkdemo_display display;
	struct demo_image color;
	struct demo_image depth;
	struct demo_image texture;
	struct demo_buffer upload_buffer;
	struct demo_buffer readback;
	struct demo_target *targets;
	uint32_t target_count;
	uint32_t target_index;
	VkSampler sampler;
	VkDescriptorSetLayout set_layout;
	VkDescriptorPool descriptor_pool;
	VkDescriptorSet descriptor_set;
	VkPipelineLayout pipeline_layout;
	VkRenderPass render_pass;
	VkShaderModule vertex_shader;
	VkShaderModule fragment_shader;
	VkPipeline pipeline;
	VkCommandPool command_pool;
	VkCommandBuffer command;
	VkFence fence;
	VkSemaphore acquired;
	VkResult last_error;
	const char *last_operation;
	int offscreen;
	int ready;
	int has_frame;
	uint8_t upload[UPLOAD_BYTES];
	uint8_t pixels[VKDEMO_BYTES];
};

/* One invocation owns this singleton; no helper or callback accesses it concurrently. */
static struct demo_renderer renderer;

/* Each face lists the positions for UV corners 00, 10, 11, 01 respectively. */
static const float face_corners[6][4][3] = {
	{{0.75f, 0.5f, 0.375f}, {0.75f, 0.5f, -0.375f},
	 {0.75f, -0.5f, -0.375f}, {0.75f, -0.5f, 0.375f}},
	{{-0.75f, 0.5f, -0.375f}, {-0.75f, 0.5f, 0.375f},
	 {-0.75f, -0.5f, 0.375f}, {-0.75f, -0.5f, -0.375f}},
	{{-0.75f, 0.5f, -0.375f}, {0.75f, 0.5f, -0.375f},
	 {0.75f, 0.5f, 0.375f}, {-0.75f, 0.5f, 0.375f}},
	{{-0.75f, -0.5f, 0.375f}, {0.75f, -0.5f, 0.375f},
	 {0.75f, -0.5f, -0.375f}, {-0.75f, -0.5f, -0.375f}},
	{{-0.75f, 0.5f, 0.375f}, {0.75f, 0.5f, 0.375f},
	 {0.75f, -0.5f, 0.375f}, {-0.75f, -0.5f, 0.375f}},
	{{0.75f, 0.5f, -0.375f}, {-0.75f, 0.5f, -0.375f},
	 {-0.75f, -0.5f, -0.375f}, {0.75f, -0.5f, -0.375f}}
};

static void record_error(VkResult status, const char *operation);
static int create_context(uint32_t device_index);
static int choose_queue(void);
static int create_storage(void);
static int allocate_memory(const VkMemoryRequirements *requirements, VkMemoryPropertyFlags flags, VkDeviceMemory *memory, VkMemoryPropertyFlags *properties);
static int create_image(struct demo_image *image, VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage);
static int create_view(VkImage image, VkFormat format, VkImageAspectFlags aspect, VkImageView *view);
static int create_buffer(struct demo_buffer *buffer, VkDeviceSize bytes, VkBufferUsageFlags usage);
static int publish_upload(void);
static int create_descriptors(void);
static int create_render_pass(void);
static int create_targets(void);
static int create_pipeline(void);
static int create_shader(const uint32_t *words, size_t bytes, VkShaderModule *shader);
static int create_commands(void);
static int begin_recording(void);
static int submit_recording(int present);
static void image_barrier(VkImage image, VkImageLayout old_layout, VkImageLayout new_layout, VkAccessFlags source_access, VkAccessFlags destination_access, VkPipelineStageFlags source_stage, VkPipelineStageFlags destination_stage);
static void buffer_barrier(VkBuffer buffer, VkDeviceSize bytes, VkAccessFlags source_access, VkAccessFlags destination_access, VkPipelineStageFlags source_stage, VkPipelineStageFlags destination_stage);
static int upload_texture(void);
static int record_frame(uint32_t milliseconds);
static int read_pixels(void);
static void prepare_upload(void);
static void store_float(uint8_t *destination, float number);
static int hash_pixels(char digest[65]);
static void destroy_image(struct demo_image *image);
static void destroy_buffer(struct demo_buffer *buffer);
static void release_resources(void);

/*
 * Create a standard Vulkan device and retain the original textured scene.
 */
int
vkdemo_initialize(
	uint32_t device_index,
	int offscreen)
{
	VkFormatProperties properties;
	int error;

	/* Keep partial initialization valid for the caller's unconditional close. */
	memset(&renderer, 0, sizeof(renderer));
	renderer.offscreen = offscreen;
	renderer.last_operation = "initialization";

	/* Enumerate an ordinary Vulkan physical device and graphics queue. */
	error = create_context(device_index);
	if (error != 0)
		return -1;

	/* Require actual color-attachment support for the selected surface format. */
	vkGetPhysicalDeviceFormatProperties(renderer.physical, renderer.color_format, &properties);
	if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0) {
		record_error(VK_ERROR_FORMAT_NOT_SUPPORTED, "color format");
		return -1;
	}

	/* Require the existing checker format to support ordinary texture sampling. */
	vkGetPhysicalDeviceFormatProperties(renderer.physical, VK_FORMAT_R8G8B8A8_UNORM, &properties);
	if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
		record_error(VK_ERROR_FORMAT_NOT_SUPPORTED, "texture format");
		return -1;
	}

	/* Preserve the original depth precision when comparing independent images. */
	vkGetPhysicalDeviceFormatProperties(renderer.physical, VK_FORMAT_D32_SFLOAT, &properties);
	if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0) {
		record_error(VK_ERROR_FORMAT_NOT_SUPPORTED, "depth format");
		return -1;
	}

	/* Allocate image and mapped buffer storage through ordinary Vulkan memory APIs. */
	error = create_storage();
	if (error != 0)
		return -1;

	/* Upload only original vertex and texture inputs from the CPU. */
	prepare_upload();
	error = publish_upload();
	if (error != 0)
		return -1;

	/* Bind the sampled checker and one time push constant to the shaders. */
	error = create_descriptors();
	if (error != 0)
		return -1;

	/* Define the color/depth pass and explicit attachment-to-transfer ordering. */
	error = create_render_pass();
	if (error != 0)
		return -1;

	/* Create a framebuffer for every borrowed swapchain or owned offscreen image. */
	error = create_targets();
	if (error != 0)
		return -1;

	/* Compile the original validated SPIR-V into an ordinary graphics pipeline. */
	error = create_pipeline();
	if (error != 0)
		return -1;

	/* Allocate a primary command buffer and explicit completion primitives. */
	error = create_commands();
	if (error != 0)
		return -1;

	/* Transfer the checker to its sampled image before the first draw. */
	error = upload_texture();
	if (error != 0)
		return -1;

	/* Publish scene readiness only after its initial Vulkan work completes. */
	renderer.ready = 1;
	printf("VKDEMO READY vertices=36 texture=64x64 shaders=vertex,fragment\n");
	fflush(stdout);

	/* Succeeded: subsequent frames reuse all scene objects through standard APIs. */
	return 0;
}

/*
 * Draw one shader time into its real color target and read the resulting pixels.
 */
int
vkdemo_render(
	uint32_t milliseconds,
	uint32_t frame,
	char digest[65])
{
	VkPresentInfoKHR present;
	VkResult status;
	int error;

	/* Frame identity belongs to the caller's diagnostic marker, not GPU state. */
	(void)frame;

	/* Refuse a partial scene before acquiring any swapchain image. */
	if (renderer.ready == 0) {
		errno = EINVAL;
		return -1;
	}

	/* Preserve the first Vulkan failure instead of issuing more rendering work. */
	if (renderer.last_error != VK_SUCCESS) {
		errno = EIO;
		return -1;
	}

	/* Select the single owned target when no display was requested. */
	renderer.target_index = 0;
	if (renderer.offscreen == 0) {
		/* Acquire an actual swapchain image and its standard availability signal. */
		status = vkAcquireNextImageKHR(renderer.device, renderer.display.swapchain, GPU_WAIT_NS, renderer.acquired, VK_NULL_HANDLE, &renderer.target_index);
		if (status == VK_SUBOPTIMAL_KHR)
			status = VK_SUCCESS;

		/* A timeout or lost surface does not authorize access to an image. */
		if (status != VK_SUCCESS) {
			record_error(status, "vkAcquireNextImageKHR");
			return -1;
		}
	}

	/* Reject an invalid implementation result before indexing target storage. */
	if (renderer.target_index >= renderer.target_count) {
		record_error(VK_ERROR_INITIALIZATION_FAILED, "swapchain image index");
		return -1;
	}

	/* The preceding upload or frame fence already returned command ownership. */
	status = vkResetFences(renderer.device, 1, &renderer.fence);
	if (status != VK_SUCCESS) {
		record_error(status, "vkResetFences");
		return -1;
	}

	/* Reset the completed primary recording without reallocating scene resources. */
	status = vkResetCommandPool(renderer.device, renderer.command_pool, 0);
	if (status != VK_SUCCESS) {
		record_error(status, "vkResetCommandPool");
		return -1;
	}

	/* The same draw and readback commands serve direct-display and offscreen use. */
	error = record_frame(milliseconds);
	if (error != 0)
		return -1;

	/* Wait on image acquisition and signal this image's distinct present semaphore. */
	error = submit_recording(1);
	if (error != 0)
		return -1;

	/* Make actual GPU buffer writes visible through standard memory operations. */
	error = read_pixels();
	if (error != 0)
		return -1;

	/* Identify the unmodified, format-decoded RGB image for independent comparison. */
	error = hash_pixels(digest);
	if (error != 0)
		return -1;

	/* Offscreen mode never pretends that readback alone is display presentation. */
	if (renderer.offscreen == 0) {
		/* The rendering signal belongs to this image until its next acquisition. */
		memset(&present, 0, sizeof(present));
		present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		present.waitSemaphoreCount = 1;
		present.pWaitSemaphores = &renderer.targets[renderer.target_index].rendered;
		present.swapchainCount = 1;
		present.pSwapchains = &renderer.display.swapchain;
		present.pImageIndices = &renderer.target_index;
		status = vkQueuePresentKHR(renderer.queue, &present);
		if (status == VK_SUBOPTIMAL_KHR)
			status = VK_SUCCESS;

		/* Failed presentation cannot be reported as a displayed frame. */
		if (status != VK_SUCCESS) {
			record_error(status, "vkQueuePresentKHR");
			return -1;
		}
	}

	/* Permit an explicit image export only after a complete successful frame. */
	renderer.has_frame = 1;

	/* Succeeded: the shader output is read back and, when requested, presented. */
	return 0;
}

/*
 * Export the latest real GPU readback as a portable RGB8 PPM image.
 */
int
vkdemo_write_frame(
	const char *path)
{
	FILE *output;
	uint8_t row[VKDEMO_WIDTH * 3U];
	uint32_t x;
	uint32_t y;
	uint32_t offset;
	size_t written;
	int error;
	int saved_errno;

	/* Never export stale BSS or a partial frame as rendered evidence. */
	if (renderer.has_frame == 0) {
		errno = EINVAL;
		return -1;
	}

	/* Image export is an explicit user-selected file operation. */
	output = fopen(path, "wb");
	if (output == NULL)
		return -1;

	/* Describe exactly the RGB bytes that follow without rescaling the frame. */
	error = fprintf(output, "P6\n%u %u\n255\n", VKDEMO_WIDTH, VKDEMO_HEIGHT);
	if (error < 0) {
		saved_errno = errno;
		fclose(output);
		errno = saved_errno;
		return -1;
	}

	/* Omit only alpha while retaining every GPU-produced color component. */
	for (y = 0; y < VKDEMO_HEIGHT; y++) {
		/* Preserve the same top-to-bottom RGB ordering used by the frame digest. */
		for (x = 0; x < VKDEMO_WIDTH; x++) {
			offset = (y * VKDEMO_WIDTH + x) * 4U;
			row[x * 3U] = renderer.pixels[offset];
			row[x * 3U + 1U] = renderer.pixels[offset + 1U];
			row[x * 3U + 2U] = renderer.pixels[offset + 2U];
		}

		/* A short write invalidates the artifact and preserves its real failure. */
		written = fwrite(row, 1, sizeof(row), output);
		if (written != sizeof(row)) {
			saved_errno = errno;
			fclose(output);
			errno = saved_errno;
			return -1;
		}
	}

	/* Report delayed file errors before calling the export complete. */
	error = fclose(output);
	if (error != 0)
		return -1;

	/* Succeeded: the requested file contains only the latest GPU readback. */
	return 0;
}

/*
 * Retire all application Vulkan objects after their device work has completed.
 */
int
vkdemo_close(
	void)
{
	VkResult status;

	/* An instance-only failure needs no device wait or device-object cleanup. */
	status = VK_SUCCESS;
	if (renderer.device != VK_NULL_HANDLE) {
		/* This standard API also orders present semaphore consumption before teardown. */
		status = vkDeviceWaitIdle(renderer.device);
		if (status != VK_SUCCESS)
			record_error(status, "vkDeviceWaitIdle");

		/* Release partially initialized objects in their dependency order. */
		release_resources();
	}

	/* Surface ownership outlives device creation failures. */
	vkdemo_display_close(renderer.instance, renderer.device, &renderer.display);

	/* Destroy the logical device only after every application-owned child. */
	if (renderer.device != VK_NULL_HANDLE)
		vkDestroyDevice(renderer.device, NULL);

	/* An instance is retained until both its surface and device have retired. */
	if (renderer.instance != VK_NULL_HANDLE)
		vkDestroyInstance(renderer.instance, NULL);

	/* Keep diagnostics while making repeated cleanup harmless. */
	renderer.device = VK_NULL_HANDLE;
	renderer.instance = VK_NULL_HANDLE;
	renderer.ready = 0;
	renderer.has_frame = 0;

	/* Preserve a real wait failure independently of the original rendering result. */
	if (status != VK_SUCCESS)
		return -1;

	/* Succeeded: this invocation owns no Vulkan or host allocation. */
	return 0;
}

/*
 * Identify the standard API operation that first failed during this invocation.
 */
const char *
vkdemo_error_operation(
	void)
{
	/* A diagnostic remains meaningful even before initialization was called. */
	if (renderer.last_operation == NULL)
		return "initialization";

	/* Succeeded: the operation name is a process-lifetime constant string. */
	return renderer.last_operation;
}

/*
 * Return the first Vulkan result without exposing a backend protocol number.
 */
int32_t
vkdemo_error_code(
	void)
{
	/* Succeeded: the recorded code belongs to the public VkResult enumeration. */
	return (int32_t)renderer.last_error;
}

/* Retain the first Vulkan diagnostic while adapting failure to the CLI convention. */
static void
record_error(
	VkResult status,
	const char *operation)
{
	/* Cleanup errors must not erase the rendering failure that triggered them. */
	if (renderer.last_error == VK_SUCCESS) {
		renderer.last_error = status;
		renderer.last_operation = operation;
	}

	/* Report an ordinary I/O failure for backend errors without an errno analogue. */
	errno = EIO;
	if (status == VK_ERROR_OUT_OF_HOST_MEMORY) {
		errno = ENOMEM;
	} else if (status == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
		errno = ENOMEM;
	} else if (status == VK_TIMEOUT) {
		errno = ETIMEDOUT;
	} else if (status == VK_ERROR_DEVICE_LOST) {
		errno = ENODEV;
	}

	/* Succeeded: both Vulkan and process-level error reporting retain context. */
	return;
}

/* Create one Vulkan1.0 device, selecting display capabilities only when requested. */
static int
create_context(
	uint32_t device_index)
{
	static const char *const instance_extensions[] = {
		VK_KHR_SURFACE_EXTENSION_NAME,
		VK_KHR_DISPLAY_EXTENSION_NAME
	};
	static const char *const device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	VkApplicationInfo application;
	VkInstanceCreateInfo instance;
	VkDeviceQueueCreateInfo queue;
	VkDeviceCreateInfo device;
	VkPhysicalDeviceProperties properties;
	VkPhysicalDevice *physical;
	VkResult status;
	uint32_t count;
	float priority;
	int error;

	/* Ask only for Vulkan1.0 and identify this portable application. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "vkdemo";
	application.applicationVersion = 1;
	application.pEngineName = "vkdemo";
	application.engineVersion = 1;
	application.apiVersion = VK_API_VERSION_1_0;

	/* Offscreen use has no window-system or native display dependency. */
	memset(&instance, 0, sizeof(instance));
	instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance.pApplicationInfo = &application;
	if (renderer.offscreen == 0) {
		instance.enabledExtensionCount = 2;
		instance.ppEnabledExtensionNames = instance_extensions;
	}

	/* Acquire one standard Vulkan instance before enumerating physical devices. */
	status = vkCreateInstance(&instance, NULL, &renderer.instance);
	if (status != VK_SUCCESS) {
		record_error(status, "vkCreateInstance");
		return -1;
	}

	/* Discover actual device count instead of assuming a GPU node path. */
	count = 0;
	status = vkEnumeratePhysicalDevices(renderer.instance, &count, NULL);
	if (status != VK_SUCCESS) {
		record_error(status, "vkEnumeratePhysicalDevices");
		return -1;
	}

	/* A device index selects the standard enumeration, not a backend identity. */
	if (device_index >= count) {
		record_error(VK_ERROR_INITIALIZATION_FAILED, "physical device index");
		return -1;
	}

	/* Protect the temporary enumeration allocation on every pointer width. */
	if (sizeof(*physical) > SIZE_MAX / count) {
		record_error(VK_ERROR_OUT_OF_HOST_MEMORY, "physical device array");
		return -1;
	}

	/* Retain the device list only while selecting the requested index. */
	physical = calloc(count, sizeof(*physical));
	if (physical == NULL) {
		record_error(VK_ERROR_OUT_OF_HOST_MEMORY, "physical device array");
		return -1;
	}

	/* Reject changed or failed enumeration without reading uninitialized entries. */
	status = vkEnumeratePhysicalDevices(renderer.instance, &count, physical);
	if (status != VK_SUCCESS) {
		free(physical);
		record_error(status, "vkEnumeratePhysicalDevices");
		return -1;
	}

	/* A disappearing device is a real initialization failure. */
	if (device_index >= count) {
		free(physical);
		record_error(VK_ERROR_INITIALIZATION_FAILED, "physical device disappeared");
		return -1;
	}

	/* Vulkan retains the selected handle after enumeration storage is released. */
	renderer.physical = physical[device_index];
	free(physical);
	vkGetPhysicalDeviceMemoryProperties(renderer.physical, &renderer.memory);

	/* Select a real display surface before choosing its presentation queue. */
	if (renderer.offscreen == 0) {
		status = vkdemo_display_open(renderer.instance, renderer.physical, VKDEMO_WIDTH, VKDEMO_HEIGHT, &renderer.display);
		if (status != VK_SUCCESS) {
			record_error(status, "direct display surface");
			return -1;
		}
	}

	/* Keep graphics and presentation in one family to avoid hidden ownership rules. */
	error = choose_queue();
	if (error != 0)
		return -1;

	/* Request one ordinary priority-one graphics queue. */
	priority = 1.0f;
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = renderer.family;
	queue.queueCount = 1;
	queue.pQueuePriorities = &priority;

	/* No optional core feature is needed by this scene. */
	memset(&device, 0, sizeof(device));
	device.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device.queueCreateInfoCount = 1;
	device.pQueueCreateInfos = &queue;
	if (renderer.offscreen == 0) {
		device.enabledExtensionCount = 1;
		device.ppEnabledExtensionNames = device_extensions;
	}

	/* Acquire the logical device through the public Vulkan entry point. */
	status = vkCreateDevice(renderer.physical, &device, NULL, &renderer.device);
	if (status != VK_SUCCESS) {
		record_error(status, "vkCreateDevice");
		return -1;
	}

	/* Resolve the queue created above without assuming its native representation. */
	vkGetDeviceQueue(renderer.device, renderer.family, 0, &renderer.queue);
	renderer.color_format = VK_FORMAT_R8G8B8A8_UNORM;

	/* Direct-display color targets belong to a real standard swapchain. */
	if (renderer.offscreen == 0) {
		status = vkdemo_display_create_swapchain(renderer.physical, renderer.device, renderer.family, &renderer.display);
		if (status != VK_SUCCESS) {
			record_error(status, "direct display swapchain");
			return -1;
		}

		/* Use the exact format accepted by the surface implementation. */
		renderer.color_format = renderer.display.format;
	}

	/* Identify the public device, API request and chosen graphics family. */
	vkGetPhysicalDeviceProperties(renderer.physical, &properties);
	printf("VKDEMO VULKAN device=%.64s api=1.0 family=%u offscreen=%d\n", properties.deviceName, renderer.family, renderer.offscreen);
	fflush(stdout);

	/* Succeeded: the application owns a standard graphics device and optional WSI. */
	return 0;
}

/* Select a family that can both render and present the chosen surface. */
static int
choose_queue(
	void)
{
	VkQueueFamilyProperties *families;
	VkBool32 supported;
	VkResult status;
	uint32_t count;
	uint32_t index;
	int found;

	/* Ask Vulkan how many queue-family properties are available. */
	count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(renderer.physical, &count, NULL);
	if (count == 0) {
		record_error(VK_ERROR_INITIALIZATION_FAILED, "queue families");
		return -1;
	}

	/* Keep enumeration storage representable on a 32-bit caller. */
	if (sizeof(*families) > SIZE_MAX / count) {
		record_error(VK_ERROR_OUT_OF_HOST_MEMORY, "queue family array");
		return -1;
	}

	/* Retain family capabilities during graphics/presentation selection. */
	families = calloc(count, sizeof(*families));
	if (families == NULL) {
		record_error(VK_ERROR_OUT_OF_HOST_MEMORY, "queue family array");
		return -1;
	}

	/* An ordinary Vulkan enumeration populates the allocated capacity. */
	vkGetPhysicalDeviceQueueFamilyProperties(renderer.physical, &count, families);
	found = 0;
	for (index = 0; index < count; index++) {
		/* A graphics flag alone cannot supply a zero-count queue family. */
		if (families[index].queueCount == 0)
			continue;

		/* This application executes both vertex and fragment stages. */
		if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
			continue;

		/* Surface ownership requires presentation support on the same family. */
		if (renderer.offscreen == 0) {
			status = vkGetPhysicalDeviceSurfaceSupportKHR(renderer.physical, index, renderer.display.surface, &supported);
			if (status != VK_SUCCESS) {
				free(families);
				record_error(status, "vkGetPhysicalDeviceSurfaceSupportKHR");
				return -1;
			}

			/* Keep looking when this family cannot drive the selected surface. */
			if (supported == VK_FALSE)
				continue;
		}

		/* Publish the actual array index that identifies this family. */
		renderer.family = index;
		found = 1;
		break;
	}

	/* The selected queue-family index does not borrow host enumeration memory. */
	free(families);
	if (found == 0) {
		record_error(VK_ERROR_INITIALIZATION_FAILED, "compatible graphics queue");
		return -1;
	}

	/* Succeeded: one family covers every operation this renderer submits. */
	return 0;
}

/* Allocate an allowed memory type, preferring coherence for host-visible buffers. */
static int
allocate_memory(
	const VkMemoryRequirements *requirements,
	VkMemoryPropertyFlags flags,
	VkDeviceMemory *memory,
	VkMemoryPropertyFlags *properties)
{
	VkMemoryAllocateInfo allocation;
	VkMemoryPropertyFlags available;
	VkResult status;
	uint32_t chosen;
	uint32_t index;

	/* VkPhysicalDeviceMemoryProperties has a fixed standard capacity of 32 types. */
	if (renderer.memory.memoryTypeCount > VK_MAX_MEMORY_TYPES) {
		record_error(VK_ERROR_INITIALIZATION_FAILED, "memory type count");
		return -1;
	}

	/* Find a permitted type that provides the requested visibility attributes. */
	chosen = UINT32_MAX;
	for (index = 0; index < renderer.memory.memoryTypeCount; index++) {
		/* Binding requirements may exclude otherwise suitable device memory. */
		if ((requirements->memoryTypeBits & (1U << index)) == 0)
			continue;

		/* Require every requested property, without borrowing host-only knowledge. */
		available = renderer.memory.memoryTypes[index].propertyFlags;
		if ((available & flags) != flags)
			continue;

		/* Retain the first compatible fallback even if it is non-coherent. */
		if (chosen == UINT32_MAX)
			chosen = index;

		/* Host-visible callers can avoid explicit flush/invalidate when coherent. */
		if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {
			/* Prefer a coherent type while preserving the non-coherent fallback. */
			if ((available & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0) {
				chosen = index;
				break;
			}
		} else {
			/* Image-only memory has no host mapping preference. */
			break;
		}
	}

	/* Unsupported memory attributes are not repaired by changing Vulkan semantics. */
	if (chosen == UINT32_MAX) {
		record_error(VK_ERROR_FEATURE_NOT_PRESENT, "compatible memory type");
		return -1;
	}

	/* Allocate the exact byte size and selected type required for this resource. */
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocation.allocationSize = requirements->size;
	allocation.memoryTypeIndex = chosen;
	status = vkAllocateMemory(renderer.device, &allocation, NULL, memory);
	if (status != VK_SUCCESS) {
		record_error(status, "vkAllocateMemory");
		return -1;
	}

	/* Mapping callers use the actual selected properties for visibility decisions. */
	if (properties != NULL)
		*properties = renderer.memory.memoryTypes[chosen].propertyFlags;

	/* Succeeded: the caller owns a valid allocation of a compatible memory type. */
	return 0;
}

/* Create one ordinary optimal-tiled image with its own bound allocation. */
static int
create_image(
	struct demo_image *image,
	VkFormat format,
	uint32_t width,
	uint32_t height,
	VkImageUsageFlags usage)
{
	VkImageCreateInfo create;
	VkMemoryRequirements requirements;
	VkResult status;
	int error;

	/* Describe a single-sample, single-layer two-dimensional image. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.imageType = VK_IMAGE_TYPE_2D;
	create.format = format;
	create.extent.width = width;
	create.extent.height = height;
	create.extent.depth = 1;
	create.mipLevels = 1;
	create.arrayLayers = 1;
	create.samples = VK_SAMPLE_COUNT_1_BIT;
	create.tiling = VK_IMAGE_TILING_OPTIMAL;
	create.usage = usage;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	status = vkCreateImage(renderer.device, &create, NULL, &image->image);
	if (status != VK_SUCCESS) {
		record_error(status, "vkCreateImage");
		return -1;
	}

	/* Obtain allocation geometry from the actual image implementation. */
	vkGetImageMemoryRequirements(renderer.device, image->image, &requirements);
	error = allocate_memory(&requirements, 0, &image->memory, NULL);
	if (error != 0)
		return -1;

	/* Bind the image only after its compatible allocation exists. */
	status = vkBindImageMemory(renderer.device, image->image, image->memory, 0);
	if (status != VK_SUCCESS) {
		record_error(status, "vkBindImageMemory");
		return -1;
	}

	/* Succeeded: the image is backed but remains in its declared initial layout. */
	return 0;
}

/* Expose exactly one image aspect, mip and layer to shader or attachment use. */
static int
create_view(
	VkImage image,
	VkFormat format,
	VkImageAspectFlags aspect,
	VkImageView *view)
{
	VkImageViewCreateInfo create;
	VkResult status;

	/* Identity component mapping preserves the image format's channel semantics. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	create.image = image;
	create.viewType = VK_IMAGE_VIEW_TYPE_2D;
	create.format = format;
	create.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
	create.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	create.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
	create.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
	create.subresourceRange.aspectMask = aspect;
	create.subresourceRange.levelCount = 1;
	create.subresourceRange.layerCount = 1;
	status = vkCreateImageView(renderer.device, &create, NULL, view);
	if (status != VK_SUCCESS) {
		record_error(status, "vkCreateImageView");
		return -1;
	}

	/* Succeeded: the caller owns a view of precisely the requested image aspect. */
	return 0;
}

/* Create one mapped host-visible buffer while retaining its actual coherence flag. */
static int
create_buffer(
	struct demo_buffer *buffer,
	VkDeviceSize bytes,
	VkBufferUsageFlags usage)
{
	VkBufferCreateInfo create;
	VkMemoryRequirements requirements;
	VkResult status;
	int error;

	/* Describe only the usage needed by this one retained scene buffer. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	create.size = bytes;
	create.usage = usage;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	status = vkCreateBuffer(renderer.device, &create, NULL, &buffer->buffer);
	if (status != VK_SUCCESS) {
		record_error(status, "vkCreateBuffer");
		return -1;
	}

	/* Select host-visible memory that satisfies the buffer's binding requirements. */
	vkGetBufferMemoryRequirements(renderer.device, buffer->buffer, &requirements);
	error = allocate_memory(&requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &buffer->memory, &buffer->properties);
	if (error != 0)
		return -1;

	/* Binding precedes both mapping and all GPU references. */
	status = vkBindBufferMemory(renderer.device, buffer->buffer, buffer->memory, 0);
	if (status != VK_SUCCESS) {
		record_error(status, "vkBindBufferMemory");
		return -1;
	}

	/* Retain the complete mapping so whole-range cache operations remain valid. */
	status = vkMapMemory(renderer.device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->mapping);
	if (status != VK_SUCCESS) {
		record_error(status, "vkMapMemory");
		return -1;
	}

	/* Succeeded: the buffer is backed and mapped through the standard Vulkan API. */
	return 0;
}

/* Allocate only the images and mapped buffers used by the original scene. */
static int
create_storage(
	void)
{
	int error;

	/* Direct-display images are borrowed from the swapchain, not allocated twice. */
	if (renderer.offscreen != 0) {
		error = create_image(&renderer.color, renderer.color_format, VKDEMO_WIDTH, VKDEMO_HEIGHT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		if (error != 0)
			return -1;
	}

	/* Keep one shared depth image because each frame waits for the previous one. */
	error = create_image(&renderer.depth, VK_FORMAT_D32_SFLOAT, VKDEMO_WIDTH, VKDEMO_HEIGHT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
	if (error != 0)
		return -1;

	/* Framebuffers see only the depth aspect of the retained depth image. */
	error = create_view(renderer.depth.image, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, &renderer.depth.view);
	if (error != 0)
		return -1;

	/* The CPU supplies texture inputs; the fragment shader performs all sampling. */
	error = create_image(&renderer.texture, VK_FORMAT_R8G8B8A8_UNORM, TEXTURE_WIDTH, TEXTURE_WIDTH, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	if (error != 0)
		return -1;

	/* Expose the complete checker image to its combined image-sampler descriptor. */
	error = create_view(renderer.texture.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, &renderer.texture.view);
	if (error != 0)
		return -1;

	/* Interleaved vertices and the checker upload share one mapped staging buffer. */
	error = create_buffer(&renderer.upload_buffer, UPLOAD_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	if (error != 0)
		return -1;

	/* Only Vulkan image-copy commands populate the retained readback buffer. */
	error = create_buffer(&renderer.readback, VKDEMO_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	if (error != 0)
		return -1;

	/* Report application storage sizes without leaking backend resource identities. */
	printf("VKDEMO STORAGE upload=%u readback=%u access=vkMapMemory\n", UPLOAD_BYTES, VKDEMO_BYTES);
	fflush(stdout);

	/* Succeeded: all scene storage has ordinary Vulkan ownership and visibility. */
	return 0;
}

/* Publish CPU vertex and texture writes according to the selected memory type. */
static int
publish_upload(
	void)
{
	VkMappedMemoryRange range;
	VkResult status;

	/* Copy only original scene inputs into the retained standard mapping. */
	memcpy(renderer.upload_buffer.mapping, renderer.upload, sizeof(renderer.upload));

	/* Non-coherent memory requires an explicit Vulkan cache operation. */
	if ((renderer.upload_buffer.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
		/* Whole allocation size avoids guessing the non-coherent atom alignment. */
		memset(&range, 0, sizeof(range));
		range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		range.memory = renderer.upload_buffer.memory;
		range.size = VK_WHOLE_SIZE;
		status = vkFlushMappedMemoryRanges(renderer.device, 1, &range);
		if (status != VK_SUCCESS) {
			record_error(status, "vkFlushMappedMemoryRanges");
			return -1;
		}
	}

	/* Succeeded: subsequent queue submission can observe the completed host writes. */
	return 0;
}

/* Bind the checker texture and one vertex-stage time constant. */
static int
create_descriptors(
	void)
{
	VkSamplerCreateInfo sampler;
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo layout;
	VkDescriptorPoolSize size;
	VkDescriptorPoolCreateInfo pool;
	VkDescriptorSetAllocateInfo allocation;
	VkDescriptorImageInfo image;
	VkWriteDescriptorSet write;
	VkPushConstantRange push;
	VkPipelineLayoutCreateInfo pipeline;
	VkResult status;

	/* Preserve nearest checker sampling and clamp-to-edge addressing. */
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_NEAREST;
	sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.maxAnisotropy = 1.0f;
	sampler.compareOp = VK_COMPARE_OP_ALWAYS;
	sampler.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
	status = vkCreateSampler(renderer.device, &sampler, NULL, &renderer.sampler);
	if (status != VK_SUCCESS) {
		record_error(status, "vkCreateSampler");
		return -1;
	}

	/* Binding zero supplies one combined texture and sampler to the fragment stage. */
	memset(&binding, 0, sizeof(binding));
	binding.binding = 0;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	/* Describe the complete descriptor layout without extension state. */
	memset(&layout, 0, sizeof(layout));
	layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout.bindingCount = 1;
	layout.pBindings = &binding;
	status = vkCreateDescriptorSetLayout(renderer.device, &layout, NULL, &renderer.set_layout);
	if (status != VK_SUCCESS) {
		record_error(status, "vkCreateDescriptorSetLayout");
		return -1;
	}

	/* Reserve precisely one descriptor for the scene's one texture. */
	memset(&size, 0, sizeof(size));
	size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	size.descriptorCount = 1;

	/* The pool owns the descriptor set until the entire scene is retired. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.maxSets = 1;
	pool.poolSizeCount = 1;
	pool.pPoolSizes = &size;
	status = vkCreateDescriptorPool(renderer.device, &pool, NULL, &renderer.descriptor_pool);
	if (status != VK_SUCCESS) {
		record_error(status, "vkCreateDescriptorPool");
		return -1;
	}

	/* Allocate the ordinary descriptor set matching binding zero. */
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocation.descriptorPool = renderer.descriptor_pool;
	allocation.descriptorSetCount = 1;
	allocation.pSetLayouts = &renderer.set_layout;
	status = vkAllocateDescriptorSets(renderer.device, &allocation, &renderer.descriptor_set);
	if (status != VK_SUCCESS) {
		record_error(status, "vkAllocateDescriptorSets");
		return -1;
	}

	/* Describe the layout that the initial texture upload will establish. */
	memset(&image, 0, sizeof(image));
	image.sampler = renderer.sampler;
	image.imageView = renderer.texture.view;
	image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	/* Publish one complete combined-image-sampler descriptor. */
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = renderer.descriptor_set;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image;
	vkUpdateDescriptorSets(renderer.device, 1, &write, 0, NULL);

	/* One float carries time; the vertex shader performs rotation and projection. */
	memset(&push, 0, sizeof(push));
	push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	push.size = sizeof(float);

	/* Link descriptor and push-constant layouts into the pipeline contract. */
	memset(&pipeline, 0, sizeof(pipeline));
	pipeline.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline.setLayoutCount = 1;
	pipeline.pSetLayouts = &renderer.set_layout;
	pipeline.pushConstantRangeCount = 1;
	pipeline.pPushConstantRanges = &push;
	status = vkCreatePipelineLayout(renderer.device, &pipeline, NULL, &renderer.pipeline_layout);
	if (status != VK_SUCCESS) {
		record_error(status, "vkCreatePipelineLayout");
		return -1;
	}

	/* Succeeded: shader inputs are represented entirely by standard Vulkan objects. */
	return 0;
}

/* Define the original clear/depth pass and its color-write-to-readback dependency. */
static int
create_render_pass(
	void)
{
	VkAttachmentDescription attachments[2];
	VkAttachmentReference color;
	VkAttachmentReference depth;
	VkSubpassDescription subpass;
	VkSubpassDependency dependencies[2];
	VkRenderPassCreateInfo create;
	VkResult status;

	/* Discard the previous color contents and retain this frame for readback. */
	memset(attachments, 0, sizeof(attachments));
	attachments[0].format = renderer.color_format;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

	/* Resolve visibility with fresh depth and discard it after the pass. */
	attachments[1].format = VK_FORMAT_D32_SFLOAT;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	/* Subpass attachment zero receives the fragment shader color. */
	memset(&color, 0, sizeof(color));
	color.attachment = 0;
	color.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	/* Subpass attachment one supplies ordinary LESS depth testing. */
	memset(&depth, 0, sizeof(depth));
	depth.attachment = 1;
	depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	/* A single graphics subpass consumes both explicitly ordered attachments. */
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &color;
	subpass.pDepthStencilAttachment = &depth;

	/* Make initial layout transitions precede color and depth attachment writes. */
	memset(dependencies, 0, sizeof(dependencies));
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	/* Make stored color writes visible to the following transfer readback. */
	dependencies[1].srcSubpass = 0;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

	/* Create the complete pass before any framebuffer or pipeline references it. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	create.attachmentCount = 2;
	create.pAttachments = attachments;
	create.subpassCount = 1;
	create.pSubpasses = &subpass;
	create.dependencyCount = 2;
	create.pDependencies = dependencies;
	status = vkCreateRenderPass(renderer.device, &create, NULL, &renderer.render_pass);
	if (status != VK_SUCCESS) {
		record_error(status, "vkCreateRenderPass");
		return -1;
	}

	/* Succeeded: the pass preserves the original scene's rendering and readback order. */
	return 0;
}

/* Create one framebuffer and one present signal for each real color image. */
static int
create_targets(
	void)
{
	VkFramebufferCreateInfo create;
	VkSemaphoreCreateInfo semaphore;
	VkImageView attachments[2];
	VkResult status;
	uint32_t index;
	int error;

	/* Offscreen verification owns one image; display mode uses actual swapchain images. */
	renderer.target_count = 1;
	if (renderer.offscreen == 0)
		renderer.target_count = renderer.display.image_count;

	/* A color target array must have at least one initialized slot. */
	if (renderer.target_count == 0) {
		record_error(VK_ERROR_INITIALIZATION_FAILED, "color targets");
		return -1;
	}

	/* Bound target allocation before constructing Vulkan references into it. */
	if (sizeof(*renderer.targets) > SIZE_MAX / renderer.target_count) {
		record_error(VK_ERROR_OUT_OF_HOST_MEMORY, "color target array");
		return -1;
	}

	/* Zero handles keep partial framebuffer initialization safely destructible. */
	renderer.targets = calloc(renderer.target_count, sizeof(*renderer.targets));
	if (renderer.targets == NULL) {
		record_error(VK_ERROR_OUT_OF_HOST_MEMORY, "color target array");
		return -1;
	}

	/* Build each image's dependent view, framebuffer and present semaphore together. */
	for (index = 0; index < renderer.target_count; index++) {
		/* Borrow swapchain images rather than allocating replacement presentation storage. */
		renderer.targets[index].image = renderer.color.image;
		if (renderer.offscreen == 0)
			renderer.targets[index].image = renderer.display.images[index];

		/* The view exactly preserves the color target's declared format. */
		error = create_view(renderer.targets[index].image, renderer.color_format, VK_IMAGE_ASPECT_COLOR_BIT, &renderer.targets[index].view);
		if (error != 0)
			return -1;

		/* Keep framebuffer attachment order consistent with the render pass. */
		attachments[0] = renderer.targets[index].view;
		attachments[1] = renderer.depth.view;
		memset(&create, 0, sizeof(create));
		create.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		create.renderPass = renderer.render_pass;
		create.attachmentCount = 2;
		create.pAttachments = attachments;
		create.width = VKDEMO_WIDTH;
		create.height = VKDEMO_HEIGHT;
		create.layers = 1;
		status = vkCreateFramebuffer(renderer.device, &create, NULL, &renderer.targets[index].framebuffer);
		if (status != VK_SUCCESS) {
			record_error(status, "vkCreateFramebuffer");
			return -1;
		}

		/* A present signal can be reused safely when this same image is reacquired. */
		if (renderer.offscreen == 0) {
			memset(&semaphore, 0, sizeof(semaphore));
			semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
			status = vkCreateSemaphore(renderer.device, &semaphore, NULL, &renderer.targets[index].rendered);
			if (status != VK_SUCCESS) {
				record_error(status, "vkCreateSemaphore");
				return -1;
			}
		}
	}

	/* Succeeded: each color target has distinct lifetime-safe presentation state. */
	return 0;
}

/* Create an ordinary shader module from the preserved original SPIR-V words. */
static int
create_shader(
	const uint32_t *words,
	size_t bytes,
	VkShaderModule *shader)
{
	VkShaderModuleCreateInfo create;
	VkResult status;

	/* Vulkan accepts the original code as an explicit byte-sized word array. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	create.codeSize = bytes;
	create.pCode = words;
	status = vkCreateShaderModule(renderer.device, &create, NULL, shader);
	if (status != VK_SUCCESS) {
		record_error(status, "vkCreateShaderModule");
		return -1;
	}

	/* Succeeded: this module is ready for standard graphics pipeline creation. */
	return 0;
}

/* Link the preserved shaders with explicit vertex, raster, depth and color state. */
static int
create_pipeline(
	void)
{
	VkPipelineShaderStageCreateInfo stages[2];
	VkVertexInputBindingDescription binding;
	VkVertexInputAttributeDescription attributes[2];
	VkPipelineVertexInputStateCreateInfo vertices;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkViewport viewport;
	VkRect2D scissor;
	VkPipelineViewportStateCreateInfo view;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo samples;
	VkPipelineDepthStencilStateCreateInfo depth;
	VkPipelineColorBlendAttachmentState attachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkGraphicsPipelineCreateInfo create;
	VkResult status;
	int error;

	/* Preserve the exact vertex shader used by the independently checked scene. */
	error = create_shader(vkdemo_vertex_shader, sizeof(vkdemo_vertex_shader), &renderer.vertex_shader);
	if (error != 0)
		return -1;

	/* Preserve the original fragment shader's texture sampling behavior. */
	error = create_shader(vkdemo_fragment_shader, sizeof(vkdemo_fragment_shader), &renderer.fragment_shader);
	if (error != 0)
		return -1;

	/* Vertex inputs enter the ordinary main entry without specialization. */
	memset(stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = renderer.vertex_shader;
	stages[0].pName = "main";

	/* The fragment stage samples the actual bound checker texture. */
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = renderer.fragment_shader;
	stages[1].pName = "main";

	/* One interleaved twenty-byte record supplies each vertex. */
	memset(&binding, 0, sizeof(binding));
	binding.binding = 0;
	binding.stride = VERTEX_STRIDE;
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	/* Position occupies three float32 components at byte zero. */
	memset(attributes, 0, sizeof(attributes));
	attributes[0].location = 0;
	attributes[0].binding = 0;
	attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
	attributes[0].offset = 0;

	/* UV occupies the following two float32 components. */
	attributes[1].location = 1;
	attributes[1].binding = 0;
	attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
	attributes[1].offset = 12;

	/* Match vertex fetch to the original CPU-supplied geometry records. */
	memset(&vertices, 0, sizeof(vertices));
	vertices.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vertices.vertexBindingDescriptionCount = 1;
	vertices.pVertexBindingDescriptions = &binding;
	vertices.vertexAttributeDescriptionCount = 2;
	vertices.pVertexAttributeDescriptions = attributes;

	/* Thirty-six explicit vertices form twelve independent triangles. */
	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	/* Positive viewport height preserves the shader's explicit clip-Y inversion. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)VKDEMO_WIDTH;
	viewport.height = (float)VKDEMO_HEIGHT;
	viewport.maxDepth = 1.0f;

	/* Draw only into the complete fixed-size target used by the image oracle. */
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent.width = VKDEMO_WIDTH;
	scissor.extent.height = VKDEMO_HEIGHT;

	/* Bake one viewport and one scissor into the pipeline. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	view.viewportCount = 1;
	view.pViewports = &viewport;
	view.scissorCount = 1;
	view.pScissors = &scissor;

	/* Disable culling so depth alone chooses the visible cuboid faces. */
	memset(&raster, 0, sizeof(raster));
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;

	/* Use ordinary single-sample rasterization without optional sample features. */
	memset(&samples, 0, sizeof(samples));
	samples.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	/* LESS depth testing and writes make visibility independent of triangle order. */
	memset(&depth, 0, sizeof(depth));
	depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depth.depthTestEnable = VK_TRUE;
	depth.depthWriteEnable = VK_TRUE;
	depth.depthCompareOp = VK_COMPARE_OP_LESS;
	depth.front.compareOp = VK_COMPARE_OP_ALWAYS;
	depth.back.compareOp = VK_COMPARE_OP_ALWAYS;
	depth.maxDepthBounds = 1.0f;

	/* Store all unblended color components emitted by the fragment shader. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
	attachment.colorBlendOp = VK_BLEND_OP_ADD;
	attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	attachment.alphaBlendOp = VK_BLEND_OP_ADD;
	attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

	/* No logic operation or blending changes the checked shader output. */
	memset(&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.logicOp = VK_LOGIC_OP_COPY;
	blend.attachmentCount = 1;
	blend.pAttachments = &attachment;

	/* Bind every state group to the one render pass and descriptor layout. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	create.stageCount = 2;
	create.pStages = stages;
	create.pVertexInputState = &vertices;
	create.pInputAssemblyState = &assembly;
	create.pViewportState = &view;
	create.pRasterizationState = &raster;
	create.pMultisampleState = &samples;
	create.pDepthStencilState = &depth;
	create.pColorBlendState = &blend;
	create.layout = renderer.pipeline_layout;
	create.renderPass = renderer.render_pass;
	create.basePipelineIndex = -1;
	status = vkCreateGraphicsPipelines(renderer.device, VK_NULL_HANDLE, 1, &create, NULL, &renderer.pipeline);
	if (status != VK_SUCCESS) {
		record_error(status, "vkCreateGraphicsPipelines");
		return -1;
	}

	/* Succeeded: the standard pipeline preserves the original shader and raster state. */
	return 0;
}

/* Retain a primary recording, completion fence and image-acquisition signal. */
static int
create_commands(
	void)
{
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo allocation;
	VkFenceCreateInfo fence;
	VkSemaphoreCreateInfo semaphore;
	VkResult status;

	/* The command pool belongs to the same graphics family as every submission. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.queueFamilyIndex = renderer.family;
	status = vkCreateCommandPool(renderer.device, &pool, NULL, &renderer.command_pool);
	if (status != VK_SUCCESS) {
		record_error(status, "vkCreateCommandPool");
		return -1;
	}

	/* Reuse one primary command buffer after its preceding fence signals. */
	memset(&allocation, 0, sizeof(allocation));
	allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocation.commandPool = renderer.command_pool;
	allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocation.commandBufferCount = 1;
	status = vkAllocateCommandBuffers(renderer.device, &allocation, &renderer.command);
	if (status != VK_SUCCESS) {
		record_error(status, "vkAllocateCommandBuffers");
		return -1;
	}

	/* The initial texture upload will signal this initially unsignaled fence. */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	status = vkCreateFence(renderer.device, &fence, NULL, &renderer.fence);
	if (status != VK_SUCCESS) {
		record_error(status, "vkCreateFence");
		return -1;
	}

	/* A single acquisition semaphore is consumed before each frame fence signals. */
	if (renderer.offscreen == 0) {
		memset(&semaphore, 0, sizeof(semaphore));
		semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		status = vkCreateSemaphore(renderer.device, &semaphore, NULL, &renderer.acquired);
		if (status != VK_SUCCESS) {
			record_error(status, "vkCreateSemaphore");
			return -1;
		}
	}

	/* Succeeded: command and synchronization objects survive every scene frame. */
	return 0;
}

/* Begin one recording after initialization or an explicitly completed pool reset. */
static int
begin_recording(
	void)
{
	VkCommandBufferBeginInfo begin;
	VkResult status;

	/* Each reset creates one new recording that is submitted exactly once. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	status = vkBeginCommandBuffer(renderer.command, &begin);
	if (status != VK_SUCCESS) {
		record_error(status, "vkBeginCommandBuffer");
		return -1;
	}

	/* Succeeded: the retained primary buffer accepts ordinary Vulkan commands. */
	return 0;
}

/* Submit one recording and observe actual Vulkan fence completion with a deadline. */
static int
submit_recording(
	int present)
{
	VkSubmitInfo submit;
	VkPipelineStageFlags stage;
	VkResult status;

	/* Finish command recording before making it visible to the graphics queue. */
	status = vkEndCommandBuffer(renderer.command);
	if (status != VK_SUCCESS) {
		record_error(status, "vkEndCommandBuffer");
		return -1;
	}

	/* Submission always contains the one complete application recording. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &renderer.command;

	/* Texture upload and offscreen rendering do not acquire display images. */
	if (present != 0) {
		/* Only direct display requires image availability and presentation signals. */
		if (renderer.offscreen == 0) {
			stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			submit.waitSemaphoreCount = 1;
			submit.pWaitSemaphores = &renderer.acquired;
			submit.pWaitDstStageMask = &stage;
			submit.signalSemaphoreCount = 1;
			submit.pSignalSemaphores = &renderer.targets[renderer.target_index].rendered;
		}
	}

	/* A successful submit transfers temporary use of these resources to the GPU. */
	status = vkQueueSubmit(renderer.queue, 1, &submit, renderer.fence);
	if (status != VK_SUCCESS) {
		record_error(status, "vkQueueSubmit");
		return -1;
	}

	/* A command-transfer acknowledgment cannot replace this Vulkan completion test. */
	status = vkWaitForFences(renderer.device, 1, &renderer.fence, VK_TRUE, GPU_WAIT_NS);
	if (status != VK_SUCCESS) {
		record_error(status, "vkWaitForFences");
		return -1;
	}

	/* Succeeded: application memory and the command pool are no longer in GPU use. */
	return 0;
}

/* Record an explicit transition for one complete color image. */
static void
image_barrier(
	VkImage image,
	VkImageLayout old_layout,
	VkImageLayout new_layout,
	VkAccessFlags source_access,
	VkAccessFlags destination_access,
	VkPipelineStageFlags source_stage,
	VkPipelineStageFlags destination_stage)
{
	VkImageMemoryBarrier barrier;

	/* Keep queue ownership unchanged while ordering this image's accesses. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = source_access;
	barrier.dstAccessMask = destination_access;
	barrier.oldLayout = old_layout;
	barrier.newLayout = new_layout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;
	vkCmdPipelineBarrier(renderer.command, source_stage, destination_stage, 0, 0, NULL, 0, NULL, 1, &barrier);

	/* Succeeded: the recording includes the requested image-layout dependency. */
	return;
}

/* Record a visibility dependency between CPU, transfer and vertex buffer uses. */
static void
buffer_barrier(
	VkBuffer buffer,
	VkDeviceSize bytes,
	VkAccessFlags source_access,
	VkAccessFlags destination_access,
	VkPipelineStageFlags source_stage,
	VkPipelineStageFlags destination_stage)
{
	VkBufferMemoryBarrier barrier;

	/* Limit the dependency to the actual buffer byte range. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.srcAccessMask = source_access;
	barrier.dstAccessMask = destination_access;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer = buffer;
	barrier.size = bytes;
	vkCmdPipelineBarrier(renderer.command, source_stage, destination_stage, 0, 0, NULL, 1, &barrier, 0, NULL);

	/* Succeeded: the recording orders the named buffer's source and destination uses. */
	return;
}

/* Upload the original checker once before the fragment shader first samples it. */
static int
upload_texture(
	void)
{
	VkBufferImageCopy copy;
	int error;

	/* The initial command buffer is already in its unused initial state. */
	error = begin_recording();
	if (error != 0)
		return -1;

	/* Publish the mapped vertex and texture inputs to their GPU readers. */
	buffer_barrier(renderer.upload_buffer.buffer, UPLOAD_BYTES, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);

	/* The new checker image must enter transfer-destination layout before copying. */
	image_barrier(renderer.texture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

	/* Copy the complete checker from its aligned position in the upload buffer. */
	memset(&copy, 0, sizeof(copy));
	copy.bufferOffset = TEXTURE_OFFSET;
	copy.bufferRowLength = TEXTURE_WIDTH;
	copy.bufferImageHeight = TEXTURE_WIDTH;
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent.width = TEXTURE_WIDTH;
	copy.imageExtent.height = TEXTURE_WIDTH;
	copy.imageExtent.depth = 1;
	vkCmdCopyBufferToImage(renderer.command, renderer.upload_buffer.buffer, renderer.texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

	/* Make transfer writes available to ordinary fragment-stage sampling. */
	image_barrier(renderer.texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

	/* Wait for the upload without involving swapchain acquisition or presentation. */
	error = submit_recording(0);
	if (error != 0)
		return -1;

	/* Succeeded: every scene frame samples the retained GPU texture image. */
	return 0;
}

/* Record the same shader execution and readback for both supported target modes. */
static int
record_frame(
	uint32_t milliseconds)
{
	VkClearValue clears[2];
	VkRenderPassBeginInfo begin;
	VkBufferImageCopy copy;
	VkDeviceSize vertex_offset;
	VkImage color;
	float seconds;
	int error;

	/* The caller reset this recording only after the previous completion fence. */
	error = begin_recording();
	if (error != 0)
		return -1;

	/* Keep mapped vertex input visibility explicit before each draw. */
	buffer_barrier(renderer.upload_buffer.buffer, UPLOAD_BYTES, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);

	/* The preceding CPU read finishes before the next transfer overwrites readback. */
	buffer_barrier(renderer.readback.buffer, VKDEMO_BYTES, VK_ACCESS_HOST_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

	/* Preserve the independently specified opaque dark-blue background. */
	memset(clears, 0, sizeof(clears));
	clears[0].color.float32[0] = 16.0f / 255.0f;
	clears[0].color.float32[1] = 24.0f / 255.0f;
	clears[0].color.float32[2] = 40.0f / 255.0f;
	clears[0].color.float32[3] = 1.0f;

	/* LESS depth testing starts with the farthest representable normalized depth. */
	clears[1].depthStencil.depth = 1.0f;

	/* Target the acquired swapchain framebuffer or the owned offscreen framebuffer. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	begin.renderPass = renderer.render_pass;
	begin.framebuffer = renderer.targets[renderer.target_index].framebuffer;
	begin.renderArea.extent.width = VKDEMO_WIDTH;
	begin.renderArea.extent.height = VKDEMO_HEIGHT;
	begin.clearValueCount = 2;
	begin.pClearValues = clears;
	vkCmdBeginRenderPass(renderer.command, &begin, VK_SUBPASS_CONTENTS_INLINE);

	/* Bind the original shaders and position/UV vertex input. */
	vkCmdBindPipeline(renderer.command, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.pipeline);
	vertex_offset = 0;
	vkCmdBindVertexBuffers(renderer.command, 0, 1, &renderer.upload_buffer.buffer, &vertex_offset);

	/* Bind the actual sampled texture to the fragment shader. */
	vkCmdBindDescriptorSets(renderer.command, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.pipeline_layout, 0, 1, &renderer.descriptor_set, 0, NULL);

	/* Only time crosses from CPU animation into the shader's rotation calculation. */
	seconds = (float)milliseconds / 1000.0f;
	vkCmdPushConstants(renderer.command, renderer.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(seconds), &seconds);

	/* Execute twelve textured, depth-tested triangles through both shader stages. */
	vkCmdDraw(renderer.command, VERTEX_COUNT, 1, 0, 0);
	vkCmdEndRenderPass(renderer.command);

	/* Copy the actual rendered color image after its render-pass dependency. */
	color = renderer.targets[renderer.target_index].image;
	memset(&copy, 0, sizeof(copy));
	copy.bufferRowLength = VKDEMO_WIDTH;
	copy.bufferImageHeight = VKDEMO_HEIGHT;
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent.width = VKDEMO_WIDTH;
	copy.imageExtent.height = VKDEMO_HEIGHT;
	copy.imageExtent.depth = 1;
	vkCmdCopyImageToBuffer(renderer.command, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, renderer.readback.buffer, 1, &copy);

	/* Make transfer writes visible to CPU reads after the completion fence signals. */
	buffer_barrier(renderer.readback.buffer, VKDEMO_BYTES, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT);

	/* Return only actual swapchain images to the standard presentation layout. */
	if (renderer.offscreen == 0)
		image_barrier(color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

	/* Succeeded: this recording contains real shader work and its own GPU readback. */
	return 0;
}

/* Observe completed mapped memory and decode only its declared channel layout. */
static int
read_pixels(
	void)
{
	VkMappedMemoryRange range;
	VkResult status;
	uint32_t offset;
	uint8_t red;

	/* Device writes require invalidation when the chosen type is non-coherent. */
	if ((renderer.readback.properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0) {
		/* Whole-allocation invalidation obeys every non-coherent atom-size limit. */
		memset(&range, 0, sizeof(range));
		range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		range.memory = renderer.readback.memory;
		range.size = VK_WHOLE_SIZE;
		status = vkInvalidateMappedMemoryRanges(renderer.device, 1, &range);
		if (status != VK_SUCCESS) {
			record_error(status, "vkInvalidateMappedMemoryRanges");
			return -1;
		}
	}

	/* Preserve GPU bytes before formatting or hashing them as ordinary RGB. */
	memcpy(renderer.pixels, renderer.readback.mapping, sizeof(renderer.pixels));

	/* BGRA surfaces have a different standard byte representation of the same color. */
	if (renderer.color_format == VK_FORMAT_B8G8R8A8_UNORM) {
		/* Decode channel order without recoloring or substituting any sample. */
		for (offset = 0; offset < VKDEMO_BYTES; offset += 4U) {
			red = renderer.pixels[offset + 2U];
			renderer.pixels[offset + 2U] = renderer.pixels[offset];
			renderer.pixels[offset] = red;
		}
	}

	/* Succeeded: CPU access follows both Vulkan execution and memory visibility. */
	return 0;
}

/* Store one shader input float in little-endian Vulkan vertex-buffer order. */
static void
store_float(
	uint8_t *destination,
	float number)
{
	uint32_t bits;

	/* Preserve float representation without unaligned or aliased stores. */
	memcpy(&bits, &number, sizeof(bits));
	destination[0] = (uint8_t)bits;
	destination[1] = (uint8_t)(bits >> 8);
	destination[2] = (uint8_t)(bits >> 16);
	destination[3] = (uint8_t)(bits >> 24);

	/* Succeeded: the vertex attribute now has the required byte representation. */
	return;
}

/* Construct the original cuboid vertices and asymmetric checker texture. */
static void
prepare_upload(
	void)
{
	static const uint32_t triangles[6] = {0, 1, 2, 0, 2, 3};
	static const float coordinates[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
	uint32_t face;
	uint32_t vertex;
	uint32_t corner;
	uint32_t component;
	uint32_t offset;
	uint32_t x;
	uint32_t y;
	uint32_t checker;

	/* Expand six independent face quads into thirty-six position/UV vertices. */
	memset(renderer.upload, 0, sizeof(renderer.upload));
	for (face = 0; face < 6U; face++) {
		/* Duplicate face corners so interpolation never joins unrelated face UVs. */
		for (vertex = 0; vertex < 6U; vertex++) {
			/* Write the original local-space position before the shader rotates it. */
			corner = triangles[vertex];
			offset = (face * 6U + vertex) * VERTEX_STRIDE;
			for (component = 0; component < 3U; component++) {
				store_float(
					&renderer.upload[offset + component * 4U],
					face_corners[face][corner][component]);
			}

			/* Store the face UV endpoints in the following two float attributes. */
			store_float(&renderer.upload[offset + 12U], coordinates[corner][0]);
			store_float(&renderer.upload[offset + 16U], coordinates[corner][1]);
		}
	}

	/* Give the checker distinct U/V gradients so the oracle can detect bad UVs. */
	for (y = 0; y < TEXTURE_WIDTH; y++) {
		/* Fill texture rows in exactly the same order used by buffer-to-image copy. */
		for (x = 0; x < TEXTURE_WIDTH; x++) {
			/* Alternate the red channel in eight-texel checker squares. */
			offset = TEXTURE_OFFSET + (y * TEXTURE_WIDTH + x) * 4U;
			checker = (x / 8U + y / 8U) & 1U;
			renderer.upload[offset] = 32;
			if (checker != 0)
				renderer.upload[offset] = 224;

			/* Encode horizontal and vertical texture location without changing alpha. */
			renderer.upload[offset + 1U] = (uint8_t)(32U + x * 3U);
			renderer.upload[offset + 2U] = (uint8_t)(32U + y * 3U);
			renderer.upload[offset + 3U] = 255;
		}
	}

	/* Succeeded: only geometry and texture inputs were painted by the CPU. */
	return;
}

/* Hash opaque GPU pixels in the same RGB row order used by the host capture. */
static int
hash_pixels(
	char digest[65])
{
	static const char hexadecimal[] = "0123456789abcdef";
	struct command_sha256_context hash;
	uint8_t row[VKDEMO_WIDTH * 3U];
	uint8_t binary[32];
	uint32_t x;
	uint32_t y;
	uint32_t offset;
	uint32_t index;
	int status;

	/* Start an independent SHA256 over RGB only, excluding stored alpha. */
	command_sha256_init(&hash);

	/* Preserve top-to-bottom, left-to-right framebuffer order. */
	for (y = 0; y < VKDEMO_HEIGHT; y++) {
		/* Extract RGB without repairing, recoloring or substituting any GPU byte. */
		for (x = 0; x < VKDEMO_WIDTH; x++) {
			/* Both the background and fragment shader must remain fully opaque. */
			offset = (y * VKDEMO_WIDTH + x) * 4U;
			if (renderer.pixels[offset + 3U] != 255) {
				errno = EIO;
				return -1;
			}

			/* Feed the exact displayed color components into the row digest. */
			row[x * 3U] = renderer.pixels[offset];
			row[x * 3U + 1U] = renderer.pixels[offset + 1U];
			row[x * 3U + 2U] = renderer.pixels[offset + 2U];
		}

		/* Hash one complete RGB row using the existing base SHA256 implementation. */
		status = command_sha256_update(&hash, row, sizeof(row));
		if (status != 0)
			return -1;
	}

	/* Finalize the actual frame digest before formatting its portable text form. */
	command_sha256_final(&hash, binary);

	/* Encode exactly sixty-four lowercase hexadecimal digits for capture matching. */
	for (index = 0; index < 32U; index++) {
		digest[index * 2U] = hexadecimal[binary[index] >> 4];
		digest[index * 2U + 1U] = hexadecimal[binary[index] & 15U];
	}

	/* Terminate the printable digest without modifying the framebuffer bytes. */
	digest[64] = '\0';

	/* Succeeded: the digest identifies every RGB byte of the real GPU readback. */
	return 0;
}

/* Release an image view, then its image, then the memory backing that image. */
static void
destroy_image(
	struct demo_image *image)
{
	/* Views cannot outlive the application image they reference. */
	if (image->view != VK_NULL_HANDLE)
		vkDestroyImageView(renderer.device, image->view, NULL);

	/* The resource must stop referencing its allocation before that allocation is freed. */
	if (image->image != VK_NULL_HANDLE)
		vkDestroyImage(renderer.device, image->image, NULL);

	/* Successful allocation may precede a later binding or view failure. */
	if (image->memory != VK_NULL_HANDLE)
		vkFreeMemory(renderer.device, image->memory, NULL);

	/* Succeeded: this application-owned image retains no Vulkan resources. */
	return;
}

/* Release a completed mapping, buffer and allocation in dependency order. */
static void
destroy_buffer(
	struct demo_buffer *buffer)
{
	/* A successful map must be explicitly ended before freeing its allocation. */
	if (buffer->mapping != NULL)
		vkUnmapMemory(renderer.device, buffer->memory);

	/* Retire the buffer before the bound memory becomes invalid. */
	if (buffer->buffer != VK_NULL_HANDLE)
		vkDestroyBuffer(renderer.device, buffer->buffer, NULL);

	/* Partial setup may own memory even if binding or mapping failed. */
	if (buffer->memory != VK_NULL_HANDLE)
		vkFreeMemory(renderer.device, buffer->memory, NULL);

	/* Succeeded: this application-owned buffer and mapping are fully retired. */
	return;
}

/* Retire partial or complete scene objects after all device work is idle. */
static void
release_resources(
	void)
{
	uint32_t index;

	/* Command-pool destruction returns all recorded child command buffers. */
	if (renderer.command_pool != VK_NULL_HANDLE)
		vkDestroyCommandPool(renderer.device, renderer.command_pool, NULL);

	/* Device idle ensures the acquisition signal is no longer consumed by a queue. */
	if (renderer.acquired != VK_NULL_HANDLE)
		vkDestroySemaphore(renderer.device, renderer.acquired, NULL);

	/* The fence has no remaining pending submission after the device wait. */
	if (renderer.fence != VK_NULL_HANDLE)
		vkDestroyFence(renderer.device, renderer.fence, NULL);

	/* Partial target allocation may leave only a prefix of Vulkan objects alive. */
	if (renderer.targets != NULL) {
		/* Destroy each completed target's dependent resources before its image. */
		for (index = 0; index < renderer.target_count; index++) {
			/* Present semaphore consumption was ordered by the device idle wait. */
			if (renderer.targets[index].rendered != VK_NULL_HANDLE)
				vkDestroySemaphore(renderer.device, renderer.targets[index].rendered, NULL);

			/* Framebuffers own references to both color and depth views. */
			if (renderer.targets[index].framebuffer != VK_NULL_HANDLE)
				vkDestroyFramebuffer(renderer.device, renderer.targets[index].framebuffer, NULL);

			/* A borrowed swapchain image itself remains owned by its swapchain. */
			if (renderer.targets[index].view != VK_NULL_HANDLE)
				vkDestroyImageView(renderer.device, renderer.targets[index].view, NULL);
		}

		/* No Vulkan object borrows the host target array after those destructions. */
		free(renderer.targets);
		renderer.targets = NULL;
	}

	/* Pipelines must retire before their layouts and render pass. */
	if (renderer.pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(renderer.device, renderer.pipeline, NULL);

	/* Shader modules are no longer referenced by any pending creation or command. */
	if (renderer.vertex_shader != VK_NULL_HANDLE)
		vkDestroyShaderModule(renderer.device, renderer.vertex_shader, NULL);

	/* Retire the independent fragment module as well. */
	if (renderer.fragment_shader != VK_NULL_HANDLE)
		vkDestroyShaderModule(renderer.device, renderer.fragment_shader, NULL);

	/* No framebuffer or pipeline now refers to this render pass. */
	if (renderer.render_pass != VK_NULL_HANDLE)
		vkDestroyRenderPass(renderer.device, renderer.render_pass, NULL);

	/* Pipeline layout lifetime ends before descriptor-layout destruction. */
	if (renderer.pipeline_layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(renderer.device, renderer.pipeline_layout, NULL);

	/* The pool returns its descriptor set and its references to texture and sampler. */
	if (renderer.descriptor_pool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(renderer.device, renderer.descriptor_pool, NULL);

	/* No pipeline layout or descriptor allocation needs the set layout now. */
	if (renderer.set_layout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(renderer.device, renderer.set_layout, NULL);

	/* The checker sampler has no remaining descriptor or GPU use. */
	if (renderer.sampler != VK_NULL_HANDLE)
		vkDestroySampler(renderer.device, renderer.sampler, NULL);

	/* Views and resources retire before their bound image allocations. */
	destroy_image(&renderer.texture);
	destroy_image(&renderer.depth);
	destroy_image(&renderer.color);

	/* CPU mappings and buffer resources likewise retire before their allocations. */
	destroy_buffer(&renderer.readback);
	destroy_buffer(&renderer.upload_buffer);

	/* Succeeded: only the device, optional swapchain and instance remain. */
	return;
}
