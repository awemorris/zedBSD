/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * vkimage: the Vulkan image test.  On the first physical device it makes a
 * linear and an optimal colour image, checks that the linear image's rows
 * are a multiple of 32 bytes, that an image declaring external memory is
 * made (the shared path's image) and that a linear B8G8R8A8 image
 * reports sampling (the swapchain and client images are linear), and
 * binds the optimal image to memory with a view and a sampler.  It prints one line:
 *
 *   vkimage: <device>: PASS
 *   vkimage: <device>: FAIL, <what> <VkResult or value>
 *   vkimage: no Vulkan device (<call> <VkResult>)
 *
 * The exit status is 0 only on PASS.
 */

#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The Vulkan objects of one run; every handle starts null and is destroyed at the end. */
struct vkimage {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkImage linear;
	VkImage optimal;
	VkImage shared;
	VkDeviceMemory memory;
	VkImageView view;
	VkSampler sampler;
	char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
	const char *failed;
	long value;
};

static VkResult open_device(struct vkimage *run);
static VkResult make_image(struct vkimage *run, VkImageTiling tiling, int external, VkImage *image);
static int check_images(struct vkimage *run);
static void destroy(struct vkimage *run);

/* Runs the checks once and prints the result line. */
int
main(
	void)
{
	struct vkimage run;
	VkResult status;
	int failed;

	/* Opens the instance and the device; with none, nothing else can run. */
	memset(&run, 0, sizeof(run));
	status = open_device(&run);
	if (status != VK_SUCCESS) {
		printf("vkimage: no Vulkan device (%s %d)\n", run.failed, (int)status);
		destroy(&run);
		return 1;
	}

	/* The checks, then the verdict. */
	failed = check_images(&run);
	if (failed != 0)
		printf("vkimage: %s: FAIL, %s %ld\n", run.name, run.failed, run.value);
	else
		printf("vkimage: %s: PASS\n", run.name);
	destroy(&run);
	return failed;
}

/* Creates the instance and a device of one queue on the first physical device. */
static VkResult
open_device(
	struct vkimage *run)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo instance_info;
	VkPhysicalDeviceProperties properties;
	VkDeviceQueueCreateInfo queue_info;
	VkDeviceCreateInfo device_info;
	uint32_t count;
	float priority;
	VkResult status;

	/* The instance. */
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "vkimage";
	/* 1.0: the library exposes the 1.0 API and refuses a later one (VK_ERROR_INCOMPATIBLE_DRIVER); nothing here needs 1.1. */
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&instance_info, 0, sizeof(instance_info));
	instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info.pApplicationInfo = &application;
	run->failed = "vkCreateInstance";
	status = vkCreateInstance(&instance_info, NULL, &run->instance);
	if (status != VK_SUCCESS)
		return status;

	/* The first physical device. */
	count = 1;
	run->failed = "vkEnumeratePhysicalDevices";
	status = vkEnumeratePhysicalDevices(run->instance, &count, &run->physical);
	if (status != VK_SUCCESS && status != VK_INCOMPLETE)
		return status;
	if (count == 0)
		return VK_ERROR_INITIALIZATION_FAILED;
	vkGetPhysicalDeviceProperties(run->physical, &properties);
	snprintf(run->name, sizeof(run->name), "%s", properties.deviceName);

	/* The device, with one queue of family 0. */
	priority = 1.0f;
	memset(&queue_info, 0, sizeof(queue_info));
	queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &priority;
	memset(&device_info, 0, sizeof(device_info));
	device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue_info;
	run->failed = "vkCreateDevice";
	status = vkCreateDevice(run->physical, &device_info, NULL, &run->device);
	if (status != VK_SUCCESS)
		return status;

	/* Succeeded: the device is open. */
	return VK_SUCCESS;
}

