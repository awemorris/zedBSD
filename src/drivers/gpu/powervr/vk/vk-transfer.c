/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's transfer commands: vkCmdCopyBuffer,
 * vkCmdCopyBufferToImage, vkCmdCopyImageToBuffer, vkCmdCopyImage,
 * vkCmdBlitImage (scaled, mirrored, nearest or linear) and
 * vkCmdClearColorImage, recorded with their regions and run as transfers
 * on the session's transfer context.
 *
 * Each region names layer 0 of its image, and level 0 but for a blit and
 * a copy between a buffer and an image, which may name any level; other
 * subresources are refused (ENOTSUP) when recorded.  A buffer copy moves whole 32-bit
 * words at 4-byte aligned offsets; others are refused (ENOTSUP).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/powervr/transfer-job.h"
#include "drivers/gpu/powervr/transfer-program.h"
#include "drivers/gpu/powervr/vk/vk.h"
#include "drivers/gpu/i915/vk/vk-codec.inc"

/* The most regions one transfer command records. */
#define TRANSFER_REGIONS_MAX		8U

/*
 * The words a buffer copy moves in each row of its rectangles (a row of
 * 4 KiB), and the most rows of one rectangle.
 *
 * Values referenced: Mesa 26.2.5 src/imagination/vulkan/rogue/pvr_blit.c
 * (PVR_MAX_TRANSFER_SIZE_IN_TEXELS 2048, the most of either edge).
 */
#define BUFFER_ROW_WORDS		1024U
#define BUFFER_ROWS_MAX			2048U

/* What a transfer record keeps: its regions, and a clear's colour. */
struct transfer_data {
	uint32_t count;
	uint32_t filter;
	uint32_t colour[4];
	union {
		VkImageCopy copies[TRANSFER_REGIONS_MAX];
		VkImageBlit blits[TRANSFER_REGIONS_MAX];
		VkBufferImageCopy buffers[TRANSFER_REGIONS_MAX];
		VkImageSubresourceRange ranges[TRANSFER_REGIONS_MAX];
		VkBufferCopy spans[TRANSFER_REGIONS_MAX];
	} regions;
};

/* The working state of one run: the request, the views it names and their layouts (a buffer's, a further level's). */
struct transfer_work {
	struct powervr_transfer_request request;
	struct powervr_view_shape view;
	struct powervr_image_layout layout;
	struct powervr_image_layout level_layout;
};

static int hold(struct powervr_vk_session *session, struct powervr_vk_record *entry, enum i915_vk_object_kind kind, uint64_t identity);
static int first_subresource(uint32_t level, uint32_t layer, uint32_t count);
static int first_layer(uint32_t layer, uint32_t count);
static int image_surface(const struct powervr_vk_image *image, uint32_t level, struct powervr_render_surface *surface);
static int level_edges(const struct powervr_vk_image *image, uint32_t level, uint32_t *width, uint32_t *height, uint32_t *padded_width);
static int image_view(const struct powervr_vk_image *image, uint32_t level, struct powervr_view_shape *view, struct powervr_image_layout *level_layout);
static void blit_request(const VkImageBlit *blit, uint32_t filter, struct powervr_transfer_request *request);
static int run_one(struct powervr_vk_session *session, struct powervr_transfer_request *request, bool writes_buffer);
static int buffer_copy_run(struct powervr_vk_session *session, const struct powervr_vk_record *entry, const VkBufferCopy *span, struct transfer_work *work);
static void buffer_rows(struct transfer_work *work, uint64_t source, uint64_t destination, uint32_t width, uint32_t height);

/*
 * Records one transfer command: its objects (the source as object 0,
 * the destination as object 1; a clear's image alone) and its regions.
 *
 * Procedure referenced: zedBSD src/drivers/gpu/amdrdna/vk/vk-record.c
 * (record_copy, record_clear: the same wire forms).
 */
int
powervr_vk_record_transfer(
	struct powervr_vk_session *session,
	uint32_t opcode,
	struct i915_wire_reader *reader,
	struct powervr_vk_record *entry)
{
	struct transfer_data *data;
	enum i915_vk_object_kind source_kind;
	enum i915_vk_object_kind destination_kind;
	uint64_t source;
	uint64_t destination;
	uint64_t count;
	uint64_t index;
	VkImageBlit *blit;
	int refused;
	int error;

