/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * wl_shm, the secondary path for client images (WS035 compositing design,
 * D2), and the compositor's own cursor image (D8).
 *
 * A pool is the client's anonymous shared memory, mapped read-only once at
 * creation and again at resize.  A surface showing a wl_shm buffer has a
 * linear, host-visible Vulkan image of the buffer's size that window mode
 * samples; after a commit the rows the client damaged are copied into it
 * with the CPU, once no drawing given still copies that image's staging
 * (a drawing's completion wakes the event loop to try again), and the
 * buffer is then released at once: the client can draw its next frame
 * without waiting for the GPU.  An image made anew (a new size) waits for
 * nothing in flight, as it uses the queue.  The GPU path
 * (kl_gpu_buffer_v1) is not touched by any of this.
 */

#include "compose.h"
#include "extras.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* The compositor's arrow: its size and hotspot. */
#define ARROW_WIDTH	12U
#define ARROW_HEIGHT	19U

static int pool_create(struct kwl_object *shm, const unsigned char *bytes, size_t size);
static int pool_buffer(struct kwl_object *pool, const unsigned char *bytes, size_t size);
static int pool_resize(struct kwl_object *pool, const unsigned char *bytes, size_t size);
static int surface_upload(struct kwl_server *server, struct kwl_object *surface);
static VkResult image_create(struct kwl_compose *compose, uint32_t width, uint32_t height, VkSampler sampler, struct kwl_import *import);
static VkResult image_layout(struct kwl_compose *compose, VkImage image, VkImageLayout old_layout);
static VkResult image_layout_record(VkCommandBuffer command, VkImage image, VkImageLayout old_layout);
static void image_copy_record(VkCommandBuffer command, const struct kwl_list_copy *copy);
static void image_release(struct kwl_compose *compose, struct kwl_import *import);
static void image_unlink(struct kwl_compose *compose, struct kwl_import *import);
static uint32_t word(const unsigned char *bytes, size_t offset);
static uint32_t kwl_row_sum(const unsigned char *row, uint32_t width);

/*
 * Tells a new wl_shm binding the two formats it may use.
 */
int
kwl_shm_bind(
	struct kwl_object *shm)
{
	uint32_t format;
	int error;

	/* ARGB8888, then XRGB8888. */
	format = KWL_SHM_ARGB8888;
	error = kwl_emit(shm->client, shm->id, 0, &format, sizeof(format));
	if (error != 0)
		return error;
	format = KWL_SHM_XRGB8888;
	error = kwl_emit(shm->client, shm->id, 0, &format, sizeof(format));
	return error;
}

/*
 * Handles the requests of wl_shm (create_pool) and wl_shm_pool
 * (create_buffer, destroy, resize).
 */
int
kwl_shm_request(
	struct kwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	int error;

	/* wl_shm has one request. */
	if (object->kind == KWL_SHM) {
		if (opcode != 0U)
			return EPROTO;
		error = pool_create(object, bytes, size);
		return error;
	}

	/* A pool's three requests. */
	switch (opcode) {
	case 0:
		error = pool_buffer(object, bytes, size);
		break;
	case 1:
		/* The pool's memory stays for the buffers made from it. */
		if (size != 0)
			return EPROTO;
		kwl_object_destroy(object);
		error = 0;
		break;
	case 2:
		error = pool_resize(object, bytes, size);
		break;
	default:
		error = EPROTO;
		break;
	}

	/* Succeeded, or the protocol error. */
	return error;
}

/*
 * Drops a reference to a pool's memory; the last one unmaps it.
 */
void
kwl_pool_put(
	struct kwl_pool *pool)
{
	/* Other holders keep the mapping. */
	if (pool == NULL)
		return;
	pool->references--;
	if (pool->references != 0)
		return;

	/* The mapping and the fd go with the last. */
	(void)munmap(pool->map, pool->size);
	close(pool->fd);
	free(pool);
}

/*
 * Copies the damaged rows of every surface's new wl_shm image into the
 * image window mode samples, and releases the buffers.  A surface's copy
 * waits while a drawing given still copies its image's staging (or, for
 * an image to be made anew, while anything is in flight); a drawing's
 * completion brings the next try.
 */
int
kwl_shm_upload(
	struct kwl_server *server)
{
	struct kwl_client *client;
	struct kwl_object *surface;
	uint64_t mark;
	int error;

	/* Without window mode nothing is copied. */
	if (server->compose == NULL)
		return 0;

	/* The copies and the images made for them are the main thread's work outside a walk (KWL PERF pass, ws213-p004). */
	mark = kwl_cycles();

	/* Each surface whose committed wl_shm image has not been copied. */
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			if (surface->kind != KWL_SURFACE || surface->dead || !surface->shm_upload)
				continue;

			/* A surface that cannot be copied is the client's protocol error. */
			error = surface_upload(server, surface);
			if (error != 0) {
				(void)kwl_error(client, surface->id, "wl_shm image could not be copied");
				surface->shm_upload = 0;
			}
		}
	}

	/* The time of the copies. */
	kwl_perf_vk_outside(server, kwl_cycles() - mark);

	/* Succeeded. */
	return 0;
}

