/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Translates standard direct-display operations at the library's native boundary.
 */

#include "wsi-internal.h"

#include <uapi/gpu.h>
#include <uapi/gpu-display.h>

#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

/*
 * One device-owned native plane shared across compatible display surfaces.
 * Separate swapchains can share images and the same physical output reservation.
 */
struct wsi_display_plane {
	struct wsi_display_plane *next;
	struct VkDevice_T *device;
	struct vulkan_allocator allocator;
	uint64_t identifier;
	uint64_t display;
	uint64_t storage;
	uint64_t storage_bytes;
	uint32_t plane;
	uint32_t references;
};

/* One surface-owned reference shared by its old and replacement swapchains. */
struct wsi_display_lease {
	struct vulkan_surface *surface;
	struct VkDevice_T *device;
	struct wsi_display_plane *plane;
	uint64_t identifier;
	uint64_t generation;
	uint32_t references;
};

static struct wsi_display_plane *display_planes;

/* Serializes native lease replacement and copied presentation without wire locks. */
static pthread_mutex_t display_mutex = PTHREAD_MUTEX_INITIALIZER;

static VkResult display_capabilities(struct vulkan_surface *surface, struct VkPhysicalDevice_T *physical, VkSurfaceCapabilitiesKHR *capabilities);
static VkResult display_formats(struct vulkan_surface *surface, struct VkPhysicalDevice_T *physical, uint32_t *count, VkSurfaceFormatKHR *formats);
static VkResult display_present_modes(struct vulkan_surface *surface, struct VkPhysicalDevice_T *physical, uint32_t *count, VkPresentModeKHR *modes);
static VkResult display_claim_native(struct vulkan_surface *surface, struct VkDevice_T *device, void **result);
static VkResult display_release_native(void *private_lease);
static VkResult display_present_native(void *private_lease, const struct vulkan_wsi_pixels *pixels, uint64_t *sequence);
static VkResult display_wait_native(void *private_lease, uint64_t sequence, uint64_t timeout);
static int display_ioctl(struct vulkan_context *context, unsigned long command, void *argument);
static VkResult display_error(int error);
static VkResult display_validate_surface(struct vulkan_surface *surface, struct VkPhysicalDevice_T *physical);
static VkResult display_storage(struct wsi_display_lease *lease, uint64_t bytes);

/* Future window-system adapters implement this same private presentation boundary. */
const struct vulkan_wsi_platform_ops vulkan_wsi_display_platform = {
	display_capabilities, display_formats, display_present_modes,
	display_claim_native, display_release_native, display_present_native,
	display_wait_native, NULL
};

/*
 * Queries native output discovery through the physical device's own GPU open.
 */
VkResult
vulkan_wsi_display_query(
	struct VkPhysicalDevice_T *physical,
	uint32_t index,
	uint32_t *count,
	struct vulkan_wsi_output *output)
{
	struct gpu_display_info request;
	int status;
	int error;

	/* Only the adapter names the kernel request or its native identifier space. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.index = index;
	status = display_ioctl(physical->object.context, GPU_DISPLAY_QUERY, &request);
	if (status != 0) {
		error = errno;
		error = display_error(error);
		return error;
	}

	/* Count-only queries do not require or initialize an output snapshot. */
	*count = request.count;

	/* Completes count-only discovery without interpreting an absent output record. */
	if (index == UINT32_MAX)
		return VK_SUCCESS;

	/* Preserves native identity while translating capabilities into WSI terms. */
	memset(output, 0, sizeof(*output));
	output->identifier = request.display_id;
	output->generation = request.generation;
	output->max_frame_bytes = request.max_frame_bytes;
	output->plane_count = request.plane_count;
	output->physical_dimensions.width = request.physical_width_mm;
	output->physical_dimensions.height = request.physical_height_mm;
	output->preferred_extent.width = request.preferred_width;
	output->preferred_extent.height = request.preferred_height;
	output->maximum_extent.width = request.max_width;
	output->maximum_extent.height = request.max_height;
	output->refresh_millihz = request.refresh_millihz;
	memcpy(output->name, request.name, sizeof(output->name));
	output->name[sizeof(output->name) - 1U] = '\0';

