/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * wl_shm, the secondary path for client images (WS035 compositing design,
 * D2), and zdesktop's own cursor image (D8).
 *
 * A pool is the client's anonymous shared memory, mapped read-only once at
 * creation and again at resize.  A surface showing a wl_shm buffer has a
 * linear, host-visible Vulkan image of the buffer's size that window mode
 * samples; after a commit the rows the client damaged are copied into it
 * with the CPU, only while no frame is in flight, and the buffer is then
 * released at once: the client can draw its next frame without waiting for
 * the GPU.  The GPU path (zed_gpu_buffer_v1) is not touched by any of this.
 */

#include "compose.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* zdesktop's arrow: its size and hotspot. */
#define ARROW_WIDTH	12U
#define ARROW_HEIGHT	19U

static int pool_create(struct zwl_object *shm, const unsigned char *bytes, size_t size);
static int pool_buffer(struct zwl_object *pool, const unsigned char *bytes, size_t size);
static int pool_resize(struct zwl_object *pool, const unsigned char *bytes, size_t size);
static int surface_upload(struct zwl_server *server, struct zwl_object *surface);
static VkResult image_create(struct zwl_compose *compose, uint32_t width, uint32_t height, VkSampler sampler, struct zwl_import *import);
static VkResult image_layout(struct zwl_compose *compose, VkImage image);
static void image_release(struct zwl_compose *compose, struct zwl_import *import);
static uint32_t word(const unsigned char *bytes, size_t offset);
static uint32_t zwl_row_sum(const unsigned char *row, uint32_t width);

/*
 * Tells a new wl_shm binding the two formats it may use.
 */
int
zwl_shm_bind(
	struct zwl_object *shm)
{
	uint32_t format;
	int error;

	/* ARGB8888, then XRGB8888. */
	format = ZWL_SHM_ARGB8888;
	error = zwl_emit(shm->client, shm->id, 0, &format, sizeof(format));
	if (error != 0)
		return error;
	format = ZWL_SHM_XRGB8888;
	error = zwl_emit(shm->client, shm->id, 0, &format, sizeof(format));
	return error;
}

/*
 * Handles the requests of wl_shm (create_pool) and wl_shm_pool
 * (create_buffer, destroy, resize).
 */
