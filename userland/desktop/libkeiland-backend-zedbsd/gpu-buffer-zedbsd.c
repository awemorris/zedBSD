/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Handles zedBSD's GPU buffer protocol and imports kernel image capabilities
 * (libkeiland-backend's GPU buffers, keiland-backend-gpu.h; WS131 p009).
 * The request retains its fd; Vulkan consumes a duplicate on a successful
 * dedicated import.  The compositor adopts the image and its memory through
 * the protocol host it lends.
 */

#include "userland/desktop/libkeiland-backend-zedbsd/gpu-zedbsd.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint32_t word_at(const unsigned char *bytes, size_t offset);
static int factory_fence(const struct kl_backend_protocol_host *host, struct kl_backend_resource *factory, const unsigned char *bytes, size_t size);
static int factory_alpha(const struct kl_backend_protocol_host *host, struct kl_backend_resource *factory, const unsigned char *bytes, size_t size);
static int buffer_import(const struct kl_backend_protocol_host *host, struct kl_backend_resource *buffer, const struct kwl_buffer_layout *layout, int descriptor);
static VkResult buffer_image(const struct kl_backend_gpu_device *device, const struct kwl_buffer_layout *image, int descriptor, VkImage *created, VkDeviceMemory *memory);
static void buffer_image_release(const struct kl_backend_gpu_device *device, VkImage *image, VkDeviceMemory *memory);

/*
 * Names the zedBSD GPU buffer global.
 */
const char *
kl_backend_gpu_global_interface(
	void)
{
	/* Succeeded: zedBSD clients share kernel image capabilities. */
	return "kl_gpu_buffer_v1";
}

/*
 * Reports the supported GPU buffer protocol version.
 */
uint32_t
kl_backend_gpu_global_version(
	void)
{
	/* Succeeded: revision three adds premultiplied alpha. */
	return 3U;
}

/*
 * Creates a GPU buffer or updates its alpha and acquire fences.
 */
int
kl_backend_gpu_request(
	const struct kl_backend_protocol_host *host,
	struct kl_backend_resource *factory,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	const struct kl_backend_gpu_device *device;
	struct kl_backend_resource *buffer;
	struct kwl_buffer_layout layout;
	struct kwl_gpu_limits limits;
	unsigned role;
	size_t wire_bytes;
	uint32_t id;
	uint32_t length;
	int descriptor;
	int error;

	/* zedBSD has no GPU protocol objects beyond the factory and ordinary buffers. */
	role = host->resource_role(factory);
	if (role == KL_BACKEND_ROLE_GPU_OBJECT)
		return EPROTO;

	/* Destroying a binding does not destroy buffers it previously created. */
	if (opcode == 0 && size == 0) {
		host->resource_destroy(factory);
		return 0;
	}

	/* Revision two: the acquire fence of a surface's next commit. */
	if (opcode == 2U) {
		error = factory_fence(host, factory, bytes, size);
		if (error != 0)
			return error;

		/* Succeeded: the buffer protocol update is applied. */
		return 0;
	}

	/* Revision three: how a buffer's alpha is read. */
	if (opcode == 3U) {
		error = factory_alpha(host, factory, bytes, size);
		if (error != 0)
			return error;

		/* Succeeded: the buffer protocol update is applied. */
		return 0;
	}

	/* The nha signature has new_id and array bytes; h contributes no wire word. */
	wire_bytes = kwl_gpu_buffer_wire_bytes();
	if (opcode != 1U || size != 8U + wire_bytes)
		return EPROTO;

	/* The array describes one complete immutable image record. */
	length = word_at(bytes, 4);
	if (length != wire_bytes)
		return EPROTO;

	/* Consume the fd only after the complete byte payload has passed framing checks. */
	descriptor = host->take_fd(factory);
	if (descriptor < 0)
		return EAGAIN;

	/* Creation failure still closes the request-owned descriptor immediately. */
	id = word_at(bytes, 0);
	buffer = host->resource_create(factory, id, KL_BACKEND_ROLE_BUFFER, 1);
	if (buffer == NULL) {
		close(descriptor);
		return EPROTO;
	}

	/* The compositor's device; without one there is nothing to import into. */
	device = host->device(factory);
	if (device == NULL) {
		close(descriptor);
		host->resource_destroy(buffer);
		return EPROTO;
	}

	/* The description's values, each checked before any reaches Vulkan (gpu-zedbsd.c). */
	limits.max_dimension = device->max_dimension;
	limits.memory_type_count = device->memory_type_count;
	error = kwl_gpu_buffer_decode(bytes + 8U, length, &limits, &layout);

	/*
	 * Window mode's Vulkan image, made once for the buffer's lifetime (design
	 * D2).  Its memory is imported for that image alone, and libvulkan checks
	 * the description against the kernel's record of the fd (WS103).
	 */
	if (error == 0)
		error = buffer_import(host, buffer, &layout, descriptor);

	/* Closes the request-owned descriptor before reporting an import failure. */
	close(descriptor);
	if (error != 0) {
		printf("KWL IMPORT_ERROR client=%llu buffer=%u errno=%d\n", (unsigned long long)host->client_number(factory), host->resource_id(buffer), error);
		host->resource_destroy(buffer);
		return EPROTO;
	}

	/* The machine log counts the imports (plan/ws099/tests/import-launch.sh reads the prefix). */
	printf("KWL IMPORT client=%llu buffer=%u width=%u height=%u bytes=%llu\n", (unsigned long long)host->client_number(factory), host->resource_id(buffer), layout.width, layout.height, (unsigned long long)layout.allocation_bytes);

	/* Succeeded: the wl_buffer owns its independently imported resource. */
	return 0;
}

/*
 * Keeps the explicit acquire fences supplied by zedBSD clients.
 */
void
kl_backend_gpu_commit(
	const struct kl_backend_protocol_host *host,
	struct kl_backend_resource *surface,
	struct kl_backend_resource *buffer)
{
	/* zedBSD fences arrive with set_acquire_fence before the commit. */
	(void)host;
	(void)surface;
	(void)buffer;

	/* Succeeded: the explicit zedBSD fences remain available to the common commit. */
	return;
}

/*
 * Reports the additional instance extensions needed by zedBSD.
 */
uint32_t
kl_backend_gpu_instance_extensions(
	const char **names,
	uint32_t capacity)
{
	/* The common instance extensions already cover zedBSD. */
	(void)names;
	(void)capacity;

	/* Succeeded: no additional instance extension is needed. */
	return 0U;
}

/*
 * Reports the additional device extensions needed by zedBSD.
 */
uint32_t
kl_backend_gpu_device_extensions(
	VkPhysicalDevice physical,
	const char **names,
	uint32_t capacity)
{
	/* The common device extensions already cover zedBSD. */
	(void)physical;
	(void)names;
	(void)capacity;

	/* Succeeded: no additional device extension is needed. */
	return 0U;
}

/*
 * Reports how zedBSD exports the compositor frame fence.
 */
VkExternalFenceHandleTypeFlagBits
kl_backend_gpu_frame_fence_type(
	void)
{
	/* Succeeded: an OPAQUE_FD fence can be polled without resetting it. */
	return VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
}

/*
 * Preserves zedBSD's factory protocol without initial format events.
 */
int
kl_backend_gpu_bind(
	const struct kl_backend_protocol_host *host,
	struct kl_backend_resource *factory)
{
	/* zedBSD clients know the kernel image description without a format snapshot. */
	(void)host;
	(void)factory;

	/* Succeeded: this factory needs no bind event. */
	return 0;
}

/*
 * Retires a buffer's backend state: zedBSD keeps none beside the
 * compositor's image.
 */
void
kl_backend_gpu_resource_free(
	const struct kl_backend_protocol_host *host,
	struct kl_backend_resource *resource)
{
	/* Nothing of the buffer is the backend's. */
	(void)host;
	(void)resource;
}

/* Takes a surface's next acquire fence and its nonzero generation. */
static int
factory_fence(
	const struct kl_backend_protocol_host *host,
	struct kl_backend_resource *factory,
	const unsigned char *bytes,
	size_t size)
{
	struct kl_backend_resource *surface;
	uint64_t generation;
	int descriptor;
	int full;
	uint32_t version;
	int logging;
	int error;

	/* The surface and the generation in two words; the fd beside them. */
	version = host->resource_version(factory);
	if (version < 2U || size != 12U)
		return EPROTO;

	/* Waits until the request-owned acquire-fence descriptor has arrived. */
	descriptor = host->take_fd(factory);
	if (descriptor < 0)
		return EAGAIN;

	/* The surface must be the client's own, with room for another fence. */
	surface = host->resource_find(factory, word_at(bytes, 0), KL_BACKEND_ROLE_SURFACE);
	if (surface == NULL) {
		close(descriptor);
		return EPROTO;
	}

	/* A surface with as many fences as a commit takes refuses another. */
	full = host->surface_fence_full(surface);
	if (full) {
		close(descriptor);
		return EPROTO;
	}

	/* The generation must be a real one (a fence's first is 1); readiness is the fd's own. */
	generation = ((uint64_t)word_at(bytes, 4) << 32) | word_at(bytes, 8);
	if (generation == 0) {
		close(descriptor);
		return EPROTO;
	}

	/* The fence joins the others of the next commit. */
	error = host->surface_fence(surface, descriptor, generation);
	if (error != 0) {
		close(descriptor);
		return EPROTO;
	}

	/* Names the fence when the per-frame lines were asked for (a present's own fence is at its first generation, ws103-p005). */
	logging = host->log_frames(factory);
	if (logging)
		printf("KWL ACQUIRE_FENCE client=%llu surface=%u generation=%llu\n", (unsigned long long)host->client_number(factory), host->resource_id(surface), (unsigned long long)generation);

	/* Succeeded: the next commit waits for this fence too. */
	return 0;
}

/* Sets whether an imported GPU buffer is opaque or premultiplied alpha. */
static int
factory_alpha(
	const struct kl_backend_protocol_host *host,
	struct kl_backend_resource *factory,
	const unsigned char *bytes,
	size_t size)
{
	struct kl_backend_resource *buffer;
	uint32_t alpha;
	uint32_t version;

	/* The buffer and the alpha in two words. */
	version = host->resource_version(factory);
	if (version < 3U || size != 8U)
		return EPROTO;

	/* Refuses a drawing mode outside opaque and premultiplied alpha. */
	alpha = word_at(bytes, 4);
	if (alpha > 1U)
		return EPROTO;

	/* The buffer must be one of the client's GPU buffers. */
	buffer = host->resource_find(factory, word_at(bytes, 0), KL_BACKEND_ROLE_BUFFER);
	if (buffer == NULL)
		return EPROTO;

	/* The window's drawing blends the buffer by its alpha, or covers what is under it; the next frame shows it. */
	host->buffer_set_alpha(buffer, alpha);

	/* Succeeded: the next frame uses the requested buffer blending mode. */
	return 0;
}

/* Imports a copy of the buffer fd and gives its image to the compositor. */
static int
buffer_import(
	const struct kl_backend_protocol_host *host,
	struct kl_backend_resource *buffer,
	const struct kwl_buffer_layout *layout,
	int descriptor)
{
	const struct kl_backend_gpu_device *device;
	VkImage image;
	VkDeviceMemory memory;
	int copy;
	int logging;
	VkResult status;

	/* The compositor's Vulkan device the image is imported into. */
	device = host->device(buffer);
	if (device == NULL)
		return EINVAL;

	/* Vulkan consumes the fd it imports, so it gets its own. */
	copy = dup(descriptor);
	if (copy < 0)
		return errno;

	/* The image bound to the imported memory (the copy is consumed or closed). */
	status = buffer_image(device, layout, copy, &image, &memory);
	if (status != VK_SUCCESS) {
		printf("KWL VULKAN_IMPORT_ERROR client=%llu buffer=%u result=%d\n", (unsigned long long)host->client_number(buffer), host->resource_id(buffer), (int)status);
		return EINVAL;
	}

	/* Its view and descriptor sets and the buffer's import record (the compositor's); the image and memory go on failure. */
	status = host->buffer_adopt(buffer, image, memory, layout->width, layout->height, layout->format);
	if (status != VK_SUCCESS) {
		printf("KWL VULKAN_IMPORT_ERROR client=%llu buffer=%u result=%d\n", (unsigned long long)host->client_number(buffer), host->resource_id(buffer), (int)status);
		return EINVAL;
	}

	/* The per-frame log names the import. */
	logging = host->log_frames(buffer);
	if (logging)
		printf("KWL VULKAN_IMPORT client=%llu buffer=%u width=%u height=%u\n", (unsigned long long)host->client_number(buffer), host->resource_id(buffer), layout->width, layout->height);

	/* Succeeded: the buffer can be drawn in window mode. */
	return 0;
}

/* Creates a linear image and binds the imported fd as dedicated memory. */
static VkResult
buffer_image(
	const struct kl_backend_gpu_device *device,
	const struct kwl_buffer_layout *image,
	int descriptor,
	VkImage *created,
	VkDeviceMemory *memory)
{
	VkExternalMemoryImageCreateInfo external;
	VkImageCreateInfo create;
	VkMemoryRequirements requirements;
	VkImageSubresource subresource;
	VkSubresourceLayout layout;
	VkMemoryDedicatedAllocateInfo dedicated;
	VkImportMemoryFdInfoKHR import_info;
	VkMemoryAllocateInfo allocate;
	VkFormat format;
	VkResult status;

	/* No Vulkan objects exist until their creation succeeds. */
	*created = VK_NULL_HANDLE;
	*memory = VK_NULL_HANDLE;

	/* The channel order is the client's (a linear four-channel format, checked by kwl_gpu_buffer_decode). */
	format = image->format;

	/* The image, sampled, with external memory. */
	memset(&external, 0, sizeof(external));
	external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	external.handleTypes = kwl_gpu_buffer_handle_type();

	/* Describe the linear image whose dedicated allocation is imported. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.pNext = &external;
	create.imageType = VK_IMAGE_TYPE_2D;
	create.format = format;
	create.extent.width = image->width;
	create.extent.height = image->height;
	create.extent.depth = 1U;
	create.mipLevels = 1U;
	create.arrayLayers = 1U;
	create.samples = VK_SAMPLE_COUNT_1_BIT;
	create.tiling = VK_IMAGE_TILING_LINEAR;
	create.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	/* Creates the sampled image before any dedicated memory is imported. */
	status = vkCreateImage(device->device, &create, NULL, created);
	if (status != VK_SUCCESS) {
		close(descriptor);
		buffer_image_release(device, created, memory);
		return status;
	}

	/* The client's layout must be the one this image has: same memory type, rows and offset. */
	vkGetImageMemoryRequirements(device->device, *created, &requirements);

	/* Selects the color plane whose rows must match the client description. */
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

	/* Queries the image layout and refuses a mismatched client allocation. */
	vkGetImageSubresourceLayout(device->device, *created, &subresource, &layout);
	if ((requirements.memoryTypeBits & (1U << image->memory_type)) == 0U ||
	    requirements.size > image->allocation_bytes ||
	    layout.offset != image->offset ||
	    layout.rowPitch != image->stride) {
		printf("KWL VULKAN_IMPORT_LAYOUT types=0x%x type=%u size=%llu bytes=%llu offset=%llu/%llu pitch=%llu/%u\n",
		       requirements.memoryTypeBits,
		       image->memory_type,
		       (unsigned long long)requirements.size,
		       (unsigned long long)image->allocation_bytes,
		       (unsigned long long)layout.offset,
		       (unsigned long long)image->offset,
		       (unsigned long long)layout.rowPitch,
		       image->stride);
		close(descriptor);
		buffer_image_release(device, created, memory);
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	}

	/*
	 * The memory is the client's allocation, imported through its fd
	 * (consumed on success) for this image alone: a dedicated import, which
	 * libvulkan checks against the kernel's record of the fd, so that the
	 * description the client sent cannot make this image read the allocation
	 * as another one.
	 */
	memset(&dedicated, 0, sizeof(dedicated));
	dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
	dedicated.image = *created;

	/* The duplicated fd supplies the allocation for that one image. */
	memset(&import_info, 0, sizeof(import_info));
	import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
	import_info.pNext = &dedicated;
	import_info.handleType = kwl_gpu_buffer_handle_type();
	import_info.fd = descriptor;

	/* Allocate the memory with the dedicated import chained to it. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.pNext = &import_info;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = image->memory_type;

	/* Imports dedicated memory and transfers its descriptor only on success. */
	status = vkAllocateMemory(device->device, &allocate, NULL, memory);
	if (status != VK_SUCCESS) {
		close(descriptor);
		buffer_image_release(device, created, memory);
		return status;
	}

	/* The image uses that memory. */
	status = vkBindImageMemory(device->device, *created, *memory, 0U);
	if (status != VK_SUCCESS) {
		buffer_image_release(device, created, memory);
		return status;
	}

	/* Succeeded: the image is bound to the client's memory. */
	return VK_SUCCESS;
}

/* Destroys what buffer_image made, whatever part of it was made, in import_release's order. */
static void
buffer_image_release(
	const struct kl_backend_gpu_device *device,
	VkImage *image,
	VkDeviceMemory *memory)
{
	/* The image, then its memory. */
	if (*image != VK_NULL_HANDLE)
		vkDestroyImage(device->device, *image, NULL);

	/* Releases memory after no image can reference it. */
	if (*memory != VK_NULL_HANDLE)
		vkFreeMemory(device->device, *memory, NULL);

	/* Leaves both output handles safe for another cleanup attempt. */
	*image = VK_NULL_HANDLE;
	*memory = VK_NULL_HANDLE;

	/* Succeeded: the imported image and memory handles are cleared. */
	return;
}

/* Reads one possibly unaligned native-endian protocol word. */
static uint32_t
word_at(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t word;

	/* Callers validate the containing payload before requesting a word. */
	memcpy(&word, bytes + offset, sizeof(word));

	/* Succeeded: return the decoded scalar without pointer-alignment assumptions. */
	return word;
}
