/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Vulkan drawing of terminal (render.c), which display-vulkan.c shows
 * the terminal's frames with.  The Vulkan header comes before libkeiland's,
 * which then declares the window's Vulkan surface.
 */

#ifndef TERMINAL_RENDER_H
#define TERMINAL_RENDER_H

#include <vulkan/vulkan.h>

#include "terminal.h"

/*
 * One swapchain image the renderer draws into; the image is the swapchain's.
 */
struct terminal_target {
	VkImage image;
	VkImageView view;
	VkFramebuffer framebuffer;
	VkSemaphore rendered;
	/*
	 * The image's frame recorded (made at its first frame, recorded again
	 * only when what it records changes: the vertices' count or the
	 * background), and what it was recorded with; recorded is zero until
	 * a recording may be submitted again as it is.
	 */
	VkCommandBuffer command;
	uint32_t vertex_count;
	uint32_t ground;
	int recorded;
};

/*
 * The Vulkan objects of the terminal's window.
 *
 * They are made once the window is configured and remade in part when the
 * window changes size.
 */
struct terminal_renderer {
	/* The instance, the surface of the window, the device and its queue. */
	VkInstance instance;
	VkSurfaceKHR surface;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;

	/* The swapchain, its format and extent, and one target per image. */
	VkSwapchainKHR swapchain;
	VkFormat format;
	VkExtent2D extent;
	struct terminal_target *targets;
	uint32_t count;

	/* The pass and the pipeline that draw cells, and what the pipeline binds. */
	VkRenderPass pass;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkDescriptorPool descriptor_pool;
	VkDescriptorSet set;
	VkSampler sampler;

	/*
	 * The glyph atlas: an optimal image in the device's own memory and its
	 * view, and the host-written buffer the font draws into (staging,
	 * mapped for good at staging_map).  The rows the font wrote are copied
	 * from the buffer into the image by the upload command buffer, which
	 * runs before the frame that needs them; atlas_defined is nonzero once
	 * the image has had its first copy (its layout is then the shader's).
	 */
	VkImage atlas;
	VkDeviceMemory atlas_memory;
	VkImageView atlas_view;
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	void *staging_map;
	VkCommandBuffer upload;
	int atlas_defined;

	/* The vertices of one frame, in host-visible memory mapped for good. */
	VkBuffer vertices;
	VkDeviceMemory vertex_memory;
	void *vertex_map;

	/*
	 * The processor reaches the mapping around its cache, so a frame's
	 * rows are built in the host's own memory (build, each at its place)
	 * and only the doublewords that changed in the parts built (the
	 * build's ranges, in vertices) are stored into the mapping
	 * (render_upload); vertex_kept is a copy of the mapping, of which the
	 * first vertex_known doublewords were written.
	 */
	struct terminal_build build;
	float *vertex_kept;
	size_t vertex_known;

	/*
	 * The command pool (each image has its command buffer, its target's),
	 * the fence that says the frame finished (pending while a frame
	 * submitted has not been waited for) and the acquire semaphore.
	 */
	VkCommandPool pool;
	VkFence fence;
	int pending;
	VkSemaphore acquired;

	/* The Vulkan call that failed last, for the error line. */
	const char *operation;
};

/* The drawing (render.c). */
VkResult terminal_renderer_open(struct terminal_renderer *renderer, struct terminal_window *window, struct terminal_font *font);
VkResult terminal_renderer_resize(struct terminal_renderer *renderer, uint32_t width, uint32_t height);
VkResult terminal_renderer_draw(struct terminal_renderer *renderer, struct terminal_screen *screen, struct terminal_font *font, const struct terminal_window *window);
VkResult terminal_renderer_idle(struct terminal_renderer *renderer);
void terminal_renderer_close(struct terminal_renderer *renderer);

#endif