int
zwl_shm_request(
	struct zwl_object *object,
	uint32_t opcode,
	const unsigned char *bytes,
	size_t size)
{
	int error;

	/* wl_shm has one request. */
	if (object->kind == ZWL_SHM) {
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
		zwl_object_destroy(object);
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
zwl_pool_put(
	struct zwl_pool *pool)
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
 * image window mode samples, and releases the buffers.  Only while no frame
 * is in flight: the frame may be sampling those images.
 */
int
zwl_shm_upload(
	struct zwl_server *server)
{
	struct zwl_client *client;
	struct zwl_object *surface;
	int error;

	/* Without window mode, or with a frame in flight, the copies wait. */
	if (server->compose == NULL || server->compose->in_flight)
		return 0;

	/* Each surface whose committed wl_shm image has not been copied. */
	for (client = server->clients; client != NULL; client = client->next) {
		if (client->fatal)
			continue;
		for (surface = client->objects; surface != NULL; surface = surface->next) {
			if (surface->kind != ZWL_SURFACE || surface->dead || !surface->shm_upload)
				continue;

			/* A surface that cannot be copied is the client's protocol error. */
			error = surface_upload(server, surface);
			if (error != 0) {
				(void)zwl_error(client, surface->id, "wl_shm image could not be copied");
				surface->shm_upload = 0;
			}
		}
	}

	/* Succeeded. */
	return 0;
}

/*
 * Releases a surface's wl_shm image: now when no frame is in flight,
 * otherwise after the frame (it may be sampling it).
 */
void
zwl_shm_image_destroy(
	struct zwl_server *server,
	struct zwl_object *surface)
{
	struct zwl_compose *compose;

	/* A surface without an image has nothing to release. */
	compose = server->compose;
	if (surface->shm_image == NULL || compose == NULL)
		return;

	/* A frame in flight finishes first. */
	zwl_compose_quiesce(server);

	/* The Vulkan objects, then the record. */
	image_release(compose, surface->shm_image);
	free(surface->shm_image);
	surface->shm_image = NULL;
}

/*
 * Makes zdesktop's arrow cursor: white with a black edge, its tip the
 * hotspot at (0, 0).
 */
int
zwl_arrow_create(
	struct zwl_server *server)
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
	struct zwl_import *arrow;
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
		image_release(server->compose, arrow);
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

	/* Succeeded: drawn with alpha. */
	arrow->draw = ZWL_DRAW_ALPHA;
	server->arrow = arrow;
	return 0;
}

/*
 * Releases the arrow cursor.
 */
void
zwl_arrow_destroy(
	struct zwl_server *server)
{
	/* No arrow was made. */
	if (server->arrow == NULL || server->compose == NULL)
		return;

	/* Its Vulkan objects, then the record. */
	image_release(server->compose, server->arrow);
	free(server->arrow);
	server->arrow = NULL;
}

/*
 * Creates a host-written image for zdesktop's own drawing (the glass look),
 * sampled with the given sampler.
 */
VkResult
zwl_host_image_create(
	struct zwl_compose *compose,
	uint32_t width,
	uint32_t height,
	VkSampler sampler,
	struct zwl_import *import)
{
	VkResult result;

	/* The same image as a wl_shm copy's. */
	result = image_create(compose, width, height, sampler, import);
	return result;
}

/*
 * Releases an image zwl_host_image_create made.
 */
void
zwl_host_image_release(
	struct zwl_compose *compose,
	struct zwl_import *import)
{
	/* Whatever part of it was made. */
	image_release(compose, import);
}

/* Maps a client's fd as a new pool (wl_shm.create_pool). */
static int
pool_create(
	struct zwl_object *shm,
	const unsigned char *bytes,
	size_t size)
{
	struct zwl_object *object;
	struct zwl_pool *pool;
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
	descriptor = zwl_take_fd(shm->client);
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
	object = zwl_create(shm->client, id, ZWL_SHM_POOL, 1);
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
	struct zwl_object *pool,
	const unsigned char *bytes,
	size_t size)
{
	struct zwl_shm_buffer *shm;
	struct zwl_object *buffer;
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
	if (format != ZWL_SHM_ARGB8888 && format != ZWL_SHM_XRGB8888)
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
	buffer = zwl_create(pool->client, word(bytes, 0), ZWL_BUFFER, 1);
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
	struct zwl_object *pool,
	const unsigned char *bytes,
	size_t size)
{
	struct zwl_pool *memory;
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
	struct zwl_server *server,
	struct zwl_object *surface)
{
	const struct zwl_shm_buffer *shm;
	struct zwl_object *buffer;
	struct zwl_import *image;
	const unsigned char *source;
	unsigned char *target;
	uint64_t mark;
	uint32_t sum;
	int32_t first;
	int32_t last;
	int32_t y;
	VkResult result;
	int error;

	/* The committed buffer, if it is still a wl_shm one. */
	buffer = surface->current;
	surface->shm_upload = 0;
	if (buffer == NULL || buffer->shm == NULL)
		return 0;
	shm = buffer->shm;

	/* An image of another size is made again, and then filled completely. */
	image = surface->shm_image;
	first = 0;
	last = (int32_t)shm->height;
	if (image == NULL || image->width != shm->width || image->height != shm->height) {
		zwl_shm_image_destroy(server, surface);
		image = calloc(1, sizeof(*image));
		if (image == NULL)
			return ENOMEM;
		result = image_create(server->compose, shm->width, shm->height, server->compose->sampler, image);
		if (result != VK_SUCCESS) {
			printf("ZWL VULKAN_ERROR operation=shm_image result=%d\n", (int)result);
			image_release(server->compose, image);
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
	image->draw = ZWL_DRAW_ALPHA;
	if (shm->format == ZWL_SHM_XRGB8888)
		image->draw = ZWL_DRAW_OPAQUE;

	/* The rows, from the pool into the image (same byte order: B, G, R, A). */
	mark = zwl_cycles();
	source = (const unsigned char *)shm->pool->map + shm->offset;
	target = image->map;
	for (y = first; y < last; y++)
		memcpy(target + (size_t)y * image->row_pitch, source + (size_t)y * shm->stride, (size_t)shm->width * 4U);
	server->perf.shm_copies++;
	server->perf.shm_copy_cycles += zwl_cycles() - mark;
	surface->committed_damaged = 0;
	server->dirty = 1;
	if (server->log_frames) {
		/* With the sum of every sixteenth row copied, so that changing pictures can be told from still ones. */
		sum = 2166136261U;
		for (y = first; y < last; y += 16)
			sum = (sum ^ zwl_row_sum(target + (size_t)y * image->row_pitch, shm->width)) * 16777619U;
		printf("ZWL SHM_COPY client=%llu surface=%u buffer=%u rows=%d-%d sum=%08x\n", (unsigned long long)surface->client->number, surface->id,
		       buffer->id, first, last, (unsigned)sum);
	}

	/* The copy is done: the client may reuse the buffer now. */
	if (buffer->busy) {
		buffer->busy = 0;
		if (!buffer->dead && !buffer->client->fatal) {
			error = zwl_emit(buffer->client, buffer->id, 0, NULL, 0);
			if (error != 0) {
				buffer->client->fatal = 1;
				buffer->client->fatal_time = zwl_milliseconds();
			}
		}
	}

	/* Succeeded. */
	return 0;
}

/*
 * Creates a linear BGRA image in host-visible, coherent memory, mapped for
 * good, with its view and descriptor set, in the general layout.
 */
static VkResult
image_create(
	struct zwl_compose *compose,
	uint32_t width,
	uint32_t height,
	VkSampler sampler,
	struct zwl_import *import)
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

	/* The image. */
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
	create.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;
	result = vkCreateImage(compose->device, &create, NULL, &import->image);
	if (result != VK_SUCCESS)
		return result;

	/* A host-visible, coherent memory type it can use. */
	vkGetImageMemoryRequirements(compose->device, import->image, &requirements);
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

	/* The memory, bound and mapped for good. */
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
	result = vkMapMemory(compose->device, import->memory, 0U, VK_WHOLE_SIZE, 0U, &import->map);
	if (result != VK_SUCCESS)
		return result;

	/* Where each row starts in that memory. */
	memset(&subresource, 0, sizeof(subresource));
	subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	vkGetImageSubresourceLayout(compose->device, import->image, &subresource, &layout);
	import->map = (unsigned char *)import->map + layout.offset;
	import->row_pitch = layout.rowPitch;

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

	/* The general layout, where the CPU writes and the shader reads. */
	result = image_layout(compose, import->image);
	if (result != VK_SUCCESS)
		return result;

	/* Its descriptor set (a spare one when there is one). */
	result = zwl_compose_set_get(compose, &import->set);
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
	result = zwl_compose_linear_set(compose, import);
	return result;
}

/* Moves a new host-written image to the general layout, keeping what the host wrote. */
static VkResult
image_layout(
	struct zwl_compose *compose,
	VkImage image)
{
	VkCommandBufferAllocateInfo allocate;
	VkCommandBufferBeginInfo begin;
	VkImageMemoryBarrier barrier;
	VkSubmitInfo submit;
	VkCommandBuffer command;
	VkResult result;

	/* A one-time command buffer. */
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

	/* From preinitialized (the host's writes kept) to general. */
	if (result == VK_SUCCESS) {
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
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0U, 0U, NULL, 0U, NULL, 1U, &barrier);
		result = vkEndCommandBuffer(command);
	}

	/* Submitted and finished before the image is first drawn. */
	if (result == VK_SUCCESS) {
		memset(&submit, 0, sizeof(submit));
		submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submit.commandBufferCount = 1U;
		submit.pCommandBuffers = &command;
		result = vkQueueSubmit(compose->queue, 1U, &submit, VK_NULL_HANDLE);
	}

	/* The barrier is done before the first frame samples the image. */
	if (result == VK_SUCCESS)
		result = vkQueueWaitIdle(compose->queue);

	/* The command buffer is not kept. */
	vkFreeCommandBuffers(compose->device, compose->pool, 1U, &command);
	return result;
}

/* Destroys what image_create made, whatever part of it was made. */
static void
image_release(
	struct zwl_compose *compose,
	struct zwl_import *import)
{
	/* Each object, in the reverse order of its making. */
	if (import->set != VK_NULL_HANDLE)
		zwl_compose_set_put(compose, import->set);
	if (import->linear_set != VK_NULL_HANDLE)
		zwl_compose_set_put(compose, import->linear_set);
	if (import->view != VK_NULL_HANDLE)
		vkDestroyImageView(compose->device, import->view, NULL);
	if (import->image != VK_NULL_HANDLE)
		vkDestroyImage(compose->device, import->image, NULL);
	if (import->memory != VK_NULL_HANDLE)
		vkFreeMemory(compose->device, import->memory, NULL);
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
zwl_row_sum(
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
