/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Actual WSI discovery and swapchain lifetime tests with a mock native display
 * and mock Vulkan rendering core. This fixture does not claim GPU execution.
 */

#include "wsi-internal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct test_memory {
	struct vulkan_memory memory;
	void *pixels;
};

struct test_buffer {
	struct vulkan_object object;
	VkDeviceSize bytes;
	struct test_memory *memory;
};

struct test_pool {
	struct vulkan_object object;
	struct VkCommandBuffer_T *command;
};

struct test_lease {
	struct vulkan_surface *surface;
	uint32_t references;
};

/* Finite fixture state counts ownership transfers and injected failure boundaries. */
static unsigned allocations;
static unsigned releases;
static unsigned fail_allocation;
static unsigned allocation_attempts;
static unsigned native_claims;
static unsigned native_releases;
static unsigned native_presents;
static unsigned submissions;
static unsigned acquire_signals;
static unsigned copies;
static unsigned query_generation = 1U;
static unsigned fail_claim;
static unsigned expected_waits;
static unsigned fail_submit;
static unsigned fail_native;
static VkResult query_error = VK_SUCCESS;
static struct VkInstance_T instance;
static struct VkPhysicalDevice_T physical;
static struct VkDevice_T device;
static struct VkQueue_T queue;
static VkQueueFamilyProperties family;
static struct VkQueue_T *queues[1];
static VkAllocationCallbacks callbacks;

static void *test_allocate(void *user, size_t bytes, size_t alignment, VkSystemAllocationScope scope);
static void test_free(void *user, void *pointer);
static struct vulkan_object *test_object(size_t bytes, enum vulkan_object_kind kind, const VkAllocationCallbacks *allocator);
static VkResult native_capabilities(struct vulkan_surface *surface, struct VkPhysicalDevice_T *gpu, VkSurfaceCapabilitiesKHR *caps);
static VkResult native_formats(struct vulkan_surface *surface, struct VkPhysicalDevice_T *gpu, uint32_t *count, VkSurfaceFormatKHR *formats);
static VkResult native_modes(struct vulkan_surface *surface, struct VkPhysicalDevice_T *gpu, uint32_t *count, VkPresentModeKHR *modes);
static VkResult native_claim(struct vulkan_surface *surface, struct VkDevice_T *gpu, void **result);
static VkResult native_release(void *pointer);
static VkResult native_present(void *pointer, const struct vulkan_wsi_pixels *pixels, uint64_t *sequence);
static VkResult native_wait(void *pointer, uint64_t sequence, uint64_t timeout);
static VkSurfaceKHR test_surface(uint32_t index, VkDisplayKHR display);
static void test_discovery(VkDisplayKHR *displays);
static void test_swapchains(VkSurfaceKHR first, VkSurfaceKHR second);
static void test_shared(VkSurfaceKHR first, VkSurfaceKHR second);
static void test_present(VkSwapchainKHR chain, uint32_t index);
static VkSwapchainCreateInfoKHR test_create_info(VkSurfaceKHR surface, uint32_t count);

/* Only this mock adapter replaces the native kernel transport in the fixture. */
const struct vulkan_wsi_platform_ops vulkan_wsi_display_platform = {
	native_capabilities, native_formats, native_modes, native_claim,
	native_release, native_present, native_wait, NULL
};

/* Exercises real WSI implementations using dynamic counts and independent owners. */
int
main(void)
{
	VkDisplayKHR displays[40];
	VkSurfaceKHR first;
	VkSurfaceKHR second;
	unsigned before;

	/* Initializes real common object prefixes without creating a renderer context. */
	memset(&instance, 0, sizeof(instance));
	memset(&physical, 0, sizeof(physical));
	memset(&device, 0, sizeof(device));
	memset(&queue, 0, sizeof(queue));
	instance.object.kind = VULKAN_OBJECT_INSTANCE;
	physical.object.kind = VULKAN_OBJECT_PHYSICAL_DEVICE;
	physical.instance = &instance;
	device.object.kind = VULKAN_OBJECT_DEVICE;
	device.physical = &physical;
	queue.object.kind = VULKAN_OBJECT_QUEUE;
	queue.device = &device;
	family.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT;
	physical.queue_families = &family;
	physical.queue_family_count = 1U;
	physical.memory.memoryTypeCount = 1U;
	physical.memory.memoryTypes[0].propertyFlags =
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	queues[0] = &queue;
	device.queues = queues;
	device.queue_count = 1U;
	pthread_mutex_init(&device.mutex, NULL);

	/* Counts both implicit instance allocations and explicit swapchain callbacks. */
	memset(&callbacks, 0, sizeof(callbacks));
	callbacks.pfnAllocation = test_allocate;
	callbacks.pfnFree = test_free;
	instance.object.allocator.has_callbacks = VK_TRUE;
	instance.object.allocator.callbacks = callbacks;
	device.object.allocator = instance.object.allocator;
	test_discovery(displays);
	first = test_surface(0U, displays[0]);
	second = test_surface(1U, displays[1]);
	test_swapchains(first, second);
	test_shared(first, second);

	/* Instance teardown must reclaim implicitly discovered modes and display names. */
	vulkan_wsi_device_finish(&device);
	vkDestroySurfaceKHR((VkInstance)&instance, first, &callbacks);
	vkDestroySurfaceKHR((VkInstance)&instance, second, &callbacks);
	before = releases;
	vulkan_wsi_instance_finish(&instance);
	assert(releases > before);
	assert(physical.object.first_child == NULL);
	assert(instance.object.first_child == NULL);
	assert(device.object.first_child == NULL);
	assert(allocations == releases);
	assert(native_claims == native_releases);
	pthread_mutex_destroy(&device.mutex);

	/* Reports only the mock-backed ownership and dispatch coverage established here. */
	puts("WSI discovery/swapchain: dynamic40 outputs,37 images, shared rollback, acquire/present and cleanup PASS");
	return 0;
}

