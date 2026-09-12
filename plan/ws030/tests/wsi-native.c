/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Tests the actual native WSI adapter with a finite ioctl device model. */

#include "wsi-internal.h"
#include <uapi/gpu.h>
#include <uapi/gpu-display.h>

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct vulkan_context context;
static struct VkDevice_T device;
static struct VkPhysicalDevice_T physical;
static struct vulkan_display display;
static struct vulkan_display_mode modes[2];
static struct vulkan_surface surfaces[2];
static unsigned claims;
static unsigned releases;
static unsigned creates;
static unsigned destroys;
static unsigned presents;
static uint64_t native_storage;
static uint64_t native_bytes;
static uint64_t copied;
static int injected_error;
static unsigned allocations;
static unsigned frees;

static void *native_allocate(void *user, size_t bytes, size_t alignment, VkSystemAllocationScope scope);
static void native_free(void *user, void *pointer);

/*
 * Serializes raw native adapter requests using the same context lock contract as production.
 */
void
vulkan_context_lock(
	struct vulkan_context *locked_context)
{
	int error;

	/* The actual adapter must acquire its shared context before any native request. */
	error = pthread_mutex_lock(&locked_context->mutex);
	assert(error == 0);

	/* Succeeded: raw device transactions cannot overlap this context's wire commands. */
	return;
}

/*
 * Ends the native adapter transaction before the next Vulkan operation can proceed.
 */
void
vulkan_context_unlock(
	struct vulkan_context *locked_context)
{
	int error;

	/* A mismatched unlock is a fixture failure rather than an ignored synchronization error. */
	error = pthread_mutex_unlock(&locked_context->mutex);
	assert(error == 0);

	/* Succeeded: the shared context can admit its next independent transaction. */
	return;
}

