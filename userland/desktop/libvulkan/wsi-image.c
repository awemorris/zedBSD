/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Owns linear, exportable presentation images without requiring a CPU mapping.
 */

#include "wsi-internal.h"

#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/*
 * Creates one shared GPU image, a copy destination (transfer and
 * sampling), using renderer requirements and real row layout.
 */
VkResult
vulkan_wsi_shared_image_create(
	struct VkDevice_T *device,
	VkFormat format,
	VkExtent2D extent,
	const VkAllocationCallbacks *allocator,
	const struct gpu_placement *placement,
	VkImage *image,
	VkDeviceMemory *memory,
	int *fd,
	struct gpu_image_descriptor *descriptor)
{
	VkResult error;

	/* The copy destination's uses. */
	error = vulkan_wsi_shared_image_create_usage(device, format, extent, 0U, allocator, placement, image, memory, fd, descriptor);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the caller owns the image, its memory and the fd. */
	return VK_SUCCESS;
}

/*
 * Creates one shared GPU image using renderer requirements and real row layout.
 * The returned fd and Vulkan objects have separate, explicit ownership.
 * A usage of zero is the copy destination's (transfer and sampling); a
 * swapchain that renders into the shared image names its own.
 */
VkResult
vulkan_wsi_shared_image_create_usage(
	struct VkDevice_T *device,
	VkFormat format,
	VkExtent2D extent,
	VkImageUsageFlags usage,
	const VkAllocationCallbacks *allocator,
	const struct gpu_placement *placement,
	VkImage *image,
	VkDeviceMemory *memory,
	int *fd,
	struct gpu_image_descriptor *descriptor)
{
	VkResult error;

	/* Rows of pixels, the layout every device shares. */
	error = vulkan_wsi_shared_image_create_layout(device, format, extent, usage, GPU_IMAGE_LINEAR, 0U, allocator, placement, image, memory, fd, descriptor);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the caller owns the image, its memory and the fd. */
	return VK_SUCCESS;
}

/*
 * Chooses the layout of a shared image a display shows (a direct
 * swapchain's): the first of the renderer's layouts, fastest first, that it
 * draws to, samples and shares (GPU_IMAGE_LAYOUTS) and that the display's
 * answer (scanout, NULL for none) lists for the same image.  Linear rows,
 * which every device has, unless such a layout comes before them.  A device
 * that names no layouts of its own (no GPU_CAP_IMAGE_LAYOUTS) has only
 * linear rows.
 */
void
vulkan_wsi_layout_choose(
	struct VkDevice_T *device,
	VkFormat format,
	VkExtent2D extent,
	const struct gpu_image_layouts *scanout,
	uint32_t *tiling,
	uint32_t *parameter)
{
	struct gpu_image_layouts renderer;
	const struct gpu_image_layout *entry;
	uint32_t index;
	uint32_t shown;
	int status;

	/* Linear rows unless a faster layout both ends take is found. */
	*tiling = GPU_IMAGE_LINEAR;
	*parameter = 0U;
	if ((device->object.context->capabilities & GPU_CAP_IMAGE_LAYOUTS) == 0U || scanout == NULL)
		return;

	/* The renderer's layouts of the image that it draws to, samples and shares. */
	memset(&renderer, 0, sizeof(renderer));
	renderer.version = GPU_ABI_VERSION;
	renderer.size = sizeof(renderer);
	renderer.format = GPU_PIXEL_RGBA8888;
	if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB)
		renderer.format = GPU_PIXEL_BGRA8888;
	renderer.width = extent.width;
	renderer.height = extent.height;
	renderer.uses = GPU_LAYOUT_RENDER | GPU_LAYOUT_SAMPLE | GPU_LAYOUT_SHARE;
	vulkan_context_lock(device->object.context);

	status = ioctl(device->object.context->fd, GPU_IMAGE_LAYOUTS, &renderer);

	vulkan_context_unlock(device->object.context);

	/* A device that does not answer keeps linear rows. */
	if (status != 0 || renderer.count > GPU_IMAGE_LAYOUTS_MAX)
		return;

	/* The renderer's layouts, fastest first: the first the display shows too, or linear rows when they come first. */
	for (index = 0U; index < renderer.count; index++) {
		entry = &renderer.entry[index];
		if (entry->tiling == GPU_IMAGE_LINEAR)
			return;
		for (shown = 0U; shown < scanout->count && shown < GPU_IMAGE_LAYOUTS_MAX; shown++) {
			if (scanout->entry[shown].tiling != entry->tiling || scanout->entry[shown].parameter != entry->parameter)
				continue;
			if ((scanout->entry[shown].flags & GPU_LAYOUT_SCANOUT) == 0U)
				continue;

			/* Succeeded: both the renderer and the display take this faster layout. */
			*tiling = entry->tiling;
			*parameter = entry->parameter;
			return;
		}
	}
}

