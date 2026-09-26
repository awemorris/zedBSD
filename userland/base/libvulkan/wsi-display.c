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
#include <uapi/gpu-scanout.h>
#include <uapi/gpu-fence.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Native planes borrow image identity only while its swapchain-owned wrapper is alive. */
struct wsi_display_image;

/* One independent GPU open keeps display waits outside the renderer's admission lock. */
struct wsi_display_connection {
	struct wsi_display_connection *next;
	struct VkDevice_T *device;
	struct vulkan_allocator allocator;
	uint32_t references;
	uint64_t device_identifier;
	uint32_t capabilities;
	int fd;
};

/* One native plane retains its selected image until a checked replacement or release. */
struct wsi_display_plane {
	struct wsi_display_plane *next;
	struct VkDevice_T *device;
	struct vulkan_allocator allocator;
	struct wsi_display_connection *connection;
	struct wsi_display_image *front;
	uint64_t identifier;
	uint64_t display;
	uint64_t frame;
	uint64_t completion_generation;
	int completion_fd;
	VkBool32 completion_used;
	uint32_t plane;
	uint32_t references;
};

/* One swapchain-owned imported image may be retained by several shared-swapchain planes. */
struct wsi_display_image {
	struct wsi_display_connection *connection;
	struct gpu_resource_import imported;
	uint32_t holds;
};

/* One surface-owned reference shared by its old and replacement swapchains. */
struct wsi_display_lease {
	struct vulkan_surface *surface;
	struct VkDevice_T *device;
	struct wsi_display_plane *plane;
	uint64_t identifier;
	uint64_t generation;
	uint64_t storage;
	uint64_t storage_bytes;
	struct gpu_scanout_constraints constraints;
	uint32_t references;
};

/* Live planes are linked under display_mutex until their last surface lease retires. */
static struct wsi_display_plane *display_planes;

/* Each logical-device/native-node pair shares one open; display_mutex protects publication and holds. */
static struct wsi_display_connection *display_connections;

/* Serializes native claims and scanout ownership without holding renderer wire locks. */
static pthread_mutex_t display_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Event observation never waits behind native presentation or renderer admission. */
static pthread_mutex_t display_event_mutex = PTHREAD_MUTEX_INITIALIZER;

static VkResult display_progress(void *private_lease);
static int display_wait_descriptor(void *private_lease);
static VkResult display_capabilities(struct vulkan_surface *surface, struct VkPhysicalDevice_T *physical, VkSurfaceCapabilitiesKHR *capabilities);
static VkResult display_formats(struct vulkan_surface *surface, struct VkPhysicalDevice_T *physical, uint32_t *count, VkSurfaceFormatKHR *formats);
static VkResult display_present_modes(struct vulkan_surface *surface, struct VkPhysicalDevice_T *physical, uint32_t *count, VkPresentModeKHR *modes);
static VkResult display_claim_native(struct vulkan_surface *surface, struct VkDevice_T *device, void **result);
static VkResult display_release_native(void *private_lease);
static VkResult display_prepare_copy(void *private_lease, VkFormat format, VkExtent2D extent);
static VkResult display_present_native(void *private_lease, const struct vulkan_wsi_pixels *pixels, uint64_t *sequence);
static VkResult display_constraints_query(struct VkPhysicalDevice_T *physical, const struct vulkan_wsi_output *output, struct gpu_scanout_constraints *request);
static VkResult display_import_image(void *private_lease, int fd, const struct gpu_image_descriptor *descriptor, void **result);
static VkResult display_present_image(void *private_lease, void *private_image, VkPresentModeKHR mode, uint64_t *sequence);
static VkResult display_present_image_sync(void *private_lease, void *private_image, VkPresentModeKHR mode, uint64_t *sequence, int wait_fd, uint64_t wait_generation);
static VkBool32 display_image_available(void *private_image);
static void display_destroy_image(void *private_image);
static VkResult display_connection_create(struct VkDevice_T *device, const struct vulkan_wsi_output *output, struct wsi_display_connection *candidate, struct wsi_display_connection **result);
static struct wsi_display_connection *display_connection_put(struct wsi_display_connection *connection);
static VkResult display_wait_native(void *private_lease, uint64_t sequence, uint64_t timeout);
static VkResult display_error(int error);
static VkResult display_placement(void *private_lease, struct gpu_placement *placement);
static VkResult display_validate_surface(struct vulkan_surface *surface, struct VkPhysicalDevice_T *physical);

/* Future window-system adapters implement this same private presentation boundary. */
const struct vulkan_wsi_platform_ops vulkan_wsi_display_platform = {
	display_capabilities, display_formats, display_present_modes,
	display_claim_native, display_release_native, display_present_native,
	display_wait_native, NULL, display_import_image, display_present_image,
	display_progress, display_image_available, display_destroy_image, display_prepare_copy, display_present_image_sync, display_placement, display_wait_descriptor,
	NULL
};

/*
 * Queries paired native output discovery through its independent cached display open.
 */