	/* Kernel and library flags deliberately have independent numeric encodings. */
	if ((request.flags & GPU_DISPLAY_CONNECTED) != 0U)
		output->flags |= VULKAN_WSI_OUTPUT_CONNECTED;

	/* Advertises FIFO only when the native display exposes its refresh completion contract. */
	if ((request.flags & GPU_DISPLAY_FIFO) != 0U)
		output->flags |= VULKAN_WSI_OUTPUT_FIFO;

	/* Marks the plane active only when the native engine currently owns a displayed image. */
	if ((request.flags & GPU_DISPLAY_ACTIVE) != 0U)
		output->flags |= VULKAN_WSI_OUTPUT_ACTIVE;

	/* Includes the red-first packed format supported by native presentation. */
	if ((request.formats & GPU_DISPLAY_FORMAT_RGBA8888) != 0U)
		output->formats |= VULKAN_WSI_FORMAT_RGBA;

	/* Includes the blue-first packed format supported by native presentation. */
	if ((request.formats & GPU_DISPLAY_FORMAT_BGRA8888) != 0U)
		output->formats |= VULKAN_WSI_FORMAT_BGRA;

	/* Succeeded: generic WSI code no longer needs the kernel request structure. */
	return VK_SUCCESS;
}

/*
 * Enumerates native mode parameters without creating or selecting a host mode.
 */
VkResult
vulkan_wsi_display_mode_query(
	struct VkPhysicalDevice_T *physical,
	const struct vulkan_wsi_output *output,
	uint32_t index,
	uint32_t *count,
	VkDisplayModeParametersKHR *mode)
{
	struct gpu_display_mode request;
	int status;
	int error;

	/* Captures the same generation which supplied the caller's output snapshot. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.display_id = (uint32_t)output->identifier;
	request.operation = GPU_DISPLAY_MODE_ENUMERATE;
	request.generation = output->generation;
	request.index = index;
	status = display_ioctl(physical->object.context, GPU_DISPLAY_MODE, &request);
	if (status != 0) {
		error = errno;
		error = display_error(error);
		return error;
	}

	/* Only an indexed query returns parameters rather than the available count. */
	*count = request.count;
	if (index != UINT32_MAX) {
		mode->visibleRegion.width = request.width;
		mode->visibleRegion.height = request.height;
		mode->refreshRate = request.refresh_millihz;
	}

	/* Succeeded: the caller received validated mode metadata, not display ownership. */
	return VK_SUCCESS;
}

/*
 * Validates custom direct modes through a side-effect-free kernel operation.
 */
VkResult
vulkan_wsi_display_mode_validate(
	struct VkPhysicalDevice_T *physical,
	const struct vulkan_wsi_output *output,
	const VkDisplayModeParametersKHR *mode)
{
	struct gpu_display_mode request;
	int status;
	int error;

	/* No reservation or mode transition occurs until a later swapchain presents. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.display_id = (uint32_t)output->identifier;
	request.operation = GPU_DISPLAY_MODE_VALIDATE;
	request.generation = output->generation;
	request.width = mode->visibleRegion.width;
	request.height = mode->visibleRegion.height;
	request.refresh_millihz = mode->refreshRate;
	status = display_ioctl(physical->object.context, GPU_DISPLAY_MODE, &request);
	if (status != 0) {
		error = errno;
		if (error == EINVAL)
			return VK_ERROR_INITIALIZATION_FAILED;
		error = display_error(error);
		return error;
	}

	/* Succeeded: this virtual mode can be selected atomically with a whole frame. */
	return VK_SUCCESS;
}