/*
 * Releases a surface's wl_shm image: now when nothing given before may
 * still use it, otherwise after that, with no wait.  No frame takes its
 * staging from now on.
 */
void
kwl_shm_image_destroy(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	struct kwl_compose *compose;

	/* A surface without an image has nothing to release. */
	compose = server->compose;
	if (surface->shm_image == NULL || compose == NULL)
		return;

	/* Let go, then the record. */
	kwl_host_image_release(compose, surface->shm_image);
	free(surface->shm_image);
	surface->shm_image = NULL;
}

/*
 * Makes the compositor's arrow cursor: white with a black edge, its tip the
 * hotspot at (0, 0).
 */
int
kwl_arrow_create(
	struct kwl_server *server)
{
	static const char *const shape[ARROW_HEIGHT] = {
		"X           ",
		"XX          ",
		"X.X         ",
		"X..X        ",
		"X...X       ",
		"X....X      ",
		"X.....X     ",
		"X......X    ",
		"X.......X   ",
		"X........X  ",
		"X.........X ",
		"X......XXXXX",
		"X...X..X    ",
		"X..XX..X    ",
		"X.X  X..X   ",
		"XX   X..X   ",
		"X     X..X  ",
		"      X..X  ",
		"       XX   "
	};
	struct kwl_import *arrow;
	uint32_t *row;
	uint32_t x;
	uint32_t y;
	VkResult result;

	/* A linear, host-visible image the CPU writes once. */
	arrow = calloc(1, sizeof(*arrow));
	if (arrow == NULL)
		return ENOMEM;
	result = image_create(server->compose, ARROW_WIDTH, ARROW_HEIGHT, server->compose->sampler, arrow);
	if (result != VK_SUCCESS) {
		kwl_host_image_release(server->compose, arrow);
		free(arrow);
		return EIO;
	}

	/* Black edge, white inside, transparent elsewhere (B, G, R, A in memory). */
	for (y = 0; y < ARROW_HEIGHT; y++) {
		row = (uint32_t *)((unsigned char *)arrow->map + y * arrow->row_pitch);
		for (x = 0; x < ARROW_WIDTH; x++) {
			if (shape[y][x] == 'X')
				row[x] = 0xff000000U;
			else if (shape[y][x] == '.')
				row[x] = 0xffffffffU;
			else
				row[x] = 0x00000000U;
		}
	}

	/* Succeeded: drawn with alpha (the other cursor shapes are made when first asked for, cursor.c). */
	arrow->draw = KWL_DRAW_ALPHA;
	server->arrow = arrow;
	return 0;
}

/*
 * Releases the arrow cursor.
 */
void
kwl_arrow_destroy(
	struct kwl_server *server)
{
	/* No arrow was made. */
	if (server->arrow == NULL || server->compose == NULL)
		return;

	/* The shapes' images first (cursor.c). */
	kwl_cursor_images_destroy(server);

	/* Its Vulkan objects, then the record. */
	kwl_host_image_release(server->compose, server->arrow);
	free(server->arrow);
	server->arrow = NULL;
}

/*
 * Creates a host-written image for the compositor's own drawing (the glass look),
 * sampled with the given sampler.
 */
VkResult
kwl_host_image_create(
	struct kwl_compose *compose,
	uint32_t width,
	uint32_t height,
	VkSampler sampler,
	struct kwl_import *import)
{
	VkResult result;

	/* The same image as a wl_shm copy's. */
	result = image_create(compose, width, height, sampler, import);
	return result;
}

/*
 * Lets go of a host image (kwl_host_image_create, a wl_shm copy, the
 * arrow): no frame takes its staging from now on, and its objects go now
 * when nothing given before may still use them, otherwise once that has
 * completed, with no wait (kwl_compose_bury).  The record is cleared and
 * may be used again or freed at once.
 */
