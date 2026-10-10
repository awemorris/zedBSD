/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The presenter of Image Viewer (derived from PDF Viewer's present.c):
 * each frame clears a swapchain image of the window to the ground (clear
 * over the glass, opaque otherwise), draws one level of the image as a
 * quad where the view places it, and lays the canvas of the viewer's words
 * and cards over it by its premultiplied alpha, with standard Vulkan and
 * Wayland WSI.
 *
 * The image's levels are linear, host-written images written once when
 * the image changes (the frame of an animated image is written into level
 * 0 as it changes).  The canvas is drawn in ordinary memory and copied row
 * by row into its image's mapping when it changed, because blending reads
 * the pixels it writes and a device mapping may be slow to read.  Each
 * frame is waited for before the next one, so the host may write the
 * images between frames without further synchronization.
 *
 * Both quads use one pipeline and one pair of shaders: the vertices are
 * window pixels divided by the size pushed (the canvas's unit square by
 * one), and their texture places.  Nothing past what the canvas already
 * used is asked of the device's shader compiler.
 */

#include "window.h"
#include "shaders.h"

#include <stdlib.h>
#include <string.h>

/* How long a frame may take on the GPU, in nanoseconds. */
#define PRESENT_TIMEOUT		10000000000ULL

/* One vertex: position and canvas place, four floats. */
#define PRESENT_VERTEX_FLOATS	4U

/* A quad's vertices: two triangles. */
#define PRESENT_VERTICES	6U

/* The quads in the vertex buffer: the canvas's, then the image's. */
#define PRESENT_QUADS		2U

/* The descriptor sets: the canvas's, and two for each level of the image. */
#define PRESENT_SETS		(1U + 2U * IV_LEVELS_MAX)

static VkResult present_device(struct iv_present *present);
static VkResult present_swapchain(struct iv_present *present, uint32_t width, uint32_t height, VkSwapchainKHR old);
static VkResult present_pass(struct iv_present *present);
static VkResult present_targets(struct iv_present *present);
static void present_targets_free(struct iv_present *present);
static VkResult present_commands(struct iv_present *present);
static VkResult present_memory(struct iv_present *present, VkMemoryRequirements *requirements, VkDeviceMemory *memory);
static VkResult present_descriptors(struct iv_present *present);
static VkResult present_canvas(struct iv_present *present);
static void present_canvas_free(struct iv_present *present);
static VkResult present_vertices(struct iv_present *present);
static VkResult present_pipeline(struct iv_present *present);
static VkResult present_module(struct iv_present *present, const uint32_t *code, size_t size, VkShaderModule *module);
static void present_record(struct iv_present *present, uint32_t image, const struct iv_quad *quad, uint32_t ground);
static VkResult present_level(struct iv_present *present, struct iv_present_level *level, const struct iv_level *source);
static void present_level_write(struct iv_present_level *level, const uint32_t *pixels);
static void present_levels_free(struct iv_present *present);
static void present_limits(struct iv_present *present);
static void present_image_vertices(struct iv_present *present, const struct iv_quad *quad);

/*
 * Makes the Vulkan objects of the window: the device, the swapchain, the
 * canvas image and the pipeline that shows it.
 */
VkResult
iv_present_open(
	struct iv_present *present,
	struct iv_window *window)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo instance;
	const char *extensions[2];
	uint32_t width;
	uint32_t height;
	VkResult error;
	int failed;

	/* Nothing is owned yet. */
	memset(present, 0, sizeof(*present));

	/* The instance, with the surface extensions a Wayland window needs. */
	extensions[0] = VK_KHR_SURFACE_EXTENSION_NAME;
	extensions[1] = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "imageview";
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&instance, 0, sizeof(instance));
	instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance.pApplicationInfo = &application;
	instance.enabledExtensionCount = 2U;
	instance.ppEnabledExtensionNames = extensions;
	present->operation = "vkCreateInstance";
	error = vkCreateInstance(&instance, NULL, &present->instance);
	if (error != VK_SUCCESS)
		return error;

	/* The window's surface, made by libkeiland over the window (WS131 p017). */
	present->operation = "kl_window_vulkan_surface";
	failed = kl_window_vulkan_surface(window->kui, present->instance, &present->surface);
	if (failed != 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A device with a queue that draws and presents to the surface. */
	error = present_device(present);
	if (error != VK_SUCCESS)
		return error;

	/* The swapchain at the window's size. */
	kl_window_size(window->kui, &width, &height);
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

	/* The sampler and the descriptor set the canvas is bound through. */
	error = present_descriptors(present);
	if (error != VK_SUCCESS)
		return error;

	/* The canvas image at the swapchain's size. */
	error = present_canvas(present);
	if (error != VK_SUCCESS)
		return error;

	/* The quad. */
	error = present_vertices(present);
	if (error != VK_SUCCESS)
		return error;

	/* The pipeline that draws both quads. */
	error = present_pipeline(present);
	if (error != VK_SUCCESS)
		return error;

	/* Whether the image's textures can be sampled smoothly, and how large one may be. */
	present_limits(present);

	/* Succeeded: frames can be shown. */
	return VK_SUCCESS;
}