/* Makes a 100x10 RGBA8 image of the tiling, declaring external memory when external. */
static VkResult
make_image(
	struct vkimage *run,
	VkImageTiling tiling,
	int external,
	VkImage *image)
{
	VkExternalMemoryImageCreateInfo declaration;
	VkImageCreateInfo info;

	/* The external declaration a window system's shared image carries. */
	memset(&declaration, 0, sizeof(declaration));
	declaration.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	declaration.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

	/* The image. */
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	if (external)
		info.pNext = &declaration;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = VK_FORMAT_R8G8B8A8_UNORM;
	info.extent.width = 100;
	info.extent.height = 10;
	info.extent.depth = 1;
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = tiling;
	info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (tiling == VK_IMAGE_TILING_OPTIMAL)
		info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	return vkCreateImage(run->device, &info, NULL, image);
}

/* Runs the checks; 0 when all pass, 1 with failed and value set. */
static int
check_images(
	struct vkimage *run)
{
	VkFormatProperties format;
	VkSubresourceLayout layout;
	VkImageSubresource subresource;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	VkImageViewCreateInfo view;
	VkSamplerCreateInfo sampler;
	VkResult status;

	/* A linear B8G8R8A8 image is sampled (a compositor samples linear client images). */
	vkGetPhysicalDeviceFormatProperties(run->physical, VK_FORMAT_B8G8R8A8_UNORM, &format);
	if ((format.linearTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) {
		run->failed = "linear not sampled";
		run->value = (long)format.linearTilingFeatures;
		return 1;
	}

	/* An image declaring external memory is made. */
	status = make_image(run, VK_IMAGE_TILING_LINEAR, 1, &run->shared);
	if (status != VK_SUCCESS) {
		run->failed = "external image";
		run->value = status;
		return 1;
	}

	/* A linear image whose rows are a multiple of 32 bytes. */
	status = make_image(run, VK_IMAGE_TILING_LINEAR, 0, &run->linear);
	if (status != VK_SUCCESS) {
		run->failed = "linear vkCreateImage";
		run->value = status;
		return 1;
	}
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vkGetImageSubresourceLayout(run->device, run->linear, &subresource, &layout);
	if (layout.rowPitch < 400 || layout.rowPitch % 32U != 0) {
		run->failed = "rowPitch";
		run->value = (long)layout.rowPitch;
		return 1;
	}

	/* An optimal image bound to memory of its requirements. */
	status = make_image(run, VK_IMAGE_TILING_OPTIMAL, 0, &run->optimal);
	if (status != VK_SUCCESS) {
		run->failed = "optimal vkCreateImage";
		run->value = status;
		return 1;
	}
	vkGetImageMemoryRequirements(run->device, run->optimal, &requirements);
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = 0;
	status = vkAllocateMemory(run->device, &allocate, NULL, &run->memory);
	if (status == VK_SUCCESS)
		status = vkBindImageMemory(run->device, run->optimal, run->memory, 0);
	if (status != VK_SUCCESS) {
		run->failed = "optimal memory";
		run->value = status;
		return 1;
	}

	/* Its view and a sampler. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = run->optimal;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_R8G8B8A8_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = 1;
	status = vkCreateImageView(run->device, &view, NULL, &run->view);
	if (status != VK_SUCCESS) {
		run->failed = "vkCreateImageView";
		run->value = status;
		return 1;
	}
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_LINEAR;
	sampler.minFilter = VK_FILTER_LINEAR;
	status = vkCreateSampler(run->device, &sampler, NULL, &run->sampler);
	if (status != VK_SUCCESS) {
		run->failed = "vkCreateSampler";
		run->value = status;
		return 1;
	}

	/* Succeeded: every check passed. */
	return 0;
}

/* Destroys whatever the run made. */
static void
destroy(
	struct vkimage *run)
{
	/* The device's objects, then the device and the instance. */
	if (run->device != VK_NULL_HANDLE) {
		vkDestroySampler(run->device, run->sampler, NULL);
		vkDestroyImageView(run->device, run->view, NULL);
		vkDestroyImage(run->device, run->optimal, NULL);
		vkDestroyImage(run->device, run->linear, NULL);
		vkDestroyImage(run->device, run->shared, NULL);
		vkFreeMemory(run->device, run->memory, NULL);
		vkDestroyDevice(run->device, NULL);
	}
	if (run->instance != VK_NULL_HANDLE)
		vkDestroyInstance(run->instance, NULL);
}