	/* The record's data. */
	data = kern_calloc(1, sizeof(*data));
	if (data == NULL)
		return ENOMEM;
	entry->data = data;

	/* A clear: [image][layout][present]{colour}[count][count]{range}. */
	if (opcode == GPU_OP_CMD_CLEAR_COLOR_IMAGE) {
		destination = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u64(reader);
		for (index = 0; index < 4U; index++)
			data->colour[index] = drv_i915_wire_read_u32(reader);
		(void)drv_i915_wire_read_u32(reader);
		count = drv_i915_wire_read_u64(reader);
		if (reader->error != 0 || count > TRANSFER_REGIONS_MAX)
			return EINVAL;
		for (index = 0; index < count; index++)
			i915_vkc_dec_VkImageSubresourceRange(reader, &session->arena, &data->regions.ranges[index]);
		data->count = (uint32_t)count;
		if (reader->error != 0)
			return EINVAL;
		error = hold(session, entry, I915_VK_OBJ_IMAGE, destination);
		if (error != 0)
			return error;

		/* Each range: level 0, layer 0 only. */
		for (index = 0; index < count; index++) {
			error = first_subresource(data->regions.ranges[index].baseMipLevel, data->regions.ranges[index].baseArrayLayer, data->regions.ranges[index].layerCount);
			if (error != 0)
				return error;
		}
		return 0;
	}

	/* A buffer copy: [source][destination][u32 count][u64 count]{VkBufferCopy}, whole aligned words. */
	if (opcode == GPU_OP_CMD_COPY_BUFFER) {
		source = drv_i915_wire_read_u64(reader);
		destination = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
		count = drv_i915_wire_read_u64(reader);
		if (reader->error != 0 || count > TRANSFER_REGIONS_MAX)
			return EINVAL;
		for (index = 0; index < count; index++)
			i915_vkc_dec_VkBufferCopy(reader, &session->arena, &data->regions.spans[index]);
		data->count = (uint32_t)count;
		if (reader->error != 0)
			return EINVAL;
		error = hold(session, entry, I915_VK_OBJ_BUFFER, source);
		if (error == 0)
			error = hold(session, entry, I915_VK_OBJ_BUFFER, destination);
		if (error != 0)
			return error;
		for (index = 0; index < count; index++) {
			if (((data->regions.spans[index].srcOffset | data->regions.spans[index].dstOffset | data->regions.spans[index].size) & 3U) != 0)
				return ENOTSUP;
		}
		return 0;
	}

	/* The two resources, as each command lays them out. */
	source_kind = I915_VK_OBJ_IMAGE;
	destination_kind = I915_VK_OBJ_IMAGE;
	if (opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE) {
		source_kind = I915_VK_OBJ_BUFFER;
		source = drv_i915_wire_read_u64(reader);
		destination = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
	} else if (opcode == GPU_OP_CMD_COPY_IMAGE_TO_BUFFER) {
		destination_kind = I915_VK_OBJ_BUFFER;
		source = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
		destination = drv_i915_wire_read_u64(reader);
	} else {
		source = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
		destination = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
	}

