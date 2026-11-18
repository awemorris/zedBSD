/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Shares the model viewer's model, camera, input, window and renderer owners.
 */

#ifndef MVIEW_H
#define MVIEW_H

/* Request the standard Wayland WSI declarations from the public Vulkan header. */
#define VK_USE_PLATFORM_WAYLAND_KHR 1
#include <vulkan/vulkan.h>
#include <wayland-client.h>
#include <xdg-shell-client-protocol.h>

#include "userland/desktop/libwayland/content-type-v1-client-protocol.h"

#include "model.h"

/* One configured fullscreen role, owned until Vulkan surface destruction. */
struct mview_window {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct xdg_wm_base *shell;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct wl_surface *surface;
	struct xdg_surface *role;
	struct xdg_toplevel *toplevel;
	struct mview_input *input;
	uint32_t seat_version;
	int32_t discrete;
	uint32_t width;
	uint32_t height;

	/* The size the viewer asked for, which a configure without a size keeps within the bounds. */
	uint32_t preferred_width;
	uint32_t preferred_height;

	/* The largest size the compositor lets the window choose (xdg-shell 4; zero: not known). */
	uint32_t bounds_width;
	uint32_t bounds_height;

	/* The compositor's content-type manager and this surface's object, when the viewer names its content (--content). */
	struct wp_content_type_manager_v1 *content_manager;
	struct wp_content_type_v1 *content_type;

	int configured;
	int resized;
	int closed;
};

/* One swapchain attachment, whose VkImage is borrowed from the swapchain. */
struct mview_target {
	VkImage image;
	VkImageView view;
	VkFramebuffer framebuffer;
	VkSemaphore rendered;
};

/* One application-owned image with its bound allocation and a full view. */
struct mview_image {
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	uint32_t levels;
};

/* One application-owned buffer with its bound allocation and optional mapping. */
struct mview_buffer {
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkDeviceSize bytes;
	void *mapping;
};

/* Pipelines are indexed by alpha mode and cull mode. */
#define MVIEW_PIPELINE_COUNT	6U

/* One application's standard Vulkan owners; no kernel or renderer ABI is present. */
/* The stages of one drawn frame whose time the renderer accumulates (in TSC cycles). */
enum mview_stage {
	MVIEW_STAGE_ACQUIRE,
	MVIEW_STAGE_RECORD,
	MVIEW_STAGE_SUBMIT,
	MVIEW_STAGE_PRESENT,
	MVIEW_STAGE_FENCE,
	MVIEW_STAGE_COUNT
};

struct mview_renderer;
void mview_renderer_report_stages(struct mview_renderer *renderer, const char *token, double seconds);
void mview_renderer_span(struct mview_renderer *renderer, int end);

/* The most frames in flight (--inflight). */
#define MVIEW_INFLIGHT_MAX	2U

struct mview_renderer {
	/* Cycles spent in each stage since the counters were last cleared, and the frames counted. */
	uint64_t stage_cycles[MVIEW_STAGE_COUNT];
	uint64_t stage_last[MVIEW_STAGE_COUNT];
	uint64_t span_start;
	uint64_t span_cycles;
	uint32_t stage_frames;

	VkInstance instance;
	VkPhysicalDevice physical;
	VkPhysicalDeviceMemoryProperties memory;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	VkSurfaceKHR surface;
	VkSwapchainKHR swapchain;
	VkFormat format;
	VkFormat depth_format;
	VkExtent2D extent;
	VkRenderPass pass;
	struct mview_image depth;
	struct mview_target *targets;
	uint32_t count;
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;
	VkSemaphore acquired;
	struct mview_buffer staging;
	struct mview_buffer vertices;
	struct mview_buffer indices;
	struct mview_image *textures;
	uint32_t texture_count;
	VkSampler sampler;
	VkDescriptorSetLayout set_layout;
	VkDescriptorPool descriptor_pool;
	VkDescriptorSet *sets;
	VkPipelineLayout layout;
	VkShaderModule vertex_shader;
	VkShaderModule fragment_shader;
	VkShaderModule cutout_shader;
	VkPipeline pipelines[MVIEW_PIPELINE_COUNT];