VkResult
vulkan_wsi_display_query(
	struct VkPhysicalDevice_T *physical,
	uint32_t index,
	uint32_t *count,
	struct vulkan_wsi_output *output)
{
	struct gpu_display_info request;
	char path[262];
	uint64_t device_id;
	VkResult error;

	/* One cached native inventory pairs display nodes with this renderer without reopening them per frame. */
	error = vulkan_wsi_display_node_query(physical, index, count, &request, &device_id, path);
	if (error != VK_SUCCESS)
		return error;

	/* Count-only discovery carries no native output record. */
	if (index == UINT32_MAX)
		return VK_SUCCESS;

	/* Preserves native identity while translating capabilities into WSI terms. */
	memset(output, 0, sizeof(*output));
	output->identifier = request.display_id;
	output->device_identifier = device_id;
	memcpy(output->device_path, path, sizeof(output->device_path));
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

	/* Shared scanout is mandatory for the normal GPU-resident presentation path. */
	if ((request.flags & GPU_DISPLAY_BLOB) != 0U)
		output->flags |= VULKAN_WSI_OUTPUT_BLOB;

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
	status = vulkan_wsi_display_node_ioctl(physical, output, GPU_DISPLAY_MODE, &request);
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
	status = vulkan_wsi_display_node_ioctl(physical, output, GPU_DISPLAY_MODE, &request);
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
	struct gpu_scanout_constraints constraints;
	VkResult error;
	uint32_t available;
	uint32_t written;
	uint32_t capacity;
	uint32_t index;

	/* Native output support and renderer image support must both hold. */
	error = display_validate_surface(surface, physical);
	if (error != VK_SUCCESS)
		return error;

	/* The saved output identity and generation must survive the preceding live topology validation. */
	error = vulkan_wsi_display_snapshot(surface->display_mode->display, &output);
	if (error != VK_SUCCESS)
		return error;

	/* Native layout and copied-route capabilities restrict the same generation of public formats. */
	error = display_constraints_query(physical, &output, &constraints);
	if (error != VK_SUCCESS)
		return error;


	/* Packed candidate order matches the independent native channel-format flags. */
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
		if ((output.formats & (1U << index)) == 0U ||
		    (constraints.formats & (index == 0U ? GPU_DISPLAY_FORMAT_RGBA8888 : GPU_DISPLAY_FORMAT_BGRA8888)) == 0U)
			continue;

		/* Rendering and transfer support are queried independently from native scanout metadata. */
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

		/* Operational rendering failures cannot be hidden as unsupported formats. */
		if (error != VK_SUCCESS)
			return error;

		/* A copied route needs only optimal-image rendering and transfer; native-only sharing also needs linear storage. */
		if ((constraints.flags & GPU_SCANOUT_COPY) == 0U) {
			/* Presentation storage must also support the exported linear GPU-copy destination. */
			error = vkGetPhysicalDeviceImageFormatProperties(
				(VkPhysicalDevice)physical,
				candidates[index],
				VK_IMAGE_TYPE_2D,
				VK_IMAGE_TILING_LINEAR,
				VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				0U,
				&properties);
			if (error == VK_ERROR_FORMAT_NOT_SUPPORTED)
				continue;

			/* Other renderer failures cannot be advertised as absent formats. */
			if (error != VK_SUCCESS)
				return error;
		}

		/* A native and renderer-compatible format contributes one public entry. */
		available++;

		/* Count every compatible format while respecting the caller's bounded output capacity. */
		if (written >= capacity)
			continue;

		/* Publish one complete standard entry without exposing internal native format bits. */
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

	/* The saved output identity and generation must survive the preceding live topology validation. */
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
	struct wsi_display_connection *new_connection;
	struct wsi_display_connection *retired_connection;
	struct vulkan_wsi_output output;
	struct gpu_display_claim request;
	struct gpu_fence_create completion;
	struct gpu_scanout_constraints constraints;
	VkResult error;
	int status;
	int native_error;

	/* Native identity is captured under the common WSI snapshot lock. */
	*result = NULL;
	new_connection = NULL;
	error = display_validate_surface(surface, device->physical);
	if (error != VK_SUCCESS)
		return error;

	/* The saved output identity and generation must survive the preceding live topology validation. */
	error = vulkan_wsi_display_snapshot(surface->display_mode->display, &output);
	if (error != VK_SUCCESS)
		return error;

	/* Storage selection uses the generation-specific native format and placement contract. */
	error = display_constraints_query(device->physical, &output, &constraints);
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