void
kwl_host_image_release(
	struct kwl_compose *compose,
	struct kwl_import *import)
{
	/* Out of the list the next frames copy from, then let go. */
	image_unlink(compose, import);
	kwl_compose_bury(compose, import, image_release);
}

/*
 * Moves a host image's record to another place (an image made aside before
 * it takes the place of one released, WS217): the list the frames copy from
 * follows it, and the old place is left empty.  The destination holds no
 * image.
 */
void
kwl_host_image_move(
	struct kwl_compose *compose,
	struct kwl_import *from,
	struct kwl_import *to)
{
	struct kwl_import **position;

	/* The list's link to the record points to its new place. */
	for (position = &compose->host_images; *position != NULL; position = &(*position)->host_next) {
		/* The link to this exact record. */
		if (*position == from) {
			*position = to;
			break;
		}
	}

	/* The record itself, with its own link onward; the old place holds nothing. */
	*to = *from;
	memset(from, 0, sizeof(*from));
}

/*
 * Marks a host image's staging as written, so the next frame copies it into the sampled image.
 */
void
kwl_host_image_written(
	struct kwl_import *import)
{
	/* Every row. */
	kwl_host_image_written_rows(import, 0U, import->height);
}

/*
 * Marks rows first to last (excluded) of a host image's staging as
 * written, so the next frame copies them into the sampled image.
 */
void
kwl_host_image_written_rows(
	struct kwl_import *import,
	uint32_t first,
	uint32_t last)
{
	/* The rows within the image; none is nothing to copy. */
	if (last > import->height)
		last = import->height;
	if (first >= last)
		return;

	/* Joined with the rows written before and not yet taken. */
	if (import->dirty_first >= import->dirty_last) {
		import->dirty_first = first;
		import->dirty_last = last;
	} else {
		if (first < import->dirty_first)
			import->dirty_first = first;
		if (last > import->dirty_last)
			import->dirty_last = last;
	}

	/* The next kwl_host_images_take gives this image to its frame and clears the mark; copies elsewhere see a new version. */
	import->written = 1U;
	import->version++;
}

/*
 * Gives every written host image to the list a drawing has just written,
 * whose frame copies its staging into its sampled image first; the
 * images' marks are cleared (ws213-p003, p001 4.4).
 */
void
kwl_host_images_take(
	struct kwl_compose *compose,
	struct kwl_list *list)
{
	struct kwl_list_copy copy;
	struct kwl_import *import;

	/* Each written image, once, in the order of the list of host images. */
	for (import = compose->host_images; import != NULL; import = import->host_next) {
		/* An unchanged staging has nothing new to copy. */
		if (!import->written)
			continue;

		/* The frame copies the rows written since the last take; a later write marks it for the next frame. */
		copy.staging = import->staging;
		copy.image = import->image;
		copy.width = import->width;
		copy.first = import->dirty_first;
		copy.last = import->dirty_last;
		kwl_list_add_copy(list, &copy);
		import->written = 0U;

		/* The drawing being written copies the staging: it is not written again until that drawing has completed. */
		import->copy_drawing = compose->drawings_given + compose->drawing_open;
		import->dirty_first = 0U;
		import->dirty_last = 0U;
	}
}

/*
 * Records the copies of the host images a list took into their sampled
 * images, for a command buffer that runs before the frame's; returns how
 * many.
 */
unsigned
kwl_host_images_record(
	const struct kwl_list *list,
	VkCommandBuffer command)
{
	unsigned index;

	/* Each image the list took, in its order. */
	for (index = 0U; index < list->copy_count; index++)
		image_copy_record(command, &list->copies[index]);

	/* The number of copies recorded. */
	return list->copy_count;
}

/*
 * Starts recording the start's host images' layout moves into one command
 * buffer (ws035-p131), submitted by kwl_host_image_batch_end.
 */
void
kwl_host_image_batch_begin(
	struct kwl_compose *compose)
{
	/* The next images' moves wait in setup_command. */
	compose->setup_batching = 1;
}

/*
 * Submits the layout moves recorded since kwl_host_image_batch_begin and
 * waits for them once, before the first frame samples those images.
 * While the render thread runs (an output turning), the moves went into
 * the task being recorded instead, which is given to it now and not
 * waited for: the frames given after it come after it in the queue.
 */
VkResult
kwl_host_image_batch_end(
	struct kwl_compose *compose)
{
	VkSubmitInfo submit;
	VkCommandBuffer command;
	VkResult result;

	/* Later images are moved one by one again. */
	compose->setup_batching = 0;

	/* The render thread submits the moves. */
	if (compose->render_started) {
		kwl_compose_task_flush(compose);
		return VK_SUCCESS;
	}

