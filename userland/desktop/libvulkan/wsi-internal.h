/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Library-owned presentation objects and native presentation boundaries.
 */

#ifndef VULKAN_WSI_INTERNAL_H
#define VULKAN_WSI_INTERNAL_H

#include "internal.h"
#include <uapi/gpu.h>
#include <uapi/gpu-display.h>
#include <uapi/gpu-scanout.h>

#define VULKAN_WSI_OUTPUT_CONNECTED	1U
#define VULKAN_WSI_OUTPUT_FIFO		2U
#define VULKAN_WSI_OUTPUT_ACTIVE	4U
#define VULKAN_WSI_OUTPUT_BLOB		8U
/* The output's last plane is a cursor plane (GPU_DISPLAY_HAS_CURSOR): small, premultiplied, on top. */
#define VULKAN_WSI_OUTPUT_CURSOR	16U
/* Planes 1 to overlay_count are overlay planes (GPU_DISPLAY_HAS_OVERLAYS): premultiplied, placed and maybe scaled. */
#define VULKAN_WSI_OUTPUT_OVERLAYS	32U
#define VULKAN_WSI_FORMAT_RGBA		1U
#define VULKAN_WSI_FORMAT_BGRA		2U

/* The most images a window system lends one chain (a compositor's game mode). */
#define VULKAN_WSI_LEND_MAX		8U

struct vulkan_wsi_platform_ops;
struct vulkan_swapchain;

/*
 * A window-system chain's creation as a lend of the compositor's images is
 * checked against (the platform's lend and lend_changed): the format the
 * application draws in, the chain's extent and uses, the image count it
 * asked for, and whether the device can draw it into images it imports
 * (one layer, uses an imported image has, opaque, a format a linear image
 * renders to).
 */
struct vulkan_wsi_lend_fit {
	VkFormat format;
	VkExtent2D extent;
	VkImageUsageFlags usage;
	uint32_t count;
	VkBool32 drawable;
};

/* What one cursor-plane present did on the output (ws141-p016 (B)). */
enum vulkan_wsi_cursor_outcome {
	/* The image and position are shown from a coming refresh boundary. */
	VULKAN_WSI_CURSOR_SHOWN,
	/* A newer present was shown already, so this one changed nothing. */
	VULKAN_WSI_CURSOR_DROPPED,
	/* The output had no free cursor image for now; nothing changed. */
	VULKAN_WSI_CURSOR_BUSY
};

/* What the output did with one overlay-plane commit (ws214-p004). */
enum vulkan_wsi_overlay_outcome {
	/* The plane shows the present from a coming refresh boundary. */
	VULKAN_WSI_OVERLAY_APPLIED,
	/* A later present of the plane was recorded already, so this one changed nothing. */
	VULKAN_WSI_OVERLAY_DROPPED,
	/* The output could not record it for now; nothing changed and it may be asked again. */
	VULKAN_WSI_OVERLAY_BUSY
};

/*
 * One overlay-plane present as the swapchain hands it to the output
 * (ws214-p004).  The swapchain fills the inputs; the decision fills serial
 * and held, and the commit reads them back.
 */
struct vulkan_wsi_overlay_present {
	/* The serial of the presenting chain: a held change is the chain's until it retires. */
	uint64_t owner;
	/* The plane shows nothing: srcRect and dstRect are both 1x1 at the origin. */
	VkBool32 hide;
	/* The present named its rectangles; without them the plane keeps its last ones. */
	VkBool32 has_rect;
	VkRect2D source;
	VkRect2D destination;
	/* No GPU work of the present remains, so the image may be shown now. */
	VkBool32 ready;
	/* The commit runs on the overlay lane's worker rather than the caller's thread. */
	VkBool32 on_worker;
	/* Why the present took its route, and the monotonic time it was called, for the overlay log. */
	const char *why;
	uint64_t call_ns;
	/* The plane's serial of this present, and whether it is held for a primary present. */
	uint64_t serial;
	VkBool32 held;
};

/*
 * The counter times (vulkan_wsi_cursor_clock) inside one cursor-plane
 * present, taken only while the cursor log is on: when its lock was held,
 * just before the image was set, and after the move returned.
 */
struct vulkan_wsi_cursor_times {
	uint64_t locked;
	uint64_t set;
	uint64_t done;
};

/*
 * One native output snapshot, independent of a particular kernel ABI.
 * Stable display identities outlive changes to this generation's geometry.
 */
struct vulkan_wsi_output {
	uint64_t identifier;
	uint64_t device_identifier;
	char device_path[262];
	uint64_t generation;
	uint64_t max_frame_bytes;
	uint32_t flags;
	uint32_t formats;
	uint32_t plane_count;
	uint32_t overlay_count;
	VkExtent2D physical_dimensions;
	VkExtent2D preferred_extent;
	VkExtent2D maximum_extent;
	uint32_t refresh_millihz;
	char name[64];
};

