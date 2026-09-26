/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shares the portable Wayland window and Vulkan renderer's explicit owners.
 */

#ifndef WLTEST_H
#define WLTEST_H

/* Request the standard Wayland WSI declarations from the public Vulkan header. */
#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>

/* One configured fullscreen role, owned until Vulkan surface destruction. */
struct wltest_window {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct xdg_wm_base *shell;
	struct wl_surface *surface;
	struct xdg_surface *role;
	struct xdg_toplevel *toplevel;
	uint32_t width;
	uint32_t height;
	int configured;
	int closed;
};

/* One swapchain attachment, whose VkImage is borrowed from the swapchain. */
struct wltest_target {
	VkImage image;
	VkImageView view;
	VkFramebuffer framebuffer;
	VkSemaphore rendered;
};

/* One application's standard Vulkan owners; no kernel or renderer ABI is present. */
struct wltest_renderer {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	VkSurfaceKHR surface;
	VkSwapchainKHR swapchain;
	VkRenderPass pass;
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;
	VkSemaphore acquired;
	struct wltest_target *targets;
	uint32_t count;
	uint32_t family;
	VkExtent2D extent;
	VkFormat format;
	VkPresentModeKHR mode;
	const char *operation;
	/* Nonzero draws the whole image in solid[] instead of the test pattern (window tests). */
	int solid_set;
	float solid[3];
	/* The time spent in vkQueuePresentKHR, in total and at most. */
	uint64_t present_ns;
	uint64_t present_max_ns;
};

/* Window lifetime encloses all renderer use of the borrowed native surface. */
int wltest_window_open(struct wltest_window *window, const char *display, uint32_t width, uint32_t height, int fullscreen);
int wltest_window_dispatch(struct wltest_window *window);
void wltest_window_close(struct wltest_window *window);

/* Renderer cleanup releases Vulkan ownership before the native window closes. */
VkResult wltest_renderer_open(struct wltest_renderer *renderer, struct wltest_window *window, VkPresentModeKHR mode);
VkResult wltest_renderer_draw(struct wltest_renderer *renderer, uint32_t frame);
VkResult wltest_renderer_recreate(struct wltest_renderer *renderer);
VkResult wltest_renderer_close(struct wltest_renderer *renderer);

#endif