	/* The start's command buffer, made by its first image. */
	command = compose->setup_command;
	compose->setup_command = VK_NULL_HANDLE;
	if (command == VK_NULL_HANDLE)
		return VK_SUCCESS;

	/* The recorded moves, submitted together. */
	result = vkEndCommandBuffer(command);
	if (result == VK_SUCCESS) {
		memset(&submit, 0, sizeof(submit));
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1U;
		submit.pCommandBuffers = &command;
		kwl_queue_lock(compose);
		result = vkQueueSubmit(compose->queue, 1U, &submit, VK_NULL_HANDLE);

		/* One wait for all of them, before the first frame (under the queue's lock: the start, before the render and cursor threads). */
		if (result == VK_SUCCESS)
			result = vkQueueWaitIdle(compose->queue);
		kwl_queue_unlock(compose);
	}

	/* The command buffer is not kept. */
	vkFreeCommandBuffers(compose->device, compose->pool, 1U, &command);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: every image of the start is in the general layout. */
	return VK_SUCCESS;
}

/* Maps a client's fd as a new pool (wl_shm.create_pool). */
static int
pool_create(
	struct kwl_object *shm,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_object *object;
	struct kwl_pool *pool;
	uint32_t id;
	int32_t length;
	int descriptor;

	/* new_id and size on the wire; the fd travels beside them. */
	if (size != 8U)
		return EPROTO;
	id = word(bytes, 0);
	length = (int32_t)word(bytes, 4);
	if (length <= 0)
		return EPROTO;
	descriptor = kwl_take_fd(shm->client);
	if (descriptor < 0)
		return EAGAIN;

	/* The memory, read-only. */
	pool = calloc(1, sizeof(*pool));
	if (pool == NULL) {
		close(descriptor);
		return ENOMEM;
	}

	/* Mapped once, read-only. */
	pool->fd = descriptor;
	pool->size = (size_t)length;
	pool->map = mmap(NULL, pool->size, PROT_READ, MAP_SHARED, descriptor, 0);
	if (pool->map == MAP_FAILED) {
		close(descriptor);
		free(pool);
		return EPROTO;
	}

	/* The pool object holds the first reference. */
	object = kwl_create(shm->client, id, KWL_SHM_POOL, 1);
	if (object == NULL) {
		(void)munmap(pool->map, pool->size);
		close(descriptor);
		free(pool);
		return EPROTO;
	}

	/* Succeeded: the object holds the memory. */
	pool->references = 1;
	object->pool = pool;

	/* Succeeded. */
	return 0;
}

/* Makes a wl_buffer of part of a pool (wl_shm_pool.create_buffer). */
static int
pool_buffer(
	struct kwl_object *pool,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_shm_buffer *shm;
	struct kwl_object *buffer;
	uint64_t end;
	int32_t offset;
	int32_t width;
	int32_t height;
	int32_t stride;
	uint32_t format;

	/* new_id, offset, width, height, stride and format. */
	if (size != 24U)
		return EPROTO;
	offset = (int32_t)word(bytes, 4);
	width = (int32_t)word(bytes, 8);
	height = (int32_t)word(bytes, 12);
	stride = (int32_t)word(bytes, 16);
	format = word(bytes, 20);

	/* A known format, a positive size, rows of whole pixels, inside the pool. */
	if (format != KWL_SHM_ARGB8888 && format != KWL_SHM_XRGB8888)
		return EPROTO;
	if (offset < 0 || width <= 0 || height <= 0 || stride < width * 4)
		return EPROTO;
	end = (uint64_t)offset + (uint64_t)stride * (uint64_t)(height - 1) + (uint64_t)width * 4U;
	if (end > pool->pool->size)
		return EPROTO;

	/* The buffer, which holds the pool's memory. */
	shm = calloc(1, sizeof(*shm));
	if (shm == NULL)
		return ENOMEM;
	buffer = kwl_create(pool->client, word(bytes, 0), KWL_BUFFER, 1);
	if (buffer == NULL) {
		free(shm);
		return EPROTO;
	}

	/* Where its pixels are. */
	shm->pool = pool->pool;
	shm->offset = (uint32_t)offset;
	shm->width = (uint32_t)width;
	shm->height = (uint32_t)height;
	shm->stride = (uint32_t)stride;
	shm->format = format;
	pool->pool->references++;
	buffer->shm = shm;

	/* Succeeded: never scanned out, copied into window mode's image. */
	return 0;
}

