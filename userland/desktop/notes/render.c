/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The drawing of Notes with Vulkan and the Wayland WSI.
 *
 * A frame is the list of draws geometry.c builds: plain rectangles (the
 * desk and the page's shadow), strokes filled through the stencil buffer in
 * three passes (fan, fringe, cover; see geometry.c), and pictures.  Five
 * pipelines share one vertex layout and are made with the renderer:
 *
 *   stencil  counts the winding of the fan into the stencil, no colour;
 *   fringe   draws where the stencil is zero, alpha falling with distance;
 *   cover    draws where the stencil is not zero, and sets it back to zero;
 *   plain    draws without the stencil;
 *   texture  draws a picture without the stencil.
 *
 * ws216-p004: two groups more are made before the first frame that uses
 * them, so that a device without them still starts and draws the pen: the
 * grain's fringe and cover, which draw a pencil through the paper's grain
 * (its tile in three sizes, each a picture of its own), and the Multiply's,
 * which multiply a watercolour into the page.  A group the device refuses
 * is logged once and its draws are drawn by the fringe and the cover, the
 * vertices carrying what those draw (a flat pencil, a watercolour laid
 * over).
 *
 * There are four pictures.  The toolbar's is drawn on the CPU into a linear
 * image, and so is the background: the page of the PDF the notebook writes
 * on, which libpdf draws on the CPU; and so is the overlay, the window's
 * size, which shows the text box over the page (ws175-p008).  The page's holds the page (with its
 * background) and its finished strokes: a second list
 * of draws, the page frame, is drawn into it with the same pipelines before
 * the frame when the page changed -- from a cleared picture after the page
 * was turned or a stroke was removed, or on top of the last picture when
 * strokes were only added -- so that a frame draws the page as one picture
 * and only the stroke being drawn as geometry (design-input-notes.md
 * section 5.3).
 *
 * Each frame's vertices are copied to host-visible memory, drawn, and
 * waited for before the next frame, so the host may write the vertices and
 * the toolbar's image between frames without further synchronization.
 * The vertices go in chunks of at most NOTES_CHUNK_BYTES (ws216-p004: i915
 * refuses an allocation of more than 16 MiB, and a dense page has more),
 * each a whole number of triangles; a draw is cut where a chunk ends and
 * goes on in the next, in the same pass, so the stencil it counts holds.
 */

#include "app.h"
#include "shaders.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How long a frame may take on the GPU, in nanoseconds. */
#define RENDER_TIMEOUT		10000000000ULL

/* The bytes of one vertex. */
#define RENDER_VERTEX_BYTES	(NOTES_VERTEX_FLOATS * sizeof(float))

/* The bytes of the push constants: the target's size, and the page's place for the grain (ws216-p004). */
#define RENDER_PUSH_BYTES	(8U * sizeof(float))

/* A frame's vertices are logged again when they are this many quarters of the count logged last. */
#define RENDER_PEAK_QUARTERS	5U

/* The FNV-1a hash's start and its prime, for the grain's log line. */
#define RENDER_FNV_START	0x811c9dc5U
#define RENDER_FNV_PRIME	0x01000193U

/* The kinds of the page's passes: from a cleared picture, adding to the last, and the fill's band (ws216-p005). */
#define RENDER_PAGE_CLEAR	0U
#define RENDER_PAGE_LOAD	1U
#define RENDER_PAGE_BAND	2U

/* The colour the window is cleared to before the desk is drawn over it (its pale sky), as floats. */
#define RENDER_DESK_RED		0.894f
#define RENDER_DESK_GREEN	0.929f
#define RENDER_DESK_BLUE	0.969f

static VkResult render_device(struct notes_renderer *renderer);
static VkResult render_swapchain(struct notes_renderer *renderer, uint32_t width, uint32_t height, VkSwapchainKHR old);
static VkResult render_stencil_format(struct notes_renderer *renderer);
static VkResult render_stencil(struct notes_renderer *renderer);
static VkResult render_stencil_image(struct notes_renderer *renderer, uint32_t width, uint32_t height, VkImage *image, VkDeviceMemory *memory, VkImageView *view);
static void render_stencil_free(struct notes_renderer *renderer);
static VkResult render_pass(struct notes_renderer *renderer);
static VkResult render_page_pass(struct notes_renderer *renderer, unsigned kind, VkRenderPass *pass);
static VkResult render_page_make(struct notes_renderer *renderer, uint32_t width, uint32_t height);
static VkResult render_band_make(struct notes_renderer *renderer, uint32_t width, uint32_t height);
static VkResult render_band_record(struct notes_renderer *renderer, const struct notes_frame *frame);
static void render_page_free(struct notes_renderer *renderer);
static VkResult render_targets(struct notes_renderer *renderer);
static void render_targets_free(struct notes_renderer *renderer);
static VkResult render_commands(struct notes_renderer *renderer);
static VkResult render_memory(struct notes_renderer *renderer, const VkMemoryRequirements *requirements, VkMemoryPropertyFlags wanted, VkDeviceMemory *memory);
static VkResult render_descriptors(struct notes_renderer *renderer);
static VkResult render_toolbar(struct notes_renderer *renderer);
static VkResult render_linear(struct notes_renderer *renderer, uint32_t width, uint32_t height, VkDescriptorSet set, VkSampler sampler, VkImage *made, VkDeviceMemory *memory, VkImageView *made_view, unsigned char **pixels, size_t *pitch);
static void render_background_free(struct notes_renderer *renderer);
static void render_overlay_free(struct notes_renderer *renderer);
static void render_toolbar_free(struct notes_renderer *renderer);
static void render_chunk_size(struct notes_renderer *renderer, size_t bytes);
static VkResult render_chunks(struct notes_renderer *renderer, size_t vertices);
static VkResult render_chunks_add(struct notes_renderer *renderer, size_t vertices);
static VkResult render_chunk(struct notes_renderer *renderer, struct notes_chunk *chunk);
static void render_chunk_free(struct notes_renderer *renderer, struct notes_chunk *chunk);
static void render_chunks_free(struct notes_renderer *renderer);
static void render_copy(struct notes_renderer *renderer, const float *vertices, size_t first, size_t count);
static void render_peak(struct notes_renderer *renderer, size_t vertices);
static void render_groups(struct notes_renderer *renderer, const struct notes_frame *frame);
static void render_group(struct notes_renderer *renderer, unsigned group);
static VkResult render_group_make(struct notes_renderer *renderer, unsigned group);
static void render_group_free(struct notes_renderer *renderer, unsigned group);
static VkResult render_grain(struct notes_renderer *renderer);
static VkResult render_grain_picture(struct notes_renderer *renderer, unsigned level, const unsigned char *tile);
static unsigned render_pipe_group(unsigned pipe);
static unsigned render_pipe(const struct notes_renderer *renderer, unsigned pipe);
static VkResult render_pipelines(struct notes_renderer *renderer);
static VkResult render_pipeline(struct notes_renderer *renderer, unsigned pipe, VkShaderModule vertex, VkShaderModule fragment);
static VkResult render_module(struct notes_renderer *renderer, const uint32_t *code, size_t size, VkShaderModule *module);
static void render_record(struct notes_renderer *renderer, uint32_t image, const struct notes_frame *frame, const struct notes_frame *page_frame, int page_clear);
static void render_pictures_ready(struct notes_renderer *renderer);
static void render_picture_ready(struct notes_renderer *renderer, VkImage image);
static void render_draws(struct notes_renderer *renderer, const struct notes_frame *frame, uint32_t base, VkExtent2D extent);
static void render_pieces(struct notes_renderer *renderer, uint32_t first, uint32_t count, size_t *bound_chunk);
static void render_grain_place(const struct notes_draw *draw, float *grain);

/*
 * Makes the Vulkan objects of the window: the device, the swapchain and
 * its stencil buffer, the pipelines, the toolbar's image and the first
 * chunk of the vertices.  The groups of pipelines a page may need later
 * (the grain's, the Multiply's) are not made here.
 */
VkResult
notes_renderer_open(
	struct notes_renderer *renderer,
	struct notes_window *window)
{
	VkApplicationInfo application;
	VkInstanceCreateInfo instance;
	const char *extensions[2];
	VkResult error;
	int failed;

	/* Nothing is owned yet. */
	memset(renderer, 0, sizeof(*renderer));