/*
 * One display identity retained until its instance is destroyed.
 * The immutable name remains valid even when its output snapshot changes.
 */
struct vulkan_display {
	struct vulkan_object object;
	struct VkPhysicalDevice_T *physical;
	struct vulkan_wsi_output output;
	struct vulkan_display *next;
	struct vulkan_display_mode *modes;
	char name[64];
};

/*
 * One mode description retained with its display until instance destruction.
 * The generation identifies the native capabilities used to validate it.
 */
struct vulkan_display_mode {
	struct vulkan_object object;
	struct vulkan_display *display;
	struct vulkan_display_mode *next;
	VkDisplayModeParametersKHR parameters;
	uint64_t generation;
};

/*
 * One instance-owned surface describing a native presentation target.
 * Swapchains borrow it; only their final destruction permits surface release.
 */
struct vulkan_surface {
	struct vulkan_object object;
	struct VkInstance_T *instance;
	const struct vulkan_wsi_platform_ops *platform;
	struct vulkan_display_mode *display_mode;
	struct vulkan_surface *next;
	struct vulkan_swapchain *active;
	void *platform_private;
	VkDisplaySurfaceCreateInfoKHR display_info;
	uint32_t swapchain_references;
	uint32_t native_plane;
	/* The surface is the output's cursor plane: its presents set the cursor image and position. */
	VkBool32 cursor;
	/* The surface is one of the output's overlay planes (ws214-p004): it borrows the primary's claim. */
	VkBool32 overlay;
	/*
	 * The last ticket given to a present of this cursor surface.  It only
	 * grows (atomically, under the swapchain mutex), and the old and
	 * replacement swapchains share it with the surface's lease, so a larger
	 * ticket is always the later present.
	 */
	uint64_t cursor_ticket;
};

/*
 * One completed pixel image borrowed only during a native presentation call.
 * Rendering and its readback fence have completed before the adapter sees it.
 */
struct vulkan_wsi_pixels {
	const void *pixels;
	VkExtent2D extent;
	uint32_t stride;
	VkFormat format;
	uint64_t frame;
};

/*
 * One native presentation implementation behind standard surface operations.
 * The direct-display implementation owns GPU requests and display leases;
 * a future window-system implementation owns its native objects instead.
 */