/*
 * Replaces the swapchain and the canvas image with ones of a new size.
 */
VkResult
iv_present_resize(
	struct iv_present *present,
	uint32_t width,
	uint32_t height)
{
	VkSwapchainKHR old;
	VkResult error;

	/* Nothing may still use the old images. */
	present->operation = "vkDeviceWaitIdle";
	error = vkDeviceWaitIdle(present->device);
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

	/* A canvas image of the new size. */
	present_canvas_free(present);
	error = present_canvas(present);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the next frame is shown at the new size. */
	return VK_SUCCESS;
}

/*
 * Shows a frame and waits for it to finish: the ground (0xAARRGGBB, not
 * premultiplied), the image where the quad places it (when visible), and
 * the canvas (pixels of the swapchain's size, stride words a row, copied
 * when canvas_changed or when the canvas image is new).
 *
 * Returns VK_ERROR_OUT_OF_DATE_KHR when the swapchain no longer matches
 * the window; the caller resizes and draws again.
 */
VkResult
iv_present_frame(
	struct iv_present *present,
	const uint32_t *pixels,
	size_t stride,
	int canvas_changed,
	const struct iv_quad *quad,
	uint32_t ground)
{
	VkSubmitInfo submit;
	VkPresentInfoKHR info;
	VkPipelineStageFlags stage;
	uint64_t started;
	uint32_t image;
	uint32_t row;
	VkResult error;

	/* The canvas's rows into its image, when they changed (a new canvas image has none yet). */
	started = iv_clock();
	if (canvas_changed || present->canvas_ready == 0) {
		/* Each row, into the image's row as its pitch places it. */
		for (row = 0; row < present->extent.height; row++)
			memcpy(present->canvas_map + (size_t)row * present->canvas_pitch, pixels + (size_t)row * stride, (size_t)present->extent.width * 4U);
	}

	/* How long the copy took. */
	present->copy_ms = (unsigned)(iv_clock() - started);

	/* The image's quad where the view places it. */
	present_image_vertices(present, quad);

	/* The image to draw into, once the compositor has given one back. */
	started = iv_clock();
	present->operation = "vkAcquireNextImageKHR";
	error = vkAcquireNextImageKHR(present->device, present->swapchain, PRESENT_TIMEOUT, present->acquired, VK_NULL_HANDLE, &image);
	present->acquire_ms = (unsigned)(iv_clock() - started);
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return error;

	/* The frame's commands. */
	present->operation = "vkResetCommandBuffer";
	error = vkResetCommandBuffer(present->command, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Records the frame and closes the recording. */
	present_record(present, image, quad, ground);
	present->operation = "vkEndCommandBuffer";
	error = vkEndCommandBuffer(present->command);
	if (error != VK_SUCCESS)
		return error;

	/* The frame's fence starts unsignalled. */
	present->operation = "vkResetFences";
	error = vkResetFences(present->device, 1U, &present->fence);
	if (error != VK_SUCCESS)
		return error;

	/* Submits the frame after the acquire, signalling the image's present semaphore. */
	stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1U;
	submit.pWaitSemaphores = &present->acquired;
	submit.pWaitDstStageMask = &stage;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &present->command;
	submit.signalSemaphoreCount = 1U;
	submit.pSignalSemaphores = &present->targets[image].rendered;
	present->operation = "vkQueueSubmit";
	error = vkQueueSubmit(present->queue, 1U, &submit, present->fence);
	if (error != VK_SUCCESS)
		return error;

	/* Presents the image to the window. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	info.waitSemaphoreCount = 1U;
	info.pWaitSemaphores = &present->targets[image].rendered;
	info.swapchainCount = 1U;
	info.pSwapchains = &present->swapchain;
	info.pImageIndices = &image;
	started = iv_clock();
	present->operation = "vkQueuePresentKHR";
	error = vkQueuePresentKHR(present->queue, &info);
	present->present_ms = (unsigned)(iv_clock() - started);
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return error;

	/* The frame is finished before the host writes the canvas again. */
	started = iv_clock();
	present->operation = "vkWaitForFences";
	error = vkWaitForFences(present->device, 1U, &present->fence, VK_TRUE, PRESENT_TIMEOUT);
	present->wait_ms = (unsigned)(iv_clock() - started);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the frame is in the window. */
	return VK_SUCCESS;
}

/*
 * Releases every Vulkan object, children before their parents.
 */
void
iv_present_close(
	struct iv_present *present)
{
	/* The device's objects, once nothing runs. */
	if (present->device != VK_NULL_HANDLE) {
		(void)vkDeviceWaitIdle(present->device);
		present_targets_free(present);
		present_canvas_free(present);
		present_levels_free(present);

		/* The drawing objects. */
		if (present->pipeline != VK_NULL_HANDLE)
			vkDestroyPipeline(present->device, present->pipeline, NULL);
		if (present->layout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(present->device, present->layout, NULL);
		if (present->descriptor_pool != VK_NULL_HANDLE)
			vkDestroyDescriptorPool(present->device, present->descriptor_pool, NULL);
		if (present->set_layout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(present->device, present->set_layout, NULL);
		if (present->sampler != VK_NULL_HANDLE)
			vkDestroySampler(present->device, present->sampler, NULL);
		if (present->smooth_sampler != VK_NULL_HANDLE)
			vkDestroySampler(present->device, present->smooth_sampler, NULL);

		/* The quad's vertices, with their memory. */
		if (present->vertices != VK_NULL_HANDLE)
			vkDestroyBuffer(present->device, present->vertices, NULL);
		if (present->vertex_memory != VK_NULL_HANDLE)
			vkFreeMemory(present->device, present->vertex_memory, NULL);

		/* The commands and the synchronization. */
		if (present->pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(present->device, present->pool, NULL);
		if (present->fence != VK_NULL_HANDLE)
			vkDestroyFence(present->device, present->fence, NULL);
		if (present->acquired != VK_NULL_HANDLE)
			vkDestroySemaphore(present->device, present->acquired, NULL);

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
 * Makes the textures of an image's levels (an animated image: level 0,
 * whose frames are written with iv_present_set_frame()), replacing those
 * of the image before.  serial names the image, so that the same one is
 * not written again.  A NULL image (or one that cannot be shown) leaves
 * no texture.
 */
VkResult
iv_present_set_image(
	struct iv_present *present,
	const struct iv_image *image,
	unsigned serial)
{
	size_t index;
	VkResult error;

	/* The same image is already there. */
	if (present->image_serial == serial &&
	    (present->level_count != 0U || present->cpu_image))
		return VK_SUCCESS;

	/* Nothing may still read the old textures. */
	present->operation = "vkDeviceWaitIdle";
	error = vkDeviceWaitIdle(present->device);
	if (error != VK_SUCCESS)
		return error;

	/* The old textures go; the new image is the one the textures are of. */
	present_levels_free(present);
	present->image_serial = serial;

	/* Nothing to show. */
	if (image == NULL ||
	    image->error != 0 ||
	    image->level_count == 0U)
		return VK_SUCCESS;

	/* A texture for each level. */
	for (index = 0; index < image->level_count; index++) {
		/* The level's texture; a failure leaves no texture at all. */
		error = present_level(present, &present->levels[index], &image->levels[index]);
		if (error != VK_SUCCESS) {
			present_levels_free(present);

			/* Resource limits must not prevent viewing a correctly decoded original. */
			if (error == VK_ERROR_OUT_OF_DEVICE_MEMORY ||
			    error == VK_ERROR_OUT_OF_HOST_MEMORY) {
				present->cpu_image = 1;
				iv_log(
				    "IMAGE sampling=cpu reason=texture-memory");
				return VK_SUCCESS;
			}

			/* Device loss and incompatible formats retain their original failure. */
			return error;
		}

		/* One more level made. */
		present->level_count = index + 1U;
	}

	/* Every level has its texture. */
	present->has_image = 1;

	/* Succeeded: the image can be drawn. */
	return VK_SUCCESS;
}

/*
 * Writes a frame of an animated image into level 0's texture (the frame
 * shown last has finished, so the host may write it).
 */
void
iv_present_set_frame(
	struct iv_present *present,
	const uint32_t *pixels)
{
	/* Only an image with its level 0. */
	if (!present->has_image || present->level_count == 0U)
		return;

	/* The frame's rows. */
	present_level_write(&present->levels[0], pixels);
}

/* Chooses a physical device and a queue family that draws and presents to the surface, and makes the device. */
static VkResult
present_device(
	struct iv_present *present)
{
	VkPhysicalDevice devices[8];
	VkQueueFamilyProperties families[16];
	VkDeviceQueueCreateInfo queue;
	VkDeviceCreateInfo create;
	const char *extension;
	float priority;
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

	/* One queue of that family and the swapchain extension. */
	priority = 1.0f;
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = present->family;
	queue.queueCount = 1U;
	queue.pQueuePriorities = &priority;
	extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	create.queueCreateInfoCount = 1U;
	create.pQueueCreateInfos = &queue;
	create.enabledExtensionCount = 1U;
	create.ppEnabledExtensionNames = &extension;
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
	struct iv_present *present,
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

	/*
	 * See-through when the surface takes premultiplied alpha (the compositor that
	 * can draw glass under the window), else opaque.
	 */
	present->premultiplied = 0;
	if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) != 0U)
		present->premultiplied = 1;

	/* Three images when the surface allows, replacing the old chain. */
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
	if (present->premultiplied != 0)
		create.compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
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

/* Makes the pass: one color attachment, cleared to the ground, drawn over and left for presenting. */
static VkResult
present_pass(
	struct iv_present *present)
{
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkRenderPassCreateInfo create;
	VkResult error;

	/* The swapchain image, cleared to the ground each frame. */
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
	struct iv_present *present)
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
		present->targets[index].image = images[index];
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
	struct iv_present *present)
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
	struct iv_present *present)
{
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo command;
	VkFenceCreateInfo fence;
	VkSemaphoreCreateInfo semaphore;
	VkResult error;

	/* A pool whose one buffer is reset every frame. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = present->family;
	present->operation = "vkCreateCommandPool";
	error = vkCreateCommandPool(present->device, &pool, NULL, &present->pool);
	if (error != VK_SUCCESS)
		return error;

	/* The buffer. */
	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = present->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = 1U;
	present->operation = "vkAllocateCommandBuffers";
	error = vkAllocateCommandBuffers(present->device, &command, &present->command);
	if (error != VK_SUCCESS)
		return error;

	/* The fence the frame's end signals. */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	present->operation = "vkCreateFence";
	error = vkCreateFence(present->device, &fence, NULL, &present->fence);
	if (error != VK_SUCCESS)
		return error;

	/* The semaphore the acquire signals. */
	memset(&semaphore, 0, sizeof(semaphore));
	semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	present->operation = "vkCreateSemaphore";
	error = vkCreateSemaphore(present->device, &semaphore, NULL, &present->acquired);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: one frame at a time can be recorded and waited for. */
	return VK_SUCCESS;
}

/* Allocates host-visible, coherent memory that meets the requirements. */
static VkResult
present_memory(
	struct iv_present *present,
	VkMemoryRequirements *requirements,
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

	/* Without one the host cannot write the canvas. */
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

/* Makes the sampler, the set layout, the pool and the one set the canvas is bound through. */
static VkResult
present_descriptors(
	struct iv_present *present)
{
	VkSamplerCreateInfo sampler;
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo set_layout;
	VkDescriptorPoolSize pool_size;
	VkDescriptorPoolCreateInfo pool;
	VkDescriptorSetAllocateInfo allocate;
	VkResult error;

	/* Nearest sampling: the canvas is the window's size, so each texel maps to one pixel (and a much enlarged image shows its pixels sharp). */
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_NEAREST;
	sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	present->operation = "vkCreateSampler";
	error = vkCreateSampler(present->device, &sampler, NULL, &present->sampler);
	if (error != VK_SUCCESS)
		return error;

	/* Smooth (bilinear) sampling for the image shown at any other scale. */
	sampler.magFilter = VK_FILTER_LINEAR;
	sampler.minFilter = VK_FILTER_LINEAR;
	present->operation = "vkCreateSampler";
	error = vkCreateSampler(present->device, &sampler, NULL, &present->smooth_sampler);
	if (error != VK_SUCCESS)
		return error;

	/* One combined image sampler for the fragment shader. */
	memset(&binding, 0, sizeof(binding));
	binding.binding = 0U;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1U;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	memset(&set_layout, 0, sizeof(set_layout));
	set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout.bindingCount = 1U;
	set_layout.pBindings = &binding;
	present->operation = "vkCreateDescriptorSetLayout";
	error = vkCreateDescriptorSetLayout(present->device, &set_layout, NULL, &present->set_layout);
	if (error != VK_SUCCESS)
		return error;

	/* A pool for the canvas's set and the levels' (which are freed as the image changes). */
	memset(&pool_size, 0, sizeof(pool_size));
	pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_size.descriptorCount = PRESENT_SETS;
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

	/* The set, pointed at the canvas once it exists. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = present->descriptor_pool;
	allocate.descriptorSetCount = 1U;
	allocate.pSetLayouts = &present->set_layout;
	present->operation = "vkAllocateDescriptorSets";
	error = vkAllocateDescriptorSets(present->device, &allocate, &present->set);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the canvas can be bound. */
	return VK_SUCCESS;
}

/* Makes the canvas image at the swapchain's size (linear, host-written, mapped for good) and binds it to the set. */
static VkResult
present_canvas(
	struct iv_present *present)
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
	present->operation = "vkCreateImage";
	error = vkCreateImage(present->device, &image, NULL, &present->canvas);
	if (error != VK_SUCCESS)
		return error;

	/* Its memory. */
	vkGetImageMemoryRequirements(present->device, present->canvas, &requirements);
	error = present_memory(present, &requirements, &present->canvas_memory);
	if (error != VK_SUCCESS)
		return error;

	/* Binds the memory to the image. */
	present->operation = "vkBindImageMemory";
	error = vkBindImageMemory(present->device, present->canvas, present->canvas_memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Maps it for the host's writes, for good. */
	present->operation = "vkMapMemory";
	error = vkMapMemory(present->device, present->canvas_memory, 0U, VK_WHOLE_SIZE, 0U, &map);
	if (error != VK_SUCCESS)
		return error;

	/* Where the rows start and how far apart they are. */
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vkGetImageSubresourceLayout(present->device, present->canvas, &subresource, &layout);
	present->canvas_map = (unsigned char *)map + layout.offset;
	present->canvas_pitch = (size_t)layout.rowPitch;
	present->canvas_ready = 0;

	/* The view the shader samples. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = present->canvas;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_B8G8R8A8_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	present->operation = "vkCreateImageView";
	error = vkCreateImageView(present->device, &view, NULL, &present->canvas_view);
	if (error != VK_SUCCESS)
		return error;

	/* The set names the canvas in the general layout it is kept in. */
	memset(&image_info, 0, sizeof(image_info));
	image_info.sampler = present->sampler;
	image_info.imageView = present->canvas_view;
	image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = present->set;
	write.dstBinding = 0U;
	write.descriptorCount = 1U;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image_info;
	vkUpdateDescriptorSets(present->device, 1U, &write, 0U, NULL);

	/* Succeeded: frames can be written into the canvas. */
	return VK_SUCCESS;
}

/* Releases the canvas image, its view and its memory. */
static void
present_canvas_free(
	struct iv_present *present)
{
	/* The view, the image and the memory, where made. */
	if (present->canvas_view != VK_NULL_HANDLE)
		vkDestroyImageView(present->device, present->canvas_view, NULL);
	if (present->canvas != VK_NULL_HANDLE)
		vkDestroyImage(present->device, present->canvas, NULL);
	if (present->canvas_memory != VK_NULL_HANDLE)
		vkFreeMemory(present->device, present->canvas_memory, NULL);

	/* Nothing of the canvas is left. */
	present->canvas_view = VK_NULL_HANDLE;
	present->canvas = VK_NULL_HANDLE;
	present->canvas_memory = VK_NULL_HANDLE;
	present->canvas_map = NULL;
	present->canvas_ready = 0;
}

/*
 * Makes the vertex buffer of the two quads, mapped for good: the canvas's
 * unit square (which a pushed size of one stretches over the window), and
 * room for the image's, written each frame.
 */
static VkResult
present_vertices(
	struct iv_present *present)
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

	/* Maps the buffer for good, and writes the canvas's corners once. */
	present->operation = "vkMapMemory";
	error = vkMapMemory(present->device, present->vertex_memory, 0U, VK_WHOLE_SIZE, 0U, &map);
	if (error != VK_SUCCESS)
		return error;

	/* The canvas's corners, which never change. */
	present->vertex_map = map;
	memcpy(present->vertex_map, quad, sizeof(quad));

	/* Succeeded: the quads can be drawn. */
	return VK_SUCCESS;
}

/* Makes the pipeline that draws the canvas: one vec4 attribute, the canvas, the scale as a push constant. */
static VkResult
present_pipeline(
	struct iv_present *present)
{
	static const VkDynamicState dynamic[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};
	VkShaderModule vertex;
	VkShaderModule fragment;
	VkPushConstantRange push;
	VkPipelineLayoutCreateInfo layout;
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
	error = present_module(present, imageview_canvas_vert, sizeof(imageview_canvas_vert), &vertex);
	if (error != VK_SUCCESS)
		return error;

	/* The fragment module; the vertex module goes when it cannot be made. */
	error = present_module(present, imageview_canvas_frag, sizeof(imageview_canvas_frag), &fragment);
	if (error != VK_SUCCESS) {
		vkDestroyShaderModule(present->device, vertex, NULL);
		return error;
	}

	/* The layout: the canvas's set and the scale. */
	memset(&push, 0, sizeof(push));
	push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	push.offset = 0U;
	push.size = 4U * sizeof(float);
	memset(&layout, 0, sizeof(layout));
	layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout.setLayoutCount = 1U;
	layout.pSetLayouts = &present->set_layout;
	layout.pushConstantRangeCount = 1U;
	layout.pPushConstantRanges = &push;
	present->operation = "vkCreatePipelineLayout";
	error = vkCreatePipelineLayout(present->device, &layout, NULL, &present->layout);
	if (error != VK_SUCCESS) {
		vkDestroyShaderModule(present->device, vertex, NULL);
		vkDestroyShaderModule(present->device, fragment, NULL);
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

	/* Each quad over what is drawn by its premultiplied alpha (the image's texels are opaque). */
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
	pipeline.layout = present->layout;
	pipeline.renderPass = present->pass;
	present->operation = "vkCreateGraphicsPipelines";
	error = vkCreateGraphicsPipelines(present->device, VK_NULL_HANDLE, 1U, &pipeline, NULL, &present->pipeline);

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
	struct iv_present *present,
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
 * Records the frame: the textures' host writes made visible to the shader,
 * the ground cleared, the image's quad (when visible) and the canvas's
 * quad over it.
 */
static void
present_record(
	struct iv_present *present,
	uint32_t image,
	const struct iv_quad *quad,
	uint32_t ground)
{
	struct iv_present_level *level;
	VkCommandBufferBeginInfo begin;
	VkImageMemoryBarrier barriers[2];
	VkRenderPassBeginInfo pass;
	VkClearValue clear;
	VkViewport viewport;
	VkRect2D scissor;
	VkRect2D clip;
	VkDeviceSize offset;
	VkDescriptorSet set;
	uint32_t barrier_count;
	float alpha;
	float size[4];

	/* One submission of this recording. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	(void)vkBeginCommandBuffer(present->command, &begin);

	/* The host's writes to the canvas before the shader reads it (the first frame also leaves the preinitialized layout). */
	memset(barriers, 0, sizeof(barriers));
	barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[0].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	if (present->canvas_ready == 0)
		barriers[0].oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
	barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].image = present->canvas;
	barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[0].subresourceRange.levelCount = 1U;
	barriers[0].subresourceRange.layerCount = 1U;
	present->canvas_ready = 1;
	barrier_count = 1U;

	/* And to the level drawn, likewise. */
	level = NULL;
	if (quad != NULL &&
	    quad->visible &&
	    present->has_image &&
	    quad->level < present->level_count) {
		/* The level's barrier is the canvas's for another image; a new level leaves its preinitialized layout. */
		level = &present->levels[quad->level];
		barriers[1] = barriers[0];
		barriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
		if (level->ready == 0)
			barriers[1].oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
		barriers[1].image = level->image;
		level->ready = 1;
		barrier_count = 2U;
	}

	/* The barriers, before the fragment shader reads. */
	vkCmdPipelineBarrier(present->command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U, 0U, NULL, 0U, NULL, barrier_count, barriers);

	/* The pass, cleared to the ground (premultiplied; a clear value is red, green, blue and alpha whatever the format's order). */
	alpha = (float)(ground >> 24) / 255.0f;
	memset(&clear, 0, sizeof(clear));
	clear.color.float32[0] = (float)((ground >> 16) & 0xffU) / 255.0f * alpha;
	clear.color.float32[1] = (float)((ground >> 8) & 0xffU) / 255.0f * alpha;
	clear.color.float32[2] = (float)(ground & 0xffU) / 255.0f * alpha;
	clear.color.float32[3] = alpha;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = present->pass;
	pass.framebuffer = present->targets[image].framebuffer;
	pass.renderArea.extent = present->extent;
	pass.clearValueCount = 1U;
	pass.pClearValues = &clear;
	vkCmdBeginRenderPass(present->command, &pass, VK_SUBPASS_CONTENTS_INLINE);

	/* The whole image is the viewport. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)present->extent.width;
	viewport.height = (float)present->extent.height;
	viewport.maxDepth = 1.0f;
	memset(&scissor, 0, sizeof(scissor));
	scissor.extent = present->extent;
	vkCmdSetViewport(present->command, 0U, 1U, &viewport);
	vkCmdSetScissor(present->command, 0U, 1U, &scissor);

	/* The pipeline and the vertices of both quads. */
	offset = 0U;
	vkCmdBindPipeline(present->command, VK_PIPELINE_BIND_POINT_GRAPHICS, present->pipeline);
	vkCmdBindVertexBuffers(present->command, 0U, 1U, &present->vertices, &offset);

	/* The image's quad, within its clip: window pixels divided by the window's size, the level smooth or to the nearest texel. */
	if (level != NULL) {
		/* The clip, kept inside the swapchain image. */
		clip.offset.x = quad->clip_x;
		clip.offset.y = quad->clip_y;
		clip.extent.width = (uint32_t)quad->clip_width;
		clip.extent.height = (uint32_t)quad->clip_height;
		if (clip.offset.x < 0)
			clip.offset.x = 0;
		if (clip.offset.y < 0)
			clip.offset.y = 0;
		if ((uint32_t)clip.offset.x + clip.extent.width > present->extent.width)
			clip.extent.width = present->extent.width - (uint32_t)clip.offset.x;
		if ((uint32_t)clip.offset.y + clip.extent.height > present->extent.height)
			clip.extent.height = present->extent.height - (uint32_t)clip.offset.y;
		vkCmdSetScissor(present->command, 0U, 1U, &clip);

		/* The level smooth, or to the nearest texel when much enlarged or when the device cannot filter it. */
		set = level->smooth_set;
		if (quad->nearest || !present->smooth)
			set = level->nearest_set;

		/* The quad in window pixels, divided by the window's size. */
		size[0] = (float)present->extent.width;
		size[1] = (float)present->extent.height;
		size[2] = 0.0f;
		size[3] = 0.0f;
		vkCmdBindDescriptorSets(present->command, VK_PIPELINE_BIND_POINT_GRAPHICS, present->layout, 0U, 1U, &set, 0U, NULL);
		vkCmdPushConstants(present->command, present->layout, VK_SHADER_STAGE_VERTEX_BIT, 0U, sizeof(size), size);
		vkCmdDraw(present->command, PRESENT_VERTICES, 1U, PRESENT_VERTICES, 0U);
	}

	/* The canvas's quad over the whole image: the unit square's corners divided by one, so it spans the viewport. */
	vkCmdSetScissor(present->command, 0U, 1U, &scissor);
	size[0] = 1.0f;
	size[1] = 1.0f;
	size[2] = 0.0f;
	size[3] = 0.0f;
	vkCmdBindDescriptorSets(present->command, VK_PIPELINE_BIND_POINT_GRAPHICS, present->layout, 0U, 1U, &present->set, 0U, NULL);
	vkCmdPushConstants(present->command, present->layout, VK_SHADER_STAGE_VERTEX_BIT, 0U, sizeof(size), size);
	vkCmdDraw(present->command, PRESENT_VERTICES, 1U, 0U, 0U);

	/* The pass ends with the image ready to present. */
	vkCmdEndRenderPass(present->command);
}

/*
 * Makes one level's texture: a linear image of its size, host-visible
 * memory mapped for good, a view, the two sets that bind it, and its
 * pixels written.
 */
static VkResult
present_level(
	struct iv_present *present,
	struct iv_present_level *level,
	const struct iv_level *source)
{
	VkImageCreateInfo image;
	VkMemoryRequirements requirements;
	VkImageSubresource subresource;
	VkSubresourceLayout layout;
	VkImageViewCreateInfo view;
	VkDescriptorSetAllocateInfo allocate;
	VkDescriptorSetLayout layouts[2];
	VkDescriptorSet sets[2];
	VkDescriptorImageInfo infos[2];
	VkWriteDescriptorSet writes[2];
	void *map;
	VkResult error;

	/* Nothing made yet. */
	memset(level, 0, sizeof(*level));
	level->width = source->width;
	level->height = source->height;

	/* The image: linear so that the host writes its rows, sampled by the fragment shader. */
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = VK_FORMAT_B8G8R8A8_UNORM;
	image.extent.width = (uint32_t)source->width;
	image.extent.height = (uint32_t)source->height;
	image.extent.depth = 1U;
	image.mipLevels = 1U;
	image.arrayLayers = 1U;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_LINEAR;
	image.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
	present->operation = "vkCreateImage";
	error = vkCreateImage(present->device, &image, NULL, &level->image);
	if (error != VK_SUCCESS)
		return error;

	/* Its memory. */
	vkGetImageMemoryRequirements(present->device, level->image, &requirements);
	error = present_memory(present, &requirements, &level->memory);
	if (error != VK_SUCCESS)
		return error;

	/* Binds the memory to the image. */
	present->operation = "vkBindImageMemory";
	error = vkBindImageMemory(present->device, level->image, level->memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Maps it for the host's writes, for good. */
	present->operation = "vkMapMemory";
	error = vkMapMemory(present->device, level->memory, 0U, VK_WHOLE_SIZE, 0U, &map);
	if (error != VK_SUCCESS)
		return error;

	/* Where the rows start and how far apart they are. */
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vkGetImageSubresourceLayout(present->device, level->image, &subresource, &layout);
	level->map = (unsigned char *)map + layout.offset;
	level->pitch = (size_t)layout.rowPitch;

	/* The view the shader samples. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = level->image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_B8G8R8A8_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	present->operation = "vkCreateImageView";
	error = vkCreateImageView(present->device, &view, NULL, &level->view);
	if (error != VK_SUCCESS)
		return error;

	/* Two sets: smooth and to the nearest texel. */
	layouts[0] = present->set_layout;
	layouts[1] = present->set_layout;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = present->descriptor_pool;
	allocate.descriptorSetCount = 2U;
	allocate.pSetLayouts = layouts;
	present->operation = "vkAllocateDescriptorSets";
	error = vkAllocateDescriptorSets(present->device, &allocate, sets);
	if (error != VK_SUCCESS)
		return error;

	/* The level keeps both sets. */
	level->smooth_set = sets[0];
	level->nearest_set = sets[1];

	/* The sets name the level in the general layout it is kept in, each with its sampler. */
	memset(infos, 0, sizeof(infos));
	infos[0].sampler = present->smooth_sampler;
	infos[0].imageView = level->view;
	infos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	infos[1] = infos[0];
	infos[1].sampler = present->sampler;
	memset(writes, 0, sizeof(writes));
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = level->smooth_set;
	writes[0].dstBinding = 0U;
	writes[0].descriptorCount = 1U;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[0].pImageInfo = &infos[0];
	writes[1] = writes[0];
	writes[1].dstSet = level->nearest_set;
	writes[1].pImageInfo = &infos[1];
	vkUpdateDescriptorSets(present->device, 2U, writes, 0U, NULL);

	/* The level's pixels. */
	present_level_write(level, source->pixels);

	/* Succeeded: the level can be drawn. */
	return VK_SUCCESS;
}

/* Writes a level's pixels (its width words a row) into its texture's rows. */
static void
present_level_write(
	struct iv_present_level *level,
	const uint32_t *pixels)
{
	int row;

	/* Each row. */
	for (row = 0; row < level->height; row++)
		memcpy(level->map + (size_t)row * level->pitch, pixels + (size_t)row * (size_t)level->width, (size_t)level->width * 4U);
}

/* Releases the levels' textures, their sets and their memory. */
static void
present_levels_free(
	struct iv_present *present)
{
	struct iv_present_level *level;
	VkDescriptorSet sets[2];
	size_t index;

	/* Each level made, what of it was made. */
	for (index = 0; index < IV_LEVELS_MAX; index++) {
		level = &present->levels[index];

		/* Its sets go back to the pool. */
		if (level->smooth_set != VK_NULL_HANDLE) {
			sets[0] = level->smooth_set;
			sets[1] = level->nearest_set;
			(void)vkFreeDescriptorSets(present->device, present->descriptor_pool, 2U, sets);
		}

		/* The view, the image and the memory. */
		if (level->view != VK_NULL_HANDLE)
			vkDestroyImageView(present->device, level->view, NULL);
		if (level->image != VK_NULL_HANDLE)
			vkDestroyImage(present->device, level->image, NULL);
		if (level->memory != VK_NULL_HANDLE)
			vkFreeMemory(present->device, level->memory, NULL);

		/* Nothing of the level is left. */
		memset(level, 0, sizeof(*level));
	}

	/* No image is drawn. */
	present->level_count = 0;
	present->cpu_image = 0;
	present->has_image = 0;
}

/*
 * Finds whether a linear texture can be sampled smoothly (otherwise the
 * image is always sampled to the nearest texel) and the largest side one
 * may have (the viewer halves larger images).
 */
static void
present_limits(
	struct iv_present *present)
{
	VkFormatProperties format;
	VkImageFormatProperties limits;
	VkResult error;

	/* Smooth sampling of a linear image of the format. */
	vkGetPhysicalDeviceFormatProperties(present->physical, VK_FORMAT_B8G8R8A8_UNORM, &format);
	present->smooth = 0;
	if ((format.linearTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0U)
		present->smooth = 1;

	/* The largest linear, sampled image. */
	present->max_dimension = 4096;
	error = vkGetPhysicalDeviceImageFormatProperties(present->physical,
							 VK_FORMAT_B8G8R8A8_UNORM,
							 VK_IMAGE_TYPE_2D,
							 VK_IMAGE_TILING_LINEAR,
							 VK_IMAGE_USAGE_SAMPLED_BIT,
							 0U,
							 &limits);
	if (error == VK_SUCCESS) {
		/* The shorter of the two sides the device allows. */
		present->max_dimension = (int)limits.maxExtent.width;
		if ((int)limits.maxExtent.height < present->max_dimension)
			present->max_dimension = (int)limits.maxExtent.height;
	}

	/* The log line of what the device allows. */
	iv_log("PRESENT smooth=%d max-dimension=%d", present->smooth, present->max_dimension);
}

/* Writes the image's quad into the vertex buffer: its corners in window pixels and their places in the texture. */
static void
present_image_vertices(
	struct iv_present *present,
	const struct iv_quad *quad)
{
	/* The corners' order as the canvas's quad has them: top left, top right, bottom left; top right, bottom right, bottom left. */
	static const unsigned corners[PRESENT_VERTICES] = { 0U, 1U, 2U, 1U, 3U, 2U };
	static const float places[4][2] = {
		{ 0.0f, 0.0f },
		{ 1.0f, 0.0f },
		{ 0.0f, 1.0f },
		{ 1.0f, 1.0f }
	};
	float *vertex;
	unsigned index;
	unsigned corner;

	/* Nothing drawn, nothing written. */
	if (quad == NULL ||
	    !quad->visible ||
	    present->vertex_map == NULL)
		return;

	/* Each of the six vertices after the canvas's. */
	for (index = 0; index < PRESENT_VERTICES; index++) {
		/* The corner the vertex is, and its place in the vertex buffer. */
		corner = corners[index];
		vertex = present->vertex_map + (PRESENT_VERTICES + index) * PRESENT_VERTEX_FLOATS;
		vertex[0] = quad->x[corner];
		vertex[1] = quad->y[corner];
		vertex[2] = places[corner][0];
		vertex[3] = places[corner][1];
	}
}