/* Supplies forty dynamically enumerated outputs with mutable capability generations. */
VkResult
vulkan_wsi_display_query(
	struct VkPhysicalDevice_T *gpu,
	uint32_t index,
	uint32_t *count,
	struct vulkan_wsi_output *output)
{
	/* The fixture has one GPU and controlled native topology failure injection. */
	assert(gpu == &physical);
	if (query_error != VK_SUCCESS)
		return query_error;
	*count = 40U;
	if (index == UINT32_MAX)
		return VK_SUCCESS;
	assert(index < 40U);
	memset(output, 0, sizeof(*output));
	output->identifier = index + 1U;
	output->generation = query_generation;
	output->max_frame_bytes = 16U * 1024U * 1024U;
	output->flags = VULKAN_WSI_OUTPUT_CONNECTED | VULKAN_WSI_OUTPUT_FIFO;
	output->formats = VULKAN_WSI_FORMAT_RGBA;
	output->plane_count = 1U;
	output->preferred_extent.width = 320U;
	output->preferred_extent.height = 240U;
	output->maximum_extent.width = 4096U;
	output->maximum_extent.height = 4096U;
	output->refresh_millihz = 50000U;
	snprintf(output->name, sizeof(output->name), "display-%u-generation-%u", index, query_generation);

	/* Succeeded: the actual discovery code receives a complete mock native snapshot. */
	return VK_SUCCESS;
}

/* Supplies several stable descriptions without creating any rendering objects. */
VkResult
vulkan_wsi_display_mode_query(
	struct VkPhysicalDevice_T *gpu,
	const struct vulkan_wsi_output *output,
	uint32_t index,
	uint32_t *count,
	VkDisplayModeParametersKHR *mode)
{
	/* Native modes belong to the exact generation used by the discovery code. */
	assert(gpu == &physical);
	assert(output->generation == query_generation);
	*count = 3U;
	if (index == UINT32_MAX)
		return VK_SUCCESS;
	assert(index < 3U);
	mode->visibleRegion.width = 320U + index * 160U;
	mode->visibleRegion.height = 240U + index * 120U;
	mode->refreshRate = 50000U;

	/* Succeeded: the real mode cache decides handle identity and allocation lifetime. */
	return VK_SUCCESS;
}