/* Maps a pool again at its new, larger size (wl_shm_pool.resize). */
static int
pool_resize(
	struct kwl_object *pool,
	const unsigned char *bytes,
	size_t size)
{
	struct kwl_pool *memory;
	int32_t length;
	void *map;

	/* A pool may only grow. */
	if (size != 4U)
		return EPROTO;
	length = (int32_t)word(bytes, 0);
	memory = pool->pool;
	if (length <= 0 || (size_t)length < memory->size)
		return EPROTO;

	/* The new mapping replaces the old one. */
	map = mmap(NULL, (size_t)length, PROT_READ, MAP_SHARED, memory->fd, 0);
	if (map == MAP_FAILED)
		return EPROTO;
	(void)munmap(memory->map, memory->size);
	memory->map = map;
	memory->size = (size_t)length;

	/* Succeeded. */
	return 0;
}

/*
 * Copies a surface's committed wl_shm image, the damaged rows or all of it
 * into a new image, and releases the buffer.
 */
static int
surface_upload(
	struct kwl_server *server,
	struct kwl_object *surface)
{
	const struct kwl_shm_buffer *shm;
	struct kwl_object *buffer;
	struct kwl_import *image;
	const unsigned char *source;
	unsigned char *target;
	uint64_t mark;
	uint32_t sum;
	int32_t first;
	int32_t last;
	int32_t y;
	VkResult result;
	int writable;
	int remake;
	int error;

	/* The committed buffer, if it is still a wl_shm one. */
	buffer = surface->current;
	if (buffer == NULL || buffer->shm == NULL) {
		surface->shm_upload = 0;
		return 0;
	}

	/* Its pool's place. */
	shm = buffer->shm;

	/*
	 * Whether the copy may go now: an image of another size is made again
	 * (which uses the queue: nothing in flight), the same one is written
	 * once no drawing given still copies its staging.  Otherwise it waits
	 * for a later pass, which a drawing's completion brings.
	 */
	image = surface->shm_image;
	remake = 0;
	if (image == NULL || image->width != shm->width || image->height != shm->height)
		remake = 1;
	if (remake) {
		writable = kwl_compose_host_writable(server);
	} else {
		writable = kwl_compose_host_image_writable(server->compose, image);
	}

	/* Not yet: the upload stays asked for. */
	if (!writable)
		return 0;
	surface->shm_upload = 0;

	/* An image of another size is made again, and then filled completely. */
	first = 0;
	last = (int32_t)shm->height;
	if (remake) {
		kwl_shm_image_destroy(server, surface);
		image = calloc(1, sizeof(*image));
		if (image == NULL)
			return ENOMEM;
		result = image_create(server->compose, shm->width, shm->height, server->compose->sampler, image);
		if (result != VK_SUCCESS) {
			printf("KWL VULKAN_ERROR operation=shm_image result=%d\n", (int)result);
			kwl_host_image_release(server->compose, image);
			free(image);
			return EIO;
		}

		/* The surface keeps it until its size changes. */
		surface->shm_image = image;
	} else if (surface->committed_damaged) {
		/* Otherwise only the damaged rows. */
		first = surface->committed_damage[1];
		last = surface->committed_damage[3];
		if (first < 0)
			first = 0;
		if (last > (int32_t)shm->height)
			last = (int32_t)shm->height;
	}

	/* XRGB8888's unused byte is not alpha. */
	image->draw = KWL_DRAW_ALPHA;
	if (shm->format == KWL_SHM_XRGB8888)
		image->draw = KWL_DRAW_OPAQUE;

	/* The rows, from the pool into the image (same byte order: B, G, R, A). */
	mark = kwl_cycles();
	source = (const unsigned char *)shm->pool->map + shm->offset;
	target = image->map;
	for (y = first; y < last; y++)
		memcpy(target + (size_t)y * image->row_pitch, source + (size_t)y * shm->stride, (size_t)shm->width * 4U);
	kwl_host_image_written_rows(image, (uint32_t)first, (uint32_t)last);
	server->perf.shm_copies++;
	server->perf.shm_copy_cycles += kwl_cycles() - mark;
	surface->committed_damaged = 0;

	/* The commit that brought the image marked what is drawn again (damage.c); the log follows. */
	if (server->log_frames) {
		/* With the sum of every sixteenth row copied, so that changing pictures can be told from still ones. */
		sum = 2166136261U;
		for (y = first; y < last; y += 16)
			sum = (sum ^ kwl_row_sum(target + (size_t)y * image->row_pitch, shm->width)) * 16777619U;
		printf("KWL SHM_COPY client=%llu surface=%u buffer=%u rows=%d-%d sum=%08x\n", (unsigned long long)surface->client->number, surface->id,
		       buffer->id, first, last, (unsigned)sum);
	}