struct vulkan_wsi_platform_ops {
	VkResult (*capabilities)(struct vulkan_surface *, struct VkPhysicalDevice_T *, VkSurfaceCapabilitiesKHR *);
	VkResult (*formats)(struct vulkan_surface *, struct VkPhysicalDevice_T *, uint32_t *, VkSurfaceFormatKHR *);
	VkResult (*present_modes)(struct vulkan_surface *, struct VkPhysicalDevice_T *, uint32_t *, VkPresentModeKHR *);
	VkResult (*claim)(struct vulkan_surface *, struct VkDevice_T *, void **);
	VkResult (*release)(void *);
	VkResult (*present)(void *, const struct vulkan_wsi_pixels *, uint64_t *);
	VkResult (*wait)(void *, uint64_t, uint64_t);
	void (*destroy_surface)(struct vulkan_surface *);
	VkResult (*import_image)(void *, int, const struct gpu_image_descriptor *, void **);
	VkResult (*present_image)(void *, void *, VkPresentModeKHR, uint64_t *);
	VkResult (*progress)(void *);
	VkBool32 (*image_available)(void *);
	void (*destroy_image)(void *);
	VkResult (*prepare_copy)(void *, VkFormat, VkExtent2D);
	VkResult (*present_image_sync)(void *, void *, VkPresentModeKHR, uint64_t *, int, uint64_t);
	VkResult (*placement)(void *, struct gpu_placement *);
	int (*wait_descriptor)(void *);
	/* Nonzero when a commit may precede the producer's completion (it travels with the fence). */
	VkBool32 (*commit_early)(void *);
	/*
	 * Tells a lease the alpha mode of its images other than opaque, one the
	 * backend advertised; NULL when only opaque images are presented.
	 */
	VkResult (*composite_alpha)(void *, VkCompositeAlphaFlagBitsKHR);
	/*
	 * Names the part of the next presented image that changed since the
	 * last (VK_KHR_incremental_present, BUG-221): x, y, width, height in
	 * image pixels; the next present alone uses it, and an empty part takes
	 * it back.  NULL when the backend damages every present whole.
	 */
	void (*damage)(void *, int32_t, int32_t, int32_t, int32_t);
	/*
	 * Shows a cursor-plane lease's image with its top-left corner at x, y
	 * (VkDisplayPresentInfoKHR's dstRect; without a position, where the last
	 * one was), unless a present with a larger ticket was shown already.
	 * The completed image may be reused when it returns.  It takes only the
	 * cursor's own lock, never the lock primary presents hold while the
	 * output adopts a frame; it advances the chain's frame and sequence it
	 * is given only when it shows the image, and reports what it did.  The
	 * times are written when not NULL.  NULL when the backend has no cursor
	 * plane.
	 */
	VkResult (*present_cursor)(void *, void *, VkBool32, int32_t, int32_t, uint64_t, uint64_t *, uint64_t *, enum vulkan_wsi_cursor_outcome *, struct vulkan_wsi_cursor_times *);
	/*
	 * Gives a primary-plane present its ticket, in the order presents are
	 * called (under the swapchain mutex); a held overlay change is shown by
	 * the first primary present whose ticket is at least the change's
	 * min_primary.  Zero for a lease whose output has no overlay planes.
	 * NULL when the backend has no overlay planes.
	 */
	uint64_t (*primary_ticket)(void *);
	/*
	 * Presents a primary image like present_image_sync, and with it every
	 * held overlay change its ticket takes, in one commit.  NULL when the
	 * backend has no overlay planes.
	 */
	VkResult (*present_primary)(void *, void *, VkPresentModeKHR, uint64_t *, int, uint64_t, uint64_t);
	/*
	 * Tells the lease that the primary present of a ticket did not reach
	 * the output by present_primary: with VK_SUCCESS the held changes it
	 * would have taken are committed alone, with an error they are dropped
	 * and their chains are out of date.  NULL when the backend has no
	 * overlay planes.
	 */
	void (*primary_skip)(void *, uint64_t, VkResult);
	/*
	 * Decides how an overlay-plane present reaches the output: held for a
	 * primary present (a change between shown and hidden, or any present to
	 * a plane with a held change), or committed alone by overlay_commit.
	 * It fills the request's serial and held.  NULL when the backend has no
	 * overlay planes.
	 */
	void (*overlay_decide)(void *, void *, struct vulkan_wsi_overlay_present *);
	/* Commits a decided overlay present that was not held, and reports what the output did. */
	VkResult (*overlay_commit)(void *, void *, struct vulkan_wsi_overlay_present *, enum vulkan_wsi_overlay_outcome *);
	/* Marks a held overlay present's pixels complete, or drops it with the error of its GPU work. */
	void (*overlay_ready)(void *, uint64_t, VkResult);
	/* Reports the error a dropped held change left for the chain of a serial, or VK_SUCCESS. */
	VkResult (*overlay_status)(void *, uint64_t);
	/* Drops the held change of a retiring chain, which no primary present then shows. */
	void (*overlay_forget)(void *, uint64_t);
	/*
	 * Waits at most timeout nanoseconds for the output to stop reading an
	 * overlay image a later commit replaced; VK_SUCCESS when an image came
	 * free, VK_NOT_READY when none is waiting for that, VK_TIMEOUT.
	 */
	VkResult (*reclaim)(void *, uint64_t);
	/*
	 * The fd a blocked acquire also waits on, readable (POLLIN) when the
	 * window system may have sent an image back: a Wayland connection,
	 * whose wl_buffer.release the next progress reads.  NULL when the
	 * backend has none (wait_descriptor is the display's, POLLPRI).
	 */
	int (*wait_input)(void *);
	/*
	 * Tells the window system the chain a creation makes (the fit), and
	 * takes the images it lends the chain to draw into when the lend it
	 * offers now fits: at most the capacity's count, each one's fd (the
	 * caller's to close) and description, and the lend's serial.
	 * VK_ERROR_FEATURE_NOT_PRESENT when no lend fits (one offered that does
	 * not fit is declined).  NULL when the backend lends no images.
	 */
	VkResult (*lend)(void *, const struct vulkan_wsi_lend_fit *, uint32_t, uint32_t *, int *, struct gpu_image_descriptor *, uint64_t *);
	/*
	 * Makes the native image of lent image index of the lend serial from
	 * its fd (which stays the caller's); it is busy until the window system
	 * gives it to the chain to draw into.
	 */
	VkResult (*lend_import)(void *, int, const struct gpu_image_descriptor *, uint64_t, uint32_t, void **);
	/*
	 * Tells whether a chain is better made again: a lend that fits it is
	 * offered to a chain of its own images (serial zero), or the lend
	 * serial a lent chain draws into has ended or been replaced.  NULL when
	 * the backend lends no images.
	 */
	VkBool32 (*lend_changed)(void *, const struct vulkan_wsi_lend_fit *, uint64_t);
};