	/* Each plane candidate can retire independently if later connection allocation fails. */
	new_plane = vulkan_allocate(
		&device->object.allocator,
		sizeof(*new_plane),
		sizeof(void *),
		VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
	if (new_plane == NULL) {
		vulkan_free(&surface->object.allocator, candidate);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Connection allocation also precedes the native lock because user callbacks may reenter Vulkan. */
	new_connection = vulkan_allocate(&device->object.allocator, sizeof(*new_connection), sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
	if (new_connection == NULL) {
		vulkan_free(&device->object.allocator, new_plane);
		vulkan_free(&device->object.allocator, new_connection);
		vulkan_free(&surface->object.allocator, candidate);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* The surface candidate owns no claim until publication under native serialization. */
	memset(candidate, 0, sizeof(*candidate));

	/* The plane candidate starts without a transferable completion capability. */
	memset(new_plane, 0, sizeof(*new_plane));
	new_plane->completion_fd = -1;

	/* Native ownership is shared only among exact device, node and plane identities. */
	pthread_mutex_lock(&display_mutex);

	/* Replacement chains borrow the same surface reference without a new K claim. */
	lease = surface->platform_private;

	/* Reuses an existing surface lease across old and replacement swapchains. */
	if (lease != NULL) {
		/* Prevents cross-device reuse and reference exhaustion in a surface lease. */
		if (lease->device != device || lease->references == UINT32_MAX) {
			pthread_mutex_unlock(&display_mutex);
			vulkan_free(&device->object.allocator, new_plane);
			vulkan_free(&device->object.allocator, new_connection);
			vulkan_free(&surface->object.allocator, candidate);
			return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
		}

		/* A replacement chain retains the existing surface claim independently of the older chain. */
		lease->references++;
		pthread_mutex_unlock(&display_mutex);
		vulkan_free(&device->object.allocator, new_plane);
		vulkan_free(&device->object.allocator, new_connection);
		vulkan_free(&surface->object.allocator, candidate);
		*result = lease;
		return VK_SUCCESS;
	}

	/* Independent surfaces on the same device may share a native display plane. */
	plane = display_planes;

	/* Finds existing ownership of the same device display plane before claiming it again. */
	while (plane != NULL) {
		/* Node identity keeps equal local display identifiers in separate ownership domains. */
		if (plane->device == device &&
		    plane->display == output.identifier &&
		    plane->connection->device_identifier == output.device_identifier &&
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
			vulkan_free(&device->object.allocator, new_connection);
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

		/* A separate open admits native display work independently from renderer transactions. */
		error = display_connection_create(device, &output, new_connection, &new_plane->connection);
		if (error != VK_SUCCESS) {
			pthread_mutex_unlock(&display_mutex);
			vulkan_free(&device->object.allocator, new_plane);
			vulkan_free(&device->object.allocator, new_connection);
			vulkan_free(&surface->object.allocator, candidate);
			return error;
		}

		/* A newly published connection consumes its candidate; a shared one leaves it unused. */
		if (new_plane->connection == new_connection)
			new_connection = NULL;

		/* Completion allocation belongs to the plane and precedes any native claim or accepted GPU job. */
		if ((new_plane->connection->capabilities & GPU_CAP_FENCE) != 0U) {
			/* Zero output fields ensure the create request owns only its new pending payload. */
			memset(&completion, 0, sizeof(completion));
			completion.version = GPU_ABI_VERSION;
			completion.size = sizeof(completion);
			completion.flags = GPU_HANDLE_CLOEXEC;
			completion.fd = -1;
			status = ioctl(new_plane->connection->fd, GPU_FENCE_CREATE, &completion);
			if (status != 0) {
				native_error = errno;
				retired_connection = display_connection_put(new_plane->connection);
				pthread_mutex_unlock(&display_mutex);
				if (retired_connection != NULL)
					vulkan_free(&device->object.allocator, retired_connection);
				vulkan_free(&device->object.allocator, new_plane);
				vulkan_free(&device->object.allocator, new_connection);
				vulkan_free(&surface->object.allocator, candidate);
				error = display_error(native_error);
				return error;
			}

			/* The plane retains its independent display-completion capability until final release. */
			new_plane->completion_fd = completion.fd;
			new_plane->completion_generation = completion.generation;
		}

		/* Planes belonging to this logical-device/native-node pair share the imported-image namespace. */
		status = ioctl(new_plane->connection->fd, GPU_DISPLAY_CLAIM, &request);
		if (status != 0) {
			native_error = errno;
			if (new_plane->completion_fd >= 0)
				close(new_plane->completion_fd);
			retired_connection = display_connection_put(new_plane->connection);
			pthread_mutex_unlock(&display_mutex);
			if (retired_connection != NULL)
				vulkan_free(&device->object.allocator, retired_connection);
			vulkan_free(&device->object.allocator, new_plane);
			vulkan_free(&device->object.allocator, new_connection);
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

	/* The new surface lease keeps this plane alive independently from all other surfaces. */
	plane->references++;

	/* Surface-local mode parameters remain distinct even when the plane is shared. */
	candidate->surface = surface;
	candidate->device = device;
	candidate->plane = plane;
	candidate->identifier = plane->identifier;
	candidate->generation = surface->display_mode->generation;
	candidate->constraints = constraints;
	candidate->references = 1U;
	surface->platform_private = candidate;
	*result = candidate;

	pthread_mutex_unlock(&display_mutex);

	vulkan_free(&device->object.allocator, new_plane);
	vulkan_free(&device->object.allocator, new_connection);

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
	struct wsi_display_connection *retired_connection;
	struct gpu_display_release request;
	struct gpu_resource_destroy destroy;
	struct vulkan_allocator allocator;
	VkBool32 final_plane;
	VkResult error;
	int status;

	/* One swapchain consumes one local lease reference regardless of native loss. */
	lease = private_lease;
	error = VK_SUCCESS;
	retired_connection = NULL;
	pthread_mutex_lock(&display_mutex);

	/* Zero consumes the last swapchain hold and authorizes retirement of this surface lease. */
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

	/* Zero consumes the last surface hold and authorizes release of this native plane. */
	plane->references--;
	final_plane = VK_FALSE;

	/* Recognizes the final surface release which must retire native plane ownership. */
	if (plane->references == 0U)
		final_plane = VK_TRUE;

	/* Copied storage belongs to this surface lease and is idle after its final accepted job drained. */
	if (lease->storage != 0U) {
		memset(&destroy, 0, sizeof(destroy));
		destroy.version = GPU_ABI_VERSION;
		destroy.size = sizeof(destroy);
		destroy.handle = lease->storage;
		status = ioctl(plane->connection->fd, GPU_RESOURCE_DESTROY, &destroy);
		if (status != 0)
			error = VK_ERROR_DEVICE_LOST;
	}

	/* Retires native ownership only after all surfaces have released the shared plane. */
	if (final_plane != VK_FALSE) {
		/* Native release ends the front reference before copied storage can retire. */
		memset(&request, 0, sizeof(request));
		request.version = GPU_ABI_VERSION;
		request.size = sizeof(request);
		request.lease = plane->identifier;
		status = ioctl(plane->connection->fd, GPU_DISPLAY_RELEASE, &request);
		if (status != 0)
			error = display_error(errno);

		/* A completed release ends this plane's independent native-image use. */
		if (status == 0 && plane->front != NULL) {
			plane->front->holds--;
			plane->front = NULL;
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

		/* Its completion capability no longer belongs to any accepted native operation. */
		if (plane->completion_fd >= 0)
			close(plane->completion_fd);

		/* The final plane close withdraws any uncertain remaining kernel scanout ownership. */
		retired_connection = display_connection_put(plane->connection);
	}

	pthread_mutex_unlock(&display_mutex);

	/* Connection allocation callbacks run only after native ownership serialization has ended. */
	if (retired_connection != NULL)
		vulkan_free(&retired_connection->allocator, retired_connection);

	/* Current surface and saved device allocators run outside every native mutex. */
	vulkan_free(&allocator, lease);

	/* Frees device-owned plane metadata only after its final native release. */
	if (final_plane != VK_FALSE)
		vulkan_free(&plane->allocator, plane);

	/* Native loss remains visible even though all local ownership has been retired. */
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: surviving surfaces retain the plane and final destruction frees it. */
	return VK_SUCCESS;
}

/* Imports a GPU allocation into the display open while preserving authoritative layout. */
static VkResult
display_import_image(
	void *private_lease,
	int fd,
	const struct gpu_image_descriptor *descriptor,
	void **result)
{
	struct wsi_display_lease *lease;
	struct wsi_display_image *image;
	struct gpu_resource_destroy destroy;
	VkResult error;
	int status;
	int mismatch;
	int native_error;

	/* Failed imports publish no image wrapper and leave the caller's fd untouched. */
	*result = NULL;
	lease = private_lease;

	/* A query is permission to trial sharing, never a promise that placement or physical backing will import. */
	if ((lease->constraints.flags & GPU_SCANOUT_SHARED) == 0U)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* Layout alignment is checked before consuming any native allocation or producer capability. */
	if ((descriptor->stride & (lease->constraints.stride_alignment - 1U)) != 0U ||
	    (descriptor->offset & (lease->constraints.offset_alignment - 1U)) != 0U)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* Wrapper allocation precedes native import and remains caller-owned until a complete alias exists. */
	image = vulkan_allocate(&lease->device->object.allocator, sizeof(*image), sizeof(void *), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	if (image == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* The native alias belongs to the same display namespace as every shared plane. */
	memset(image, 0, sizeof(*image));
	image->connection = lease->plane->connection;
	image->imported.version = GPU_ABI_VERSION;
	image->imported.size = sizeof(image->imported);
	image->imported.fd = fd;
	image->imported.flags = GPU_IMPORT_SCANOUT;
	pthread_mutex_lock(&display_mutex);

	/* Import validates both GPU identity and the independent allocation capability. */
	status = ioctl(image->connection->fd, GPU_RESOURCE_IMPORT, &image->imported);
	if (status != 0) {
		native_error = errno;
		error = display_error(native_error);
		if (native_error == ENOTSUP || native_error == EXDEV)
			error = VK_ERROR_FORMAT_NOT_SUPPORTED;
		pthread_mutex_unlock(&display_mutex);
		vulkan_free(&lease->device->object.allocator, image);
		return error;
	}

	/* A claimed row layout cannot reinterpret the descriptor retained by the kernel. */
	mismatch = memcmp(&image->imported.image, descriptor, sizeof(*descriptor));
	if (mismatch != 0) {
		memset(&destroy, 0, sizeof(destroy));
		destroy.version = GPU_ABI_VERSION;
		destroy.size = sizeof(destroy);
		destroy.handle = image->imported.handle;
		status = ioctl(image->connection->fd, GPU_RESOURCE_DESTROY, &destroy);
		pthread_mutex_unlock(&display_mutex);
		vulkan_free(&lease->device->object.allocator, image);
		if (status != 0)
			return VK_ERROR_DEVICE_LOST;
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	pthread_mutex_unlock(&display_mutex);

	*result = image;

	/* Succeeded: the swapchain owns an independent native alias without CPU pixel storage. */
	return VK_SUCCESS;
}

/* Presents an already completed image when no explicit shared dependency was requested. */
static VkResult
display_present_image(
	void *private_lease,
	void *private_image,
	VkPresentModeKHR mode,
	uint64_t *sequence)
{
	VkResult error;

	/* The ordinary adapter contract still requires actual producer completion before this call. */
	error = display_present_image_sync(private_lease, private_image, mode, sequence, -1, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: native selection completed without a transferable dependency payload. */
	return VK_SUCCESS;
}

/* Selects a completed GPU allocation and releases only the displaced native image. */
static VkResult
display_present_image_sync(
	void *private_lease,
	void *private_image,
	VkPresentModeKHR mode,
	uint64_t *sequence,
	int wait_fd,
	uint64_t wait_generation)
{
	struct wsi_display_lease *lease;
	struct wsi_display_image *image;
	struct wsi_display_image *previous;
	struct gpu_display_present request;
	struct gpu_display_present_sync synchronized;
	struct gpu_fence_state reset;
	const struct gpu_image_descriptor *descriptor;
	VkResult error;
	int status;

	/* Only the enumerated FIFO mode may reach this native display adapter. */
	if (mode != VK_PRESENT_MODE_FIFO_KHR)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The image and lease must resolve to the same logical-device/native-node namespace. */
	lease = private_lease;
	image = private_image;
	if (image->connection != lease->plane->connection)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* No caller pixel pointer or transfer ioctl participates in normal presentation. */
	descriptor = &image->imported.image;
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.lease = lease->identifier;
	request.handle = image->imported.handle;
	request.offset = descriptor->offset;
	request.width = descriptor->width;
	request.height = descriptor->height;
	request.stride = descriptor->stride;
	request.format = descriptor->format;
	request.refresh_millihz = lease->surface->display_mode->parameters.refreshRate;
	request.flags = GPU_DISPLAY_PRESENT_FIFO | GPU_DISPLAY_PRESENT_BLOB;
	request.generation = lease->generation;
	pthread_mutex_lock(&display_mutex);

	/* A retained image cannot acquire another plane reference after counter exhaustion. */
	if (image->holds == UINT32_MAX || lease->plane->frame == UINT64_MAX) {
		pthread_mutex_unlock(&display_mutex);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* Every selected frame advances this plane's nonzero native ordering domain. */
	request.frame = lease->plane->frame + 1U;

	/* The kernel returns only after checked native selection and flush have completed. */
	if (wait_fd >= 0 && lease->plane->completion_fd >= 0) {
		/* Only a completed previous selection permits reuse of this plane's signal payload generation. */
		if (lease->plane->completion_used != VK_FALSE) {
			/* Reset inputs contain only the exact owned generation, never earlier query output state. */
			memset(&reset, 0, sizeof(reset));
			reset.version = GPU_ABI_VERSION;
			reset.size = sizeof(reset);
			reset.fd = lease->plane->completion_fd;
			reset.generation = lease->plane->completion_generation;
			status = ioctl(image->connection->fd, GPU_FENCE_RESET, &reset);
			if (status != 0) {
				error = display_error(errno);
				pthread_mutex_unlock(&display_mutex);
				return error;
			}

			/* The next selection owns the newly pending native completion generation. */
			lease->plane->completion_generation = reset.generation;
		}

		/* The exact producer generation is checked before hardware access; selection signals a separate native payload. */
		memset(&synchronized, 0, sizeof(synchronized));
		synchronized.present = request;
		synchronized.wait_fd = wait_fd;
		synchronized.wait_generation = wait_generation;
		synchronized.signal_fd = lease->plane->completion_fd;
		synchronized.signal_generation = lease->plane->completion_generation;
		status = ioctl(image->connection->fd, GPU_DISPLAY_PRESENT_SYNC, &synchronized);

		/* Even a failed operation must advance or reject this consumed generation before reuse. */
		request = synchronized.present;
		lease->plane->completion_used = VK_TRUE;
	} else {
		/* The worker already proved actual producer completion for a backend without shared-fence capability. */
		status = ioctl(image->connection->fd, GPU_DISPLAY_PRESENT, &request);
	}

	/* A failed native selection keeps the previous front allocation retained. */
	if (status != 0) {
		error = display_error(errno);
		pthread_mutex_unlock(&display_mutex);
		return error;
	}

	/* Only successful selection advances the native frame ordinal. */
	lease->plane->frame = request.frame;

	/* Current blob ownership survives sequence completion until replacement or release. */
	previous = lease->plane->front;
	image->holds++;
	lease->plane->front = image;

	/* Checked replacement ends only the displaced image's independent native read hold. */
	if (previous != NULL)
		previous->holds--;

	/* The completed sequence describes selection, not reuse of the newly selected allocation. */
	*sequence = request.sequence;

	pthread_mutex_unlock(&display_mutex);

	/* Succeeded: the plane retains this GPU image and any displaced image may become reusable. */
	return VK_SUCCESS;
}

/* Reports whether every native plane has stopped retaining this image. */
static VkBool32
display_image_available(
	void *private_image)
{
	struct wsi_display_image *image;
	uint32_t holds;

	/* Plane replacement and acquisition observe the same protected reference count. */
	image = private_image;
	pthread_mutex_lock(&display_mutex);

	holds = image->holds;

	pthread_mutex_unlock(&display_mutex);

	/* A selected scanout still borrows storage even after its presentation fence has completed. */
	if (holds != 0U)
		return VK_FALSE;

	/* Succeeded: no native plane will read this allocation before another present. */
	return VK_TRUE;
}

/* Retires a swapchain-owned alias while kernel scanout references retain any current allocation. */
static void
display_destroy_image(
	void *private_image)
{
	struct wsi_display_image *image;
	struct wsi_display_plane *plane;
	struct gpu_resource_destroy request;
	struct vulkan_allocator allocator;
	int status;

	/* The swapchain retires native wrappers before releasing its surviving display lease. */
	image = private_image;
	allocator = image->connection->allocator;
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.handle = image->imported.handle;
	pthread_mutex_lock(&display_mutex);

	/* Software image identity may disappear while the kernel retains the current scanout allocation. */
	for (plane = display_planes; plane != NULL; plane = plane->next) {
		/* A later successful replacement must not dereference this retired wrapper. */
		if (plane->front == image)
			plane->front = NULL;
	}

	/* Native destruction consumes only this alias, never the kernel's separate scanout reference. */
	status = ioctl(image->connection->fd, GPU_RESOURCE_DESTROY, &request);
	if (status != 0)
		vulkan_device_error(image->connection->device, VK_ERROR_DEVICE_LOST);

	pthread_mutex_unlock(&display_mutex);

	/* Allocator callbacks cannot reenter while native ownership remains serialized. */
	vulkan_free(&allocator, image);

	/* Succeeded: no swapchain-owned alias or wrapper remains after its final image retirement. */
	return;
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
	pthread_mutex_lock(&display_mutex);

	status = ioctl(lease->plane->connection->fd, GPU_DISPLAY_WAIT, &request);
	error = errno;

	pthread_mutex_unlock(&display_mutex);

	if (status != 0) {
		/* A missing completed sequence has not yet reached the requested native boundary. */
		if (error == EAGAIN)
			return VK_NOT_READY;

		/* Reports expiration of the requested presentation completion interval. */
		if (error == ETIMEDOUT)
			return VK_TIMEOUT;
		error = display_error(error);
		return error;
	}

	/* Succeeded: native selection completed; the selected blob may remain in scanout use. */
	return VK_SUCCESS;
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

	/* Succeeded: unclassified native failures map conservatively to surface loss. */
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

	/* The saved output identity and generation must survive the preceding live topology validation. */
	error = vulkan_wsi_display_snapshot(surface->display_mode->display, &output);
	if (error != VK_SUCCESS)
		return error;

	/* Rejects presentation through a mode description invalidated by a topology event. */
	if (surface->display_mode->generation != output.generation)
		return VK_ERROR_OUT_OF_DATE_KHR;

	/* Succeeded: no native topology event invalidated this mode description. */
	return VK_SUCCESS;
}

/* Acquires the logical device's independent display open under native ownership serialization. */
static VkResult
display_connection_create(
	struct VkDevice_T *device,
	const struct vulkan_wsi_output *output,
	struct wsi_display_connection *candidate,
	struct wsi_display_connection **result)
{
	struct wsi_display_connection *connection;
	struct gpu_device_info identity;
	struct gpu_info info;
	int status;

	/* Existing planes share both the fd namespace and its lifetime reference. */
	for (connection = display_connections; connection != NULL; connection = connection->next) {
		/* A logical device cannot borrow another device's display admission state. */
		if (connection->device != device || connection->device_identifier != output->device_identifier)
			continue;

		/* Counter exhaustion must not create an unowned successful plane. */
		if (connection->references == UINT32_MAX)
			return VK_ERROR_OUT_OF_HOST_MEMORY;

		/* The new plane retains the existing display open independently. */
		connection->references++;
		*result = connection;
		return VK_SUCCESS;
	}

	/* The caller allocated this candidate before entering native ownership serialization. */
	connection = candidate;

	/* The selected discovery path names the GPU independently from any process fd number. */
	memset(connection, 0, sizeof(*connection));
	connection->device = device;
	connection->allocator = device->object.allocator;
	connection->device_identifier = output->device_identifier;
	connection->fd = open(output->device_path, O_RDWR | O_CLOEXEC);
	if (connection->fd < 0)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* A path reused after discovery must not claim an unrelated device with coincident local display IDs. */
	memset(&identity, 0, sizeof(identity));
	identity.version = GPU_ABI_VERSION;
	identity.size = sizeof(identity);
	status = ioctl(connection->fd, GPU_DEVICE_QUERY, &identity);
	if (status != 0 || identity.device_id != output->device_identifier) {
		close(connection->fd);
		return VK_ERROR_SURFACE_LOST_KHR;
	}

	/* The display node independently advertises support for typed native wait/signal requests. */
	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	status = ioctl(connection->fd, GPU_GET_INFO, &info);
	if (status != 0) {
		close(connection->fd);
		return VK_ERROR_SURFACE_LOST_KHR;
	}

	connection->capabilities = info.capabilities;

	/* Native import checks immutable allocation device identity before any scanout can use this fd. */
	connection->references = 1U;
	connection->next = display_connections;
	display_connections = connection;
	*result = connection;

	/* Succeeded: this plane owns one reference to its independent display open. */
	return VK_SUCCESS;
}

/* Ends one plane reference and closes the display namespace after its final owner retires. */
static struct wsi_display_connection *
display_connection_put(
	struct wsi_display_connection *connection)
{
	struct wsi_display_connection **link;

	/* Other planes preserve the same independent open and imported-image namespace. */
	connection->references--;
	if (connection->references != 0U)
		return NULL;

	/* Withdraw the exact connection before a future plane can open a new generation. */
	link = &display_connections;
	while (*link != connection)
		link = &(*link)->next;
	*link = connection->next;

	/* Descriptor close performs the kernel's ordinary scanout and alias lifetime cleanup. */
	close(connection->fd);

	/* Succeeded: the caller may free this retired connection outside native serialization. */
	return connection;
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

	/* Route preparation fixed this allocation before any semaphore-consuming job was accepted. */
	if (lease->storage == 0U || bytes != lease->storage_bytes) {
		pthread_mutex_unlock(&display_mutex);
		return VK_ERROR_INITIALIZATION_FAILED;
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
		transfer.handle = lease->storage;
		transfer.offset = offset;
		transfer.address = (uint64_t)(uintptr_t)(source + (size_t)offset);
		transfer.bytes = chunk;
		status = ioctl(lease->plane->connection->fd, GPU_RESOURCE_WRITE, &transfer);
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
	request.handle = lease->storage;
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
	status = ioctl(lease->plane->connection->fd, GPU_DISPLAY_PRESENT, &request);
	if (status != 0) {
		native_error = errno;
		pthread_mutex_unlock(&display_mutex);
		error = display_error(native_error);
		return error;
	}

	/* Successful copied selection also ends any earlier GPU-resident front image hold. */
	if (lease->plane->front != NULL) {
		lease->plane->front->holds--;
		lease->plane->front = NULL;
	}

	*sequence = request.sequence;

	pthread_mutex_unlock(&display_mutex);

	/* Succeeded: caller pixels are no longer borrowed by the native display. */
	return VK_SUCCESS;
}

/* Prepares the explicitly selected copied route before accepting presentation work. */
static VkResult
display_prepare_copy(
	void *private_lease,
	VkFormat format,
	VkExtent2D extent)
{
	struct wsi_display_lease *lease;
	struct gpu_resource_create request;
	uint64_t bytes;
	VkResult error;
	int status;

	/* A copied fallback must be an advertised display operation, not an assumed universal path. */
	lease = private_lease;
	if ((lease->constraints.flags & GPU_SCANOUT_COPY) == 0U)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* Native copied storage preserves only the two advertised packed channel formats. */
	if (format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_B8G8R8A8_UNORM)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* Extents are bounded by surface validation before this allocation request. */
	bytes = (uint64_t)extent.width * extent.height * 4U;
	pthread_mutex_lock(&display_mutex);

	if (lease->storage != 0U) {
		/* Existing storage can be reused only by an exactly matching copied extent. */
		status = 0;
		if (lease->storage_bytes == bytes)
			status = 1;
		pthread_mutex_unlock(&display_mutex);
		if (status == 0)
			return VK_ERROR_INITIALIZATION_FAILED;
		return VK_SUCCESS;
	}

	/* All native storage allocation happens at swapchain creation, once the import trial failed safely. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.bytes = bytes;
	request.usage = GPU_RESOURCE_USAGE_STORAGE;
	status = ioctl(lease->plane->connection->fd, GPU_RESOURCE_CREATE, &request);
	if (status != 0) {
		error = display_error(errno);
		pthread_mutex_unlock(&display_mutex);
		return error;
	}

	/* Replacement swapchains share the same surface-owned copied storage under the native mutex. */
	lease->storage = request.handle;
	lease->storage_bytes = bytes;

	pthread_mutex_unlock(&display_mutex);

	/* Succeeded: per-frame presentation will perform no allocation or route renegotiation. */
	return VK_SUCCESS;
}

/* Copies generation-specific physical conditions before any native image allocation begins. */
static VkResult
display_placement(
	void *private_lease,
	struct gpu_placement *placement)
{
	struct wsi_display_lease *lease;

	/* The live lease keeps its immutable creation-generation requirements through this trial. */
	lease = private_lease;
	memset(placement, 0, sizeof(*placement));
	placement->flags = lease->constraints.placement;
	placement->max_dma_address = lease->constraints.max_dma_address;

	/* Image offset and row pitch constraints are checked against renderer layout during import. */
	placement->alignment = 0U;

	/* Succeeded: WSI will submit these requirements to K before exposing a shared display image. */
	return VK_SUCCESS;
}

/* Retrieves immutable generation constraints before a swapchain chooses any storage route. */
static VkResult
display_constraints_query(
	struct VkPhysicalDevice_T *physical,
	const struct vulkan_wsi_output *output,
	struct gpu_scanout_constraints *request)
{
	VkResult error;
	int status;

	/* The native request describes exactly the generation used by the surface. */
	memset(request, 0, sizeof(*request));
	request->version = GPU_ABI_VERSION;
	request->size = sizeof(*request);
	request->display_id = (uint32_t)output->identifier;
	request->generation = output->generation;
	status = vulkan_wsi_display_node_ioctl(physical, output, GPU_DISPLAY_CONSTRAINTS, request);
	if (status != 0) {
		error = display_error(errno);
		return error;
	}

	/* Zero imposes no extra byte alignment; physical placement remains checked by the actual import operation. */
	if (request->stride_alignment == 0U)
		request->stride_alignment = 1U;

	/* An unconstrained image base likewise permits any byte offset validated by import. */
	if (request->offset_alignment == 0U)
		request->offset_alignment = 1U;

	/* Non-power-of-two alignments cannot define the advertised importable row layout. */
	if ((request->stride_alignment & (request->stride_alignment - 1U)) != 0U ||
	    (request->offset_alignment & (request->offset_alignment - 1U)) != 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* Succeeded: allocation and import may now be attempted against a concrete native contract. */
	return VK_SUCCESS;
}

/* Returns the lease-owned native fd whose errors and topology can wake acquisition. */
static int
display_wait_descriptor(
	void *private_lease)
{
	struct wsi_display_lease *lease;

	/* Valid acquisition retains its swapchain and therefore this independent display connection. */
	lease = private_lease;

	/* Succeeded: callers borrow the fd only while the native lease remains alive. */
	return lease->plane->connection->fd;
}

/* Observes native error domains and acknowledges only an inventoried topology snapshot. */
static VkResult
display_progress(
	void *private_lease)
{
	struct wsi_display_lease *lease;
	struct vulkan_context *context;
	struct gpu_display_events request;
	struct pollfd descriptors[2];
	uint64_t sequence;
	VkResult error;
	int status;

	/* Error readiness is independent of ordinary completion records retained by other observers. */
	lease = private_lease;
	context = lease->device->object.context;
	memset(descriptors, 0, sizeof(descriptors));
	descriptors[0].fd = context->fd;
	descriptors[1].fd = lease->plane->connection->fd;
	descriptors[1].events = POLLPRI;
	status = poll(descriptors, 2, 0);
	if (status < 0) {
		/* Interruption merely asks the acquisition loop to observe its predicates again. */
		if (errno == EINTR)
			return VK_SUCCESS;

		/* A failed observation cannot establish that the renderer is still usable. */
		vulkan_context_error(context, VK_ERROR_DEVICE_LOST);
		return VK_ERROR_DEVICE_LOST;
	}

	/* Renderer namespace failure dominates a simultaneous lost output on that device. */
	if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
		vulkan_context_error(context, VK_ERROR_DEVICE_LOST);
		return VK_ERROR_DEVICE_LOST;
	}

	/* An independent display node may disappear while the renderer remains usable. */
	if ((descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
		return VK_ERROR_SURFACE_LOST_KHR;

	/* Ordinary POLLIN records do not trigger repeated reaping or unrelated busy polling here. */
	if ((descriptors[1].revents & POLLPRI) == 0)
		return VK_SUCCESS;

	/* Topology serialization never waits for a presentation holding display_mutex. */
	pthread_mutex_lock(&display_event_mutex);

	/* QUERY records the exact observation which the following inventory is allowed to acknowledge. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	status = ioctl(lease->plane->connection->fd, GPU_DISPLAY_EVENTS, &request);
	if (status != 0) {
		error = display_error(errno);
		pthread_mutex_unlock(&display_event_mutex);
		return error;
	}

	/* Inventory refresh checks the saved surface generation before any acknowledgement. */
	sequence = request.sequence;
	error = display_validate_surface(lease->surface, lease->device->physical);
	if (error != VK_SUCCESS) {
		pthread_mutex_unlock(&display_event_mutex);
		return error;
	}

	/* Events arriving during inventory remain ready because ACK names only the earlier snapshot. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.flags = GPU_DISPLAY_EVENT_ACK;
	request.ack_sequence = sequence;
	status = ioctl(lease->plane->connection->fd, GPU_DISPLAY_EVENTS, &request);
	if (status != 0) {
		error = display_error(errno);
		pthread_mutex_unlock(&display_event_mutex);
		return error;
	}

	pthread_mutex_unlock(&display_event_mutex);

	/* Succeeded: this open no longer repeatedly wakes for the acknowledged inventory. */
	return VK_SUCCESS;
}
