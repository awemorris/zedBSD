/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The drawing layer on the GPU: the pixels of the top-level windows and of
 * the pixmaps of depth 24 kept in Vulkan images, and the operations of
 * render.h drawn there by the X rules.
 *
 * An image keeps its pixels in memory as well (render-cpu.c's), and its
 * copy on the GPU is the newer one once the GPU has drawn into it; the
 * memory is brought up to date only when something reads it (GetImage, a
 * copy into memory, the backend's present).  The image is B8G8R8A8_UNORM
 * with its alpha kept at 0xff.
 *
 * Boxes are filled by clearing them (a solid copy of every plane) or by
 * drawing them through paint.frag, whose pipeline joins the source with
 * the target by the graphics context's function (a logic operation) and
 * writes only the colour channels the plane mask names.  A copy of every
 * plane is a copy of images or from a staging buffer.  What the device
 * cannot do this way (a function without logic operations, a plane mask
 * that is not whole channels, an image the device cannot make) is drawn
 * by the processor: the part of the image is read back, drawn in memory
 * and put back.
 *
 * The commands are recorded into one of three command buffers and
 * submitted when something must wait for them (a read back, a present) or
 * a buffer is full.  The staging memory a command buffer uses for its
 * uploads and boxes is reused only after its fence.  Images are kept in
 * VK_IMAGE_LAYOUT_GENERAL; a barrier is recorded between two operations
 * only when the second touches what the first wrote, or writes what it
 * read.
 */

#include "userland/desktop/xserver/internal.h"
#include "userland/desktop/xserver/render-vulkan.h"
#include "userland/desktop/xserver/render-shaders.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The command buffers recorded and submitted in turn. */
#define RENDER_SLOTS		3U

/* The staging memory a command buffer starts with (it grows for a larger upload). */
#define RENDER_STAGING_SIZE	((VkDeviceSize)4U * 1024U * 1024U)

/* The descriptor sets a command buffer may use before it is submitted. */
#define RENDER_DESCRIPTORS	256U

/* The most images (windows' and pixmaps') kept on the GPU; later ones are drawn in memory. */
#define RENDER_IMAGES_MAX	2048U

/* How long a wait for the GPU may take, in nanoseconds. */
#define RENDER_WAIT_NS		5000000000ULL

/* The alpha every pixel of depth 24 has on the GPU. */
#define RENDER_OPAQUE		0xff000000U

/* The colour planes of depth 24. */
#define RENDER_PLANES		0x00ffffffU

/* The modes of paint.frag. */
#define RENDER_MODE_SOLID		0
#define RENDER_MODE_TILED		1
#define RENDER_MODE_STIPPLED		2
#define RENDER_MODE_OPAQUE_STIPPLED	3
#define RENDER_MODE_COPY		4

/* What an image on the GPU is: a drawable's, a tile's or stipple's copy, or the layer's scratch. */
#define RENDER_KIND_TARGET	0U
#define RENDER_KIND_PATTERN	1U
#define RENDER_KIND_SCRATCH	2U

/* The colour channels a pipeline may write (bit 0 blue, 1 green, 2 red, as the planes of a pixel). */
#define RENDER_WRITE_ALL	7U

/*
 * An image's copy on the GPU: the Vulkan image, its memory and view, the
 * framebuffer it is drawn through (made with the first draw), its size
 * (the room of its pixels in memory), which copy is newer, and what the
 * operations since the last barrier did with it.
 */
struct x11_gpu_image {
	struct x11_vulkan *vulkan;
	unsigned kind;
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkFramebuffer framebuffer;
	int width;
	int height;

	/* Nonzero when the GPU has drawn what the pixels in memory do not show. */
	int memory_stale;

	/* Nonzero when the pixels in memory are newer than the GPU's (a pattern drawn in memory). */
	int gpu_stale;

	/*
	 * The barrier generation the image was last touched in, whether it was
	 * read and written then, and the box written: a later operation that
	 * reads what was written, or writes what was read or written, needs a
	 * barrier first.
	 */
	uint64_t touched;
	int read;
	int written;
	struct x11_box written_box;
};

/*
 * One command buffer and what it uses until its fence: the staging
 * buffer its uploads and boxes are written into, the descriptor sets, and
 * the images that went while it was recorded (destroyed after the fence).
 */
struct render_slot {
	VkCommandBuffer command;
	VkFence fence;
	int submitted;
	VkBuffer staging;
	VkDeviceMemory staging_memory;
	uint8_t *staging_bytes;
	VkDeviceSize staging_size;
	VkDeviceSize staging_used;
	VkDescriptorPool descriptors;
	unsigned descriptors_used;
	struct x11_gpu_image **retired;
	size_t retired_count;
	size_t retired_capacity;
};

/*
 * The drawing layer's state on the device: the command buffers, the
 * render pass and pipelines, the pattern of a solid fill, the scratch
 * image copies go through, the buffer read backs land in, and counts the
 * log reports.
 */
struct x11_render_vulkan {
	VkCommandPool pool;
	struct render_slot slots[RENDER_SLOTS];
	unsigned current;
	int recording;
	uint64_t generation;

	VkRenderPass render_pass;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkShaderModule vertex;
	VkShaderModule fragment;
	VkSampler sampler;
	VkPipeline pipelines[16][RENDER_WRITE_ALL + 1U];

	struct x11_gpu_image *blank;
	struct x11_gpu_image *scratch;

	VkBuffer readback;
	VkDeviceMemory readback_memory;
	uint8_t *readback_bytes;
	VkDeviceSize readback_size;
	int readback_coherent;

	unsigned images;
	unsigned long fallbacks;
	unsigned long readbacks;
};

/*
 * The values paint.vert and paint.frag take (their push constants, laid
 * out as the shaders' block: the pairs first, each on 8 bytes).
 */
struct render_push {
	int32_t target_size[2];
	int32_t origin[2];
	int32_t pattern_size[2];
	int32_t mode;
	uint32_t foreground;
	uint32_t background;
};

/*
 * One image an operation reads or writes, with the box it writes.
 */
struct render_access {
	struct x11_gpu_image *image;
	int write;
	struct x11_box box;
};

/*
 * The boxes an operation draws, clipped: a growing array.
 */
struct render_parts {
	struct x11_box *boxes;
	size_t count;
	size_t capacity;
	struct x11_box extents;
};

static VkResult render_slot_make(struct x11_vulkan *vulkan, struct render_slot *slot);
static VkResult render_slot_staging(struct x11_vulkan *vulkan, struct render_slot *slot, VkDeviceSize size);
static void render_slot_free(struct x11_vulkan *vulkan, struct render_slot *slot);
static void render_slot_release(struct x11_vulkan *vulkan, struct render_slot *slot);
static VkResult render_pipeline_objects(struct x11_vulkan *vulkan);
static VkPipeline render_pipeline(struct x11_vulkan *vulkan, unsigned function, unsigned write);
static VkCommandBuffer render_begin(struct x11_vulkan *vulkan);
static VkResult render_wait(struct x11_vulkan *vulkan);
static void render_barrier(struct x11_vulkan *vulkan, VkCommandBuffer command);
static void render_accesses(struct x11_vulkan *vulkan, VkCommandBuffer command, const struct render_access *accesses, unsigned count);
static void *render_staging(struct x11_vulkan *vulkan, VkDeviceSize size, VkBuffer *buffer, VkDeviceSize *offset);
static struct x11_gpu_image *render_image_make(struct x11_vulkan *vulkan, unsigned kind, int width, int height);
static void render_image_destroy(struct x11_vulkan *vulkan, struct x11_gpu_image *image);
static void render_image_retire(struct x11_gpu_image *image);
static VkResult render_framebuffer(struct x11_gpu_image *image);
static int render_upload(struct x11_gpu_image *image, int x, int y, int width, int height, const uint32_t *pixels, size_t stride);
static int render_download(struct x11_gpu_image *image, int x, int y, int width, int height, uint32_t *pixels, size_t stride);
static int render_readback_room(struct x11_vulkan *vulkan, VkDeviceSize size);
static struct x11_gpu_image *render_pattern(struct x11_vulkan *vulkan, const struct x11_image *pattern);
static struct x11_gpu_image *render_scratch(struct x11_vulkan *vulkan, int width, int height);
static int render_parts_add(struct render_parts *parts, const struct x11_box *box);
static void render_parts_clip(struct render_parts *parts, const struct x11_image *target, const struct x11_region *clip, const struct x11_box *box);
static int render_write_mask(uint32_t plane_mask, unsigned *write);
static int render_fill_gpu(struct x11_image *target, const struct x11_paint *paint, const struct render_parts *parts);
static int render_clear(struct x11_image *target, uint32_t pixel, const struct render_parts *parts);
static int render_draw(struct x11_image *target, struct x11_gpu_image *pattern, unsigned function, unsigned write, const struct render_push *push, const struct render_parts *parts);
static int render_copy_gpu(struct x11_image *target, const struct x11_image *source, int dx, int dy, unsigned function, uint32_t plane_mask, const struct render_parts *parts);
static int render_copy_images(struct x11_gpu_image *target, int target_dx, int target_dy, struct x11_gpu_image *source, int source_dx, int source_dy, const struct render_parts *parts);
static void render_fallback_begin(const struct x11_image *image, const struct x11_box *box, struct x11_image *view);
static void render_fallback_end(struct x11_image *image, const struct x11_box *box);
static int render_usable(const struct x11_image *image);
static int render_wrap(int value, int size);

/*
 * Makes the drawing layer on the server's device: its command buffers,
 * render pass, pipeline layout and shaders.  Returns 0, or an errno value
 * (the windows and pixmaps are then drawn in memory).
 */
int
x11_render_vulkan_open(
	struct x11_vulkan *vulkan)
{
	struct x11_render_vulkan *render;
	VkCommandPoolCreateInfo pool;
	VkResult error;
	unsigned index;

	/* The state. */
	render = calloc(1U, sizeof(*render));
	if (render == NULL)
		return ENOMEM;
	vulkan->render = render;
	render->generation = 1U;

	/* The pool the command buffers come from, each recorded again after its fence. */
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = vulkan->family;
	error = vulkan->vk.vkCreateCommandPool(vulkan->device, &pool, NULL, &render->pool);
	if (error != VK_SUCCESS) {
		x11_render_vulkan_close(vulkan);
		return EIO;
	}

	/* The command buffers and their staging memory. */
	for (index = 0U; index < RENDER_SLOTS; index++) {
		error = render_slot_make(vulkan, &render->slots[index]);
		if (error != VK_SUCCESS) {
			x11_render_vulkan_close(vulkan);
			return EIO;
		}
	}

	/* The render pass, the shaders and the layout the pipelines share. */
	error = render_pipeline_objects(vulkan);
	if (error != VK_SUCCESS) {
		x11_render_vulkan_close(vulkan);
		return EIO;
	}

	/* The pattern bound when a fill takes none (one black pixel). */
	render->blank = render_image_make(vulkan, RENDER_KIND_SCRATCH, 1, 1);
	if (render->blank == NULL) {
		x11_render_vulkan_close(vulkan);
		return EIO;
	}

	/* Succeeded: the windows and pixmaps can be drawn on the device. */
	x11_log(vulkan->server, "X11SERVER RENDER vulkan logic_op=%d image_side_max=%u", vulkan->logic_op, (unsigned)vulkan->image_side_max);
	return 0;
}

/*
 * Ends the drawing layer, after everything submitted to the device (every
 * image of a window or pixmap has gone before).
 */
void
x11_render_vulkan_close(
	struct x11_vulkan *vulkan)
{
	struct x11_render_vulkan *render;
	VkDevice device;
	unsigned function;
	unsigned write;
	unsigned index;

	/* Nothing was made. */
	render = vulkan->render;
	if (render == NULL)
		return;
	device = vulkan->device;

	/* The device finishes what it was given first. */
	(void)vulkan->vk.vkDeviceWaitIdle(device);
	x11_log(vulkan->server, "X11SERVER RENDER end fallbacks=%lu readbacks=%lu", render->fallbacks, render->readbacks);

	/* The images that went, the scratch and the blank pattern. */
	for (index = 0U; index < RENDER_SLOTS; index++)
		render_slot_release(vulkan, &render->slots[index]);
	if (render->scratch != NULL)
		render_image_destroy(vulkan, render->scratch);
	if (render->blank != NULL)
		render_image_destroy(vulkan, render->blank);

	/* The command buffers and their memory. */
	for (index = 0U; index < RENDER_SLOTS; index++)
		render_slot_free(vulkan, &render->slots[index]);
	if (render->pool != VK_NULL_HANDLE)
		vulkan->vk.vkDestroyCommandPool(device, render->pool, NULL);

	/* The buffer read backs land in. */
	if (render->readback != VK_NULL_HANDLE)
		vulkan->vk.vkDestroyBuffer(device, render->readback, NULL);
	if (render->readback_memory != VK_NULL_HANDLE)
		vulkan->vk.vkFreeMemory(device, render->readback_memory, NULL);

	/* The pipelines. */
	for (function = 0U; function < 16U; function++) {
		for (write = 0U; write <= RENDER_WRITE_ALL; write++) {
			if (render->pipelines[function][write] != VK_NULL_HANDLE)
				vulkan->vk.vkDestroyPipeline(device, render->pipelines[function][write], NULL);
		}
	}

	/* What the pipelines share. */
	if (render->sampler != VK_NULL_HANDLE)
		vulkan->vk.vkDestroySampler(device, render->sampler, NULL);
	if (render->vertex != VK_NULL_HANDLE)
		vulkan->vk.vkDestroyShaderModule(device, render->vertex, NULL);
	if (render->fragment != VK_NULL_HANDLE)
		vulkan->vk.vkDestroyShaderModule(device, render->fragment, NULL);
	if (render->layout != VK_NULL_HANDLE)
		vulkan->vk.vkDestroyPipelineLayout(device, render->layout, NULL);
	if (render->set_layout != VK_NULL_HANDLE)
		vulkan->vk.vkDestroyDescriptorSetLayout(device, render->set_layout, NULL);
	if (render->render_pass != VK_NULL_HANDLE)
		vulkan->vk.vkDestroyRenderPass(device, render->render_pass, NULL);

	/* The state. */
	free(render);
	vulkan->render = NULL;
}

/*
 * Gives an image of depth 24 a copy on the GPU whose every pixel is fill
 * (as the pixels in memory are), with room of the image's stride by rows.
 * Without the GPU's layer, for another depth, or when the device cannot
 * hold another image, the image stays in memory only.
 */
void
x11_render_image_attach(
	struct x11_vulkan *vulkan,
	struct x11_image *image,
	int rows,
	uint32_t fill)
{
	struct x11_gpu_image *copy;
	VkClearColorValue colour;
	VkImageSubresourceRange range;
	VkCommandBuffer command;

	/* Only a drawable of depth 24 is drawn on the GPU, while there is room for it. */
	image->gpu = NULL;
	if (vulkan == NULL || vulkan->render == NULL || vulkan->lost)
		return;
	if (image->depth != X11_DEPTH || image->pixels == NULL)
		return;
	if (vulkan->render->images >= RENDER_IMAGES_MAX)
		return;
	if ((uint32_t)image->stride > vulkan->image_side_max || (uint32_t)rows > vulkan->image_side_max)
		return;

	/* The image on the device. */
	copy = render_image_make(vulkan, RENDER_KIND_TARGET, (int)image->stride, rows);
	if (copy == NULL)
		return;

	/* Every pixel the fill, opaque, as in memory. */
	command = render_begin(vulkan);
	if (command == VK_NULL_HANDLE) {
		render_image_destroy(vulkan, copy);
		return;
	}
	colour.float32[0] = (float)((fill >> 16) & 0xffU) / 255.0f;
	colour.float32[1] = (float)((fill >> 8) & 0xffU) / 255.0f;
	colour.float32[2] = (float)(fill & 0xffU) / 255.0f;
	colour.float32[3] = 1.0f;
	memset(&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1U;
	range.layerCount = 1U;
	vulkan->vk.vkCmdClearColorImage(command, copy->image, VK_IMAGE_LAYOUT_GENERAL, &colour, 1U, &range);

	/* The clear wrote all of it: the next operation waits for it. */
	copy->touched = vulkan->render->generation;
	copy->written = 1;
	copy->written_box.x0 = 0;
	copy->written_box.y0 = 0;
	copy->written_box.x1 = copy->width;
	copy->written_box.y1 = copy->height;

	/* Succeeded: the image is drawn on the GPU from now on. */
	vulkan->render->images++;
	image->gpu = copy;
}

/*
 * Takes an image's copy off the GPU (before its pixels go).  The device's
 * image is destroyed once the commands that may use it are done.
 */
void
x11_render_image_detach(
	struct x11_image *image)
{
	struct x11_gpu_image *copy;

	/* Nothing on the GPU. */
	copy = image->gpu;
	if (copy == NULL)
		return;
	image->gpu = NULL;

	/* A drawable's image no longer counts against the most there may be. */
	if (copy->kind == RENDER_KIND_TARGET && copy->vulkan->render != NULL)
		copy->vulkan->render->images--;

	/* Succeeded: it goes after the commands that may use it. */
	render_image_retire(copy);
}

/*
 * Brings all of an image's pixels in memory up to date with its copy on
 * the GPU, before something other than these operations reads them.
 */
void
x11_render_image_memory(
	const struct x11_image *image)
{
	struct x11_gpu_image *copy;
	int failed;

	/* Memory is already as new. */
	copy = image->gpu;
	if (copy == NULL || !copy->memory_stale || copy->vulkan->lost)
		return;

	/* The whole room read back. */
	failed = render_download(copy, 0, 0, copy->width, copy->height, image->pixels, image->stride);
	if (failed != 0)
		return;

	/* Succeeded: memory is as new as the GPU. */
	copy->memory_stale = 0;
}

/*
 * Brings a box of an image's pixels in memory up to date with its copy on
 * the GPU (the box is kept inside the image).
 */
void
x11_render_image_memory_box(
	const struct x11_image *image,
	const struct x11_box *box)
{
	struct x11_gpu_image *copy;
	struct x11_box bounds;
	struct x11_box part;
	int inside;

	/* Memory is already as new. */
	copy = image->gpu;
	if (copy == NULL || !copy->memory_stale || copy->vulkan->lost)
		return;

	/* The box inside the room. */
	bounds.x0 = 0;
	bounds.y0 = 0;
	bounds.x1 = copy->width;
	bounds.y1 = copy->height;
	inside = x11_box_intersect(box, &bounds, &part);
	if (!inside)
		return;

	/* Succeeded: that part read back (the rest of memory stays older). */
	(void)render_download(copy, part.x0, part.y0, part.x1 - part.x0, part.y1 - part.y0, image->pixels + (size_t)part.y0 * image->stride + (size_t)part.x0, image->stride);
}

/*
 * Fills boxes on an image with a copy on the GPU (render-cpu.c's
 * x11_render_boxes for such an image).
 */
void
x11_render_vulkan_boxes(
	struct x11_image *target,
	const struct x11_region *clip,
	const struct x11_paint *paint,
	const struct x11_box *boxes,
	size_t count)
{
	struct render_parts parts;
	struct x11_image view;
	size_t index;
	int drawn;

	/* An image the GPU cannot draw on is drawn in memory. */
	drawn = render_usable(target);
	if (!drawn) {
		render_fallback_begin(target, NULL, &view);
		x11_render_boxes(&view, clip, paint, boxes, count);
		render_fallback_end(target, NULL);
		return;
	}

	/* The boxes, clipped. */
	memset(&parts, 0, sizeof(parts));
	for (index = 0U; index < count; index++)
		render_parts_clip(&parts, target, clip, &boxes[index]);
	if (parts.count == 0U) {
		free(parts.boxes);
		return;
	}

	/* Filled on the GPU when it can, else in memory around the boxes. */
	drawn = render_fill_gpu(target, paint, &parts);
	if (!drawn) {
		render_fallback_begin(target, &parts.extents, &view);
		x11_render_boxes(&view, clip, paint, boxes, count);
		render_fallback_end(target, &parts.extents);
	}
	free(parts.boxes);
}

/*
 * Fills spans on an image with a copy on the GPU: each a box one pixel
 * high.
 */
void
x11_render_vulkan_spans(
	struct x11_image *target,
	const struct x11_region *clip,
	const struct x11_paint *paint,
	const struct x11_span *spans,
	size_t count)
{
	struct x11_box *boxes;
	struct x11_image view;
	size_t index;

	/* Nothing to fill. */
	if (count == 0U)
		return;

	/* The spans as boxes; without memory for them, drawn in memory. */
	boxes = malloc(count * sizeof(*boxes));
	if (boxes == NULL) {
		render_fallback_begin(target, NULL, &view);
		x11_render_spans(&view, clip, paint, spans, count);
		render_fallback_end(target, NULL);
		return;
	}
	for (index = 0U; index < count; index++) {
		boxes[index].x0 = spans[index].x0;
		boxes[index].y0 = spans[index].y;
		boxes[index].x1 = spans[index].x1;
		boxes[index].y1 = spans[index].y + 1;
	}

	/* Filled as boxes. */
	x11_render_vulkan_boxes(target, clip, paint, boxes, count);
	free(boxes);
}

/*
 * Fills the set pixels of a bitmap on an image with a copy on the GPU:
 * each row's runs of set pixels are boxes one pixel high.
 */
void
x11_render_vulkan_mask(
	struct x11_image *target,
	const struct x11_region *clip,
	const struct x11_paint *paint,
	int x,
	int y,
	int width,
	int height,
	const uint8_t *bits,
	size_t stride)
{
	struct render_parts runs;
	struct x11_box run;
	struct x11_image view;
	unsigned byte;
	int row;
	int column;
	int start;
	int set;
	int failed;

	/* Each row's runs of set pixels. */
	memset(&runs, 0, sizeof(runs));
	failed = 0;
	for (row = 0; row < height && failed == 0; row++) {
		start = -1;
		for (column = 0; column <= width; column++) {
			/* Whether this pixel is set (past the last, none is). */
			set = 0;
			if (column < width) {
				byte = bits[(size_t)row * stride + (size_t)column / 8U];
				if ((byte & (0x80U >> ((unsigned)column & 7U))) != 0U)
					set = 1;
			}

			/* A run starts, or one ends and is kept. */
			if (set && start < 0) {
				start = column;
			} else if (!set && start >= 0) {
				run.x0 = x + start;
				run.y0 = y + row;
				run.x1 = x + column;
				run.y1 = y + row + 1;
				failed = render_parts_add(&runs, &run);
				start = -1;
			}
		}
	}

	/* Without memory for the runs, drawn in memory. */
	if (failed != 0) {
		free(runs.boxes);
		render_fallback_begin(target, NULL, &view);
		x11_render_mask(&view, clip, paint, x, y, width, height, bits, stride);
		render_fallback_end(target, NULL);
		return;
	}

	/* Filled as boxes. */
	if (runs.count != 0U)
		x11_render_vulkan_boxes(target, clip, paint, runs.boxes, runs.count);
	free(runs.boxes);
}

/*
 * Copies pixels between images when either has a copy on the GPU
 * (render-cpu.c's x11_render_copy for them).  Returns 0, or ENOMEM.
 */
int
x11_render_vulkan_copy(
	struct x11_image *target,
	const struct x11_region *clip,
	int x,
	int y,
	const struct x11_image *source,
	int source_x,
	int source_y,
	unsigned function,
	uint32_t plane_mask)
{
	struct render_parts parts;
	struct x11_image target_view;
	struct x11_image source_view;
	struct x11_box readable;
	struct x11_box limit;
	struct x11_box source_box;
	size_t index;
	int usable;
	int drawn;
	int dx;
	int dy;
	int error;

	/* The part of the target whose source is in the source image, inside the clip. */
	dx = source_x - x;
	dy = source_y - y;
	readable.x0 = -dx;
	readable.y0 = -dy;
	readable.x1 = source->width - dx;
	readable.y1 = source->height - dy;
	memset(&parts, 0, sizeof(parts));
	for (index = 0U; index < clip->count; index++) {
		usable = x11_box_intersect(&clip->boxes[index], &readable, &limit);
		if (!usable)
			continue;
		render_parts_clip(&parts, target, NULL, &limit);
	}
	if (parts.count == 0U) {
		free(parts.boxes);
		return 0;
	}

	/* On the GPU when the target is there and the device can. */
	usable = render_usable(target);
	if (usable) {
		drawn = render_copy_gpu(target, source, dx, dy, function, plane_mask, &parts);
		if (drawn) {
			free(parts.boxes);
			return 0;
		}
	}

	/* Else in memory: the source's part brought up to date, the target's drawn there and put back. */
	source_box = parts.extents;
	source_box.x0 += dx;
	source_box.y0 += dy;
	source_box.x1 += dx;
	source_box.y1 += dy;
	x11_render_image_memory_box(source, &source_box);
	source_view = *source;
	source_view.gpu = NULL;
	render_fallback_begin(target, &parts.extents, &target_view);
	error = x11_render_copy(&target_view, clip, x, y, &source_view, source_x, source_y, function, plane_mask);
	render_fallback_end(target, &parts.extents);
	free(parts.boxes);
	if (error != 0)
		return error;

	/* Succeeded: copied in memory. */
	return 0;
}

/*
 * Puts pixels onto an image with a copy on the GPU (render-cpu.c's
 * x11_render_put for it).
 */
void
x11_render_vulkan_put(
	struct x11_image *target,
	const struct x11_region *clip,
	int x,
	int y,
	int width,
	int height,
	const uint32_t *pixels,
	size_t stride,
	unsigned function,
	uint32_t plane_mask)
{
	struct render_parts parts;
	struct x11_image view;
	struct x11_box rectangle;
	struct x11_box *part;
	size_t index;
	int usable;
	int failed;

	/* The rectangle, clipped. */
	rectangle.x0 = x;
	rectangle.y0 = y;
	rectangle.x1 = x + width;
	rectangle.y1 = y + height;
	memset(&parts, 0, sizeof(parts));
	render_parts_clip(&parts, target, clip, &rectangle);
	if (parts.count == 0U) {
		free(parts.boxes);
		return;
	}

	/* A copy of every plane goes straight from a staging buffer, each part of it. */
	usable = render_usable(target);
	if (usable && function == X11_FUNCTION_COPY && (plane_mask & RENDER_PLANES) == RENDER_PLANES) {
		failed = 0;
		for (index = 0U; index < parts.count && failed == 0; index++) {
			part = &parts.boxes[index];
			failed = render_upload(target->gpu, part->x0, part->y0, part->x1 - part->x0, part->y1 - part->y0, pixels + (size_t)(part->y0 - y) * stride + (size_t)(part->x0 - x), stride);
		}
		if (failed == 0) {
			target->gpu->memory_stale = 1;
			free(parts.boxes);
			return;
		}
	}

	/* Another function or plane mask is drawn in memory around the rectangle. */
	render_fallback_begin(target, &parts.extents, &view);
	x11_render_put(&view, clip, x, y, width, height, pixels, stride, function, plane_mask);
	render_fallback_end(target, &parts.extents);
	free(parts.boxes);
}

/*
 * Reads a rectangle of an image with a copy on the GPU (a pixel outside
 * the image reads as 0).
 */
void
x11_render_vulkan_read(
	const struct x11_image *source,
	int x,
	int y,
	int width,
	int height,
	uint32_t *pixels,
	size_t stride)
{
	struct x11_image view;
	struct x11_box rectangle;
	struct x11_box bounds;
	struct x11_box part;
	int inside;
	int row;
	int failed;

	/* Memory as new as the GPU, or a GPU that cannot be read, is read in memory. */
	view = *source;
	view.gpu = NULL;
	if (!source->gpu->memory_stale || source->gpu->vulkan->lost) {
		x11_render_read(&view, x, y, width, height, pixels, stride);
		return;
	}

	/* Every pixel 0, then the part inside the image read back over it. */
	for (row = 0; row < height; row++)
		memset(pixels + (size_t)row * stride, 0, (size_t)width * sizeof(*pixels));
	rectangle.x0 = x;
	rectangle.y0 = y;
	rectangle.x1 = x + width;
	rectangle.y1 = y + height;
	bounds.x0 = 0;
	bounds.y0 = 0;
	bounds.x1 = source->width;
	bounds.y1 = source->height;
	inside = x11_box_intersect(&rectangle, &bounds, &part);
	if (!inside)
		return;
	failed = render_download(source->gpu, part.x0, part.y0, part.x1 - part.x0, part.y1 - part.y0, pixels + (size_t)(part.y0 - y) * stride + (size_t)(part.x0 - x), stride);

	/* A read back that failed reads what memory has. */
	if (failed != 0)
		x11_render_read(&view, x, y, width, height, pixels, stride);
}

/*
 * Gives the Vulkan image of an image's copy on the GPU (VK_NULL_HANDLE:
 * none).
 */
VkImage
x11_render_vulkan_image(
	const struct x11_image *image)
{
	/* No copy on the GPU. */
	if (image->gpu == NULL)
		return VK_NULL_HANDLE;

	/* Succeeded: the image, in VK_IMAGE_LAYOUT_GENERAL. */
	return image->gpu->image;
}

/*
 * Gives the command buffer being recorded, for a swapchain's frame to be
 * recorded after the drawing; everything drawn before is ordered before
 * what is recorded now.  Returns VK_NULL_HANDLE when the device failed.
 */
VkCommandBuffer
x11_render_vulkan_command(
	struct x11_vulkan *vulkan)
{
	VkCommandBuffer command;

	/* The buffer being recorded. */
	command = render_begin(vulkan);
	if (command == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;

	/* Succeeded: a barrier first, so the frame sees all that was drawn and what comes after waits for it. */
	render_barrier(vulkan, command);
	return command;
}

/*
 * Submits the command buffer being recorded, waiting for a semaphore and
 * signalling one (VK_NULL_HANDLE: none); the next operation records into
 * the next buffer.
 */
VkResult
x11_render_vulkan_submit(
	struct x11_vulkan *vulkan,
	VkSemaphore wait,
	VkSemaphore signal)
{
	struct x11_render_vulkan *render;
	struct render_slot *slot;
	VkPipelineStageFlags stage;
	VkSubmitInfo submit;
	VkResult error;

	/* Nothing recorded. */
	render = vulkan->render;
	if (!render->recording)
		return VK_SUCCESS;
	slot = &render->slots[render->current];

	/* The recording ends, and the buffer goes in the next turn. */
	render->recording = 0;
	render->current = (render->current + 1U) % RENDER_SLOTS;
	error = vulkan->vk.vkEndCommandBuffer(slot->command);
	if (error != VK_SUCCESS) {
		x11_vulkan_failed(vulkan, error, "vkEndCommandBuffer");
		return error;
	}

	/* Submitted with its fence. */
	stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	if (wait != VK_NULL_HANDLE) {
		submit.waitSemaphoreCount = 1U;
		submit.pWaitSemaphores = &wait;
		submit.pWaitDstStageMask = &stage;
	}
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &slot->command;
	if (signal != VK_NULL_HANDLE) {
		submit.signalSemaphoreCount = 1U;
		submit.pSignalSemaphores = &signal;
	}
	error = vulkan->vk.vkQueueSubmit(vulkan->queue, 1U, &submit, slot->fence);
	if (error != VK_SUCCESS) {
		x11_vulkan_failed(vulkan, error, "vkQueueSubmit");
		return error;
	}

	/* Succeeded: the device has it; the slot is used again after its fence. */
	slot->submitted = 1;
	return VK_SUCCESS;
}

/* Makes one command buffer, its fence, staging buffer and descriptor pool. */
static VkResult
render_slot_make(
	struct x11_vulkan *vulkan,
	struct render_slot *slot)
{
	VkCommandBufferAllocateInfo command;
	VkFenceCreateInfo fence;
	VkDescriptorPoolCreateInfo pool;
	VkDescriptorPoolSize size;
	VkResult error;

	/* The command buffer. */
	memset(&command, 0, sizeof(command));
	command.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command.commandPool = vulkan->render->pool;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command.commandBufferCount = 1U;
	error = vulkan->vk.vkAllocateCommandBuffers(vulkan->device, &command, &slot->command);
	if (error != VK_SUCCESS)
		return error;

	/* Its fence (unsignalled: it is waited on only after a submit). */
	memset(&fence, 0, sizeof(fence));
	fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	error = vulkan->vk.vkCreateFence(vulkan->device, &fence, NULL, &slot->fence);
	if (error != VK_SUCCESS)
		return error;

	/* The descriptor sets of its draws (one pattern each). */
	size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	size.descriptorCount = RENDER_DESCRIPTORS;
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.maxSets = RENDER_DESCRIPTORS;
	pool.poolSizeCount = 1U;
	pool.pPoolSizes = &size;
	error = vulkan->vk.vkCreateDescriptorPool(vulkan->device, &pool, NULL, &slot->descriptors);
	if (error != VK_SUCCESS)
		return error;

	/* Its staging buffer. */
	error = render_slot_staging(vulkan, slot, RENDER_STAGING_SIZE);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the slot can be recorded. */
	return VK_SUCCESS;
}

/*
 * Makes a command buffer's staging buffer of a size in memory the host
 * sees and keeps coherent, mapped for good, replacing the one it had
 * (which no submitted command uses any more).
 */
static VkResult
render_slot_staging(
	struct x11_vulkan *vulkan,
	struct render_slot *slot,
	VkDeviceSize size)
{
	VkMemoryRequirements requirements;
	VkBufferCreateInfo create;
	VkMemoryAllocateInfo allocate;
	VkDevice device;
	uint32_t type;
	void *mapped;
	VkResult error;

	/* The old buffer goes. */
	device = vulkan->device;
	if (slot->staging != VK_NULL_HANDLE)
		vulkan->vk.vkDestroyBuffer(device, slot->staging, NULL);
	if (slot->staging_memory != VK_NULL_HANDLE)
		vulkan->vk.vkFreeMemory(device, slot->staging_memory, NULL);
	slot->staging = VK_NULL_HANDLE;
	slot->staging_memory = VK_NULL_HANDLE;
	slot->staging_bytes = NULL;
	slot->staging_size = 0U;
	slot->staging_used = 0U;

	/* The buffer: uploads come from it and boxes are drawn from it. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	create.size = size;
	create.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	error = vulkan->vk.vkCreateBuffer(device, &create, NULL, &slot->staging);
	if (error != VK_SUCCESS)
		return error;

	/* Memory the host writes without flushing. */
	vulkan->vk.vkGetBufferMemoryRequirements(device, slot->staging, &requirements);
	type = x11_vulkan_memory_type(vulkan, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (type == UINT32_MAX)
		return VK_ERROR_FEATURE_NOT_PRESENT;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	error = vulkan->vk.vkAllocateMemory(device, &allocate, NULL, &slot->staging_memory);
	if (error != VK_SUCCESS)
		return error;

	/* Bound and mapped. */
	error = vulkan->vk.vkBindBufferMemory(device, slot->staging, slot->staging_memory, 0U);
	if (error != VK_SUCCESS)
		return error;
	error = vulkan->vk.vkMapMemory(device, slot->staging_memory, 0U, VK_WHOLE_SIZE, 0U, &mapped);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: the staging buffer is empty. */
	slot->staging_bytes = mapped;
	slot->staging_size = size;
	return VK_SUCCESS;
}

/* Destroys one command buffer's fence, staging buffer and descriptor pool (the buffer goes with the pool). */
static void
render_slot_free(
	struct x11_vulkan *vulkan,
	struct render_slot *slot)
{
	VkDevice device;

	/* The staging buffer and its memory (unmapped with it). */
	device = vulkan->device;
	if (slot->staging != VK_NULL_HANDLE)
		vulkan->vk.vkDestroyBuffer(device, slot->staging, NULL);
	if (slot->staging_memory != VK_NULL_HANDLE)
		vulkan->vk.vkFreeMemory(device, slot->staging_memory, NULL);
	slot->staging = VK_NULL_HANDLE;
	slot->staging_memory = VK_NULL_HANDLE;
	slot->staging_bytes = NULL;

	/* The descriptor pool and the fence. */
	if (slot->descriptors != VK_NULL_HANDLE)
		vulkan->vk.vkDestroyDescriptorPool(device, slot->descriptors, NULL);
	if (slot->fence != VK_NULL_HANDLE)
		vulkan->vk.vkDestroyFence(device, slot->fence, NULL);
	free(slot->retired);
	slot->retired = NULL;
}

/* Destroys the images that went while a command buffer was recorded (its fence has passed). */
static void
render_slot_release(
	struct x11_vulkan *vulkan,
	struct render_slot *slot)
{
	size_t index;

	/* Each image. */
	for (index = 0U; index < slot->retired_count; index++)
		render_image_destroy(vulkan, slot->retired[index]);
	slot->retired_count = 0U;
}

/* Makes the render pass, the descriptor set and pipeline layouts, the shaders and the sampler the pipelines share. */
static VkResult
render_pipeline_objects(
	struct x11_vulkan *vulkan)
{
	struct x11_render_vulkan *render;
	VkAttachmentDescription attachment;
	VkAttachmentReference reference;
	VkSubpassDescription subpass;
	VkRenderPassCreateInfo pass;
	VkDescriptorSetLayoutBinding binding;
	VkDescriptorSetLayoutCreateInfo set_layout;
	VkPushConstantRange range;
	VkPipelineLayoutCreateInfo layout;
	VkShaderModuleCreateInfo module;
	VkSamplerCreateInfo sampler;
	VkDevice device;
	VkResult error;

	/* One colour attachment, kept in VK_IMAGE_LAYOUT_GENERAL, whose pixels are kept and drawn over. */
	render = vulkan->render;
	device = vulkan->device;
	memset(&attachment, 0, sizeof(attachment));
	attachment.format = VK_FORMAT_B8G8R8A8_UNORM;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
	attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
	reference.attachment = 0U;
	reference.layout = VK_IMAGE_LAYOUT_GENERAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1U;
	subpass.pColorAttachments = &reference;
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	pass.attachmentCount = 1U;
	pass.pAttachments = &attachment;
	pass.subpassCount = 1U;
	pass.pSubpasses = &subpass;
	error = vulkan->vk.vkCreateRenderPass(device, &pass, NULL, &render->render_pass);
	if (error != VK_SUCCESS)
		return error;

	/* One pattern (a tile, a stipple or a copy's source) the fragment shader reads. */
	memset(&binding, 0, sizeof(binding));
	binding.binding = 0U;
	binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	binding.descriptorCount = 1U;
	binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	memset(&set_layout, 0, sizeof(set_layout));
	set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout.bindingCount = 1U;
	set_layout.pBindings = &binding;
	error = vulkan->vk.vkCreateDescriptorSetLayout(device, &set_layout, NULL, &render->set_layout);
	if (error != VK_SUCCESS)
		return error;

	/* The pattern and the push constants both shaders take. */
	range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	range.offset = 0U;
	range.size = (uint32_t)sizeof(struct render_push);
	memset(&layout, 0, sizeof(layout));
	layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout.setLayoutCount = 1U;
	layout.pSetLayouts = &render->set_layout;
	layout.pushConstantRangeCount = 1U;
	layout.pPushConstantRanges = &range;
	error = vulkan->vk.vkCreatePipelineLayout(device, &layout, NULL, &render->layout);
	if (error != VK_SUCCESS)
		return error;

	/* The vertex shader. */
	memset(&module, 0, sizeof(module));
	module.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module.codeSize = sizeof(x11_render_paint_vert);
	module.pCode = x11_render_paint_vert;
	error = vulkan->vk.vkCreateShaderModule(device, &module, NULL, &render->vertex);
	if (error != VK_SUCCESS)
		return error;

	/* The fragment shader. */
	module.codeSize = sizeof(x11_render_paint_frag);
	module.pCode = x11_render_paint_frag;
	error = vulkan->vk.vkCreateShaderModule(device, &module, NULL, &render->fragment);
	if (error != VK_SUCCESS)
		return error;

	/* The sampler of the pattern (texelFetch reads texels as they are). */
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = VK_FILTER_NEAREST;
	sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.maxLod = 0.0f;
	error = vulkan->vk.vkCreateSampler(device, &sampler, NULL, &render->sampler);
	if (error != VK_SUCCESS)
		return error;

	/* Succeeded: pipelines can be made. */
	return VK_SUCCESS;
}

/*
 * Gives the pipeline that joins the source with the target by an X
 * function (whose numbers are Vulkan's logic operations) and writes the
 * colour channels of a mask, made the first time it is asked for.
 * Returns VK_NULL_HANDLE when it cannot be made.
 */
static VkPipeline
render_pipeline(
	struct x11_vulkan *vulkan,
	unsigned function,
	unsigned write)
{
	struct x11_render_vulkan *render;
	VkPipelineShaderStageCreateInfo stages[2];
	VkVertexInputBindingDescription binding;
	VkVertexInputAttributeDescription attribute;
	VkPipelineVertexInputStateCreateInfo input;
	VkPipelineInputAssemblyStateCreateInfo assembly;
	VkPipelineViewportStateCreateInfo viewport;
	VkPipelineRasterizationStateCreateInfo raster;
	VkPipelineMultisampleStateCreateInfo multisample;
	VkPipelineColorBlendAttachmentState blend;
	VkPipelineColorBlendStateCreateInfo blending;
	VkDynamicState dynamic_states[2];
	VkPipelineDynamicStateCreateInfo dynamic;
	VkGraphicsPipelineCreateInfo create;
	VkPipeline pipeline;
	VkResult error;

	/* Made before. */
	render = vulkan->render;
	pipeline = render->pipelines[function][write];
	if (pipeline != VK_NULL_HANDLE)
		return pipeline;

	/* The two shaders. */
	memset(stages, 0, sizeof(stages));
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = render->vertex;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = render->fragment;
	stages[1].pName = "main";

	/* One box (x0, y0, x1, y1) an instance, six vertices of two triangles each. */
	binding.binding = 0U;
	binding.stride = (uint32_t)(4U * sizeof(int32_t));
	binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
	attribute.location = 0U;
	attribute.binding = 0U;
	attribute.format = VK_FORMAT_R32G32B32A32_SINT;
	attribute.offset = 0U;
	memset(&input, 0, sizeof(input));
	input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	input.vertexBindingDescriptionCount = 1U;
	input.pVertexBindingDescriptions = &binding;
	input.vertexAttributeDescriptionCount = 1U;
	input.pVertexAttributeDescriptions = &attribute;
	memset(&assembly, 0, sizeof(assembly));
	assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	/* The viewport and scissor are the target's, set when drawing. */
	memset(&viewport, 0, sizeof(viewport));
	viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	viewport.viewportCount = 1U;
	viewport.scissorCount = 1U;
	dynamic_states[0] = VK_DYNAMIC_STATE_VIEWPORT;
	dynamic_states[1] = VK_DYNAMIC_STATE_SCISSOR;
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamic.dynamicStateCount = 2U;
	dynamic.pDynamicStates = dynamic_states;

	/* Filled triangles, one sample a pixel. */
	memset(&raster, 0, sizeof(raster));
	raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	raster.polygonMode = VK_POLYGON_MODE_FILL;
	raster.cullMode = VK_CULL_MODE_NONE;
	raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	raster.lineWidth = 1.0f;
	memset(&multisample, 0, sizeof(multisample));
	multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	/* The channels the plane mask names, joined by the function (copy needs no logic operation). */
	memset(&blend, 0, sizeof(blend));
	blend.blendEnable = VK_FALSE;
	if ((write & 4U) != 0U)
		blend.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
	if ((write & 2U) != 0U)
		blend.colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
	if ((write & 1U) != 0U)
		blend.colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
	memset(&blending, 0, sizeof(blending));
	blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	blending.logicOpEnable = VK_FALSE;
	blending.logicOp = VK_LOGIC_OP_COPY;
	if (function != X11_FUNCTION_COPY) {
		blending.logicOpEnable = VK_TRUE;
		blending.logicOp = (VkLogicOp)function;
	}
	blending.attachmentCount = 1U;
	blending.pAttachments = &blend;

	/* The pipeline. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	create.stageCount = 2U;
	create.pStages = stages;
	create.pVertexInputState = &input;
	create.pInputAssemblyState = &assembly;
	create.pViewportState = &viewport;
	create.pRasterizationState = &raster;
	create.pMultisampleState = &multisample;
	create.pColorBlendState = &blending;
	create.pDynamicState = &dynamic;
	create.layout = render->layout;
	create.renderPass = render->render_pass;
	error = vulkan->vk.vkCreateGraphicsPipelines(vulkan->device, VK_NULL_HANDLE, 1U, &create, NULL, &pipeline);
	if (error != VK_SUCCESS) {
		x11_log(vulkan->server, "X11SERVER RENDER pipeline function=%u write=%u failed: %d", function, write, (int)error);
		return VK_NULL_HANDLE;
	}

	/* Succeeded: kept for the next time. */
	render->pipelines[function][write] = pipeline;
	return pipeline;
}

/*
 * Gives the command buffer being recorded, starting the next one when
 * none is (after its fence: its staging memory, descriptor sets and the
 * images that went with it are free then).  Returns VK_NULL_HANDLE when
 * the device failed.
 */
static VkCommandBuffer
render_begin(
	struct x11_vulkan *vulkan)
{
	struct x11_render_vulkan *render;
	struct render_slot *slot;
	VkCommandBufferBeginInfo begin;
	VkResult error;

	/* A device that failed records nothing. */
	render = vulkan->render;
	if (vulkan->lost || render == NULL)
		return VK_NULL_HANDLE;

	/* The buffer being recorded. */
	slot = &render->slots[render->current];
	if (render->recording)
		return slot->command;

	/* The device is done with the buffer's last submission. */
	if (slot->submitted) {
		error = vulkan->vk.vkWaitForFences(vulkan->device, 1U, &slot->fence, VK_TRUE, RENDER_WAIT_NS);
		if (error != VK_SUCCESS) {
			x11_vulkan_failed(vulkan, error, "vkWaitForFences");
			return VK_NULL_HANDLE;
		}
		error = vulkan->vk.vkResetFences(vulkan->device, 1U, &slot->fence);
		if (error != VK_SUCCESS) {
			x11_vulkan_failed(vulkan, error, "vkResetFences");
			return VK_NULL_HANDLE;
		}
		slot->submitted = 0;
	}

	/* What it used is free: the images that went, its descriptor sets and staging memory. */
	render_slot_release(vulkan, slot);
	error = vulkan->vk.vkResetDescriptorPool(vulkan->device, slot->descriptors, 0U);
	if (error != VK_SUCCESS) {
		x11_vulkan_failed(vulkan, error, "vkResetDescriptorPool");
		return VK_NULL_HANDLE;
	}
	slot->descriptors_used = 0U;
	slot->staging_used = 0U;

	/* Recorded afresh. */
	error = vulkan->vk.vkResetCommandBuffer(slot->command, 0U);
	if (error != VK_SUCCESS) {
		x11_vulkan_failed(vulkan, error, "vkResetCommandBuffer");
		return VK_NULL_HANDLE;
	}
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	error = vulkan->vk.vkBeginCommandBuffer(slot->command, &begin);
	if (error != VK_SUCCESS) {
		x11_vulkan_failed(vulkan, error, "vkBeginCommandBuffer");
		return VK_NULL_HANDLE;
	}
	render->recording = 1;

	/* Succeeded: after a barrier, so this buffer's work waits for what was submitted before. */
	render_barrier(vulkan, slot->command);
	return slot->command;
}

/*
 * Submits what is recorded and waits for the device to finish it.
 * Returns VK_SUCCESS, or the error (the device is then given up).
 */
static VkResult
render_wait(
	struct x11_vulkan *vulkan)
{
	struct x11_render_vulkan *render;
	struct render_slot *slot;
	VkResult error;

	/* The buffer being recorded, submitted. */
	render = vulkan->render;
	slot = &render->slots[render->current];
	if (render->recording) {
		error = x11_render_vulkan_submit(vulkan, VK_NULL_HANDLE, VK_NULL_HANDLE);
		if (error != VK_SUCCESS)
			return error;
	}

	/* Nothing submitted to wait for. */
	if (!slot->submitted)
		return VK_SUCCESS;

	/* Its fence. */
	error = vulkan->vk.vkWaitForFences(vulkan->device, 1U, &slot->fence, VK_TRUE, RENDER_WAIT_NS);
	if (error != VK_SUCCESS) {
		x11_vulkan_failed(vulkan, error, "vkWaitForFences");
		return error;
	}

	/* Succeeded: the device has done all it was given. */
	return VK_SUCCESS;
}

/*
 * Records a barrier between everything recorded before and everything
 * after, and starts a new generation: no image is touched in it yet.
 */
static void
render_barrier(
	struct x11_vulkan *vulkan,
	VkCommandBuffer command)
{
	VkMemoryBarrier barrier;

	/* Every write before is seen by every access after. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	vulkan->vk.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0U, 1U, &barrier, 0U, NULL, 0U, NULL);

	/* The generation the accesses after it count in. */
	vulkan->render->generation++;
}

/*
 * Records a barrier before an operation when it needs one (it reads what
 * an earlier operation wrote, or writes what one read, or a box one
 * wrote), then counts its accesses.
 */
static void
render_accesses(
	struct x11_vulkan *vulkan,
	VkCommandBuffer command,
	const struct render_access *accesses,
	unsigned count)
{
	struct x11_gpu_image *image;
	struct x11_box overlap;
	uint64_t generation;
	unsigned index;
	int needed;
	int overlaps;

	/* Whether any access meets an earlier one since the last barrier. */
	generation = vulkan->render->generation;
	needed = 0;
	for (index = 0U; index < count && !needed; index++) {
		image = accesses[index].image;
		if (image->touched != generation)
			continue;
		if (image->written && !accesses[index].write) {
			/* A read of what was written. */
			needed = 1;
		} else if (image->read && accesses[index].write) {
			/* A write over what was read. */
			needed = 1;
		} else if (image->written && accesses[index].write) {
			/* A write over what was written: only where the boxes meet. */
			overlaps = x11_box_intersect(&image->written_box, &accesses[index].box, &overlap);
			if (overlaps)
				needed = 1;
		}
	}

	/* The barrier, starting a new generation. */
	if (needed)
		render_barrier(vulkan, command);

	/* Each access counted in the generation. */
	generation = vulkan->render->generation;
	for (index = 0U; index < count; index++) {
		image = accesses[index].image;
		if (image->touched != generation) {
			image->touched = generation;
			image->read = 0;
			image->written = 0;
		}

		/* A read, or a write whose box grows the box written. */
		if (!accesses[index].write) {
			image->read = 1;
			continue;
		}
		if (!image->written) {
			image->written_box = accesses[index].box;
			image->written = 1;
			continue;
		}
		if (accesses[index].box.x0 < image->written_box.x0)
			image->written_box.x0 = accesses[index].box.x0;
		if (accesses[index].box.y0 < image->written_box.y0)
			image->written_box.y0 = accesses[index].box.y0;
		if (accesses[index].box.x1 > image->written_box.x1)
			image->written_box.x1 = accesses[index].box.x1;
		if (accesses[index].box.y1 > image->written_box.y1)
			image->written_box.y1 = accesses[index].box.y1;
	}
}

/*
 * Gives room in the recorded buffer's staging memory and a descriptor
 * set for an operation, submitting the buffer and starting the next when
 * it has not enough (a fresh buffer's staging grows to the size).
 * Returns the bytes (16-byte aligned) with the buffer and its offset, or
 * NULL when the device failed.
 */
static void *
render_staging(
	struct x11_vulkan *vulkan,
	VkDeviceSize size,
	VkBuffer *buffer,
	VkDeviceSize *offset)
{
	struct x11_render_vulkan *render;
	struct render_slot *slot;
	VkCommandBuffer command;
	VkDeviceSize start;
	VkDeviceSize grown;
	VkResult error;

	/* The buffer being recorded. */
	render = vulkan->render;
	command = render_begin(vulkan);
	if (command == VK_NULL_HANDLE)
		return NULL;
	slot = &render->slots[render->current];

	/* A buffer that has used some of its room or descriptor sets goes to the device; the next one starts. */
	start = (slot->staging_used + 15U) & ~(VkDeviceSize)15U;
	if ((start + size > slot->staging_size || slot->descriptors_used >= RENDER_DESCRIPTORS) && slot->staging_used != 0U) {
		error = x11_render_vulkan_submit(vulkan, VK_NULL_HANDLE, VK_NULL_HANDLE);
		if (error != VK_SUCCESS)
			return NULL;
		command = render_begin(vulkan);
		if (command == VK_NULL_HANDLE)
			return NULL;
		slot = &render->slots[render->current];
		start = 0U;
	}

	/* A fresh buffer too small grows (nothing recorded uses its staging yet). */
	if (start + size > slot->staging_size) {
		grown = slot->staging_size;
		while (grown < size)
			grown *= 2U;
		error = render_slot_staging(vulkan, slot, grown);
		if (error != VK_SUCCESS) {
			x11_vulkan_failed(vulkan, error, "staging");
			return NULL;
		}
		start = 0U;
	}

	/* Succeeded: the room. */
	slot->staging_used = start + size;
	*buffer = slot->staging;
	*offset = start;
	return slot->staging_bytes + start;
}

/*
 * Makes an image on the device of a size, B8G8R8A8_UNORM, drawn into,
 * sampled and copied, in VK_IMAGE_LAYOUT_GENERAL from the first command
 * recorded after.  Returns NULL when it cannot be made.
 */
static struct x11_gpu_image *
render_image_make(
	struct x11_vulkan *vulkan,
	unsigned kind,
	int width,
	int height)
{
	struct x11_gpu_image *image;
	VkImageCreateInfo create;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	VkImageViewCreateInfo view;
	VkImageMemoryBarrier barrier;
	VkCommandBuffer command;
	uint32_t type;
	VkResult error;

	/* The record. */
	image = calloc(1U, sizeof(*image));
	if (image == NULL)
		return NULL;
	image->vulkan = vulkan;
	image->kind = kind;
	image->width = width;
	image->height = height;

	/* The image. */
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.imageType = VK_IMAGE_TYPE_2D;
	create.format = VK_FORMAT_B8G8R8A8_UNORM;
	create.extent.width = (uint32_t)width;
	create.extent.height = (uint32_t)height;
	create.extent.depth = 1U;
	create.mipLevels = 1U;
	create.arrayLayers = 1U;
	create.samples = VK_SAMPLE_COUNT_1_BIT;
	create.tiling = VK_IMAGE_TILING_OPTIMAL;
	create.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	error = vulkan->vk.vkCreateImage(vulkan->device, &create, NULL, &image->image);
	if (error != VK_SUCCESS) {
		render_image_destroy(vulkan, image);
		return NULL;
	}

	/* Its memory: the device's own when it has some, else any it may use. */
	vulkan->vk.vkGetImageMemoryRequirements(vulkan->device, image->image, &requirements);
	type = x11_vulkan_memory_type(vulkan, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type == UINT32_MAX)
		type = x11_vulkan_memory_type(vulkan, requirements.memoryTypeBits, 0U);
	if (type == UINT32_MAX) {
		render_image_destroy(vulkan, image);
		return NULL;
	}
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	error = vulkan->vk.vkAllocateMemory(vulkan->device, &allocate, NULL, &image->memory);
	if (error != VK_SUCCESS) {
		image->memory = VK_NULL_HANDLE;
		render_image_destroy(vulkan, image);
		return NULL;
	}
	error = vulkan->vk.vkBindImageMemory(vulkan->device, image->image, image->memory, 0U);
	if (error != VK_SUCCESS) {
		render_image_destroy(vulkan, image);
		return NULL;
	}

	/* Its view. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = image->image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_B8G8R8A8_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	error = vulkan->vk.vkCreateImageView(vulkan->device, &view, NULL, &image->view);
	if (error != VK_SUCCESS) {
		image->view = VK_NULL_HANDLE;
		render_image_destroy(vulkan, image);
		return NULL;
	}

	/* Into the layout it keeps. */
	command = render_begin(vulkan);
	if (command == VK_NULL_HANDLE) {
		render_image_destroy(vulkan, image);
		return NULL;
	}
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = 0U;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image->image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1U;
	barrier.subresourceRange.layerCount = 1U;
	vulkan->vk.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);

	/* Succeeded: the image. */
	return image;
}

/* Destroys an image on the device that no command uses any more. */
static void
render_image_destroy(
	struct x11_vulkan *vulkan,
	struct x11_gpu_image *image)
{
	VkDevice device;

	/* Its framebuffer and view. */
	device = vulkan->device;
	if (image->framebuffer != VK_NULL_HANDLE)
		vulkan->vk.vkDestroyFramebuffer(device, image->framebuffer, NULL);
	if (image->view != VK_NULL_HANDLE)
		vulkan->vk.vkDestroyImageView(device, image->view, NULL);

	/* The image and its memory. */
	if (image->image != VK_NULL_HANDLE)
		vulkan->vk.vkDestroyImage(device, image->image, NULL);
	if (image->memory != VK_NULL_HANDLE)
		vulkan->vk.vkFreeMemory(device, image->memory, NULL);
	free(image);
}

/*
 * Lets an image on the device go: destroyed when the command buffer
 * being recorded has been done (or now, when the device is done with
 * everything).
 */
static void
render_image_retire(
	struct x11_gpu_image *image)
{
	struct x11_vulkan *vulkan;
	struct render_slot *slot;
	struct x11_gpu_image **grown;
	VkCommandBuffer command;
	size_t capacity;

	/* The buffer being recorded (none when the device failed: nothing more runs on it). */
	vulkan = image->vulkan;
	command = render_begin(vulkan);
	if (command == VK_NULL_HANDLE) {
		(void)vulkan->vk.vkDeviceWaitIdle(vulkan->device);
		render_image_destroy(vulkan, image);
		return;
	}
	slot = &vulkan->render->slots[vulkan->render->current];

	/* Room in its list; without memory for it, everything is waited for and the image goes now. */
	if (slot->retired_count == slot->retired_capacity) {
		capacity = slot->retired_capacity * 2U;
		if (capacity == 0U)
			capacity = 16U;
		grown = realloc(slot->retired, capacity * sizeof(*grown));
		if (grown == NULL) {
			(void)render_wait(vulkan);
			(void)vulkan->vk.vkDeviceWaitIdle(vulkan->device);
			render_image_destroy(vulkan, image);
			return;
		}
		slot->retired = grown;
		slot->retired_capacity = capacity;
	}

	/* Succeeded: it goes after the buffer's fence. */
	slot->retired[slot->retired_count] = image;
	slot->retired_count++;
}

/* Makes the framebuffer an image is drawn through, the first time. */
static VkResult
render_framebuffer(
	struct x11_gpu_image *image)
{
	struct x11_vulkan *vulkan;
	VkFramebufferCreateInfo create;
	VkResult error;

	/* Made before. */
	if (image->framebuffer != VK_NULL_HANDLE)
		return VK_SUCCESS;

	/* The image as the render pass's one attachment. */
	vulkan = image->vulkan;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	create.renderPass = vulkan->render->render_pass;
	create.attachmentCount = 1U;
	create.pAttachments = &image->view;
	create.width = (uint32_t)image->width;
	create.height = (uint32_t)image->height;
	create.layers = 1U;
	error = vulkan->vk.vkCreateFramebuffer(vulkan->device, &create, NULL, &image->framebuffer);
	if (error != VK_SUCCESS) {
		image->framebuffer = VK_NULL_HANDLE;
		return error;
	}

	/* Succeeded: drawable. */
	return VK_SUCCESS;
}

/*
 * Records the upload of a rectangle of pixels (rows stride pixels apart)
 * into an image at (x, y), made opaque.  Returns 0, or EIO.
 */
static int
render_upload(
	struct x11_gpu_image *image,
	int x,
	int y,
	int width,
	int height,
	const uint32_t *pixels,
	size_t stride)
{
	struct x11_vulkan *vulkan;
	struct render_access access;
	VkBufferImageCopy region;
	VkCommandBuffer command;
	VkBuffer buffer;
	VkDeviceSize offset;
	uint32_t *target;
	const uint32_t *line;
	int row;
	int column;

	/* Nothing to upload. */
	vulkan = image->vulkan;
	if (width <= 0 || height <= 0)
		return 0;

	/* The pixels into staging memory, opaque. */
	target = render_staging(vulkan, (VkDeviceSize)width * (VkDeviceSize)height * sizeof(uint32_t), &buffer, &offset);
	if (target == NULL)
		return EIO;
	for (row = 0; row < height; row++) {
		line = pixels + (size_t)row * stride;
		for (column = 0; column < width; column++)
			target[(size_t)row * (size_t)width + (size_t)column] = line[column] | RENDER_OPAQUE;
	}

	/* The copy into the image, after what it must wait for. */
	command = render_begin(vulkan);
	if (command == VK_NULL_HANDLE)
		return EIO;
	access.image = image;
	access.write = 1;
	access.box.x0 = x;
	access.box.y0 = y;
	access.box.x1 = x + width;
	access.box.y1 = y + height;
	render_accesses(vulkan, command, &access, 1U);
	memset(&region, 0, sizeof(region));
	region.bufferOffset = offset;
	region.bufferRowLength = (uint32_t)width;
	region.bufferImageHeight = (uint32_t)height;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1U;
	region.imageOffset.x = x;
	region.imageOffset.y = y;
	region.imageExtent.width = (uint32_t)width;
	region.imageExtent.height = (uint32_t)height;
	region.imageExtent.depth = 1U;
	vulkan->vk.vkCmdCopyBufferToImage(command, buffer, image->image, VK_IMAGE_LAYOUT_GENERAL, 1U, &region);

	/* Succeeded: recorded. */
	return 0;
}

/*
 * Reads a rectangle of an image back into pixels (rows stride pixels
 * apart, the alpha dropped), waiting for everything recorded before.
 * Returns 0, or EIO.
 */
static int
render_download(
	struct x11_gpu_image *image,
	int x,
	int y,
	int width,
	int height,
	uint32_t *pixels,
	size_t stride)
{
	struct x11_vulkan *vulkan;
	struct x11_render_vulkan *render;
	struct render_access access;
	VkBufferImageCopy region;
	VkMappedMemoryRange range;
	VkBufferMemoryBarrier barrier;
	VkCommandBuffer command;
	const uint32_t *source;
	VkResult error;
	int failed;
	int row;
	int column;

	/* Nothing to read. */
	vulkan = image->vulkan;
	render = vulkan->render;
	if (width <= 0 || height <= 0)
		return 0;

	/* Room for the pixels. */
	failed = render_readback_room(vulkan, (VkDeviceSize)width * (VkDeviceSize)height * sizeof(uint32_t));
	if (failed != 0)
		return failed;

	/* The copy into the buffer after what was drawn, and the buffer then made visible to the host. */
	command = render_begin(vulkan);
	if (command == VK_NULL_HANDLE)
		return EIO;
	access.image = image;
	access.write = 0;
	access.box.x0 = x;
	access.box.y0 = y;
	access.box.x1 = x + width;
	access.box.y1 = y + height;
	render_accesses(vulkan, command, &access, 1U);
	memset(&region, 0, sizeof(region));
	region.bufferRowLength = (uint32_t)width;
	region.bufferImageHeight = (uint32_t)height;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1U;
	region.imageOffset.x = x;
	region.imageOffset.y = y;
	region.imageExtent.width = (uint32_t)width;
	region.imageExtent.height = (uint32_t)height;
	region.imageExtent.depth = 1U;
	vulkan->vk.vkCmdCopyImageToBuffer(command, image->image, VK_IMAGE_LAYOUT_GENERAL, render->readback, 1U, &region);
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.buffer = render->readback;
	barrier.size = VK_WHOLE_SIZE;
	vulkan->vk.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0U, 0U, NULL, 1U, &barrier, 0U, NULL);

	/* Done on the device. */
	error = render_wait(vulkan);
	if (error != VK_SUCCESS)
		return EIO;
	render->readbacks++;

	/* Memory the host does not keep coherent is invalidated first. */
	if (!render->readback_coherent) {
		memset(&range, 0, sizeof(range));
		range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
		range.memory = render->readback_memory;
		range.size = VK_WHOLE_SIZE;
		error = vulkan->vk.vkInvalidateMappedMemoryRanges(vulkan->device, 1U, &range);
		if (error != VK_SUCCESS)
			return EIO;
	}

	/* Each pixel without its alpha. */
	source = (const uint32_t *)render->readback_bytes;
	for (row = 0; row < height; row++) {
		for (column = 0; column < width; column++)
			pixels[(size_t)row * stride + (size_t)column] = source[(size_t)row * (size_t)width + (size_t)column] & RENDER_PLANES;
	}

	/* Succeeded: read. */
	return 0;
}

/*
 * Makes the buffer read backs land in at least a size, in memory the
 * host sees (cached when it can be).  Returns 0, or EIO.
 */
static int
render_readback_room(
	struct x11_vulkan *vulkan,
	VkDeviceSize size)
{
	struct x11_render_vulkan *render;
	VkMemoryRequirements requirements;
	VkBufferCreateInfo create;
	VkMemoryAllocateInfo allocate;
	VkDeviceSize grown;
	uint32_t type;
	void *mapped;
	VkResult error;

	/* Big enough already. */
	render = vulkan->render;
	if (render->readback != VK_NULL_HANDLE && render->readback_size >= size)
		return 0;

	/* The old buffer goes after everything that may use it. */
	error = render_wait(vulkan);
	if (error != VK_SUCCESS)
		return EIO;
	if (render->readback != VK_NULL_HANDLE)
		vulkan->vk.vkDestroyBuffer(vulkan->device, render->readback, NULL);
	if (render->readback_memory != VK_NULL_HANDLE)
		vulkan->vk.vkFreeMemory(vulkan->device, render->readback_memory, NULL);
	render->readback = VK_NULL_HANDLE;
	render->readback_memory = VK_NULL_HANDLE;
	render->readback_bytes = NULL;
	render->readback_size = 0U;

	/* A buffer of the size in whole mebibytes. */
	grown = (size + 0xfffffU) & ~(VkDeviceSize)0xfffffU;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	create.size = grown;
	create.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	error = vulkan->vk.vkCreateBuffer(vulkan->device, &create, NULL, &render->readback);
	if (error != VK_SUCCESS) {
		render->readback = VK_NULL_HANDLE;
		return EIO;
	}

	/* Memory the host reads, cached and coherent when there is such, else any it sees. */
	vulkan->vk.vkGetBufferMemoryRequirements(vulkan->device, render->readback, &requirements);
	render->readback_coherent = 1;
	type = x11_vulkan_memory_type(vulkan, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (type == UINT32_MAX)
		type = x11_vulkan_memory_type(vulkan, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (type == UINT32_MAX) {
		type = x11_vulkan_memory_type(vulkan, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
		render->readback_coherent = 0;
	}
	if (type == UINT32_MAX)
		return EIO;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	error = vulkan->vk.vkAllocateMemory(vulkan->device, &allocate, NULL, &render->readback_memory);
	if (error != VK_SUCCESS) {
		render->readback_memory = VK_NULL_HANDLE;
		return EIO;
	}

	/* Bound and mapped. */
	error = vulkan->vk.vkBindBufferMemory(vulkan->device, render->readback, render->readback_memory, 0U);
	if (error != VK_SUCCESS)
		return EIO;
	error = vulkan->vk.vkMapMemory(vulkan->device, render->readback_memory, 0U, VK_WHOLE_SIZE, 0U, &mapped);
	if (error != VK_SUCCESS)
		return EIO;

	/* Succeeded: room for the read back. */
	render->readback_bytes = mapped;
	render->readback_size = grown;
	return 0;
}

/*
 * Gives the copy on the GPU of a tile or stipple (whose pixels are in
 * memory), made the first time and uploaded again when it was drawn in
 * memory since.  The copy is kept with the image and goes with it.
 * Returns NULL when it cannot be had.
 */
static struct x11_gpu_image *
render_pattern(
	struct x11_vulkan *vulkan,
	const struct x11_image *pattern)
{
	struct x11_gpu_image *copy;
	struct x11_image *kept;
	int failed;

	/* A pattern the device cannot hold. */
	if (pattern->width <= 0 || pattern->height <= 0)
		return NULL;
	if ((uint32_t)pattern->width > vulkan->image_side_max || (uint32_t)pattern->height > vulkan->image_side_max)
		return NULL;

	/* Its copy, made now when it has none; the image keeps it (a cache: what the image shows does not change). */
	copy = pattern->gpu;
	if (copy == NULL) {
		copy = render_image_make(vulkan, RENDER_KIND_PATTERN, pattern->width, pattern->height);
		if (copy == NULL)
			return NULL;
		copy->gpu_stale = 1;
		kept = (struct x11_image *)pattern;
		kept->gpu = copy;
	}

	/* Uploaded when memory is newer. */
	if (copy->gpu_stale) {
		failed = render_upload(copy, 0, 0, pattern->width, pattern->height, pattern->pixels, pattern->stride);
		if (failed != 0)
			return NULL;
		copy->gpu_stale = 0;
	}

	/* Succeeded: the pattern on the GPU. */
	return copy;
}

/*
 * Gives the scratch image copies within one image go through, at least of
 * a size (a smaller one goes after the commands that use it).
 */
static struct x11_gpu_image *
render_scratch(
	struct x11_vulkan *vulkan,
	int width,
	int height)
{
	struct x11_render_vulkan *render;
	struct x11_gpu_image *scratch;

	/* Big enough already. */
	render = vulkan->render;
	scratch = render->scratch;
	if (scratch != NULL && scratch->width >= width && scratch->height >= height)
		return scratch;

	/* A larger one, of at least the old one's size. */
	if (scratch != NULL) {
		if (scratch->width > width)
			width = scratch->width;
		if (scratch->height > height)
			height = scratch->height;
		render_image_retire(scratch);
		render->scratch = NULL;
	}
	scratch = render_image_make(vulkan, RENDER_KIND_SCRATCH, width, height);
	if (scratch == NULL)
		return NULL;

	/* Succeeded: the scratch. */
	render->scratch = scratch;
	return scratch;
}

/* Adds a box to an operation's boxes, growing their extents.  Returns 0, or ENOMEM. */
static int
render_parts_add(
	struct render_parts *parts,
	const struct x11_box *box)
{
	struct x11_box *grown;
	size_t capacity;

	/* Room for one more. */
	if (parts->count == parts->capacity) {
		capacity = parts->capacity * 2U;
		if (capacity == 0U)
			capacity = 32U;
		grown = realloc(parts->boxes, capacity * sizeof(*grown));
		if (grown == NULL)
			return ENOMEM;
		parts->boxes = grown;
		parts->capacity = capacity;
	}

	/* The extents grow to it. */
	if (parts->count == 0U) {
		parts->extents = *box;
	} else {
		if (box->x0 < parts->extents.x0)
			parts->extents.x0 = box->x0;
		if (box->y0 < parts->extents.y0)
			parts->extents.y0 = box->y0;
		if (box->x1 > parts->extents.x1)
			parts->extents.x1 = box->x1;
		if (box->y1 > parts->extents.y1)
			parts->extents.y1 = box->y1;
	}

	/* Succeeded: kept. */
	parts->boxes[parts->count] = *box;
	parts->count++;
	return 0;
}

/*
 * Adds the parts of a box inside an image and each box of a clip (NULL:
 * the image only) to an operation's boxes.  Without memory, a part is
 * left out; its extents still cover it, so a fallback over them draws it.
 */
static void
render_parts_clip(
	struct render_parts *parts,
	const struct x11_image *target,
	const struct x11_region *clip,
	const struct x11_box *box)
{
	struct x11_box bounds;
	struct x11_box inside;
	struct x11_box part;
	size_t index;
	int meets;

	/* The box inside the image. */
	bounds.x0 = 0;
	bounds.y0 = 0;
	bounds.x1 = target->width;
	bounds.y1 = target->height;
	meets = x11_box_intersect(box, &bounds, &inside);
	if (!meets)
		return;

	/* With no clip, all of it. */
	if (clip == NULL) {
		(void)render_parts_add(parts, &inside);
		return;
	}

	/* Each part inside a box of the clip. */
	for (index = 0U; index < clip->count; index++) {
		meets = x11_box_intersect(&clip->boxes[index], &inside, &part);
		if (!meets)
			continue;
		(void)render_parts_add(parts, &part);
	}
}

/*
 * Gives the colour channels a plane mask writes (bit 2 red, 1 green, 0
 * blue).  Returns nonzero when the mask is whole channels, 0 when it
 * names part of one (the GPU cannot write that).
 */
static int
render_write_mask(
	uint32_t plane_mask,
	unsigned *write)
{
	uint32_t channel;
	unsigned index;

	/* Each channel's eight planes, all or none. */
	*write = 0U;
	for (index = 0U; index < 3U; index++) {
		channel = (plane_mask >> (index * 8U)) & 0xffU;
		if (channel == 0xffU) {
			*write |= 1U << index;
			continue;
		}
		if (channel != 0U)
			return 0;
	}

	/* Succeeded: whole channels. */
	return 1;
}

/*
 * Fills an operation's boxes with a paint on the GPU.  Returns nonzero
 * when drawn (or when nothing changes), 0 when the GPU cannot draw it.
 */
static int
render_fill_gpu(
	struct x11_image *target,
	const struct x11_paint *paint,
	const struct render_parts *parts)
{
	struct x11_vulkan *vulkan;
	struct x11_gpu_image *pattern;
	const struct x11_image *pattern_image;
	struct render_push push;
	unsigned function;
	unsigned write;
	int whole;

	/* The channels the plane mask writes; none leaves every pixel as it is, as does the function no-op. */
	vulkan = target->gpu->vulkan;
	whole = render_write_mask(paint->plane_mask, &write);
	if (!whole)
		return 0;
	function = paint->function & 15U;
	if (write == 0U || function == 5U)
		return 1;

	/* A solid fill of every channel that ignores the target is a clear. */
	if (paint->fill_style == X11_FILL_SOLID && write == RENDER_WRITE_ALL) {
		if (function == X11_FUNCTION_COPY)
			return render_clear(target, paint->foreground, parts);
		if (function == X11_FUNCTION_CLEAR)
			return render_clear(target, 0U, parts);
		if (function == X11_FUNCTION_SET)
			return render_clear(target, RENDER_PLANES, parts);
	}

	/* Another function needs the device's logic operations. */
	if (function != X11_FUNCTION_COPY && !vulkan->logic_op)
		return 0;

	/* The source: the foreground, a tile, or a stipple. */
	memset(&push, 0, sizeof(push));
	push.foreground = paint->foreground & RENDER_PLANES;
	push.background = paint->background & RENDER_PLANES;
	pattern_image = NULL;
	if (paint->fill_style == X11_FILL_SOLID) {
		push.mode = RENDER_MODE_SOLID;
	} else if (paint->fill_style == X11_FILL_TILED) {
		push.mode = RENDER_MODE_TILED;
		pattern_image = paint->tile;
	} else if (paint->fill_style == X11_FILL_STIPPLED) {
		push.mode = RENDER_MODE_STIPPLED;
		pattern_image = paint->stipple;
	} else {
		push.mode = RENDER_MODE_OPAQUE_STIPPLED;
		pattern_image = paint->stipple;
	}

	/* A tile or stipple fill without its pattern is left to the processor's rules. */
	if (push.mode != RENDER_MODE_SOLID && pattern_image == NULL)
		return 0;

	/* The pattern on the GPU, its origin given less its size so the shader's place is never negative. */
	pattern = vulkan->render->blank;
	if (pattern_image != NULL) {
		pattern = render_pattern(vulkan, pattern_image);
		if (pattern == NULL || pattern == target->gpu)
			return 0;
		push.pattern_size[0] = pattern_image->width;
		push.pattern_size[1] = pattern_image->height;
		push.origin[0] = render_wrap(paint->origin_x, pattern_image->width) - pattern_image->width;
		push.origin[1] = render_wrap(paint->origin_y, pattern_image->height) - pattern_image->height;
	}

	/* Succeeded or not as the draw is. */
	return render_draw(target, pattern, function, write, &push, parts);
}

/*
 * Clears an operation's boxes to a pixel on the GPU.  Returns nonzero
 * when recorded, 0 when it could not be.
 */
static int
render_clear(
	struct x11_image *target,
	uint32_t pixel,
	const struct render_parts *parts)
{
	struct x11_vulkan *vulkan;
	struct x11_gpu_image *image;
	struct render_access access;
	VkRenderPassBeginInfo begin;
	VkClearAttachment attachment;
	VkClearRect *rects;
	VkCommandBuffer command;
	VkResult error;
	size_t index;

	/* The rectangles. */
	image = target->gpu;
	vulkan = image->vulkan;
	rects = malloc(parts->count * sizeof(*rects));
	if (rects == NULL)
		return 0;
	for (index = 0U; index < parts->count; index++) {
		rects[index].rect.offset.x = parts->boxes[index].x0;
		rects[index].rect.offset.y = parts->boxes[index].y0;
		rects[index].rect.extent.width = (uint32_t)(parts->boxes[index].x1 - parts->boxes[index].x0);
		rects[index].rect.extent.height = (uint32_t)(parts->boxes[index].y1 - parts->boxes[index].y0);
		rects[index].baseArrayLayer = 0U;
		rects[index].layerCount = 1U;
	}

	/* The framebuffer, and the buffer after what the clear must wait for. */
	error = render_framebuffer(image);
	command = render_begin(vulkan);
	if (error != VK_SUCCESS || command == VK_NULL_HANDLE) {
		free(rects);
		return 0;
	}
	access.image = image;
	access.write = 1;
	access.box = parts->extents;
	render_accesses(vulkan, command, &access, 1U);

	/* The pixel, opaque. */
	memset(&attachment, 0, sizeof(attachment));
	attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	attachment.colorAttachment = 0U;
	attachment.clearValue.color.float32[0] = (float)((pixel >> 16) & 0xffU) / 255.0f;
	attachment.clearValue.color.float32[1] = (float)((pixel >> 8) & 0xffU) / 255.0f;
	attachment.clearValue.color.float32[2] = (float)(pixel & 0xffU) / 255.0f;
	attachment.clearValue.color.float32[3] = 1.0f;

	/* Cleared inside the render pass. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	begin.renderPass = vulkan->render->render_pass;
	begin.framebuffer = image->framebuffer;
	begin.renderArea.extent.width = (uint32_t)image->width;
	begin.renderArea.extent.height = (uint32_t)image->height;
	vulkan->vk.vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
	vulkan->vk.vkCmdClearAttachments(command, 1U, &attachment, (uint32_t)parts->count, rects);
	vulkan->vk.vkCmdEndRenderPass(command);
	free(rects);

	/* Succeeded: the GPU's pixels are newer than memory's. */
	image->memory_stale = 1;
	return 1;
}

/*
 * Draws an operation's boxes on the GPU through paint.frag with a
 * pattern, a function and the channels written.  Returns nonzero when
 * recorded, 0 when it could not be.
 */
static int
render_draw(
	struct x11_image *target,
	struct x11_gpu_image *pattern,
	unsigned function,
	unsigned write,
	const struct render_push *push,
	const struct render_parts *parts)
{
	struct x11_vulkan *vulkan;
	struct x11_render_vulkan *render;
	struct x11_gpu_image *image;
	struct render_slot *slot;
	struct render_access accesses[2];
	struct render_push values;
	VkDescriptorSetAllocateInfo allocate;
	VkDescriptorImageInfo described;
	VkWriteDescriptorSet update;
	VkDescriptorSet set;
	VkRenderPassBeginInfo begin;
	VkViewport viewport;
	VkRect2D scissor;
	VkCommandBuffer command;
	VkPipeline pipeline;
	VkBuffer buffer;
	VkDeviceSize offset;
	int32_t *boxes;
	VkResult error;
	size_t index;

	/* The pipeline and framebuffer. */
	image = target->gpu;
	vulkan = image->vulkan;
	render = vulkan->render;
	pipeline = render_pipeline(vulkan, function, write);
	if (pipeline == VK_NULL_HANDLE)
		return 0;
	error = render_framebuffer(image);
	if (error != VK_SUCCESS)
		return 0;

	/* The boxes in staging memory (this also gives room for the descriptor set below). */
	boxes = render_staging(vulkan, (VkDeviceSize)parts->count * 4U * sizeof(int32_t), &buffer, &offset);
	if (boxes == NULL)
		return 0;
	for (index = 0U; index < parts->count; index++) {
		boxes[index * 4U + 0U] = parts->boxes[index].x0;
		boxes[index * 4U + 1U] = parts->boxes[index].y0;
		boxes[index * 4U + 2U] = parts->boxes[index].x1;
		boxes[index * 4U + 3U] = parts->boxes[index].y1;
	}

	/* The pattern's descriptor set. */
	slot = &render->slots[render->current];
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = slot->descriptors;
	allocate.descriptorSetCount = 1U;
	allocate.pSetLayouts = &render->set_layout;
	error = vulkan->vk.vkAllocateDescriptorSets(vulkan->device, &allocate, &set);
	if (error != VK_SUCCESS)
		return 0;
	slot->descriptors_used++;
	described.sampler = render->sampler;
	described.imageView = pattern->view;
	described.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	memset(&update, 0, sizeof(update));
	update.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	update.dstSet = set;
	update.dstBinding = 0U;
	update.descriptorCount = 1U;
	update.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	update.pImageInfo = &described;
	vulkan->vk.vkUpdateDescriptorSets(vulkan->device, 1U, &update, 0U, NULL);

	/* After what the draw must wait for: the pattern read, the boxes written. */
	command = render_begin(vulkan);
	if (command == VK_NULL_HANDLE)
		return 0;
	accesses[0].image = pattern;
	accesses[0].write = 0;
	memset(&accesses[0].box, 0, sizeof(accesses[0].box));
	accesses[1].image = image;
	accesses[1].write = 1;
	accesses[1].box = parts->extents;
	render_accesses(vulkan, command, accesses, 2U);

	/* The values, with the target's size. */
	values = *push;
	values.target_size[0] = image->width;
	values.target_size[1] = image->height;

	/* The draw: one instance a box. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	begin.renderPass = render->render_pass;
	begin.framebuffer = image->framebuffer;
	begin.renderArea.extent.width = (uint32_t)image->width;
	begin.renderArea.extent.height = (uint32_t)image->height;
	viewport.x = 0.0f;
	viewport.y = 0.0f;
	viewport.width = (float)image->width;
	viewport.height = (float)image->height;
	viewport.minDepth = 0.0f;
	viewport.maxDepth = 1.0f;
	scissor.offset.x = 0;
	scissor.offset.y = 0;
	scissor.extent.width = (uint32_t)image->width;
	scissor.extent.height = (uint32_t)image->height;
	vulkan->vk.vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
	vulkan->vk.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vulkan->vk.vkCmdSetViewport(command, 0U, 1U, &viewport);
	vulkan->vk.vkCmdSetScissor(command, 0U, 1U, &scissor);
	vulkan->vk.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, render->layout, 0U, 1U, &set, 0U, NULL);
	vulkan->vk.vkCmdPushConstants(command, render->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0U, (uint32_t)sizeof(values), &values);
	vulkan->vk.vkCmdBindVertexBuffers(command, 0U, 1U, &buffer, &offset);
	vulkan->vk.vkCmdDraw(command, 6U, (uint32_t)parts->count, 0U, 0U);
	vulkan->vk.vkCmdEndRenderPass(command);

	/* Succeeded: the GPU's pixels are newer than memory's. */
	image->memory_stale = 1;
	return 1;
}

/*
 * Copies an operation's boxes from a source (target + (dx, dy)) on the
 * GPU.  Returns nonzero when drawn (or when nothing changes), 0 when the
 * GPU cannot.
 */
static int
render_copy_gpu(
	struct x11_image *target,
	const struct x11_image *source,
	int dx,
	int dy,
	unsigned function,
	uint32_t plane_mask,
	const struct render_parts *parts)
{
	struct x11_vulkan *vulkan;
	struct x11_gpu_image *from;
	struct x11_gpu_image *scratch;
	struct render_push push;
	const struct x11_box *part;
	unsigned write;
	size_t index;
	int whole;
	int drawn;
	int failed;

	/* The channels written; none, or the function no-op, changes nothing. */
	vulkan = target->gpu->vulkan;
	whole = render_write_mask(plane_mask, &write);
	if (!whole)
		return 0;
	function &= 15U;
	if (write == 0U || function == 5U)
		return 1;

	/* A source in memory: a copy of every channel is an upload, else drawn in memory. */
	from = source->gpu;
	if (from == NULL) {
		if (function != X11_FUNCTION_COPY || write != RENDER_WRITE_ALL)
			return 0;
		for (index = 0U; index < parts->count; index++) {
			part = &parts->boxes[index];
			failed = render_upload(target->gpu, part->x0, part->y0, part->x1 - part->x0, part->y1 - part->y0, source->pixels + (size_t)(part->y0 + dy) * source->stride + (size_t)(part->x0 + dx), source->stride);
			if (failed != 0)
				return 0;
		}
		target->gpu->memory_stale = 1;
		return 1;
	}

	/* A source whose newest pixels are in memory is brought up to date on the GPU first. */
	if (from->gpu_stale) {
		from = render_pattern(vulkan, source);
		if (from == NULL)
			return 0;
	}

	/* Within one image, the source's pixels go through the scratch (at their own places) first. */
	if (from == target->gpu) {
		scratch = render_scratch(vulkan, from->width, from->height);
		if (scratch == NULL)
			return 0;
		drawn = render_copy_images(scratch, dx, dy, from, dx, dy, parts);
		if (!drawn)
			return 0;
		from = scratch;
	}

	/* A copy of every channel copies the images. */
	if (function == X11_FUNCTION_COPY && write == RENDER_WRITE_ALL) {
		drawn = render_copy_images(target->gpu, 0, 0, from, dx, dy, parts);
		if (drawn)
			target->gpu->memory_stale = 1;
		return drawn;
	}

	/* Another function needs the device's logic operations. */
	if (function != X11_FUNCTION_COPY && !vulkan->logic_op)
		return 0;

	/* Drawn reading the source at the target's place plus (dx, dy). */
	memset(&push, 0, sizeof(push));
	push.mode = RENDER_MODE_COPY;
	push.origin[0] = -dx;
	push.origin[1] = -dy;
	push.pattern_size[0] = from->width;
	push.pattern_size[1] = from->height;

	/* Succeeded or not as the draw is. */
	return render_draw(target, from, function, write, &push, parts);
}

/*
 * Records copies of an operation's boxes between images: each box b of
 * the target at b + (target_dx, target_dy) from the source at b +
 * (source_dx, source_dy).  Returns nonzero when recorded.
 */
static int
render_copy_images(
	struct x11_gpu_image *target,
	int target_dx,
	int target_dy,
	struct x11_gpu_image *source,
	int source_dx,
	int source_dy,
	const struct render_parts *parts)
{
	struct x11_vulkan *vulkan;
	struct render_access accesses[2];
	VkImageCopy *regions;
	VkCommandBuffer command;
	size_t index;

	/* The regions. */
	vulkan = target->vulkan;
	regions = calloc(parts->count, sizeof(*regions));
	if (regions == NULL)
		return 0;
	for (index = 0U; index < parts->count; index++) {
		regions[index].srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		regions[index].srcSubresource.layerCount = 1U;
		regions[index].srcOffset.x = parts->boxes[index].x0 + source_dx;
		regions[index].srcOffset.y = parts->boxes[index].y0 + source_dy;
		regions[index].dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		regions[index].dstSubresource.layerCount = 1U;
		regions[index].dstOffset.x = parts->boxes[index].x0 + target_dx;
		regions[index].dstOffset.y = parts->boxes[index].y0 + target_dy;
		regions[index].extent.width = (uint32_t)(parts->boxes[index].x1 - parts->boxes[index].x0);
		regions[index].extent.height = (uint32_t)(parts->boxes[index].y1 - parts->boxes[index].y0);
		regions[index].extent.depth = 1U;
	}

	/* After what the copy must wait for: the source read, the target's boxes written. */
	command = render_begin(vulkan);
	if (command == VK_NULL_HANDLE) {
		free(regions);
		return 0;
	}
	accesses[0].image = source;
	accesses[0].write = 0;
	memset(&accesses[0].box, 0, sizeof(accesses[0].box));
	accesses[1].image = target;
	accesses[1].write = 1;
	accesses[1].box.x0 = parts->extents.x0 + target_dx;
	accesses[1].box.y0 = parts->extents.y0 + target_dy;
	accesses[1].box.x1 = parts->extents.x1 + target_dx;
	accesses[1].box.y1 = parts->extents.y1 + target_dy;
	render_accesses(vulkan, command, accesses, 2U);

	/* Succeeded: the copy recorded. */
	vulkan->vk.vkCmdCopyImage(command, source->image, VK_IMAGE_LAYOUT_GENERAL, target->image, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)parts->count, regions);
	free(regions);
	return 1;
}

/*
 * Starts drawing an image in memory instead of on the GPU: the box (NULL:
 * all of it) brought up to date in memory, and a view of the image that
 * draws in memory only.
 */
static void
render_fallback_begin(
	const struct x11_image *image,
	const struct x11_box *box,
	struct x11_image *view)
{
	/* Memory brought up to date where the drawing goes. */
	if (image->gpu != NULL) {
		if (image->gpu->kind == RENDER_KIND_TARGET && image->gpu->vulkan->render != NULL)
			image->gpu->vulkan->render->fallbacks++;
		if (box == NULL) {
			x11_render_image_memory(image);
		} else {
			x11_render_image_memory_box(image, box);
		}
	}

	/* The same pixels, without the GPU. */
	*view = *image;
	view->gpu = NULL;
}

/*
 * Ends drawing an image in memory: the box (NULL: all of it) put back on
 * the GPU (a pattern's copy is uploaded again when next used).
 */
static void
render_fallback_end(
	struct x11_image *image,
	const struct x11_box *box)
{
	struct x11_gpu_image *copy;
	struct x11_box all;
	int failed;

	/* Nothing on the GPU, or a device that no longer draws. */
	copy = image->gpu;
	if (copy == NULL || copy->vulkan->lost)
		return;

	/* A pattern's copy is made again from memory when it is used next. */
	if (copy->kind != RENDER_KIND_TARGET) {
		copy->gpu_stale = 1;
		return;
	}

	/* The box drawn, or all of the image. */
	if (box == NULL) {
		all.x0 = 0;
		all.y0 = 0;
		all.x1 = image->width;
		all.y1 = image->height;
		box = &all;
	}

	/* Put back on the GPU. */
	failed = render_upload(copy, box->x0, box->y0, box->x1 - box->x0, box->y1 - box->y0, image->pixels + (size_t)box->y0 * image->stride + (size_t)box->x0, image->stride);
	if (failed != 0)
		x11_log(copy->vulkan->server, "X11SERVER RENDER upload after a fallback failed");
}

/* Tells whether an image is drawn on the GPU: a drawable's copy on a device that still draws. */
static int
render_usable(
	const struct x11_image *image)
{
	/* No copy, or a pattern's. */
	if (image->gpu == NULL || image->gpu->kind != RENDER_KIND_TARGET)
		return 0;

	/* A device that failed. */
	if (image->gpu->vulkan->lost || image->gpu->vulkan->render == NULL)
		return 0;

	/* Succeeded: drawn on the GPU. */
	return 1;
}

/* Returns a value modulo a size, never negative. */
static int
render_wrap(
	int value,
	int size)
{
	int remainder;

	/* C's remainder takes the sign of the value. */
	remainder = value % size;
	if (remainder < 0)
		remainder += size;

	/* The place within the size. */
	return remainder;
}
