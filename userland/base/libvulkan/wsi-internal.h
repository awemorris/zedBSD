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

#define VULKAN_WSI_OUTPUT_CONNECTED	1U
#define VULKAN_WSI_OUTPUT_FIFO		2U
#define VULKAN_WSI_OUTPUT_ACTIVE	4U
#define VULKAN_WSI_FORMAT_RGBA		1U
#define VULKAN_WSI_FORMAT_BGRA		2U

struct vulkan_wsi_platform_ops;
struct vulkan_swapchain;

/*
 * One native output snapshot, independent of a particular kernel ABI.
 * Stable display identities outlive changes to this generation's geometry.
 */
struct vulkan_wsi_output {
	uint64_t identifier;
	uint64_t generation;
	uint64_t max_frame_bytes;
	uint32_t flags;
	uint32_t formats;
	uint32_t plane_count;
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
};

/* The direct adapter supplies these after its kernel contract is finalized. */
extern const struct vulkan_wsi_platform_ops vulkan_wsi_display_platform;

VkResult vulkan_wsi_display_query(struct VkPhysicalDevice_T *physical, uint32_t index, uint32_t *count, struct vulkan_wsi_output *output);
VkResult vulkan_wsi_display_mode_query(struct VkPhysicalDevice_T *physical, const struct vulkan_wsi_output *output, uint32_t index, uint32_t *count, VkDisplayModeParametersKHR *mode);
VkResult vulkan_wsi_display_mode_validate(struct VkPhysicalDevice_T *physical, const struct vulkan_wsi_output *output, const VkDisplayModeParametersKHR *mode);
struct vulkan_surface *vulkan_wsi_surface(VkSurfaceKHR surface);
struct vulkan_display *vulkan_wsi_display(VkDisplayKHR display);
struct vulkan_display_mode *vulkan_wsi_display_mode(VkDisplayModeKHR mode);
VkResult vulkan_wsi_display_snapshot(struct vulkan_display *display, struct vulkan_wsi_output *output);
VkResult vulkan_wsi_display_refresh(struct vulkan_display *display);
VkResult vulkan_wsi_surface_retain(struct vulkan_surface *surface);
void vulkan_wsi_surface_release(struct vulkan_surface *surface);

#endif
