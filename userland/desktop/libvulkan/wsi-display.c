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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
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

/*
 * One overlay-plane present held back for the primary present that shows it
 * (ws214-p004): a change between shown and hidden, or a later present to a
 * plane that already has one, which then replaces it.
 *
 * display_overlay_mutex protects it.  A held image carries one hold, which
 * goes back when the change is replaced or dropped and passes to the plane
 * when a commit records it.
 */
struct wsi_display_hold {
	VkBool32 held;
	/* The image's pixels are complete; only a ready change goes to the output. */
	VkBool32 ready;
	VkBool32 shown;
	struct wsi_display_image *image;
	VkRect2D source;
	VkRect2D destination;
	uint64_t serial;
	/*
	 * The first primary ticket that takes the change: the tickets given
	 * before the first change of the hold, plus one.  A replacement keeps
	 * it, and the deadline, of the change it replaces.
	 */
	uint64_t min_primary;
	/* The monotonic time after which, with no primary present to take it, the change is committed alone. */
	uint64_t deadline;
	/* The serial of the chain that presented it. */
	uint64_t owner;
	/* The monotonic time the present was called, for the overlay log. */
	uint64_t call_ns;
};

/*
 * One overlay plane of a claimed output, as this library has asked for it
 * (ws214-p004).  display_overlay_mutex protects every field.
 */
struct wsi_display_overlay {
	/* The lease of the surface presenting on the plane, or NULL. */
	struct wsi_display_lease *lease;
	/* The image the output's recorded state shows, which carries one hold. */
	struct wsi_display_image *front;
	/* Images a recorded state replaced, each holding until its record is adopted. */
	struct wsi_display_image *retiring;
	struct wsi_display_hold hold;
	/* The rectangles of the last shown present, used by a present that names none. */
	VkRect2D source;
	VkRect2D destination;
	VkBool32 placed;
	/* The last serial given to a present of the plane; serials restart with the claim, as the kernel's do. */
	uint64_t serial;
	/* The highest serial sent to the output: a commit with a lower one is dropped without asking. */
	uint64_t recorded_serial;
	/* Whether the plane shows once every present decided outside the hold is recorded. */
	VkBool32 shown;
	/* The chain whose held change was dropped, and what it reports from then on. */
	uint64_t failed_owner;
	VkResult failed_error;
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
	/* The output's mode generation the claim was made for, which every commit names. */
	uint64_t generation;
	/*
	 * The last ticket given to a primary present (atomically, under the
	 * swapchain mutex); a held overlay change takes this plus one as its
	 * min_primary.
	 */
	uint64_t primary_issued;
	/* The highest record number the output reported adopted with every lower one (display_overlay_mutex). */
	uint64_t adopted;
	/* One refresh period of the claimed mode, in nanoseconds. */
	uint64_t refresh_ns;
	/* The output's overlay planes (1 to overlay_count), zero when it has none. */
	uint32_t overlay_count;
	struct wsi_display_overlay overlays[GPU_DISPLAY_OVERLAY_MAX];
	/*
	 * The thread that commits a held change alone when no primary present
	 * takes it in time: started with the first held change, stopped and
	 * joined by the final release (display_overlay_mutex).
	 */
	pthread_t watcher;
	VkBool32 watching;
	VkBool32 watch_stop;
};

/*
 * One swapchain-owned imported image may be retained by several
 * shared-swapchain planes.  Holds count the planes that may read it: an
 * overlay image's under display_overlay_mutex, any other's changed under
 * display_mutex with atomic stores, so that acquisition reads them without
 * waiting for a present that holds display_mutex until its frame is taken.
 */
struct wsi_display_image {
	struct wsi_display_connection *connection;
	struct gpu_resource_import imported;
	uint32_t holds;
	/*
	 * An overlay plane's image (ws214-p004): its plane (1 and up, zero for
	 * any other image) and claim.  Its holds, and the two retiring fields,
	 * are protected by display_overlay_mutex rather than display_mutex.
	 */
	uint32_t overlay;
	struct wsi_display_plane *claim;
	/* While the image is retiring: the next retiring image and the record number whose adoption frees it. */
	struct wsi_display_image *retire_next;
	uint64_t retire_sequence;
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
	/* A cursor-plane lease borrows its output's primary claim. */
	VkBool32 cursor;
	/* An overlay-plane lease borrows it too; this is its plane (1 and up), zero for any other lease. */
	uint32_t overlay;
	/*
	 * Where the cursor was last shown, and the ticket of the present that
	 * showed it last (zero before the first); display_cursor_mutex protects
	 * all three.  A present whose ticket is not above it is older than what
	 * is shown, and is dropped.
	 */
	int32_t x;
	int32_t y;
	uint64_t applied_ticket;
};

/* Live planes are linked under display_mutex until their last surface lease retires. */
static struct wsi_display_plane *display_planes;

/* Each logical-device/native-node pair shares one open; display_mutex protects publication and holds. */
static struct wsi_display_connection *display_connections;

/* Serializes native claims and scanout ownership without holding renderer wire locks. */
static pthread_mutex_t display_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Event observation never waits behind native presentation or renderer admission. */
static pthread_mutex_t display_event_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Serializes the cursor presents of every cursor-plane lease: their
 * position, applied ticket, and the frame and sequence of their chains.
 * It is never held together with display_mutex, so a cursor is set and
 * moved while a primary present holds display_mutex for its synchronized
 * selection (ws141-p016 (B) review-B5).  A lease's other fields do not
 * change while a swapchain holds it.
 */
static pthread_mutex_t display_cursor_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Serializes every overlay plane's state, held changes and overlay image
 * holds (ws214-p004), and each overlay commit from the decision that names
 * its serial to the record, so the output records overlay states in the
 * order they were decided.  It may be taken while display_mutex is held,
 * never the other way round, and it is never held with display_cursor_mutex.
 */
static pthread_mutex_t display_overlay_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Signals, under display_overlay_mutex, that a held change was made,
 * replaced, made ready or dropped, or that a watcher is to stop: primary
 * presents waiting for ready changes and the watchers wait on it.
 */
static pthread_cond_t display_overlay_condition = PTHREAD_COND_INITIALIZER;

/*
 * Nonzero once an overlay surface was claimed while the overlay log's
 * environment variable was 1; it never returns to zero.
 */
static int display_overlay_log_on;

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
static VkResult display_present_cursor(void *private_lease, void *private_image, VkBool32 has_position, int32_t x, int32_t y, uint64_t ticket, uint64_t *frame, uint64_t *sequence, enum vulkan_wsi_cursor_outcome *outcome, struct vulkan_wsi_cursor_times *times);
static uint64_t display_primary_ticket(void *private_lease);
static VkResult display_present_primary(void *private_lease, void *private_image, VkPresentModeKHR mode, uint64_t *sequence, int wait_fd, uint64_t wait_generation, uint64_t ticket);
static void display_primary_skip(void *private_lease, uint64_t ticket, VkResult error);
static void display_overlay_decide(void *private_lease, void *private_image, struct vulkan_wsi_overlay_present *request);
static VkResult display_overlay_commit(void *private_lease, void *private_image, struct vulkan_wsi_overlay_present *request, enum vulkan_wsi_overlay_outcome *outcome);
static void display_overlay_ready(void *private_lease, uint64_t serial, VkResult error);
static VkResult display_overlay_status(void *private_lease, uint64_t owner);
static void display_overlay_forget(void *private_lease, uint64_t owner);
static VkResult display_overlay_reclaim(void *private_lease, uint64_t timeout);
static VkResult display_primary_commit(struct wsi_display_lease *lease, struct wsi_display_image *image, uint64_t *sequence, int wait_fd, uint64_t wait_generation, uint64_t ticket);
static void display_overlay_flush_locked(struct wsi_display_plane *plane, uint64_t ticket, VkBool32 expired_only, const char *why, uint64_t *sequence);
static void display_overlay_hide_locked(struct wsi_display_plane *plane, struct wsi_display_overlay *overlay, const char *why, uint64_t *sequence);
static void display_overlay_drop_locked(struct wsi_display_overlay *overlay, VkResult error);
static void display_overlay_image_retire(struct wsi_display_image *image);
static void display_overlay_ready_wait_locked(struct wsi_display_plane *plane, uint64_t ticket);
static void display_overlay_unretire_locked(struct wsi_display_overlay *overlay, struct wsi_display_image *image);
static void display_overlay_record_locked(struct wsi_display_overlay *overlay, VkBool32 shown, struct wsi_display_image *image, uint64_t sequence);
static VkBool32 display_overlay_reap_locked(struct wsi_display_plane *plane, VkBool32 every);
static void display_overlay_adopted_locked(struct wsi_display_plane *plane, uint64_t adopted);
static VkResult display_overlay_wait(struct wsi_display_plane *plane, uint64_t sequence, uint64_t timeout);
static void display_overlay_watch_start_locked(struct wsi_display_plane *plane);
static void *display_overlay_watch(void *argument);
static void display_overlay_state(struct gpu_display_plane_state *state, uint32_t plane, uint64_t serial, VkBool32 shown, const struct wsi_display_image *image, const VkRect2D *source, const VkRect2D *destination);
static void display_overlay_request(struct gpu_display_commit *request, const struct wsi_display_plane *plane);
static uint64_t display_overlay_now(void);
static void display_overlay_log(uint32_t plane, uint64_t serial, uint64_t ticket, uint64_t min_primary, const char *path, const char *why, int result, uint64_t sequence, uint64_t adopted, uint64_t call_ns);

static VkResult display_layouts(void *private_lease, struct gpu_image_layouts *request);

