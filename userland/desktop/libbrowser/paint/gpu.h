/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GPU renderer (plan/ws074/design.md §8.2): a display list drawn with
 * Vulkan, one instanced quad per rectangle or glyph, into a framebuffer of
 * the caller's image (the view's target, <browser/browser.h>).  It either submits
 * the drawing itself and waits for it (paint_gpu_draw), or records it into
 * the caller's command buffer (paint_gpu_prepare, then paint_gpu_record).
 * The offscreen image (paint_offscreen) is a device and an image of the
 * renderer's own that a drawing is read back from (the headless
 * --render-gpu mode and the tests).
 *
 * It follows the CPU reference renderer's coverage rules (paint.h), so the
 * two pictures can be compared pixel by pixel.  Glyph bitmaps are copied
 * into an atlas image the first time they are drawn.
 */

#ifndef KEILAND_BROWSER_PAINT_GPU_H
#define KEILAND_BROWSER_PAINT_GPU_H

#include "paint/paint.h"

#include <vulkan/vulkan.h>

/* The atlas's width and height, in texels (glyphs and images share it). */
#define PAINT_GPU_ATLAS_SIZE	2048U

struct paint_gpu_slot;
struct paint_gpu_image;

/*
 * The Vulkan objects of the GPU renderer.
 *
 * The instance, the device and its queue are the caller's.  One frame is
 * drawn at a time: a draw waits for its fence, and the caller of a
 * recorded frame waits for its own before the next one, so the host may
 * write the atlas and the instances between frames.
 */
struct paint_gpu {
	/* The device the renderer draws with. */
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;

	/* The pass (its color format and the layout it leaves the image in), the pipeline and what it binds. */
	VkFormat format;
	VkRenderPass pass;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkDescriptorPool descriptor_pool;
	VkDescriptorSet set;
	VkSampler sampler;

	/* The frame's items, six vertices each, host-visible and mapped, with room for capacity of them. */
	VkBuffer instances;
	VkDeviceMemory instance_memory;
	void *instance_map;
	size_t instance_capacity;
	struct wb_vector staged;

	/* The atlas: a linear, host-written image, mapped for good, and whether it left its first layout. */
	VkImage atlas;
	VkDeviceMemory atlas_memory;
	VkImageView atlas_view;
	unsigned char *atlas_map;
	size_t atlas_pitch;
	int atlas_ready;

	/*
	 * The atlas's packing: glyphs go left to right on shelves as tall as
	 * their tallest glyph.  The table maps a glyph's bitmap (which the text
	 * system keeps for its life) to its place; full says a glyph did not fit
	 * and the atlas starts over before the next frame.  Images go on the same
	 * shelves; their places are a list by the image's serial (an image's
	 * pixels may be freed and their memory used again, a serial never).
	 */
	uint32_t shelf_x;
	uint32_t shelf_y;
	uint32_t shelf_height;
	struct paint_gpu_slot *slots;
	size_t slot_capacity;
	size_t slot_count;
	struct wb_vector images;
	int full;

	/* One command buffer and the fence its submission signals. */
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;

	/* The Vulkan call that failed last, for the error line. */
	const char *operation;
};

/*
 * A device and an image of the renderer's own, for drawing without a
 * window: the instance, the device with its queue, the image (color
 * attachment and transfer source, in format) with its memory and view,
 * its extent, and the command buffer and fence of the copy that reads it
 * back.  operation names the Vulkan call that failed last.
 */
struct paint_offscreen {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	VkFormat format;
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkExtent2D extent;
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;
	const char *operation;
};

/* The GPU renderer (vulkan.c). */
VkResult paint_gpu_open(struct paint_gpu *gpu, VkInstance instance, VkPhysicalDevice physical, uint32_t family, VkDevice device, VkFormat format, VkImageLayout final_layout);
VkResult paint_gpu_prepare(struct paint_gpu *gpu, const struct paint_list *list, struct text_system *text, layout_unit scroll_y, VkExtent2D extent);
void paint_gpu_record(struct paint_gpu *gpu, VkCommandBuffer commands, const struct paint_list *list, VkFramebuffer framebuffer, VkExtent2D extent);
VkResult paint_gpu_draw(struct paint_gpu *gpu, const struct paint_list *list, struct text_system *text, layout_unit scroll_y, VkFramebuffer framebuffer, VkExtent2D extent, VkSemaphore wait, VkSemaphore signal);
void paint_gpu_forget_glyphs(struct paint_gpu *gpu);
void paint_gpu_close(struct paint_gpu *gpu);

/* The offscreen image (vulkan.c). */
VkResult paint_offscreen_open(struct paint_offscreen *offscreen, uint32_t width, uint32_t height);
VkResult paint_offscreen_read(struct paint_offscreen *offscreen, uint32_t *pixels, size_t stride);
void paint_offscreen_close(struct paint_offscreen *offscreen);

#endif