	/* The regions. */
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0 || count > TRANSFER_REGIONS_MAX)
		return EINVAL;
	for (index = 0; index < count; index++) {
		if (opcode == GPU_OP_CMD_COPY_IMAGE) {
			i915_vkc_dec_VkImageCopy(reader, &session->arena, &data->regions.copies[index]);
		} else if (opcode == GPU_OP_CMD_BLIT_IMAGE) {
			i915_vkc_dec_VkImageBlit(reader, &session->arena, &data->regions.blits[index]);
		} else {
			i915_vkc_dec_VkBufferImageCopy(reader, &session->arena, &data->regions.buffers[index]);
		}
	}
	data->count = (uint32_t)count;

	/* A blit's filter. */
	if (opcode == GPU_OP_CMD_BLIT_IMAGE)
		data->filter = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Both resources held. */
	error = hold(session, entry, source_kind, source);
	if (error == 0)
		error = hold(session, entry, destination_kind, destination);
	if (error != 0)
		return error;

	/* Layer 0 of each image, and level 0 of both images of a copy between images. */
	refused = 0;
	for (index = 0; refused == 0 && index < count; index++) {
		if (opcode == GPU_OP_CMD_COPY_IMAGE) {
			refused = first_subresource(data->regions.copies[index].srcSubresource.mipLevel, data->regions.copies[index].srcSubresource.baseArrayLayer, data->regions.copies[index].srcSubresource.layerCount);
			if (refused == 0)
				refused = first_subresource(data->regions.copies[index].dstSubresource.mipLevel, data->regions.copies[index].dstSubresource.baseArrayLayer, data->regions.copies[index].dstSubresource.layerCount);
		} else if (opcode == GPU_OP_CMD_BLIT_IMAGE) {
			blit = &data->regions.blits[index];
			refused = first_layer(blit->srcSubresource.baseArrayLayer, blit->srcSubresource.layerCount);
			if (refused == 0)
				refused = first_layer(blit->dstSubresource.baseArrayLayer, blit->dstSubresource.layerCount);
			if (refused == 0 && (blit->srcOffsets[0].x < 0 || blit->srcOffsets[0].y < 0 || blit->srcOffsets[1].x < 0 || blit->srcOffsets[1].y < 0 ||
			    blit->dstOffsets[0].x < 0 || blit->dstOffsets[0].y < 0 || blit->dstOffsets[1].x < 0 || blit->dstOffsets[1].y < 0))
				refused = EINVAL;
		} else {
			refused = first_layer(data->regions.buffers[index].imageSubresource.baseArrayLayer, data->regions.buffers[index].imageSubresource.layerCount);
		}
	}
	if (refused != 0)
		return refused;

	/* Succeeded: the transfer is recorded. */
	return 0;
}

/*
 * Runs one recorded transfer command, region by region; ETIMEDOUT when
 * one did not finish, the session then abandoned.
 */
