/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Video Player's own presenter (ws236-p011, after Image Viewer's): a
 * window of KL_PRESENT_NONE drawn with standard Vulkan and Wayland WSI.
 *
 * Each frame clears a swapchain image to black, draws the decoded picture
 * the GPU holds as a quad where the player fits it -- its two planes
 * sampled from the decoder's own image, imported once by its memory
 * handle, and turned into RGB by the fragment shader -- and lays the
 * overlay over it by its premultiplied alpha: the bar of controls, a
 * notice, or a picture the CPU scaled, drawn by the CPU into ordinary
 * memory and copied into the overlay's image only when it changed.  An
 * empty overlay is not drawn.  Two frames may be on the GPU at once (each
 * with its command buffer, fence and acquire semaphore): a frame waits
 * only for the one two before it, and the overlay is written only once
 * every frame that may read it is done.  The caller frees a picture once
 * the frames that drew it are done (vp_present_done).
 *
 * The CPU touches no pixel of a picture the GPU holds: the device turns
 * its planes into textures and the shader converts and scales them.
 */

#include <vulkan/vulkan.h>
#include <keiland/keiland.h>

#include "videoplayer.h"
#include "userland/desktop/videoplayer/shaders.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* How long a frame may take on the GPU, in nanoseconds. */
#define PRESENT_TIMEOUT		10000000000ULL

/* One vertex: position and texture place, four floats. */
#define PRESENT_VERTEX_FLOATS	4U

/* A quad's vertices: two triangles. */
#define PRESENT_VERTICES	6U

/* The quads in the vertex buffer: the overlay's unit square, then the picture's of each frame on the GPU. */
#define PRESENT_QUADS		(1U + VP_PRESENT_FRAMES)

/* The push constants: the size, then the colour rows of the picture's shader. */
#define PRESENT_PUSH_FLOATS	16U

/* The descriptor sets: the overlay's and one for each imported picture image. */
#define PRESENT_SETS		(1U + VP_PRESENT_IMPORTS)

static VkResult present_device(struct vp_present *present);
static VkResult present_swapchain(struct vp_present *present, uint32_t width, uint32_t height, VkSwapchainKHR old);
static VkResult present_pass(struct vp_present *present);
static VkResult present_targets(struct vp_present *present);
static void present_targets_free(struct vp_present *present);
static VkResult present_commands(struct vp_present *present);
static VkResult present_memory(struct vp_present *present, const VkMemoryRequirements *requirements, VkDeviceMemory *memory);
static VkResult present_descriptors(struct vp_present *present);
static VkResult present_overlay(struct vp_present *present);
static void present_overlay_free(struct vp_present *present);
static VkResult present_vertices(struct vp_present *present);
static VkResult present_pipeline(struct vp_present *present, const uint32_t *fragment_code, size_t fragment_size, VkPipelineLayout layout, VkPipeline *pipeline);
static VkResult present_layouts(struct vp_present *present);
static VkResult present_module(struct vp_present *present, const uint32_t *code, size_t size, VkShaderModule *module);
static VkResult present_import(struct vp_present *present, const struct media_gpu_picture *picture, struct vp_present_import **found);
static VkResult present_import_make(struct vp_present *present, const struct media_gpu_picture *picture, struct vp_present_import *entry);
static void present_import_free(struct vp_present *present, struct vp_present_import *entry);
static void present_imports_free(struct vp_present *present);
static void present_record(struct vp_present *present, uint32_t slot, uint32_t image, const struct vp_present_quad *quad, struct vp_present_import *picture, int overlay);
static VkResult present_wait_slot(struct vp_present *present, uint32_t slot);
static VkResult present_wait_all(struct vp_present *present);
static uint64_t present_clock_us(void);
static void present_colour(const struct media_gpu_picture *picture, float *rows);

/*
 * Makes the Vulkan objects of the window: the device, the swapchain, the
 * overlay and the pipelines that draw the picture and the overlay.
 */
VkResult
vp_present_open(
	struct vp_present *present,
	struct kl_window *window)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo instance;
	const char *extensions[4];
	uint32_t width;
	uint32_t height;
	VkResult error;
	int failed;

	/* Nothing is owned yet. */
	memset(present, 0, sizeof(*present));

	/* The instance, with the surface extensions a Wayland window needs and those that import memory. */
	extensions[0] = VK_KHR_SURFACE_EXTENSION_NAME;
	extensions[1] = "VK_KHR_wayland_surface";
	extensions[2] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
	extensions[3] = VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME;
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "videoplayer";
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&instance, 0, sizeof(instance));
	instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance.pApplicationInfo = &application;
	instance.enabledExtensionCount = 4U;
	instance.ppEnabledExtensionNames = extensions;
	present->operation = "vkCreateInstance";
	error = vkCreateInstance(&instance, NULL, &present->instance);
	if (error != VK_SUCCESS)
		return error;

	/* The window's surface, made by libkeiland over the window. */
	present->operation = "kl_window_vulkan_surface";
	failed = kl_window_vulkan_surface(window, present->instance, &present->surface);
	if (failed != 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A device with a queue that draws and presents to the surface, and imports memory. */
	error = present_device(present);
	if (error != VK_SUCCESS)
		return error;

	/* The swapchain at the window's size. */
	kl_window_size(window, &width, &height);
	error = present_swapchain(present, width, height, VK_NULL_HANDLE);
	if (error != VK_SUCCESS)
		return error;

	/* The pass that draws into the swapchain's images. */
	error = present_pass(present);
	if (error != VK_SUCCESS)
		return error;

	/* A framebuffer for each swapchain image. */
	error = present_targets(present);
	if (error != VK_SUCCESS)
		return error;

	/* The command buffer and the frame's synchronization. */
	error = present_commands(present);
	if (error != VK_SUCCESS)
		return error;

	/* The samplers, the set layouts, the pool and the overlay's set. */
	error = present_descriptors(present);
	if (error != VK_SUCCESS)
		return error;

	/* The overlay image at the swapchain's size. */
	error = present_overlay(present);
	if (error != VK_SUCCESS)
		return error;

	/* The quads. */
	error = present_vertices(present);
	if (error != VK_SUCCESS)
		return error;

	/* The pipeline layouts and the two pipelines. */
	error = present_layouts(present);
	if (error != VK_SUCCESS)
		return error;
	error = present_pipeline(present, videoplayer_picture_frag, sizeof(videoplayer_picture_frag), present->picture_layout, &present->picture_pipeline);
	if (error != VK_SUCCESS)
		return error;
	error = present_pipeline(present, videoplayer_overlay_frag, sizeof(videoplayer_overlay_frag), present->overlay_layout, &present->overlay_pipeline);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: frames can be shown. */
	return VK_SUCCESS;
}

/*
 * Replaces the swapchain and the overlay image with ones of a new size.
 */