/* Exercises native reservation sharing and surviving independent surface lifetimes. */
int
main(void)
{
	struct vulkan_wsi_pixels pixels;
	VkAllocationCallbacks allocator;
	VkDisplayModeParametersKHR parameters;
	void *first;
	void *replacement;
	void *second;
	uint8_t *bytes;
	uint64_t sequence;
	uint32_t count;
	uint32_t index;
	VkResult error;

	/* All raw requests must serialize with this actual shared context mutex. */
	memset(&context, 0, sizeof(context));
	context.fd = 7;
	pthread_mutex_init(&context.mutex, NULL);
	device.object.context = &context;
	device.physical = &physical;
	physical.object.context = &context;
	display.physical = &physical;
	display.output.identifier = 1U;
	display.output.generation = 1U;
	display.output.flags = VULKAN_WSI_OUTPUT_CONNECTED | VULKAN_WSI_OUTPUT_FIFO;
	display.output.formats = VULKAN_WSI_FORMAT_RGBA;
	display.output.plane_count = 1U;
	display.output.refresh_millihz = 50000U;

	/* Count callbacks separately from device-owned plane and surface-owned lease scopes. */
	memset(&allocator, 0, sizeof(allocator));
	allocator.pfnAllocation = native_allocate;
	allocator.pfnFree = native_free;
	device.object.allocator.has_callbacks = VK_TRUE;
	device.object.allocator.callbacks = allocator;
	for (index = 0U; index < 2U; index++) {
		modes[index].display = &display;
		modes[index].generation = 1U;
		modes[index].parameters.visibleRegion.width = 320U * (index + 1U);
		modes[index].parameters.visibleRegion.height = 240U * (index + 1U);
		modes[index].parameters.refreshRate = 50000U;
		surfaces[index].display_mode = &modes[index];
		surfaces[index].object.allocator = device.object.allocator;
		surfaces[index].display_info.imageExtent = modes[index].parameters.visibleRegion;
	}

	/* Different surface modes on the same native plane share one kernel claim. */
	error = vulkan_wsi_display_platform.claim(&surfaces[0], &device, &first);
	assert(error == VK_SUCCESS);
	error = vulkan_wsi_display_platform.claim(&surfaces[0], &device, &replacement);
	assert(error == VK_SUCCESS && first == replacement);
	error = vulkan_wsi_display_platform.claim(&surfaces[1], &device, &second);
	assert(error == VK_SUCCESS && first != second);
	assert(claims == 1U);

	/* Native presentation receives completed Vulkan bytes through bounded copies. */
	bytes = malloc(640U * 480U * 4U);
	assert(bytes != NULL);
	memset(bytes, 0x73, 640U * 480U * 4U);
	memset(&pixels, 0, sizeof(pixels));
	pixels.pixels = bytes;
	pixels.extent = modes[0].parameters.visibleRegion;
	pixels.stride = pixels.extent.width * 4U;
	pixels.format = VK_FORMAT_R8G8B8A8_UNORM;
	pixels.frame = 1U;
	error = vulkan_wsi_display_platform.present(first, &pixels, &sequence);
	assert(error == VK_SUCCESS && sequence == 1U);
	assert(creates == 1U);
	error = vulkan_wsi_display_platform.wait(first, sequence, 0U);
	assert(error == VK_SUCCESS);

	/* A surface can retire completely while another surface keeps the native plane. */
	error = vulkan_wsi_display_platform.release(first);
	assert(error == VK_SUCCESS);
	error = vulkan_wsi_display_platform.release(replacement);
	assert(error == VK_SUCCESS);
	assert(surfaces[0].platform_private == NULL);
	assert(releases == 0U);
	memset(&surfaces[0], 0, sizeof(surfaces[0]));
	pixels.extent = modes[1].parameters.visibleRegion;
	pixels.stride = pixels.extent.width * 4U;
	pixels.frame = 2U;
	error = vulkan_wsi_display_platform.present(second, &pixels, &sequence);
	assert(error == VK_SUCCESS && sequence == 2U);
	assert(creates == 2U && destroys == 1U);
	error = vulkan_wsi_display_platform.release(second);
	assert(error == VK_SUCCESS);
	assert(surfaces[1].platform_private == NULL);
	assert(releases == 1U && destroys == 2U);

	/* Native error classification occurs after dropping the shared context mutex. */
	injected_error = EBUSY;
	error = vulkan_wsi_display_platform.claim(&surfaces[1], &device, &second);
	assert(error == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR);
	assert(surfaces[1].platform_private == NULL);
	injected_error = EINVAL;
	parameters = modes[1].parameters;
	error = vulkan_wsi_display_mode_validate(&physical, &display.output, &parameters);
	assert(error == VK_ERROR_INITIALIZATION_FAILED);
	injected_error = 0;
	error = vulkan_wsi_display_query(&physical, UINT32_MAX, &count, NULL);
	assert(error == VK_SUCCESS && count == 1U);
	assert(allocations == frees);
	free(bytes);
	pthread_mutex_destroy(&context.mutex);

	/* Reports native boundary coverage without claiming actual GPU or display execution. */
	puts("WSI native adapter: context mutex, shared plane/surface lifetime, mode replacement and copied frame PASS");
	return 0;
}

/* Supplies a controlled ioctl implementation and verifies admission serialization. */
int
ioctl(
	int fd,
	unsigned long command,
	...)
{
	va_list arguments;
	void *pointer;
	struct gpu_display_claim *claim;
	struct gpu_display_present *present;
	struct gpu_display_wait *wait;
	struct gpu_resource_create *create;
	struct gpu_resource_destroy *destroy;
	struct gpu_transfer *transfer;
	struct gpu_display_info *query;
	const uint8_t *source;
	uint32_t index;
	int status;

	/* A raw adapter request must hold the same mutex used by wire and memory paths. */
	assert(fd == context.fd);
	status = pthread_mutex_trylock(&context.mutex);
	assert(status == EBUSY);
	va_start(arguments, command);
	pointer = va_arg(arguments, void *);
	va_end(arguments);
	if (injected_error != 0) {
		errno = injected_error;
		return -1;
	}

	/* Every mock command checks the source or lifetime boundary it represents. */
	switch (command) {
	case GPU_DISPLAY_CLAIM:
		claim = pointer;
		assert(claim->version == GPU_ABI_VERSION);
		assert(claim->display_id == 1U && claim->plane_index == 0U);
		assert(claim->generation == 1U && claim->lease == 0U);
		claim->lease = 100U;
		claims++;
		break;
	case GPU_DISPLAY_RELEASE:
		assert(((struct gpu_display_release *)pointer)->lease == 100U);
		releases++;
		break;
	case GPU_RESOURCE_CREATE:
		create = pointer;
		assert(create->usage == GPU_RESOURCE_USAGE_STORAGE);
		assert(create->handle == 0U);
		create->handle = ++creates;
		native_storage = create->handle;
		native_bytes = create->bytes;
		copied = 0U;
		break;
	case GPU_RESOURCE_DESTROY:
		destroy = pointer;
		assert(destroy->handle != 0U);
		destroys++;
		break;
	case GPU_RESOURCE_WRITE:
		transfer = pointer;
		assert(transfer->handle == native_storage);
		assert(transfer->bytes <= GPU_COPY_MAX);
		assert(transfer->offset == copied);
		source = (const uint8_t *)(uintptr_t)transfer->address;
		for (index = 0U; index < transfer->bytes; index++)
			assert(source[index] == 0x73U);
		copied += transfer->bytes;
		break;
	case GPU_DISPLAY_PRESENT:
		present = pointer;
		assert(present->lease == 100U && present->handle == native_storage);
		assert(present->flags == GPU_DISPLAY_PRESENT_FIFO);
		assert(present->refresh_millihz == 50000U);
		assert((uint64_t)present->stride * present->height == native_bytes);
		assert(copied == native_bytes);
		assert(present->sequence == 0U);
		present->sequence = ++presents;
		copied = 0U;
		break;
	case GPU_DISPLAY_WAIT:
		wait = pointer;
		assert(wait->lease == 100U && wait->sequence <= presents);
		wait->completed_sequence = presents;
		wait->generation = 1U;
		break;
	case GPU_DISPLAY_QUERY:
		query = pointer;
		assert(query->index == UINT32_MAX);
		query->count = 1U;
		break;
	default:
		assert(0);
	}

	/* Succeeded: this complete request ran while holding native admission ownership. */
	return 0;
}