int
powervr_vk_transfer_run(
	struct powervr_vk_session *session,
	const struct powervr_vk_record *entry)
{
	struct transfer_work *work;
	const struct transfer_data *data;
	const struct powervr_vk_image *image;
	const struct powervr_vk_image *other;
	const struct powervr_vk_buffer *buffer;
	const VkBufferImageCopy *copy;
	uint64_t address;
	uint32_t row;
	uint32_t height;
	uint32_t index;
	int error;

	/* The working state, and the record's data. */
	work = kern_calloc(1, sizeof(*work));
	if (work == NULL)
		return ENOMEM;
	data = entry->data;
	error = 0;
	for (index = 0; error == 0 && index < data->count; index++) {
		kern_memset(&work->request, 0, sizeof(work->request));
		if (entry->opcode == GPU_OP_CMD_COPY_BUFFER) {
			/* A buffer copy runs its own transfers. */
			error = buffer_copy_run(session, entry, &data->regions.spans[index], work);
			continue;
		} else if (entry->opcode == GPU_OP_CMD_CLEAR_COLOR_IMAGE) {
			/* The whole of level 0 to the colour. */
			image = entry->objects[0]->payload;
			work->request.kind = POWERVR_TRANSFER_CLEAR;
			error = image_surface(image, 0, &work->request.destination);
			work->request.area.width = image->shape.width;
			work->request.area.height = image->shape.height;
			kern_memcpy(work->request.colour, data->colour, sizeof(work->request.colour));
		} else if (entry->opcode == GPU_OP_CMD_COPY_IMAGE || entry->opcode == GPU_OP_CMD_BLIT_IMAGE) {
			/* From the source image's view to the destination's rectangle. */
			image = entry->objects[1]->payload;
			other = entry->objects[0]->payload;
			work->request.kind = POWERVR_TRANSFER_COPY;
			if (entry->opcode == GPU_OP_CMD_BLIT_IMAGE) {
				error = image_surface(image, data->regions.blits[index].dstSubresource.mipLevel, &work->request.destination);
				if (error == 0)
					error = image_view(other, data->regions.blits[index].srcSubresource.mipLevel, &work->view, &work->level_layout);
			} else {
				error = image_surface(image, 0, &work->request.destination);
				if (error == 0)
					error = image_view(other, 0, &work->view, &work->level_layout);
			}
			/* A copy moves raw texels: the two formats need the same texel size. */
			if (error == 0 && entry->opcode == GPU_OP_CMD_COPY_IMAGE && other->format->texel_bytes != image->format->texel_bytes)
				error = ENOTSUP;
			if (entry->opcode == GPU_OP_CMD_COPY_IMAGE) {
				work->request.area.x = (uint32_t)data->regions.copies[index].dstOffset.x;
				work->request.area.y = (uint32_t)data->regions.copies[index].dstOffset.y;
				work->request.area.width = data->regions.copies[index].extent.width;
				work->request.area.height = data->regions.copies[index].extent.height;
				work->request.source_x = (uint32_t)data->regions.copies[index].srcOffset.x;
				work->request.source_y = (uint32_t)data->regions.copies[index].srcOffset.y;
			} else {
				blit_request(&data->regions.blits[index], data->filter, &work->request);
			}
			work->request.source = &work->view;
		} else {
			/* Between a buffer laid out as linear rows of the image's texels and the image. */
			copy = &data->regions.buffers[index];
			if (entry->opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE) {
				buffer = entry->objects[0]->payload;
				image = entry->objects[1]->payload;
			} else {
				image = entry->objects[0]->payload;
				buffer = entry->objects[1]->payload;
			}
			row = copy->bufferRowLength != 0 ? copy->bufferRowLength : copy->imageExtent.width;
			height = copy->bufferImageHeight != 0 ? copy->bufferImageHeight : copy->imageExtent.height;
			error = powervr_vk_buffer_address(buffer, &address);
			address += copy->bufferOffset;
			work->request.kind = POWERVR_TRANSFER_COPY;
			if (error == 0 && entry->opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE) {
				/* The buffer as a linear view, read from its first texel. */
				error = image_surface(image, copy->imageSubresource.mipLevel, &work->request.destination);
				kern_memset(&work->layout, 0, sizeof(work->layout));
				work->layout.row_pitch = row * image->format->texel_bytes;
				work->layout.padded_width = row;
				work->layout.padded_height = height;
				work->layout.padded_depth = 1;
				work->layout.layer_bytes = (uint64_t)work->layout.row_pitch * height;
				work->layout.bytes = work->layout.layer_bytes;
				kern_memset(&work->view, 0, sizeof(work->view));
				work->view.format = image->format;
				work->view.layout = &work->layout;
				work->view.address = address;
				work->view.width = row;
				work->view.height = height;
				work->view.image_levels = 1;
				work->view.level_count = 1;
				work->view.layer_count = 1;
				work->view.linear = 1;
				work->request.area.x = (uint32_t)copy->imageOffset.x;
				work->request.area.y = (uint32_t)copy->imageOffset.y;
				work->request.source = &work->view;
			} else if (error == 0) {
				/* The buffer as a linear destination, written from its first texel. */
				error = image_view(image, copy->imageSubresource.mipLevel, &work->view, &work->level_layout);
				work->request.destination.format = image->format;
				work->request.destination.address = address;
				work->request.destination.width = row;
				work->request.destination.height = height;
				work->request.destination.linear = 1;
				work->request.destination.row_pitch = row * image->format->texel_bytes;
				work->request.destination.padded_width = row;
				work->request.source_x = (uint32_t)copy->imageOffset.x;
				work->request.source_y = (uint32_t)copy->imageOffset.y;
				work->request.source = &work->view;
			}
			work->request.area.width = copy->imageExtent.width;
			work->request.area.height = copy->imageExtent.height;
		}

		/* The region's transfer (into a buffer for an image's copy to one). */
		if (error == 0)
			error = run_one(session, &work->request, entry->opcode == GPU_OP_CMD_COPY_IMAGE_TO_BUFFER);
	}

	/* The working state goes; a transfer that could not run. */
	kern_free(work);
	if (error != 0)
		return error;

	/* Succeeded: every region is done. */
	return 0;
}

/* Holds an object of a kind for a record, refusing an unknown one. */
static int
hold(
	struct powervr_vk_session *session,
	struct powervr_vk_record *entry,
	enum i915_vk_object_kind kind,
	uint64_t identity)
{
	struct powervr_vk_object *object;
	int error;

	/* The object, held. */
	object = powervr_vk_object_find(session, kind, identity);
	if (object == NULL)
		return EINVAL;

	error = powervr_vk_object_retain(object);
	if (error != 0)
		return EINVAL;
	entry->objects[entry->object_count++] = object;

	/* Succeeded: the object is the record's. */
	return 0;
}

/*
 * Refuses a subresource beyond level 0 and layer 0; the remaining layers
 * from layer 0 (VK_REMAINING_ARRAY_LAYERS) are the one layer the images
 * have.
 */