/* Future window-system adapters implement this same private presentation boundary. */
const struct vulkan_wsi_platform_ops vulkan_wsi_display_platform = {
	display_capabilities, display_formats, display_present_modes,
	display_claim_native, display_release_native, display_present_native,
	display_wait_native, NULL, display_import_image, display_present_image,
	display_progress, display_image_available, display_destroy_image, display_prepare_copy, display_present_image_sync, display_placement, display_wait_descriptor,
	NULL, NULL, NULL, display_present_cursor,
	display_primary_ticket, display_present_primary, display_primary_skip,
	display_overlay_decide, display_overlay_commit, display_overlay_ready,
	display_overlay_status, display_overlay_forget, display_overlay_reclaim,
	NULL,
	NULL, NULL, NULL,
	NULL, display_layouts
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

	/* Overlay planes (GPU_DISPLAY_HAS_OVERLAYS) follow the primary, below the cursor plane. */
	if ((request.flags & GPU_DISPLAY_HAS_OVERLAYS) != 0U && output->plane_count > 1U) {
		output->flags |= VULKAN_WSI_OUTPUT_OVERLAYS;
		output->overlay_count = output->plane_count - 1U;
	}

	/* A cursor plane follows the output's ordinary planes, on top of them. */
	if ((request.flags & GPU_DISPLAY_HAS_CURSOR) != 0U && output->plane_count != 0U && output->plane_count < UINT32_MAX) {
		output->flags |= VULKAN_WSI_OUTPUT_CURSOR;
		output->plane_count++;
	}

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

/*
 * Powers a display on or off for the logical device presenting to it.
 *
 * The native request needs this device's own lease on the display, so a
 * display the device has no swapchain on cannot be powered.  A request made
 * against an old generation is asked once more after the display's snapshot
 * is refreshed.
 */
VkResult
vulkan_wsi_display_power(
	struct VkDevice_T *device,
	struct vulkan_display *display,
	uint32_t state)
{
	struct wsi_display_plane *plane;
	struct vulkan_wsi_output output;
	struct gpu_display_power request;
	uint32_t attempt;
	VkResult error;
	int status;
	int native_error;

	/* Asks the display's lease, and once more after a generation change. */
	status = -1;
	native_error = EBUSY;
	for (attempt = 0U; attempt < 2U; attempt++) {
		/* The current identity and generation name the output to the kernel. */
		error = vulkan_wsi_display_snapshot(display, &output);
		if (error != VK_SUCCESS)
			return error;

		/* Finds this device's lease on the display and asks the kernel under it. */
		pthread_mutex_lock(&display_mutex);

		status = -1;
		native_error = EBUSY;
		for (plane = display_planes; plane != NULL; plane = plane->next) {
			/* Only this device's own claim on the same node's output holds the lease. */
			if (plane->device == device &&
			    plane->display == output.identifier &&
			    plane->connection->device_identifier == output.device_identifier)
				break;
		}

		/* The lease's open makes the request; no lease leaves EBUSY. */
		if (plane != NULL) {
			memset(&request, 0, sizeof(request));
			request.version = GPU_ABI_VERSION;
			request.size = sizeof(request);
			request.display_id = (uint32_t)output.identifier;
			request.state = state;
			request.generation = output.generation;
			status = ioctl(plane->connection->fd, GPU_DISPLAY_POWER, &request);
			native_error = errno;
		}

		pthread_mutex_unlock(&display_mutex);

		/* Only an old generation is worth asking again. */
		if (status == 0 || native_error != ESTALE)
			break;

		/* Takes the display's current generation before the second request. */
		error = vulkan_wsi_display_refresh(display);
		if (error != VK_SUCCESS)
			return error;
	}

	/* Reports a refused power change: no lease, no power control, or a lost output. */
	if (status != 0) {
		/* Kernel allocation pressure is the one failure the command names. */
		if (native_error == ENOMEM)
			return VK_ERROR_OUT_OF_HOST_MEMORY;

		/* Every other refusal is an unknown failure of this command. */
		return VK_ERROR_UNKNOWN;
	}

	/* Succeeded: the display is in the requested power state. */
	return VK_SUCCESS;
}

/*
 * Reads one overlay plane's limits on the output's current mode (GPU_DISPLAY_OVERLAY_INFO).
 */
VkResult
vulkan_wsi_display_overlay_info(
	struct VkPhysicalDevice_T *physical,
	const struct vulkan_wsi_output *output,
	uint32_t plane,
	struct gpu_display_overlay_info *info)
{
	int status;
	VkResult error;

	/* The output of its generation and one of its overlays; every output field starts zero. */
	memset(info, 0, sizeof(*info));
	info->version = GPU_ABI_VERSION;
	info->size = sizeof(*info);
	info->display_id = (uint32_t)output->identifier;
	info->generation = output->generation;
	info->plane = plane;
	status = vulkan_wsi_display_node_ioctl(physical, output, GPU_DISPLAY_OVERLAY_INFO, info);
	if (status != 0) {
		error = display_error(errno);
		return error;
	}

	/* Succeeded: the overlay's limits until the mode changes. */
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

	/*
	 * A primary plane's images may be sampled too: a compositor lends them
	 * to a client and composes them after the lend.  A direct chain's
	 * linear image and a copied chain's own image both allow it.
	 */
	if (surface->cursor == VK_FALSE && surface->overlay == VK_FALSE)
		capabilities->supportedUsageFlags |= VK_IMAGE_USAGE_SAMPLED_BIT;

	/*
	 * An overlay chain of two images may have both acquired at once, so a
	 * picture is copied into both by one submission (ws214-p004).
	 */
	if (surface->overlay != VK_FALSE)
		capabilities->minImageCount = 1U;

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
	struct wsi_display_overlay *overlay;
	const char *log_variable;
	uint32_t millihertz;
	VkBool32 taken;
	VkResult error;
	int status;
	int native_error;
	int log_compare;

	/* An overlay surface asks once whether its presents are to be logged; only the value 1 turns the log on. */
	*result = NULL;
	log_variable = NULL;
	if (surface->overlay != VK_FALSE)
		log_variable = getenv("VULKAN_ZEDBSD_OVERLAY_LOG");
	if (log_variable != NULL) {
		log_compare = strcmp(log_variable, "1");
		if (log_compare == 0)
			__atomic_store_n(&display_overlay_log_on, 1, __ATOMIC_RELAXED);
	}

	/* Native identity is captured under the common WSI snapshot lock. */
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

	/* Finds existing ownership of the same device display plane before claiming it again; a cursor or overlay plane borrows the first plane's. */
	while (plane != NULL) {
		/* Node identity keeps equal local display identifiers in separate ownership domains. */
		if (plane->device == device &&
		    plane->display == output.identifier &&
		    plane->connection->device_identifier == output.device_identifier &&
		    (plane->plane == surface->native_plane ||
		     ((surface->cursor != VK_FALSE || surface->overlay != VK_FALSE) && plane->plane == 0U)))
			break;
		plane = plane->next;
	}

	/* A cursor or overlay plane has no claim of its own: the output's first plane must be claimed by this device. */
	if ((surface->cursor != VK_FALSE || surface->overlay != VK_FALSE) && plane == NULL) {
		pthread_mutex_unlock(&display_mutex);
		vulkan_free(&device->object.allocator, new_plane);
		vulkan_free(&device->object.allocator, new_connection);
		vulkan_free(&surface->object.allocator, candidate);
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	/* An overlay plane is one the claimed output has, presented by one surface at a time. */
	if (surface->overlay != VK_FALSE) {
		/* A plane past the claim's overlays was not enumerated with this claim. */
		if (surface->native_plane == 0U || surface->native_plane > plane->overlay_count) {
			pthread_mutex_unlock(&display_mutex);
			vulkan_free(&device->object.allocator, new_plane);
			vulkan_free(&device->object.allocator, new_connection);
			vulkan_free(&surface->object.allocator, candidate);
			return VK_ERROR_INITIALIZATION_FAILED;
		}

		/* Another surface presenting on the same overlay plane keeps it. */
		overlay = &plane->overlays[surface->native_plane - 1U];
		pthread_mutex_lock(&display_overlay_mutex);

		taken = VK_FALSE;
		if (overlay->lease != NULL)
			taken = VK_TRUE;

		pthread_mutex_unlock(&display_overlay_mutex);

		/* Refuses a plane another surface presents on. */
		if (taken != VK_FALSE) {
			pthread_mutex_unlock(&display_mutex);
			vulkan_free(&device->object.allocator, new_plane);
			vulkan_free(&device->object.allocator, new_connection);
			vulkan_free(&surface->object.allocator, candidate);
			return VK_ERROR_NATIVE_WINDOW_IN_USE_KHR;
		}
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

			/*
			 * The limit of outputs shown at once is not a lost surface: the
			 * swapchain cannot be made now and may be after another output
			 * is released or the topology changes, so the caller keeps its
			 * device and its other outputs (D-LIMIT).
			 */
			if (native_error == ENOSPC)
				error = VK_ERROR_INITIALIZATION_FAILED;

			/* Reports why the display could not be claimed. */
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
		plane->generation = request.generation;
		plane->next = display_planes;
		display_planes = plane;

		/* The output's overlay planes, as many as one commit can name. */
		plane->overlay_count = output.overlay_count;
		if (plane->overlay_count > GPU_DISPLAY_OVERLAY_MAX)
			plane->overlay_count = GPU_DISPLAY_OVERLAY_MAX;

		/* One refresh of the claimed mode, or of 60 Hz when it names no rate, times a held change's deadline. */
		plane->refresh_ns = 16666667U;
		millihertz = surface->display_mode->parameters.refreshRate;
		if (millihertz != 0U)
			plane->refresh_ns = 1000000000000ULL / millihertz;
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
	candidate->cursor = surface->cursor;
	candidate->overlay = 0U;
	surface->platform_private = candidate;
	*result = candidate;

	/* An overlay lease is the one its plane's presents and dropped changes are reported to. */
	if (surface->overlay != VK_FALSE) {
		candidate->overlay = surface->native_plane;
		pthread_mutex_lock(&display_overlay_mutex);

		plane->overlays[surface->native_plane - 1U].lease = candidate;

		pthread_mutex_unlock(&display_overlay_mutex);
	}

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
	struct gpu_display_cursor_hide hide;
	struct gpu_resource_destroy destroy;
	struct vulkan_allocator allocator;
	struct wsi_display_overlay *overlay;
	uint64_t sequence;
	uint32_t index;
	VkBool32 final_plane;
	VkBool32 watching;
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

	/* A cursor goes with its surface; the primary claim it borrowed may stay. */
	if (lease->cursor != VK_FALSE && final_plane == VK_FALSE) {
		memset(&hide, 0, sizeof(hide));
		hide.version = GPU_ABI_VERSION;
		hide.size = sizeof(hide);
		hide.lease = plane->identifier;
		hide.generation = lease->generation;
		status = ioctl(plane->connection->fd, GPU_DISPLAY_CURSOR_HIDE, &hide);
		if (status != 0 && errno != ESTALE)
			error = display_error(errno);
	}

	/*
	 * An overlay plane goes with its surface: its held change is dropped
	 * and a shown plane is hidden (its images were destroyed already, so
	 * normally nothing shows), and the plane takes another surface.
	 */
	if (lease->overlay != 0U) {
		overlay = &plane->overlays[lease->overlay - 1U];
		pthread_mutex_lock(&display_overlay_mutex);

		display_overlay_drop_locked(overlay, VK_SUCCESS);

		/* A plane still shown is hidden while the claim stays; a final release hides it anyway. */
		if (overlay->front != NULL && final_plane == VK_FALSE)
			display_overlay_hide_locked(plane, overlay, "release", &sequence);

		/* The plane may be presented by another surface from now on. */
		if (overlay->lease == lease)
			overlay->lease = NULL;
		pthread_cond_broadcast(&display_overlay_condition);

		pthread_mutex_unlock(&display_overlay_mutex);
	}

	/* The watcher stops before the claim it commits through is released. */
	if (final_plane != VK_FALSE) {
		pthread_mutex_lock(&display_overlay_mutex);

		plane->watch_stop = VK_TRUE;
		watching = plane->watching;
		pthread_cond_broadcast(&display_overlay_condition);

		pthread_mutex_unlock(&display_overlay_mutex);

		/* The watcher takes only display_overlay_mutex, so it ends while display_mutex is held here. */
		if (watching != VK_FALSE)
			(void)pthread_join(plane->watcher, NULL);
	}

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
			__atomic_sub_fetch(&plane->front->holds, 1U, __ATOMIC_RELEASE);
			plane->front = NULL;
		}

		/* The output no longer reads any overlay image, whatever the release reported. */
		pthread_mutex_lock(&display_overlay_mutex);

		for (index = 0U; index < plane->overlay_count; index++) {
			/* Drops the plane's held change, and the hold of the image it showed. */
			overlay = &plane->overlays[index];
			display_overlay_drop_locked(overlay, VK_SUCCESS);

			/* The shown image is no longer read. */
			if (overlay->front != NULL) {
				overlay->front->holds--;
				overlay->front = NULL;
			}
		}
		(void)display_overlay_reap_locked(plane, VK_TRUE);

		pthread_mutex_unlock(&display_overlay_mutex);

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
	if (lease->device->object.context->copy_display != VK_FALSE)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

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
	image->overlay = lease->overlay;
	image->claim = lease->plane;
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

	/* A cursor or overlay plane is shown only through its own present, which knows the position and ticket. */
	if (lease->cursor != VK_FALSE || lease->overlay != 0U)
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
	if (__atomic_load_n(&image->holds, __ATOMIC_ACQUIRE) == UINT32_MAX || lease->plane->frame == UINT64_MAX) {
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

	/*
	 * Only successful selection advances the native frame ordinal.  A
	 * cursor present reads it without display_mutex, so it is stored whole.
	 */
	__atomic_store_n(&lease->plane->frame, request.frame, __ATOMIC_RELEASE);

	/* Current blob ownership survives sequence completion until replacement or release. */
	previous = lease->plane->front;
	__atomic_add_fetch(&image->holds, 1U, __ATOMIC_RELEASE);
	lease->plane->front = image;

	/* Checked replacement ends only the displaced image's independent native read hold. */
	if (previous != NULL)
		__atomic_sub_fetch(&previous->holds, 1U, __ATOMIC_RELEASE);

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
	if (image->overlay != 0U) {
		/* An overlay image's holds are the overlay lock's, so a primary present does not delay this. */
		pthread_mutex_lock(&display_overlay_mutex);

		holds = image->holds;

		pthread_mutex_unlock(&display_overlay_mutex);
	} else {
		/*
		 * Any other image's holds change under display_mutex, which a
		 * primary present keeps until its frame is taken: read without it,
		 * so the next image is acquired while the frame before waits for
		 * its blank.  A hold is only ever added to an image being
		 * presented, which is not available anyway.
		 */
		holds = __atomic_load_n(&image->holds, __ATOMIC_ACQUIRE);
	}

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

	/* An overlay image is first taken out of the plane, which then no longer shows it (ws214-p004). */
	image = private_image;
	if (image->overlay != 0U)
		display_overlay_image_retire(image);

	/* The swapchain retires native wrappers before releasing its surviving display lease. */
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

	/* A cursor or overlay plane takes only shared images; it has no copied-pixel route. */
	if (lease->cursor != VK_FALSE || lease->overlay != 0U)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* The copied frame's size, against the storage prepared for it. */
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
		__atomic_sub_fetch(&lease->plane->front->holds, 1U, __ATOMIC_RELEASE);
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

	/* An overlay plane shows only shared images, so its chain has no copied route. */
	if (lease->overlay != 0U)
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

/*
 * Asks the lease's display node which layouts of an image it shows (the
 * request's format, width and height filled in by the caller): the
 * display's identity and the scanout use are added here.  Returns
 * VK_SUCCESS with the answer, or the error of a node that does not answer
 * (an older kernel's).
 */
static VkResult
display_layouts(
	void *private_lease,
	struct gpu_image_layouts *request)
{
	struct wsi_display_lease *lease;
	struct vulkan_wsi_output output;
	VkResult error;
	int status;

	/* The output the lease shows, as its surface's display is now. */
	lease = private_lease;
	error = vulkan_wsi_display_snapshot(lease->surface->display_mode->display, &output);
	if (error != VK_SUCCESS)
		return error;

	/* The question, for this display and its scanout. */
	request->version = GPU_ABI_VERSION;
	request->size = sizeof(*request);
	request->uses = GPU_LAYOUT_SCANOUT;
	request->display_id = (uint32_t)output.identifier;
	request->count = 0U;
	status = vulkan_wsi_display_node_ioctl(lease->device->physical, &output, GPU_IMAGE_LAYOUTS, request);
	if (status != 0)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* Succeeded: the display's layouts of the image. */
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

/*
 * Shows a completed image on the output's cursor plane, unless a later
 * present was shown already.
 *
 * The kernel copies the image at once, so the plane keeps no hold on it and
 * the swapchain may reuse it as soon as this returns.  Only
 * display_cursor_mutex is held, never display_mutex.
 */
static VkResult
display_present_cursor(
	void *private_lease,
	void *private_image,
	VkBool32 has_position,
	int32_t x,
	int32_t y,
	uint64_t ticket,
	uint64_t *frame,
	uint64_t *sequence,
	enum vulkan_wsi_cursor_outcome *outcome,
	struct vulkan_wsi_cursor_times *times)
{
	struct wsi_display_lease *lease;
	struct wsi_display_image *image;
	struct gpu_display_cursor_image request;
	struct gpu_display_cursor_move move;
	const struct gpu_image_descriptor *descriptor;
	VkResult error;
	int status;
	int native_error;

	/* Only a cursor plane's image, from the lease's own display open. */
	lease = private_lease;
	image = private_image;
	*outcome = VULKAN_WSI_CURSOR_DROPPED;
	if (lease->cursor == VK_FALSE || image->connection != lease->plane->connection)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* The image, as the display open imported it. */
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
	request.generation = lease->generation;

	/* The move that shows it; its position is chosen under the cursor's lock. */
	memset(&move, 0, sizeof(move));
	move.version = GPU_ABI_VERSION;
	move.size = sizeof(move);
	move.lease = lease->identifier;
	move.generation = lease->generation;

	/* Decides, sets and moves as one step against every other cursor present. */
	pthread_mutex_lock(&display_cursor_mutex);

	/* The time the lock was held, for the cursor log. */
	if (times != NULL)
		times->locked = vulkan_wsi_cursor_clock();

	/* A present at or below the applied ticket is older than what is shown. */
	if (ticket <= lease->applied_ticket) {
		pthread_mutex_unlock(&display_cursor_mutex);
		return VK_SUCCESS;
	}

	/* A chain's frame ordinal cannot advance past its last value. */
	if (*frame == UINT64_MAX) {
		pthread_mutex_unlock(&display_cursor_mutex);
		return VK_ERROR_DEVICE_LOST;
	}

	/* The present's position, or where the cursor was last shown. */
	move.x = lease->x;
	move.y = lease->y;
	if (has_position != VK_FALSE) {
		move.x = x;
		move.y = y;
	}

	/* The time just before the image is set, for the cursor log. */
	if (times != NULL)
		times->set = vulkan_wsi_cursor_clock();

	/* The image first. */
	status = ioctl(lease->plane->connection->fd, GPU_DISPLAY_CURSOR_SET, &request);
	native_error = errno;
	if (status != 0) {
		/*
		 * EBUSY while this open holds the lease means every cursor image
		 * is named by a list the hardware has not yet left; nothing
		 * changed, and the caller may try again a refresh later.
		 */
		if (native_error == EBUSY) {
			*outcome = VULKAN_WSI_CURSOR_BUSY;
			pthread_mutex_unlock(&display_cursor_mutex);
			return VK_SUCCESS;
		}

		/* The kernel keeps showing the earlier image where it was. */
		error = display_error(native_error);
		pthread_mutex_unlock(&display_cursor_mutex);
		return error;
	}

	/* The new image is shown, so no older present may replace it, even if the move fails. */
	lease->applied_ticket = ticket;

	/* Then the position that shows it. */
	status = ioctl(lease->plane->connection->fd, GPU_DISPLAY_CURSOR_MOVE, &move);
	native_error = errno;

	/* The time the move returned, for the cursor log. */
	if (times != NULL)
		times->done = vulkan_wsi_cursor_clock();

	/* The cursor stays where it was when the move fails. */
	if (status != 0) {
		error = display_error(native_error);
		pthread_mutex_unlock(&display_cursor_mutex);
		return error;
	}

	/*
	 * The cursor is where this present put it.  The chain names one more
	 * frame, and its sequence is the plane's frame, which only primary
	 * presents advance.
	 */
	lease->x = move.x;
	lease->y = move.y;
	(*frame)++;
	*sequence = __atomic_load_n(&lease->plane->frame, __ATOMIC_ACQUIRE);
	*outcome = VULKAN_WSI_CURSOR_SHOWN;

	pthread_mutex_unlock(&display_cursor_mutex);

	/* Succeeded: the cursor shows from a coming refresh boundary. */
	return VK_SUCCESS;
}

/*
 * Gives a primary-plane present its ticket (ws214-p004).
 *
 * The swapchain asks under its mutex while it reserves the present's images,
 * so tickets follow the order presents are called.  A cursor or overlay
 * lease, and a primary whose output has no overlay planes, takes none.
 */
static uint64_t
display_primary_ticket(
	void *private_lease)
{
	struct wsi_display_lease *lease;
	uint64_t ticket;

	/* Only the primary of an output with overlay planes takes held changes. */
	lease = private_lease;
	if (lease->cursor != VK_FALSE || lease->overlay != 0U)
		return 0U;
	if (lease->plane->overlay_count == 0U)
		return 0U;

	/* The next ticket; a held change made later takes this one plus one as its min_primary. */
	ticket = __atomic_add_fetch(&lease->plane->primary_issued, 1U, __ATOMIC_ACQ_REL);

	/* Succeeded: the present's ticket. */
	return ticket;
}

/*
 * Presents a primary image, with every held overlay change its ticket takes.
 *
 * Without such a change the present is the ordinary synchronized selection,
 * so an output without overlays, and every present between overlay changes,
 * goes the way it always did.
 */
static VkResult
display_present_primary(
	void *private_lease,
	void *private_image,
	VkPresentModeKHR mode,
	uint64_t *sequence,
	int wait_fd,
	uint64_t wait_generation,
	uint64_t ticket)
{
	struct wsi_display_lease *lease;
	struct wsi_display_image *image;
	struct wsi_display_plane *plane;
	struct wsi_display_hold *hold;
	uint32_t index;
	VkBool32 takes;
	VkResult error;

	/* The checks of a synchronized selection come first. */
	lease = private_lease;
	image = private_image;
	plane = lease->plane;
	if (mode != VK_PRESENT_MODE_FIFO_KHR)
		return VK_ERROR_INITIALIZATION_FAILED;
	if (image->connection != plane->connection)
		return VK_ERROR_INITIALIZATION_FAILED;
	if (lease->cursor != VK_FALSE || lease->overlay != 0U)
		return VK_ERROR_INITIALIZATION_FAILED;

	/*
	 * Looks for a held change this ticket takes.  One made after this
	 * look has a min_primary above the ticket, which was given before it.
	 */
	takes = VK_FALSE;
	if (ticket != 0U) {
		pthread_mutex_lock(&display_overlay_mutex);

		for (index = 0U; index < plane->overlay_count; index++) {
			/* A held change whose first primary ticket is this one or an earlier one. */
			hold = &plane->overlays[index].hold;
			if (hold->held != VK_FALSE && hold->min_primary <= ticket)
				takes = VK_TRUE;
		}

		pthread_mutex_unlock(&display_overlay_mutex);
	}

	/* Without one, the ordinary synchronized selection. */
	if (takes == VK_FALSE) {
		error = display_present_image_sync(private_lease, private_image, mode, sequence, wait_fd, wait_generation);
		if (error != VK_SUCCESS)
			return error;

		/* Succeeded: the primary is selected as before. */
		return VK_SUCCESS;
	}

	/* With one, the primary and the held changes go in one commit. */
	error = display_primary_commit(lease, image, sequence, wait_fd, wait_generation, ticket);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the primary and the changes show from one refresh boundary. */
	return VK_SUCCESS;
}

/*
 * Commits a primary image together with the held overlay changes its ticket
 * takes, then waits for the adoption as a synchronized selection does.
 *
 * A commit the output refuses for an overlay's sake (ENOSPC, EINVAL) drops
 * every taken change, whose chains are then out of date, and leaves the
 * primary unshown, so the screen keeps its earlier, complete picture
 * (ws214-p001 N10); the present itself still succeeds.
 */
static VkResult
display_primary_commit(
	struct wsi_display_lease *lease,
	struct wsi_display_image *image,
	uint64_t *sequence,
	int wait_fd,
	uint64_t wait_generation,
	uint64_t ticket)
{
	struct gpu_display_commit request;
	struct wsi_display_plane *plane;
	struct wsi_display_overlay *overlay;
	struct wsi_display_hold *hold;
	struct wsi_display_image *previous;
	const struct gpu_image_descriptor *descriptor;
	VkRect2D whole;
	uint32_t taken[GPU_DISPLAY_OVERLAY_MAX];
	uint32_t count;
	uint32_t index;
	uint32_t at;
	const char *why;
	VkResult error;
	VkResult dropped;
	int status;
	int native_error;

	/* The primary covers the whole output from its whole image. */
	plane = lease->plane;
	descriptor = &image->imported.image;
	memset(&whole, 0, sizeof(whole));
	whole.extent.width = descriptor->width;
	whole.extent.height = descriptor->height;
	pthread_mutex_lock(&display_mutex);

	/* A retained image cannot acquire another plane reference after counter exhaustion. */
	if (__atomic_load_n(&image->holds, __ATOMIC_ACQUIRE) == UINT32_MAX || plane->frame == UINT64_MAX) {
		pthread_mutex_unlock(&display_mutex);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	/* The taken changes are ready, and the commit names them with the primary. */
	pthread_mutex_lock(&display_overlay_mutex);

	display_overlay_ready_wait_locked(plane, ticket);

	/* The primary's new state, after its producer's fence. */
	display_overlay_request(&request, plane);
	request.generation = lease->generation;
	display_overlay_state(&request.planes[0], GPU_DISPLAY_PLANE_PRIMARY, ticket, VK_TRUE, image, &whole, &whole);

	/* The kernel checks the producer's fence again before the record, as for a synchronized selection. */
	if (wait_fd >= 0) {
		request.planes[0].wait_fd = wait_fd;
		request.planes[0].wait_generation = wait_generation;
	}
	count = 1U;

	/* Each held change this ticket takes. */
	for (index = 0U; index < plane->overlay_count; index++) {
		/* Only a held change whose first primary ticket is this one or earlier. */
		hold = &plane->overlays[index].hold;
		if (hold->held == VK_FALSE || hold->min_primary > ticket)
			continue;

		display_overlay_state(&request.planes[count], index + 1U, hold->serial, hold->shown, hold->image, &hold->source, &hold->destination);
		taken[count - 1U] = index;
		count++;
	}
	request.count = count;

	/* Records the primary and the changes at once; the adoption is waited for below. */
	status = ioctl(plane->connection->fd, GPU_DISPLAY_COMMIT, &request);
	native_error = errno;

	/* A refused commit leaves the output as it was and drops every taken change. */
	if (status != 0) {
		/* An overlay the output cannot show keeps the earlier picture; any other refusal is the primary's too. */
		why = "error";
		dropped = display_error(native_error);
		if (native_error == ENOSPC) {
			why = "nospace";
			dropped = VK_ERROR_OUT_OF_DATE_KHR;
		} else if (native_error == EINVAL) {
			why = "invalid";
			dropped = VK_ERROR_OUT_OF_DATE_KHR;
		}

		/* Every taken change is dropped, and its chain reports why. */
		for (at = 0U; at + 1U < count; at++) {
			overlay = &plane->overlays[taken[at]];
			display_overlay_log(taken[at] + 1U, overlay->hold.serial, ticket, overlay->hold.min_primary, "commit", why, (int)dropped, 0U, plane->adopted, overlay->hold.call_ns);
			display_overlay_drop_locked(overlay, dropped);
		}
		pthread_cond_broadcast(&display_overlay_condition);

		pthread_mutex_unlock(&display_overlay_mutex);
		pthread_mutex_unlock(&display_mutex);

		vulkan_wsi_image_notify();

		/* The primary is not shown; its chain goes on unless the refusal was its own. */
		if (native_error == ENOSPC || native_error == EINVAL)
			return VK_SUCCESS;

		/* Reports why the primary could not be shown. */
		return dropped;
	}

	/* Each taken change is recorded, or was overtaken by a later one. */
	display_overlay_adopted_locked(plane, request.adopted);
	for (at = 0U; at + 1U < count; at++) {
		/* The plane follows a recorded change, whose image hold passes to the plane. */
		overlay = &plane->overlays[taken[at]];
		hold = &overlay->hold;
		display_overlay_log(taken[at] + 1U, hold->serial, ticket, hold->min_primary, "commit", "none", (int)request.planes[at + 1U].result, request.sequence, request.adopted, hold->call_ns);
		if (request.planes[at + 1U].result == GPU_DISPLAY_PLANE_APPLIED) {
			display_overlay_record_locked(overlay, hold->shown, hold->image, request.sequence);
			overlay->shown = hold->shown;
			if (hold->serial > overlay->recorded_serial)
				overlay->recorded_serial = hold->serial;
			memset(hold, 0, sizeof(*hold));
		} else {
			/* An overtaken change gives its image back at once. */
			display_overlay_drop_locked(overlay, VK_SUCCESS);
		}
	}
	(void)display_overlay_reap_locked(plane, VK_FALSE);
	pthread_cond_broadcast(&display_overlay_condition);

	pthread_mutex_unlock(&display_overlay_mutex);

	/*
	 * A recorded primary advances the frame ordinal (a cursor present reads
	 * it without display_mutex, so it is stored whole) and borrows its image
	 * in place of the one it replaced.
	 */
	if (request.planes[0].result == GPU_DISPLAY_PLANE_APPLIED) {
		__atomic_store_n(&plane->frame, plane->frame + 1U, __ATOMIC_RELEASE);
		previous = plane->front;
		__atomic_add_fetch(&image->holds, 1U, __ATOMIC_RELEASE);
		plane->front = image;
		if (previous != NULL)
			__atomic_sub_fetch(&previous->holds, 1U, __ATOMIC_RELEASE);
	}
	*sequence = request.sequence;

	/* The present returns once the output shows the record, as a synchronized selection does. */
	error = display_overlay_wait(plane, request.sequence, 1000000000ULL);

	pthread_mutex_unlock(&display_mutex);

	vulkan_wsi_image_notify();

	/* An adoption that did not come in a second loses the output as a selection's would. */
	if (error == VK_TIMEOUT || error == VK_NOT_READY)
		return VK_ERROR_DEVICE_LOST;

	/* Reports a lost lease or output during the wait. */
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the output shows the primary and the changes together. */
	return VK_SUCCESS;
}

/*
 * Tells the claim that the primary present of a ticket did not reach the
 * output through display_present_primary.
 *
 * With VK_SUCCESS (a copied-route primary) the held changes it would have
 * taken are committed alone; with an error they are dropped and their
 * chains are out of date (ws214-p001 4.4 (6)).
 */
static void
display_primary_skip(
	void *private_lease,
	uint64_t ticket,
	VkResult error)
{
	struct wsi_display_lease *lease;
	struct wsi_display_plane *plane;
	struct wsi_display_hold *hold;
	uint64_t sequence;
	uint32_t index;

	/* A present without a ticket took no changes. */
	lease = private_lease;
	if (ticket == 0U)
		return;
	plane = lease->plane;
	pthread_mutex_lock(&display_overlay_mutex);

	/* The changes are ready before either outcome. */
	display_overlay_ready_wait_locked(plane, ticket);

	/* A primary that went its own way leaves the changes to be committed alone. */
	if (error == VK_SUCCESS) {
		display_overlay_flush_locked(plane, ticket, VK_FALSE, "flush", &sequence);
	} else {
		/* A failed primary drops every change it would have taken. */
		for (index = 0U; index < plane->overlay_count; index++) {
			/* A held change whose first primary ticket is this one or earlier. */
			hold = &plane->overlays[index].hold;
			if (hold->held == VK_FALSE || hold->min_primary > ticket)
				continue;
			display_overlay_log(index + 1U, hold->serial, ticket, hold->min_primary, "commit", "result", (int)error, 0U, plane->adopted, hold->call_ns);
			display_overlay_drop_locked(&plane->overlays[index], VK_ERROR_OUT_OF_DATE_KHR);
		}
	}
	pthread_cond_broadcast(&display_overlay_condition);

	pthread_mutex_unlock(&display_overlay_mutex);

	vulkan_wsi_image_notify();

	/* Succeeded: no change waits for the ticket any more. */
	return;
}

/*
 * Decides how an overlay-plane present reaches the output (ws214-p004).
 *
 * A change between shown and hidden, judged against the plane as every
 * present decided so far leaves it, is held for the first primary present
 * called after it; a present to a plane with a held change replaces that
 * change, keeping its min_primary and deadline; any other present is
 * committed alone by display_overlay_commit.  The plane's serial is given
 * here, in the order presents are called.
 */
static void
display_overlay_decide(
	void *private_lease,
	void *private_image,
	struct vulkan_wsi_overlay_present *request)
{
	struct wsi_display_lease *lease;
	struct wsi_display_image *image;
	struct wsi_display_plane *plane;
	struct wsi_display_overlay *overlay;
	struct wsi_display_hold *hold;
	const char *path;
	uint64_t now;
	VkBool32 shown;

	/* The plane, and the time a new held change starts its deadline from. */
	lease = private_lease;
	image = private_image;
	plane = lease->plane;
	overlay = &plane->overlays[lease->overlay - 1U];
	hold = &overlay->hold;
	shown = VK_TRUE;
	if (request->hide != VK_FALSE)
		shown = VK_FALSE;
	now = display_overlay_now();
	pthread_mutex_lock(&display_overlay_mutex);

	/* The present's serial, after every present decided before it. */
	overlay->serial++;
	request->serial = overlay->serial;
	request->held = VK_FALSE;

	/* A shown present names its rectangles, keeps the last ones, or shows the whole image at the origin. */
	if (shown != VK_FALSE) {
		if (request->has_rect == VK_FALSE && overlay->placed != VK_FALSE) {
			request->source = overlay->source;
			request->destination = overlay->destination;
		} else if (request->has_rect == VK_FALSE) {
			memset(&request->source, 0, sizeof(request->source));
			request->source.extent.width = image->imported.image.width;
			request->source.extent.height = image->imported.image.height;
			request->destination = request->source;
		}
		overlay->source = request->source;
		overlay->destination = request->destination;
		overlay->placed = VK_TRUE;
	}

	/* A plane with a held change, or a change of shown and hidden, holds the present. */
	path = NULL;
	if (hold->held != VK_FALSE) {
		/* The replaced change's image is free at once: the output never named it. */
		if (hold->image != NULL)
			hold->image->holds--;
		hold->image = NULL;
		path = "replaced";
	} else if (shown != overlay->shown) {
		/* A new change waits for the primary presents called after it, or for its deadline. */
		hold->held = VK_TRUE;
		hold->min_primary = __atomic_load_n(&plane->primary_issued, __ATOMIC_ACQUIRE) + 1U;
		hold->deadline = now + 2U * plane->refresh_ns;
		path = "held";
		display_overlay_watch_start_locked(plane);
	}

	/* The held change is this present now; its image holds until it is recorded or dropped. */
	if (path != NULL) {
		hold->ready = request->ready;
		hold->shown = shown;
		hold->serial = request->serial;
		hold->owner = request->owner;
		hold->call_ns = request->call_ns;
		hold->source = request->source;
		hold->destination = request->destination;
		if (shown != VK_FALSE) {
			hold->image = image;
			image->holds++;
		}
		request->held = VK_TRUE;
		display_overlay_log(lease->overlay, hold->serial, 0U, hold->min_primary, path, request->why, 0, 0U, plane->adopted, request->call_ns);
		pthread_cond_broadcast(&display_overlay_condition);
	}

	pthread_mutex_unlock(&display_overlay_mutex);

	/* A replaced change's image may be acquired again. */
	if (path != NULL)
		vulkan_wsi_image_notify();

	/* Succeeded: the present is held, or ready for its own commit. */
	return;
}

/*
 * Commits a decided overlay present that was not held, alone, without
 * waiting for the adoption.
 *
 * A present overtaken by a later recorded one is dropped without asking.
 * BUSY means the output could not record the change for now (all its places
 * for replaced images are taken); nothing changed and the caller may ask
 * again a refresh later.  ENOSPC and EINVAL make only this chain out of date.
 */
static VkResult
display_overlay_commit(
	void *private_lease,
	void *private_image,
	struct vulkan_wsi_overlay_present *request,
	enum vulkan_wsi_overlay_outcome *outcome)
{
	struct gpu_display_commit commit;
	struct wsi_display_lease *lease;
	struct wsi_display_image *image;
	struct wsi_display_plane *plane;
	struct wsi_display_overlay *overlay;
	const char *path;
	const char *why;
	VkBool32 shown;
	VkResult error;
	int status;
	int native_error;

	/* The plane, and the log's name for where the commit runs. */
	lease = private_lease;
	image = private_image;
	plane = lease->plane;
	overlay = &plane->overlays[lease->overlay - 1U];
	*outcome = VULKAN_WSI_OVERLAY_DROPPED;
	path = "fast";
	if (request->on_worker != VK_FALSE)
		path = "lane";
	shown = VK_TRUE;
	if (request->hide != VK_FALSE)
		shown = VK_FALSE;
	pthread_mutex_lock(&display_overlay_mutex);

	/* A later present of the plane was sent already: this one would only be dropped. */
	if (request->serial <= overlay->recorded_serial) {
		display_overlay_log(lease->overlay, request->serial, 0U, 0U, path, request->why, (int)GPU_DISPLAY_PLANE_DROPPED, 0U, plane->adopted, request->call_ns);
		pthread_mutex_unlock(&display_overlay_mutex);
		return VK_SUCCESS;
	}

	/* The plane's new state; a shown image holds while the output may read it. */
	display_overlay_request(&commit, plane);
	commit.generation = lease->generation;
	commit.count = 1U;
	display_overlay_state(&commit.planes[0], lease->overlay, request->serial, shown, image, &request->source, &request->destination);
	if (shown != VK_FALSE)
		image->holds++;

	/* Records it, without waiting for the refresh boundary. */
	status = ioctl(plane->connection->fd, GPU_DISPLAY_COMMIT, &commit);
	native_error = errno;

	/* A refused commit changed nothing; the image holds no longer. */
	if (status != 0) {
		/* Whether to ask again, to make the chain out of date, or to fail as the output does. */
		if (shown != VK_FALSE)
			image->holds--;
		why = "error";
		error = display_error(native_error);
		if (native_error == EBUSY) {
			why = "busy";
			error = VK_SUCCESS;
			*outcome = VULKAN_WSI_OVERLAY_BUSY;
		} else if (native_error == ENOSPC) {
			why = "nospace";
			error = VK_ERROR_OUT_OF_DATE_KHR;
		} else if (native_error == EINVAL) {
			why = "invalid";
			error = VK_ERROR_OUT_OF_DATE_KHR;
		}
		display_overlay_log(lease->overlay, request->serial, 0U, 0U, path, why, (int)error, 0U, plane->adopted, request->call_ns);

		pthread_mutex_unlock(&display_overlay_mutex);

		/* Reports why the plane could not take the present. */
		if (error != VK_SUCCESS)
			return error;

		/* Succeeded: busy, nothing changed. */
		return VK_SUCCESS;
	}

	/* The plane follows a recorded state; an overtaken one gives its image back. */
	display_overlay_adopted_locked(plane, commit.adopted);
	if (request->serial > overlay->recorded_serial)
		overlay->recorded_serial = request->serial;
	if (commit.planes[0].result == GPU_DISPLAY_PLANE_APPLIED) {
		display_overlay_record_locked(overlay, shown, image, commit.sequence);
		*outcome = VULKAN_WSI_OVERLAY_APPLIED;
	} else if (shown != VK_FALSE) {
		image->holds--;
	}
	(void)display_overlay_reap_locked(plane, VK_FALSE);
	display_overlay_log(lease->overlay, request->serial, 0U, 0U, path, request->why, (int)commit.planes[0].result, commit.sequence, commit.adopted, request->call_ns);

	pthread_mutex_unlock(&display_overlay_mutex);

	/* Images whose replacement was adopted may be acquired again. */
	vulkan_wsi_image_notify();

	/* Succeeded: the plane shows the present from a coming refresh boundary, or a later one did. */
	return VK_SUCCESS;
}

/* Marks a held present's pixels complete, or drops it with the error of its GPU work. */
static void
display_overlay_ready(
	void *private_lease,
	uint64_t serial,
	VkResult error)
{
	struct wsi_display_lease *lease;
	struct wsi_display_overlay *overlay;

	/* Only the held change of the present itself; a replaced one is gone already. */
	lease = private_lease;
	overlay = &lease->plane->overlays[lease->overlay - 1U];
	pthread_mutex_lock(&display_overlay_mutex);

	if (overlay->hold.held != VK_FALSE && overlay->hold.serial == serial) {
		/* A failed copy is never shown; a complete one may be. */
		if (error != VK_SUCCESS) {
			display_overlay_drop_locked(overlay, error);
		} else {
			overlay->hold.ready = VK_TRUE;
		}
		pthread_cond_broadcast(&display_overlay_condition);
	}

	pthread_mutex_unlock(&display_overlay_mutex);

	/* A dropped change's image may be acquired again. */
	if (error != VK_SUCCESS)
		vulkan_wsi_image_notify();

	/* Succeeded: the change waits for its primary only. */
	return;
}

/* Reports the error a dropped held change left for the chain of a serial, or VK_SUCCESS. */
static VkResult
display_overlay_status(
	void *private_lease,
	uint64_t owner)
{
	struct wsi_display_lease *lease;
	struct wsi_display_overlay *overlay;
	VkResult error;

	/* Only the chain whose change was dropped is told. */
	lease = private_lease;
	overlay = &lease->plane->overlays[lease->overlay - 1U];
	error = VK_SUCCESS;
	pthread_mutex_lock(&display_overlay_mutex);

	if (owner != 0U && overlay->failed_owner == owner)
		error = overlay->failed_error;

	pthread_mutex_unlock(&display_overlay_mutex);

	/* Reports what the dropped change left. */
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: no change of the chain was dropped. */
	return VK_SUCCESS;
}

/* Drops the held change of a retiring chain, which no primary present then shows. */
static void
display_overlay_forget(
	void *private_lease,
	uint64_t owner)
{
	struct wsi_display_lease *lease;
	struct wsi_display_overlay *overlay;
	VkBool32 dropped;

	/* Only a change the retiring chain presented. */
	lease = private_lease;
	overlay = &lease->plane->overlays[lease->overlay - 1U];
	dropped = VK_FALSE;
	pthread_mutex_lock(&display_overlay_mutex);

	if (overlay->hold.held != VK_FALSE && overlay->hold.owner == owner) {
		display_overlay_drop_locked(overlay, VK_SUCCESS);
		pthread_cond_broadcast(&display_overlay_condition);
		dropped = VK_TRUE;
	}

	pthread_mutex_unlock(&display_overlay_mutex);

	/* The dropped change's image may be acquired again. */
	if (dropped != VK_FALSE)
		vulkan_wsi_image_notify();

	/* Succeeded: the retired chain holds no change. */
	return;
}

/*
 * Waits at most timeout nanoseconds for the output to stop reading an
 * overlay image a later record replaced.
 *
 * No display lock is held while the kernel waits, so primary presents and
 * other planes go on (ws214-p001 4.4 (5)).
 */
static VkResult
display_overlay_reclaim(
	void *private_lease,
	uint64_t timeout)
{
	struct wsi_display_lease *lease;
	struct wsi_display_plane *plane;
	struct wsi_display_overlay *overlay;
	struct wsi_display_image *image;
	uint64_t target;
	VkBool32 released;
	VkResult error;

	/* Only an overlay lease's images retire. */
	lease = private_lease;
	if (lease->overlay == 0U)
		return VK_NOT_READY;
	plane = lease->plane;
	overlay = &plane->overlays[lease->overlay - 1U];

	/* Frees what is adopted already, and finds the lowest record number another image waits for. */
	target = 0U;
	pthread_mutex_lock(&display_overlay_mutex);

	released = display_overlay_reap_locked(plane, VK_FALSE);
	for (image = overlay->retiring; image != NULL; image = image->retire_next) {
		/* The image whose record comes first frees first. */
		if (target == 0U || image->retire_sequence < target)
			target = image->retire_sequence;
	}

	pthread_mutex_unlock(&display_overlay_mutex);

	/* Something came free without waiting. */
	if (released != VK_FALSE) {
		vulkan_wsi_image_notify();
		return VK_SUCCESS;
	}

	/* No image waits for an adoption. */
	if (target == 0U)
		return VK_NOT_READY;

	/* Waits for the adoption that frees the first of them. */
	error = display_overlay_wait(plane, target, timeout);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: an image came free. */
	vulkan_wsi_image_notify();
	return VK_SUCCESS;
}

/*
 * Commits every ready held change of a device's outputs alone and waits for
 * the adoption (vkQueueWaitIdle and vkDeviceWaitIdle, ws214-p001 4.1 rule 6).
 *
 * It runs after the device's presentation workers drained, so a change still
 * held has no primary present coming that would take it.
 */
void
vulkan_wsi_display_overlay_flush(
	struct VkDevice_T *device)
{
	struct wsi_display_plane *plane;
	uint64_t sequence;

	/* Every claim of the device with overlay planes. */
	pthread_mutex_lock(&display_mutex);

	for (plane = display_planes; plane != NULL; plane = plane->next) {
		/* Another device's claims, and outputs without overlays, hold nothing. */
		if (plane->device != device || plane->overlay_count == 0U)
			continue;

		/* The ready changes go alone. */
		sequence = 0U;
		pthread_mutex_lock(&display_overlay_mutex);

		display_overlay_flush_locked(plane, 0U, VK_FALSE, "flush", &sequence);

		pthread_mutex_unlock(&display_overlay_mutex);

		/* The flush returns once the output shows them. */
		if (sequence != 0U)
			(void)display_overlay_wait(plane, sequence, 1000000000ULL);
	}

	pthread_mutex_unlock(&display_mutex);

	vulkan_wsi_image_notify();

	/* Succeeded: no ready change of the device is held. */
	return;
}

/*
 * Takes an overlay image out of its plane before its wrapper is destroyed
 * (vkDestroySwapchainKHR, ws214-p001 4.1 rule 6).
 *
 * A held change naming it is dropped; a plane showing it is hidden by a
 * commit of its own, whose adoption is waited for, as is the adoption that
 * frees a retiring image.  An image the output still reads after a second
 * is let go anyway: the kernel keeps its own reference.
 */
static void
display_overlay_image_retire(
	struct wsi_display_image *image)
{
	struct wsi_display_plane *plane;
	struct wsi_display_overlay *overlay;
	uint64_t sequence;
	uint64_t target;

	/* The image's plane. */
	plane = image->claim;
	overlay = &plane->overlays[image->overlay - 1U];
	sequence = 0U;
	target = 0U;
	pthread_mutex_lock(&display_overlay_mutex);

	/* A held change naming it is never shown. */
	if (overlay->hold.held != VK_FALSE && overlay->hold.image == image) {
		display_overlay_drop_locked(overlay, VK_SUCCESS);
		pthread_cond_broadcast(&display_overlay_condition);
	}

	/* A plane showing it is hidden. */
	if (overlay->front == image)
		display_overlay_hide_locked(plane, overlay, "destroy", &sequence);

	/* A retiring image waits for its record's adoption. */
	if (image->retire_sequence != 0U)
		target = image->retire_sequence;

	pthread_mutex_unlock(&display_overlay_mutex);

	/* Waits, a second at most, until the output no longer reads it. */
	if (target != 0U)
		(void)display_overlay_wait(plane, target, 1000000000ULL);

	/* Whatever the wait reported, the image leaves the plane now. */
	pthread_mutex_lock(&display_overlay_mutex);

	(void)display_overlay_reap_locked(plane, VK_FALSE);
	if (image->retire_sequence != 0U)
		display_overlay_unretire_locked(overlay, image);

	pthread_mutex_unlock(&display_overlay_mutex);

	/* Succeeded: no plane names the image. */
	return;
}

/*
 * Commits the ready held changes of a claim alone; the caller holds
 * display_overlay_mutex.
 *
 * With a ticket only the changes it would take go; with expired_only only
 * those past their deadline that no primary present called since will take.
 * sequence receives the record's number, or zero when nothing was recorded.
 */
static void
display_overlay_flush_locked(
	struct wsi_display_plane *plane,
	uint64_t ticket,
	VkBool32 expired_only,
	const char *why,
	uint64_t *sequence)
{
	struct gpu_display_commit request;
	struct wsi_display_overlay *overlay;
	struct wsi_display_hold *hold;
	uint32_t taken[GPU_DISPLAY_OVERLAY_MAX];
	uint32_t count;
	uint32_t index;
	uint64_t issued;
	uint64_t now;
	VkResult dropped;
	int status;
	int native_error;

	/* The changes that go. */
	*sequence = 0U;
	issued = __atomic_load_n(&plane->primary_issued, __ATOMIC_ACQUIRE);
	now = display_overlay_now();
	display_overlay_request(&request, plane);
	count = 0U;
	for (index = 0U; index < plane->overlay_count; index++) {
		/* Only a held change whose pixels are complete. */
		hold = &plane->overlays[index].hold;
		if (hold->held == VK_FALSE || hold->ready == VK_FALSE)
			continue;

		/* Only the changes a ticket would take. */
		if (ticket != 0U && hold->min_primary > ticket)
			continue;

		/* Only an expired change no primary present will take. */
		if (expired_only != VK_FALSE) {
			if (hold->deadline > now)
				continue;
			if (issued >= hold->min_primary)
				continue;
		}

		display_overlay_state(&request.planes[count], index + 1U, hold->serial, hold->shown, hold->image, &hold->source, &hold->destination);
		taken[count] = index;
		count++;
	}

	/* Nothing to commit. */
	if (count == 0U)
		return;

	/* Records them, without a primary and without waiting. */
	request.count = count;
	status = ioctl(plane->connection->fd, GPU_DISPLAY_COMMIT, &request);
	native_error = errno;

	/* A refused commit drops every change, whose chains report why. */
	if (status != 0) {
		/* An overlay the output cannot show makes its chain out of date. */
		dropped = display_error(native_error);
		if (native_error == ENOSPC || native_error == EINVAL)
			dropped = VK_ERROR_OUT_OF_DATE_KHR;

		/* Every taken change goes. */
		for (index = 0U; index < count; index++) {
			overlay = &plane->overlays[taken[index]];
			display_overlay_log(taken[index] + 1U, overlay->hold.serial, ticket, overlay->hold.min_primary, "flush", why, (int)dropped, 0U, plane->adopted, overlay->hold.call_ns);
			display_overlay_drop_locked(overlay, dropped);
		}
		pthread_cond_broadcast(&display_overlay_condition);
		return;
	}

	/* Each plane follows its recorded change. */
	display_overlay_adopted_locked(plane, request.adopted);
	for (index = 0U; index < count; index++) {
		/* A recorded change's image hold passes to the plane; an overtaken one goes back. */
		overlay = &plane->overlays[taken[index]];
		hold = &overlay->hold;
		display_overlay_log(taken[index] + 1U, hold->serial, ticket, hold->min_primary, "flush", why, (int)request.planes[index].result, request.sequence, request.adopted, hold->call_ns);
		if (request.planes[index].result == GPU_DISPLAY_PLANE_APPLIED) {
			display_overlay_record_locked(overlay, hold->shown, hold->image, request.sequence);
			overlay->shown = hold->shown;
			if (hold->serial > overlay->recorded_serial)
				overlay->recorded_serial = hold->serial;
			memset(hold, 0, sizeof(*hold));
		} else {
			display_overlay_drop_locked(overlay, VK_SUCCESS);
		}
	}
	(void)display_overlay_reap_locked(plane, VK_FALSE);
	pthread_cond_broadcast(&display_overlay_condition);
	*sequence = request.sequence;

	/* Succeeded: the changes show from a coming refresh boundary. */
	return;
}

/*
 * Hides a shown overlay plane by a commit of its own; the caller holds
 * display_overlay_mutex.  sequence receives the record's number, or zero.
 * A refused hide lets the image go: the lease is gone or going.
 */
static void
display_overlay_hide_locked(
	struct wsi_display_plane *plane,
	struct wsi_display_overlay *overlay,
	const char *why,
	uint64_t *sequence)
{
	struct gpu_display_commit request;
	uint32_t index;
	int status;

	/* The plane's next serial, hidden. */
	*sequence = 0U;
	index = (uint32_t)(overlay - plane->overlays) + 1U;
	overlay->serial++;
	display_overlay_request(&request, plane);
	request.count = 1U;
	display_overlay_state(&request.planes[0], index, overlay->serial, VK_FALSE, NULL, NULL, NULL);

	/* Records it, without waiting. */
	status = ioctl(plane->connection->fd, GPU_DISPLAY_COMMIT, &request);

	/* A refused hide: nothing will show the plane through this lease again. */
	if (status != 0) {
		display_overlay_log(index, overlay->serial, 0U, 0U, "flush", why, (int)display_error(errno), 0U, plane->adopted, 0U);
		if (overlay->front != NULL)
			overlay->front->holds--;
		overlay->front = NULL;
		overlay->shown = VK_FALSE;
		return;
	}

	/* The plane is hidden from the record on; its image retires until the adoption. */
	display_overlay_adopted_locked(plane, request.adopted);
	overlay->recorded_serial = overlay->serial;
	if (request.planes[0].result == GPU_DISPLAY_PLANE_APPLIED) {
		display_overlay_record_locked(overlay, VK_FALSE, NULL, request.sequence);
		overlay->shown = VK_FALSE;
		*sequence = request.sequence;
	}
	(void)display_overlay_reap_locked(plane, VK_FALSE);
	display_overlay_log(index, overlay->serial, 0U, 0U, "flush", why, (int)request.planes[0].result, request.sequence, request.adopted, 0U);

	/* Succeeded: the plane is hidden. */
	return;
}

/*
 * Waits, a second at most, until every held change a ticket takes is ready;
 * the caller holds display_overlay_mutex, which the wait lets go.  A change
 * still not ready then is dropped and its chain is out of date.
 */
static void
display_overlay_ready_wait_locked(
	struct wsi_display_plane *plane,
	uint64_t ticket)
{
	struct wsi_display_hold *hold;
	struct timespec deadline;
	uint64_t limit;
	uint64_t now;
	uint32_t index;
	VkBool32 waiting;
	int status;

	/* Rechecks the changes after every wake, since they may be replaced or dropped meanwhile. */
	limit = display_overlay_now() + 1000000000ULL;
	for (;;) {
		/* Whether a change the ticket takes still waits for its copy. */
		waiting = VK_FALSE;
		for (index = 0U; index < plane->overlay_count; index++) {
			/* A held change of the ticket whose pixels are not complete. */
			hold = &plane->overlays[index].hold;
			if (hold->held != VK_FALSE &&
			    hold->min_primary <= ticket &&
			    hold->ready == VK_FALSE)
				waiting = VK_TRUE;
		}

		/* Every change is ready. */
		if (waiting == VK_FALSE)
			return;

		/* The second is over: the changes not ready are dropped. */
		now = display_overlay_now();
		if (now >= limit)
			break;

		/* Sleeps until a change is made ready, replaced or dropped, or the second ends. */
		deadline.tv_sec = (time_t)(limit / 1000000000ULL);
		deadline.tv_nsec = (long)(limit % 1000000000ULL);
		status = pthread_cond_clockwait(&display_overlay_condition, &display_overlay_mutex, CLOCK_MONOTONIC, &deadline);
		if (status != 0 &&
		    status != ETIMEDOUT &&
		    status != EINTR)
			break;
	}

	/* Drops every change of the ticket that is still not ready. */
	for (index = 0U; index < plane->overlay_count; index++) {
		/* A held change of the ticket whose pixels never completed. */
		hold = &plane->overlays[index].hold;
		if (hold->held == VK_FALSE ||
		    hold->min_primary > ticket ||
		    hold->ready != VK_FALSE)
			continue;
		display_overlay_log(index + 1U, hold->serial, ticket, hold->min_primary, "commit", "ready", (int)VK_ERROR_OUT_OF_DATE_KHR, 0U, plane->adopted, hold->call_ns);
		display_overlay_drop_locked(&plane->overlays[index], VK_ERROR_OUT_OF_DATE_KHR);
	}

	/* Succeeded: no change of the ticket waits any more. */
	return;
}

/*
 * Drops a plane's held change, giving its image back; the caller holds
 * display_overlay_mutex.  With an error, the change's chain reports it from
 * then on.
 */
static void
display_overlay_drop_locked(
	struct wsi_display_overlay *overlay,
	VkResult error)
{
	struct wsi_display_hold *hold;

	/* A plane without a held change has nothing to drop. */
	hold = &overlay->hold;
	if (hold->held == VK_FALSE)
		return;

	/* The image the change named is free. */
	if (hold->image != NULL)
		hold->image->holds--;

	/* The chain that presented it is told why. */
	if (error != VK_SUCCESS) {
		overlay->failed_owner = hold->owner;
		overlay->failed_error = error;
	}

	/* Succeeded: the plane holds no change. */
	memset(hold, 0, sizeof(*hold));
	return;
}

/*
 * Makes a recorded state the plane's; the caller holds display_overlay_mutex.
 *
 * The image the output showed before retires until the record numbered
 * sequence is adopted; a shown image takes over the hold its present carried,
 * and showing the same image again keeps only one hold for it.
 */
static void
display_overlay_record_locked(
	struct wsi_display_overlay *overlay,
	VkBool32 shown,
	struct wsi_display_image *image,
	uint64_t sequence)
{
	struct wsi_display_image *previous;

	/* The same image shown again stays the plane's, with one hold. */
	previous = overlay->front;
	if (shown != VK_FALSE && previous == image) {
		image->holds--;
		return;
	}

	/* The earlier image is read until a list without it is adopted. */
	if (previous != NULL) {
		previous->retire_sequence = sequence;
		previous->retire_next = overlay->retiring;
		overlay->retiring = previous;
	}

	/* The plane shows the new image, or nothing. */
	overlay->front = NULL;
	if (shown != VK_FALSE)
		overlay->front = image;

	/* Succeeded: the plane follows the record. */
	return;
}

/*
 * Lets go of the retiring images whose replacement is adopted, or of every
 * one; the caller holds display_overlay_mutex.  Reports whether one went.
 */
static VkBool32
display_overlay_reap_locked(
	struct wsi_display_plane *plane,
	VkBool32 every)
{
	struct wsi_display_overlay *overlay;
	struct wsi_display_image **link;
	struct wsi_display_image *image;
	uint32_t index;
	VkBool32 released;

	/* Every plane's retiring images. */
	released = VK_FALSE;
	for (index = 0U; index < plane->overlay_count; index++) {
		overlay = &plane->overlays[index];
		link = &overlay->retiring;

		/* Unlinks each image the output no longer reads. */
		while (*link != NULL) {
			/* An image whose replacing record is not adopted stays. */
			image = *link;
			if (every == VK_FALSE && image->retire_sequence > plane->adopted) {
				link = &image->retire_next;
				continue;
			}

			/* The image is free of the output. */
			*link = image->retire_next;
			image->retire_next = NULL;
			image->retire_sequence = 0U;
			image->holds--;
			released = VK_TRUE;
		}
	}

	/* Reports whether an image came free. */
	if (released != VK_FALSE)
		return VK_TRUE;

	/* Succeeded: nothing came free. */
	return VK_FALSE;
}

/* Lets go of one retiring image whatever its record; the caller holds display_overlay_mutex. */
static void
display_overlay_unretire_locked(
	struct wsi_display_overlay *overlay,
	struct wsi_display_image *image)
{
	struct wsi_display_image **link;

	/* Finds the image among the plane's retiring ones. */
	link = &overlay->retiring;
	while (*link != NULL && *link != image)
		link = &(*link)->retire_next;

	/* An image no longer retiring has nothing to let go. */
	if (*link == NULL)
		return;

	/* Succeeded: the image leaves the list with its hold. */
	*link = image->retire_next;
	image->retire_next = NULL;
	image->retire_sequence = 0U;
	image->holds--;
	return;
}

/* Raises the claim's adopted record number to one the output reported; the caller holds display_overlay_mutex. */
static void
display_overlay_adopted_locked(
	struct wsi_display_plane *plane,
	uint64_t adopted)
{
	/* The number only grows: every lower record is adopted too. */
	if (adopted > plane->adopted)
		plane->adopted = adopted;

	/* Succeeded: the claim knows the adoption. */
	return;
}

/*
 * Waits at most timeout nanoseconds (a second at most, the kernel's limit)
 * until the record numbered sequence is adopted, and lets go of the images
 * that frees.  No display lock is taken while the kernel waits.
 */
static VkResult
display_overlay_wait(
	struct wsi_display_plane *plane,
	uint64_t sequence,
	uint64_t timeout)
{
	struct gpu_display_wait request;
	uint64_t adopted;
	int status;
	int native_error;

	/* A record already adopted needs no wait. */
	pthread_mutex_lock(&display_overlay_mutex);

	adopted = plane->adopted;

	pthread_mutex_unlock(&display_overlay_mutex);

	if (sequence <= adopted)
		return VK_SUCCESS;

	/* The kernel sleeps until the adoption, the lease's end, a signal or the time. */
	memset(&request, 0, sizeof(request));
	request.version = GPU_ABI_VERSION;
	request.size = sizeof(request);
	request.lease = plane->identifier;
	request.sequence = sequence;
	request.timeout_ns = timeout;
	status = ioctl(plane->connection->fd, GPU_DISPLAY_WAIT, &request);
	native_error = errno;

	/* The record is not adopted yet. */
	if (status != 0) {
		/* No time to wait, or a signal: the caller may look again. */
		if (native_error == EAGAIN || native_error == EINTR)
			return VK_NOT_READY;

		/* The time ran out. */
		if (native_error == ETIMEDOUT)
			return VK_TIMEOUT;

		/* Reports the lease's end or the output's loss. */
		return display_error(native_error);
	}

	/* The images the adoption frees go now. */
	pthread_mutex_lock(&display_overlay_mutex);

	display_overlay_adopted_locked(plane, request.completed_sequence);
	(void)display_overlay_reap_locked(plane, VK_FALSE);

	pthread_mutex_unlock(&display_overlay_mutex);

	/* Succeeded: the record is adopted. */
	return VK_SUCCESS;
}

/* Starts the claim's watcher with its first held change; the caller holds display_overlay_mutex. */
static void
display_overlay_watch_start_locked(
	struct wsi_display_plane *plane)
{
	int status;

	/* One watcher a claim, never one after the final release began. */
	if (plane->watching != VK_FALSE || plane->watch_stop != VK_FALSE)
		return;

	/*
	 * Without a watcher a held change still goes with a primary present,
	 * an idle wait or a destruction; only its deadline is lost.
	 */
	status = pthread_create(&plane->watcher, NULL, display_overlay_watch, plane);
	if (status != 0)
		return;

	/* Succeeded: the final release joins it. */
	plane->watching = VK_TRUE;
	return;
}

/*
 * Commits a held change alone when no primary present takes it in time
 * (ws214-p001 4.1 rule 6): two refreshes after it was held, while no primary
 * present was called after it, the change goes with log why=expired.
 */
static void *
display_overlay_watch(
	void *argument)
{
	struct wsi_display_plane *plane;
	struct wsi_display_hold *hold;
	struct timespec deadline;
	uint64_t earliest;
	uint64_t issued;
	uint64_t now;
	uint64_t sequence;
	uint32_t index;
	VkBool32 expired;

	/* The claim, which the final release keeps alive until it has joined this thread. */
	plane = argument;
	pthread_mutex_lock(&display_overlay_mutex);

	/* Looks at the held changes after every wake, until the release stops it. */
	while (plane->watch_stop == VK_FALSE) {
		/* The expired changes, and the deadline of the first one still to expire. */
		now = display_overlay_now();
		issued = __atomic_load_n(&plane->primary_issued, __ATOMIC_ACQUIRE);
		earliest = UINT64_MAX;
		expired = VK_FALSE;
		for (index = 0U; index < plane->overlay_count; index++) {
			/* A held change a primary present called after it will take is not the watcher's. */
			hold = &plane->overlays[index].hold;
			if (hold->held == VK_FALSE || issued >= hold->min_primary)
				continue;

			/* An expired change goes once its pixels are complete; another waits for its deadline. */
			if (hold->deadline <= now) {
				if (hold->ready != VK_FALSE)
					expired = VK_TRUE;
			} else if (hold->deadline < earliest) {
				earliest = hold->deadline;
			}
		}

		/* The expired changes go alone. */
		if (expired != VK_FALSE) {
			display_overlay_flush_locked(plane, 0U, VK_TRUE, "expired", &sequence);
			continue;
		}

		/* Sleeps until a change is held, made ready or dropped, or the first deadline. */
		if (earliest == UINT64_MAX) {
			(void)pthread_cond_wait(&display_overlay_condition, &display_overlay_mutex);
		} else {
			deadline.tv_sec = (time_t)(earliest / 1000000000ULL);
			deadline.tv_nsec = (long)(earliest % 1000000000ULL);
			(void)pthread_cond_clockwait(&display_overlay_condition, &display_overlay_mutex, CLOCK_MONOTONIC, &deadline);
		}
	}

	pthread_mutex_unlock(&display_overlay_mutex);

	/* Succeeded: the release may free the claim. */
	return NULL;
}

/*
 * Fills one plane's state of a commit: shown with its image's layout and
 * rectangles, or hidden, naming no image.
 */
static void
display_overlay_state(
	struct gpu_display_plane_state *state,
	uint32_t plane,
	uint64_t serial,
	VkBool32 shown,
	const struct wsi_display_image *image,
	const VkRect2D *source,
	const VkRect2D *destination)
{
	const struct gpu_image_descriptor *descriptor;

	/* The plane and its serial; no fence, since the image's pixels are complete. */
	memset(state, 0, sizeof(*state));
	state->plane = plane;
	state->serial = serial;
	state->wait_fd = -1;

	/* A hidden plane names nothing more. */
	if (shown == VK_FALSE)
		return;

	/* The image as the display open imported it. */
	descriptor = &image->imported.image;
	state->flags = GPU_DISPLAY_PLANE_SHOWN | GPU_DISPLAY_PLANE_IMAGE;
	state->handle = image->imported.handle;
	state->offset = descriptor->offset;
	state->width = descriptor->width;
	state->height = descriptor->height;
	state->stride = descriptor->stride;
	state->format = descriptor->format;

	/* The part of the image shown, and where on the output (both checked non-negative by the swapchain). */
	state->src_x = (uint32_t)source->offset.x;
	state->src_y = (uint32_t)source->offset.y;
	state->src_width = source->extent.width;
	state->src_height = source->extent.height;
	state->dst_x = (uint32_t)destination->offset.x;
	state->dst_y = (uint32_t)destination->offset.y;
	state->dst_width = destination->extent.width;
	state->dst_height = destination->extent.height;

	/* Succeeded: the state names the image and its rectangles. */
	return;
}

/* Starts a commit of a claim with no planes named yet: every entry without a fence. */
static void
display_overlay_request(
	struct gpu_display_commit *request,
	const struct wsi_display_plane *plane)
{
	uint32_t index;

	/* The claim's lease of its generation, with no signal fence. */
	memset(request, 0, sizeof(*request));
	request->version = GPU_ABI_VERSION;
	request->size = sizeof(*request);
	request->lease = plane->identifier;
	request->generation = plane->generation;
	request->signal_fd = -1;

	/* An unused entry has no fence either. */
	for (index = 0U; index < 1U + GPU_DISPLAY_OVERLAY_MAX; index++)
		request->planes[index].wait_fd = -1;

	/* Succeeded: the caller names the planes. */
	return;
}

/* Reads the monotonic clock in nanoseconds, or zero when it cannot be read. */
static uint64_t
display_overlay_now(
	void)
{
	struct timespec now;
	int status;

	/* The clock the deadlines and the log share with the swapchain. */
	status = clock_gettime(CLOCK_MONOTONIC, &now);
	if (status != 0)
		return 0U;

	/* Succeeded: the time. */
	return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

/*
 * Writes one overlay log line to standard error while the overlay log is
 * on (VULKAN_ZEDBSD_OVERLAY_LOG=1).
 *
 * path is held, replaced, fast, lane, commit or flush; result is the
 * kernel's plane result (1 applied, 2 dropped) for a record, otherwise a
 * VkResult; ticket is the primary's that committed it, zero for none.
 */
static void
display_overlay_log(
	uint32_t plane,
	uint64_t serial,
	uint64_t ticket,
	uint64_t min_primary,
	const char *path,
	const char *why,
	int result,
	uint64_t sequence,
	uint64_t adopted,
	uint64_t call_ns)
{
	char line[320];
	int logging;
	int length;

	/* Nothing is written while the log is off. */
	logging = __atomic_load_n(&display_overlay_log_on, __ATOMIC_RELAXED);
	if (logging == 0)
		return;

	/* One line, written whole by one call. */
	if (why == NULL)
		why = "none";
	length = snprintf(
		line,
		sizeof(line),
		"WSI OVERLAY present plane=%u serial=%llu ticket=%llu min_primary=%llu path=%s why=%s result=%d sequence=%llu adopted=%llu call_ns=%llu\n",
		plane,
		(unsigned long long)serial,
		(unsigned long long)ticket,
		(unsigned long long)min_primary,
		path,
		why,
		result,
		(unsigned long long)sequence,
		(unsigned long long)adopted,
		(unsigned long long)call_ns);

	/* A line that could not be formatted is not written; a long one is cut to the buffer. */
	if (length <= 0)
		return;
	if ((size_t)length >= sizeof(line))
		length = (int)sizeof(line) - 1;

	/* A diagnostic line that cannot be written is not retried. */
	(void)write(2, line, (size_t)length);

	/* Succeeded: the line is written. */
	return;
}