/* Accepts a finite virtual mode range for custom mode construction tests. */
VkResult
vulkan_wsi_display_mode_validate(
	struct VkPhysicalDevice_T *gpu,
	const struct vulkan_wsi_output *output,
	const VkDisplayModeParametersKHR *mode)
{
	/* A deliberately invalid refresh must fail without acquiring native ownership. */
	assert(gpu == &physical);
	assert(output->generation == query_generation);
	if (mode->refreshRate != 50000U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Succeeded: mode creation remains a metadata operation. */
	return VK_SUCCESS;
}

/* Mocks the core's completed acquire payload boundary, independently of WSI state. */
VkResult
vulkan_wsi_acquire_signal(
	struct VkDevice_T *gpu,
	VkSemaphore semaphore,
	VkFence fence)
{
	/* The WSI must request at least one standard synchronization object. */
	assert(gpu == &device);
	assert(semaphore != VK_NULL_HANDLE || fence != VK_NULL_HANDLE);
	acquire_signals++;

	/* Succeeded: image acquisition can now publish its selected index. */
	return VK_SUCCESS;
}

/* Records exactly one semaphore-consuming submit for an entire present operation. */
VkResult
vulkan_queue_submit(
	struct VkQueue_T *target,
	uint32_t count,
	const VkSubmitInfo *submit,
	VkFence fence)
{
	/* Injected enqueue failure must preserve every acquired image and wait payload. */
	assert(target == &queue);
	assert(count == 1U);
	assert(submit->waitSemaphoreCount == expected_waits);
	assert(submit->commandBufferCount == 1U);
	assert(fence != VK_NULL_HANDLE);
	if (fail_submit != 0U)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	submissions++;

	/* Succeeded: the fixture has accepted one complete queue transaction. */
	return VK_SUCCESS;
}

/* Completes the mock GPU fence; real GPU execution is tested separately. */
VkResult
vulkan_fences_wait(
	struct VkDevice_T *gpu,
	uint32_t count,
	const VkFence *fences,
	VkBool32 all,
	uint64_t timeout)
{
	/* Native presentation must wait for the actual submitted fence boundary. */
	assert(gpu == &device);
	assert(count == 1U);
	assert(fences[0] != VK_NULL_HANDLE);
	assert(all != VK_FALSE);
	assert(timeout != 0U);

	/* Succeeded: the recorded mock copy is now available to native presentation. */
	return VK_SUCCESS;
}

/* Creates an ordinary image object so real swapchain ownership metadata is exercised. */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImage(
	VkDevice gpu,
	const VkImageCreateInfo *info,
	const VkAllocationCallbacks *allocator,
	VkImage *result)
{
	struct vulkan_image *image;

	/* The WSI must preserve requested geometry and add internal readback use. */
	assert(gpu == (VkDevice)&device);
	assert((info->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0U);
	image = (struct vulkan_image *)test_object(sizeof(*image), VULKAN_OBJECT_IMAGE, allocator);
	if (image == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	image->extent = info->extent;
	image->format = info->format;
	*result = (VkImage)vulkan_nondispatchable_handle(&image->object);

	/* Succeeded: this is a normal Vulkan image with WSI-controlled lifetime. */
	return VK_SUCCESS;
}

/* Verifies that WSI clears its ownership guard before legitimate image retirement. */
VKAPI_ATTR void VKAPI_CALL
vkDestroyImage(
	VkDevice gpu,
	VkImage handle,
	const VkAllocationCallbacks *allocator)
{
	struct vulkan_image *image;

	/* Shared image groups may destroy an image only once. */
	(void)allocator;
	assert(gpu == (VkDevice)&device);
	image = vulkan_image(handle);
	assert(image->swapchain_owned == VK_FALSE);
	vulkan_object_free(&image->object);

	/* Succeeded: the real shared-group reference count authorized image destruction. */
	return;
}

/* Supplies nontrivial allocation alignment independently of packed pixel assumptions. */
VKAPI_ATTR void VKAPI_CALL
vkGetImageMemoryRequirements(
	VkDevice gpu,
	VkImage handle,
	VkMemoryRequirements *requirements)
{
	struct vulkan_image *image;

	/* Actual WSI code must obtain and honor the renderer's requirement record. */
	assert(gpu == (VkDevice)&device);
	image = vulkan_image(handle);
	requirements->size = (VkDeviceSize)image->extent.width * image->extent.height * 4U;
	requirements->alignment = 4096U;
	requirements->memoryTypeBits = 1U;

	/* Succeeded: allocation is driven by the queried requirements. */
	return;
}

/* Checks the ordinary image binding path used by the real WSI implementation. */
VKAPI_ATTR VkResult VKAPI_CALL
vkBindImageMemory(
	VkDevice gpu,
	VkImage image,
	VkDeviceMemory memory,
	VkDeviceSize offset)
{
	/* Bound handles must be real fixture objects of their standard kinds. */
	assert(gpu == (VkDevice)&device);
	assert(vulkan_image(image) != NULL);
	assert(vulkan_memory(memory) != NULL);
	assert(offset == 0U);

	/* Succeeded: the mock renderer now accepts this image allocation. */
	return VK_SUCCESS;
}

/* Creates ordinary buffer ownership and remembers its requested readback extent. */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateBuffer(
	VkDevice gpu,
	const VkBufferCreateInfo *info,
	const VkAllocationCallbacks *allocator,
	VkBuffer *result)
{
	struct test_buffer *buffer;

	/* WSI staging carries only completed readback and is not a native scanout image. */
	assert(gpu == (VkDevice)&device);
	assert(info->usage == VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	buffer = (struct test_buffer *)test_object(sizeof(*buffer), VULKAN_OBJECT_BUFFER, allocator);
	if (buffer == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	buffer->bytes = info->size;
	*result = (VkBuffer)vulkan_nondispatchable_handle(&buffer->object);

	/* Succeeded: the real WSI owns this standard buffer handle. */
	return VK_SUCCESS;
}

/* Frees ordinary buffer storage through the common object allocator. */
VKAPI_ATTR void VKAPI_CALL
vkDestroyBuffer(
	VkDevice gpu,
	VkBuffer handle,
	const VkAllocationCallbacks *allocator)
{
	/* The corresponding device memory must remain independently owned. */
	(void)allocator;
	assert(gpu == (VkDevice)&device);
	vulkan_object_free(vulkan_nondispatchable_object((uint64_t)handle));

	/* Succeeded: only the buffer object has retired. */
	return;
}

/* Returns the mock buffer extent as the memory allocation requirement. */
VKAPI_ATTR void VKAPI_CALL
vkGetBufferMemoryRequirements(
	VkDevice gpu,
	VkBuffer handle,
	VkMemoryRequirements *requirements)
{
	struct test_buffer *buffer;

	/* Buffer and image requirement queries are distinct paths. */
	assert(gpu == (VkDevice)&device);
	buffer = (struct test_buffer *)vulkan_nondispatchable_object((uint64_t)handle);
	requirements->size = buffer->bytes;
	requirements->alignment = 4096U;
	requirements->memoryTypeBits = 1U;

	/* Succeeded: the real WSI can allocate compatible staging memory. */
	return;
}

/* Associates the mock buffer with the ordinary mapped allocation used by readback. */
VKAPI_ATTR VkResult VKAPI_CALL
vkBindBufferMemory(
	VkDevice gpu,
	VkBuffer handle,
	VkDeviceMemory memory,
	VkDeviceSize offset)
{
	struct test_buffer *buffer;

	/* Binding retains a memory reference without transferring its lifetime. */
	assert(gpu == (VkDevice)&device);
	assert(offset == 0U);
	buffer = (struct test_buffer *)vulkan_nondispatchable_object((uint64_t)handle);
	buffer->memory = (struct test_memory *)vulkan_memory(memory);

	/* Succeeded: recorded image copies can locate this buffer's mapped pixels. */
	return VK_SUCCESS;
}

/* Allocates a counted ordinary memory object with fixture-only CPU backing. */
VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateMemory(
	VkDevice gpu,
	const VkMemoryAllocateInfo *info,
	const VkAllocationCallbacks *allocator,
	VkDeviceMemory *result)
{
	struct test_memory *memory;

	/* The fixture accepts only its enumerated host-coherent memory type. */
	assert(gpu == (VkDevice)&device);
	assert(info->memoryTypeIndex == 0U);
	memory = (struct test_memory *)test_object(sizeof(*memory), VULKAN_OBJECT_DEVICE_MEMORY, allocator);
	if (memory == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	memory->pixels = calloc(1U, (size_t)info->allocationSize);
	assert(memory->pixels != NULL);
	memory->memory.bytes = info->allocationSize;
	*result = (VkDeviceMemory)vulkan_nondispatchable_handle(&memory->memory.object);

	/* Succeeded: this mock allocation is separate from every resource object. */
	return VK_SUCCESS;
}

/* Frees mock backing only when the real WSI releases its allocation lifetime. */
VKAPI_ATTR void VKAPI_CALL
vkFreeMemory(
	VkDevice gpu,
	VkDeviceMemory handle,
	const VkAllocationCallbacks *allocator)
{
	struct test_memory *memory;

	/* Host sanitizers detect any repeated free or shared-group use after retirement. */
	(void)allocator;
	assert(gpu == (VkDevice)&device);
	memory = (struct test_memory *)vulkan_memory(handle);
	free(memory->pixels);
	vulkan_object_free(&memory->memory.object);

	/* Succeeded: both the allocation object and its backing have retired. */
	return;
}

/* Returns the mock coherent view; this does not replace the real shared-mmap test. */
VKAPI_ATTR VkResult VKAPI_CALL
vkMapMemory(
	VkDevice gpu,
	VkDeviceMemory handle,
	VkDeviceSize offset,
	VkDeviceSize bytes,
	VkMemoryMapFlags flags,
	void **result)
{
	struct test_memory *memory;

	/* WSI maps once over the whole allocation and retains it across frames. */
	assert(gpu == (VkDevice)&device);
	assert(offset == 0U);
	assert(bytes == VK_WHOLE_SIZE);
	assert(flags == 0U);
	memory = (struct test_memory *)vulkan_memory(handle);
	*result = memory->pixels;

	/* Succeeded: the actual WSI may read completed fixture copy bytes. */
	return VK_SUCCESS;
}

/* Confirms explicit mapped-view retirement before memory is freed. */
VKAPI_ATTR void VKAPI_CALL
vkUnmapMemory(
	VkDevice gpu,
	VkDeviceMemory handle)
{
	/* The allocation object still exists when its view is removed. */
	assert(gpu == (VkDevice)&device);
	assert(vulkan_memory(handle) != NULL);

	/* Succeeded: fixture backing remains owned until vkFreeMemory. */
	return;
}

/* Creates a mock transient command pool with ordinary allocator ownership. */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateCommandPool(
	VkDevice gpu,
	const VkCommandPoolCreateInfo *info,
	const VkAllocationCallbacks *allocator,
	VkCommandPool *result)
{
	struct test_pool *pool;

	/* Each presentation must use the actual presenting queue's family. */
	assert(gpu == (VkDevice)&device);
	assert(info->queueFamilyIndex == queue.family);
	pool = (struct test_pool *)test_object(sizeof(*pool), VULKAN_OBJECT_COMMAND_POOL, allocator);
	if (pool == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	*result = (VkCommandPool)vulkan_nondispatchable_handle(&pool->object);

	/* Succeeded: command-buffer lifetime belongs to this pool. */
	return VK_SUCCESS;
}

/* Releases the command buffer with its owning pool after presentation completes. */
VKAPI_ATTR void VKAPI_CALL
vkDestroyCommandPool(
	VkDevice gpu,
	VkCommandPool handle,
	const VkAllocationCallbacks *allocator)
{
	struct test_pool *pool;

	/* Partial recording failure must reclaim both pool and any allocated buffer. */
	(void)allocator;
	assert(gpu == (VkDevice)&device);
	pool = (struct test_pool *)vulkan_nondispatchable_object((uint64_t)handle);
	if (pool->command != NULL)
		vulkan_object_free(&pool->command->object);
	vulkan_object_free(&pool->object);

	/* Succeeded: no transient command storage survives its presentation job. */
	return;
}

/* Creates one actual command-buffer object for the real WSI's recorded copy batch. */
VKAPI_ATTR VkResult VKAPI_CALL
vkAllocateCommandBuffers(
	VkDevice gpu,
	const VkCommandBufferAllocateInfo *info,
	VkCommandBuffer *result)
{
	struct test_pool *pool;
	const VkAllocationCallbacks *allocator;

	/* The fixture requires exactly one command buffer for the complete present call. */
	assert(gpu == (VkDevice)&device);
	assert(info->commandBufferCount == 1U);
	pool = (struct test_pool *)vulkan_nondispatchable_object((uint64_t)info->commandPool);
	allocator = NULL;
	if (pool->object.allocator.has_callbacks != VK_FALSE)
		allocator = &pool->object.allocator.callbacks;
	pool->command = (struct VkCommandBuffer_T *)test_object(sizeof(*pool->command),
	    VULKAN_OBJECT_COMMAND_BUFFER, allocator);
	if (pool->command == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	*result = (VkCommandBuffer)pool->command;

	/* Succeeded: command recording can exercise the actual WSI barrier sequence. */
	return VK_SUCCESS;
}

/* Accepts the complete one-time recording contract used by present jobs. */
VKAPI_ATTR VkResult VKAPI_CALL
vkBeginCommandBuffer(
	VkCommandBuffer command,
	const VkCommandBufferBeginInfo *info)
{
	/* Recording begins before any semaphore-consuming queue operation. */
	assert(command != VK_NULL_HANDLE);
	assert(info->flags == VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

	/* Succeeded: the mock encoder accepts following copy commands. */
	return VK_SUCCESS;
}

/* Accepts a fully recorded mock command buffer. */
VKAPI_ATTR VkResult VKAPI_CALL
vkEndCommandBuffer(
	VkCommandBuffer command)
{
	/* Recording must refer to the actual allocated command buffer. */
	assert(command != VK_NULL_HANDLE);

	/* Succeeded: the real WSI can submit its complete transaction. */
	return VK_SUCCESS;
}

/* Verifies read-only image transitions and explicit host-read availability. */
VKAPI_ATTR void VKAPI_CALL
vkCmdPipelineBarrier(
	VkCommandBuffer command,
	VkPipelineStageFlags source_stage,
	VkPipelineStageFlags target_stage,
	VkDependencyFlags flags,
	uint32_t memory_count,
	const VkMemoryBarrier *memories,
	uint32_t buffer_count,
	const VkBufferMemoryBarrier *buffers,
	uint32_t image_count,
	const VkImageMemoryBarrier *images)
{
	/* Both barriers preserve queue-family ownership and the same presentable image. */
	(void)source_stage;
	(void)target_stage;
	assert(command != VK_NULL_HANDLE);
	assert(flags == 0U);
	assert(memory_count == 0U);
	assert(memories == NULL);
	assert(image_count == 1U);
	assert(images[0].srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
	assert(images[0].dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
	if (buffer_count == 0U) {
		assert(images[0].oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
		assert(images[0].newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	} else {
		assert(buffer_count == 1U);
		assert(images[0].newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
		assert(buffers[0].dstAccessMask == VK_ACCESS_HOST_READ_BIT);
	}

	/* Succeeded: presentation records a read-only copy with restored image layout. */
	return;
}

/* Simulates completed pixels only for testing native transport and lifetime boundaries. */
VKAPI_ATTR void VKAPI_CALL
vkCmdCopyImageToBuffer(
	VkCommandBuffer command,
	VkImage image,
	VkImageLayout layout,
	VkBuffer handle,
	uint32_t count,
	const VkBufferImageCopy *regions)
{
	struct test_buffer *buffer;

	/* Geometry and image selection come from the actual acquired swapchain index. */
	assert(command != VK_NULL_HANDLE);
	assert(vulkan_image(image) != NULL);
	assert(layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	assert(count == 1U);
	assert(regions[0].imageExtent.width == 320U);
	assert(regions[0].imageExtent.height == 240U);
	buffer = (struct test_buffer *)vulkan_nondispatchable_object((uint64_t)handle);
	memset(buffer->memory->pixels, 0x5a, (size_t)buffer->bytes);
	copies++;

	/* Succeeded: a mock copy supplies a recognizable completed native frame. */
	return;
}

/* Creates a counted ordinary fence for the actual WSI completion boundary. */
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateFence(
	VkDevice gpu,
	const VkFenceCreateInfo *info,
	const VkAllocationCallbacks *allocator,
	VkFence *result)
{
	struct vulkan_object *object;

	/* The presentation fence begins unsignaled before its one queue submission. */
	assert(gpu == (VkDevice)&device);
	assert(info->flags == 0U);
	object = test_object(sizeof(*object), VULKAN_OBJECT_FENCE, allocator);
	if (object == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	*result = (VkFence)vulkan_nondispatchable_handle(object);

	/* Succeeded: the actual job owns this fence until completion or rollback. */
	return VK_SUCCESS;
}

/* Retires a presentation fence through the same saved allocation policy. */
VKAPI_ATTR void VKAPI_CALL
vkDestroyFence(
	VkDevice gpu,
	VkFence handle,
	const VkAllocationCallbacks *allocator)
{
	/* Destruction must not outlive the device's common object ownership. */
	(void)allocator;
	assert(gpu == (VkDevice)&device);
	vulkan_object_free(vulkan_nondispatchable_object((uint64_t)handle));

	/* Succeeded: the completed or unsubmitted job no longer retains its fence. */
	return;
}

/* Counts application callback allocations and can fail one exact future call. */
static void *
test_allocate(
	void *user,
	size_t bytes,
	size_t alignment,
	VkSystemAllocationScope scope)
{
	void *pointer;

	/* All tested object alignments fit the host malloc alignment. */
	(void)user;
	(void)scope;
	assert(alignment <= 16U);
	allocation_attempts++;
	if (fail_allocation != 0U && allocation_attempts == fail_allocation)
		return NULL;
	pointer = malloc(bytes);
	if (pointer != NULL)
		allocations++;

	/* Succeeded or returned the explicit allocation failure requested by this test. */
	return pointer;
}

/* Verifies callback-backed allocations are returned exactly once. */
static void
test_free(
	void *user,
	void *pointer)
{
	/* NULL allocations are filtered by the real common allocator. */
	(void)user;
	assert(pointer != NULL);
	releases++;
	free(pointer);

	/* Succeeded: the counted allocation has returned to its original owner. */
	return;
}

/* Creates mock Vulkan objects using the actual library allocator and parent list. */
static struct vulkan_object *
test_object(
	size_t bytes,
	enum vulkan_object_kind kind,
	const VkAllocationCallbacks *allocator)
{
	struct vulkan_object *object;
	VkResult error;

	/* A failed callback must propagate through the real common object constructor. */
	error = vulkan_object_alloc(bytes, sizeof(void *), kind, &device.object,
	    NULL, allocator, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, &object);
	if (error != VK_SUCCESS)
		return NULL;
	error = vulkan_object_publish(object);
	assert(error == VK_SUCCESS);

	/* Succeeded: real teardown observes this mock rendering object as a normal child. */
	return object;
}

/* Describes a full-output mock surface with dynamic image count support. */
static VkResult
native_capabilities(
	struct vulkan_surface *surface,
	struct VkPhysicalDevice_T *gpu,
	VkSurfaceCapabilitiesKHR *caps)
{
	/* Surface validity is distinct from whether a replacement chain retired its old chain. */
	assert(gpu == &physical);
	if (surface->display_mode->generation != query_generation)
		return VK_ERROR_OUT_OF_DATE_KHR;
	memset(caps, 0, sizeof(*caps));
	caps->minImageCount = 2U;
	caps->currentExtent = surface->display_info.imageExtent;
	caps->minImageExtent = caps->currentExtent;
	caps->maxImageExtent = caps->currentExtent;
	caps->maxImageArrayLayers = 1U;
	caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	caps->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	/* Succeeded: no native reservation was created by this query. */
	return VK_SUCCESS;
}

/* Enumerates the sole test format through standard count and truncation semantics. */
static VkResult
native_formats(
	struct vulkan_surface *surface,
	struct VkPhysicalDevice_T *gpu,
	uint32_t *count,
	VkSurfaceFormatKHR *formats)
{
	/* Native capability functions never consume or modify acquisition state. */
	(void)surface;
	assert(gpu == &physical);
	if (formats == NULL) {
		*count = 1U;
		return VK_SUCCESS;
	}
	if (*count == 0U)
		return VK_INCOMPLETE;
	formats[0].format = VK_FORMAT_R8G8B8A8_UNORM;
	formats[0].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	*count = 1U;

	/* Succeeded: the actual WSI sees one supported packed format. */
	return VK_SUCCESS;
}

/* Advertises the mandatory FIFO present mode in the mock native boundary. */
static VkResult
native_modes(
	struct vulkan_surface *surface,
	struct VkPhysicalDevice_T *gpu,
	uint32_t *count,
	VkPresentModeKHR *modes)
{
	/* FIFO behavior itself belongs to the separately tested native display engine. */
	(void)surface;
	assert(gpu == &physical);
	if (modes == NULL) {
		*count = 1U;
		return VK_SUCCESS;
	}
	if (*count == 0U)
		return VK_INCOMPLETE;
	modes[0] = VK_PRESENT_MODE_FIFO_KHR;
	*count = 1U;

	/* Succeeded: standard enumeration has exactly one available present mode. */
	return VK_SUCCESS;
}

/* Shares one mock lease across replacement chains and injects native claim failure. */
static VkResult
native_claim(
	struct vulkan_surface *surface,
	struct VkDevice_T *gpu,
	void **result)
{
	struct test_lease *lease;

	/* A failed replacement must still leave the real old chain retired. */
	assert(gpu == &device);
	if (fail_claim != 0U)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;
	lease = surface->platform_private;
	if (lease == NULL) {
		lease = calloc(1U, sizeof(*lease));
		assert(lease != NULL);
		lease->surface = surface;
		surface->platform_private = lease;
		native_claims++;
	}
	lease->references++;
	*result = lease;

	/* Succeeded: the fixture tracks the actual WSI native-lifetime reference count. */
	return VK_SUCCESS;
}

/* Retires a mock native reservation only after the final borrowing chain disappears. */
static VkResult
native_release(
	void *pointer)
{
	struct test_lease *lease;

	/* Surviving replacement chains continue using their existing native lease. */
	lease = pointer;
	assert(lease->references != 0U);
	lease->references--;
	if (lease->references == 0U) {
		lease->surface->platform_private = NULL;
		free(lease);
		native_releases++;
	}

	/* Succeeded: one swapchain's native ownership reference has retired. */
	return VK_SUCCESS;
}

/* Checks the actual WSI's completed native pixel handoff and sequence publication. */
static VkResult
native_present(
	void *pointer,
	const struct vulkan_wsi_pixels *pixels,
	uint64_t *sequence)
{
	const uint8_t *bytes;

	/* Native rejection follows the already-consumed Vulkan semaphore transaction. */
	assert(pointer != NULL);
	if (fail_native != 0U)
		return VK_ERROR_OUT_OF_DATE_KHR;
	assert(pixels->extent.width == 320U);
	assert(pixels->extent.height == 240U);
	assert(pixels->stride == 1280U);
	assert(pixels->format == VK_FORMAT_R8G8B8A8_UNORM);
	bytes = pixels->pixels;
	assert(bytes[0] == 0x5aU);
	assert(bytes[320U * 240U * 4U - 1U] == 0x5aU);
	native_presents++;
	*sequence = native_presents;

	/* Succeeded: this mock display has consumed the complete copied frame. */
	return VK_SUCCESS;
}

/* Completes a previously returned mock native sequence. */
static VkResult
native_wait(
	void *pointer,
	uint64_t sequence,
	uint64_t timeout)
{
	/* Completion observation cannot create a future sequence or reserve a new output. */
	(void)timeout;
	assert(pointer != NULL);
	assert(sequence <= native_presents);

	/* Succeeded: the mock native display owns no borrowed pixels. */
	return VK_SUCCESS;
}

/* Exercises count-only, truncation, immutable names and implicit instance ownership. */
static void
test_discovery(
	VkDisplayKHR *displays)
{
	VkDisplayPropertiesKHR properties[40];
	VkDisplayPlanePropertiesKHR planes[40];
	VkDisplayModePropertiesKHR modes[3];
	VkDisplayKHR first;
	const char *name;
	VkResult error;
	uint32_t count;
	uint32_t index;
	unsigned before;

	/* The real implementation must enumerate beyond historical small fixed arrays. */
	count = 0U;
	error = vkGetPhysicalDeviceDisplayPropertiesKHR((VkPhysicalDevice)&physical, &count, NULL);
	assert(error == VK_SUCCESS && count == 40U);
	count = 3U;
	error = vkGetPhysicalDeviceDisplayPropertiesKHR((VkPhysicalDevice)&physical, &count, properties);
	assert(error == VK_INCOMPLETE && count == 3U);
	first = properties[0].display;
	name = properties[0].displayName;
	assert(strcmp(name, "display-0-generation-1") == 0);
	count = 40U;
	error = vkGetPhysicalDeviceDisplayPropertiesKHR((VkPhysicalDevice)&physical, &count, properties);
	assert(error == VK_SUCCESS && count == 40U);
	assert(properties[0].display == first);
	for (index = 0U; index < 40U; index++)
		displays[index] = properties[index].display;
	count = 40U;
	error = vkGetPhysicalDeviceDisplayPlanePropertiesKHR((VkPhysicalDevice)&physical, &count, planes);
	assert(error == VK_SUCCESS && count == 40U);

	/* DisplayName retains instance lifetime even when native names and modes change. */
	query_generation = 2U;
	count = 40U;
	error = vkGetPhysicalDeviceDisplayPropertiesKHR((VkPhysicalDevice)&physical, &count, properties);
	assert(error == VK_SUCCESS);
	assert(properties[0].display == first);
	assert(properties[0].displayName == name);
	assert(strcmp(name, "display-0-generation-1") == 0);

	/* Implicit mode descriptions use the saved instance allocator and stable handles. */
	before = allocations;
	count = 3U;
	error = vkGetDisplayModePropertiesKHR((VkPhysicalDevice)&physical, first, &count, modes);
	assert(error == VK_SUCCESS && count == 3U);
	assert(allocations >= before + 3U);

	/* Discovery APIs normalize native surface failure to their allowed result set. */
	query_error = VK_ERROR_SURFACE_LOST_KHR;
	count = 0U;
	error = vkGetPhysicalDeviceDisplayPropertiesKHR((VkPhysicalDevice)&physical, &count, NULL);
	assert(error == VK_ERROR_UNKNOWN);
	query_error = VK_SUCCESS;

	/* Succeeded: inventory and mode identities remain independently owned and stable. */
	return;
}

/* Constructs a surface from real mode and plane objects without reserving the display. */
static VkSurfaceKHR
test_surface(
	uint32_t index,
	VkDisplayKHR display)
{
	VkDisplayModePropertiesKHR modes[3];
	VkDisplaySurfaceCreateInfoKHR create;
	VkSurfaceKHR surface;
	VkResult error;
	uint32_t count;
	unsigned before;

	/* The first enumerated mode matches the finite test renderer geometry. */
	count = 3U;
	error = vkGetDisplayModePropertiesKHR((VkPhysicalDevice)&physical, display, &count, modes);
	assert(error == VK_SUCCESS);
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
	create.displayMode = modes[0].displayMode;
	create.planeIndex = index;
	create.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	create.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
	create.globalAlpha = 1.0f;
	create.imageExtent.width = 320U;
	create.imageExtent.height = 240U;
	before = native_claims;
	error = vkCreateDisplayPlaneSurfaceKHR((VkInstance)&instance, &create, &callbacks, &surface);
	assert(error == VK_SUCCESS);
	assert(native_claims == before);

	/* Succeeded: the instance owns a description without claiming a native plane. */
	return surface;
}

/* Builds a standard finite mode request with a freely chosen dynamic image count. */
static VkSwapchainCreateInfoKHR
test_create_info(
	VkSurfaceKHR surface,
	uint32_t count)
{
	VkSwapchainCreateInfoKHR create;

	/* The application requests only standard Vulkan surface and image properties. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	create.surface = surface;
	create.minImageCount = count;
	create.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
	create.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	create.imageExtent.width = 320U;
	create.imageExtent.height = 240U;
	create.imageArrayLayers = 1U;
	create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	create.presentMode = VK_PRESENT_MODE_FIFO_KHR;

	/* Succeeded: this request can exercise arbitrary supported image counts. */
	return create;
}

/* Presents a previously acquired image through the real copy and completion path. */
static void
test_present(
	VkSwapchainKHR chain,
	uint32_t index)
{
	VkPresentInfoKHR info;
	VkSemaphore waits[2];
	VkResult per_chain;
	VkResult error;
	unsigned before;

	/* Multiple wait semaphores must be consumed by one submission, not per copy. */
	waits[0] = (VkSemaphore)11U;
	waits[1] = (VkSemaphore)12U;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	info.waitSemaphoreCount = 2U;
	info.pWaitSemaphores = waits;
	info.swapchainCount = 1U;
	info.pSwapchains = &chain;
	info.pImageIndices = &index;
	info.pResults = &per_chain;
	expected_waits = 2U;
	before = submissions;
	error = vkQueuePresentKHR((VkQueue)&queue, &info);
	assert(error == VK_SUCCESS);
	assert(per_chain == VK_SUCCESS);
	assert(submissions == before + 1U);

	/* Succeeded: the real WSI recorded, submitted, waited and presented one frame. */
	return;
}

/* Exercises dynamic image ownership, timeout, enqueue rollback and failed replacement. */
static void
test_swapchains(
	VkSurfaceKHR first,
	VkSurfaceKHR second)
{
	VkSwapchainCreateInfoKHR create;
	VkSwapchainKHR chain;
	VkSwapchainKHR replacement;
	VkImage images[37];
	VkPresentInfoKHR present;
	VkResult error;
	uint32_t indices[37];
	uint32_t index;
	uint32_t count;
	uint32_t unused;
	unsigned before;

	/* Thirty-seven images exceed both earlier fixed kernel and driver array limits. */
	(void)second;
	create = test_create_info(first, 37U);
	error = vkCreateSwapchainKHR((VkDevice)&device, &create, &callbacks, &chain);
	assert(error == VK_SUCCESS);
	count = 37U;
	error = vkGetSwapchainImagesKHR((VkDevice)&device, chain, &count, images);
	assert(error == VK_SUCCESS && count == 37U);
	for (index = 0U; index < count; index++) {
		error = vkAcquireNextImageKHR((VkDevice)&device, chain, 0U,
		    VK_NULL_HANDLE, (VkFence)(uintptr_t)(index + 1U), &indices[index]);
		assert(error == VK_SUCCESS);
		assert(indices[index] == index);
	}
	error = vkAcquireNextImageKHR((VkDevice)&device, chain, 0U,
	    VK_NULL_HANDLE, (VkFence)99U, &unused);
	assert(error == VK_NOT_READY);
	error = vkAcquireNextImageKHR((VkDevice)&device, chain, 1000000U,
	    VK_NULL_HANDLE, (VkFence)99U, &unused);
	assert(error == VK_TIMEOUT);

	/* An allocation failure before enqueue must leave the acquired image usable. */
	memset(&present, 0, sizeof(present));
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.swapchainCount = 1U;
	present.pSwapchains = &chain;
	present.pImageIndices = &indices[0];
	expected_waits = 0U;
	fail_submit = 1U;
	before = submissions;
	error = vkQueuePresentKHR((VkQueue)&queue, &present);
	assert(error == VK_ERROR_OUT_OF_HOST_MEMORY);
	assert(submissions == before);
	fail_submit = 0U;
	test_present(chain, indices[0]);
	error = vkAcquireNextImageKHR((VkDevice)&device, chain, 0U,
	    VK_NULL_HANDLE, (VkFence)99U, &unused);
	assert(error == VK_SUCCESS && unused == indices[0]);

	/* Old-chain retirement occurs even when a new native claim fails. */
	create.oldSwapchain = chain;
	fail_claim = 1U;
	error = vkCreateSwapchainKHR((VkDevice)&device, &create, &callbacks, &replacement);
	assert(error == VK_ERROR_OUT_OF_DEVICE_MEMORY);
	assert(replacement == VK_NULL_HANDLE);
	fail_claim = 0U;
	error = vkAcquireNextImageKHR((VkDevice)&device, chain, 0U,
	    VK_NULL_HANDLE, (VkFence)99U, &unused);
	assert(error == VK_ERROR_OUT_OF_DATE_KHR);
	test_present(chain, indices[1]);
	vkDestroySwapchainKHR((VkDevice)&device, chain, &callbacks);

	/* Succeeded: acquired images survived failure without reopening retired acquisition. */
	return;
}

/* Verifies shared handles, surviving owners and all-or-nothing creation rollback. */
static void
test_shared(
	VkSurfaceKHR first,
	VkSurfaceKHR second)
{
	VkSwapchainCreateInfoKHR infos[2];
	VkSwapchainKHR chains[2];
	VkImage images[2][2];
	VkResult error;
	uint32_t count;
	uint32_t index;
	unsigned active_before;

	/* Both independent display surfaces must share the same two actual image handles. */
	infos[0] = test_create_info(first, 2U);
	infos[1] = test_create_info(second, 2U);
	error = vkCreateSharedSwapchainsKHR((VkDevice)&device, 2U, infos, &callbacks, chains);
	assert(error == VK_SUCCESS);
	count = 2U;
	error = vkGetSwapchainImagesKHR((VkDevice)&device, chains[0], &count, images[0]);
	assert(error == VK_SUCCESS);
	count = 2U;
	error = vkGetSwapchainImagesKHR((VkDevice)&device, chains[1], &count, images[1]);
	assert(error == VK_SUCCESS);
	assert(images[0][0] == images[1][0]);
	assert(images[0][1] == images[1][1]);

	/* Destroying one chain must preserve the surviving chain and shared image backing. */
	vkDestroySwapchainKHR((VkDevice)&device, chains[0], &callbacks);
	error = vkAcquireNextImageKHR((VkDevice)&device, chains[1], 0U,
	    VK_NULL_HANDLE, (VkFence)1U, &index);
	assert(error == VK_SUCCESS);
	test_present(chains[1], index);
	vkDestroySwapchainKHR((VkDevice)&device, chains[1], &callbacks);

	/* Every tested allocation failure must restore both native and host ownership counts. */
	for (index = 1U; index <= 24U; index++) {
		active_before = allocations - releases;
		fail_allocation = allocation_attempts + index;
		error = vkCreateSharedSwapchainsKHR((VkDevice)&device, 2U, infos, &callbacks, chains);
		fail_allocation = 0U;
		if (error == VK_SUCCESS) {
			vkDestroySwapchainKHR((VkDevice)&device, chains[0], &callbacks);
			vkDestroySwapchainKHR((VkDevice)&device, chains[1], &callbacks);
		} else {
			assert(error == VK_ERROR_OUT_OF_HOST_MEMORY);
			assert(chains[0] == VK_NULL_HANDLE && chains[1] == VK_NULL_HANDLE);
		}
		assert(allocations - releases == active_before);
	}

	/* Succeeded: partial creation never leaves a published chain or leaked image group. */
	return;
}