static int
first_subresource(
	uint32_t level,
	uint32_t layer,
	uint32_t count)
{
	/* Level 0, layer 0, one layer or all of them. */
	if (level != 0 || layer != 0 || (count != 1 && count != VK_REMAINING_ARRAY_LAYERS))
		return ENOTSUP;

	/* Succeeded: the first subresource. */
	return 0;
}

/*
 * Refuses a layer but the first: the images have one layer (the
 * remaining layers from layer 0, VK_REMAINING_ARRAY_LAYERS, are it).
 */
static int
first_layer(
	uint32_t layer,
	uint32_t count)
{
	/* Layer 0, one layer or all of them. */
	if (layer != 0 || (count != 1 && count != VK_REMAINING_ARRAY_LAYERS))
		return ENOTSUP;

	/* Succeeded: the first layer. */
	return 0;
}

/*
 * Works out a level of a twiddled image as an image of its own: the
 * level's edges (halved per level, at least one) and its padded row,
 * which must be the power of two the level's own width rounds up to
 * (the core twiddles a surface by its edges rounded up), else ENOTSUP.
 *
 * Procedure referenced: Mesa 26.2.5 src/imagination/vulkan/pvr_image.c
 * (pvr_image_plane_setup_mip_levels: each level's physical extent the
 * level-0 one halved) and src/imagination/vulkan/rogue/pvr_blit.c
 * (pvr_setup_transfer_surface: a level as a surface of its own at the
 * level's offset).
 */
static int
level_edges(
	const struct powervr_vk_image *image,
	uint32_t level,
	uint32_t *width,
	uint32_t *height,
	uint32_t *padded_width)
{
	uint32_t padded_height;
	uint32_t power;

	/* A level the image has. */
	if (level >= image->shape.levels || level >= POWERVR_IMAGE_LEVELS_MAX)
		return EINVAL;

	/* The level's edges and padded edges. */
	*width = image->shape.width >> level;
	if (*width == 0)
		*width = 1;
	*height = image->shape.height >> level;
	if (*height == 0)
		*height = 1;
	*padded_width = image->layout.padded_width >> level;
	if (*padded_width == 0)
		*padded_width = 1;
	padded_height = image->layout.padded_height >> level;
	if (padded_height == 0)
		padded_height = 1;

	/* Level 0 is the image itself; a level beyond it must twiddle as its padded edges. */
	if (level == 0)
		return 0;
	if (image->shape.linear)
		return ENOTSUP;
	power = 1;
	while (power < *width)
		power <<= 1;
	if (power != *padded_width)
		return ENOTSUP;
	power = 1;
	while (power < *height)
		power <<= 1;
	if (power != padded_height)
		return ENOTSUP;

	/* Succeeded: the level's edges. */
	return 0;
}

/* Describes one level of an image as a destination: the level's place, edges and rows. */
static int
image_surface(
	const struct powervr_vk_image *image,
	uint32_t level,
	struct powervr_render_surface *surface)
{
	uint64_t address;
	uint32_t width;
	uint32_t height;
	uint32_t padded;
	int error;

	/* The level's edges, and the image's memory. */
	error = level_edges(image, level, &width, &height, &padded);
	if (error != 0)
		return error;
	error = powervr_vk_image_address(image, &address);
	if (error != 0)
		return error;

	/* Its format, the level's address, edges and rows. */
	surface->format = image->format;
	surface->address = address + image->layout.level_offset[level];
	surface->width = width;
	surface->height = height;
	surface->linear = image->shape.linear;
	surface->row_pitch = image->layout.row_pitch;
	surface->padded_width = image->layout.padded_width;
	if (level != 0) {
		surface->padded_width = padded;
		surface->row_pitch = padded * image->format->texel_bytes;
	}

	/* Succeeded: the surface. */
	return 0;
}

/*
 * Describes one level of an image as a source view: level 0 as the
 * image, a further level as an image of one level of its own at the
 * level's offset, its layout written to level_layout.
 */
static int
image_view(
	const struct powervr_vk_image *image,
	uint32_t level,
	struct powervr_view_shape *view,
	struct powervr_image_layout *level_layout)
{
	uint64_t address;
	uint32_t width;
	uint32_t height;
	uint32_t padded;
	int error;

	/* The level's edges, and the image's memory. */
	error = level_edges(image, level, &width, &height, &padded);
	if (error != 0)
		return error;
	error = powervr_vk_image_address(image, &address);
	if (error != 0)
		return error;

