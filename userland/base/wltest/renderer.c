/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Renders independently predictable images through standard Vulkan and Wayland WSI.
 */

#include "wltest.h"

#include <stdlib.h>
#include <string.h>

#define WLTEST_GPU_TIMEOUT 10000000000ULL

static VkResult renderer_device(struct wltest_renderer *renderer);
static VkResult renderer_swapchain(struct wltest_renderer *renderer, VkSwapchainKHR old);
static VkResult renderer_pass(struct wltest_renderer *renderer);
static VkResult renderer_commands(struct wltest_renderer *renderer);
static VkResult renderer_targets(struct wltest_renderer *renderer);
static void renderer_targets_free(struct wltest_renderer *renderer);
static void renderer_rect(VkCommandBuffer command, uint32_t x, uint32_t y, uint32_t width, uint32_t height, float red, float green, float blue);

/*
 * Creates native-independent Vulkan resources after the initial Wayland configure.
 */
VkResult
wltest_renderer_open(
	struct wltest_renderer *renderer,
	struct wltest_window *window,
	VkPresentModeKHR mode)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo instance;
	VkWaylandSurfaceCreateInfoKHR surface;
	const char *extensions[2];
	VkResult error;

	/* Initializes renderer ownership and the configured native image extent. */
	memset(renderer, 0, sizeof(*renderer));
	renderer->extent.width = window->width;
	renderer->extent.height = window->height;
	renderer->mode = mode;

	/* The application requests only standard instance extensions. */
	extensions[0] = VK_KHR_SURFACE_EXTENSION_NAME;
	extensions[1] = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;

	/* Declares the application and its standard Vulkan version. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "wltest";
	application.apiVersion = VK_API_VERSION_1_0;

	/* Requests the standard surface and Wayland instance extensions. */
	memset(&instance, 0, sizeof(instance));
	instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance.pApplicationInfo = &application;
	instance.enabledExtensionCount = 2U;
	instance.ppEnabledExtensionNames = extensions;
	renderer->operation = "vkCreateInstance";
	error = vkCreateInstance(&instance, NULL, &renderer->instance);
	if (error != VK_SUCCESS)
		return error;

	/* This is the only native-window description supplied to Vulkan. */
	memset(&surface, 0, sizeof(surface));
	surface.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
	surface.display = window->display;
	surface.surface = window->surface;
	renderer->operation = "vkCreateWaylandSurfaceKHR";
	error = vkCreateWaylandSurfaceKHR(renderer->instance, &surface, NULL, &renderer->surface);
	if (error != VK_SUCCESS)
		return error;

	/* Selects a graphics queue with presentation support for this surface. */
	error = renderer_device(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Creates the first chain using the native window configuration. */
	error = renderer_swapchain(renderer, VK_NULL_HANDLE);
	if (error != VK_SUCCESS)
		return error;

	/* Creates the color-only pass used by the deterministic GPU pattern. */
	error = renderer_pass(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Builds application attachments for every borrowed swapchain image. */
	error = renderer_targets(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Creates reusable command and synchronization owners. */
	error = renderer_commands(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the renderer owns commands, attachments and its presentation chain. */
	return VK_SUCCESS;
}

/*
 * Draws one moving pattern without any CPU readback or private GPU operation.
 */
VkResult
wltest_renderer_draw(
	struct wltest_renderer *renderer,
	uint32_t frame)
{
	VkCommandBufferBeginInfo begin;
	VkRenderPassBeginInfo pass;
	VkClearValue clear;
	VkSubmitInfo submit;
	VkPresentInfoKHR present;
	VkPipelineStageFlags stage;
	uint32_t image;
	uint32_t x;
	uint32_t width;
	uint32_t height;
	VkResult error;

	/* The acquire primitive is signaled only after the compositor releases an image. */
	renderer->operation = "vkAcquireNextImageKHR";
	error = vkAcquireNextImageKHR(renderer->device, renderer->swapchain, WLTEST_GPU_TIMEOUT, renderer->acquired, VK_NULL_HANDLE, &image);
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return error;

	/* Retires the prior recording before describing the next acquired image. */
	renderer->operation = "vkResetCommandBuffer";
	error = vkResetCommandBuffer(renderer->command, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Records a single-use command stream after the previous fence completed. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	renderer->operation = "vkBeginCommandBuffer";
	error = vkBeginCommandBuffer(renderer->command, &begin);
	if (error != VK_SUCCESS)
		return error;

	/* Exact zero/one color channels make the independent screenshot oracle unambiguous. */
	memset(&clear, 0, sizeof(clear));
	clear.color.float32[3] = 1.0f;

	/* Binds the acquired image attachment over its full render area. */
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = renderer->pass;
	pass.framebuffer = renderer->targets[image].framebuffer;
	pass.renderArea.extent = renderer->extent;
	pass.clearValueCount = 1U;
	pass.pClearValues = &clear;
	vkCmdBeginRenderPass(renderer->command, &pass, VK_SUBPASS_CONTENTS_INLINE);

	/*
	 * Three fixed color regions and one moving bar define the independent
	 * image oracle; a window test fills the whole image with one color the
	 * screen reader looks for instead.
	 */
	width = renderer->extent.width;
	height = renderer->extent.height;
	if (renderer->solid_set) {
		renderer_rect(renderer->command, 0U, 0U, width, height, renderer->solid[0], renderer->solid[1], renderer->solid[2]);
	} else {
		renderer_rect(renderer->command, 0U, 0U, width / 2U, height / 2U, 1.0f, 0.0f, 0.0f);
		renderer_rect(renderer->command, width / 2U, 0U, width - width / 2U, height / 2U, 0.0f, 1.0f, 0.0f);
		renderer_rect(renderer->command, 0U, height / 2U, width, height - height / 2U, 0.0f, 0.0f, 1.0f);
		x = ((frame - 1U) * 29U) % (width - 40U);
		renderer_rect(renderer->command, x, height / 3U, 40U, height / 3U, 1.0f, 1.0f, 1.0f);
	}
	vkCmdEndRenderPass(renderer->command);

	/* Finishes the render stream before it can be submitted. */
	renderer->operation = "vkEndCommandBuffer";
	error = vkEndCommandBuffer(renderer->command);
	if (error != VK_SUCCESS)
		return error;

	/* One explicit submission orders acquire, rendering, and the present semaphore. */
	renderer->operation = "vkResetFences";
	error = vkResetFences(renderer->device, 1U, &renderer->fence);
	if (error != VK_SUCCESS)
		return error;

	/* Acquire must finish before the color attachment is first written. */
	stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

	/* Orders acquired-image access and signals this image's present semaphore. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1U;
	submit.pWaitSemaphores = &renderer->acquired;
	submit.pWaitDstStageMask = &stage;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &renderer->command;
	submit.signalSemaphoreCount = 1U;
	submit.pSignalSemaphores = &renderer->targets[image].rendered;
	renderer->operation = "vkQueueSubmit";
	error = vkQueueSubmit(renderer->queue, 1U, &submit, renderer->fence);
	if (error != VK_SUCCESS)
		return error;

	/* Transfers the completed acquired image to the selected surface. */
	memset(&present, 0, sizeof(present));
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1U;
	present.pWaitSemaphores = &renderer->targets[image].rendered;
	present.swapchainCount = 1U;
	present.pSwapchains = &renderer->swapchain;
	present.pImageIndices = &image;
	renderer->operation = "vkQueuePresentKHR";
	error = vkQueuePresentKHR(renderer->queue, &present);
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return error;

	/* Command-buffer reuse is ordered by the actual rendering fence, not socket progress. */
	renderer->operation = "vkWaitForFences";
	error = vkWaitForFences(renderer->device, 1U, &renderer->fence, VK_TRUE, WLTEST_GPU_TIMEOUT);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the submitted render has completed before command reuse. */
	return VK_SUCCESS;
}

/*
 * Exercises standard oldSwapchain replacement while preserving the configured surface.
 */
VkResult
wltest_renderer_recreate(
	struct wltest_renderer *renderer)
{
	VkSwapchainKHR old;
	VkResult error;

	/* All application references to old image views retire before their swapchain. */
	renderer->operation = "vkDeviceWaitIdle";
	error = vkDeviceWaitIdle(renderer->device);
	if (error != VK_SUCCESS)
		return error;

	/* Retires application attachments before replacing the borrowed image collection. */
	old = renderer->swapchain;
	renderer->swapchain = VK_NULL_HANDLE;
	renderer_targets_free(renderer);

	/* Consumes the retired chain on either creation outcome without losing that outcome. */
	error = renderer_swapchain(renderer, old);
	if (error != VK_SUCCESS) {
		vkDestroySwapchainKHR(renderer->device, old, NULL);
		return error;
	}

	/* The replacement owns its images, allowing the retired chain to release its storage. */
	vkDestroySwapchainKHR(renderer->device, old, NULL);

	/* Builds application attachments for every borrowed swapchain image. */
	error = renderer_targets(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the replacement swapchain owns its application attachments. */
	return VK_SUCCESS;
}

/*
 * Consumes every application owner in dependency order even after partial initialization.
 */
VkResult
wltest_renderer_close(
	struct wltest_renderer *renderer)
{
	VkResult error;

	/* Preserve a real completion failure while still releasing process-local owners. */
	error = VK_SUCCESS;
	if (renderer->device != VK_NULL_HANDLE) {
		/* Retains a completion error while teardown still consumes every local owner. */
		error = vkDeviceWaitIdle(renderer->device);

		/* Releases attachments before the objects referenced by their recorded rendering. */
		renderer_targets_free(renderer);

		/* The pool owns the application's sole command buffer. */
		if (renderer->pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(renderer->device, renderer->pool, NULL);

		/* The pass no longer has live application framebuffers. */
		if (renderer->pass != VK_NULL_HANDLE)
			vkDestroyRenderPass(renderer->device, renderer->pass, NULL);

		/* The acquire semaphore has no future render submission to serve. */
		if (renderer->acquired != VK_NULL_HANDLE)
			vkDestroySemaphore(renderer->device, renderer->acquired, NULL);

		/* The final completion wait has already consumed the fence's purpose. */
		if (renderer->fence != VK_NULL_HANDLE)
			vkDestroyFence(renderer->device, renderer->fence, NULL);

		/* Drops the protocol image owners before retiring their device context. */
		if (renderer->swapchain != VK_NULL_HANDLE)
			vkDestroySwapchainKHR(renderer->device, renderer->swapchain, NULL);

		/* No child object retains this application device. */
		vkDestroyDevice(renderer->device, NULL);
	}

	/* The instance surface only borrows the application's native Wayland objects. */
	if (renderer->surface != VK_NULL_HANDLE)
		vkDestroySurfaceKHR(renderer->instance, renderer->surface, NULL);

	/* Releases the instance after its only surface and device have retired. */
	if (renderer->instance != VK_NULL_HANDLE)
		vkDestroyInstance(renderer->instance, NULL);

	/* Clears stale application identities after consuming every reachable owner. */
	memset(renderer, 0, sizeof(*renderer));
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: no application GPU or native-surface owner remains. */
	return VK_SUCCESS;
}

/* Chooses a real physical device and queue supporting graphics and this surface. */
static VkResult
renderer_device(
	struct wltest_renderer *renderer)
{
	VkPhysicalDevice *devices;
	VkQueueFamilyProperties *families;
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

	/* Enumeration storage is local and never mistaken for driver-owned identities. */
	renderer->operation = "vkEnumeratePhysicalDevices";
	count = 0U;
	error = vkEnumeratePhysicalDevices(renderer->instance, &count, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* No physical device can provide the requested renderer. */
	if (count == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Allocates temporary storage for the finite physical-device inventory. */
	devices = calloc(count, sizeof(*devices));
	if (devices == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Copies physical identities while retaining no ownership over those handles. */
	error = vkEnumeratePhysicalDevices(renderer->instance, &count, devices);
	if (error != VK_SUCCESS) {
		free(devices);
		return error;
	}

	/* Searches each physical device until one exposes a usable graphics/present queue. */
	for (index = 0U; index < count; index++) {
		/* Enumerates this device's queue families before allocating their descriptions. */
		family_count = 0U;
		vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &family_count, NULL);
		if (family_count == 0U)
			continue;

		/* Holds this device's queue descriptions only for the current search iteration. */
		families = calloc(family_count, sizeof(*families));
		if (families == NULL) {
			free(devices);
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}

		/* Obtains the actual queue flags and counts advertised by the device. */
		vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &family_count, families);

		/* Accepts only queues with actual graphics capacity and native surface support. */
		for (family = 0U; family < family_count; family++) {
			/* A graphics queue with no queue instances cannot serve this renderer. */
			if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U || families[family].queueCount == 0U)
				continue;

			/* Queries support for this exact native surface before choosing its queue. */
			supported = VK_FALSE;
			error = vkGetPhysicalDeviceSurfaceSupportKHR(devices[index], family, renderer->surface, &supported);
			if (error == VK_SUCCESS && supported != VK_FALSE) {
				renderer->physical = devices[index];
				renderer->family = family;
				break;
			}
		}

		/* A selected physical handle survives disposal of its temporary descriptions. */
		free(families);
		if (renderer->physical != VK_NULL_HANDLE)
			break;
	}

	/* Enumeration storage is no longer needed after selecting or exhausting the inventory. */
	free(devices);
	if (renderer->physical == VK_NULL_HANDLE)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Standard swapchain is implemented locally by the selected Vulkan library. */
	priority = 1.0f;

	/* Requests exactly one queue from the selected graphics/presentation family. */
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = renderer->family;
	queue.queueCount = 1U;
	queue.pQueuePriorities = &priority;

	/* Enables the standard device swapchain extension used by the application. */
	extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

	/* Describes the device with its one requested queue and standard swapchain extension. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	create.queueCreateInfoCount = 1U;
	create.pQueueCreateInfos = &queue;
	create.enabledExtensionCount = 1U;
	create.ppEnabledExtensionNames = &extension;
	renderer->operation = "vkCreateDevice";
	error = vkCreateDevice(renderer->physical, &create, NULL, &renderer->device);
	if (error != VK_SUCCESS)
		return error;

	/* Obtains the queue created for this graphics and surface pair. */
	vkGetDeviceQueue(renderer->device, renderer->family, 0U, &renderer->queue);

	/* Succeeded: the renderer has one graphics queue for this surface. */
	return VK_SUCCESS;
}

/* Creates a swapchain solely from surface capabilities and advertised formats/modes. */
static VkResult
renderer_swapchain(
	struct wltest_renderer *renderer,
	VkSwapchainKHR old)
{
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR *formats;
	VkPresentModeKHR *modes;
	VkSwapchainCreateInfoKHR create;
	uint32_t count;
	uint32_t index;
	VkBool32 supported;
	VkResult error;

	/* Validate the requested mode before replacing any existing chain. */
	renderer->operation = "vkGetPhysicalDeviceSurfacePresentModesKHR";
	count = 0U;
	error = vkGetPhysicalDeviceSurfacePresentModesKHR(renderer->physical, renderer->surface, &count, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* Allocates the finite presentation-mode inventory returned by this surface. */
	modes = calloc(count, sizeof(*modes));
	if (modes == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* A changed or failed enumeration cannot supply valid mode entries. */
	error = vkGetPhysicalDeviceSurfacePresentModesKHR(renderer->physical, renderer->surface, &count, modes);
	if (error != VK_SUCCESS) {
		free(modes);
		return error;
	}

	/* Checks whether the native presentation-mode list contains the requested mode. */
	supported = VK_FALSE;
	for (index = 0U; index < count; index++) {
		/* Marks support only for the mode explicitly requested by the user. */
		if (modes[index] == renderer->mode)
			supported = VK_TRUE;
	}

	/* The mode inventory has served its purpose without creating a chain. */
	free(modes);

	/* Refuses a requested mode that the platform does not offer. */
	if (supported == VK_FALSE)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* Choose only opaque RGBA/BGRA UNORM for the exact independent color oracle. */
	renderer->operation = "vkGetPhysicalDeviceSurfaceFormatsKHR";
	count = 0U;
	error = vkGetPhysicalDeviceSurfaceFormatsKHR(renderer->physical, renderer->surface, &count, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* Holds the finite format and color-space inventory for this surface. */
	formats = calloc(count, sizeof(*formats));
	if (formats == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Failed enumeration leaves no format entries safe to inspect. */
	error = vkGetPhysicalDeviceSurfaceFormatsKHR(renderer->physical, renderer->surface, &count, formats);
	if (error != VK_SUCCESS) {
		free(formats);
		return error;
	}

	/* Selects an exact UNORM format while preserving the surface's color-space contract. */
	renderer->format = VK_FORMAT_UNDEFINED;
	for (index = 0U; index < count; index++) {
		/* Opaque UNORM channels provide the same exact pattern on either byte order. */
		if ((formats[index].format == VK_FORMAT_R8G8B8A8_UNORM ||
		    formats[index].format == VK_FORMAT_B8G8R8A8_UNORM) &&
		    formats[index].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			renderer->format = formats[index].format;
			break;
		}
	}

	/* Retains the chosen format while releasing its enumeration storage. */
	free(formats);

	/* The test cannot supply its exact pattern through an unsupported format. */
	if (renderer->format == VK_FORMAT_UNDEFINED)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* Obtains the native extent and object-count limits before creation. */
	renderer->operation = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR";
	error = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(renderer->physical, renderer->surface, &capabilities);
	if (error != VK_SUCCESS)
		return error;

	/* The test does not create a differently sized native window behind the compositor's back. */
	if (renderer->extent.width < 64U ||
	    renderer->extent.height < 12U ||
	    renderer->extent.width < capabilities.minImageExtent.width ||
	    renderer->extent.height < capabilities.minImageExtent.height ||
	    renderer->extent.width > capabilities.maxImageExtent.width ||
	    renderer->extent.height > capabilities.maxImageExtent.height)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Requires the attachment usage and opacity actually used by the test pattern. */
	if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0U ||
	    (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) == 0U)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* Starts the native chain description with the required minimum image count. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	create.surface = renderer->surface;
	create.minImageCount = capabilities.minImageCount;

	/* Prefers triple buffering within the advertised native image-count range. */
	if (create.minImageCount < 3U)
		create.minImageCount = 3U;

	/* A nonzero maximum also constrains implementations that permit only two images. */
	if (capabilities.maxImageCount != 0U && create.minImageCount > capabilities.maxImageCount)
		create.minImageCount = capabilities.maxImageCount;

	/* Rejects a contradictory capability range before making an invalid Vulkan request. */
	if (create.minImageCount < capabilities.minImageCount)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Presents the configured window using its native transform and opaque color attachment. */
	create.imageFormat = renderer->format;
	create.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	create.imageExtent = renderer->extent;
	create.imageArrayLayers = 1U;
	create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.preTransform = capabilities.currentTransform;
	create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	create.presentMode = renderer->mode;
	create.clipped = VK_TRUE;
	create.oldSwapchain = old;

	/* Creation alone publishes the new owner; a failed call leaves no replacement handle. */
	renderer->swapchain = VK_NULL_HANDLE;
	renderer->operation = "vkCreateSwapchainKHR";
	error = vkCreateSwapchainKHR(renderer->device, &create, NULL, &renderer->swapchain);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the requested presentation chain belongs to the renderer. */
	return VK_SUCCESS;
}

/* A single color attachment suffices to prove GPU drawing and shared image presentation. */
static VkResult
renderer_pass(
	struct wltest_renderer *renderer)
{
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkRenderPassCreateInfo create;
	VkResult error;

	/* Store every rendered pixel and leave the swapchain image in its required present layout. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = renderer->format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	/* Names the single color attachment used by the graphics subpass. */
	reference.attachment = 0U;
	reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	/* Uses the color attachment from the sole graphics subpass. */
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1U;
	subpass.pColorAttachments = &reference;

	/* Publishes the color attachment and graphics subpass as one render pass. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	create.attachmentCount = 1U;
	create.pAttachments = &attachment;
	create.subpassCount = 1U;
	create.pSubpasses = &subpass;
	renderer->operation = "vkCreateRenderPass";
	error = vkCreateRenderPass(renderer->device, &create, NULL, &renderer->pass);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the render pass preserves the completed color image. */
	return VK_SUCCESS;
}

/* Allocates reusable command and synchronization objects with explicit creation checks. */
static VkResult
renderer_commands(
	struct wltest_renderer *renderer)
{
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo command;
	VkFenceCreateInfo fence;
	VkSemaphoreCreateInfo semaphore;
	VkResult error;

	/* The pool's family is the same graphics/presentation family selected for the device. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = renderer->family;
	renderer->operation = "vkCreateCommandPool";
	error = vkCreateCommandPool(renderer->device, &pool, NULL, &renderer->pool);
	if (error != VK_SUCCESS)
		return error;

	/* Allocates one primary command buffer from the reusable graphics pool. */
	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = renderer->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = 1U;
	renderer->operation = "vkAllocateCommandBuffers";
	error = vkAllocateCommandBuffers(renderer->device, &command, &renderer->command);
	if (error != VK_SUCCESS)
		return error;

	/* Creates the completion fence used before command reuse. */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	renderer->operation = "vkCreateFence";
	error = vkCreateFence(renderer->device, &fence, NULL, &renderer->fence);
	if (error != VK_SUCCESS)
		return error;

	/* Creates the acquire semaphore consumed by each render submission. */
	memset(&semaphore, 0, sizeof(semaphore));
	semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	renderer->operation = "vkCreateSemaphore";
	error = vkCreateSemaphore(renderer->device, &semaphore, NULL, &renderer->acquired);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the renderer can submit and wait for one frame at a time. */
	return VK_SUCCESS;
}

/* Wraps each swapchain image in ordinary image-view and framebuffer objects. */
static VkResult
renderer_targets(
	struct wltest_renderer *renderer)
{
	VkImage *images;
	VkImageViewCreateInfo view;
	VkSemaphoreCreateInfo semaphore;
	VkFramebufferCreateInfo framebuffer;
	uint32_t index;
	VkResult error;

	/* Partial target acquisition remains reachable through the renderer for cleanup. */
	renderer->operation = "vkGetSwapchainImagesKHR";
	renderer->count = 0U;
	error = vkGetSwapchainImagesKHR(renderer->device, renderer->swapchain, &renderer->count, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* An empty chain has no image that the renderer can acquire. */
	if (renderer->count == 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Holds only the borrowed image identities returned by enumeration. */
	images = calloc(renderer->count, sizeof(*images));
	if (images == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Publishes a zeroed attachment table so partial creation remains reclaimable. */
	renderer->targets = calloc(renderer->count, sizeof(*renderer->targets));
	if (renderer->targets == NULL) {
		free(images);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Copies the borrowed image inventory before creating any dependent views. */
	error = vkGetSwapchainImagesKHR(renderer->device, renderer->swapchain, &renderer->count, images);
	if (error != VK_SUCCESS) {
		free(images);
		return error;
	}

	/* Constructs independently reclaimable attachment owners for each borrowed image. */
	for (index = 0U; index < renderer->count; index++) {
		/* Each view refers to the full color subresource of its borrowed image. */
		renderer->targets[index].image = images[index];
		memset(&view, 0, sizeof(view));
		view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view.image = images[index];
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = renderer->format;
		view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		view.subresourceRange.levelCount = 1U;
		view.subresourceRange.layerCount = 1U;
		renderer->operation = "vkCreateImageView";
		error = vkCreateImageView(renderer->device, &view, NULL, &renderer->targets[index].view);
		if (error != VK_SUCCESS)
			break;

		/* Binds this image view as the sole framebuffer attachment. */
		memset(&framebuffer, 0, sizeof(framebuffer));
		framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebuffer.renderPass = renderer->pass;
		framebuffer.attachmentCount = 1U;
		framebuffer.pAttachments = &renderer->targets[index].view;
		framebuffer.width = renderer->extent.width;
		framebuffer.height = renderer->extent.height;
		framebuffer.layers = 1U;
		renderer->operation = "vkCreateFramebuffer";
		error = vkCreateFramebuffer(renderer->device, &framebuffer, NULL, &renderer->targets[index].framebuffer);
		if (error != VK_SUCCESS)
			break;

		/* Reacquiring this image proves its previous present wait consumed this signal. */
		memset(&semaphore, 0, sizeof(semaphore));
		semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		renderer->operation = "vkCreateSemaphore";
		error = vkCreateSemaphore(renderer->device, &semaphore, NULL, &renderer->targets[index].rendered);
		if (error != VK_SUCCESS)
			break;
	}

	/* Releases enumeration storage independently of partially created attachments. */
	free(images);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: every swapchain image has its own view, framebuffer and semaphore. */
	return VK_SUCCESS;
}

/* Releases app-created attachments without destroying borrowed swapchain images. */
static void
renderer_targets_free(
	struct wltest_renderer *renderer)
{
	uint32_t index;

	/* Both arrays can be partially populated after a failed view or framebuffer creation. */
	if (renderer->targets != NULL) {
		/* Consumes only the owners that completed creation in each table entry. */
		for (index = 0U; index < renderer->count; index++) {
			/* The semaphore belongs to this image's presentation cycle. */
			if (renderer->targets[index].rendered != VK_NULL_HANDLE)
				vkDestroySemaphore(renderer->device, renderer->targets[index].rendered, NULL);

			/* The framebuffer must retire before its image view. */
			if (renderer->targets[index].framebuffer != VK_NULL_HANDLE)
				vkDestroyFramebuffer(renderer->device, renderer->targets[index].framebuffer, NULL);

			/* Dropping a view leaves the borrowed swapchain image itself intact. */
			if (renderer->targets[index].view != VK_NULL_HANDLE)
				vkDestroyImageView(renderer->device, renderer->targets[index].view, NULL);
		}
	}

	/* Clears the attachment collection so later cleanup cannot reuse stale identities. */
	free(renderer->targets);
	renderer->targets = NULL;
	renderer->count = 0U;

	/* Succeeded: all application image attachments have retired. */
	return;
}

/* Records one opaque rectangle whose coordinates and exact colors form the capture oracle. */
static void
renderer_rect(
	VkCommandBuffer command,
	uint32_t x,
	uint32_t y,
	uint32_t width,
	uint32_t height,
	float red,
	float green,
	float blue)
{
	VkClearAttachment attachment;
	VkClearRect rect;

	/* ClearAttachments executes on the GPU inside the standard render pass. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	attachment.clearValue.color.float32[0] = red;
	attachment.clearValue.color.float32[1] = green;
	attachment.clearValue.color.float32[2] = blue;
	attachment.clearValue.color.float32[3] = 1.0f;

	/* Restricts this GPU clear to the requested opaque rectangle. */
	memset(&rect, 0, sizeof(rect));
	rect.rect.offset.x = (int32_t)x;
	rect.rect.offset.y = (int32_t)y;
	rect.rect.extent.width = width;
	rect.rect.extent.height = height;
	rect.layerCount = 1U;
	vkCmdClearAttachments(command, 1U, &attachment, 1U, &rect);

	/* Succeeded: the GPU command stream contains the requested opaque rectangle. */
	return;
}