VkResult
vp_present_resize(
	struct vp_present *present,
	uint32_t width,
	uint32_t height)
{
	VkSwapchainKHR old;
	VkResult error;

	/* Nothing may still use the old images. */
	error = present_wait_all(present);
	if (error != VK_SUCCESS)
		return error;

	/* The old targets go, and the new chain replaces the old one. */
	present_targets_free(present);
	old = present->swapchain;
	present->swapchain = VK_NULL_HANDLE;
	error = present_swapchain(present, width, height, old);

	/* The old chain goes whether or not the new one was made; a chain that was not made fails the resize. */
	vkDestroySwapchainKHR(present->device, old, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* Framebuffers for the new images. */
	error = present_targets(present);
	if (error != VK_SUCCESS)
		return error;

	/* An overlay image of the new size. */
	present_overlay_free(present);
	error = present_overlay(present);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the next frame is shown at the new size. */
	return VK_SUCCESS;
}

/*
 * Shows a frame: black, the picture the GPU holds where quad places it
 * (NULL for none), and the overlay (pixels of the swapchain's size, stride
 * words a row, premultiplied) when overlay_shown, copied first when
 * overlay_changed.  The frame is submitted and presented, not waited for:
 * it is present->serial when this returns, and done once vp_present_done
 * reports it.
 *
 * Returns VK_ERROR_OUT_OF_DATE_KHR when the swapchain no longer matches
 * the window; the caller resizes and draws again.
 */
VkResult
vp_present_frame(
	struct vp_present *present,
	const uint32_t *pixels,
	size_t stride,
	int overlay_shown,
	int overlay_changed,
	const struct vp_present_quad *quad)
{
	struct vp_present_import *picture;
	VkSubmitInfo submit;
	VkPresentInfoKHR info;
	VkPipelineStageFlags stage;
	uint64_t started;
	uint32_t slot;
	uint32_t image;
	uint32_t row;
	VkResult error;

	/* The frame's slot, free once the frame two before it is done. */
	slot = (uint32_t)(present->serial % VP_PRESENT_FRAMES);
	started = present_clock_us();
	error = present_wait_slot(present, slot);
	present->wait_us += present_clock_us() - started;
	if (error != VK_SUCCESS)
		return error;

	/* The overlay's rows into its image, when they changed (a new overlay image has none yet), once no frame reads it. */
	if (overlay_shown && (overlay_changed || present->overlay_ready == 0)) {
		started = present_clock_us();
		error = present_wait_all(present);
		present->wait_us += present_clock_us() - started;
		if (error != VK_SUCCESS)
			return error;

		/* Each row, into the image's row as its pitch places it. */
		for (row = 0; row < present->extent.height; row++)
			memcpy(present->overlay_map + (size_t)row * present->overlay_pitch, pixels + (size_t)row * stride, (size_t)present->extent.width * 4U);
		present->overlay_written = 1;
	}

	/* The picture's image, imported the first time it is shown. */
	picture = NULL;
	if (quad != NULL) {
		error = present_import(present, &quad->picture, &picture);
		if (error != VK_SUCCESS)
			return error;
	}

	/* The image to draw into, once the compositor has given one back, however long a busy compositor takes (a closed connection ends the wait with an error). */
	started = present_clock_us();
	present->operation = "vkAcquireNextImageKHR";
	error = vkAcquireNextImageKHR(present->device, present->swapchain, UINT64_MAX, present->acquired[slot], VK_NULL_HANDLE, &image);
	present->acquire_us += present_clock_us() - started;
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return error;

	/* The frame's commands. */
	present->operation = "vkResetCommandBuffer";
	error = vkResetCommandBuffer(present->commands[slot], 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Records the frame and closes the recording. */
	present_record(present, slot, image, quad, picture, overlay_shown && present->overlay_written);
	present->operation = "vkEndCommandBuffer";
	error = vkEndCommandBuffer(present->commands[slot]);
	if (error != VK_SUCCESS)
		return error;

	/* The frame's fence starts unsignalled. */
	present->operation = "vkResetFences";
	error = vkResetFences(present->device, 1U, &present->fences[slot]);
	if (error != VK_SUCCESS)
		return error;

	/* Submits the frame after the acquire, signalling the image's present semaphore. */
	started = present_clock_us();
	stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1U;
	submit.pWaitSemaphores = &present->acquired[slot];
	submit.pWaitDstStageMask = &stage;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &present->commands[slot];
	submit.signalSemaphoreCount = 1U;
	submit.pSignalSemaphores = &present->targets[image].rendered;
	present->operation = "vkQueueSubmit";
	error = vkQueueSubmit(present->queue, 1U, &submit, present->fences[slot]);
	if (error != VK_SUCCESS)
		return error;

	/* The frame is on the GPU from here, under its serial. */
	present->serial++;
	present->slot_serial[slot] = present->serial;

	/* Presents the image to the window. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	info.waitSemaphoreCount = 1U;
	info.pWaitSemaphores = &present->targets[image].rendered;
	info.swapchainCount = 1U;
	info.pSwapchains = &present->swapchain;
	info.pImageIndices = &image;
	present->operation = "vkQueuePresentKHR";
	error = vkQueuePresentKHR(present->queue, &info);
	present->submit_us += present_clock_us() - started;
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return error;

	/* Succeeded: the frame is on its way to the window (a suboptimal swapchain is told, to be made again). */
	return error;
}

/*
 * Reports the serial of the newest frame known done: every frame up to it
 * has finished on the GPU, so what it drew may be freed.
 */
uint64_t
vp_present_done(
	const struct vp_present *present)
{
	/* The newest waited for, or every frame after a wait for all. */
	return present->completed;
}

/*
 * Waits for every frame on the GPU, as before freeing everything they drew.
 */
VkResult
vp_present_idle(
	struct vp_present *present)
{
	VkResult error;

	/* Each slot's frame. */
	error = present_wait_all(present);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: every frame is done. */
	return VK_SUCCESS;
}

/*
 * Releases every Vulkan object, children before their parents.
 */
void
vp_present_close(
	struct vp_present *present)
{
	uint32_t slot;

	/* The device's objects, once nothing runs. */
	if (present->device != VK_NULL_HANDLE) {
		(void)vkDeviceWaitIdle(present->device);
		present_targets_free(present);
		present_overlay_free(present);
		present_imports_free(present);

		/* The drawing objects. */
		if (present->picture_pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(present->device, present->picture_pipeline, NULL);
		if (present->overlay_pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(present->device, present->overlay_pipeline, NULL);
		if (present->picture_layout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(present->device, present->picture_layout, NULL);
		if (present->overlay_layout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(present->device, present->overlay_layout, NULL);
		if (present->descriptor_pool != VK_NULL_HANDLE)
			vkDestroyDescriptorPool(present->device, present->descriptor_pool, NULL);
		if (present->picture_set_layout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(present->device, present->picture_set_layout, NULL);
		if (present->overlay_set_layout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(present->device, present->overlay_set_layout, NULL);
		if (present->nearest != VK_NULL_HANDLE)
			vkDestroySampler(present->device, present->nearest, NULL);
		if (present->smooth != VK_NULL_HANDLE)
			vkDestroySampler(present->device, present->smooth, NULL);

		/* The quads' vertices, with their memory. */
		if (present->vertices != VK_NULL_HANDLE)
			vkDestroyBuffer(present->device, present->vertices, NULL);
		if (present->vertex_memory != VK_NULL_HANDLE)
			vkFreeMemory(present->device, present->vertex_memory, NULL);

		/* The commands and each slot's synchronization. */
		if (present->pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(present->device, present->pool, NULL);
		for (slot = 0U; slot < VP_PRESENT_FRAMES; slot++) {
			if (present->fences[slot] != VK_NULL_HANDLE)
				vkDestroyFence(present->device, present->fences[slot], NULL);
			if (present->acquired[slot] != VK_NULL_HANDLE)
				vkDestroySemaphore(present->device, present->acquired[slot], NULL);
		}

		/* The pass, the swapchain and the device itself. */
		if (present->pass != VK_NULL_HANDLE)
			vkDestroyRenderPass(present->device, present->pass, NULL);
		if (present->swapchain != VK_NULL_HANDLE)
			vkDestroySwapchainKHR(present->device, present->swapchain, NULL);
		vkDestroyDevice(present->device, NULL);
	}

	/* The surface and the instance. */
	if (present->surface != VK_NULL_HANDLE)
		vkDestroySurfaceKHR(present->instance, present->surface, NULL);
	if (present->instance != VK_NULL_HANDLE)
		vkDestroyInstance(present->instance, NULL);

	/* Nothing is owned any more. */
	memset(present, 0, sizeof(*present));
}

/*
 * Chooses a physical device and a queue family that draws and presents to
 * the surface, and makes the device with the swapchain and the import of
 * memory by fd.
 */
static VkResult
present_device(
	struct vp_present *present)
{
	static const char *const extensions[] = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME
	};
	VkPhysicalDevice devices[8];
	VkQueueFamilyProperties families[16];
	VkExtensionProperties *available;
	VkDeviceQueueCreateInfo queue;
	VkDeviceCreateInfo create;
	float priority;
	uint32_t extension_count;
	uint32_t candidate;
	uint32_t matched;
	uint32_t extension;
	int same;
	uint32_t count;
	uint32_t family_count;
	uint32_t index;
	uint32_t family;
	VkBool32 supported;
	VkResult error;

	/* The physical devices (the first eight are enough). */
	count = 8U;
	present->operation = "vkEnumeratePhysicalDevices";
	error = vkEnumeratePhysicalDevices(present->instance, &count, devices);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;

	/* The first family of any device that draws and presents to this surface. */
	for (index = 0U;
	     index < count && present->physical == VK_NULL_HANDLE;
	     index++) {
		/* The device's queue families (the first sixteen are enough). */
		family_count = 16U;
		vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &family_count, families);
		for (family = 0U; family < family_count; family++) {
			/* A family must draw and have a queue. */
			if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U || families[family].queueCount == 0U)
				continue;

			/* And present to this very surface. */
			supported = VK_FALSE;
			error = vkGetPhysicalDeviceSurfaceSupportKHR(devices[index], family, present->surface, &supported);
			if (error != VK_SUCCESS || supported == VK_FALSE)
				continue;

			/* This family of this device draws the window. */
			present->physical = devices[index];
			present->family = family;
			break;
		}
	}

	/* No device can draw this window. */
	if (present->physical == VK_NULL_HANDLE)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Whether the device imports memory by fd: without it only pictures the CPU scaled are shown. */
	matched = 0U;
	extension_count = 0U;
	present->operation = "vkEnumerateDeviceExtensionProperties";
	error = vkEnumerateDeviceExtensionProperties(present->physical, NULL, &extension_count, NULL);
	if (error != VK_SUCCESS)
		return error;
	available = calloc(extension_count + 1U, sizeof(*available));
	if (available == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	error = vkEnumerateDeviceExtensionProperties(present->physical, NULL, &extension_count, available);
	for (extension = 1U; error == VK_SUCCESS && extension < 3U; extension++) {
		/* Each importing extension the device lists. */
		for (candidate = 0U; candidate < extension_count; candidate++) {
			same = strcmp(available[candidate].extensionName, extensions[extension]);
			if (same == 0) {
				matched++;
				break;
			}
		}
	}

	/* The list goes; a failed enumeration fails the device. */
	free(available);
	if (error != VK_SUCCESS)
		return error;
	present->imports_supported = 0;
	if (matched == 2U)
		present->imports_supported = 1;

	/* One queue of that family and the extensions. */
	priority = 1.0f;
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = present->family;
	queue.queueCount = 1U;
	queue.pQueuePriorities = &priority;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	create.queueCreateInfoCount = 1U;
	create.pQueueCreateInfos = &queue;
	create.enabledExtensionCount = 1U;
	if (present->imports_supported)
		create.enabledExtensionCount = 3U;
	create.ppEnabledExtensionNames = extensions;
	present->operation = "vkCreateDevice";
	error = vkCreateDevice(present->physical, &create, NULL, &present->device);
	if (error != VK_SUCCESS)
		return error;

	/* The device's one queue. */
	vkGetDeviceQueue(present->device, present->family, 0U, &present->queue);

	/* Succeeded: the device and its queue. */
	return VK_SUCCESS;
}

/* Makes the swapchain at a size, in an 8-bit UNORM format, presenting in FIFO order. */
static VkResult
present_swapchain(
	struct vp_present *present,
	uint32_t width,
	uint32_t height,
	VkSwapchainKHR old)
{
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR formats[16];
	VkSwapchainCreateInfoKHR create;
	uint32_t count;
	uint32_t index;
	VkResult error;

	/* The formats the surface offers. */
	count = 16U;
	present->operation = "vkGetPhysicalDeviceSurfaceFormatsKHR";
	error = vkGetPhysicalDeviceSurfaceFormatsKHR(present->physical, present->surface, &count, formats);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;

	/* The first of those formats that is 8-bit UNORM. */
	present->format = VK_FORMAT_UNDEFINED;
	for (index = 0U; index < count; index++) {
		/* Either order of the four 8-bit channels will do. */
		if (formats[index].format == VK_FORMAT_B8G8R8A8_UNORM || formats[index].format == VK_FORMAT_R8G8B8A8_UNORM) {
			present->format = formats[index].format;
			break;
		}
	}

	/* A surface without one cannot show the colors as they are. */
	if (present->format == VK_FORMAT_UNDEFINED)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* The size, kept inside what the surface takes. */
	present->operation = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR";
	error = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(present->physical, present->surface, &capabilities);
	if (error != VK_SUCCESS)
		return error;

	/* The window's size, clamped to the surface's range. */
	if (width < capabilities.minImageExtent.width)
		width = capabilities.minImageExtent.width;
	if (height < capabilities.minImageExtent.height)
		height = capabilities.minImageExtent.height;
	if (width > capabilities.maxImageExtent.width)
		width = capabilities.maxImageExtent.width;
	if (height > capabilities.maxImageExtent.height)
		height = capabilities.maxImageExtent.height;
	present->extent.width = width;
	present->extent.height = height;

	/* Three images when the surface allows, opaque, replacing the old chain. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	create.surface = present->surface;
	create.minImageCount = 3U;
	if (create.minImageCount < capabilities.minImageCount)
		create.minImageCount = capabilities.minImageCount;
	if (capabilities.maxImageCount != 0U && create.minImageCount > capabilities.maxImageCount)
		create.minImageCount = capabilities.maxImageCount;
	create.imageFormat = present->format;
	create.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	create.imageExtent = present->extent;
	create.imageArrayLayers = 1U;
	create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.preTransform = capabilities.currentTransform;
	create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	create.clipped = VK_TRUE;
	create.oldSwapchain = old;
	present->operation = "vkCreateSwapchainKHR";
	error = vkCreateSwapchainKHR(present->device, &create, NULL, &present->swapchain);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the chain at the window's size. */
	return VK_SUCCESS;
}

/* Makes the pass: one color attachment, cleared to black, drawn over and left for presenting. */
static VkResult
present_pass(
	struct vp_present *present)
{
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkRenderPassCreateInfo create;
	VkResult error;

	/* The swapchain image, cleared each frame. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = present->format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	/* The single subpass that draws into it. */
	reference.attachment = 0U;
	reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1U;
	subpass.pColorAttachments = &reference;

	/* The pass. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	create.attachmentCount = 1U;
	create.pAttachments = &attachment;
	create.subpassCount = 1U;
	create.pSubpasses = &subpass;
	present->operation = "vkCreateRenderPass";
	error = vkCreateRenderPass(present->device, &create, NULL, &present->pass);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the pass. */
	return VK_SUCCESS;
}

/* Makes a view, a framebuffer and a present semaphore for each swapchain image. */
static VkResult
present_targets(
	struct vp_present *present)
{
	VkImage images[8];
	VkImageViewCreateInfo view;
	VkFramebufferCreateInfo framebuffer;
	VkSemaphoreCreateInfo semaphore;
	uint32_t index;
	VkResult error;

	/* The swapchain's images (at most eight). */
	present->count = 8U;
	present->operation = "vkGetSwapchainImagesKHR";
	error = vkGetSwapchainImagesKHR(present->device, present->swapchain, &present->count, images);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;

	/* A zeroed table, so that a failure part-way leaves only made objects to release. */
	present->targets = calloc(present->count, sizeof(present->targets[0]));
	if (present->targets == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Each image's objects. */
	for (index = 0U; index < present->count; index++) {
		/* The view of the image. */
		memset(&view, 0, sizeof(view));
		view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view.image = images[index];
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = present->format;
		view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		view.subresourceRange.levelCount = 1U;
		view.subresourceRange.layerCount = 1U;
		present->operation = "vkCreateImageView";
		error = vkCreateImageView(present->device, &view, NULL, &present->targets[index].view);
		if (error != VK_SUCCESS)
			return error;

		/* The framebuffer over it. */
		memset(&framebuffer, 0, sizeof(framebuffer));
		framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebuffer.renderPass = present->pass;
		framebuffer.attachmentCount = 1U;
		framebuffer.pAttachments = &present->targets[index].view;
		framebuffer.width = present->extent.width;
		framebuffer.height = present->extent.height;
		framebuffer.layers = 1U;
		present->operation = "vkCreateFramebuffer";
		error = vkCreateFramebuffer(present->device, &framebuffer, NULL, &present->targets[index].framebuffer);
		if (error != VK_SUCCESS)
			return error;

		/* The semaphore its present waits for. */
		memset(&semaphore, 0, sizeof(semaphore));
		semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		present->operation = "vkCreateSemaphore";
		error = vkCreateSemaphore(present->device, &semaphore, NULL, &present->targets[index].rendered);
		if (error != VK_SUCCESS)
			return error;
	}

	/* Succeeded: every image can be drawn into. */
	return VK_SUCCESS;
}

/* Releases the objects of the swapchain's images (not the images, which are the swapchain's). */
static void
present_targets_free(
	struct vp_present *present)
{
	uint32_t index;

	/* Without a table there is nothing of the images to release. */
	if (present->targets != NULL) {
		/* Each image's semaphore, framebuffer and view, where made. */
		for (index = 0U; index < present->count; index++) {
			/* The semaphore its present waited for. */
			if (present->targets[index].rendered != VK_NULL_HANDLE)
				vkDestroySemaphore(present->device, present->targets[index].rendered, NULL);

			/* The framebuffer over it. */
			if (present->targets[index].framebuffer != VK_NULL_HANDLE)
				vkDestroyFramebuffer(present->device, present->targets[index].framebuffer, NULL);

			/* The view of it. */
			if (present->targets[index].view != VK_NULL_HANDLE)
				vkDestroyImageView(present->device, present->targets[index].view, NULL);
		}
	}

	/* The table itself. */
	free(present->targets);
	present->targets = NULL;
	present->count = 0U;
}

/* Makes the command pool and buffer, the frame's fence and the acquire semaphore. */
static VkResult
present_commands(
	struct vp_present *present)
{
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo command;
	VkFenceCreateInfo fence;
	VkSemaphoreCreateInfo semaphore;
	uint32_t slot;
	VkResult error;

	/* A pool whose buffers are reset every frame. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = present->family;
	present->operation = "vkCreateCommandPool";
	error = vkCreateCommandPool(present->device, &pool, NULL, &present->pool);
	if (error != VK_SUCCESS)
		return error;

	/* A buffer for each frame on the GPU. */
	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = present->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = VP_PRESENT_FRAMES;
	present->operation = "vkAllocateCommandBuffers";
	error = vkAllocateCommandBuffers(present->device, &command, present->commands);
	if (error != VK_SUCCESS)
		return error;

	/* Each frame's fence, which its end signals, and the semaphore its acquire signals. */
	for (slot = 0U; slot < VP_PRESENT_FRAMES; slot++) {
		memset(&fence, 0, sizeof(fence));
		fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		present->operation = "vkCreateFence";
		error = vkCreateFence(present->device, &fence, NULL, &present->fences[slot]);
		if (error != VK_SUCCESS)
			return error;
		memset(&semaphore, 0, sizeof(semaphore));
		semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		present->operation = "vkCreateSemaphore";
		error = vkCreateSemaphore(present->device, &semaphore, NULL, &present->acquired[slot]);
		if (error != VK_SUCCESS)
			return error;
	}

	/* Succeeded: VP_PRESENT_FRAMES frames can be on the GPU at once. */
	return VK_SUCCESS;
}

/* Allocates host-visible, coherent memory that meets the requirements. */
static VkResult
present_memory(
	struct vp_present *present,
	const VkMemoryRequirements *requirements,
	VkDeviceMemory *memory)
{
	VkPhysicalDeviceMemoryProperties properties;
	VkMemoryAllocateInfo allocate;
	VkMemoryPropertyFlags wanted;
	uint32_t index;
	VkResult error;

	/* The first allowed type that the host sees and keeps coherent. */
	vkGetPhysicalDeviceMemoryProperties(present->physical, &properties);
	wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (index = 0U; index < properties.memoryTypeCount; index++) {
		/* A type the resource allows, with every property wanted. */
		if ((requirements->memoryTypeBits & (1U << index)) != 0U && (properties.memoryTypes[index].propertyFlags & wanted) == wanted)
			break;
	}

	/* Without one the host cannot write the overlay. */
	if (index == properties.memoryTypeCount)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* The allocation. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements->size;
	allocate.memoryTypeIndex = index;
	present->operation = "vkAllocateMemory";
	error = vkAllocateMemory(present->device, &allocate, NULL, memory);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: host-visible memory. */
	return VK_SUCCESS;
}

/*
 * Makes the samplers, the set layouts (the picture's two planes, the
 * overlay's one image), the pool and the overlay's set.
 */
static VkResult
present_descriptors(
	struct vp_present *present)
{
	VkSamplerCreateInfo sampler;
	VkDescriptorSetLayoutBinding bindings[2];
	VkDescriptorSetLayoutCreateInfo set_layout;
	VkDescriptorPoolSize pool_size;
	VkDescriptorPoolCreateInfo pool;
	VkDescriptorSetAllocateInfo allocate;
	VkResult error;

	/* Nearest sampling for the overlay, which is the window's size, texel for pixel. */
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_NEAREST;
	sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	present->operation = "vkCreateSampler";
	error = vkCreateSampler(present->device, &sampler, NULL, &present->nearest);
	if (error != VK_SUCCESS)
		return error;

	/* Bilinear sampling for the picture's planes, which are scaled to the window. */
	sampler.magFilter = VK_FILTER_LINEAR;
	sampler.minFilter = VK_FILTER_LINEAR;
	present->operation = "vkCreateSampler";
	error = vkCreateSampler(present->device, &sampler, NULL, &present->smooth);
	if (error != VK_SUCCESS)
		return error;

	/* The picture's set: its luma at binding 0 and its chroma at binding 1. */
	memset(bindings, 0, sizeof(bindings));
	bindings[0].binding = 0U;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[0].descriptorCount = 1U;
	bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	bindings[1] = bindings[0];
	bindings[1].binding = 1U;
	memset(&set_layout, 0, sizeof(set_layout));
	set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout.bindingCount = 2U;
	set_layout.pBindings = bindings;
	present->operation = "vkCreateDescriptorSetLayout picture";
	error = vkCreateDescriptorSetLayout(present->device, &set_layout, NULL, &present->picture_set_layout);
	if (error != VK_SUCCESS)
		return error;

	/* The overlay's set: its image at binding 0. */
	set_layout.bindingCount = 1U;
	present->operation = "vkCreateDescriptorSetLayout overlay";
	error = vkCreateDescriptorSetLayout(present->device, &set_layout, NULL, &present->overlay_set_layout);
	if (error != VK_SUCCESS)
		return error;

	/* A pool for the overlay's set and the imported pictures' (freed as the decoder changes). */
	memset(&pool_size, 0, sizeof(pool_size));
	pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_size.descriptorCount = 1U + 2U * VP_PRESENT_IMPORTS;
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pool.maxSets = PRESENT_SETS;
	pool.poolSizeCount = 1U;
	pool.pPoolSizes = &pool_size;
	present->operation = "vkCreateDescriptorPool";
	error = vkCreateDescriptorPool(present->device, &pool, NULL, &present->descriptor_pool);
	if (error != VK_SUCCESS)
		return error;

	/* The overlay's set, pointed at the overlay once it exists. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = present->descriptor_pool;
	allocate.descriptorSetCount = 1U;
	allocate.pSetLayouts = &present->overlay_set_layout;
	present->operation = "vkAllocateDescriptorSets overlay";
	error = vkAllocateDescriptorSets(present->device, &allocate, &present->overlay_set);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the overlay and the pictures can be bound. */
	return VK_SUCCESS;
}

/* Makes the overlay image at the swapchain's size (linear, host-written, mapped for good) and binds it to its set. */
static VkResult
present_overlay(
	struct vp_present *present)
{
	VkImageCreateInfo image;
	VkMemoryRequirements requirements;
	VkImageSubresource subresource;
	VkSubresourceLayout layout;
	VkImageViewCreateInfo view;
	VkDescriptorImageInfo image_info;
	VkWriteDescriptorSet write;
	void *map;
	VkResult error;

	/* The image: linear so that the host writes its rows, sampled by the fragment shader. */
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = VK_FORMAT_B8G8R8A8_UNORM;
	image.extent.width = present->extent.width;
	image.extent.height = present->extent.height;
	image.extent.depth = 1U;
	image.mipLevels = 1U;
	image.arrayLayers = 1U;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_LINEAR;
	image.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
	present->operation = "vkCreateImage overlay";
	error = vkCreateImage(present->device, &image, NULL, &present->overlay);
	if (error != VK_SUCCESS)
		return error;

	/* Its memory. */
	vkGetImageMemoryRequirements(present->device, present->overlay, &requirements);
	error = present_memory(present, &requirements, &present->overlay_memory);
	if (error != VK_SUCCESS)
		return error;

	/* Binds the memory to the image. */
	present->operation = "vkBindImageMemory overlay";
	error = vkBindImageMemory(present->device, present->overlay, present->overlay_memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Maps it for the host's writes, for good. */
	present->operation = "vkMapMemory overlay";
	error = vkMapMemory(present->device, present->overlay_memory, 0U, VK_WHOLE_SIZE, 0U, &map);
	if (error != VK_SUCCESS)
		return error;

	/* Where the rows start and how far apart they are; nothing is written yet. */
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vkGetImageSubresourceLayout(present->device, present->overlay, &subresource, &layout);
	present->overlay_map = (unsigned char *)map + layout.offset;
	present->overlay_pitch = (size_t)layout.rowPitch;
	present->overlay_ready = 0;
	present->overlay_written = 0;

	/* The view the shader samples. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = present->overlay;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_B8G8R8A8_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	present->operation = "vkCreateImageView overlay";
	error = vkCreateImageView(present->device, &view, NULL, &present->overlay_view);
	if (error != VK_SUCCESS)
		return error;

	/* The set names the overlay in the general layout it is kept in. */
	memset(&image_info, 0, sizeof(image_info));
	image_info.sampler = present->nearest;
	image_info.imageView = present->overlay_view;
	image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = present->overlay_set;
	write.dstBinding = 0U;
	write.descriptorCount = 1U;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image_info;
	vkUpdateDescriptorSets(present->device, 1U, &write, 0U, NULL);

	/* Succeeded: frames can be written into the overlay. */
	return VK_SUCCESS;
}

/* Releases the overlay image, its view and its memory. */
static void
present_overlay_free(
	struct vp_present *present)
{
	/* The view, the image and the memory, where made. */
	if (present->overlay_view != VK_NULL_HANDLE)
		vkDestroyImageView(present->device, present->overlay_view, NULL);
	if (present->overlay != VK_NULL_HANDLE)
		vkDestroyImage(present->device, present->overlay, NULL);
	if (present->overlay_memory != VK_NULL_HANDLE)
		vkFreeMemory(present->device, present->overlay_memory, NULL);

	/* Nothing of the overlay is left. */
	present->overlay_view = VK_NULL_HANDLE;
	present->overlay = VK_NULL_HANDLE;
	present->overlay_memory = VK_NULL_HANDLE;
	present->overlay_map = NULL;
	present->overlay_ready = 0;
	present->overlay_written = 0;
}

/*
 * Makes the vertex buffer of the two quads, mapped for good: the overlay's
 * unit square (which a pushed size of one stretches over the window), and
 * room for the picture's, written each frame.
 */
static VkResult
present_vertices(
	struct vp_present *present)
{
	static const float quad[PRESENT_VERTICES * PRESENT_VERTEX_FLOATS] = {
		0.0f, 0.0f, 0.0f, 0.0f,
		1.0f, 0.0f, 1.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 1.0f,
		1.0f, 0.0f, 1.0f, 0.0f,
		1.0f, 1.0f, 1.0f, 1.0f,
		0.0f, 1.0f, 0.0f, 1.0f
	};
	VkBufferCreateInfo buffer;
	VkMemoryRequirements requirements;
	void *map;
	VkResult error;

	/* The buffer of both quads' corners. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.size = sizeof(quad) * PRESENT_QUADS;
	buffer.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	present->operation = "vkCreateBuffer";
	error = vkCreateBuffer(present->device, &buffer, NULL, &present->vertices);
	if (error != VK_SUCCESS)
		return error;

	/* Its memory. */
	vkGetBufferMemoryRequirements(present->device, present->vertices, &requirements);
	error = present_memory(present, &requirements, &present->vertex_memory);
	if (error != VK_SUCCESS)
		return error;

	/* Binds the memory to the buffer. */
	present->operation = "vkBindBufferMemory";
	error = vkBindBufferMemory(present->device, present->vertices, present->vertex_memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Maps the buffer for good, and writes the overlay's corners once. */
	present->operation = "vkMapMemory vertices";
	error = vkMapMemory(present->device, present->vertex_memory, 0U, VK_WHOLE_SIZE, 0U, &map);
	if (error != VK_SUCCESS)
		return error;

	/* The overlay's corners, which never change. */
	present->vertex_map = map;
	memcpy(present->vertex_map, quad, sizeof(quad));

	/* Succeeded: the quads can be drawn. */
	return VK_SUCCESS;
}

/*
 * Makes the pipeline layouts: the picture's (its set, and the size and the
 * colour rows pushed for both stages) and the overlay's (its set, and the
 * size pushed for the vertex stage).
 */
static VkResult
present_layouts(
	struct vp_present *present)
{
	VkPushConstantRange push;
	VkPipelineLayoutCreateInfo layout;
	VkResult error;

	/* The picture's: the size for the vertex stage and the rows for the fragment stage, in one block. */
	memset(&push, 0, sizeof(push));
	push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	push.offset = 0U;
	push.size = PRESENT_PUSH_FLOATS * sizeof(float);
	memset(&layout, 0, sizeof(layout));
	layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout.setLayoutCount = 1U;
	layout.pSetLayouts = &present->picture_set_layout;
	layout.pushConstantRangeCount = 1U;
	layout.pPushConstantRanges = &push;
	present->operation = "vkCreatePipelineLayout picture";
	error = vkCreatePipelineLayout(present->device, &layout, NULL, &present->picture_layout);
	if (error != VK_SUCCESS)
		return error;

	/* The overlay's: the size for the vertex stage. */
	push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	push.size = 4U * sizeof(float);
	layout.pSetLayouts = &present->overlay_set_layout;
	present->operation = "vkCreatePipelineLayout overlay";
	error = vkCreatePipelineLayout(present->device, &layout, NULL, &present->overlay_layout);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: both layouts. */
	return VK_SUCCESS;
}

/*
 * Makes one pipeline: the quad's vertex shader, a fragment shader, one vec4
 * attribute, and blending by premultiplied alpha (the picture is opaque).
 */
static VkResult
present_pipeline(
	struct vp_present *present,
	const uint32_t *fragment_code,
	size_t fragment_size,
	VkPipelineLayout layout,
	VkPipeline *made)
{
	static const VkDynamicState dynamic[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};
	VkShaderModule vertex;
	VkShaderModule fragment;
	VkPipelineShaderStageCreateInfo stages[2];
	VkVertexInputBindingDescription binding;
	VkVertexInputAttributeDescription attribute;
	VkPipelineVertexInputStateCreateInfo input;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineColorBlendAttachmentState blend_attachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkPipelineDynamicStateCreateInfo dynamic_state;
	VkGraphicsPipelineCreateInfo pipeline;
	VkResult error;

	/* The vertex shader module. */
	error = present_module(present, videoplayer_picture_vert, sizeof(videoplayer_picture_vert), &vertex);
	if (error != VK_SUCCESS)
		return error;

	/* The fragment module; the vertex module goes when it cannot be made. */
	error = present_module(present, fragment_code, fragment_size, &fragment);
	if (error != VK_SUCCESS) {
		vkDestroyShaderModule(present->device, vertex, NULL);
		return error;
	}

	/* The two stages. */
	memset(stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertex;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragment;
	stages[1].pName = "main";

	/* One vertex buffer of one vec4 a vertex. */
	memset(&binding, 0, sizeof(binding));
	binding.binding = 0U;
	binding.stride = PRESENT_VERTEX_FLOATS * sizeof(float);
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	memset(&attribute, 0, sizeof(attribute));
	attribute.location = 0U;
	attribute.binding = 0U;
	attribute.format = VK_FORMAT_R32G32B32A32_SFLOAT;
	attribute.offset = 0U;

	/* The input state: that buffer, drawn as a list of triangles. */
	memset(&input, 0, sizeof(input));
	input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	input.vertexBindingDescriptionCount = 1U;
	input.pVertexBindingDescriptions = &binding;
	input.vertexAttributeDescriptionCount = 1U;
	input.pVertexAttributeDescriptions = &attribute;
	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	/* The viewport and scissor follow the swapchain, set each frame. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1U;
	viewport.scissorCount = 1U;
	memset(&raster, 0, sizeof(raster));
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;
	memset(&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	/* Over what is drawn by premultiplied alpha. */
	memset(&blend_attachment, 0, sizeof(blend_attachment));
	blend_attachment.blendEnable = VK_TRUE;
	blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
	blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
	blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	memset(&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1U;
	blend.pAttachments = &blend_attachment;
	memset(&dynamic_state, 0, sizeof(dynamic_state));
	dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic_state.dynamicStateCount = 2U;
	dynamic_state.pDynamicStates = dynamic;

	/* The pipeline. */
	memset(&pipeline, 0, sizeof(pipeline));
	pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline.stageCount = 2U;
	pipeline.pStages = stages;
	pipeline.pVertexInputState = &input;
	pipeline.pInputAssemblyState = &assembly;
	pipeline.pViewportState = &viewport;
	pipeline.pRasterizationState = &raster;
	pipeline.pMultisampleState = &multisample;
	pipeline.pColorBlendState = &blend;
	pipeline.pDynamicState = &dynamic_state;
	pipeline.layout = layout;
	pipeline.renderPass = present->pass;
	present->operation = "vkCreateGraphicsPipelines";
	error = vkCreateGraphicsPipelines(present->device, VK_NULL_HANDLE, 1U, &pipeline, NULL, made);

	/* The modules are not needed once the pipeline is made (or could not be). */
	vkDestroyShaderModule(present->device, vertex, NULL);
	vkDestroyShaderModule(present->device, fragment, NULL);

	/* A pipeline that could not be made fails the presenter. */
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the pipeline. */
	return VK_SUCCESS;
}

/* Makes a shader module from SPIR-V words. */
static VkResult
present_module(
	struct vp_present *present,
	const uint32_t *code,
	size_t size,
	VkShaderModule *module)
{
	VkShaderModuleCreateInfo create;
	VkResult error;

	/* The module over the words. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	create.codeSize = size;
	create.pCode = code;
	present->operation = "vkCreateShaderModule";
	error = vkCreateShaderModule(present->device, &create, NULL, module);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the module. */
	return VK_SUCCESS;
}

/*
 * Finds the imported image of a picture the GPU holds, importing it the
 * first time: an NV12 image over the decoder's memory (a duplicate of its
 * handle), a view of each plane, and the set that binds them.  A picture
 * of another decoding context first drops every image of the one before.
 */
static VkResult
present_import(
	struct vp_present *present,
	const struct media_gpu_picture *picture,
	struct vp_present_import **found)
{
	struct vp_present_import *entry;
	VkResult error;

	/* A new decoding context: nothing may still read the old one's images, which go. */
	*found = NULL;
	if (present->identity != picture->identity) {
		error = present_wait_all(present);
		if (error != VK_SUCCESS)
			return error;
		present_imports_free(present);
		present->identity = picture->identity;
	}

	/* The image of the picture's slot, when it is imported already; a device that imports nothing has none. */
	if (!present->imports_supported)
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	if (picture->slot >= VP_PRESENT_IMPORTS)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	entry = &present->imports[picture->slot];
	if (entry->set != VK_NULL_HANDLE) {
		*found = entry;
		return VK_SUCCESS;
	}

	/* A new import; one that fails leaves nothing of itself. */
	error = present_import_make(present, picture, entry);
	if (error != VK_SUCCESS) {
		present_import_free(present, entry);
		return error;
	}

	/* Succeeded: the image can be drawn; its first frame moves it to the shader's layout. */
	*found = entry;
	return VK_SUCCESS;
}

/* Imports one picture image: the image, its memory, its planes' views and its set. */
static VkResult
present_import_make(
	struct vp_present *present,
	const struct media_gpu_picture *picture,
	struct vp_present_import *entry)
{
	VkExternalMemoryImageCreateInfo external;
	VkImageCreateInfo image;
	VkImportMemoryFdInfoKHR import;
	VkMemoryAllocateInfo allocate;
	VkImageViewCreateInfo view;
	VkDescriptorSetAllocateInfo set;
	VkDescriptorImageInfo images[2];
	VkWriteDescriptorSet writes[2];
	unsigned index;
	int fd;
	VkResult error;

	/* The image: NV12 of the decoder's extent, its planes viewed in their own formats, its memory imported. */
	memset(&external, 0, sizeof(external));
	external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.pNext = &external;
	image.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	image.extent.width = picture->image_width;
	image.extent.height = picture->image_height;
	image.extent.depth = 1U;
	image.mipLevels = 1U;
	image.arrayLayers = 1U;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	present->operation = "vkCreateImage picture";
	error = vkCreateImage(present->device, &image, NULL, &entry->image);
	if (error != VK_SUCCESS)
		return error;

	/* A duplicate of the decoder's handle, which the import consumes. */
	fd = dup(picture->fd);
	if (fd < 0) {
		present->operation = "dup picture fd";
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;
	}

	/* The memory, imported with the decoder's size and type. */
	memset(&import, 0, sizeof(import));
	import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
	import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	import.fd = fd;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.pNext = &import;
	allocate.allocationSize = picture->memory_bytes;
	allocate.memoryTypeIndex = picture->memory_type;
	present->operation = "vkAllocateMemory import";
	error = vkAllocateMemory(present->device, &allocate, NULL, &entry->memory);
	if (error != VK_SUCCESS) {
		(void)close(fd);
		return error;
	}

	/* Binds the memory to the image. */
	present->operation = "vkBindImageMemory picture";
	error = vkBindImageMemory(present->device, entry->image, entry->memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* The luma plane as R8, then the chroma plane as RG8. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = entry->image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_R8_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	present->operation = "vkCreateImageView luma";
	error = vkCreateImageView(present->device, &view, NULL, &entry->luma);
	if (error != VK_SUCCESS)
		return error;
	view.format = VK_FORMAT_R8G8_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
	present->operation = "vkCreateImageView chroma";
	error = vkCreateImageView(present->device, &view, NULL, &entry->chroma);
	if (error != VK_SUCCESS)
		return error;

	/* The set that binds both planes, sampled smoothly. */
	memset(&set, 0, sizeof(set));
	set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	set.descriptorPool = present->descriptor_pool;
	set.descriptorSetCount = 1U;
	set.pSetLayouts = &present->picture_set_layout;
	present->operation = "vkAllocateDescriptorSets picture";
	error = vkAllocateDescriptorSets(present->device, &set, &entry->set);
	if (error != VK_SUCCESS)
		return error;
	memset(images, 0, sizeof(images));
	memset(writes, 0, sizeof(writes));
	for (index = 0U; index < 2U; index++) {
		/* Each plane's binding, in the layout the frame moves it to. */
		images[index].sampler = present->smooth;
		images[index].imageView = entry->luma;
		if (index == 1U)
			images[index].imageView = entry->chroma;
		images[index].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[index].dstSet = entry->set;
		writes[index].dstBinding = index;
		writes[index].descriptorCount = 1U;
		writes[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[index].pImageInfo = &images[index];
	}

	/* Both bindings at once. */
	vkUpdateDescriptorSets(present->device, 2U, writes, 0U, NULL);

	/* Succeeded: the image is imported; its first frame moves it to the shader's layout. */
	entry->ready = 0;
	return VK_SUCCESS;
}

/* Releases one imported picture image's objects, where made (nothing may use them any more). */
static void
present_import_free(
	struct vp_present *present,
	struct vp_present_import *entry)
{
	/* The set, the views, the image and the memory. */
	if (entry->set != VK_NULL_HANDLE)
		(void)vkFreeDescriptorSets(present->device, present->descriptor_pool, 1U, &entry->set);
	if (entry->luma != VK_NULL_HANDLE)
		vkDestroyImageView(present->device, entry->luma, NULL);
	if (entry->chroma != VK_NULL_HANDLE)
		vkDestroyImageView(present->device, entry->chroma, NULL);
	if (entry->image != VK_NULL_HANDLE)
		vkDestroyImage(present->device, entry->image, NULL);
	if (entry->memory != VK_NULL_HANDLE)
		vkFreeMemory(present->device, entry->memory, NULL);

	/* Nothing of it is left. */
	memset(entry, 0, sizeof(*entry));
}

/* Releases every imported picture image (nothing may use them any more). */
static void
present_imports_free(
	struct vp_present *present)
{
	struct vp_present_import *entry;
	unsigned index;

	/* Each slot's objects, where made. */
	for (index = 0U; index < VP_PRESENT_IMPORTS; index++) {
		entry = &present->imports[index];
		present_import_free(present, entry);
	}

	/* No context's images are imported. */
	present->identity = 0U;
}

/*
 * Records the frame: the overlay's host writes made visible, the picture's
 * image moved to the shader's layout the first time, black, the picture's
 * quad and the overlay's quad over it.
 */
static void
present_record(
	struct vp_present *present,
	uint32_t slot,
	uint32_t image,
	const struct vp_present_quad *quad,
	struct vp_present_import *picture,
	int overlay)
{
	VkCommandBufferBeginInfo begin;
	VkImageMemoryBarrier barriers[2];
	VkRenderPassBeginInfo pass;
	VkClearValue clear;
	VkViewport viewport;
	VkRect2D scissor;
	VkDeviceSize offset;
	uint32_t barrier_count;
	float push[PRESENT_PUSH_FLOATS];
	float *corners;
	float u0;
	float v0;
	float u1;
	float v1;

	/* One submission of this recording. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	(void)vkBeginCommandBuffer(present->commands[slot], &begin);

	/* The host's writes to the overlay before the shader reads it (the first frame also leaves the preinitialized layout). */
	memset(barriers, 0, sizeof(barriers));
	barrier_count = 0U;
	if (overlay) {
		barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barriers[0].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
		barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		if (present->overlay_ready == 0)
			barriers[0].oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
		barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[0].image = present->overlay;
		barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barriers[0].subresourceRange.levelCount = 1U;
		barriers[0].subresourceRange.layerCount = 1U;
		present->overlay_ready = 1;
		barrier_count = 1U;
	}

	/* A newly imported picture image moves to the shader's layout once; the decoder wrote it before its picture came. */
	if (picture != NULL && picture->ready == 0) {
		barriers[barrier_count].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barriers[barrier_count].srcAccessMask = 0U;
		barriers[barrier_count].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		barriers[barrier_count].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barriers[barrier_count].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		barriers[barrier_count].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[barrier_count].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barriers[barrier_count].image = picture->image;
		barriers[barrier_count].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barriers[barrier_count].subresourceRange.levelCount = 1U;
		barriers[barrier_count].subresourceRange.layerCount = 1U;
		picture->ready = 1;
		barrier_count++;
	}

	/* The barriers, before the fragment shader reads. */
	if (barrier_count != 0U)
		vkCmdPipelineBarrier(present->commands[slot], VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U, 0U, NULL, 0U, NULL, barrier_count, barriers);

	/* The pass, cleared to black. */
	memset(&clear, 0, sizeof(clear));
	clear.color.float32[3] = 1.0f;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = present->pass;
	pass.framebuffer = present->targets[image].framebuffer;
	pass.renderArea.extent = present->extent;
	pass.clearValueCount = 1U;
	pass.pClearValues = &clear;
	vkCmdBeginRenderPass(present->commands[slot], &pass, VK_SUBPASS_CONTENTS_INLINE);

	/* The whole image is the viewport and the scissor. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)present->extent.width;
	viewport.height = (float)present->extent.height;
	viewport.maxDepth = 1.0f;
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent = present->extent;
	vkCmdSetViewport(present->commands[slot], 0U, 1U, &viewport);
	vkCmdSetScissor(present->commands[slot], 0U, 1U, &scissor);
	offset = 0U;
	vkCmdBindVertexBuffers(present->commands[slot], 0U, 1U, &present->vertices, &offset);

	/* The picture's quad: its corners in window pixels, its window of the decoder's image, its colour rows. */
	if (picture != NULL) {
		/* The texture places of the picture's window in the image. */
		u0 = (float)quad->picture.x / (float)quad->picture.image_width;
		v0 = (float)quad->picture.y / (float)quad->picture.image_height;
		u1 = (float)(quad->picture.x + quad->picture.width) / (float)quad->picture.image_width;
		v1 = (float)(quad->picture.y + quad->picture.height) / (float)quad->picture.image_height;

		/* The quad's two triangles, after the overlay's. */
		corners = present->vertex_map + (size_t)(1U + slot) * PRESENT_VERTICES * PRESENT_VERTEX_FLOATS;
		corners[0] = (float)quad->x;
		corners[1] = (float)quad->y;
		corners[2] = u0;
		corners[3] = v0;
		corners[4] = (float)(quad->x + quad->width);
		corners[5] = (float)quad->y;
		corners[6] = u1;
		corners[7] = v0;
		corners[8] = (float)quad->x;
		corners[9] = (float)(quad->y + quad->height);
		corners[10] = u0;
		corners[11] = v1;
		memcpy(corners + 12, corners + 4, 4U * sizeof(float));
		corners[16] = (float)(quad->x + quad->width);
		corners[17] = (float)(quad->y + quad->height);
		corners[18] = u1;
		corners[19] = v1;
		memcpy(corners + 20, corners + 8, 4U * sizeof(float));

		/* The size, then the colour rows. */
		memset(push, 0, sizeof(push));
		push[0] = (float)present->extent.width;
		push[1] = (float)present->extent.height;
		present_colour(&quad->picture, push + 4);
		vkCmdBindPipeline(present->commands[slot], VK_PIPELINE_BIND_POINT_GRAPHICS, present->picture_pipeline);
		vkCmdBindDescriptorSets(present->commands[slot], VK_PIPELINE_BIND_POINT_GRAPHICS, present->picture_layout, 0U, 1U, &picture->set, 0U, NULL);
		vkCmdPushConstants(present->commands[slot], present->picture_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0U, sizeof(push), push);
		vkCmdDraw(present->commands[slot], PRESENT_VERTICES, 1U, (1U + slot) * PRESENT_VERTICES, 0U);
	}

	/* The overlay's quad over the whole image: the unit square's corners divided by one. */
	if (overlay) {
		memset(push, 0, sizeof(push));
		push[0] = 1.0f;
		push[1] = 1.0f;
		vkCmdBindPipeline(present->commands[slot], VK_PIPELINE_BIND_POINT_GRAPHICS, present->overlay_pipeline);
		vkCmdBindDescriptorSets(present->commands[slot], VK_PIPELINE_BIND_POINT_GRAPHICS, present->overlay_layout, 0U, 1U, &present->overlay_set, 0U, NULL);
		vkCmdPushConstants(present->commands[slot], present->overlay_layout, VK_SHADER_STAGE_VERTEX_BIT, 0U, 4U * sizeof(float), push);
		vkCmdDraw(present->commands[slot], PRESENT_VERTICES, 1U, 0U, 0U);
	}

	/* The pass ends with the image ready to present. */
	vkCmdEndRenderPass(present->commands[slot]);
}

/*
 * Writes the three colour rows of a picture: red, green and blue are each
 * dot((Y, Cb, Cr, 1), row), with the samples in 0..1 as the planes give
 * them.  The matrix is the picture's (BT.601, BT.709 or BT.2020), and a
 * limited range is stretched (luma 16..235, chroma 16..240 of 255).
 */
static void
present_colour(
	const struct media_gpu_picture *picture,
	float *rows)
{
	double kr;
	double kb;
	double kg;
	double luma_scale;
	double chroma_scale;
	double luma_offset;
	double chroma_offset;
	double red_cr;
	double green_cb;
	double green_cr;
	double blue_cb;

	/* The matrix's luma weights of red and blue. */
	kr = 0.299;
	kb = 0.114;
	if (picture->colour == MEDIA_COLOUR_709) {
		kr = 0.2126;
		kb = 0.0722;
	} else if (picture->colour == MEDIA_COLOUR_2020) {
		kr = 0.2627;
		kb = 0.0593;
	}

	/* Green's weight is what red and blue leave. */
	kg = 1.0 - kr - kb;

	/* The range: a limited one is stretched to 0..1 and centred. */
	luma_scale = 255.0 / 219.0;
	chroma_scale = 255.0 / 224.0;
	luma_offset = 16.0 / 255.0;
	chroma_offset = 128.0 / 255.0;
	if (picture->full_range) {
		luma_scale = 1.0;
		chroma_scale = 1.0;
		luma_offset = 0.0;
	}

	/* The chroma's contributions to each colour. */
	red_cr = 2.0 * (1.0 - kr) * chroma_scale;
	green_cb = -2.0 * kb * (1.0 - kb) / kg * chroma_scale;
	green_cr = -2.0 * kr * (1.0 - kr) / kg * chroma_scale;
	blue_cb = 2.0 * (1.0 - kb) * chroma_scale;

	/* Red: luma and Cr. */
	rows[0] = (float)luma_scale;
	rows[1] = 0.0f;
	rows[2] = (float)red_cr;
	rows[3] = (float)(-luma_scale * luma_offset - red_cr * chroma_offset);

	/* Green: luma, Cb and Cr. */
	rows[4] = (float)luma_scale;
	rows[5] = (float)green_cb;
	rows[6] = (float)green_cr;
	rows[7] = (float)(-luma_scale * luma_offset - (green_cb + green_cr) * chroma_offset);

	/* Blue: luma and Cb. */
	rows[8] = (float)luma_scale;
	rows[9] = (float)blue_cb;
	rows[10] = 0.0f;
	rows[11] = (float)(-luma_scale * luma_offset - blue_cb * chroma_offset);
}

/* Waits for the frame in a slot, if one is there, and counts it done. */
static VkResult
present_wait_slot(
	struct vp_present *present,
	uint32_t slot)
{
	VkResult error;

	/* An empty slot has nothing to wait for. */
	if (present->slot_serial[slot] == 0U)
		return VK_SUCCESS;

	/* The frame's fence. */
	present->operation = "vkWaitForFences";
	error = vkWaitForFences(present->device, 1U, &present->fences[slot], VK_TRUE, PRESENT_TIMEOUT);
	if (error != VK_SUCCESS)
		return error;

	/* It is done, and so is every frame before it. */
	if (present->slot_serial[slot] > present->completed)
		present->completed = present->slot_serial[slot];
	present->slot_serial[slot] = 0U;

	/* Succeeded: the slot is free. */
	return VK_SUCCESS;
}

/* Waits for every frame on the GPU. */
static VkResult
present_wait_all(
	struct vp_present *present)
{
	uint32_t slot;
	VkResult error;

	/* Each slot's frame. */
	for (slot = 0U; slot < VP_PRESENT_FRAMES; slot++) {
		error = present_wait_slot(present, slot);
		if (error != VK_SUCCESS)
			return error;
	}

	/* Succeeded: every frame submitted is done. */
	present->completed = present->serial;
	return VK_SUCCESS;
}

/* Reads the monotonic clock in microseconds, for the frame's cost. */
static uint64_t
present_clock_us(
	void)
{
	struct timespec now;
	int failed;

	/* The clock, or zero when it cannot be read. */
	failed = clock_gettime(CLOCK_MONOTONIC, &now);
	if (failed != 0)
		return 0U;

	/* Succeeded: microseconds. */
	return (uint64_t)now.tv_sec * 1000000U + (uint64_t)now.tv_nsec / 1000U;
}