	/* The copy is done: the client may reuse the buffer now. */
	if (buffer->busy) {
		buffer->busy = 0;
		if (!buffer->dead && !buffer->client->fatal) {
			error = kwl_emit(buffer->client, buffer->id, 0, NULL, 0);
			if (error != 0) {
				kwl_client_fail(buffer->client);
			}
		}
	}

	/* Succeeded. */
	return 0;
}

/*
 * Creates a host-written BGRA image: a linear staging image in host-visible,
 * coherent memory, mapped for good, and an optimal sampled image that the
 * frame fills from it (kwl_host_images_take), with its view and
 * descriptor set; both in the general layout.
 */
static VkResult
image_create(
	struct kwl_compose *compose,
	uint32_t width,
	uint32_t height,
	VkSampler sampler,
	struct kwl_import *import)
{
	VkPhysicalDeviceMemoryProperties memory;
	VkImageCreateInfo create;
	VkMemoryRequirements requirements;
	VkImageSubresource subresource;
	VkSubresourceLayout layout;
	VkMemoryAllocateInfo allocate;
	VkImageViewCreateInfo view;
	VkDescriptorImageInfo image_info;
	VkWriteDescriptorSet write;
	VkMemoryPropertyFlags wanted;
	uint32_t index;
	VkResult result;

	/* The staging image the CPU writes. */
	import->width = width;
	import->height = height;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.imageType = VK_IMAGE_TYPE_2D;
	create.format = VK_FORMAT_B8G8R8A8_UNORM;
	create.extent.width = width;
	create.extent.height = height;
	create.extent.depth = 1U;
	create.mipLevels = 1U;
	create.arrayLayers = 1U;
	create.samples = VK_SAMPLE_COUNT_1_BIT;
	create.tiling = VK_IMAGE_TILING_LINEAR;
	create.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
	result = vkCreateImage(compose->device, &create, NULL, &import->staging);
	if (result != VK_SUCCESS)
		return result;

	/* A host-visible, coherent memory type it can use. */
	vkGetImageMemoryRequirements(compose->device, import->staging, &requirements);
	vkGetPhysicalDeviceMemoryProperties(compose->physical, &memory);
	wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (index = 0; index < memory.memoryTypeCount; index++) {
		if ((requirements.memoryTypeBits & (1U << index)) != 0U &&
		    (memory.memoryTypes[index].propertyFlags & wanted) == wanted)
			break;
	}

	/* Without one the image cannot be written by the CPU. */
	if (index == memory.memoryTypeCount)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* The staging memory, bound and mapped for good. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = index;
	result = vkAllocateMemory(compose->device, &allocate, NULL, &import->staging_memory);
	if (result != VK_SUCCESS)
		return result;
	result = vkBindImageMemory(compose->device, import->staging, import->staging_memory, 0U);
	if (result != VK_SUCCESS)
		return result;
	result = vkMapMemory(compose->device, import->staging_memory, 0U, VK_WHOLE_SIZE, 0U, &import->map);
	if (result != VK_SUCCESS)
		return result;

	/* Where each row starts in that memory. */
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vkGetImageSubresourceLayout(compose->device, import->staging, &subresource, &layout);
	import->map = (unsigned char *)import->map + layout.offset;
	import->row_pitch = layout.rowPitch;

	/* The sampled image, which no CPU maps. */
	create.tiling = VK_IMAGE_TILING_OPTIMAL;
	create.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	result = vkCreateImage(compose->device, &create, NULL, &import->image);
	if (result != VK_SUCCESS)
		return result;