/*
 * Creates one shared GPU image in a layout (a GPU_IMAGE_* tiling and its
 * parameter): linear rows as a linear image, another layout as an optimal
 * image the renderer places so (the layout record chained after the
 * external-memory declaration).  The returned fd and Vulkan objects have
 * separate, explicit ownership.  A usage of zero is the copy destination's.
 */
VkResult
vulkan_wsi_shared_image_create_layout(
	struct VkDevice_T *device,
	VkFormat format,
	VkExtent2D extent,
	VkImageUsageFlags usage,
	uint32_t tiling,
	uint32_t parameter,
	const VkAllocationCallbacks *allocator,
	const struct gpu_placement *placement,
	VkImage *image,
	VkDeviceMemory *memory,
	int *fd,
	struct gpu_image_descriptor *descriptor)
{
	struct vulkan_external_image_info external;
	struct vulkan_image_layout_info placed;
	VkFormatFeatureFlags features;
	VkImageCreateInfo create;
	VkMemoryAllocateInfo allocate;
	VkExportMemoryAllocateInfo export;
	VkMemoryRequirements requirements;
	VkImageSubresource subresource;
	VkSubresourceLayout layout;
	VkFormatProperties properties;
	uint32_t type;
	VkResult error;

	/* Partial construction never transfers any of the three independent owners. */
	*image = VK_NULL_HANDLE;
	*memory = VK_NULL_HANDLE;
	*fd = -1;
	memset(descriptor, 0, sizeof(*descriptor));