	/* The instance, with the surface extensions a Wayland window needs. */
	extensions[0] = VK_KHR_SURFACE_EXTENSION_NAME;
	extensions[1] = VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME;
	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "notes";
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&instance, 0, sizeof(instance));
	instance.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance.pApplicationInfo = &application;
	instance.enabledExtensionCount = 2U;
	instance.ppEnabledExtensionNames = extensions;
	renderer->operation = "vkCreateInstance";
	error = vkCreateInstance(&instance, NULL, &renderer->instance);
	if (error != VK_SUCCESS)
		return error;

	/* The window's surface (libkeiland's, WS131 p018). */
	renderer->operation = "kl_window_vulkan_surface";
	failed = kl_window_vulkan_surface(window->kui, renderer->instance, &renderer->surface);
	if (failed != 0)
		return VK_ERROR_INITIALIZATION_FAILED;

	/* A device with a queue that draws and presents to the surface. */
	error = render_device(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* The swapchain at the window's size. */
	error = render_swapchain(renderer, window->width, window->height, VK_NULL_HANDLE);
	if (error != VK_SUCCESS)
		return error;

	/* A stencil format the device can render to. */
	error = render_stencil_format(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* The stencil buffer at the swapchain's size. */
	error = render_stencil(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* The pass that draws into a swapchain image and the stencil buffer. */
	error = render_pass(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* The page's pass that starts from a cleared picture. */
	error = render_page_pass(renderer, RENDER_PAGE_CLEAR, &renderer->page_clear_pass);
	if (error != VK_SUCCESS)
		return error;

	/* And the one that adds to the picture the last frame left. */
	error = render_page_pass(renderer, RENDER_PAGE_LOAD, &renderer->page_load_pass);
	if (error != VK_SUCCESS)
		return error;

	/* A framebuffer for each swapchain image. */
	error = render_targets(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* The command buffer and the frame's synchronization. */
	error = render_commands(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* The sampler and the descriptor set of the toolbar's image. */
	error = render_descriptors(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* The toolbar's image at the window's width. */
	error = render_toolbar(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* The first chunk of the vertices, at the largest size. */
	render_chunk_size(renderer, NOTES_CHUNK_BYTES);
	error = render_chunks(renderer, 1U);
	if (error != VK_SUCCESS)
		return error;

	/* The pipelines. */
	error = render_pipelines(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: frames can be drawn. */
	return VK_SUCCESS;
}

/*
 * Replaces the swapchain, the stencil buffer and the toolbar's image with
 * ones of a new size.
 */
VkResult
notes_renderer_resize(
	struct notes_renderer *renderer,
	uint32_t width,
	uint32_t height)
{
	VkSwapchainKHR old;
	VkResult error;

	/* Nothing may still use the old images. */
	renderer->operation = "vkDeviceWaitIdle";
	error = vkDeviceWaitIdle(renderer->device);
	if (error != VK_SUCCESS)
		return error;

	/*
	 * The old targets, the page's picture (whose framebuffer holds the
	 * stencil buffer) and the stencil buffer go, and the new chain replaces
	 * the old one.  The next frame makes the page's picture again.
	 */
	render_targets_free(renderer);
	render_page_free(renderer);
	render_stencil_free(renderer);
	old = renderer->swapchain;
	renderer->swapchain = VK_NULL_HANDLE;
	error = render_swapchain(renderer, width, height, old);
	vkDestroySwapchainKHR(renderer->device, old, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* The stencil buffer at the new size. */
	error = render_stencil(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Framebuffers for the new images. */
	error = render_targets(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* The toolbar's image at the new width. */
	render_toolbar_free(renderer);
	error = render_toolbar(renderer);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the next frame is drawn at the new size. */
	return VK_SUCCESS;
}

/*
 * Makes the page's picture at a size in pixels (at most NOTES_PICTURE_MAX
 * either way), with its stencil buffer, unless it has that size already.
 *
 * A picture made again is empty and has a new page_serial: the caller
 * draws the whole page into it with the next frame.  A picture that could
 * not be made leaves none (BUG-296: a device may refuse a large one, and
 * the caller asks for a smaller).
 */
VkResult
notes_renderer_page(
	struct notes_renderer *renderer,
	uint32_t width,
	uint32_t height)
{
	VkResult error;

	/* The picture is at most NOTES_PICTURE_MAX either way (it has a stencil buffer of its own), and never empty. */
	if (width == 0U)
		width = 1U;
	if (width > NOTES_PICTURE_MAX)
		width = NOTES_PICTURE_MAX;
	if (height == 0U)
		height = 1U;
	if (height > NOTES_PICTURE_MAX)
		height = NOTES_PICTURE_MAX;

	/* A picture of the size stands. */
	if (renderer->page != VK_NULL_HANDLE &&
	    renderer->page_width == width &&
	    renderer->page_height == height)
		return VK_SUCCESS;

	/* The old picture goes once nothing uses it. */
	renderer->operation = "vkDeviceWaitIdle";
	error = vkDeviceWaitIdle(renderer->device);
	if (error != VK_SUCCESS)
		return error;
	render_page_free(renderer);

	/* A new picture, or none at all when any part of it could not be made. */
	error = render_page_make(renderer, width, height);
	if (error != VK_SUCCESS) {
		render_page_free(renderer);
		return error;
	}

	/* Succeeded: a new, empty picture, which the serial announces. */
	renderer->page_width = width;
	renderer->page_height = height;
	renderer->page_serial++;
	return VK_SUCCESS;
}

/*
 * Draws a frame and presents it, and waits for it to finish.
 *
 * A page frame, when given, is drawn into the page's picture first: into a
 * cleared picture when page_clear is set, on top of the last one otherwise.
 * A group of pipelines the frames use for the first time is made first;
 * one the device refuses is drawn instead and does not fail the frame.
 * Returns VK_ERROR_OUT_OF_DATE_KHR when the swapchain no longer matches
 * the window; the caller resizes and draws again.
 */
VkResult
notes_renderer_draw(
	struct notes_renderer *renderer,
	const struct notes_frame *frame,
	const struct notes_frame *page_frame,
	int page_clear)
{
	VkSubmitInfo submit;
	VkPresentInfoKHR present;
	VkPipelineStageFlags stage;
	size_t page_vertices;
	size_t vertices;
	uint32_t image;
	VkResult error;

	/* The page frame's vertices go first in the buffer, the frame's after them. */
	page_vertices = 0;
	if (page_frame != NULL)
		page_vertices = page_frame->vertex_count;
	vertices = page_vertices + frame->vertex_count;

	/* The groups of pipelines the frames are the first to use. */
	render_groups(renderer, page_frame);
	render_groups(renderer, frame);

	/* The chunks that hold every vertex, more of them when the frames have more than ever. */
	error = render_chunks(renderer, vertices);
	if (error != VK_SUCCESS)
		return error;
	render_peak(renderer, vertices);

	/* The page frame's vertices. */
	if (page_vertices != 0U)
		render_copy(renderer, page_frame->vertices, 0U, page_vertices);

	/* The frame's vertices, after them. */
	if (frame->vertex_count != 0U)
		render_copy(renderer, frame->vertices, page_vertices, frame->vertex_count);

	/* The image to draw into, once the compositor has given one back, however long a busy compositor takes (a closed connection ends the wait with an error). */
	renderer->operation = "vkAcquireNextImageKHR";
	error = vkAcquireNextImageKHR(renderer->device, renderer->swapchain, UINT64_MAX, renderer->acquired, VK_NULL_HANDLE, &image);
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return error;

	/* The frame's commands. */
	renderer->operation = "vkResetCommandBuffer";
	error = vkResetCommandBuffer(renderer->command, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Records the frame and closes the recording. */
	render_record(renderer, image, frame, page_frame, page_clear);
	renderer->operation = "vkEndCommandBuffer";
	error = vkEndCommandBuffer(renderer->command);
	if (error != VK_SUCCESS)
		return error;

	/* The frame's fence starts unsignalled. */
	renderer->operation = "vkResetFences";
	error = vkResetFences(renderer->device, 1U, &renderer->fence);
	if (error != VK_SUCCESS)
		return error;

	/* Submits the frame after the acquire, signalling the image's present semaphore. */
	stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.waitSemaphoreCount = 1U;
	submit.pWaitSemaphores = &renderer->acquired;
	submit.pWaitDstStageMask = &stage;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &renderer->command;
	submit.signalSemaphoreCount = 1U;
	submit.pSignalSemaphores = &renderer->targets[image].rendered;
	renderer->operation = "vkQueueSubmit";
	error = vkQueueSubmit(renderer->queue, 1U, &submit, renderer->fence);
	if (error != VK_SUCCESS)
		return error;

	/* Presents the image to the window. */
	memset(&present, 0, sizeof(present));
	present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	present.waitSemaphoreCount = 1U;
	present.pWaitSemaphores = &renderer->targets[image].rendered;
	present.swapchainCount = 1U;
	present.pSwapchains = &renderer->swapchain;
	present.pImageIndices = &image;
	renderer->operation = "vkQueuePresentKHR";
	error = vkQueuePresentKHR(renderer->queue, &present);
	if (error != VK_SUCCESS && error != VK_SUBOPTIMAL_KHR)
		return error;

	/* The frame is finished before the host touches the vertices or the toolbar again. */
	renderer->operation = "vkWaitForFences";
	error = vkWaitForFences(renderer->device, 1U, &renderer->fence, VK_TRUE, RENDER_TIMEOUT);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the frame is on the window. */
	return VK_SUCCESS;
}

/*
 * Gives the pixels of the page's background picture (B8G8R8A8) at a size,
 * and their row pitch, for the host to draw the page of the PDF the
 * notebook writes on into between frames; the page frame shows it with
 * NOTES_TEXTURE_BACKGROUND.
 *
 * An image of another size is made again (its content is gone); an image
 * of the size keeps what the host drew into it last.
 */
VkResult
notes_renderer_background(
	struct notes_renderer *renderer,
	uint32_t width,
	uint32_t height,
	unsigned char **pixels,
	size_t *pitch)
{
	VkResult error;

	/* An image of the size stands. */
	if (renderer->background != VK_NULL_HANDLE &&
	    renderer->background_width == width &&
	    renderer->background_height == height) {
		*pixels = renderer->background_pixels;
		*pitch = renderer->background_pitch;
		return VK_SUCCESS;
	}

	/* The old image goes once nothing uses it. */
	renderer->operation = "vkDeviceWaitIdle";
	error = vkDeviceWaitIdle(renderer->device);
	if (error != VK_SUCCESS)
		return error;
	render_background_free(renderer);

	/* The new image, named in the background's set. */
	error = render_linear(renderer,
			      width,
			      height,
			      renderer->sets[NOTES_TEXTURE_BACKGROUND],
			      renderer->linear_sampler,
			      &renderer->background,
			      &renderer->background_memory,
			      &renderer->background_view,
			      &renderer->background_pixels,
			      &renderer->background_pitch);
	if (error != VK_SUCCESS) {
		render_background_free(renderer);
		return error;
	}

	/* Succeeded: the next frame moves the image to the general layout. */
	renderer->background_width = width;
	renderer->background_height = height;
	renderer->background_ready = 0;
	*pixels = renderer->background_pixels;
	*pitch = renderer->background_pitch;
	return VK_SUCCESS;
}

/*
 * Gives the pixels of the overlay picture (B8G8R8A8, straight alpha) at a
 * size, and their row pitch, for the host to draw the text box into
 * between frames (ws175-p008); the frame shows it with
 * NOTES_TEXTURE_OVERLAY.  made is 1 when the image was made anew (its
 * pixels are transparent), 0 when it keeps what the host drew last.
 */
VkResult
notes_renderer_overlay(
	struct notes_renderer *renderer,
	uint32_t width,
	uint32_t height,
	unsigned char **pixels,
	size_t *pitch,
	int *made)
{
	VkResult error;
	uint32_t row;

	/* An image of the size stands. */
	*made = 0;
	if (renderer->overlay != VK_NULL_HANDLE &&
	    renderer->overlay_width == width &&
	    renderer->overlay_height == height) {
		*pixels = renderer->overlay_pixels;
		*pitch = renderer->overlay_pitch;
		return VK_SUCCESS;
	}

	/* The old image goes once nothing uses it. */
	renderer->operation = "vkDeviceWaitIdle";
	error = vkDeviceWaitIdle(renderer->device);
	if (error != VK_SUCCESS)
		return error;
	render_overlay_free(renderer);

	/* The new image, named in the overlay's set. */
	error = render_linear(renderer,
			      width,
			      height,
			      renderer->sets[NOTES_TEXTURE_OVERLAY],
			      renderer->sampler,
			      &renderer->overlay,
			      &renderer->overlay_memory,
			      &renderer->overlay_view,
			      &renderer->overlay_pixels,
			      &renderer->overlay_pitch);
	if (error != VK_SUCCESS) {
		render_overlay_free(renderer);
		return error;
	}

	/* Transparent all over. */
	for (row = 0; row < height; row++)
		memset(renderer->overlay_pixels + (size_t)row * renderer->overlay_pitch, 0, (size_t)width * 4U);

	/* Succeeded: the next frame moves the image to the general layout. */
	renderer->overlay_width = width;
	renderer->overlay_height = height;
	renderer->overlay_ready = 0;
	*pixels = renderer->overlay_pixels;
	*pitch = renderer->overlay_pitch;
	*made = 1;
	return VK_SUCCESS;
}

/*
 * Gives the toolbar's pixels (B8G8R8A8, the swapchain's width by
 * NOTES_TOOLBAR_IMAGE_HEIGHT) and their row pitch, for the toolbar to draw into
 * between frames.
 */
void
notes_renderer_toolbar(
	struct notes_renderer *renderer,
	unsigned char **pixels,
	size_t *pitch)
{
	/* The rows mapped when the image was made (NULL without the image). */
	*pixels = renderer->toolbar_pixels;
	*pitch = renderer->toolbar_pitch;
}

/*
 * Draws a frame into the fill's band, off the screen, and reads the band's
 * pixels back (ws216-p005): width × height pixels of 4 bytes, the
 * picture's format (the window's), rows one after another.
 *
 * The band's objects are made for a size and kept for the next band of
 * the size; notes_renderer_band_free() frees them after the fill.  The
 * last frame is finished (notes_renderer_draw() waits for it), so the
 * vertices and the command buffer are free.  Returns VK_SUCCESS, or the
 * error of the device (VK_ERROR_OUT_OF_DEVICE_MEMORY when it refused the
 * band's memory: the caller draws a smaller band).
 */
VkResult
notes_renderer_band(
	struct notes_renderer *renderer,
	const struct notes_frame *frame,
	uint32_t width,
	uint32_t height,
	unsigned char *pixels)
{
	VkSubmitInfo submit;
	VkResult error;

	/*
	 * Nothing the device runs uses the vertices, the command buffer or the
	 * band any more (a frame that failed to present did not wait for its
	 * fence, review-1 M-1).
	 */
	renderer->operation = "vkDeviceWaitIdle";
	error = vkDeviceWaitIdle(renderer->device);
	if (error != VK_SUCCESS)
		return error;

	/* Objects of another size are made again. */
	if (renderer->band == VK_NULL_HANDLE ||
	    renderer->band_width != width ||
	    renderer->band_height != height) {
		notes_renderer_band_free(renderer);

		/* The band's objects, or none at all when any part could not be made. */
		error = render_band_make(renderer, width, height);
		if (error != VK_SUCCESS) {
			notes_renderer_band_free(renderer);
			return error;
		}
		renderer->band_width = width;
		renderer->band_height = height;
	}

	/* The groups of pipelines the frame is the first to use, as for the screen. */
	render_groups(renderer, frame);

	/* The chunks that hold the frame's vertices, and the vertices. */
	error = render_chunks(renderer, frame->vertex_count);
	if (error != VK_SUCCESS)
		return error;
	if (frame->vertex_count != 0U)
		render_copy(renderer, frame->vertices, 0U, frame->vertex_count);

	/* The band's commands. */
	error = render_band_record(renderer, frame);
	if (error != VK_SUCCESS)
		return error;

	/* The submission's fence starts unsignalled. */
	renderer->operation = "vkResetFences";
	error = vkResetFences(renderer->device, 1U, &renderer->fence);
	if (error != VK_SUCCESS)
		return error;

	/* Submits the band's commands alone. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &renderer->command;
	renderer->operation = "vkQueueSubmit";
	error = vkQueueSubmit(renderer->queue, 1U, &submit, renderer->fence);
	if (error != VK_SUCCESS)
		return error;

	/* The band is in the host's buffer once the commands finished. */
	renderer->operation = "vkWaitForFences";
	error = vkWaitForFences(renderer->device, 1U, &renderer->fence, VK_TRUE, RENDER_TIMEOUT);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the band's pixels for the caller. */
	memcpy(pixels, renderer->band_map, (size_t)width * height * 4U);
	return VK_SUCCESS;
}

/*
 * Frees the fill's band: its pass, picture, stencil buffer, framebuffer
 * and buffer, where made, once the device is idle.
 */
void
notes_renderer_band_free(
	struct notes_renderer *renderer)
{
	/*
	 * Nothing runs that uses them: a band whose fence timed out or failed
	 * may still be drawn (p005 code review 3).
	 */
	if (renderer->band != VK_NULL_HANDLE || renderer->band_buffer != VK_NULL_HANDLE)
		(void)vkDeviceWaitIdle(renderer->device);

	/* The framebuffer, the picture's view, image and memory. */
	if (renderer->band_framebuffer != VK_NULL_HANDLE)
		vkDestroyFramebuffer(renderer->device, renderer->band_framebuffer, NULL);
	if (renderer->band_view != VK_NULL_HANDLE)
		vkDestroyImageView(renderer->device, renderer->band_view, NULL);
	if (renderer->band != VK_NULL_HANDLE)
		vkDestroyImage(renderer->device, renderer->band, NULL);
	if (renderer->band_memory != VK_NULL_HANDLE)
		vkFreeMemory(renderer->device, renderer->band_memory, NULL);

	/* The stencil buffer. */
	if (renderer->band_stencil_view != VK_NULL_HANDLE)
		vkDestroyImageView(renderer->device, renderer->band_stencil_view, NULL);
	if (renderer->band_stencil != VK_NULL_HANDLE)
		vkDestroyImage(renderer->device, renderer->band_stencil, NULL);
	if (renderer->band_stencil_memory != VK_NULL_HANDLE)
		vkFreeMemory(renderer->device, renderer->band_stencil_memory, NULL);

	/* The host's buffer and its memory (which unmaps it), and the pass. */
	if (renderer->band_buffer != VK_NULL_HANDLE)
		vkDestroyBuffer(renderer->device, renderer->band_buffer, NULL);
	if (renderer->band_buffer_memory != VK_NULL_HANDLE)
		vkFreeMemory(renderer->device, renderer->band_buffer_memory, NULL);
	if (renderer->band_pass != VK_NULL_HANDLE)
		vkDestroyRenderPass(renderer->device, renderer->band_pass, NULL);

	/* Nothing is left. */
	renderer->band_framebuffer = VK_NULL_HANDLE;
	renderer->band_view = VK_NULL_HANDLE;
	renderer->band = VK_NULL_HANDLE;
	renderer->band_memory = VK_NULL_HANDLE;
	renderer->band_stencil_view = VK_NULL_HANDLE;
	renderer->band_stencil = VK_NULL_HANDLE;
	renderer->band_stencil_memory = VK_NULL_HANDLE;
	renderer->band_buffer = VK_NULL_HANDLE;
	renderer->band_buffer_memory = VK_NULL_HANDLE;
	renderer->band_map = NULL;
	renderer->band_pass = VK_NULL_HANDLE;
	renderer->band_width = 0;
	renderer->band_height = 0;
}

/*
 * Releases every Vulkan object, children before their parents.
 */
void
notes_renderer_close(
	struct notes_renderer *renderer)
{
	unsigned pipe;
	unsigned group;

	/* The device's objects, once nothing runs. */
	if (renderer->device != VK_NULL_HANDLE) {
		(void)vkDeviceWaitIdle(renderer->device);
		render_targets_free(renderer);
		render_page_free(renderer);
		notes_renderer_band_free(renderer);
		render_stencil_free(renderer);
		render_toolbar_free(renderer);
		render_background_free(renderer);
		render_overlay_free(renderer);
		render_chunks_free(renderer);

		/* The groups made when first used: their pipelines, and the grain's pictures and sampler. */
		for (group = 0; group < NOTES_GROUPS; group++)
			render_group_free(renderer, group);

		/* The pipelines and what they bind. */
		for (pipe = 0; pipe < NOTES_PIPES; pipe++) {
			if (renderer->pipes[pipe] != VK_NULL_HANDLE)
				vkDestroyPipeline(renderer->device, renderer->pipes[pipe], NULL);
		}

		/* The layout, the set and its sampler. */
		if (renderer->layout != VK_NULL_HANDLE)
			vkDestroyPipelineLayout(renderer->device, renderer->layout, NULL);
		if (renderer->descriptor_pool != VK_NULL_HANDLE)
			vkDestroyDescriptorPool(renderer->device, renderer->descriptor_pool, NULL);
		if (renderer->set_layout != VK_NULL_HANDLE)
			vkDestroyDescriptorSetLayout(renderer->device, renderer->set_layout, NULL);
		if (renderer->sampler != VK_NULL_HANDLE)
			vkDestroySampler(renderer->device, renderer->sampler, NULL);
		if (renderer->linear_sampler != VK_NULL_HANDLE)
			vkDestroySampler(renderer->device, renderer->linear_sampler, NULL);

		/* The commands and the synchronization. */
		if (renderer->pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(renderer->device, renderer->pool, NULL);
		if (renderer->fence != VK_NULL_HANDLE)
			vkDestroyFence(renderer->device, renderer->fence, NULL);
		if (renderer->acquired != VK_NULL_HANDLE)
			vkDestroySemaphore(renderer->device, renderer->acquired, NULL);

		/* The passes, the swapchain and the device itself. */
		if (renderer->pass != VK_NULL_HANDLE)
			vkDestroyRenderPass(renderer->device, renderer->pass, NULL);
		if (renderer->page_clear_pass != VK_NULL_HANDLE)
			vkDestroyRenderPass(renderer->device, renderer->page_clear_pass, NULL);
		if (renderer->page_load_pass != VK_NULL_HANDLE)
			vkDestroyRenderPass(renderer->device, renderer->page_load_pass, NULL);
		if (renderer->swapchain != VK_NULL_HANDLE)
			vkDestroySwapchainKHR(renderer->device, renderer->swapchain, NULL);
		vkDestroyDevice(renderer->device, NULL);
	}

	/* The surface and the instance. */
	if (renderer->surface != VK_NULL_HANDLE)
		vkDestroySurfaceKHR(renderer->instance, renderer->surface, NULL);
	if (renderer->instance != VK_NULL_HANDLE)
		vkDestroyInstance(renderer->instance, NULL);

	/* Nothing is owned any more. */
	memset(renderer, 0, sizeof(*renderer));
}

/* Chooses a physical device and a queue family that draws and presents to the surface, and makes the device. */
static VkResult
render_device(
	struct notes_renderer *renderer)
{
	VkPhysicalDevice devices[8];
	VkQueueFamilyProperties families[16];
	VkDeviceQueueCreateInfo queue;
	VkDeviceCreateInfo create;
	const char *extension;
	float priority;
	uint32_t count;
	uint32_t family_count;
	uint32_t index;
	uint32_t family;
	VkBool32 supported;
	VkResult error;

	/* The physical devices (the first eight are enough). */
	count = 8U;
	renderer->operation = "vkEnumeratePhysicalDevices";
	error = vkEnumeratePhysicalDevices(renderer->instance, &count, devices);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;

	/* The first family of any device that draws and presents to this surface. */
	for (index = 0U; index < count && renderer->physical == VK_NULL_HANDLE; index++) {
		family_count = 16U;
		vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &family_count, families);
		for (family = 0U; family < family_count; family++) {
			/* A family must draw and have a queue. */
			if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0U || families[family].queueCount == 0U)
				continue;

			/* And present to this very surface. */
			supported = VK_FALSE;
			error = vkGetPhysicalDeviceSurfaceSupportKHR(devices[index], family, renderer->surface, &supported);
			if (error != VK_SUCCESS || supported == VK_FALSE)
				continue;

			/* This family of this device draws the window. */
			renderer->physical = devices[index];
			renderer->family = family;
			break;
		}
	}

	/* No device can draw this window. */
	if (renderer->physical == VK_NULL_HANDLE)
		return VK_ERROR_INITIALIZATION_FAILED;
	vkGetPhysicalDeviceMemoryProperties(renderer->physical, &renderer->memory);

	/* One queue of that family and the swapchain extension. */
	priority = 1.0f;
	memset(&queue, 0, sizeof(queue));
	queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue.queueFamilyIndex = renderer->family;
	queue.queueCount = 1U;
	queue.pQueuePriorities = &priority;
	extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	create.queueCreateInfoCount = 1U;
	create.pQueueCreateInfos = &queue;
	create.enabledExtensionCount = 1U;
	create.ppEnabledExtensionNames = &extension;
	renderer->operation = "vkCreateDevice";
	error = vkCreateDevice(renderer->physical, &create, NULL, &renderer->device);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the device and its queue. */
	vkGetDeviceQueue(renderer->device, renderer->family, 0U, &renderer->queue);
	return VK_SUCCESS;
}

/* Makes the swapchain at a size, in an 8-bit UNORM format, presenting in FIFO order. */
static VkResult
render_swapchain(
	struct notes_renderer *renderer,
	uint32_t width,
	uint32_t height,
	VkSwapchainKHR old)
{
	VkSurfaceCapabilitiesKHR capabilities;
	VkSurfaceFormatKHR formats[16];
	VkSwapchainCreateInfoKHR create;
	uint32_t count;
	uint32_t index;
	VkResult error;

	/* The formats the surface offers. */
	count = 16U;
	renderer->operation = "vkGetPhysicalDeviceSurfaceFormatsKHR";
	error = vkGetPhysicalDeviceSurfaceFormatsKHR(renderer->physical, renderer->surface, &count, formats);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;

	/* The first of those that is 8-bit UNORM. */
	renderer->format = VK_FORMAT_UNDEFINED;
	for (index = 0U; index < count; index++) {
		if (formats[index].format == VK_FORMAT_B8G8R8A8_UNORM || formats[index].format == VK_FORMAT_R8G8B8A8_UNORM) {
			renderer->format = formats[index].format;
			break;
		}
	}

	/* A surface without one cannot show the colours as they are. */
	if (renderer->format == VK_FORMAT_UNDEFINED)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* The size the surface takes. */
	renderer->operation = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR";
	error = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(renderer->physical, renderer->surface, &capabilities);
	if (error != VK_SUCCESS)
		return error;

	/* The window's size, clamped to the surface's range. */
	if (width < capabilities.minImageExtent.width)
		width = capabilities.minImageExtent.width;
	if (height < capabilities.minImageExtent.height)
		height = capabilities.minImageExtent.height;
	if (width > capabilities.maxImageExtent.width)
		width = capabilities.maxImageExtent.width;
	if (height > capabilities.maxImageExtent.height)
		height = capabilities.maxImageExtent.height;
	renderer->extent.width = width;
	renderer->extent.height = height;

	/* Three images when the surface allows, opaque, replacing the old chain. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	create.surface = renderer->surface;
	create.minImageCount = 3U;
	if (create.minImageCount < capabilities.minImageCount)
		create.minImageCount = capabilities.minImageCount;
	if (capabilities.maxImageCount != 0U && create.minImageCount > capabilities.maxImageCount)
		create.minImageCount = capabilities.maxImageCount;
	create.imageFormat = renderer->format;
	create.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	create.imageExtent = renderer->extent;
	create.imageArrayLayers = 1U;
	create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.preTransform = capabilities.currentTransform;
	create.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
	create.clipped = VK_TRUE;
	create.oldSwapchain = old;
	renderer->operation = "vkCreateSwapchainKHR";
	error = vkCreateSwapchainKHR(renderer->device, &create, NULL, &renderer->swapchain);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the chain at the window's size. */
	return VK_SUCCESS;
}

/* Chooses a format with a stencil the device can render to, preferring the common combined ones. */
static VkResult
render_stencil_format(
	struct notes_renderer *renderer)
{
	static const VkFormat candidates[] = {
		VK_FORMAT_D24_UNORM_S8_UINT,
		VK_FORMAT_D32_SFLOAT_S8_UINT,
		VK_FORMAT_S8_UINT,
		VK_FORMAT_D16_UNORM_S8_UINT
	};
	VkFormatProperties properties;
	unsigned index;

	/* The first candidate usable as an optimally tiled depth-stencil attachment. */
	for (index = 0; index < sizeof(candidates) / sizeof(candidates[0]); index++) {
		vkGetPhysicalDeviceFormatProperties(renderer->physical, candidates[index], &properties);
		if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0U)
			continue;

		/* A stencil-only format has one aspect, a combined one two. */
		renderer->stencil_format = candidates[index];
		renderer->stencil_aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		if (candidates[index] == VK_FORMAT_S8_UINT)
			renderer->stencil_aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
		return VK_SUCCESS;
	}

	/* No stencil: strokes cannot be filled. */
	renderer->operation = "stencil format";
	return VK_ERROR_FORMAT_NOT_SUPPORTED;
}

/* Makes the stencil buffer at the swapchain's size. */
static VkResult
render_stencil(
	struct notes_renderer *renderer)
{
	VkResult error;

	/* The window's stencil buffer. */
	error = render_stencil_image(renderer, renderer->extent.width, renderer->extent.height, &renderer->stencil,
				     &renderer->stencil_memory, &renderer->stencil_view);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the stencil buffer. */
	return VK_SUCCESS;
}

/* Makes a stencil buffer of a size: its image, its memory and the view a framebuffer attaches. */
static VkResult
render_stencil_image(
	struct notes_renderer *renderer,
	uint32_t width,
	uint32_t height,
	VkImage *stencil,
	VkDeviceMemory *memory,
	VkImageView *stencil_view)
{
	VkImageCreateInfo image;
	VkMemoryRequirements requirements;
	VkImageViewCreateInfo view;
	VkResult error;

	/* The image, used only as the pass's attachment. */
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = renderer->stencil_format;
	image.extent.width = width;
	image.extent.height = height;
	image.extent.depth = 1U;
	image.mipLevels = 1U;
	image.arrayLayers = 1U;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	renderer->operation = "vkCreateImage";
	error = vkCreateImage(renderer->device, &image, NULL, stencil);
	if (error != VK_SUCCESS)
		return error;

	/* Its memory, on the device when it can be. */
	vkGetImageMemoryRequirements(renderer->device, *stencil, &requirements);
	error = render_memory(renderer, &requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory);
	if (error != VK_SUCCESS)
		return error;

	/* Binds the memory to the image. */
	renderer->operation = "vkBindImageMemory";
	error = vkBindImageMemory(renderer->device, *stencil, *memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* The view the framebuffers attach. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = *stencil;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = renderer->stencil_format;
	view.subresourceRange.aspectMask = renderer->stencil_aspect;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	renderer->operation = "vkCreateImageView";
	error = vkCreateImageView(renderer->device, &view, NULL, stencil_view);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the stencil buffer. */
	return VK_SUCCESS;
}

/* Releases the stencil buffer. */
static void
render_stencil_free(
	struct notes_renderer *renderer)
{
	/* The view, the image and the memory, where made. */
	if (renderer->stencil_view != VK_NULL_HANDLE)
		vkDestroyImageView(renderer->device, renderer->stencil_view, NULL);
	if (renderer->stencil != VK_NULL_HANDLE)
		vkDestroyImage(renderer->device, renderer->stencil, NULL);
	if (renderer->stencil_memory != VK_NULL_HANDLE)
		vkFreeMemory(renderer->device, renderer->stencil_memory, NULL);
	renderer->stencil_view = VK_NULL_HANDLE;
	renderer->stencil = VK_NULL_HANDLE;
	renderer->stencil_memory = VK_NULL_HANDLE;
}

/* Makes the pass: the colour attachment cleared to the desk, the stencil cleared to zero. */
static VkResult
render_pass(
	struct notes_renderer *renderer)
{
	VkAttachmentDescription attachments[2];
	VkAttachmentReference color;
	VkAttachmentReference stencil;
	VkSubpassDescription subpass;
	VkSubpassDependency dependency;
	VkRenderPassCreateInfo create;
	VkResult error;

	/* The swapchain image, cleared and stored for presenting. */
	memset(attachments, 0, sizeof(attachments));
	attachments[0].format = renderer->format;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	/* The stencil, cleared every frame and never kept. */
	attachments[1].format = renderer->stencil_format;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	/* The single subpass that draws into both. */
	color.attachment = 0U;
	color.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	stencil.attachment = 1U;
	stencil.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1U;
	subpass.pColorAttachments = &color;
	subpass.pDepthStencilAttachment = &stencil;

	/* The acquired image's transition waits for the acquire, and the stencil is not cleared under the last frame. */
	memset(&dependency, 0, sizeof(dependency));
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0U;
	dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

	/* The pass. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	create.attachmentCount = 2U;
	create.pAttachments = attachments;
	create.subpassCount = 1U;
	create.pSubpasses = &subpass;
	create.dependencyCount = 1U;
	create.pDependencies = &dependency;
	renderer->operation = "vkCreateRenderPass";
	error = vkCreateRenderPass(renderer->device, &create, NULL, &renderer->pass);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the pass. */
	return VK_SUCCESS;
}

/*
 * Makes a pass that draws into the page's picture: from a cleared picture
 * (RENDER_PAGE_CLEAR), or from the picture the last frame left
 * (RENDER_PAGE_LOAD); or into the fill's band from a cleared picture
 * (RENDER_PAGE_BAND, ws216-p005).
 *
 * Its attachments have the formats of the window's pass, so the pipelines
 * made for that pass draw in this one too.  It leaves the page's picture
 * ready to be sampled by the frame that follows, and the band ready to be
 * copied to the host's buffer.
 */
static VkResult
render_page_pass(
	struct notes_renderer *renderer,
	unsigned kind,
	VkRenderPass *pass)
{
	VkAttachmentDescription attachments[2];
	VkAttachmentReference color;
	VkAttachmentReference stencil;
	VkSubpassDescription subpass;
	VkSubpassDependency dependencies[2];
	VkRenderPassCreateInfo create;
	VkResult error;

	/* The picture, cleared or kept, and left for sampling. */
	memset(attachments, 0, sizeof(attachments));
	attachments[0].format = renderer->format;
	attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

	/* A picture added to keeps what it holds, in the layout the last pass left it. */
	if (kind == RENDER_PAGE_LOAD) {
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		attachments[0].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	/* The window's stencil buffer, cleared for the pass and not kept. */
	attachments[1].format = renderer->stencil_format;
	attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
	attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	/* The single subpass that draws into both. */
	color.attachment = 0U;
	color.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	stencil.attachment = 1U;
	stencil.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1U;
	subpass.pColorAttachments = &color;
	subpass.pDepthStencilAttachment = &stencil;

	/*
	 * Before the pass: the last frame's sampling of the picture and its use
	 * of the stencil are done.  After it: the frame samples what it drew.
	 */
	memset(dependencies, 0, sizeof(dependencies));
	dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[0].dstSubpass = 0U;
	dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
	    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dependencies[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
	    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dependencies[1].srcSubpass = 0U;
	dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
	dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

	/*
	 * The band is left as drawn, for the copy to the host's buffer that
	 * follows its pass (review-1 S-1: the Raspberry Pi 4's driver ends a
	 * pass in a colour, general or sampled layout only; a barrier moves it
	 * to the copy's layout).  Its writes are done before the copy reads.
	 */
	if (kind == RENDER_PAGE_BAND) {
		attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	}

	/* The pass. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	create.attachmentCount = 2U;
	create.pAttachments = attachments;
	create.subpassCount = 1U;
	create.pSubpasses = &subpass;
	create.dependencyCount = 2U;
	create.pDependencies = dependencies;
	renderer->operation = "vkCreateRenderPass";
	error = vkCreateRenderPass(renderer->device, &create, NULL, pass);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the pass. */
	return VK_SUCCESS;
}

/* Makes the page's picture, its stencil buffer and framebuffer at a size, and names it in the page's set. */
static VkResult
render_page_make(
	struct notes_renderer *renderer,
	uint32_t width,
	uint32_t height)
{
	VkImageCreateInfo image;
	VkMemoryRequirements requirements;
	VkImageViewCreateInfo view;
	VkFramebufferCreateInfo framebuffer;
	VkImageView attachments[2];
	VkDescriptorImageInfo image_info;
	VkWriteDescriptorSet write;
	VkResult error;

	/* The image: drawn into by the page's passes and sampled by the frame. */
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = renderer->format;
	image.extent.width = width;
	image.extent.height = height;
	image.extent.depth = 1U;
	image.mipLevels = 1U;
	image.arrayLayers = 1U;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	renderer->operation = "vkCreateImage";
	error = vkCreateImage(renderer->device, &image, NULL, &renderer->page);
	if (error != VK_SUCCESS)
		return error;

	/* Its memory, on the device when it can be. */
	vkGetImageMemoryRequirements(renderer->device, renderer->page, &requirements);
	error = render_memory(renderer, &requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &renderer->page_memory);
	if (error != VK_SUCCESS)
		return error;

	/* Binds the memory to the image. */
	renderer->operation = "vkBindImageMemory";
	error = vkBindImageMemory(renderer->device, renderer->page, renderer->page_memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* The view the framebuffer attaches and the shader samples. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = renderer->page;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = renderer->format;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	renderer->operation = "vkCreateImageView";
	error = vkCreateImageView(renderer->device, &view, NULL, &renderer->page_view);
	if (error != VK_SUCCESS)
		return error;

	/* The picture's own stencil buffer, of its size. */
	error = render_stencil_image(renderer, width, height, &renderer->page_stencil, &renderer->page_stencil_memory,
				     &renderer->page_stencil_view);
	if (error != VK_SUCCESS)
		return error;

	/* The framebuffer over the picture and its stencil buffer. */
	attachments[0] = renderer->page_view;
	attachments[1] = renderer->page_stencil_view;
	memset(&framebuffer, 0, sizeof(framebuffer));
	framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer.renderPass = renderer->page_clear_pass;
	framebuffer.attachmentCount = 2U;
	framebuffer.pAttachments = attachments;
	framebuffer.width = width;
	framebuffer.height = height;
	framebuffer.layers = 1U;
	renderer->operation = "vkCreateFramebuffer";
	error = vkCreateFramebuffer(renderer->device, &framebuffer, NULL, &renderer->page_framebuffer);
	if (error != VK_SUCCESS)
		return error;

	/* The page's set names the picture in the layout its passes leave it in. */
	memset(&image_info, 0, sizeof(image_info));
	image_info.sampler = renderer->sampler;
	image_info.imageView = renderer->page_view;
	image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = renderer->sets[NOTES_TEXTURE_PAGE];
	write.dstBinding = 0U;
	write.descriptorCount = 1U;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image_info;
	vkUpdateDescriptorSets(renderer->device, 1U, &write, 0U, NULL);

	/* Succeeded: the picture and what draws into it and samples it are made. */
	return VK_SUCCESS;
}


/* Releases the page's picture; the next frame makes it again. */
static void
render_page_free(
	struct notes_renderer *renderer)
{
	/* The framebuffer, the view, the image and the memory, where made. */
	if (renderer->page_framebuffer != VK_NULL_HANDLE)
		vkDestroyFramebuffer(renderer->device, renderer->page_framebuffer, NULL);
	if (renderer->page_view != VK_NULL_HANDLE)
		vkDestroyImageView(renderer->device, renderer->page_view, NULL);
	if (renderer->page != VK_NULL_HANDLE)
		vkDestroyImage(renderer->device, renderer->page, NULL);
	if (renderer->page_memory != VK_NULL_HANDLE)
		vkFreeMemory(renderer->device, renderer->page_memory, NULL);

	/* Its stencil buffer. */
	if (renderer->page_stencil_view != VK_NULL_HANDLE)
		vkDestroyImageView(renderer->device, renderer->page_stencil_view, NULL);
	if (renderer->page_stencil != VK_NULL_HANDLE)
		vkDestroyImage(renderer->device, renderer->page_stencil, NULL);
	if (renderer->page_stencil_memory != VK_NULL_HANDLE)
		vkFreeMemory(renderer->device, renderer->page_stencil_memory, NULL);
	renderer->page_stencil_view = VK_NULL_HANDLE;
	renderer->page_stencil = VK_NULL_HANDLE;
	renderer->page_stencil_memory = VK_NULL_HANDLE;

	/* Nothing is left. */
	renderer->page_framebuffer = VK_NULL_HANDLE;
	renderer->page_view = VK_NULL_HANDLE;
	renderer->page = VK_NULL_HANDLE;
	renderer->page_memory = VK_NULL_HANDLE;
	renderer->page_width = 0;
	renderer->page_height = 0;
}

/*
 * Makes the fill's band at a size: its pass, a picture of the window's
 * format that the pass draws into and the host's buffer is copied from,
 * its stencil buffer, the framebuffer, and the host-visible buffer,
 * mapped.
 */
static VkResult
render_band_make(
	struct notes_renderer *renderer,
	uint32_t width,
	uint32_t height)
{
	VkImageCreateInfo image;
	VkMemoryRequirements requirements;
	VkImageViewCreateInfo view;
	VkFramebufferCreateInfo framebuffer;
	VkBufferCreateInfo buffer;
	VkImageView attachments[2];
	VkResult error;

	/* The pass, which leaves the band to be copied. */
	error = render_page_pass(renderer, RENDER_PAGE_BAND, &renderer->band_pass);
	if (error != VK_SUCCESS)
		return error;

	/* The picture: drawn into by the pass and copied from (the page's picture is not, L3). */
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = renderer->format;
	image.extent.width = width;
	image.extent.height = height;
	image.extent.depth = 1U;
	image.mipLevels = 1U;
	image.arrayLayers = 1U;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	renderer->operation = "vkCreateImage";
	error = vkCreateImage(renderer->device, &image, NULL, &renderer->band);
	if (error != VK_SUCCESS)
		return error;

	/* Its memory, on the device when it can be. */
	vkGetImageMemoryRequirements(renderer->device, renderer->band, &requirements);
	error = render_memory(renderer, &requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &renderer->band_memory);
	if (error != VK_SUCCESS)
		return error;

	/* Binds the memory to the picture. */
	renderer->operation = "vkBindImageMemory";
	error = vkBindImageMemory(renderer->device, renderer->band, renderer->band_memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* The view the framebuffer attaches. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = renderer->band;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = renderer->format;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	renderer->operation = "vkCreateImageView";
	error = vkCreateImageView(renderer->device, &view, NULL, &renderer->band_view);
	if (error != VK_SUCCESS)
		return error;

	/* The band's own stencil buffer. */
	error = render_stencil_image(renderer, width, height, &renderer->band_stencil, &renderer->band_stencil_memory,
				     &renderer->band_stencil_view);
	if (error != VK_SUCCESS)
		return error;

	/* The framebuffer over the picture and its stencil buffer. */
	attachments[0] = renderer->band_view;
	attachments[1] = renderer->band_stencil_view;
	memset(&framebuffer, 0, sizeof(framebuffer));
	framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer.renderPass = renderer->band_pass;
	framebuffer.attachmentCount = 2U;
	framebuffer.pAttachments = attachments;
	framebuffer.width = width;
	framebuffer.height = height;
	framebuffer.layers = 1U;
	renderer->operation = "vkCreateFramebuffer";
	error = vkCreateFramebuffer(renderer->device, &framebuffer, NULL, &renderer->band_framebuffer);
	if (error != VK_SUCCESS)
		return error;

	/* The host's buffer the picture is copied to. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.size = (VkDeviceSize)width * height * 4U;
	buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	renderer->operation = "vkCreateBuffer";
	error = vkCreateBuffer(renderer->device, &buffer, NULL, &renderer->band_buffer);
	if (error != VK_SUCCESS)
		return error;

	/* Its memory, which the host sees. */
	vkGetBufferMemoryRequirements(renderer->device, renderer->band_buffer, &requirements);
	error = render_memory(renderer, &requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			      &renderer->band_buffer_memory);
	if (error != VK_SUCCESS)
		return error;

	/* Binds the memory to the buffer. */
	renderer->operation = "vkBindBufferMemory";
	error = vkBindBufferMemory(renderer->device, renderer->band_buffer, renderer->band_buffer_memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Maps it for the host's reads. */
	renderer->operation = "vkMapMemory";
	error = vkMapMemory(renderer->device, renderer->band_buffer_memory, 0U, VK_WHOLE_SIZE, 0U, &renderer->band_map);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the band can be drawn and read. */
	return VK_SUCCESS;
}

/*
 * Records the band: the pictures the host wrote made ready, the frame
 * drawn into the cleared band, the band copied to the host's buffer, and
 * the copy made visible to the host.
 */
static VkResult
render_band_record(
	struct notes_renderer *renderer,
	const struct notes_frame *frame)
{
	VkCommandBufferBeginInfo begin;
	VkRenderPassBeginInfo pass;
	VkClearValue clear[2];
	VkBufferImageCopy copy;
	VkImageMemoryBarrier layout;
	VkBufferMemoryBarrier barrier;
	VkExtent2D extent;
	VkResult error;

	/* The command buffer starts afresh, for one submission. */
	renderer->operation = "vkResetCommandBuffer";
	error = vkResetCommandBuffer(renderer->command, 0U);
	if (error != VK_SUCCESS)
		return error;
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	renderer->operation = "vkBeginCommandBuffer";
	error = vkBeginCommandBuffer(renderer->command, &begin);
	if (error != VK_SUCCESS)
		return error;

	/* The pictures the host wrote, ready to be sampled. */
	render_pictures_ready(renderer);

	/* The band, cleared to white (the paper) and the stencil to zero. */
	memset(clear, 0, sizeof(clear));
	clear[0].color.float32[0] = 1.0f;
	clear[0].color.float32[1] = 1.0f;
	clear[0].color.float32[2] = 1.0f;
	clear[0].color.float32[3] = 1.0f;
	clear[1].depthStencil.depth = 1.0f;
	clear[1].depthStencil.stencil = 0U;
	extent.width = renderer->band_width;
	extent.height = renderer->band_height;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = renderer->band_pass;
	pass.framebuffer = renderer->band_framebuffer;
	pass.renderArea.extent = extent;
	pass.clearValueCount = 2U;
	pass.pClearValues = clear;
	vkCmdBeginRenderPass(renderer->command, &pass, VK_SUBPASS_CONTENTS_INLINE);

	/* The frame's draws, with the band's size. */
	render_draws(renderer, frame, 0U, extent);
	vkCmdEndRenderPass(renderer->command);

	/* The band, drawn, moves to the layout the copy reads. */
	memset(&layout, 0, sizeof(layout));
	layout.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	layout.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	layout.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	layout.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	layout.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	layout.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	layout.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	layout.image = renderer->band;
	layout.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	layout.subresourceRange.levelCount = 1U;
	layout.subresourceRange.layerCount = 1U;
	vkCmdPipelineBarrier(renderer->command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &layout);

	/* The band's pixels into the host's buffer, row after row. */
	memset(&copy, 0, sizeof(copy));
	copy.bufferOffset = 0U;
	copy.bufferRowLength = 0U;
	copy.bufferImageHeight = 0U;
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.mipLevel = 0U;
	copy.imageSubresource.baseArrayLayer = 0U;
	copy.imageSubresource.layerCount = 1U;
	copy.imageExtent.width = extent.width;
	copy.imageExtent.height = extent.height;
	copy.imageExtent.depth = 1U;
	vkCmdCopyImageToBuffer(renderer->command, renderer->band, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, renderer->band_buffer, 1U, &copy);

	/* The copy's writes are seen by the host's reads. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer = renderer->band_buffer;
	barrier.offset = 0U;
	barrier.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(renderer->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0U, 0U, NULL, 1U, &barrier, 0U, NULL);

	/* Closes the recording. */
	renderer->operation = "vkEndCommandBuffer";
	error = vkEndCommandBuffer(renderer->command);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the band's commands. */
	return VK_SUCCESS;
}

/* Makes a view, a framebuffer and a present semaphore for each swapchain image. */
static VkResult
render_targets(
	struct notes_renderer *renderer)
{
	VkImage images[8];
	VkImageView attachments[2];
	VkImageViewCreateInfo view;
	VkFramebufferCreateInfo framebuffer;
	VkSemaphoreCreateInfo semaphore;
	uint32_t index;
	VkResult error;

	/* The swapchain's images (at most eight). */
	renderer->count = 8U;
	renderer->operation = "vkGetSwapchainImagesKHR";
	error = vkGetSwapchainImagesKHR(renderer->device, renderer->swapchain, &renderer->count, images);
	if (error != VK_SUCCESS && error != VK_INCOMPLETE)
		return error;

	/* A zeroed table, so that a failure part-way leaves only made objects to release. */
	renderer->targets = calloc(renderer->count, sizeof(renderer->targets[0]));
	if (renderer->targets == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;

	/* Each image's objects. */
	for (index = 0U; index < renderer->count; index++) {
		/* The view of the image. */
		renderer->targets[index].image = images[index];
		memset(&view, 0, sizeof(view));
		view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		view.image = images[index];
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = renderer->format;
		view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		view.subresourceRange.levelCount = 1U;
		view.subresourceRange.layerCount = 1U;
		renderer->operation = "vkCreateImageView";
		error = vkCreateImageView(renderer->device, &view, NULL, &renderer->targets[index].view);
		if (error != VK_SUCCESS)
			return error;

		/* The framebuffer over it and the shared stencil buffer. */
		attachments[0] = renderer->targets[index].view;
		attachments[1] = renderer->stencil_view;
		memset(&framebuffer, 0, sizeof(framebuffer));
		framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		framebuffer.renderPass = renderer->pass;
		framebuffer.attachmentCount = 2U;
		framebuffer.pAttachments = attachments;
		framebuffer.width = renderer->extent.width;
		framebuffer.height = renderer->extent.height;
		framebuffer.layers = 1U;
		renderer->operation = "vkCreateFramebuffer";
		error = vkCreateFramebuffer(renderer->device, &framebuffer, NULL, &renderer->targets[index].framebuffer);
		if (error != VK_SUCCESS)
			return error;

		/* The semaphore its present waits for. */
		memset(&semaphore, 0, sizeof(semaphore));
		semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		renderer->operation = "vkCreateSemaphore";
		error = vkCreateSemaphore(renderer->device, &semaphore, NULL, &renderer->targets[index].rendered);
		if (error != VK_SUCCESS)
			return error;
	}

	/* Succeeded: every image can be drawn into. */
	return VK_SUCCESS;
}

/* Releases the objects of the swapchain's images (not the images, which are the swapchain's). */
static void
render_targets_free(
	struct notes_renderer *renderer)
{
	uint32_t index;

	/* Each image's semaphore, framebuffer and view, where made. */
	for (index = 0U; renderer->targets != NULL && index < renderer->count; index++) {
		if (renderer->targets[index].rendered != VK_NULL_HANDLE)
			vkDestroySemaphore(renderer->device, renderer->targets[index].rendered, NULL);
		if (renderer->targets[index].framebuffer != VK_NULL_HANDLE)
			vkDestroyFramebuffer(renderer->device, renderer->targets[index].framebuffer, NULL);
		if (renderer->targets[index].view != VK_NULL_HANDLE)
			vkDestroyImageView(renderer->device, renderer->targets[index].view, NULL);
	}

	/* The table itself. */
	free(renderer->targets);
	renderer->targets = NULL;
	renderer->count = 0U;
}

/* Makes the command pool and buffer, the frame's fence and the acquire semaphore. */
static VkResult
render_commands(
	struct notes_renderer *renderer)
{
	VkCommandPoolCreateInfo pool;
	VkCommandBufferAllocateInfo command;
	VkFenceCreateInfo fence;
	VkSemaphoreCreateInfo semaphore;
	VkResult error;

	/* A pool whose one buffer is reset every frame. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = renderer->family;
	renderer->operation = "vkCreateCommandPool";
	error = vkCreateCommandPool(renderer->device, &pool, NULL, &renderer->pool);
	if (error != VK_SUCCESS)
		return error;

	/* The buffer. */
	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = renderer->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = 1U;
	renderer->operation = "vkAllocateCommandBuffers";
	error = vkAllocateCommandBuffers(renderer->device, &command, &renderer->command);
	if (error != VK_SUCCESS)
		return error;

	/* The fence the frame's end signals. */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	renderer->operation = "vkCreateFence";
	error = vkCreateFence(renderer->device, &fence, NULL, &renderer->fence);
	if (error != VK_SUCCESS)
		return error;

	/* The semaphore the acquire signals. */
	memset(&semaphore, 0, sizeof(semaphore));
	semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	renderer->operation = "vkCreateSemaphore";
	error = vkCreateSemaphore(renderer->device, &semaphore, NULL, &renderer->acquired);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: one frame at a time can be recorded and waited for. */
	return VK_SUCCESS;
}

/* Allocates memory that meets the requirements, with the wanted properties when a type has them. */
static VkResult
render_memory(
	struct notes_renderer *renderer,
	const VkMemoryRequirements *requirements,
	VkMemoryPropertyFlags wanted,
	VkDeviceMemory *memory)
{
	VkMemoryAllocateInfo allocate;
	uint32_t chosen;
	uint32_t fallback;
	uint32_t index;
	VkResult error;

	/* The first allowed type with the properties, and the first allowed type at all. */
	chosen = UINT32_MAX;
	fallback = UINT32_MAX;
	for (index = 0U; index < renderer->memory.memoryTypeCount; index++) {
		/* A type the resource does not allow is passed. */
		if ((requirements->memoryTypeBits & (1U << index)) == 0U)
			continue;
		if (fallback == UINT32_MAX)
			fallback = index;

		/* The first with the wanted properties is chosen. */
		if ((renderer->memory.memoryTypes[index].propertyFlags & wanted) == wanted) {
			chosen = index;
			break;
		}
	}

	/* Host-visible memory must be host-visible; other wishes fall back to any allowed type. */
	if (chosen == UINT32_MAX && (wanted & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0U)
		chosen = fallback;
	renderer->operation = "memory type";
	if (chosen == UINT32_MAX)
		return VK_ERROR_FEATURE_NOT_PRESENT;

	/* The allocation. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements->size;
	allocate.memoryTypeIndex = chosen;
	renderer->operation = "vkAllocateMemory";
	error = vkAllocateMemory(renderer->device, &allocate, NULL, memory);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the memory. */
	return VK_SUCCESS;
}

/* Makes the sampler, the descriptor set layout, the pool and a set for each picture. */
static VkResult
render_descriptors(
	struct notes_renderer *renderer)
{
	VkSamplerCreateInfo sampler;
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo set_layout;
	VkDescriptorPoolSize pool_size;
	VkDescriptorPoolCreateInfo pool;
	VkDescriptorSetAllocateInfo allocate;
	VkDescriptorSetLayout layouts[NOTES_TEXTURES];
	unsigned texture;
	VkResult error;

	/* Nearest sampling: the pictures' pixels land on the window's pixels one to one. */
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_NEAREST;
	sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	renderer->operation = "vkCreateSampler";
	error = vkCreateSampler(renderer->device, &sampler, NULL, &renderer->sampler);
	if (error != VK_SUCCESS)
		return error;

	/*
	 * Linear sampling for the background (ws216-p005, review-1 M-5): the
	 * same one to one on the screen, and blended when the fill's band draws
	 * the page larger than the picture it was drawn for.
	 */
	sampler.magFilter = VK_FILTER_LINEAR;
	sampler.minFilter = VK_FILTER_LINEAR;
	error = vkCreateSampler(renderer->device, &sampler, NULL, &renderer->linear_sampler);
	if (error != VK_SUCCESS)
		return error;

	/* One combined image sampler for the fragment shader. */
	memset(&binding, 0, sizeof(binding));
	binding.binding = 0U;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1U;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	memset(&set_layout, 0, sizeof(set_layout));
	set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout.bindingCount = 1U;
	set_layout.pBindings = &binding;
	renderer->operation = "vkCreateDescriptorSetLayout";
	error = vkCreateDescriptorSetLayout(renderer->device, &set_layout, NULL, &renderer->set_layout);
	if (error != VK_SUCCESS)
		return error;

	/* A pool for a set of each picture. */
	memset(&pool_size, 0, sizeof(pool_size));
	pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	pool_size.descriptorCount = NOTES_TEXTURES;
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.maxSets = NOTES_TEXTURES;
	pool.poolSizeCount = 1U;
	pool.pPoolSizes = &pool_size;
	renderer->operation = "vkCreateDescriptorPool";
	error = vkCreateDescriptorPool(renderer->device, &pool, NULL, &renderer->descriptor_pool);
	if (error != VK_SUCCESS)
		return error;

	/* The sets, all of the one layout (the grain's sizes too, filled when its group is made). */
	for (texture = 0; texture < NOTES_TEXTURES; texture++)
		layouts[texture] = renderer->set_layout;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = renderer->descriptor_pool;
	allocate.descriptorSetCount = NOTES_TEXTURES;
	allocate.pSetLayouts = layouts;
	renderer->operation = "vkAllocateDescriptorSets";
	error = vkAllocateDescriptorSets(renderer->device, &allocate, renderer->sets);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: each set is filled when its picture is made. */
	return VK_SUCCESS;
}

/* Makes the toolbar's image (linear, host-written) at the swapchain's width, and names it in the set. */
static VkResult
render_toolbar(
	struct notes_renderer *renderer)
{
	VkResult error;

	/* The image across the window, as high as the toolbar's band and the notice under it. */
	error = render_linear(renderer,
			      renderer->extent.width,
			      NOTES_TOOLBAR_IMAGE_HEIGHT,
			      renderer->sets[NOTES_TEXTURE_TOOLBAR],
			      renderer->sampler,
			      &renderer->toolbar,
			      &renderer->toolbar_memory,
			      &renderer->toolbar_view,
			      &renderer->toolbar_pixels,
			      &renderer->toolbar_pitch);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the first frame moves the image to the general layout. */
	renderer->toolbar_ready = 0;
	return VK_SUCCESS;
}

/*
 * Makes a picture the host draws: a linear B8G8R8A8 image of a size in
 * host-visible memory, mapped for as long as it lives, with the view the
 * shader samples, and names it in a set with a sampler, in the general
 * layout it is kept in.  Gives the mapped rows and their pitch.
 */
static VkResult
render_linear(
	struct notes_renderer *renderer,
	uint32_t width,
	uint32_t height,
	VkDescriptorSet set,
	VkSampler sampler,
	VkImage *made,
	VkDeviceMemory *memory,
	VkImageView *made_view,
	unsigned char **pixels,
	size_t *pitch)
{
	VkImageCreateInfo image;
	VkMemoryRequirements requirements;
	VkImageSubresource subresource;
	VkSubresourceLayout row_layout;
	VkImageViewCreateInfo view;
	VkDescriptorImageInfo image_info;
	VkWriteDescriptorSet write;
	void *map;
	VkResult error;

	/* The image: linear so that the host writes its rows, sampled by the fragment shader. */
	memset(&image, 0, sizeof(image));
	image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = VK_FORMAT_B8G8R8A8_UNORM;
	image.extent.width = width;
	image.extent.height = height;
	image.extent.depth = 1U;
	image.mipLevels = 1U;
	image.arrayLayers = 1U;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_LINEAR;
	image.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
	renderer->operation = "vkCreateImage";
	error = vkCreateImage(renderer->device, &image, NULL, made);
	if (error != VK_SUCCESS)
		return error;

	/* Its memory, which the host sees. */
	vkGetImageMemoryRequirements(renderer->device, *made, &requirements);
	error = render_memory(renderer, &requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memory);
	if (error != VK_SUCCESS)
		return error;

	/* Binds the memory to the image. */
	renderer->operation = "vkBindImageMemory";
	error = vkBindImageMemory(renderer->device, *made, *memory, 0U);
	if (error != VK_SUCCESS)
		return error;

	/* Maps it for the host's writes, for as long as the image lives. */
	renderer->operation = "vkMapMemory";
	error = vkMapMemory(renderer->device, *memory, 0U, VK_WHOLE_SIZE, 0U, &map);
	if (error != VK_SUCCESS)
		return error;

	/* Where its rows start, which the host needs to draw into it. */
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vkGetImageSubresourceLayout(renderer->device, *made, &subresource, &row_layout);
	*pixels = (unsigned char *)map + row_layout.offset;
	*pitch = (size_t)row_layout.rowPitch;

	/* The view the shader samples. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = *made;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_B8G8R8A8_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	renderer->operation = "vkCreateImageView";
	error = vkCreateImageView(renderer->device, &view, NULL, made_view);
	if (error != VK_SUCCESS)
		return error;

	/* The set names the image, with its sampler, in the general layout it is kept in. */
	memset(&image_info, 0, sizeof(image_info));
	image_info.sampler = sampler;
	image_info.imageView = *made_view;
	image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = set;
	write.dstBinding = 0U;
	write.descriptorCount = 1U;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image_info;
	vkUpdateDescriptorSets(renderer->device, 1U, &write, 0U, NULL);

	/* Succeeded: the host may draw into the rows. */
	return VK_SUCCESS;
}

/* Releases the background's image. */
static void
render_background_free(
	struct notes_renderer *renderer)
{
	/* The view, the image and the memory (which unmaps it), where made. */
	if (renderer->background_view != VK_NULL_HANDLE)
		vkDestroyImageView(renderer->device, renderer->background_view, NULL);
	if (renderer->background != VK_NULL_HANDLE)
		vkDestroyImage(renderer->device, renderer->background, NULL);
	if (renderer->background_memory != VK_NULL_HANDLE)
		vkFreeMemory(renderer->device, renderer->background_memory, NULL);
	renderer->background_view = VK_NULL_HANDLE;
	renderer->background = VK_NULL_HANDLE;
	renderer->background_memory = VK_NULL_HANDLE;
	renderer->background_pixels = NULL;
	renderer->background_pitch = 0;
	renderer->background_width = 0;
	renderer->background_height = 0;
}

/* Releases the overlay's image. */
static void
render_overlay_free(
	struct notes_renderer *renderer)
{
	/* The view, the image and the memory (which unmaps it), where made. */
	if (renderer->overlay_view != VK_NULL_HANDLE)
		vkDestroyImageView(renderer->device, renderer->overlay_view, NULL);
	if (renderer->overlay != VK_NULL_HANDLE)
		vkDestroyImage(renderer->device, renderer->overlay, NULL);
	if (renderer->overlay_memory != VK_NULL_HANDLE)
		vkFreeMemory(renderer->device, renderer->overlay_memory, NULL);
	renderer->overlay_view = VK_NULL_HANDLE;
	renderer->overlay = VK_NULL_HANDLE;
	renderer->overlay_memory = VK_NULL_HANDLE;
	renderer->overlay_pixels = NULL;
	renderer->overlay_pitch = 0;
	renderer->overlay_width = 0;
	renderer->overlay_height = 0;
}

/* Releases the toolbar's image. */
static void
render_toolbar_free(
	struct notes_renderer *renderer)
{
	/* The view, the image and the memory (which unmaps it), where made. */
	if (renderer->toolbar_view != VK_NULL_HANDLE)
		vkDestroyImageView(renderer->device, renderer->toolbar_view, NULL);
	if (renderer->toolbar != VK_NULL_HANDLE)
		vkDestroyImage(renderer->device, renderer->toolbar, NULL);
	if (renderer->toolbar_memory != VK_NULL_HANDLE)
		vkFreeMemory(renderer->device, renderer->toolbar_memory, NULL);
	renderer->toolbar_view = VK_NULL_HANDLE;
	renderer->toolbar = VK_NULL_HANDLE;
	renderer->toolbar_memory = VK_NULL_HANDLE;
	renderer->toolbar_pixels = NULL;
	renderer->toolbar_pitch = 0;
}

/* Sets the chunks' size: the most whole triangles of vertices that fit a number of bytes. */
static void
render_chunk_size(
	struct notes_renderer *renderer,
	size_t bytes)
{
	size_t vertices;

	/* Whole triangles only, so that a chunk's edge is always a triangle's. */
	vertices = bytes / RENDER_VERTEX_BYTES;
	vertices -= vertices % 3U;

	/* The size every chunk is made at from now on. */
	renderer->chunk_bytes = bytes;
	renderer->chunk_vertices = vertices;
}

/*
 * Makes the chunks a number of vertices needs, keeping the chunks made
 * already.  A device that refuses a chunk for memory (a smaller allocation
 * may still be given) gets chunks of half the size, every chunk made
 * again, down to NOTES_CHUNK_BYTES_MIN; a refusal of that size, or of
 * another kind, is reported.
 */
static VkResult
render_chunks(
	struct notes_renderer *renderer,
	size_t vertices)
{
	VkResult error;

	/* Chunks enough, smaller each time the device refuses one. */
	for (;;) {
		error = render_chunks_add(renderer, vertices);
		if (error == VK_SUCCESS)
			break;

		/* Only a refusal for memory is met with smaller chunks. */
		if (error != VK_ERROR_OUT_OF_DEVICE_MEMORY && error != VK_ERROR_OUT_OF_HOST_MEMORY)
			return error;

		/* The refusal is logged for the tests and the measurements. */
		printf("NOTES FRAME chunk refused bytes=%lu error=%d\n", (unsigned long)renderer->chunk_bytes, (int)error);
		fflush(stdout);

		/* A refusal of the smallest chunks fails the frame, as one buffer too large did before. */
		if (renderer->chunk_bytes / 2U < NOTES_CHUNK_BYTES_MIN)
			return error;

		/* Every chunk goes once nothing uses them, and they are made again at half the size. */
		renderer->operation = "vkDeviceWaitIdle";
		error = vkDeviceWaitIdle(renderer->device);
		if (error != VK_SUCCESS)
			return error;
		render_chunks_free(renderer);
		render_chunk_size(renderer, renderer->chunk_bytes / 2U);
	}

	/* Succeeded: the chunks hold every vertex. */
	return VK_SUCCESS;
}

/* Adds the chunks a number of vertices needs beyond those made (at least one in all). */
static VkResult
render_chunks_add(
	struct notes_renderer *renderer,
	size_t vertices)
{
	struct notes_chunk *larger;
	size_t needed;
	VkResult error;

	/* The chunks the vertices fill, and one for none at all. */
	needed = (vertices + renderer->chunk_vertices - 1U) / renderer->chunk_vertices;
	if (needed == 0U)
		needed = 1U;

	/* The chunks made already may be enough. */
	if (needed <= renderer->chunk_count)
		return VK_SUCCESS;

	/* A table of the chunks, larger. */
	larger = realloc(renderer->chunks, needed * sizeof(*larger));
	renderer->operation = "chunk table";
	if (larger == NULL)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	renderer->chunks = larger;

	/* Each chunk more; one that fails leaves the ones before it made. */
	while (renderer->chunk_count < needed) {
		error = render_chunk(renderer, &renderer->chunks[renderer->chunk_count]);
		if (error != VK_SUCCESS)
			return error;
		renderer->chunk_count++;
	}

	/* Succeeded: chunks enough. */
	return VK_SUCCESS;
}

/* Makes one chunk at the chunks' size: a host-visible vertex buffer, mapped for good. */
static VkResult
render_chunk(
	struct notes_renderer *renderer,
	struct notes_chunk *chunk)
{
	VkBufferCreateInfo buffer;
	VkMemoryRequirements requirements;
	VkResult error;

	/* Nothing is made yet. */
	memset(chunk, 0, sizeof(*chunk));

	/* The buffer, of whole triangles. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.size = (VkDeviceSize)(renderer->chunk_vertices * RENDER_VERTEX_BYTES);
	buffer.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	renderer->operation = "vkCreateBuffer";
	error = vkCreateBuffer(renderer->device, &buffer, NULL, &chunk->buffer);
	if (error != VK_SUCCESS) {
		render_chunk_free(renderer, chunk);
		return error;
	}

	/* Its memory, which the host sees. */
	vkGetBufferMemoryRequirements(renderer->device, chunk->buffer, &requirements);
	error = render_memory(renderer, &requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &chunk->memory);
	if (error != VK_SUCCESS) {
		render_chunk_free(renderer, chunk);
		return error;
	}

	/* Binds the memory to the buffer. */
	renderer->operation = "vkBindBufferMemory";
	error = vkBindBufferMemory(renderer->device, chunk->buffer, chunk->memory, 0U);
	if (error != VK_SUCCESS) {
		render_chunk_free(renderer, chunk);
		return error;
	}

	/* Maps it for the host's writes, for good. */
	renderer->operation = "vkMapMemory";
	error = vkMapMemory(renderer->device, chunk->memory, 0U, VK_WHOLE_SIZE, 0U, &chunk->map);
	if (error != VK_SUCCESS) {
		render_chunk_free(renderer, chunk);
		return error;
	}

	/* Succeeded: the host writes a frame's vertices here. */
	return VK_SUCCESS;
}

/* Releases one chunk's buffer and memory (which unmaps it), where made. */
static void
render_chunk_free(
	struct notes_renderer *renderer,
	struct notes_chunk *chunk)
{
	/* The buffer and the memory. */
	if (chunk->buffer != VK_NULL_HANDLE)
		vkDestroyBuffer(renderer->device, chunk->buffer, NULL);
	if (chunk->memory != VK_NULL_HANDLE)
		vkFreeMemory(renderer->device, chunk->memory, NULL);

	/* Nothing is left. */
	memset(chunk, 0, sizeof(*chunk));
}

/* Releases every chunk and their table. */
static void
render_chunks_free(
	struct notes_renderer *renderer)
{
	size_t index;

	/* Each chunk. */
	for (index = 0; index < renderer->chunk_count; index++)
		render_chunk_free(renderer, &renderer->chunks[index]);

	/* The table. */
	free(renderer->chunks);
	renderer->chunks = NULL;
	renderer->chunk_count = 0;
}

/*
 * Copies vertices into the chunks at a place in the run of the frame's
 * vertices (the chunks must hold it), the part of a chunk's end going on
 * at the start of the next.
 */
static void
render_copy(
	struct notes_renderer *renderer,
	const float *vertices,
	size_t first,
	size_t count)
{
	size_t chunk;
	size_t offset;
	size_t piece;

	/* Each piece that falls in one chunk. */
	while (count > 0U) {
		/* The chunk the place falls in, the place in it and how much of the rest it holds. */
		chunk = first / renderer->chunk_vertices;
		offset = first % renderer->chunk_vertices;
		piece = renderer->chunk_vertices - offset;
		if (piece > count)
			piece = count;

		/* The piece. */
		memcpy((unsigned char *)renderer->chunks[chunk].map + offset * RENDER_VERTEX_BYTES, vertices, piece * RENDER_VERTEX_BYTES);

		/* The rest goes on after it. */
		vertices += piece * NOTES_VERTEX_FLOATS;
		first += piece;
		count -= piece;
	}
}

/*
 * Keeps the most vertices a frame has had, and logs it (for the
 * measurement of a dense page) when it grew by a quarter of the count
 * logged last.
 */
static void
render_peak(
	struct notes_renderer *renderer,
	size_t vertices)
{
	size_t chunks;

	/* Only a new peak is kept. */
	if (vertices <= renderer->vertex_peak)
		return;
	renderer->vertex_peak = vertices;

	/* Logged when it is a quarter more than the last logged (the first always). */
	if (renderer->vertex_logged != 0U && vertices * 4U < renderer->vertex_logged * RENDER_PEAK_QUARTERS)
		return;
	renderer->vertex_logged = vertices;
	chunks = (vertices + renderer->chunk_vertices - 1U) / renderer->chunk_vertices;
	printf("NOTES FRAME vertices=%lu chunks=%lu bytes=%lu\n", (unsigned long)vertices, (unsigned long)chunks, (unsigned long)renderer->chunk_bytes);
	fflush(stdout);
}

/* Makes each group of pipelines a frame's draws use that was never tried. */
static void
render_groups(
	struct notes_renderer *renderer,
	const struct notes_frame *frame)
{
	unsigned group;
	size_t index;

	/* A missing frame uses nothing. */
	if (frame == NULL)
		return;

	/* Each draw's group, when it has one not tried yet. */
	for (index = 0; index < frame->draw_count; index++) {
		group = render_pipe_group(frame->draws[index].pipe);
		if (group >= NOTES_GROUPS)
			continue;
		if (renderer->groups[group] != NOTES_GROUP_UNTRIED)
			continue;
		render_group(renderer, group);
	}
}

/*
 * Makes a group of pipelines (with the grain's pictures for the grain), or
 * when the device refuses any part of it, keeps none of it and draws the
 * group's draws by the fringe and the cover from then on.  Either outcome
 * is logged once.
 */
static void
render_group(
	struct notes_renderer *renderer,
	unsigned group)
{
	const char *name;
	VkResult error;

	/* The group's name in the log. */
	name = "multiply";
	if (group == NOTES_GROUP_GRAIN)
		name = "grain";

	/* The group, whole or not at all. */
	error = render_group_make(renderer, group);
	if (error != VK_SUCCESS) {
		render_group_free(renderer, group);
		renderer->groups[group] = NOTES_GROUP_REFUSED;
		printf("NOTES PIPE fallback name=%s error=%d\n", name, (int)error);
		fflush(stdout);
		return;
	}

	/* Succeeded: the group draws its draws from now on. */
	renderer->groups[group] = NOTES_GROUP_MADE;
	printf("NOTES PIPE made name=%s\n", name);
	fflush(stdout);
}

/* Makes the parts of a group: for the grain its pictures, then the fringe's and the cover's pipelines. */
static VkResult
render_group_make(
	struct notes_renderer *renderer,
	unsigned group)
{
	VkShaderModule vertex;
	VkShaderModule fragment;
	unsigned fringe;
	unsigned cover;
	VkResult error;

	/* The grain's pictures and sampler, and its shaders and pipelines. */
	if (group == NOTES_GROUP_GRAIN) {
		error = render_grain(renderer);
		if (error != VK_SUCCESS)
			return error;
		error = render_module(renderer, notes_grain_vert, sizeof(notes_grain_vert), &vertex);
		if (error != VK_SUCCESS)
			return error;
		error = render_module(renderer, notes_grain_frag, sizeof(notes_grain_frag), &fragment);
		fringe = NOTES_PIPE_FRINGE_GRAIN;
		cover = NOTES_PIPE_COVER_GRAIN;
	} else {
		/* The Multiply's shaders and pipelines. */
		error = render_module(renderer, notes_draw_vert, sizeof(notes_draw_vert), &vertex);
		if (error != VK_SUCCESS)
			return error;
		error = render_module(renderer, notes_multiply_frag, sizeof(notes_multiply_frag), &fragment);
		fringe = NOTES_PIPE_FRINGE_MULTIPLY;
		cover = NOTES_PIPE_COVER_MULTIPLY;
	}

	/* A fragment shader the device refused leaves the vertex shader to release. */
	if (error != VK_SUCCESS) {
		vkDestroyShaderModule(renderer->device, vertex, NULL);
		return error;
	}

	/* The two pipelines, the cover's only after the fringe's. */
	error = render_pipeline(renderer, fringe, vertex, fragment);
	if (error != VK_SUCCESS) {
		vkDestroyShaderModule(renderer->device, vertex, NULL);
		vkDestroyShaderModule(renderer->device, fragment, NULL);
		return error;
	}

	/* The cover's pipeline. */
	error = render_pipeline(renderer, cover, vertex, fragment);

	/* The modules are not needed once the pipelines are made (or refused). */
	vkDestroyShaderModule(renderer->device, vertex, NULL);
	vkDestroyShaderModule(renderer->device, fragment, NULL);

	/* Reports a refused cover (the caller releases the fringe's pipeline with the group). */
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the group's pipelines. */
	return VK_SUCCESS;
}

/* Releases what a group made: its pipelines, and the grain's pictures and sampler. */
static void
render_group_free(
	struct notes_renderer *renderer,
	unsigned group)
{
	struct notes_grain_picture *picture;
	unsigned pipe;
	unsigned level;

	/* The group's pipelines, where made. */
	for (pipe = NOTES_PIPES_FIRST; pipe < NOTES_PIPES; pipe++) {
		if (render_pipe_group(pipe) != group)
			continue;
		if (renderer->pipes[pipe] != VK_NULL_HANDLE)
			vkDestroyPipeline(renderer->device, renderer->pipes[pipe], NULL);
		renderer->pipes[pipe] = VK_NULL_HANDLE;
	}

	/* Only the grain has more. */
	if (group != NOTES_GROUP_GRAIN)
		return;

	/* Each of its pictures: the view, the image and the memory (which unmaps it), where made. */
	for (level = 0; level < NOTES_GRAIN_LEVELS; level++) {
		picture = &renderer->grain[level];
		if (picture->view != VK_NULL_HANDLE)
			vkDestroyImageView(renderer->device, picture->view, NULL);
		if (picture->image != VK_NULL_HANDLE)
			vkDestroyImage(renderer->device, picture->image, NULL);
		if (picture->memory != VK_NULL_HANDLE)
			vkFreeMemory(renderer->device, picture->memory, NULL);
		memset(picture, 0, sizeof(*picture));
	}

	/* Its sampler. */
	if (renderer->grain_sampler != VK_NULL_HANDLE)
		vkDestroySampler(renderer->device, renderer->grain_sampler, NULL);
	renderer->grain_sampler = VK_NULL_HANDLE;
}

/*
 * Makes the grain's sampler, which repeats the tile and blends its texels,
 * and its pictures: the tile (shape.c, the bytes the PDF's grain has) and
 * its two coarser sizes, made once on the CPU.  The tile's hash is logged
 * (the same on every machine: the tile is made with integers only).
 */
static VkResult
render_grain(
	struct notes_renderer *renderer)
{
	VkSamplerCreateInfo sampler;
	unsigned char *tiles[NOTES_GRAIN_LEVELS];
	size_t side;
	size_t index;
	uint32_t hash;
	unsigned level;
	VkResult error;

	/* The sampler: repeated every cell of the page, its texels blended. */
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_LINEAR;
	sampler.minFilter = VK_FILTER_LINEAR;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	renderer->operation = "vkCreateSampler";
	error = vkCreateSampler(renderer->device, &sampler, NULL, &renderer->grain_sampler);
	if (error != VK_SUCCESS)
		return error;

	/* Room for the tile at each size; one that is refused ends the making. */
	memset(tiles, 0, sizeof(tiles));
	for (level = 0; level < NOTES_GRAIN_LEVELS; level++) {
		side = NOTES_GRAIN_SIDE >> level;
		tiles[level] = malloc(side * side);
		if (tiles[level] == NULL) {
			renderer->operation = "grain tile";
			error = VK_ERROR_OUT_OF_HOST_MEMORY;
			goto done;
		}
	}

	/* The tile, and each coarser size from the one before. */
	notes_grain_tile(tiles[0]);
	for (level = 1; level < NOTES_GRAIN_LEVELS; level++)
		notes_grain_level(tiles[level - 1U], NOTES_GRAIN_SIDE >> (level - 1U), tiles[level]);

	/* Each size's picture. */
	for (level = 0; level < NOTES_GRAIN_LEVELS; level++) {
		error = render_grain_picture(renderer, level, tiles[level]);
		if (error != VK_SUCCESS)
			goto done;
	}

	/* The tile's FNV-1a hash, logged for the comparison between machines. */
	hash = RENDER_FNV_START;
	for (index = 0; index < (size_t)NOTES_GRAIN_SIDE * NOTES_GRAIN_SIDE; index++) {
		hash ^= tiles[0][index];
		hash *= RENDER_FNV_PRIME;
	}
	printf("NOTES GRAIN hash=%08x\n", (unsigned)hash);
	fflush(stdout);

done:
	/* The CPU's tiles are not needed once the pictures hold them (or could not be made). */
	for (level = 0; level < NOTES_GRAIN_LEVELS; level++)
		free(tiles[level]);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the grain at every size. */
	return VK_SUCCESS;
}

/* Makes the picture of one size of the grain from its texels: each the same in all four channels. */
static VkResult
render_grain_picture(
	struct notes_renderer *renderer,
	unsigned level,
	const unsigned char *tile)
{
	struct notes_grain_picture *picture;
	unsigned char *pixels;
	unsigned char *row;
	size_t pitch;
	uint32_t side;
	uint32_t x;
	uint32_t y;
	VkResult error;

	/* The linear image, named in the size's set with the grain's sampler. */
	picture = &renderer->grain[level];
	side = NOTES_GRAIN_SIDE >> level;
	error = render_linear(renderer,
			      side,
			      side,
			      renderer->sets[NOTES_TEXTURE_GRAIN + level],
			      renderer->grain_sampler,
			      &picture->image,
			      &picture->memory,
			      &picture->view,
			      &pixels,
			      &pitch);
	if (error != VK_SUCCESS)
		return error;

	/* Each texel's value in blue, green, red and alpha. */
	for (y = 0; y < side; y++) {
		row = pixels + (size_t)y * pitch;
		for (x = 0; x < side; x++) {
			row[x * 4U] = tile[(size_t)y * side + x];
			row[x * 4U + 1U] = tile[(size_t)y * side + x];
			row[x * 4U + 2U] = tile[(size_t)y * side + x];
			row[x * 4U + 3U] = tile[(size_t)y * side + x];
		}
	}

	/* Succeeded: the next frame moves the picture to the general layout. */
	picture->side = side;
	picture->ready = 0;
	return VK_SUCCESS;
}

/* Reports the group a pipeline belongs to, or NOTES_GROUPS for one made with the renderer. */
static unsigned
render_pipe_group(
	unsigned pipe)
{
	/* The grain's and the Multiply's pipelines. */
	switch (pipe) {
	case NOTES_PIPE_FRINGE_GRAIN:
	case NOTES_PIPE_COVER_GRAIN:
		return NOTES_GROUP_GRAIN;
	case NOTES_PIPE_FRINGE_MULTIPLY:
	case NOTES_PIPE_COVER_MULTIPLY:
		return NOTES_GROUP_MULTIPLY;
	default:
		break;
	}

	/* Every other is made with the renderer. */
	return NOTES_GROUPS;
}

/*
 * Reports the pipeline a draw is drawn by: its own, or for a group the
 * device refused (or not tried), the fringe or the cover it stands in for.
 */
static unsigned
render_pipe(
	const struct notes_renderer *renderer,
	unsigned pipe)
{
	unsigned group;

	/* A pipeline made with the renderer, or of a group that was made, draws. */
	group = render_pipe_group(pipe);
	if (group >= NOTES_GROUPS)
		return pipe;
	if (renderer->groups[group] == NOTES_GROUP_MADE)
		return pipe;

	/* Otherwise the fringe stands for a fringe, the cover for a cover. */
	if (pipe == NOTES_PIPE_FRINGE_GRAIN || pipe == NOTES_PIPE_FRINGE_MULTIPLY)
		return NOTES_PIPE_FRINGE;

	/* Reports the cover. */
	return NOTES_PIPE_COVER;
}

/* Makes the pipeline layout and the pipelines made with the renderer. */
static VkResult
render_pipelines(
	struct notes_renderer *renderer)
{
	VkShaderModule vertex;
	VkShaderModule fill;
	VkShaderModule texture;
	VkPushConstantRange push;
	VkPipelineLayoutCreateInfo layout;
	unsigned pipe;
	VkResult error;

	/* The layout: a picture's set, the target's size and the page's place for the grain. */
	memset(&push, 0, sizeof(push));
	push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	push.offset = 0U;
	push.size = RENDER_PUSH_BYTES;
	memset(&layout, 0, sizeof(layout));
	layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout.setLayoutCount = 1U;
	layout.pSetLayouts = &renderer->set_layout;
	layout.pushConstantRangeCount = 1U;
	layout.pPushConstantRanges = &push;
	renderer->operation = "vkCreatePipelineLayout";
	error = vkCreatePipelineLayout(renderer->device, &layout, NULL, &renderer->layout);
	if (error != VK_SUCCESS)
		return error;

	/* The vertex shader. */
	error = render_module(renderer, notes_draw_vert, sizeof(notes_draw_vert), &vertex);
	if (error != VK_SUCCESS)
		return error;

	/* The fill shader. */
	error = render_module(renderer, notes_fill_frag, sizeof(notes_fill_frag), &fill);
	if (error != VK_SUCCESS) {
		vkDestroyShaderModule(renderer->device, vertex, NULL);
		return error;
	}

	/* The texture shader. */
	error = render_module(renderer, notes_texture_frag, sizeof(notes_texture_frag), &texture);
	if (error != VK_SUCCESS) {
		vkDestroyShaderModule(renderer->device, vertex, NULL);
		vkDestroyShaderModule(renderer->device, fill, NULL);
		return error;
	}

	/* Each pipeline but the groups'; the texture pipeline samples, the others fill. */
	for (pipe = 0; pipe < NOTES_PIPES_FIRST; pipe++) {
		if (pipe == NOTES_PIPE_TEXTURE)
			error = render_pipeline(renderer, pipe, vertex, texture);
		else
			error = render_pipeline(renderer, pipe, vertex, fill);
		if (error != VK_SUCCESS)
			break;
	}

	/* The modules are not needed once the pipelines are made. */
	vkDestroyShaderModule(renderer->device, vertex, NULL);
	vkDestroyShaderModule(renderer->device, fill, NULL);
	vkDestroyShaderModule(renderer->device, texture, NULL);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: every pipeline made with the renderer. */
	return VK_SUCCESS;
}

/* Makes one pipeline: two vec4 attributes, triangles, its blending and its stencil use. */
static VkResult
render_pipeline(
	struct notes_renderer *renderer,
	unsigned pipe,
	VkShaderModule vertex,
	VkShaderModule fragment)
{
	static const VkDynamicState dynamic[] = {
		VK_DYNAMIC_STATE_VIEWPORT,
		VK_DYNAMIC_STATE_SCISSOR
	};
	VkPipelineShaderStageCreateInfo stages[2];
	VkVertexInputBindingDescription binding;
	VkVertexInputAttributeDescription attributes[2];
	VkPipelineVertexInputStateCreateInfo input;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineDepthStencilStateCreateInfo stencil;
	VkPipelineColorBlendAttachmentState blend_attachment;
	VkPipelineColorBlendStateCreateInfo blend;
	VkPipelineDynamicStateCreateInfo dynamic_state;
	VkGraphicsPipelineCreateInfo pipeline;
	VkResult error;

	/* The two stages. */
	memset(stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vertex;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragment;
	stages[1].pName = "main";

	/* One vertex buffer of two vec4s a vertex: position and extra, then colour. */
	memset(&binding, 0, sizeof(binding));
	binding.binding = 0U;
	binding.stride = NOTES_VERTEX_FLOATS * sizeof(float);
	binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	memset(attributes, 0, sizeof(attributes));
	attributes[0].location = 0U;
	attributes[0].binding = 0U;
	attributes[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	attributes[0].offset = 0U;
	attributes[1].location = 1U;
	attributes[1].binding = 0U;
	attributes[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	attributes[1].offset = 4U * sizeof(float);
	memset(&input, 0, sizeof(input));
	input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	input.vertexBindingDescriptionCount = 1U;
	input.pVertexBindingDescriptions = &binding;
	input.vertexAttributeDescriptionCount = 2U;
	input.pVertexAttributeDescriptions = attributes;
	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	/* The viewport and scissor are set each frame; both faces are drawn (the fan's winding needs them). */
	memset(&viewport, 0, sizeof(viewport));
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1U;
	viewport.scissorCount = 1U;
	memset(&raster, 0, sizeof(raster));
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;
	memset(&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	/* No depth test; the stencil as the pipeline's role asks. */
	memset(&stencil, 0, sizeof(stencil));
	stencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	stencil.front.compareMask = 0xffU;
	stencil.front.writeMask = 0xffU;
	stencil.front.reference = 0U;
	stencil.front.failOp = VK_STENCIL_OP_KEEP;
	stencil.front.depthFailOp = VK_STENCIL_OP_KEEP;
	stencil.front.passOp = VK_STENCIL_OP_KEEP;
	stencil.front.compareOp = VK_COMPARE_OP_ALWAYS;
	stencil.back = stencil.front;
	if (pipe == NOTES_PIPE_STENCIL) {
		/* The fan counts front faces up and back faces down: the winding number, modulo 256. */
		stencil.stencilTestEnable = VK_TRUE;
		stencil.front.passOp = VK_STENCIL_OP_INCREMENT_AND_WRAP;
		stencil.back.passOp = VK_STENCIL_OP_DECREMENT_AND_WRAP;
	} else if (pipe == NOTES_PIPE_FRINGE ||
		   pipe == NOTES_PIPE_FRINGE_GRAIN ||
		   pipe == NOTES_PIPE_FRINGE_MULTIPLY) {
		/* A fringe draws only outside the polygon. */
		stencil.stencilTestEnable = VK_TRUE;
		stencil.front.compareOp = VK_COMPARE_OP_EQUAL;
		stencil.front.writeMask = 0U;
		stencil.back = stencil.front;
	} else if (pipe == NOTES_PIPE_COVER ||
		   pipe == NOTES_PIPE_COVER_GRAIN ||
		   pipe == NOTES_PIPE_COVER_MULTIPLY) {
		/* A cover draws only inside, and sets the stencil back to zero there. */
		stencil.stencilTestEnable = VK_TRUE;
		stencil.front.compareOp = VK_COMPARE_OP_NOT_EQUAL;
		stencil.front.passOp = VK_STENCIL_OP_ZERO;
		stencil.back = stencil.front;
	}

	/*
	 * Premultiplied-alpha blending, and no colour at all while counting the
	 * winding.  The shaders give the colour already multiplied by its alpha,
	 * so the source is added at one; this is the only blend the Raspberry
	 * Pi 4's driver takes, and it draws the same pixels as straight alpha did.
	 */
	memset(&blend_attachment, 0, sizeof(blend_attachment));
	blend_attachment.blendEnable = VK_TRUE;
	blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
	blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
	blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	if (pipe == NOTES_PIPE_STENCIL)
		blend_attachment.colorWriteMask = 0U;

	/*
	 * The Multiply's (ws216-p004): the target's colour times the shader's
	 * (1 - A + A cs), its alpha kept.  A device that refuses these factors
	 * refuses the pipeline when it is made, and the group is drawn instead.
	 */
	if (pipe == NOTES_PIPE_FRINGE_MULTIPLY || pipe == NOTES_PIPE_COVER_MULTIPLY) {
		blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
		blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
		blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	}
	memset(&blend, 0, sizeof(blend));
	blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blend.attachmentCount = 1U;
	blend.pAttachments = &blend_attachment;
	memset(&dynamic_state, 0, sizeof(dynamic_state));
	dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic_state.dynamicStateCount = 2U;
	dynamic_state.pDynamicStates = dynamic;

	/* The pipeline. */
	memset(&pipeline, 0, sizeof(pipeline));
	pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline.stageCount = 2U;
	pipeline.pStages = stages;
	pipeline.pVertexInputState = &input;
	pipeline.pInputAssemblyState = &assembly;
	pipeline.pViewportState = &viewport;
	pipeline.pRasterizationState = &raster;
	pipeline.pMultisampleState = &multisample;
	pipeline.pDepthStencilState = &stencil;
	pipeline.pColorBlendState = &blend;
	pipeline.pDynamicState = &dynamic_state;
	pipeline.layout = renderer->layout;
	pipeline.renderPass = renderer->pass;
	renderer->operation = "vkCreateGraphicsPipelines";
	error = vkCreateGraphicsPipelines(renderer->device, VK_NULL_HANDLE, 1U, &pipeline, NULL, &renderer->pipes[pipe]);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the pipeline. */
	return VK_SUCCESS;
}

/* Makes a shader module from SPIR-V words. */
static VkResult
render_module(
	struct notes_renderer *renderer,
	const uint32_t *code,
	size_t size,
	VkShaderModule *module)
{
	VkShaderModuleCreateInfo create;
	VkResult error;

	/* The module over the words. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	create.codeSize = size;
	create.pCode = code;
	renderer->operation = "vkCreateShaderModule";
	error = vkCreateShaderModule(renderer->device, &create, NULL, module);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the module. */
	return VK_SUCCESS;
}

/*
 * Records the frame: the toolbar's first layout change, the page frame's
 * pass into the page's picture when there is one, then the window's
 * cleared pass with every draw in order.
 */
static void
render_record(
	struct notes_renderer *renderer,
	uint32_t image,
	const struct notes_frame *frame,
	const struct notes_frame *page_frame,
	int page_clear)
{
	VkCommandBufferBeginInfo begin;
	VkRenderPassBeginInfo pass;
	VkClearValue clear[2];
	VkExtent2D page_extent;
	uint32_t base;

	/* One submission of this recording. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	(void)vkBeginCommandBuffer(renderer->command, &begin);

	/* The pictures the host wrote since the last frame, moved to the layout they are sampled in. */
	render_pictures_ready(renderer);

	/* The page frame, drawn into the page's picture: cleared to nothing, or kept, and the stencil to zero. */
	base = 0;
	if (page_frame != NULL && renderer->page != VK_NULL_HANDLE) {
		memset(clear, 0, sizeof(clear));
		clear[1].depthStencil.depth = 1.0f;
		clear[1].depthStencil.stencil = 0U;
		page_extent.width = renderer->page_width;
		page_extent.height = renderer->page_height;
		memset(&pass, 0, sizeof(pass));
		pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
		pass.renderPass = renderer->page_load_pass;
		if (page_clear)
			pass.renderPass = renderer->page_clear_pass;
		pass.framebuffer = renderer->page_framebuffer;
		pass.renderArea.extent = page_extent;
		pass.clearValueCount = 2U;
		pass.pClearValues = clear;
		vkCmdBeginRenderPass(renderer->command, &pass, VK_SUBPASS_CONTENTS_INLINE);

		/* Its draws, with the picture's size. */
		render_draws(renderer, page_frame, 0U, page_extent);
		vkCmdEndRenderPass(renderer->command);
	}

	/* The frame's vertices follow the page frame's. */
	if (page_frame != NULL)
		base = (uint32_t)page_frame->vertex_count;

	/* The window's pass, the colour cleared to the desk and the stencil to zero. */
	memset(clear, 0, sizeof(clear));
	clear[0].color.float32[0] = RENDER_DESK_RED;
	clear[0].color.float32[1] = RENDER_DESK_GREEN;
	clear[0].color.float32[2] = RENDER_DESK_BLUE;
	clear[0].color.float32[3] = 1.0f;
	clear[1].depthStencil.depth = 1.0f;
	clear[1].depthStencil.stencil = 0U;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = renderer->pass;
	pass.framebuffer = renderer->targets[image].framebuffer;
	pass.renderArea.extent = renderer->extent;
	pass.clearValueCount = 2U;
	pass.pClearValues = clear;
	vkCmdBeginRenderPass(renderer->command, &pass, VK_SUBPASS_CONTENTS_INLINE);

	/* The frame's draws, with the window's size. */
	render_draws(renderer, frame, base, renderer->extent);

	/* The pass ends with the image ready to present. */
	vkCmdEndRenderPass(renderer->command);
}

/*
 * Moves each picture the host wrote and no frame used yet (the toolbar's,
 * the background's, the overlay's and the grain's) from its first layout,
 * which kept the host's writes, to the general layout it is sampled in.
 */
static void
render_pictures_ready(
	struct notes_renderer *renderer)
{
	unsigned level;

	/* The toolbar's image. */
	if (renderer->toolbar != VK_NULL_HANDLE && renderer->toolbar_ready == 0) {
		render_picture_ready(renderer, renderer->toolbar);
		renderer->toolbar_ready = 1;
	}

	/* The background's. */
	if (renderer->background != VK_NULL_HANDLE && renderer->background_ready == 0) {
		render_picture_ready(renderer, renderer->background);
		renderer->background_ready = 1;
	}

	/* The overlay's (ws175-p008). */
	if (renderer->overlay != VK_NULL_HANDLE && renderer->overlay_ready == 0) {
		render_picture_ready(renderer, renderer->overlay);
		renderer->overlay_ready = 1;
	}

	/* Each of the grain's (ws216-p004). */
	for (level = 0; level < NOTES_GRAIN_LEVELS; level++) {
		if (renderer->grain[level].image == VK_NULL_HANDLE || renderer->grain[level].ready)
			continue;
		render_picture_ready(renderer, renderer->grain[level].image);
		renderer->grain[level].ready = 1;
	}
}

/* Records the move of one host-written picture from preinitialized (the host's writes kept) to general. */
static void
render_picture_ready(
	struct notes_renderer *renderer,
	VkImage image)
{
	VkImageMemoryBarrier barrier;

	/* The host's writes are seen by the fragment shader's reads. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1U;
	barrier.subresourceRange.layerCount = 1U;
	vkCmdPipelineBarrier(renderer->command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);
}

/*
 * Records a list of draws into the pass begun: the viewport and the size
 * the vertex shader maps from, then each draw in order, its vertices
 * counted from base in the run of the chunks.  A draw of a group the
 * device does not have is drawn by the pipeline that stands in for it; a
 * draw through the grain binds the grain's size for its scale and pushes
 * its page's place.
 */
static void
render_draws(
	struct notes_renderer *renderer,
	const struct notes_frame *frame,
	uint32_t base,
	VkExtent2D extent)
{
	VkViewport viewport;
	VkRect2D scissor;
	VkRect2D whole;
	const struct notes_draw *draw;
	float size[4];
	float grain[4];
	float pushed[3];
	int place_changed;
	size_t bound_chunk;
	unsigned bound;
	unsigned bound_texture;
	unsigned pipe;
	unsigned texture;
	size_t index;

	/* The whole target is the viewport. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.width = (float)extent.width;
	viewport.height = (float)extent.height;
	viewport.maxDepth = 1.0f;
	memset(&whole, 0, sizeof(whole));
	whole.extent = extent;
	vkCmdSetViewport(renderer->command, 0U, 1U, &viewport);

	/* The target's size, which the vertex shader maps pixels from. */
	size[0] = (float)extent.width;
	size[1] = (float)extent.height;
	size[2] = 0.0f;
	size[3] = 0.0f;
	vkCmdPushConstants(renderer->command, renderer->layout, VK_SHADER_STAGE_VERTEX_BIT, 0U, sizeof(size), size);

	/* Each draw in order, binding its pipeline and its picture when they change and its scissor every time. */
	bound = NOTES_PIPES;
	bound_texture = NOTES_TEXTURES;
	bound_chunk = (size_t)-1;
	pushed[0] = 0.0f;
	pushed[1] = 0.0f;
	pushed[2] = -1.0f;
	for (index = 0; index < frame->draw_count; index++) {
		draw = &frame->draws[index];
		if (draw->count == 0U)
			continue;

		/* The pipeline: the draw's own, or the one that stands in for it. */
		pipe = render_pipe(renderer, draw->pipe);
		if (pipe != bound) {
			vkCmdBindPipeline(renderer->command, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->pipes[pipe]);
			bound = pipe;
		}

		/* A picture's set, for a texture draw (the page's and the background's only once they are made). */
		if (pipe == NOTES_PIPE_TEXTURE && draw->texture != bound_texture) {
			if (draw->texture == NOTES_TEXTURE_PAGE && renderer->page == VK_NULL_HANDLE)
				continue;
			if (draw->texture == NOTES_TEXTURE_BACKGROUND && renderer->background == VK_NULL_HANDLE)
				continue;
			if (draw->texture == NOTES_TEXTURE_OVERLAY && renderer->overlay == VK_NULL_HANDLE)
				continue;
			vkCmdBindDescriptorSets(renderer->command, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer->layout, 0U, 1U,
						&renderer->sets[draw->texture], 0U, NULL);
			bound_texture = draw->texture;
		}

		/* The grain's size for the draw's scale, and its page's place when it is not the one pushed last. */
		if (pipe == NOTES_PIPE_FRINGE_GRAIN || pipe == NOTES_PIPE_COVER_GRAIN) {
			texture = NOTES_TEXTURE_GRAIN + notes_frame_grain_level(draw->place[2]);
			if (texture != bound_texture) {
				vkCmdBindDescriptorSets(renderer->command,
							VK_PIPELINE_BIND_POINT_GRAPHICS,
							renderer->layout,
							0U,
							1U,
							&renderer->sets[texture],
							0U,
							NULL);
				bound_texture = texture;
			}

			/* The page's place, pushed after the target's size when another page's was pushed last. */
			place_changed = memcmp(pushed, draw->place, sizeof(pushed));
			if (place_changed != 0) {
				render_grain_place(draw, grain);
				vkCmdPushConstants(renderer->command,
						   renderer->layout,
						   VK_SHADER_STAGE_VERTEX_BIT,
						   (uint32_t)sizeof(size),
						   sizeof(grain),
						   grain);
				memcpy(pushed, draw->place, sizeof(pushed));
			}
		}

		/* The clipping rectangle for a clipped draw (kept inside the target), or the whole target. */
		scissor = whole;
		if (draw->clipped) {
			scissor.offset.x = draw->clip[0];
			scissor.offset.y = draw->clip[1];
			scissor.extent.width = (uint32_t)draw->clip[2];
			scissor.extent.height = (uint32_t)draw->clip[3];
			if (scissor.offset.x < 0) {
				scissor.extent.width = (uint32_t)((int32_t)scissor.extent.width + scissor.offset.x);
				scissor.offset.x = 0;
			}

			/* And above the top. */
			if (scissor.offset.y < 0) {
				scissor.extent.height = (uint32_t)((int32_t)scissor.extent.height + scissor.offset.y);
				scissor.offset.y = 0;
			}
		}

		/* The draw inside its scissor, in pieces at the chunks' edges. */
		vkCmdSetScissor(renderer->command, 0U, 1U, &scissor);
		render_pieces(renderer, base + draw->first, draw->count, &bound_chunk);
	}
}

/*
 * Records a draw of a run of vertices as one draw for each chunk the run
 * falls in, binding each chunk when it is not the one bound.  A chunk holds
 * whole triangles and a run starts at a triangle, so each piece is whole
 * triangles too.
 */
static void
render_pieces(
	struct notes_renderer *renderer,
	uint32_t first,
	uint32_t count,
	size_t *bound_chunk)
{
	VkDeviceSize offset;
	size_t chunk;
	uint32_t start;
	uint32_t piece;

	/* Each piece that falls in one chunk. */
	while (count > 0U) {
		/* The chunk the run is in now, where in it, and how much of the run it holds. */
		chunk = first / renderer->chunk_vertices;
		start = (uint32_t)(first % renderer->chunk_vertices);
		piece = (uint32_t)(renderer->chunk_vertices - start);
		if (piece > count)
			piece = count;

		/* The chunk, bound when another is. */
		if (chunk != *bound_chunk) {
			offset = 0U;
			vkCmdBindVertexBuffers(renderer->command, 0U, 1U, &renderer->chunks[chunk].buffer, &offset);
			*bound_chunk = chunk;
		}

		/* The piece, and the run goes on after it. */
		vkCmdDraw(renderer->command, piece, 1U, start, 0U);
		first += piece;
		count -= piece;
	}
}

/*
 * Gives the push constants of the grain for a draw: its page's top left
 * in the target's pixels, and the tile's cells a pixel at its scale.
 */
static void
render_grain_place(
	const struct notes_draw *draw,
	float *grain)
{
	/* The page's place. */
	grain[0] = draw->place[0];
	grain[1] = draw->place[1];
	grain[2] = 0.0f;
	grain[3] = 0.0f;

	/* A cell is NOTES_GRAIN_CELL points; a draw without a scale keeps one place of the tile. */
	if (draw->place[2] > 0.0f)
		grain[2] = (float)(1.0 / (NOTES_GRAIN_CELL * (double)draw->place[2]));
}