	/* All of level 0 and layer 0, the identity mapping. */
	kern_memset(view, 0, sizeof(*view));
	view->format = image->format;
	view->layout = &image->layout;
	view->address = address;
	view->width = image->shape.width;
	view->height = image->shape.height;
	view->image_levels = image->shape.levels;
	view->level_count = 1;
	view->layer_count = 1;
	view->linear = image->shape.linear;

	/* A further level: an image of its own of that level. */
	if (level != 0) {
		kern_memset(level_layout, 0, sizeof(*level_layout));
		level_layout->row_pitch = padded * image->format->texel_bytes;
		level_layout->padded_width = padded;
		level_layout->padded_height = image->layout.padded_height >> level;
		if (level_layout->padded_height == 0)
			level_layout->padded_height = 1;
		level_layout->padded_depth = 1;
		level_layout->layer_bytes = (uint64_t)level_layout->row_pitch * level_layout->padded_height;
		level_layout->bytes = level_layout->layer_bytes;
		view->layout = level_layout;
		view->address = address + image->layout.level_offset[level];
		view->width = width;
		view->height = height;
		view->image_levels = 1;
	}

	/* Succeeded: the view. */
	return 0;
}

/*
 * Fills a blit's request: the destination and source rectangles from
 * their corners in either order, each axis flipped when the two
 * rectangles run in opposite directions along it, linear for
 * VK_FILTER_LINEAR (1).
 */
static void
blit_request(
	const VkImageBlit *blit,
	uint32_t filter,
	struct powervr_transfer_request *request)
{
	int32_t low;
	int32_t high;

	/* The destination rectangle. */
	request->kind = POWERVR_TRANSFER_BLIT;
	low = blit->dstOffsets[0].x < blit->dstOffsets[1].x ? blit->dstOffsets[0].x : blit->dstOffsets[1].x;
	high = blit->dstOffsets[0].x < blit->dstOffsets[1].x ? blit->dstOffsets[1].x : blit->dstOffsets[0].x;
	request->area.x = (uint32_t)low;
	request->area.width = (uint32_t)(high - low);
	low = blit->dstOffsets[0].y < blit->dstOffsets[1].y ? blit->dstOffsets[0].y : blit->dstOffsets[1].y;
	high = blit->dstOffsets[0].y < blit->dstOffsets[1].y ? blit->dstOffsets[1].y : blit->dstOffsets[0].y;
	request->area.y = (uint32_t)low;
	request->area.height = (uint32_t)(high - low);

	/* The source rectangle. */
	low = blit->srcOffsets[0].x < blit->srcOffsets[1].x ? blit->srcOffsets[0].x : blit->srcOffsets[1].x;
	high = blit->srcOffsets[0].x < blit->srcOffsets[1].x ? blit->srcOffsets[1].x : blit->srcOffsets[0].x;
	request->source_area.x = (uint32_t)low;
	request->source_area.width = (uint32_t)(high - low);
	low = blit->srcOffsets[0].y < blit->srcOffsets[1].y ? blit->srcOffsets[0].y : blit->srcOffsets[1].y;
	high = blit->srcOffsets[0].y < blit->srcOffsets[1].y ? blit->srcOffsets[1].y : blit->srcOffsets[0].y;
	request->source_area.y = (uint32_t)low;
	request->source_area.height = (uint32_t)(high - low);

	/* An axis whose rectangles run in opposite directions is flipped. */
	request->flip_x = (blit->srcOffsets[0].x > blit->srcOffsets[1].x) != (blit->dstOffsets[0].x > blit->dstOffsets[1].x);
	request->flip_y = (blit->srcOffsets[0].y > blit->srcOffsets[1].y) != (blit->dstOffsets[0].y > blit->dstOffsets[1].y);
	request->linear = filter == 1U;
}

/*
 * Runs one region's transfer, after the session's work before it and
 * left running; one that did not finish abandons the session.  One that
 * writes a buffer is noted, for a later render's geometry to wait for.
 */
static int
run_one(
	struct powervr_vk_session *session,
	struct powervr_transfer_request *request,
	bool writes_buffer)
{
	struct powervr_session *owner;
	int error;

	/* The transfer on the session's transfer context. */
	owner = session->owner;
	error = powervr_transfer_run_async(owner->render, owner, request);
	if (error == ETIMEDOUT)
		session->abandoned = true;
	if (error != 0) {
		kern_logf("powervr: vk: a transfer did not run (%d)\n", error);
		return error;
	}

	/* The transfer's point, when it wrote a buffer. */
	if (writes_buffer) {
		owner->transfer_wrote_buffer = 1;
		owner->transfer_buffer_value = owner->last_value;
	}

	/* Succeeded: the region is done. */
	return 0;
}