/* Describes the full-output, identity-transform plane backed by this surface. */
static VkResult
display_capabilities(
	struct vulkan_surface *surface,
	struct VkPhysicalDevice_T *physical,
	VkSurfaceCapabilitiesKHR *capabilities)
{
	VkResult error;

	/* A native generation change makes the old surface description obsolete. */
	error = display_validate_surface(surface, physical);
	if (error != VK_SUCCESS)
		return error;

	/* Image counts are dynamically allocated; the minimum supports FIFO reuse. */
	memset(capabilities, 0, sizeof(*capabilities));
	capabilities->minImageCount = 2U;
	capabilities->maxImageCount = 0U;
	capabilities->currentExtent = surface->display_info.imageExtent;
	capabilities->minImageExtent = surface->display_info.imageExtent;
	capabilities->maxImageExtent = surface->display_info.imageExtent;
	capabilities->maxImageArrayLayers = 1U;
	capabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	capabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	capabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	capabilities->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
	    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	/* Succeeded: the advertised behavior matches the native full-frame adapter. */
	return VK_SUCCESS;
}

/* Lists only native four-byte formats whose Vulkan image use is supported. */
static VkResult
display_formats(
	struct vulkan_surface *surface,
	struct VkPhysicalDevice_T *physical,
	uint32_t *count,
	VkSurfaceFormatKHR *formats)
{
	VkFormat candidates[2];
	struct vulkan_wsi_output output;
	VkImageFormatProperties properties;
	VkResult error;
	uint32_t available;
	uint32_t written;
	uint32_t capacity;
	uint32_t index;

	/* Native output support and renderer image support must both hold. */
	error = display_validate_surface(surface, physical);
	if (error != VK_SUCCESS)
		return error;
	error = vulkan_wsi_display_snapshot(surface->display_mode->display, &output);
	if (error != VK_SUCCESS)
		return error;
	candidates[0] = VK_FORMAT_R8G8B8A8_UNORM;
	candidates[1] = VK_FORMAT_B8G8R8A8_UNORM;
	capacity = 0U;
	if (formats != NULL)
		capacity = *count;

	/* Enumerates the intersection without advertising an unusable image format. */
	available = 0U;
	written = 0U;

	/* Intersects each native packed format with real Vulkan image support. */
	for (index = 0U; index < 2U; index++) {
		/* Skips a packed format which the selected native output cannot present. */
		if ((output.formats & (1U << index)) == 0U)
			continue;
		error = vkGetPhysicalDeviceImageFormatProperties(
			(VkPhysicalDevice)physical,
			candidates[index],
			VK_IMAGE_TYPE_2D,
			VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			0U,
			&properties);

		/* Excludes native formats which the renderer cannot use with required image operations. */
		if (error == VK_ERROR_FORMAT_NOT_SUPPORTED)
			continue;
		if (error != VK_SUCCESS)
			return error;
		available++;
		if (written >= capacity)
			continue;
		formats[written].format = candidates[index];
		formats[written].colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		written++;
	}

	/* Standard enumeration returns written count and VK_INCOMPLETE for truncation. */
	if (formats == NULL) {
		*count = available;
		return VK_SUCCESS;
	}

	*count = written;
	if (written < available)
		return VK_INCOMPLETE;

	/* Succeeded: every supported packed surface format has been returned. */
	return VK_SUCCESS;
}

/* Advertises FIFO only when the native display explicitly guarantees it. */
static VkResult
display_present_modes(
	struct vulkan_surface *surface,
	struct VkPhysicalDevice_T *physical,
	uint32_t *count,
	VkPresentModeKHR *modes)
{
	VkResult error;
	struct vulkan_wsi_output output;

	/* A receipt-only display transport must not be advertised as FIFO. */
	error = display_validate_surface(surface, physical);
	if (error != VK_SUCCESS)
		return error;
	error = vulkan_wsi_display_snapshot(surface->display_mode->display, &output);
	if (error != VK_SUCCESS)
		return error;

	/* Refuses to advertise a surface when mandatory FIFO semantics are unavailable. */
	if ((output.flags & VULKAN_WSI_OUTPUT_FIFO) == 0U)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* The synchronous native FIFO implementation has one standard present mode. */
	if (modes == NULL) {
		*count = 1U;
		return VK_SUCCESS;
	}

	/* Reports a truncated present-mode list without writing past caller capacity. */
	if (*count == 0U)
		return VK_INCOMPLETE;
	modes[0] = VK_PRESENT_MODE_FIFO_KHR;
	*count = 1U;

	/* Succeeded: this surface supports the mandatory Vulkan FIFO mode. */
	return VK_SUCCESS;
}