/* Keeps native capability generation stable without linking another discovery layer. */
VkResult
vulkan_wsi_display_refresh(
	struct vulkan_display *value)
{
	/* Only the initialized physical output belongs to this finite test model. */
	assert(value == &display);

	/* Succeeded: no native topology event occurred. */
	return VK_SUCCESS;
}

/* Returns an immutable fixture snapshot through the actual adapter's shared boundary. */
VkResult
vulkan_wsi_display_snapshot(
	struct vulkan_display *value,
	struct vulkan_wsi_output *output)
{
	/* Production uses the WSI lock here; the fixture owns one fixed output. */
	assert(value == &display);
	*output = display.output;

	/* Succeeded: adapter tests never read a partially changed capability structure. */
	return VK_SUCCESS;
}

/* Reports renderer support for the native adapter's ordinary packed image usage. */
VKAPI_ATTR VkResult VKAPI_CALL
vkGetPhysicalDeviceImageFormatProperties(
	VkPhysicalDevice gpu,
	VkFormat format,
	VkImageType type,
	VkImageTiling tiling,
	VkImageUsageFlags usage,
	VkImageCreateFlags flags,
	VkImageFormatProperties *properties)
{
	/* The native adapter must intersect format support with the rendering device. */
	assert(gpu == (VkPhysicalDevice)&physical);
	assert(format == VK_FORMAT_R8G8B8A8_UNORM);
	assert(type == VK_IMAGE_TYPE_2D && tiling == VK_IMAGE_TILING_OPTIMAL);
	assert((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0U);
	assert(flags == 0U);
	memset(properties, 0, sizeof(*properties));

	/* Succeeded: this mock renderer supports the requested packed image contract. */
	return VK_SUCCESS;
}

/* Tracks callbacks while allowing ordinary host alignment for these local objects. */
static void *
native_allocate(
	void *user,
	size_t bytes,
	size_t alignment,
	VkSystemAllocationScope scope)
{
	void *pointer;

	/* A native plane and its lease have distinct device and object allocation scopes. */
	(void)user;
	assert(alignment <= 16U);
	assert(scope == VK_SYSTEM_ALLOCATION_SCOPE_DEVICE ||
	    scope == VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	pointer = malloc(bytes);
	assert(pointer != NULL);
	allocations++;

	/* Succeeded: the actual adapter owns one counted local allocation. */
	return pointer;
}

/* Confirms every candidate, lease and shared plane is eventually freed once. */
static void
native_free(
	void *user,
	void *pointer)
{
	/* The real allocator excludes null pointers before invoking this callback. */
	(void)user;
	assert(pointer != NULL);
	frees++;
	free(pointer);

	/* Succeeded: this local ownership allocation has been returned to its callback. */
	return;
}