	/* The native allocation capability must exist before renderer construction starts. */
	if ((device->object.context->capabilities & GPU_CAP_SHARE) == 0U)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* Another layout than linear rows only on a device that names its own. */
	if (tiling != GPU_IMAGE_LINEAR && (device->object.context->capabilities & GPU_CAP_IMAGE_LAYOUTS) == 0U)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* The image is sampled in its tiling. */
	vkGetPhysicalDeviceFormatProperties((VkPhysicalDevice)device->physical, format, &properties);
	features = properties.linearTilingFeatures;
	if (tiling != GPU_IMAGE_LINEAR)
		features = properties.optimalTilingFeatures;
	if ((features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0U)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* Only the renderer sees this external-memory declaration, never a guest dma-buf API. */
	memset(&external, 0, sizeof(external));
	external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	external.handle_types = VULKAN_EXTERNAL_MEMORY_DMABUF;
	if (device->object.context->xml_version == VK_MAKE_VERSION(1, 4, 343) &&
	    device->object.context->copy_display == VK_FALSE)
		external.handle_types = VULKAN_EXTERNAL_MEMORY_OPAQUE;

	/* Another layout than linear rows: the renderer places the optimal image so. */
	memset(&placed, 0, sizeof(placed));
	placed.sType = VULKAN_STRUCTURE_TYPE_IMAGE_LAYOUT;
	placed.tiling = tiling;
	placed.parameter = parameter;
	if (tiling != GPU_IMAGE_LINEAR)
		external.pNext = &placed;

	/* One progressive color image supports GPU copies and later renderer sampling. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.pNext = &external;
	create.imageType = VK_IMAGE_TYPE_2D;
	create.format = format;
	create.extent.width = extent.width;
	create.extent.height = extent.height;
	create.extent.depth = 1U;
	create.mipLevels = 1U;
	create.arrayLayers = 1U;
	create.samples = VK_SAMPLE_COUNT_1_BIT;
	create.tiling = VK_IMAGE_TILING_LINEAR;
	if (tiling != GPU_IMAGE_LINEAR)
		create.tiling = VK_IMAGE_TILING_OPTIMAL;
	create.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	    VK_IMAGE_USAGE_SAMPLED_BIT;
	if (usage != 0U)
		create.usage = usage;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	error = vkCreateImage((VkDevice)device, &create, allocator, image);
	if (error != VK_SUCCESS)
		return error;

	/* The allocation satisfies the renderer's actual image alignment and memory set. */
	memset(&requirements, 0, sizeof(requirements));
	vkGetImageMemoryRequirements((VkDevice)device, *image, &requirements);

	/* A paired Windows host maps native scanout memory for its display. */
	if (device->object.context->xml_version == VK_MAKE_VERSION(1, 4, 343) &&
	    device->object.context->copy_display == VK_FALSE) {
		for (type = 0U; type < device->physical->memory.memoryTypeCount; type++) {
			VkMemoryPropertyFlags flags =
			    device->physical->memory.memoryTypes[type].propertyFlags;
			if ((requirements.memoryTypeBits & (1U << type)) != 0U &&
			    (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
			    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
				break;
		}
		if (type == device->physical->memory.memoryTypeCount) {
			error = VK_ERROR_FEATURE_NOT_PRESENT;
			goto cleanup;
		}
		goto selected;
	}

	/* Select the first actual memory type compatible with this image's requirements. */
	for (type = 0U; type < device->physical->memory.memoryTypeCount; type++) {
		/* The renderer's compatibility mask determines which allocation type may be bound. */
		if ((requirements.memoryTypeBits & (1U << type)) != 0U)
			break;
	}

	/* No compatible memory type leaves the already-created image on the cleanup path. */
	if (type == device->physical->memory.memoryTypeCount) {
		error = VK_ERROR_OUT_OF_DEVICE_MEMORY;
		goto cleanup;
	}

selected:

	/* The shared allocation remains GPU-only even when its chosen heap also permits host access. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	if (device->object.context->xml_version == VK_MAKE_VERSION(1, 4, 343) &&
	    device->object.context->copy_display == VK_FALSE) {
		memset(&export, 0, sizeof(export));
		export.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
		export.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		allocate.pNext = &export;
	}
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	error = vulkan_memory_allocate_placed((VkDevice)device, &allocate, allocator, placement, memory);
	if (error != VK_SUCCESS)
		goto cleanup;

	/* Binding completes before any row layout or external capability is published. */
	error = vkBindImageMemory((VkDevice)device, *image, *memory, 0U);
	if (error != VK_SUCCESS)
		goto cleanup;

	/* The only plane, mip level and array layer define the exported image subresource. */
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

	/* Layout is queried; packed width is never substituted for renderer row pitch. */
	memset(&layout, 0, sizeof(layout));
	vkGetImageSubresourceLayout((VkDevice)device, *image, &subresource, &layout);
	if (layout.rowPitch > UINT32_MAX || layout.rowPitch < (uint64_t)extent.width * 4U) {
		error = VK_ERROR_FORMAT_NOT_SUPPORTED;
		goto cleanup;
	}

	/* Public metadata describes the complete linear image without exposing renderer pointers. */
	descriptor->version = GPU_ABI_VERSION;
	descriptor->size = sizeof(*descriptor);
	descriptor->width = extent.width;
	descriptor->height = extent.height;
	descriptor->format = GPU_PIXEL_RGBA8888;

	/* The alternate supported packed format changes only the declared channel order. */
	if (format == VK_FORMAT_B8G8R8A8_UNORM)
		descriptor->format = GPU_PIXEL_BGRA8888;

	/* Renderer layout, rather than application guesses, supplies stride and allocation offset. */
	descriptor->stride = (uint32_t)layout.rowPitch;
	descriptor->offset = layout.offset;
	descriptor->tiling = tiling;
	descriptor->layout_parameter = parameter;
	descriptor->usage = create.usage;
	error = vulkan_memory_image_fd(device, *memory, descriptor, fd);

cleanup:
	/* A failed export retires the bound image before its allocation. */
	if (error != VK_SUCCESS) {
		vkDestroyImage((VkDevice)device, *image, allocator);

		/* Allocation failure may have left only the image itself to retire. */
		if (*memory != VK_NULL_HANDLE)
			vkFreeMemory((VkDevice)device, *memory, allocator);

		/* Failed construction leaves no local Vulkan owner in either output slot. */
		*image = VK_NULL_HANDLE;
		*memory = VK_NULL_HANDLE;

		/* Preserve the construction failure after releasing every partially created owner. */
		return error;
	}

	/* Succeeded: the caller separately owns a bound image, its memory and an allocation fd. */
	return VK_SUCCESS;
}

/*
 * Imports the same allocation into another renderer context and binds a compatible image.
 * The fd remains caller-owned; successful memory creation owns the imported kernel alias.
 */
VkResult
vulkan_wsi_shared_image_import(
	struct VkDevice_T *device,
	int fd,
	const VkAllocationCallbacks *allocator,
	VkImage *image,
	VkDeviceMemory *memory,
	struct gpu_image_descriptor *descriptor)
{
	VkResult error;

	/* The image in the format of the allocation's own channel order. */
	error = vulkan_wsi_shared_image_import_format(device, fd, VK_FORMAT_UNDEFINED, allocator, image, memory, descriptor);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the caller owns the image and the imported memory. */
	return VK_SUCCESS;
}

/*
 * Exports the memory of an exportable direct swapchain's image (one made
 * with VkExternalMemoryImageCreateInfo) as a new image fd with the
 * description the image was made with: vkGetMemoryFdKHR's answer for a
 * swapchain image.  A compositor lends it to a client, whose WSI imports
 * it.  Returns VK_SUCCESS or VK_ERROR_INVALID_EXTERNAL_HANDLE.
 */
VkResult
vulkan_wsi_swapchain_image_fd(
	struct VkDevice_T *device,
	VkImage image,
	int *fd)
{
	struct gpu_image_descriptor descriptor;
	VkDeviceMemory memory;
	VkResult error;

	/* No fd until the export gives one. */
	*fd = -1;
	if (vulkan_wsi_swapchain_image_memory == NULL)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* The image's memory and description, found among the exportable chains. */
	error = vulkan_wsi_swapchain_image_memory(device, image, &memory, &descriptor);
	if (error != VK_SUCCESS)
		return error;

	/* The kernel stamps the device itself, and a repeated export carries the first one's description. */
	descriptor.device_id = 0U;
	error = vulkan_memory_image_fd(device, memory, &descriptor, fd);
	if (error != VK_SUCCESS)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* Succeeded: the caller owns an fd of the image. */
	return VK_SUCCESS;
}

/*
 * Imports a shared allocation as an image of a format: VK_FORMAT_UNDEFINED
 * takes the allocation's own channel order in UNORM, and another format
 * must keep that channel order (its UNORM or SRGB form, the same bytes).
 * The fd remains caller-owned; successful memory creation owns the
 * imported kernel alias.
 */
VkResult
vulkan_wsi_shared_image_import_format(
	struct VkDevice_T *device,
	int fd,
	VkFormat format,
	const VkAllocationCallbacks *allocator,
	VkImage *image,
	VkDeviceMemory *memory,
	struct gpu_image_descriptor *descriptor)
{
	struct gpu_resource_import request;
	struct vulkan_external_image_info external;
	struct vulkan_image_layout_info placed;
	VkImageCreateInfo create;
	VkMemoryAllocateInfo allocate;
	VkExportMemoryAllocateInfo export;
	VkMemoryRequirements requirements;
	VkImageSubresource subresource;
	VkSubresourceLayout layout;
	VkResult error;
	VkResult cleanup;
	int status;

	/* Failed construction transfers neither a Vulkan object nor unverified image metadata. */
	*image = VK_NULL_HANDLE;
	*memory = VK_NULL_HANDLE;
	memset(descriptor, 0, sizeof(*descriptor));

	/* The fd is the only image-authority input to the kernel's import operation. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.fd = fd;

	/* K supplies authoritative metadata and checks device identity before creating an alias. */
	vulkan_context_lock(device->object.context);

	status = ioctl(device->object.context->fd, GPU_RESOURCE_IMPORT, &request);

	vulkan_context_unlock(device->object.context);

	/* Import failure leaves the caller's original fd untouched. */
	if (status != 0)
		return VK_ERROR_INVALID_EXTERNAL_HANDLE;

	/* Linear rows, or another layout on a device that names its own (the kernel took it from this device's export). */
	error = VK_ERROR_FORMAT_NOT_SUPPORTED;
	if (request.image.tiling != GPU_IMAGE_LINEAR &&
	    (device->object.context->capabilities & GPU_CAP_IMAGE_LAYOUTS) == 0U)
		goto cleanup;

	/* Only the four-channel image family is supported. */
	if ((request.image.format != GPU_PIXEL_RGBA8888 &&
	     request.image.format != GPU_PIXEL_BGRA8888) ||
	    request.image.memory_type >= device->physical->memory.memoryTypeCount)
		goto cleanup;

	/* The receiver creates its own external-compatible image in a separate renderer namespace. */
	memset(&external, 0, sizeof(external));
	external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	external.handle_types = VULKAN_EXTERNAL_MEMORY_DMABUF;
	if (device->object.context->xml_version == VK_MAKE_VERSION(1, 4, 343) &&
	    device->object.context->copy_display == VK_FALSE)
		external.handle_types = VULKAN_EXTERNAL_MEMORY_OPAQUE;

	/* Another layout than linear rows: the renderer places the optimal image as the description says. */
	memset(&placed, 0, sizeof(placed));
	placed.sType = VULKAN_STRUCTURE_TYPE_IMAGE_LAYOUT;
	placed.tiling = request.image.tiling;
	placed.parameter = request.image.layout_parameter;
	if (request.image.tiling != GPU_IMAGE_LINEAR)
		external.pNext = &placed;

	/* Geometry and usage exactly reproduce the allocation's immutable exported description. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.pNext = &external;
	create.imageType = VK_IMAGE_TYPE_2D;
	create.format = VK_FORMAT_R8G8B8A8_UNORM;

	/* Channel ordering must agree with the original image rather than the receiving process. */
	if (request.image.format == GPU_PIXEL_BGRA8888)
		create.format = VK_FORMAT_B8G8R8A8_UNORM;

	/* A format asked for keeps the channel order: the same bytes, read as UNORM or SRGB. */
	if (format != VK_FORMAT_UNDEFINED) {
		error = VK_ERROR_FORMAT_NOT_SUPPORTED;
		if (create.format == VK_FORMAT_R8G8B8A8_UNORM &&
		    format != VK_FORMAT_R8G8B8A8_UNORM &&
		    format != VK_FORMAT_R8G8B8A8_SRGB)
			goto cleanup;
		if (create.format == VK_FORMAT_B8G8R8A8_UNORM &&
		    format != VK_FORMAT_B8G8R8A8_UNORM &&
		    format != VK_FORMAT_B8G8R8A8_SRGB)
			goto cleanup;
		create.format = format;
	}

	/* This independent image object will be bound to the imported allocation below. */
	create.extent.width = request.image.width;
	create.extent.height = request.image.height;
	create.extent.depth = 1U;
	create.mipLevels = 1U;
	create.arrayLayers = 1U;
	create.samples = VK_SAMPLE_COUNT_1_BIT;
	create.tiling = VK_IMAGE_TILING_LINEAR;
	if (request.image.tiling != GPU_IMAGE_LINEAR)
		create.tiling = VK_IMAGE_TILING_OPTIMAL;
	create.usage = request.image.usage;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	error = vkCreateImage((VkDevice)device, &create, allocator, image);
	if (error != VK_SUCCESS)
		goto cleanup;

	/* Receiving image requirements must permit the already-selected source memory type. */
	memset(&requirements, 0, sizeof(requirements));
	vkGetImageMemoryRequirements((VkDevice)device, *image, &requirements);

	/* Only the image's single color subresource participates in this shared allocation. */
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

	/* Import never silently changes the memory type, stride or allocation bounds. */
	memset(&layout, 0, sizeof(layout));
	vkGetImageSubresourceLayout((VkDevice)device, *image, &subresource, &layout);
	if ((requirements.memoryTypeBits & (1U << request.image.memory_type)) == 0U ||
	    requirements.size > request.image.allocation_bytes ||
	    (request.image.tiling == GPU_IMAGE_LINEAR &&
	     (layout.offset != request.image.offset ||
	      layout.rowPitch != request.image.stride))) {
		error = VK_ERROR_FORMAT_NOT_SUPPORTED;
		goto cleanup;
	}

	/* The renderer imports the attached resource without allocating a replacement pixel store. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	if (device->object.context->xml_version == VK_MAKE_VERSION(1, 4, 343) &&
	    device->object.context->copy_display == VK_FALSE) {
		memset(&export, 0, sizeof(export));
		export.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
		export.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		allocate.pNext = &export;
	}
	allocate.allocationSize = request.image.allocation_bytes;
	allocate.memoryTypeIndex = request.image.memory_type;
	error = vulkan_memory_import(
		device,
		&allocate,
		allocator,
		request.resource_id,
		request.handle,
		memory);
	if (error != VK_SUCCESS)
		goto cleanup;

	/* Successful memory import transfers alias cleanup from this function to vkFreeMemory. */
	request.handle = 0U;
	error = vkBindImageMemory((VkDevice)device, *image, *memory, 0U);
	if (error == VK_SUCCESS)
		*descriptor = request.image;

cleanup:
	/* Memory owns an alias only after successful import; other failures retain local rollback. */
	if (error != VK_SUCCESS) {
		/* An earlier image-creation failure may have produced no local Vulkan image. */
		if (*image != VK_NULL_HANDLE)
			vkDestroyImage((VkDevice)device, *image, allocator);

		/* A published memory object owns its imported alias even when binding subsequently fails. */
		if (*memory != VK_NULL_HANDLE)
			vkFreeMemory((VkDevice)device, *memory, allocator);

		/* Neither output may retain a pointer to an object consumed by this cleanup. */
		*image = VK_NULL_HANDLE;
		*memory = VK_NULL_HANDLE;
	}

	/* An alias not transferred to memory remains this function's independent cleanup responsibility. */
	if (request.handle != 0U) {
		vulkan_context_lock(device->object.context);

		cleanup = vulkan_resource_destroy(device->object.context, request.handle);

		vulkan_context_unlock(device->object.context);

		/* Uncertain alias retirement overrides the earlier failure with namespace loss. */
		if (cleanup != VK_SUCCESS)
			return VK_ERROR_DEVICE_LOST;
	}

	/* Creation, import or binding errors remain observable after every owned object is retired. */
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: a separately owned image and allocation refer to the producer's original GPU storage. */
	return VK_SUCCESS;
}