	/* Nonzero for per-pixel lighting (--shading=pixel): the scene block in a uniform buffer, binding 1 of every set. */
	int pixel_shading;
	struct mview_buffer scene;

	/* The presentation mode asked for (--present) and the one the swapchain uses (FIFO when the asked one is not offered). */
	VkPresentModeKHR wanted_present;
	VkPresentModeKHR present_mode;

	/*
	 * How many copies of the model each frame draws, side by side in a grid
	 * (--copies), and whether each copy is its own draws (--draws=each)
	 * instead of one instanced draw a material group.  The copies' places
	 * are a per-instance vertex buffer of four floats each (the shift in
	 * clip space, then in view space), rewritten only when the shift of
	 * one column or one row (basis) changes.
	 */
	uint32_t copies;
	int each_draw;

	/*
	 * How many submissions draw a frame (--submits), each its own command
	 * buffer and its own part of the copies; with more than one, the
	 * passes that open (clear), continue and close (present) a frame.
	 */
	uint32_t submits;
	VkCommandBuffer *commands;

	/*
	 * How many frames may be in flight (--inflight, one by default): with
	 * one, a frame waits for its own fence before the next is recorded;
	 * with more, each slot has its command buffers (commands holds
	 * submits of them a slot), fence and acquire semaphore, and a frame
	 * waits only for the frame that last used its slot.  fence and
	 * acquired are the current slot's.
	 */
	uint32_t inflight;
	uint32_t slot;
	VkFence slot_fences[MVIEW_INFLIGHT_MAX];
	VkSemaphore slot_acquired[MVIEW_INFLIGHT_MAX];

	/*
	 * Nonzero when every pipeline culls both faces (--cull=all): the
	 * vertex work alone, nothing rasterized, for measuring the geometry
	 * side of the scene.
	 */
	int cull_all;

	/* The swapchain's image count asked for (--images), 0 for the viewer's own choice. */
	uint32_t images;
	VkRenderPass part_passes[3];
	struct mview_buffer instances;
	float instance_basis[6];
	uint32_t max_texture_side;
	int blit_mipmaps;
	int linear_filter;
	const char *operation;

	/*
	 * Nonzero when the viewer draws with no window (--offscreen): its own
	 * colour images stand in for the swapchain's, frames are drawn into
	 * them in turn and never presented, and mview_renderer_read copies the
	 * latest one back.  offscreen_images has count entries.
	 */
	int offscreen;
	struct mview_image *offscreen_images;
	uint32_t offscreen_latest;
};

/* Window lifetime encloses all renderer use of the borrowed native surface. */
int mview_window_open(struct mview_window *window, const char *display, struct mview_input *input, uint32_t width, uint32_t height, int fullscreen, uint32_t content);
int mview_window_dispatch(struct mview_window *window, int timeout);
void mview_window_close(struct mview_window *window);

/* Renderer cleanup releases Vulkan ownership before the native window closes. */
VkResult mview_renderer_open(struct mview_renderer *renderer, struct mview_window *window, uint32_t width, uint32_t height, int pixel_shading, VkPresentModeKHR present_mode, uint32_t copies, int each_draw, uint32_t submits, uint32_t inflight, int cull_all, uint32_t images);
VkResult mview_renderer_load(struct mview_renderer *renderer, const struct mview_model *model);
VkResult mview_renderer_draw(struct mview_renderer *renderer, const struct mview_model *model, const struct mview_camera *camera);
VkResult mview_renderer_recreate(struct mview_renderer *renderer, uint32_t width, uint32_t height);
VkResult mview_renderer_close(struct mview_renderer *renderer);
VkResult mview_renderer_read(struct mview_renderer *renderer, const char *path, uint32_t *checksum);

#endif
