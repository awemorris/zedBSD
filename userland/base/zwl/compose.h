/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Vulkan state of window mode, shared by compose.c and import.c.
 *
 * The device, pipelines and descriptor pool live as long as the compositor.
 * The display surface and its swapchain (vkdemo's standard display-plane
 * selection, userland/base/vkdemo/display.c) exist only in window mode: entering
 * fullscreen mode destroys them, which returns the display lease to the
 * compositor's own GPU_DISPLAY_CLAIM.  One frame is in flight at a time; its
 * fence is exported as an fd the event loop polls, and the buffers and frame
 * callbacks the frame used are held until it signals.
 */

#ifndef ZWL_COMPOSE_H
#define ZWL_COMPOSE_H

#include "zwl.h"

#include <vulkan/vulkan.h>

#include "../vkdemo/display.h"

/* Bound the swapchain images and the buffers one frame may sample. */
#define ZWL_SWAPCHAIN_MAX	8U
#define ZWL_FRAME_WINDOWS	64U

/* Bound the buffers that hold a descriptor set at once. */
#define ZWL_DESCRIPTOR_MAX	256U

/* The window-mode background, a dark blue-grey (0x20, 0x30, 0x40). */
#define ZWL_BACKGROUND_RED	(32.0f / 255.0f)
#define ZWL_BACKGROUND_GREEN	(48.0f / 255.0f)
#define ZWL_BACKGROUND_BLUE	(64.0f / 255.0f)

/*
 * How a window is drawn (design D10): an opaque quad, a quad blended by its
 * alpha, and later effects that read what is already drawn under it.
 */
enum zwl_draw {
	ZWL_DRAW_OPAQUE,
	ZWL_DRAW_ALPHA
};

/* A buffer's image as window mode samples it. */
struct zwl_import {
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkDescriptorSet set;
	uint32_t width;
	uint32_t height;
	enum zwl_draw draw;
};

/* The Vulkan device, the display output and the frame in flight. */
struct zwl_compose {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	VkRenderPass pass;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipelines[2];
	VkSampler sampler;
	VkDescriptorPool descriptors;
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;
	VkSemaphore acquired;
	struct vkdemo_display output;
	unsigned output_open;
	VkFormat format;
	VkImageView views[ZWL_SWAPCHAIN_MAX];
	VkFramebuffer framebuffers[ZWL_SWAPCHAIN_MAX];
	VkSemaphore rendered[ZWL_SWAPCHAIN_MAX];
	struct zwl_object *held[ZWL_FRAME_WINDOWS];
	struct zwl_object *callbacks;
	unsigned held_count;
	unsigned in_flight;
	uint64_t frame_start_cycles;
};

#endif