	/* Device-local memory when the device has it, else any type the image accepts. */
	vkGetImageMemoryRequirements(compose->device, import->image, &requirements);
	for (index = 0; index < memory.memoryTypeCount; index++) {
		if ((requirements.memoryTypeBits & (1U << index)) != 0U &&
		    (memory.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0U)
			break;
	}
	if (index == memory.memoryTypeCount) {
		/* Falls back to the first type the image accepts. */
		for (index = 0; index < memory.memoryTypeCount; index++) {
			if ((requirements.memoryTypeBits & (1U << index)) != 0U)
				break;
		}
	}

	/* Without one the image cannot be sampled. */
	if (index == memory.memoryTypeCount)
		return VK_ERROR_FORMAT_NOT_SUPPORTED;

	/* The sampled memory, bound. */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = index;
	result = vkAllocateMemory(compose->device, &allocate, NULL, &import->memory);
	if (result != VK_SUCCESS)
		return result;
	result = vkBindImageMemory(compose->device, import->image, import->memory, 0U);
	if (result != VK_SUCCESS)
		return result;

	/* The view the shader samples. */
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = import->image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_B8G8R8A8_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1U;
	view.subresourceRange.layerCount = 1U;
	result = vkCreateImageView(compose->device, &view, NULL, &import->view);
	if (result != VK_SUCCESS)
		return result;

	/* The general layout for both: the CPU writes the staging image, copies fill the sampled one. */
	result = image_layout(compose, import->staging, VK_IMAGE_LAYOUT_PREINITIALIZED);
	if (result != VK_SUCCESS)
		return result;

	/* The task that moves them, if any, which the image is let go after. */
	import->task = kwl_compose_task_current(compose);
	result = image_layout(compose, import->image, VK_IMAGE_LAYOUT_UNDEFINED);
	if (result != VK_SUCCESS)
		return result;

	/* Its own number, which tells it apart from an earlier image at the same address (ws213-p003). */
	import->generation = kwl_compose_generation(compose);

	/* The first frame copies whatever the creator writes before it: every row. */
	import->written = 1U;
	import->version = 1U;
	import->dirty_first = 0U;
	import->dirty_last = height;
	import->host_next = compose->host_images;
	compose->host_images = import;

	/* Its descriptor set (a spare one when there is one). */
	result = kwl_compose_set_get(compose, &import->set);
	if (result != VK_SUCCESS)
		return result;
	memset(&image_info, 0, sizeof(image_info));
	image_info.sampler = sampler;
	image_info.imageView = import->view;
	image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = import->set;
	write.dstBinding = 0U;
	write.descriptorCount = 1U;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &image_info;
	vkUpdateDescriptorSets(compose->device, 1U, &write, 0U, NULL);

	/* And the same image sampled linearly. */
	result = kwl_compose_linear_set(compose, import);
	return result;
}

/*
 * Moves a new host-written image to the general layout, keeping what the
 * host wrote: recorded into the task being recorded while the render
 * thread runs (it is given at the next flush, after the host's writes of
 * this pass), into the start's batch while there is one, or submitted
 * and waited for alone.
 */
static VkResult
image_layout(
	struct kwl_compose *compose,
	VkImage image,
	VkImageLayout old_layout)
{
	VkCommandBufferAllocateInfo allocate;
	VkCommandBufferBeginInfo begin;
	VkSubmitInfo submit;
	VkCommandBuffer command;
	VkResult result;

	/* The render thread submits it as a task, ahead of the frames that sample the image. */
	if (compose->render_started) {
		result = kwl_compose_task_record(compose, &command);
		if (result != VK_SUCCESS)
			return result;
		result = image_layout_record(command, image, old_layout);
		return result;
	}

	/* The start's batch: its command buffer made and begun by the first image. */
	if (compose->setup_batching && compose->setup_command != VK_NULL_HANDLE) {
		result = image_layout_record(compose->setup_command, image, old_layout);
		return result;
	}

	/* A command buffer, the batch's or a one-time one (before the render thread, which records from the same pool). */
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	allocate.commandPool = compose->pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1U;
	result = vkAllocateCommandBuffers(compose->device, &allocate, &command);
	if (result != VK_SUCCESS)
		return result;
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(command, &begin);

	/* The move, recorded. */
	if (result == VK_SUCCESS)
		result = image_layout_record(command, image, old_layout);

	/* In the batch, the command buffer waits for the others (kwl_host_image_batch_end). */
	if (result == VK_SUCCESS && compose->setup_batching) {
		compose->setup_command = command;
		return VK_SUCCESS;
	}

	/* Alone: submitted and finished before the image is first drawn. */
	if (result == VK_SUCCESS)
		result = vkEndCommandBuffer(command);
	if (result == VK_SUCCESS) {
		memset(&submit, 0, sizeof(submit));
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1U;
		submit.pCommandBuffers = &command;
		kwl_queue_lock(compose);
		result = vkQueueSubmit(compose->queue, 1U, &submit, VK_NULL_HANDLE);

		/* The barrier is done before the first frame samples the image (the cursor's thread waits for the queue's lock meanwhile). */
		if (result == VK_SUCCESS)
			result = vkQueueWaitIdle(compose->queue);
		kwl_queue_unlock(compose);
	}

	/* The command buffer is not kept. */
	vkFreeCommandBuffers(compose->device, compose->pool, 1U, &command);
	if (result != VK_SUCCESS)
		return result;

	/* Succeeded: the image is in the general layout. */
	return VK_SUCCESS;
}

/* Records one image's move to general: from preinitialized keeping the host's writes, or from undefined. */
static VkResult
image_layout_record(
	VkCommandBuffer command,
	VkImage image,
	VkImageLayout old_layout)
{
	VkImageMemoryBarrier barrier;

	/* The host's writes, before and after this, are seen by later copies and the fragment shader. */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.oldLayout = old_layout;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1U;
	barrier.subresourceRange.layerCount = 1U;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);