/*
 * Runs one region of a buffer copy: the words as rows of BUFFER_ROW_WORDS
 * 32-bit texels in rectangles of up to BUFFER_ROWS_MAX rows, then the
 * words left over as one shorter row, each a same-format copy between
 * linear surfaces.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/rogue/pvr_blit.c
 * (pvr_cmd_copy_buffer_region: the buffer copy
 * as transfers of linear rectangles of raw texels, the remainder as a
 * row of its own).
 */
static int
buffer_copy_run(
	struct powervr_vk_session *session,
	const struct powervr_vk_record *entry,
	const VkBufferCopy *span,
	struct transfer_work *work)
{
	const struct powervr_vk_buffer *source_buffer;
	const struct powervr_vk_buffer *destination_buffer;
	uint64_t source;
	uint64_t destination;
	uint64_t words;
	uint64_t rows;
	uint64_t done;
	uint32_t height;
	uint32_t left;
	int error;

	/* The two buffers' memory at the region's offsets. */
	source_buffer = entry->objects[0]->payload;
	destination_buffer = entry->objects[1]->payload;
	error = powervr_vk_buffer_address(source_buffer, &source);
	if (error == 0)
		error = powervr_vk_buffer_address(destination_buffer, &destination);
	if (error != 0)
		return error;
	source += span->srcOffset;
	destination += span->dstOffset;

	/* The whole rows, a rectangle of up to BUFFER_ROWS_MAX at a time. */
	words = span->size / 4U;
	rows = words / BUFFER_ROW_WORDS;
	for (done = 0; done < rows; done += height) {
		height = BUFFER_ROWS_MAX;
		if (rows - done < BUFFER_ROWS_MAX)
			height = (uint32_t)(rows - done);
		buffer_rows(work, source + done * BUFFER_ROW_WORDS * 4U, destination + done * BUFFER_ROW_WORDS * 4U, BUFFER_ROW_WORDS, height);
		error = run_one(session, &work->request, true);
		if (error != 0)
			return error;
	}

	/* The words left over, as one row. */
	left = (uint32_t)(words % BUFFER_ROW_WORDS);
	if (left != 0) {
		buffer_rows(work, source + rows * BUFFER_ROW_WORDS * 4U, destination + rows * BUFFER_ROW_WORDS * 4U, left, 1);
		error = run_one(session, &work->request, true);
		if (error != 0)
			return error;
	}

	/* Succeeded: the region is copied. */
	return 0;
}

/* Fills the request and the source view of a copy of a rectangle of 32-bit words between two linear places. */
static void
buffer_rows(
	struct transfer_work *work,
	uint64_t source,
	uint64_t destination,
	uint32_t width,
	uint32_t height)
{
	const struct powervr_format *format;

	/* The raw 32-bit texels on both sides. */
	format = powervr_transfer_raw_format(4);

	/* The source: a linear view of the rows. */
	kern_memset(&work->layout, 0, sizeof(work->layout));
	work->layout.row_pitch = width * 4U;
	work->layout.padded_width = width;
	work->layout.padded_height = height;
	work->layout.padded_depth = 1;
	work->layout.layer_bytes = (uint64_t)work->layout.row_pitch * height;
	work->layout.bytes = work->layout.layer_bytes;
	kern_memset(&work->view, 0, sizeof(work->view));
	work->view.format = format;
	work->view.layout = &work->layout;
	work->view.address = source;
	work->view.width = width;
	work->view.height = height;
	work->view.image_levels = 1;
	work->view.level_count = 1;
	work->view.layer_count = 1;
	work->view.linear = 1;

	/* The destination: the same rows, linear, copied from the source's corner. */
	kern_memset(&work->request, 0, sizeof(work->request));
	work->request.kind = POWERVR_TRANSFER_COPY;
	work->request.destination.format = format;
	work->request.destination.address = destination;
	work->request.destination.width = width;
	work->request.destination.height = height;
	work->request.destination.linear = 1;
	work->request.destination.row_pitch = width * 4U;
	work->request.destination.padded_width = width;
	work->request.area.width = width;
	work->request.area.height = height;
	work->request.source = &work->view;
}