/* Shares one native claim across a surface's old and replacement swapchains. */
static VkResult
display_claim_native(
	struct vulkan_surface *surface,
	struct VkDevice_T *device,
	void **result)
{
	struct wsi_display_lease *lease;
	struct wsi_display_lease *candidate;
	struct wsi_display_plane *plane;
	struct wsi_display_plane *new_plane;
	struct vulkan_wsi_output output;
	struct gpu_display_claim request;
	VkResult error;
	int status;
	int native_error;

	/* Native identity is captured under the common WSI snapshot lock. */
	*result = NULL;
	error = display_validate_surface(surface, device->physical);
	if (error != VK_SUCCESS)
		return error;
	error = vulkan_wsi_display_snapshot(surface->display_mode->display, &output);
	if (error != VK_SUCCESS)
		return error;

	/* Every application callback runs before native ownership serialization. */
	candidate = vulkan_allocate(
		&surface->object.allocator,
		sizeof(*candidate),
		sizeof(void *),
		VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (candidate == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	new_plane = vulkan_allocate(
		&device->object.allocator,
		sizeof(*new_plane),
		sizeof(void *),
		VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
	if (new_plane == NULL) {
		vulkan_free(&surface->object.allocator, candidate);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	memset(candidate, 0, sizeof(*candidate));
	memset(new_plane, 0, sizeof(*new_plane));
	pthread_mutex_lock(&display_mutex);

	/* Replacement chains borrow the same surface reference without a new K claim. */
	lease = surface->platform_private;

	/* Reuses an existing surface lease across old and replacement swapchains. */
	if (lease != NULL) {
		/* Prevents cross-device reuse and reference exhaustion in a surface lease. */
		if (lease->device != device || lease->references == UINT32_MAX) {
			pthread_mutex_unlock(&display_mutex);
			vulkan_free(&device->object.allocator, new_plane);
			vulkan_free(&surface->object.allocator, candidate);
			return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
		}

		lease->references++;
		pthread_mutex_unlock(&display_mutex);
		vulkan_free(&device->object.allocator, new_plane);
		vulkan_free(&surface->object.allocator, candidate);
		*result = lease;
		return VK_SUCCESS;
	}

	/* Independent surfaces on the same device may share a native display plane. */
	plane = display_planes;

	/* Finds existing ownership of the same device display plane before claiming it again. */
	while (plane != NULL) {
		if (plane->device == device &&
		    plane->display == output.identifier &&
		    plane->plane == surface->native_plane)
			break;
		plane = plane->next;
	}

	/* Shares the existing native claim between independent surface descriptions. */
	if (plane != NULL) {
		/* Rejects shared plane reference exhaustion before incrementing its lifetime hold. */
		if (plane->references == UINT32_MAX) {
			pthread_mutex_unlock(&display_mutex);
			vulkan_free(&device->object.allocator, new_plane);
			vulkan_free(&surface->object.allocator, candidate);
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}
	} else {
		/* Only the kernel arbitrates reservations against other device opens. */
		memset(&request, 0, sizeof(request));
		request.version = GPU_ABI_VERSION;
		request.size = sizeof(request);
		request.display_id = (uint32_t)output.identifier;
		request.plane_index = surface->native_plane;
		request.generation = surface->display_mode->generation;
		status = display_ioctl(device->object.context, GPU_DISPLAY_CLAIM, &request);
		if (status != 0) {
			native_error = errno;
			pthread_mutex_unlock(&display_mutex);
			vulkan_free(&device->object.allocator, new_plane);
			vulkan_free(&surface->object.allocator, candidate);
			error = display_error(native_error);
			return error;
		}

		/* Device scope keeps the native reservation alive after any one surface dies. */
		plane = new_plane;
		new_plane = NULL;
		plane->device = device;
		plane->allocator = device->object.allocator;
		plane->identifier = request.lease;
		plane->display = output.identifier;
		plane->plane = surface->native_plane;
		plane->next = display_planes;
		display_planes = plane;
	}

	/* Surface-local mode parameters remain distinct even when the plane is shared. */
	plane->references++;
	candidate->surface = surface;
	candidate->device = device;
	candidate->plane = plane;
	candidate->identifier = plane->identifier;
	candidate->generation = surface->display_mode->generation;
	candidate->references = 1U;
	surface->platform_private = candidate;
	*result = candidate;
	pthread_mutex_unlock(&display_mutex);
	vulkan_free(&device->object.allocator, new_plane);

	/* Succeeded: this surface now owns one reference to a live native reservation. */
	return VK_SUCCESS;
}

/* Releases the final native claim after all replacement swapchains retire. */
static VkResult
display_release_native(
	void *private_lease)
{
	struct wsi_display_lease *lease;
	struct wsi_display_plane *plane;
	struct wsi_display_plane **link;
	struct gpu_display_release request;
	struct gpu_resource_destroy destroy;
	struct vulkan_allocator allocator;
	VkBool32 final_plane;
	VkResult error;
	int status;

	/* One swapchain consumes one local lease reference regardless of native loss. */
	lease = private_lease;
	error = VK_SUCCESS;
	pthread_mutex_lock(&display_mutex);
	lease->references--;

	/* Keeps the surface lease alive while another swapchain still holds it. */
	if (lease->references != 0U) {
		pthread_mutex_unlock(&display_mutex);
		return VK_SUCCESS;
	}

	/* The retiring surface no longer has a pointer into shared native ownership. */
	lease->surface->platform_private = NULL;
	allocator = lease->surface->object.allocator;
	plane = lease->plane;
	plane->references--;
	final_plane = VK_FALSE;

	/* Recognizes the final surface release which must retire native plane ownership. */
	if (plane->references == 0U)
		final_plane = VK_TRUE;

	/* Retires native ownership only after all surfaces have released the shared plane. */
	if (final_plane != VK_FALSE) {
		/* Native release ends the front reference before copied storage can retire. */
		memset(&request, 0, sizeof(request));
		request.version = GPU_ABI_VERSION;
		request.size = sizeof(request);
		request.lease = plane->identifier;
		status = display_ioctl(plane->device->object.context, GPU_DISPLAY_RELEASE, &request);
		if (status != 0)
			error = display_error(errno);

		/* Kernel core retains uncertain hardware references after failed cleanup. */
		if (plane->storage != 0U) {
			memset(&destroy, 0, sizeof(destroy));
			destroy.version = GPU_ABI_VERSION;
			destroy.size = sizeof(destroy);
			destroy.handle = plane->storage;
			status = display_ioctl(plane->device->object.context, GPU_RESOURCE_DESTROY, &destroy);
			if (status != 0 && error == VK_SUCCESS)
				error = display_error(errno);
		}

		/* Removes the global identity before freeing its device-owned allocation. */
		link = &display_planes;

		/* Removes only the retiring native plane from the process-wide ownership list. */
		while (*link != NULL) {
			/* Unlinks the exact plane whose final surface reference was consumed. */
			if (*link == plane) {
				*link = plane->next;
				break;
			}

			link = &(*link)->next;
		}
	}

	pthread_mutex_unlock(&display_mutex);

	/* Current surface and saved device allocators run outside every native mutex. */
	vulkan_free(&allocator, lease);

	/* Frees device-owned plane metadata only after its final native release. */
	if (final_plane != VK_FALSE)
		vulkan_free(&plane->allocator, plane);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: surviving surfaces retain the plane and final destruction frees it. */
	return VK_SUCCESS;
}

/* Copies completed Vulkan readback into native storage and presents one frame. */
static VkResult
display_present_native(
	void *private_lease,
	const struct vulkan_wsi_pixels *pixels,
	uint64_t *sequence)
{
	struct wsi_display_lease *lease;
	struct gpu_transfer transfer;
	struct gpu_display_present request;
	const uint8_t *source;
	uint64_t bytes;
	uint64_t offset;
	uint32_t chunk;
	VkResult error;
	int status;
	int native_error;

	/* A native lease serializes storage replacement with every presentation. */
	lease = private_lease;
	bytes = (uint64_t)pixels->stride * pixels->extent.height;
	pthread_mutex_lock(&display_mutex);
	error = display_storage(lease, bytes);
	if (error != VK_SUCCESS) {
		pthread_mutex_unlock(&display_mutex);
		return error;
	}

	/* Only bounded copied I/O crosses the kernel boundary; app pointers never do. */
	source = pixels->pixels;
	offset = 0U;
	while (offset < bytes) {
		chunk = GPU_COPY_MAX;

		/* Limits the last copy to the remaining completed Vulkan frame bytes. */
		if (bytes - offset < chunk)
			chunk = (uint32_t)(bytes - offset);
		memset(&transfer, 0, sizeof(transfer));
		transfer.version = GPU_ABI_VERSION;
		transfer.size = sizeof(transfer);
		transfer.handle = lease->plane->storage;
		transfer.offset = offset;
		transfer.address = (uint64_t)(uintptr_t)(source + (size_t)offset);
		transfer.bytes = chunk;
		status = display_ioctl(lease->device->object.context, GPU_RESOURCE_WRITE, &transfer);
		if (status != 0) {
			native_error = errno;
			pthread_mutex_unlock(&display_mutex);
			error = display_error(native_error);
			return error;
		}

		offset += chunk;
	}

	/* Geometry and the complete pixel image reach the same native FIFO operation. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.lease = lease->identifier;
	request.handle = lease->plane->storage;
	request.frame = pixels->frame;
	request.width = pixels->extent.width;
	request.height = pixels->extent.height;
	request.stride = pixels->stride;
	request.format = GPU_PIXEL_RGBA8888;

	/* Preserves the standard blue-first channel order at the native presentation boundary. */
	if (pixels->format == VK_FORMAT_B8G8R8A8_UNORM)
		request.format = GPU_PIXEL_BGRA8888;
	request.refresh_millihz = lease->surface->display_mode->parameters.refreshRate;
	request.flags = GPU_DISPLAY_PRESENT_FIFO;
	request.generation = lease->generation;
	status = display_ioctl(lease->device->object.context, GPU_DISPLAY_PRESENT, &request);
	if (status != 0) {
		native_error = errno;
		pthread_mutex_unlock(&display_mutex);
		error = display_error(native_error);
		return error;
	}

	*sequence = request.sequence;
	pthread_mutex_unlock(&display_mutex);

	/* Succeeded: caller pixels are no longer borrowed by the native display. */
	return VK_SUCCESS;
}

/* Observes the native completion associated with a previously returned sequence. */
static VkResult
display_wait_native(
	void *private_lease,
	uint64_t sequence,
	uint64_t timeout)
{
	struct wsi_display_lease *lease;
	struct gpu_display_wait request;
	int status;
	int error;

	/* A sequence describes virtual completion rather than host physical vblank. */
	lease = private_lease;
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.lease = lease->identifier;
	request.sequence = sequence;
	request.timeout_ns = timeout;
	status = display_ioctl(lease->device->object.context, GPU_DISPLAY_WAIT, &request);
	if (status != 0) {
		error = errno;
		if (error == EAGAIN)
			return VK_NOT_READY;

		/* Reports expiration of the requested presentation completion interval. */
		if (error == ETIMEDOUT)
			return VK_TIMEOUT;
		error = display_error(error);
		return error;
	}

	/* Succeeded: all native use through this presentation sequence has finished. */
	return VK_SUCCESS;
}

/* Shares the GPU open's admission mutex with ordinary wire and memory requests. */
static int
display_ioctl(
	struct vulkan_context *context,
	unsigned long command,
	void *argument)
{
	int status;
	int error;

	/* Raw native operations serialize with renderer transactions on the same fd. */
	vulkan_context_lock(context);

	status = ioctl(context->fd, command, argument);
	error = errno;
	
	vulkan_context_unlock(context);

	if (status != 0) {
		errno = error;
		return -1;
	}

	/* Succeeded: no context mutex survives into any Vulkan call or completion wait. */
	return 0;
}

/* Converts native failure domains without reporting an unrelated Vulkan success. */
static VkResult
display_error(
	int error)
{
	/* Kernel allocation failures describe unavailable native device storage. */
	if (error == ENOMEM)
		return VK_ERROR_OUT_OF_DEVICE_MEMORY;

	/* Reports display ownership held by a competing native window or device session. */
	if (error == EBUSY)
		return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;

	/* Reports a surface whose validated native mode generation has changed. */
	if (error == ESTALE)
		return VK_ERROR_OUT_OF_DATE_KHR;

	/* Reports loss of the native output backing this surface. */
	if (error == ENODEV || error == ENXIO)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Reports a native execution failure which invalidates the device namespace. */
	if (error == EIO || error == ETIMEDOUT)
		return VK_ERROR_DEVICE_LOST;

	/* Other native failures cannot establish a usable presentation target. */
	return VK_ERROR_SURFACE_LOST_KHR;
}

/* Ensures a surface still describes the exact native mode generation it owns. */
static VkResult
display_validate_surface(
	struct vulkan_surface *surface,
	struct VkPhysicalDevice_T *physical)
{
	VkResult error;
	struct vulkan_wsi_output output;

	/* A direct surface is restricted to its creating GPU's display inventory. */
	if (surface->display_mode->display->physical != physical)
		return VK_ERROR_SURFACE_LOST_KHR;
	error = vulkan_wsi_display_refresh(surface->display_mode->display);
	if (error != VK_SUCCESS)
		return error;
	error = vulkan_wsi_display_snapshot(surface->display_mode->display, &output);
	if (error != VK_SUCCESS)
		return error;

	/* Rejects presentation through a mode description invalidated by a topology event. */
	if (surface->display_mode->generation != output.generation)
		return VK_ERROR_OUT_OF_DATE_KHR;

	/* Succeeded: no native topology event invalidated this mode description. */
	return VK_SUCCESS;
}

/* Replaces native copied storage only when a new whole-frame extent requires it. */
static VkResult
display_storage(
	struct wsi_display_lease *lease,
	uint64_t bytes)
{
	struct gpu_resource_create create;
	struct gpu_resource_destroy destroy;
	int status;
	int error;

	/* Repeated frames and same-sized replacement swapchains reuse their storage. */
	if (lease->plane->storage != 0U && lease->plane->storage_bytes == bytes)
		return VK_SUCCESS;

	/* Acquires the replacement before consuming an existing usable allocation. */
	memset(&create, 0, sizeof(create));
	create.version = GPU_ABI_VERSION;
	create.size = sizeof(create);
	create.bytes = bytes;
	create.usage = GPU_RESOURCE_USAGE_STORAGE;
	status = display_ioctl(lease->device->object.context, GPU_RESOURCE_CREATE, &create);
	if (status != 0) {
		error = errno;
		error = display_error(error);
		return error;
	}

	/* Native presentation copies storage synchronously, so old storage is idle. */
	if (lease->plane->storage != 0U) {
		memset(&destroy, 0, sizeof(destroy));
		destroy.version = GPU_ABI_VERSION;
		destroy.size = sizeof(destroy);
		destroy.handle = lease->plane->storage;
		status = display_ioctl(lease->device->object.context, GPU_RESOURCE_DESTROY, &destroy);
		if (status != 0) {
			error = errno;
			destroy.handle = create.handle;
			display_ioctl(lease->device->object.context, GPU_RESOURCE_DESTROY, &destroy);
			error = display_error(error);
			return error;
		}
	}

	/* The lease becomes the sole owner of the fully acquired replacement storage. */
	lease->plane->storage = create.handle;
	lease->plane->storage_bytes = bytes;

	/* Succeeded: the next complete frame can be copied into this native resource. */
	return VK_SUCCESS;
}