	/* Succeeded: the move is recorded. */
	return VK_SUCCESS;
}

/* Takes an image out of the list of host images, so that no later frame copies its staging. */
static void
image_unlink(
	struct kwl_compose *compose,
	struct kwl_import *import)
{
	struct kwl_import **position;

	/* The link to this exact record, if it is still listed. */
	for (position = &compose->host_images; *position != NULL; position = &(*position)->host_next) {
		if (*position == import) {
			*position = import->host_next;
			import->host_next = NULL;
			break;
		}
	}
}

/* Destroys what image_create made, whatever part of it was made. */
static void
image_release(
	struct kwl_compose *compose,
	struct kwl_import *import)
{
	/* Each object, in the reverse order of its making. */
	if (import->set != VK_NULL_HANDLE)
		kwl_compose_set_put(compose, import->set);
	if (import->linear_set != VK_NULL_HANDLE)
		kwl_compose_set_put(compose, import->linear_set);
	if (import->view != VK_NULL_HANDLE)
		vkDestroyImageView(compose->device, import->view, NULL);
	if (import->image != VK_NULL_HANDLE)
		vkDestroyImage(compose->device, import->image, NULL);
	if (import->memory != VK_NULL_HANDLE)
		vkFreeMemory(compose->device, import->memory, NULL);
	if (import->staging != VK_NULL_HANDLE)
		vkDestroyImage(compose->device, import->staging, NULL);
	if (import->staging_memory != VK_NULL_HANDLE)
		vkFreeMemory(compose->device, import->staging_memory, NULL);
	memset(import, 0, sizeof(*import));
}

/* Reads one little-endian wire word. */
static uint32_t
word(
	const unsigned char *bytes,
	size_t offset)
{
	uint32_t value;

	/* The wire is in the host's byte order. */
	memcpy(&value, bytes + offset, sizeof(value));
	return value;
}

/* Returns a sum of a row's pixels (for the frame log). */
static uint32_t
kwl_row_sum(
	const unsigned char *row,
	uint32_t width)
{
	uint32_t sum;
	uint32_t pixel;
	uint32_t x;

	/* Each pixel into the sum. */
	sum = 0U;
	for (x = 0U; x < width; x++) {
		memcpy(&pixel, row + (size_t)x * 4U, sizeof(pixel));
		sum = sum * 31U + pixel;
	}

	/* The sum. */
	return sum;
}

/* Records one host image's copy from its staging into its sampled image (the rows taken), between the frames' reads. */
static void
image_copy_record(
	VkCommandBuffer command,
	const struct kwl_list_copy *copy)
{
	VkImageMemoryBarrier barriers[2];
	VkImageCopy region;

	/* The host's writes reach the copy, and earlier frames' sampling ends before it overwrites. */
	memset(barriers, 0, sizeof(barriers));
	barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barriers[0].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barriers[0].image = copy->staging;
	barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barriers[0].subresourceRange.levelCount = 1U;
	barriers[0].subresourceRange.layerCount = 1U;
	barriers[1] = barriers[0];
	barriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[1].image = copy->image;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0U, 0U, NULL, 0U, NULL, 2U, barriers);

	/* The rows taken, staging to sampled (the whole image when every row was written, q1534). */
	if (copy->first < copy->last) {
		memset(&region, 0, sizeof(region));
		region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.srcSubresource.layerCount = 1U;
		region.dstSubresource = region.srcSubresource;
		region.srcOffset.y = (int32_t)copy->first;
		region.dstOffset.y = (int32_t)copy->first;
		region.extent.width = copy->width;
		region.extent.height = copy->last - copy->first;
		region.extent.depth = 1U;
		vkCmdCopyImage(command, copy->staging, VK_IMAGE_LAYOUT_GENERAL, copy->image, VK_IMAGE_LAYOUT_GENERAL, 1U, &region);
	}

	/* The copy reaches the fragment shader of this frame and the next ones. */
	barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barriers[1]);
}