VkResult vulkan_wsi_display_node_query(struct VkPhysicalDevice_T *physical, uint32_t index, uint32_t *count, struct gpu_display_info *request, uint64_t *device_id, char *path);
int vulkan_wsi_display_node_ioctl(struct VkPhysicalDevice_T *physical, const struct vulkan_wsi_output *output, unsigned long command, void *argument);
void vulkan_wsi_display_overlay_flush(struct VkDevice_T *device);
VkResult vulkan_wsi_display_overlay_info(struct VkPhysicalDevice_T *physical, const struct vulkan_wsi_output *output, uint32_t plane, struct gpu_display_overlay_info *info);
void vulkan_wsi_display_nodes_finish(struct VkInstance_T *instance);
VkResult vulkan_wsi_display_node_topology(struct VkPhysicalDevice_T *physical, uint64_t *sequence);

/* Reads a monotonic nanosecond clock fine enough for the cursor log's lengths. */
uint64_t vulkan_wsi_cursor_clock(void);

/* The direct adapter supplies these after its kernel contract is finalized. */
/* Native ownership changes wake image acquisition without dispatching application events. */
void vulkan_wsi_image_notify(void);

extern const struct vulkan_wsi_platform_ops vulkan_wsi_display_platform;

VkResult vulkan_wsi_display_query(struct VkPhysicalDevice_T *physical, uint32_t index, uint32_t *count, struct vulkan_wsi_output *output);
VkResult vulkan_wsi_display_mode_query(struct VkPhysicalDevice_T *physical, const struct vulkan_wsi_output *output, uint32_t index, uint32_t *count, VkDisplayModeParametersKHR *mode);
VkResult vulkan_wsi_display_mode_validate(struct VkPhysicalDevice_T *physical, const struct vulkan_wsi_output *output, const VkDisplayModeParametersKHR *mode);
struct vulkan_surface *vulkan_wsi_surface(VkSurfaceKHR surface);
struct vulkan_display *vulkan_wsi_display(VkDisplayKHR display);
struct vulkan_display_mode *vulkan_wsi_display_mode(VkDisplayModeKHR mode);
VkResult vulkan_wsi_display_snapshot(struct vulkan_display *display, struct vulkan_wsi_output *output);
VkResult vulkan_wsi_display_power(struct VkDevice_T *device, struct vulkan_display *display, uint32_t state);
VkResult vulkan_wsi_display_refresh(struct vulkan_display *display);
VkResult vulkan_wsi_surface_retain(struct vulkan_surface *surface);
void vulkan_wsi_surface_release(struct vulkan_surface *surface);

VkResult vulkan_wsi_surface_publish(struct vulkan_surface *surface, VkSurfaceKHR *handle);
VkResult vulkan_wsi_shared_image_create(struct VkDevice_T *device, VkFormat format, VkExtent2D extent, const VkAllocationCallbacks *allocator, const struct gpu_placement *placement, VkImage *image, VkDeviceMemory *memory, int *fd, struct gpu_image_descriptor *descriptor);

/*
 * The shared image with the uses a swapchain renders with (wsi-image.c).
 * It is weak because the host tests of the swapchain link stubs of
 * wsi-image.c without it; a null test there leaves the chain on its copy.
 */
extern VkResult vulkan_wsi_shared_image_create_usage(struct VkDevice_T *device, VkFormat format, VkExtent2D extent, VkImageUsageFlags usage, const VkAllocationCallbacks *allocator, const struct gpu_placement *placement, VkImage *image, VkDeviceMemory *memory, int *fd, struct gpu_image_descriptor *descriptor) __attribute__((weak));

VkResult vulkan_wsi_shared_image_import(struct VkDevice_T *device, int fd, const VkAllocationCallbacks *allocator, VkImage *image, VkDeviceMemory *memory, struct gpu_image_descriptor *descriptor);

/*
 * The import of a shared image as an image of a given format of the same
 * channel order (wsi-image.c), and the memory and description of an
 * exportable chain's image (wsi-swapchain.c).  They are weak because the
 * host tests of the swapchain link stubs of wsi-image.c without the first,
 * and the second's callers are linked without the swapchain in host tests;
 * a null one leaves the chain on its own images, or refuses the export.
 */
extern VkResult vulkan_wsi_shared_image_import_format(struct VkDevice_T *device, int fd, VkFormat format, const VkAllocationCallbacks *allocator, VkImage *image, VkDeviceMemory *memory, struct gpu_image_descriptor *descriptor) __attribute__((weak));
extern VkResult vulkan_wsi_swapchain_image_memory(struct VkDevice_T *device, VkImage image, VkDeviceMemory *memory, struct gpu_image_descriptor *descriptor) __attribute__((weak));

#endif
