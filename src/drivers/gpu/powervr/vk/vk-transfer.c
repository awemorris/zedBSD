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
 * Each region names a level and layers of its images, or slices of a
 * volume (a three-dimensional image), and runs one transfer a layer or
 * slice: a layer of a flat image (and a cube's face) is a surface at its
 * layer's offset, a slice of a volume is stored by the pixel back end's
 * slice and read as the third coordinate of a three-dimensional sample.
 * A blit takes flat images only.  Subresources outside an image are
 * refused (EINVAL) when recorded.  A buffer copy moves whole 32-bit
 * words at 4-byte aligned offsets; others are refused (ENOTSUP).  A
 * multi-planar (NV12) image is only copied out to a buffer, a plane a
 * region (PLANE_0 as R8 texels, PLANE_1 as R8G8), from level 0; any
 * other transfer of one is refused (ENOTSUP).
 *
 * Procedure referenced: Mesa 26.2.6 src/imagination/vulkan/rogue/
 * pvr_blit.c (pvr_setup_transfer_surface and its callers: a transfer a
 * layer of each region, a 3D-twiddled image's slices by z position,
 * other images' layers by their address).
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

/*
 * The most regions one transfer command records (the specification sets
 * none; libGLESv2 sends a region for each face and level of a texture, up
 * to 6 x 15).  A record's regions are allocated to their count.
 */
#define TRANSFER_REGIONS_MAX		1024U

/*
 * The words a buffer copy moves in each row of its rectangles (a row of
 * 4 KiB), and the most rows of one rectangle.
 *
 * Values referenced: Mesa 26.2.5 src/imagination/vulkan/rogue/pvr_blit.c
 * (PVR_MAX_TRANSFER_SIZE_IN_TEXELS 2048, the most of either edge).
 */
#define BUFFER_ROW_WORDS		1024U
#define BUFFER_ROWS_MAX			2048U

/* One region of a transfer command, of the command's kind. */
union transfer_region {
	VkImageCopy copy;
	VkImageBlit blit;
	VkBufferImageCopy buffer;
	VkImageSubresourceRange range;
	VkBufferCopy span;
};

/*
 * What a transfer record keeps: its regions (count of them, in the same
 * allocation behind the structure), a blit's filter and a clear's colour.
 */
struct transfer_data {
	uint32_t count;
	uint32_t filter;
	uint32_t colour[4];
	union {
		VkImageCopy *copies;
		VkImageBlit *blits;
		VkBufferImageCopy *buffers;
		VkImageSubresourceRange *ranges;
		VkBufferCopy *spans;
	} regions;
};

/* The working state of one run: a clear's colour, the request, the views it names and their layouts (a buffer's, a further level's). */
struct transfer_work {
	uint32_t colour[4];
	struct powervr_transfer_request request;
	struct powervr_view_shape view;
	struct powervr_image_layout layout;
	struct powervr_image_layout level_layout;
	struct powervr_view_shape merge;
	struct powervr_image_layout merge_layout;
};

static int hold(struct powervr_vk_session *session, struct powervr_vk_record *entry, enum i915_vk_object_kind kind, uint64_t identity);
static int regions_make(struct powervr_vk_record *entry, struct transfer_data **data, uint64_t count);
static int region_slices(const struct powervr_vk_image *image, uint32_t level, uint32_t base_layer, uint32_t layer_count, int32_t z, uint32_t depth, uint32_t *first, uint32_t *count);
static int whole_texels(const struct powervr_vk_image *image, uint32_t aspects, bool to_buffer);
static int aspect_merge(const struct powervr_vk_image *image, uint32_t aspects, const struct powervr_format **texels, uint32_t *shift, uint32_t *mask);
static int image_merge(const struct powervr_vk_image *source, const struct powervr_vk_image *destination, const VkImageCopy *copy, uint32_t *mask);
static int aspect_extract(const struct powervr_vk_image *image, uint32_t aspects, const struct powervr_format **texels, uint32_t *shift);
static int blit_formats(const struct powervr_vk_image *source, const struct powervr_vk_image *destination, const VkImageBlit *blit, uint32_t filter);
static int range_counts(const struct powervr_vk_image *image, const VkImageSubresourceRange *range, uint32_t *levels, uint32_t *layers);
static int record_regions(uint32_t opcode, const struct powervr_vk_record *entry, const struct transfer_data *data);
static int clear_run(struct powervr_vk_session *session, const struct powervr_vk_record *entry, const VkImageSubresourceRange *range, struct transfer_work *work);
static int copy_run(struct powervr_vk_session *session, const struct powervr_vk_record *entry, const VkImageCopy *copy, struct transfer_work *work);
static int blit_run(struct powervr_vk_session *session, const struct powervr_vk_record *entry, const VkImageBlit *blit, uint32_t filter, struct transfer_work *work);
static int resolve_regions(const struct powervr_vk_record *entry, const struct transfer_data *data);
static int resolve_run(struct powervr_vk_session *session, const struct powervr_vk_record *entry, const VkImageCopy *region, struct transfer_work *work);
static int buffer_image_run(struct powervr_vk_session *session, const struct powervr_vk_record *entry, const VkBufferImageCopy *copy, struct transfer_work *work);
static int planar_regions(uint32_t opcode, const struct powervr_vk_record *entry, const struct transfer_data *data);
static int plane_source(const struct powervr_vk_image *image, uint32_t aspect, struct powervr_view_shape *view);
static int plane_words(struct powervr_view_shape *view, struct powervr_image_layout *layout, uint32_t *per_word);
static int image_surface(const struct powervr_vk_image *image, uint32_t level, uint32_t slice, struct powervr_render_surface *surface);
static int level_edges(const struct powervr_vk_image *image, uint32_t level, uint32_t *width, uint32_t *height, uint32_t *padded_width);
static uint32_t level_depth(const struct powervr_vk_image *image, uint32_t level);
static int image_view(const struct powervr_vk_image *image, uint32_t level, uint32_t slice, struct powervr_view_shape *view, struct powervr_image_layout *level_layout, uint32_t *z);
static void blit_request(const VkImageBlit *blit, uint32_t filter, struct powervr_transfer_request *request);
static int sample_surface(const struct powervr_vk_image *image, uint32_t layer, struct powervr_render_surface *surface);
static int sample_view(const struct powervr_vk_image *image, uint32_t layer, struct powervr_view_shape *view, struct powervr_image_layout *layout);
static int sample_copy_check(const struct powervr_vk_image *source, const struct powervr_vk_image *destination, const VkImageCopy *copy);
static bool sample_copy_whole(const struct powervr_vk_image *source, const struct powervr_vk_image *destination, const VkImageCopy *copy);
static int run_one(struct powervr_vk_session *session, struct powervr_transfer_request *request, bool writes_buffer);
static int buffer_copy_run(struct powervr_vk_session *session, const struct powervr_vk_record *entry, const VkBufferCopy *span, struct transfer_work *work);
static void buffer_rows(struct transfer_work *work, uint64_t source, uint64_t destination, uint32_t width, uint32_t height);
static int record_depth_clear(struct powervr_vk_session *session, struct i915_wire_reader *reader, struct powervr_vk_record *entry, struct transfer_data **made);

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
	VkImageResolve resolve;
	enum i915_vk_object_kind source_kind;
	enum i915_vk_object_kind destination_kind;
	uint64_t source;
	uint64_t destination;
	uint64_t count;
	uint64_t index;
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
		if (reader->error != 0)
			return EINVAL;
		error = regions_make(entry, &data, count);
		if (error != 0)
			return error;
		for (index = 0; index < count; index++)
			i915_vkc_dec_VkImageSubresourceRange(reader, &session->arena, &data->regions.ranges[index]);
		if (reader->error != 0)
			return EINVAL;
		error = hold(session, entry, I915_VK_OBJ_IMAGE, destination);
		if (error != 0)
			return error;

		/* A multi-planar image is not cleared. */
		if (((const struct powervr_vk_image *)entry->objects[0]->payload)->plane_count != 0)
			return ENOTSUP;

		/* Each range: levels and layers of the image. */
		error = record_regions(opcode, entry, data);
		return error;
	}

	/* A depth image's clear: run as an image's clear of the depth's value (record_depth_clear). */
	if (opcode == GPU_OP_CMD_CLEAR_DEPTH_STENCIL_IMAGE) {
		error = record_depth_clear(session, reader, entry, &data);
		return error;
	}

	/* A buffer copy: [source][destination][u32 count][u64 count]{VkBufferCopy}, whole aligned words. */
	if (opcode == GPU_OP_CMD_COPY_BUFFER) {
		source = drv_i915_wire_read_u64(reader);
		destination = drv_i915_wire_read_u64(reader);
		(void)drv_i915_wire_read_u32(reader);
		count = drv_i915_wire_read_u64(reader);
		if (reader->error != 0)
			return EINVAL;
		error = regions_make(entry, &data, count);
		if (error != 0)
			return error;
		for (index = 0; index < count; index++)
			i915_vkc_dec_VkBufferCopy(reader, &session->arena, &data->regions.spans[index]);
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
	if (reader->error != 0)
		return EINVAL;
	error = regions_make(entry, &data, count);
	if (error != 0)
		return error;
	for (index = 0; index < count; index++) {
		if (opcode == GPU_OP_CMD_COPY_IMAGE) {
			i915_vkc_dec_VkImageCopy(reader, &session->arena, &data->regions.copies[index]);
		} else if (opcode == GPU_OP_CMD_RESOLVE_IMAGE) {
			/* A resolve's region has a copy's fields in a copy's order; kept as one. */
			i915_vkc_dec_VkImageResolve(reader, &session->arena, &resolve);
			data->regions.copies[index].srcSubresource = resolve.srcSubresource;
			data->regions.copies[index].srcOffset = resolve.srcOffset;
			data->regions.copies[index].dstSubresource = resolve.dstSubresource;
			data->regions.copies[index].dstOffset = resolve.dstOffset;
			data->regions.copies[index].extent = resolve.extent;
		} else if (opcode == GPU_OP_CMD_BLIT_IMAGE) {
			i915_vkc_dec_VkImageBlit(reader, &session->arena, &data->regions.blits[index]);
		} else {
			i915_vkc_dec_VkBufferImageCopy(reader, &session->arena, &data->regions.buffers[index]);
		}
	}

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

	/* A multi-planar image only as a copy's source of its planes to a buffer. */
	refused = planar_regions(opcode, entry, data);
	if (refused != 0)
		return refused;

	/* The levels, layers and slices of each region inside its images. */
	refused = record_regions(opcode, entry, data);
	if (refused != 0)
		return refused;

	/* Succeeded: the transfer is recorded. */
	return 0;
}

/*
 * Runs one recorded transfer command, region by region and a layer or
 * slice of each at a time; ETIMEDOUT when one did not finish, the
 * session then abandoned.
 */
int
powervr_vk_transfer_run(
	struct powervr_vk_session *session,
	const struct powervr_vk_record *entry)
{
	struct transfer_work *work;
	const struct transfer_data *data;
	const VkBufferImageCopy *copy;
	uint32_t index;
	int error;

	/* The working state, and the record's data. */
	work = kern_calloc(1, sizeof(*work));
	if (work == NULL)
		return ENOMEM;
	data = entry->data;
	error = 0;
	for (index = 0; error == 0 && index < data->count; index++) {
		if (entry->opcode == GPU_OP_CMD_COPY_BUFFER) {
			/* A buffer copy runs its own transfers. */
			error = buffer_copy_run(session, entry, &data->regions.spans[index], work);
		} else if (entry->opcode == GPU_OP_CMD_CLEAR_COLOR_IMAGE) {
			/* Each level and layer of the range, whole, to the colour. */
			kern_memcpy(work->colour, data->colour, sizeof(work->colour));
			error = clear_run(session, entry, &data->regions.ranges[index], work);
		} else if (entry->opcode == GPU_OP_CMD_COPY_IMAGE) {
			/* From each layer or slice of the source to the destination's. */
			error = copy_run(session, entry, &data->regions.copies[index], work);
		} else if (entry->opcode == GPU_OP_CMD_BLIT_IMAGE) {
			/* Each layer of the source rectangle onto the destination's. */
			error = blit_run(session, entry, &data->regions.blits[index], data->filter, work);
		} else if (entry->opcode == GPU_OP_CMD_RESOLVE_IMAGE) {
			/* Each layer's four samples a pixel averaged onto the destination's. */
			error = resolve_run(session, entry, &data->regions.copies[index], work);
		} else {
			/* Between a buffer laid out as linear rows of the image's texels and each layer or slice of the image. */
			error = buffer_image_run(session, entry, &data->regions.buffers[index], work);
		}
	}

	/* The working state goes; a transfer that could not run, with its region's aspects and subresource when it has one. */
	kern_free(work);
	if (error != 0) {
		if (entry->opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE || entry->opcode == GPU_OP_CMD_COPY_IMAGE_TO_BUFFER) {
			copy = &data->regions.buffers[index - 1U];
			kern_logf("powervr: vk: region %u of a buffer and image copy did not run (%d): aspects 0x%x, level %u, layers %u+%u, offset %d,%d,%d, extent %ux%ux%u\n",
				  (unsigned)(index - 1U), error, (unsigned)copy->imageSubresource.aspectMask, (unsigned)copy->imageSubresource.mipLevel,
				  (unsigned)copy->imageSubresource.baseArrayLayer, (unsigned)copy->imageSubresource.layerCount, (int)copy->imageOffset.x,
				  (int)copy->imageOffset.y, (int)copy->imageOffset.z, (unsigned)copy->imageExtent.width, (unsigned)copy->imageExtent.height,
				  (unsigned)copy->imageExtent.depth);
		} else {
			kern_logf("powervr: vk: region %u of a transfer (opcode %u) did not run (%d)\n", (unsigned)(index - 1U), (unsigned)entry->opcode, error);
		}
		return error;
	}

	/* Succeeded: every region is done. */
	return 0;
}

/* Clears every level and layer (or slice of a volume) of a range to the colour, each whole. */
static int
clear_run(
	struct powervr_vk_session *session,
	const struct powervr_vk_record *entry,
	const VkImageSubresourceRange *range,
	struct transfer_work *work)
{
	const struct powervr_vk_image *image;
	uint32_t levels;
	uint32_t layers;
	uint32_t level;
	uint32_t slice;
	uint32_t slices;
	int error;

	/* The levels and layers the range names. */
	image = entry->objects[0]->payload;
	error = range_counts(image, range, &levels, &layers);
	if (error != 0)
		return error;

	/* Each level's layers, or a volume's slices of that level. */
	for (level = range->baseMipLevel; level < range->baseMipLevel + levels; level++) {
		slices = layers;
		if (image->shape.volume)
			slices = level_depth(image, level);
		for (slice = 0; slice < slices; slice++) {
			kern_memset(&work->request, 0, sizeof(work->request));
			work->request.kind = POWERVR_TRANSFER_CLEAR;
			if (image->layout.samples == 4U)
				error = sample_surface(image, range->baseArrayLayer + slice, &work->request.destination);
			else
				error = image_surface(image, level, image->shape.volume ? slice : range->baseArrayLayer + slice, &work->request.destination);
			if (error != 0)
				return error;
			work->request.area.width = work->request.destination.width;
			work->request.area.height = work->request.destination.height;
			kern_memcpy(work->request.colour, work->colour, sizeof(work->request.colour));
			error = run_one(session, &work->request, false);
			if (error != 0)
				return error;
		}
	}

	/* Succeeded: the range is cleared. */
	return 0;
}

/* Copies a region between two images, a layer or slice at a time. */
static int
copy_run(
	struct powervr_vk_session *session,
	const struct powervr_vk_record *entry,
	const VkImageCopy *copy,
	struct transfer_work *work)
{
	const struct powervr_vk_image *source;
	const struct powervr_vk_image *destination;
	uint32_t source_first;
	uint32_t destination_first;
	uint32_t count;
	uint32_t other;
	uint32_t index;
	uint32_t merge_mask;
	uint32_t merge_z;
	int error;

	/* The two images, and the layers or slices of each side. */
	source = entry->objects[0]->payload;
	destination = entry->objects[1]->payload;
	error = region_slices(source, copy->srcSubresource.mipLevel, copy->srcSubresource.baseArrayLayer, copy->srcSubresource.layerCount,
	    copy->srcOffset.z, copy->extent.depth, &source_first, &count);
	if (error == 0)
		error = region_slices(destination, copy->dstSubresource.mipLevel, copy->dstSubresource.baseArrayLayer, copy->dstSubresource.layerCount,
		    copy->dstOffset.z, copy->extent.depth, &destination_first, &other);
	if (error == 0 && other != count)
		error = EINVAL;
	if (error != 0)
		return error;

	/*
	 * A copy moves raw texels: the two formats need the same texel size,
	 * and a packed depth and stencil both aspects or one merged into the
	 * destination's other (image_merge).
	 */
	if (source->format->texel_bytes != destination->format->texel_bytes)
		return ENOTSUP;
	merge_mask = 0;
	error = whole_texels(source, copy->srcSubresource.aspectMask, false);
	if (error == 0)
		error = whole_texels(destination, copy->dstSubresource.aspectMask, false);
	if (error == ENOTSUP)
		error = image_merge(source, destination, copy, &merge_mask);
	if (error != 0)
		return error;

	/* Each layer or slice: the source's view to the destination's rectangle. */
	for (index = 0; index < count; index++) {
		kern_memset(&work->request, 0, sizeof(work->request));
		work->request.kind = POWERVR_TRANSFER_COPY;
		if (source->layout.samples == 4U && sample_copy_whole(source, destination, copy)) {
			/* A whole layer of four samples onto one of its shape, every texel. */
			error = sample_surface(destination, destination_first + index, &work->request.destination);
			if (error == 0)
				error = sample_view(source, source_first + index, &work->view, &work->level_layout);
			if (error != 0)
				return error;
			work->request.area.width = work->request.destination.width;
			work->request.area.height = work->request.destination.height;
			work->request.source = &work->view;
			error = run_one(session, &work->request, false);
			if (error != 0)
				return error;
			continue;
		}
		error = image_surface(destination, copy->dstSubresource.mipLevel, destination_first + index, &work->request.destination);
		if (error == 0)
			error = image_view(source, copy->srcSubresource.mipLevel, source_first + index, &work->view, &work->level_layout, &work->request.source_z);
		if (error != 0)
			return error;
		work->request.area.x = (uint32_t)copy->dstOffset.x;
		work->request.area.y = (uint32_t)copy->dstOffset.y;
		work->request.area.width = copy->extent.width;
		work->request.area.height = copy->extent.height;
		work->request.source_x = (uint32_t)copy->srcOffset.x;
		work->request.source_y = (uint32_t)copy->srcOffset.y;
		work->request.source = &work->view;

		/* One aspect: the destination's own texels of the layer keep the other. */
		if (merge_mask != 0) {
			error = image_view(destination, copy->dstSubresource.mipLevel, destination_first + index, &work->merge, &work->merge_layout, &merge_z);
			if (error != 0)
				return error;
			work->request.merge = &work->merge;
			work->request.merge_mask = merge_mask;
		}

		/* Four samples to four: each sample of the rectangle from the source's same sample. */
		if (source->layout.samples == 4U)
			work->request.samples = 4;
		error = run_one(session, &work->request, false);
		if (error != 0)
			return error;
	}

	/* Succeeded: the region is copied. */
	return 0;
}

/* Blits a region between two flat images, a layer at a time. */
static int
blit_run(
	struct powervr_vk_session *session,
	const struct powervr_vk_record *entry,
	const VkImageBlit *blit,
	uint32_t filter,
	struct transfer_work *work)
{
	const struct powervr_vk_image *source;
	const struct powervr_vk_image *destination;
	uint32_t source_first;
	uint32_t destination_first;
	uint32_t count;
	uint32_t other;
	uint32_t index;
	uint32_t z;
	int error;

	/* The two images and their layers. */
	source = entry->objects[0]->payload;
	destination = entry->objects[1]->payload;
	if (source->shape.volume || destination->shape.volume)
		return ENOTSUP;
	error = region_slices(source, blit->srcSubresource.mipLevel, blit->srcSubresource.baseArrayLayer, blit->srcSubresource.layerCount, 0, 1, &source_first, &count);
	if (error == 0)
		error = region_slices(destination, blit->dstSubresource.mipLevel, blit->dstSubresource.baseArrayLayer, blit->dstSubresource.layerCount, 0, 1, &destination_first, &other);
	if (error == 0 && other != count)
		error = EINVAL;
	if (error != 0)
		return error;

	/* Each layer: the source rectangle onto the destination's. */
	for (index = 0; index < count; index++) {
		kern_memset(&work->request, 0, sizeof(work->request));
		error = image_surface(destination, blit->dstSubresource.mipLevel, destination_first + index, &work->request.destination);
		if (error == 0)
			error = image_view(source, blit->srcSubresource.mipLevel, source_first + index, &work->view, &work->level_layout, &z);
		if (error != 0)
			return error;
		blit_request(blit, filter, &work->request);
		work->request.source = &work->view;
		error = run_one(session, &work->request, false);
		if (error != 0)
			return error;
	}

	/* Succeeded: the region is blitted. */
	return 0;
}

/*
 * Checks a resolve's images and regions: a source of four samples and a
 * destination of one, flat images of the same format, each region's
 * layers as many on each side at level 0 of the source, its rectangles
 * not negative and inside both images.  The format must be one the
 * resolve's program takes (powervr_transfer_resolve_samples: an average
 * of a float or normalised colour format, sample 0 of an integer one);
 * ENOTSUP otherwise.
 *
 * Procedure referenced: the Vulkan 1.3 specification, vkCmdResolveImage
 * valid usage (srcImage multisampled, dstImage of one sample, the same
 * format, regions inside the subresources, the layer counts equal).
 */
static int
resolve_regions(
	const struct powervr_vk_record *entry,
	const struct transfer_data *data)
{
	const struct powervr_vk_image *source;
	const struct powervr_vk_image *destination;
	const VkImageCopy *region;
	uint32_t source_first;
	uint32_t destination_first;
	uint32_t count;
	uint32_t other;
	uint32_t width;
	uint32_t height;
	uint32_t index;
	int error;

	/* Four samples to one, flat, of one format the program resolves. */
	source = entry->objects[0]->payload;
	destination = entry->objects[1]->payload;
	if (source->layout.samples != 4U || destination->layout.samples == 4U)
		return EINVAL;
	if (source->format != destination->format)
		return EINVAL;
	if (source->shape.volume || destination->shape.volume || source->plane_count != 0 || destination->plane_count != 0)
		return ENOTSUP;
	if (powervr_transfer_resolve_samples(source->format) == 0)
		return ENOTSUP;

	/* Each region: its layers on both sides, its rectangles inside both images. */
	for (index = 0; index < data->count; index++) {
		region = &data->regions.copies[index];
		error = region_slices(source, region->srcSubresource.mipLevel, region->srcSubresource.baseArrayLayer, region->srcSubresource.layerCount,
		    0, 1, &source_first, &count);
		if (error == 0)
			error = region_slices(destination, region->dstSubresource.mipLevel, region->dstSubresource.baseArrayLayer, region->dstSubresource.layerCount,
			    0, 1, &destination_first, &other);
		if (error == 0 && other != count)
			error = EINVAL;
		if (error != 0)
			return error;
		if (region->srcOffset.x < 0 || region->srcOffset.y < 0 || region->dstOffset.x < 0 || region->dstOffset.y < 0 ||
		    region->extent.width == 0 || region->extent.height == 0 || region->extent.depth != 1)
			return EINVAL;
		error = level_edges(source, region->srcSubresource.mipLevel, &width, &height, &other);
		if (error == 0 && ((uint32_t)region->srcOffset.x > width || region->extent.width > width - (uint32_t)region->srcOffset.x ||
		    (uint32_t)region->srcOffset.y > height || region->extent.height > height - (uint32_t)region->srcOffset.y))
			error = EINVAL;
		if (error == 0)
			error = level_edges(destination, region->dstSubresource.mipLevel, &width, &height, &other);
		if (error == 0 && ((uint32_t)region->dstOffset.x > width || region->extent.width > width - (uint32_t)region->dstOffset.x ||
		    (uint32_t)region->dstOffset.y > height || region->extent.height > height - (uint32_t)region->dstOffset.y))
			error = EINVAL;
		if (error != 0)
			return error;
	}

	/* Succeeded: every region resolves. */
	return 0;
}

/*
 * Resolves a region, a layer at a time: the transfer's blit of the
 * source rectangle onto the destination's of the same size, point
 * sampled, whose program reads the four samples of each pixel and
 * averages them, or of an integer format moves sample 0's raw texel
 * (transfer-job.c takes a blit from a source of four samples as a
 * resolve).
 *
 * Procedure referenced: Mesa 26.2.6
 * src/imagination/vulkan/pvr_arch_job_transfer.c (pvr_msaa_state: a
 * multisampled source and a single-sampled destination, PVR_RESOLVE_BLEND,
 * or for an integer format PVR_RESOLVE_SAMPLE0 by rogue/pvr_blit.c) and
 * src/imagination/vulkan/pvr_usc.c (resolve_samples: the samples added
 * and the sum scaled by one over their count).
 */
static int
resolve_run(
	struct powervr_vk_session *session,
	const struct powervr_vk_record *entry,
	const VkImageCopy *region,
	struct transfer_work *work)
{
	const struct powervr_vk_image *source;
	const struct powervr_vk_image *destination;
	uint32_t source_first;
	uint32_t destination_first;
	uint32_t count;
	uint32_t other;
	uint32_t index;
	uint32_t z;
	int error;

	/* The two images and their layers. */
	source = entry->objects[0]->payload;
	destination = entry->objects[1]->payload;
	error = region_slices(source, region->srcSubresource.mipLevel, region->srcSubresource.baseArrayLayer, region->srcSubresource.layerCount, 0, 1, &source_first, &count);
	if (error == 0)
		error = region_slices(destination, region->dstSubresource.mipLevel, region->dstSubresource.baseArrayLayer, region->dstSubresource.layerCount, 0, 1, &destination_first, &other);
	if (error == 0 && other != count)
		error = EINVAL;
	if (error != 0)
		return error;

	/* Each layer: the source rectangle onto the destination's, one to one. */
	for (index = 0; index < count; index++) {
		kern_memset(&work->request, 0, sizeof(work->request));
		error = image_surface(destination, region->dstSubresource.mipLevel, destination_first + index, &work->request.destination);
		if (error == 0)
			error = image_view(source, region->srcSubresource.mipLevel, source_first + index, &work->view, &work->level_layout, &z);
		if (error != 0)
			return error;
		work->request.kind = POWERVR_TRANSFER_BLIT;
		work->request.area.x = (uint32_t)region->dstOffset.x;
		work->request.area.y = (uint32_t)region->dstOffset.y;
		work->request.area.width = region->extent.width;
		work->request.area.height = region->extent.height;
		work->request.source_area.x = (uint32_t)region->srcOffset.x;
		work->request.source_area.y = (uint32_t)region->srcOffset.y;
		work->request.source_area.width = region->extent.width;
		work->request.source_area.height = region->extent.height;
		work->request.source = &work->view;
		error = run_one(session, &work->request, false);
		if (error != 0)
			return error;
	}

	/* Succeeded: the region is resolved. */
	return 0;
}

/*
 * Copies a region between a buffer and an image, a layer or slice at a
 * time: the buffer holds the region's rows (of bufferRowLength texels,
 * else the extent's width) of each layer or slice (of bufferImageHeight
 * rows, else the extent's height) one after another.  A plane of a
 * multi-planar image is copied out as words of its texels.
 */
static int
buffer_image_run(
	struct powervr_vk_session *session,
	const struct powervr_vk_record *entry,
	const VkBufferImageCopy *copy,
	struct transfer_work *work)
{
	const struct powervr_vk_image *image;
	const struct powervr_vk_buffer *buffer;
	const struct powervr_format *texels;
	const struct powervr_format *buffer_texels;
	uint64_t address;
	uint64_t slice_bytes;
	uint32_t merge_shift;
	uint32_t merge_mask;
	uint32_t merge_z;
	uint32_t extract_shift;
	uint32_t per_word;
	uint32_t first;
	uint32_t count;
	uint32_t index;
	uint32_t row;
	uint32_t height;
	bool to_image;
	int error;

	/* The buffer, the image and the layers or slices (a plane's one). */
	to_image = entry->opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE;
	if (to_image) {
		buffer = entry->objects[0]->payload;
		image = entry->objects[1]->payload;
	} else {
		image = entry->objects[0]->payload;
		buffer = entry->objects[1]->payload;
	}
	first = 0;
	count = 1;
	buffer_texels = image->format;
	merge_mask = 0;
	merge_shift = 0;
	extract_shift = 0;
	error = whole_texels(image, copy->imageSubresource.aspectMask, !to_image);
	if (error == ENOTSUP && to_image)
		error = aspect_merge(image, copy->imageSubresource.aspectMask, &buffer_texels, &merge_shift, &merge_mask);
	else if (error == ENOTSUP)
		error = aspect_extract(image, copy->imageSubresource.aspectMask, &buffer_texels, &extract_shift);
	if (error == 0 && image->plane_count == 0)
		error = region_slices(image, copy->imageSubresource.mipLevel, copy->imageSubresource.baseArrayLayer, copy->imageSubresource.layerCount,
		    copy->imageOffset.z, copy->imageExtent.depth, &first, &count);
	if (error == 0)
		error = powervr_vk_buffer_address(buffer, &address);
	if (error != 0)
		return error;

	/* The buffer's rows and the bytes of one layer or slice of them. */
	row = copy->bufferRowLength != 0 ? copy->bufferRowLength : copy->imageExtent.width;
	height = copy->bufferImageHeight != 0 ? copy->bufferImageHeight : copy->imageExtent.height;
	address += copy->bufferOffset;
	slice_bytes = (uint64_t)row * height * buffer_texels->texel_bytes;

	/* Each layer or slice. */
	for (index = 0; index < count; index++) {
		kern_memset(&work->request, 0, sizeof(work->request));
		work->request.kind = POWERVR_TRANSFER_COPY;
		per_word = 1;
		if (to_image) {
			/* The buffer as a linear view, read from its first texel. */
			error = image_surface(image, copy->imageSubresource.mipLevel, first + index, &work->request.destination);
			if (error == 0 && merge_mask != 0) {
				/* One aspect: the image's own texels of the layer keep the other. */
				error = image_view(image, copy->imageSubresource.mipLevel, first + index, &work->merge, &work->merge_layout, &merge_z);
				work->request.merge = &work->merge;
				work->request.merge_shift = merge_shift;
				work->request.merge_mask = merge_mask;
			}
			kern_memset(&work->layout, 0, sizeof(work->layout));
			work->layout.row_pitch = row * buffer_texels->texel_bytes;
			work->layout.padded_width = row;
			work->layout.padded_height = height;
			work->layout.padded_depth = 1;
			work->layout.layer_bytes = (uint64_t)work->layout.row_pitch * height;
			work->layout.bytes = work->layout.layer_bytes;
			kern_memset(&work->view, 0, sizeof(work->view));
			work->view.format = buffer_texels;
			work->view.layout = &work->layout;
			work->view.address = address + index * slice_bytes;
			work->view.width = row;
			work->view.height = height;
			work->view.image_levels = 1;
			work->view.level_count = 1;
			work->view.layer_count = 1;
			work->view.linear = 1;
			work->request.area.x = (uint32_t)copy->imageOffset.x;
			work->request.area.y = (uint32_t)copy->imageOffset.y;
		} else {
			/* The buffer as a linear destination, written from its first texel, of the image's (the plane's, one aspect's) texels. */
			texels = buffer_texels;
			work->request.extract_shift = extract_shift;
			if (image->plane_count != 0) {
				error = plane_source(image, copy->imageSubresource.aspectMask, &work->view);
				if (error == 0)
					error = plane_words(&work->view, &work->level_layout, &per_word);
				if (error == 0)
					texels = work->view.format;
			} else {
				error = image_view(image, copy->imageSubresource.mipLevel, first + index, &work->view, &work->level_layout, &work->request.source_z);
			}
			work->request.destination.format = texels;
			work->request.destination.address = address + index * slice_bytes;
			work->request.destination.width = row / per_word;
			work->request.destination.height = height;
			work->request.destination.linear = 1;
			work->request.destination.row_pitch = row / per_word * texels->texel_bytes;
			work->request.destination.padded_width = row / per_word;
			work->request.source_x = (uint32_t)copy->imageOffset.x / per_word;
			work->request.source_y = (uint32_t)copy->imageOffset.y;
		}
		if (error != 0)
			return error;
		work->request.source = &work->view;
		work->request.area.width = copy->imageExtent.width / per_word;
		work->request.area.height = copy->imageExtent.height;

		/* The transfer (into a buffer for an image's copy to one). */
		error = run_one(session, &work->request, !to_image);
		if (error != 0)
			return error;
	}

	/* Succeeded: the region is copied. */
	return 0;
}

/*
 * Says whether a copy of an image's aspects moves its texels whole, as
 * the raw copy does: any copy of a format without a stencil, both
 * aspects of a packed depth and stencil, or its depth alone into a
 * buffer (whose 32-bit depth word leaves the top byte undefined).  A
 * stencil alone (a byte a texel in a buffer) or a depth alone into an
 * image (which keeps the stencil) is not taken (ENOTSUP); a buffer's
 * aspect into an image merges (aspect_merge), an image's into another
 * image too (image_merge), and a stencil into a buffer is extracted
 * (aspect_extract).
 *
 * Procedure referenced: the Vulkan 1.4 specification, "Copying Data
 * Between Buffers and Images" (D24_UNORM_S8_UINT's depth aspect is
 * copied as 32-bit words with the top 8 bits undefined, its stencil
 * aspect as 8-bit ones).
 */
static int
whole_texels(
	const struct powervr_vk_image *image,
	uint32_t aspects,
	bool to_buffer)
{
	/* Any aspect of a format without a stencil. */
	if (image->format == NULL || (image->format->kind & POWERVR_FORMAT_STENCIL) == 0)
		return 0;

	/* Both aspects, or the depth into a buffer. */
	if (aspects == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
		return 0;
	if (to_buffer && aspects == VK_IMAGE_ASPECT_DEPTH_BIT)
		return 0;

	/* One aspect a raw copy would mix with the other. */
	return ENOTSUP;
}

/*
 * Says how a buffer's single aspect is copied into a packed depth and
 * stencil image (D24_UNORM_S8_UINT, the depth in bits 0 to 23 and the
 * stencil in 24 to 31), keeping the other aspect: the buffer's texels
 * (*texels: the depth's 32-bit words as the image's format, the
 * stencil's bytes as an 8-bit raw format), the shift that puts them in
 * place (24 for the stencil) and the bits they write.  Refuses other
 * formats and aspects (ENOTSUP).
 *
 * Procedure referenced: the Vulkan 1.4 specification, "Copying Data
 * Between Buffers and Images" (a D24_UNORM_S8_UINT depth aspect as
 * 32-bit words, the depth in the low 24 bits; the stencil aspect as
 * 8-bit ones; a copy of one aspect leaves the other unchanged) and Mesa
 * 26.2.6 src/imagination/vulkan/pvr_usc.c (merge_depth_stencil: the
 * depth's mask 0x00ffffff, the stencil's 0xff000000).
 */
static int
aspect_merge(
	const struct powervr_vk_image *image,
	uint32_t aspects,
	const struct powervr_format **texels,
	uint32_t *shift,
	uint32_t *mask)
{
	/* The packed depth and stencil only, of one image a layer at a time. */
	if (image->format == NULL || image->format->vk_format != VK_FORMAT_D24_UNORM_S8_UINT || image->layout.samples > 1U || image->shape.linear)
		return ENOTSUP;

	/* The depth: its words, the low 24 bits. */
	if (aspects == VK_IMAGE_ASPECT_DEPTH_BIT) {
		*texels = image->format;
		*shift = 0;
		*mask = 0x00FFFFFFU;
		return 0;
	}

	/* The stencil: its bytes, the top 8 bits. */
	if (aspects == VK_IMAGE_ASPECT_STENCIL_BIT) {
		*texels = powervr_transfer_raw_format(1);
		*shift = 24;
		*mask = 0xFF000000U;
		return 0;
	}

	/* Not one aspect. */
	return ENOTSUP;
}

/*
 * Says how one aspect of a packed depth and stencil image is copied into
 * the same aspect of another such image, keeping the destination's
 * other aspect: the bits the source's texel writes (*mask, as
 * aspect_merge has them for the destination), the source's whole texel
 * read as it lies (no shift).  Both images D24_UNORM_S8_UINT of one
 * sample, the one aspect named on both sides; ENOTSUP otherwise.
 *
 * Procedure referenced: Mesa 26.2.6 src/imagination/vulkan/
 * pvr_arch_job_transfer.c (pvr_pbe_src_format_pick_depth and _stencil:
 * D24S8 onto D24S8 merges as DMRG_D24S8_D24S8 or SMRG_D24S8_D24S8) and
 * pvr_usc.c (merge_depth_stencil); the Vulkan 1.4 specification,
 * vkCmdCopyImage (a depth/stencil copy names the same aspects on both
 * sides, and only those aspects are copied).
 */
static int
image_merge(
	const struct powervr_vk_image *source,
	const struct powervr_vk_image *destination,
	const VkImageCopy *copy,
	uint32_t *mask)
{
	const struct powervr_format *texels;
	uint32_t shift;
	int error;

	/* One aspect, the same on both sides, of two packed depth and stencil images of one sample. */
	if (source->format != destination->format || source->layout.samples > 1U)
		return ENOTSUP;
	if (copy->srcSubresource.aspectMask != copy->dstSubresource.aspectMask)
		return ENOTSUP;

	/* The destination's bits of the aspect; the source's word read as it lies. */
	error = aspect_merge(destination, copy->dstSubresource.aspectMask, &texels, &shift, mask);
	if (error != 0)
		return error;

	/* Succeeded: the copy merges. */
	return 0;
}

/*
 * Says how a packed depth and stencil image's stencil aspect is copied
 * into a buffer: the buffer's texels a byte each (*texels, the 8-bit raw
 * format) from each texel shifted right by 24 (*shift).  Refuses other
 * formats and aspects (ENOTSUP); the depth goes whole (whole_texels).
 *
 * Procedure referenced: the Vulkan 1.4 specification, "Copying Data
 * Between Buffers and Images" (a D24_UNORM_S8_UINT stencil aspect as
 * 8-bit texels); Mesa 26.2.6 src/imagination/vulkan/
 * pvr_arch_job_transfer.c (pvr_pbe_src_format_ds: a 24-bit depth source
 * onto a format without depth is SWAP_LMSB) and pvr_usc.c
 * (pvr_uscgen_tq_frag_conv: SWAP_LMSB shifts the word right by 24).
 */
static int
aspect_extract(
	const struct powervr_vk_image *image,
	uint32_t aspects,
	const struct powervr_format **texels,
	uint32_t *shift)
{
	/* The stencil of the packed depth and stencil. */
	if (image->format == NULL || image->format->vk_format != VK_FORMAT_D24_UNORM_S8_UINT || aspects != VK_IMAGE_ASPECT_STENCIL_BIT)
		return ENOTSUP;

	/* Its bytes, the top 8 bits. */
	*texels = powervr_transfer_raw_format(1);
	*shift = 24;
	return 0;
}

/*
 * Checks a blit's formats: a destination the blit's program packs (four
 * 8-bit unorm channels, or sRGB ones), or the same format on both sides
 * moved as raw texels (powervr_transfer_blits_raw), point sampled -- a
 * linear filter only when the rectangles are of one size, where it is
 * the point's -- and every aspect of a format with a stencil.  ENOTSUP
 * otherwise, before any of the command buffer runs.
 */
static int
blit_formats(
	const struct powervr_vk_image *source,
	const struct powervr_vk_image *destination,
	const VkImageBlit *blit,
	uint32_t filter)
{
	const struct powervr_format *format;
	int32_t source_width;
	int32_t source_height;
	int32_t width;
	int32_t height;
	int error;

	/* A destination the blit's program packs. */
	format = destination->format;
	if (format == NULL)
		return ENOTSUP;
	if (powervr_format_is_unorm8x4(format) || ((format->kind & POWERVR_FORMAT_SRGB) != 0 && format->texel_bytes == 4))
		return 0;

	/* Else raw texels of one format, point sampled. */
	if (!powervr_transfer_blits_raw(source->format, format))
		return ENOTSUP;
	source_width = blit->srcOffsets[1].x - blit->srcOffsets[0].x;
	source_height = blit->srcOffsets[1].y - blit->srcOffsets[0].y;
	width = blit->dstOffsets[1].x - blit->dstOffsets[0].x;
	height = blit->dstOffsets[1].y - blit->dstOffsets[0].y;
	if (source_width < 0)
		source_width = -source_width;
	if (source_height < 0)
		source_height = -source_height;
	if (width < 0)
		width = -width;
	if (height < 0)
		height = -height;
	if (filter == 1U && (source_width != width || source_height != height))
		return ENOTSUP;
	error = whole_texels(source, blit->srcSubresource.aspectMask, false);
	if (error == 0)
		error = whole_texels(destination, blit->dstSubresource.aspectMask, false);
	if (error != 0)
		return error;

	/* Succeeded: the transfer blits the formats. */
	return 0;
}

/*
 * The memory of a layer of a four-sample image as a twiddled surface of
 * one sample and twice the layout's padded edges.  The layer holds four
 * times the texels of its padded edges (powervr_image_lay_out_samples),
 * which a twiddled surface of the doubled edges -- powers of two --
 * covers exactly, so a transfer that writes every texel of it alike (a
 * clear) or moves every texel between two such layers of one shape (a
 * whole copy) does not depend on where in the layer each sample lies.
 *
 * Procedure referenced: Mesa 26.2.6 src/imagination/vulkan/pvr_image.c
 * (pvr_image_setup_mip_levels: a multisampled layer is the samples times
 * the level's pitch and height); the Vulkan 1.3 specification,
 * vkCmdClearColorImage (each subresource of the ranges is cleared whole,
 * every sample) and vkCmdCopyImage (samples copied to the same samples).
 */
static int
sample_surface(
	const struct powervr_vk_image *image,
	uint32_t layer,
	struct powervr_render_surface *surface)
{
	int error;

	/* Level 0 of the layer (an image of samples has one level), its edges doubled. */
	error = image_surface(image, 0, layer, surface);
	if (error != 0)
		return error;
	if (image->shape.linear || image->layout.padded_width == 0 || image->layout.padded_height == 0)
		return ENOTSUP;
	surface->width = image->layout.padded_width * 2U;
	surface->height = image->layout.padded_height * 2U;
	surface->padded_width = surface->width;
	surface->row_pitch = surface->width * image->format->texel_bytes;

	/* Succeeded: the layer's surface. */
	return 0;
}

/*
 * The view a copy reads a layer of a four-sample image through: as
 * sample_surface, one sample and twice the padded edges, with a layout
 * of its own of those edges.
 */
static int
sample_view(
	const struct powervr_vk_image *image,
	uint32_t layer,
	struct powervr_view_shape *view,
	struct powervr_image_layout *layout)
{
	uint32_t z;
	int error;

	/* The image's view of the layer, its edges and layout doubled to one sample. */
	error = image_view(image, 0, layer, view, layout, &z);
	if (error != 0)
		return error;
	if (image->shape.linear || image->layout.padded_width == 0 || image->layout.padded_height == 0)
		return ENOTSUP;
	kern_memset(layout, 0, sizeof(*layout));
	layout->padded_width = image->layout.padded_width * 2U;
	layout->padded_height = image->layout.padded_height * 2U;
	layout->padded_depth = 1;
	layout->row_pitch = layout->padded_width * image->format->texel_bytes;
	layout->layer_bytes = (uint64_t)layout->row_pitch * layout->padded_height;
	layout->bytes = layout->layer_bytes;
	layout->samples = 1;
	view->layout = layout;
	view->width = layout->padded_width;
	view->height = layout->padded_height;
	view->image_levels = 1;

	/* Succeeded: the layer's view. */
	return 0;
}

/*
 * Checks a copy that touches an image of four samples: both images of
 * four samples (EINVAL for one image of four samples and one of one,
 * which Vulkan does not copy between).
 *
 * Procedure referenced: the Vulkan 1.3 specification
 * (VUID-vkCmdCopyImage-srcImage-00136: the same sample count).
 */
static int
sample_copy_check(
	const struct powervr_vk_image *source,
	const struct powervr_vk_image *destination,
	const VkImageCopy *copy)
{
	/* Four samples on both sides. */
	(void)copy;
	if (source->layout.samples != destination->layout.samples)
		return EINVAL;

	/* Succeeded: a copy of samples to samples. */
	return 0;
}

/*
 * Tells whether a copy between two images of four samples moves whole
 * layers of one shape: one format and layout, the whole of each, which
 * the transfer copies texel by texel over their memory (sample_surface)
 * whatever the order of the samples in it.
 */
static bool
sample_copy_whole(
	const struct powervr_vk_image *source,
	const struct powervr_vk_image *destination,
	const VkImageCopy *copy)
{
	/* One format and layout. */
	if (source->format != destination->format ||
	    source->layout.padded_width != destination->layout.padded_width ||
	    source->layout.padded_height != destination->layout.padded_height ||
	    source->shape.width != destination->shape.width || source->shape.height != destination->shape.height)
		return false;

	/* The whole of each. */
	return copy->srcOffset.x == 0 && copy->srcOffset.y == 0 && copy->dstOffset.x == 0 && copy->dstOffset.y == 0 &&
	    copy->extent.width == source->shape.width && copy->extent.height == source->shape.height && copy->extent.depth == 1;
}

/*
 * Copies rows of 32-bit words between two linear places of the session's
 * memory, each with its own pitch: rows rows of row_bytes bytes (a
 * multiple of four).  The transfer is left running, the session's last
 * work, after the session's earlier work.
 */
int
powervr_vk_transfer_rows(
	struct powervr_vk_session *session,
	uint64_t source,
	uint32_t source_pitch,
	uint64_t destination,
	uint32_t destination_pitch,
	uint32_t row_bytes,
	uint32_t rows)
{
	struct transfer_work *work;
	const struct powervr_format *format;
	uint32_t width;
	int error;

	/* Whole words, in rows no wider than their pitches. */
	if (row_bytes == 0 || rows == 0 || (row_bytes % 4U) != 0 || (source_pitch % 4U) != 0 || (destination_pitch % 4U) != 0)
		return EINVAL;
	if (row_bytes > source_pitch || row_bytes > destination_pitch || rows > BUFFER_ROWS_MAX)
		return EINVAL;

	/* The working state. */
	work = kern_calloc(1, sizeof(*work));
	if (work == NULL)
		return ENOMEM;

	/* The raw 32-bit texels on both sides, as many as a row holds. */
	format = powervr_transfer_raw_format(4);
	width = row_bytes / 4U;

	/* The source: a linear view of its rows at their pitch. */
	work->layout.row_pitch = source_pitch;
	work->layout.padded_width = source_pitch / 4U;
	work->layout.padded_height = rows;
	work->layout.padded_depth = 1;
	work->layout.layer_bytes = (uint64_t)source_pitch * rows;
	work->layout.bytes = work->layout.layer_bytes;
	work->view.format = format;
	work->view.layout = &work->layout;
	work->view.address = source;
	work->view.width = width;
	work->view.height = rows;
	work->view.image_levels = 1;
	work->view.level_count = 1;
	work->view.layer_count = 1;
	work->view.linear = 1;

	/* The destination: the same rows at its own pitch, copied from the source's corner. */
	work->request.kind = POWERVR_TRANSFER_COPY;
	work->request.destination.format = format;
	work->request.destination.address = destination;
	work->request.destination.width = width;
	work->request.destination.height = rows;
	work->request.destination.linear = 1;
	work->request.destination.row_pitch = destination_pitch;
	work->request.destination.padded_width = destination_pitch / 4U;
	work->request.area.width = width;
	work->request.area.height = rows;
	work->request.source = &work->view;

	/* The transfer, into an image's memory rather than a buffer the host reads. */
	error = run_one(session, &work->request, false);
	kern_free(work);
	if (error != 0)
		return error;

	/* Succeeded: the rows are copied once the transfer has run. */
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
 * Gives a record's data room for count regions: a new allocation of the
 * data and its regions behind it, the scalars kept, which replaces the
 * record's data (the old one freed).  EINVAL past TRANSFER_REGIONS_MAX,
 * ENOMEM without memory (the old data stays the record's).
 */
static int
regions_make(
	struct powervr_vk_record *entry,
	struct transfer_data **data,
	uint64_t count)
{
	struct transfer_data *made;

	/* A count the command may carry. */
	if (count > TRANSFER_REGIONS_MAX)
		return EINVAL;

	/* The data with its regions behind it (the structure's size keeps them aligned as its pointer is). */
	made = kern_calloc(1, sizeof(*made) + (size_t)count * sizeof(union transfer_region));
	if (made == NULL)
		return ENOMEM;
	made->count = (uint32_t)count;
	made->filter = (*data)->filter;
	kern_memcpy(made->colour, (*data)->colour, sizeof(made->colour));
	made->regions.copies = (VkImageCopy *)(void *)(made + 1);

	/* The record keeps the new data. */
	kern_free(*data);
	entry->data = made;
	*data = made;

	/* Succeeded: room for the regions. */
	return 0;
}

/*
 * Works out the layers or slices one side of a region names at a level of
 * an image: a flat image's layers from base_layer (count of them, or the
 * remaining ones for VK_REMAINING_ARRAY_LAYERS) with z 0 and a depth of
 * one, or a volume's slices z to z + depth - 1 of the level, of layer 0.
 * EINVAL for a level, layers or slices outside the image.
 *
 * Procedure referenced: the Vulkan 1.3 specification, VkImageCopy and
 * VkBufferImageCopy (a 3D image's region by its offset's z and extent's
 * depth, another image's by its subresource's layers).
 */
static int
region_slices(
	const struct powervr_vk_image *image,
	uint32_t level,
	uint32_t base_layer,
	uint32_t layer_count,
	int32_t z,
	uint32_t depth,
	uint32_t *first,
	uint32_t *count)
{
	uint32_t slices;

	/* A level of the image. */
	if (level >= image->shape.levels)
		return EINVAL;

	/* A volume's slices of the level, from layer 0. */
	if (image->shape.volume) {
		if (base_layer != 0 || (layer_count != 1 && layer_count != VK_REMAINING_ARRAY_LAYERS))
			return EINVAL;
		slices = level_depth(image, level);
		if (z < 0 || depth == 0 || (uint32_t)z >= slices || depth > slices - (uint32_t)z)
			return EINVAL;
		*first = (uint32_t)z;
		*count = depth;
		return 0;
	}

	/* A flat image's layers, the region one slice deep. */
	if (z != 0 || depth != 1)
		return EINVAL;
	if (base_layer >= image->shape.layers)
		return EINVAL;
	if (layer_count == VK_REMAINING_ARRAY_LAYERS)
		layer_count = image->shape.layers - base_layer;
	if (layer_count == 0 || layer_count > image->shape.layers - base_layer)
		return EINVAL;
	*first = base_layer;
	*count = layer_count;

	/* Succeeded: the layers. */
	return 0;
}

/*
 * Works out the levels and layers a clear's range names (the remaining
 * ones for VK_REMAINING_MIP_LEVELS and VK_REMAINING_ARRAY_LAYERS); a
 * volume's range is its layer 0.  EINVAL for a range outside the image.
 */
static int
range_counts(
	const struct powervr_vk_image *image,
	const VkImageSubresourceRange *range,
	uint32_t *levels,
	uint32_t *layers)
{
	/* Levels of the image. */
	if (range->baseMipLevel >= image->shape.levels)
		return EINVAL;
	*levels = range->levelCount;
	if (*levels == VK_REMAINING_MIP_LEVELS)
		*levels = image->shape.levels - range->baseMipLevel;
	if (*levels == 0 || *levels > image->shape.levels - range->baseMipLevel)
		return EINVAL;

	/* Layers of the image. */
	if (range->baseArrayLayer >= image->shape.layers)
		return EINVAL;
	*layers = range->layerCount;
	if (*layers == VK_REMAINING_ARRAY_LAYERS)
		*layers = image->shape.layers - range->baseArrayLayer;
	if (*layers == 0 || *layers > image->shape.layers - range->baseArrayLayer)
		return EINVAL;

	/* Succeeded: the counts. */
	return 0;
}

/*
 * Checks each region of a recorded transfer against its images: a
 * clear's ranges and the levels, layers and slices of each side of a
 * copy (the same count on both sides), and a blit's layers of two flat
 * images with no negative corner.  EINVAL for what lies outside the
 * images, ENOTSUP for a blit of a volume.  A plane of a multi-planar
 * image was checked by planar_regions.
 */
static int
record_regions(
	uint32_t opcode,
	const struct powervr_vk_record *entry,
	const struct transfer_data *data)
{
	const struct powervr_vk_image *source;
	const struct powervr_vk_image *destination;
	const VkImageCopy *copy;
	const VkImageBlit *blit;
	const VkBufferImageCopy *buffer;
	uint32_t levels;
	uint32_t first;
	uint32_t count;
	uint32_t other;
	uint32_t index;
	int error;

	/* A resolve's images and regions are its own (resolve_regions). */
	if (opcode == GPU_OP_CMD_RESOLVE_IMAGE)
		return resolve_regions(entry, data);

	/*
	 * An image of four samples is cleared (whole subresources) or copied
	 * to another of four samples; a blit and a copy between it and a
	 * buffer are not valid usage (EINVAL).
	 *
	 * Procedure referenced: the Vulkan 1.3 specification
	 * (VUID-vkCmdBlitImage-srcImage-00233 and -dstImage-00234: both images
	 * of VK_SAMPLE_COUNT_1_BIT; VUID-vkCmdCopyBufferToImage-dstImage-07973
	 * and VUID-vkCmdCopyImageToBuffer-srcImage-07973: the image of
	 * VK_SAMPLE_COUNT_1_BIT).
	 */
	for (index = 0; index < entry->object_count; index++) {
		if (entry->objects[index] != NULL && entry->objects[index]->kind == I915_VK_OBJ_IMAGE &&
		    ((const struct powervr_vk_image *)entry->objects[index]->payload)->layout.samples == 4U &&
		    opcode != GPU_OP_CMD_CLEAR_COLOR_IMAGE && opcode != GPU_OP_CMD_COPY_IMAGE)
			return EINVAL;
	}

	/* Each region as its command names it. */
	error = 0;
	for (index = 0; error == 0 && index < data->count; index++) {
		if (opcode == GPU_OP_CMD_CLEAR_COLOR_IMAGE) {
			/* Levels and layers of the image. */
			destination = entry->objects[0]->payload;
			error = range_counts(destination, &data->regions.ranges[index], &levels, &count);
		} else if (opcode == GPU_OP_CMD_COPY_IMAGE) {
			/* As many layers or slices on each side. */
			copy = &data->regions.copies[index];
			source = entry->objects[0]->payload;
			destination = entry->objects[1]->payload;
			error = region_slices(source, copy->srcSubresource.mipLevel, copy->srcSubresource.baseArrayLayer, copy->srcSubresource.layerCount,
			    copy->srcOffset.z, copy->extent.depth, &first, &count);
			if (error == 0)
				error = region_slices(destination, copy->dstSubresource.mipLevel, copy->dstSubresource.baseArrayLayer, copy->dstSubresource.layerCount,
				    copy->dstOffset.z, copy->extent.depth, &first, &other);
			if (error == 0 && other != count)
				error = EINVAL;
			if (error == 0 && (copy->srcOffset.x < 0 || copy->srcOffset.y < 0 || copy->dstOffset.x < 0 || copy->dstOffset.y < 0))
				error = EINVAL;
			if (error == 0 && (source->layout.samples == 4U || destination->layout.samples == 4U))
				error = sample_copy_check(source, destination, copy);
		} else if (opcode == GPU_OP_CMD_BLIT_IMAGE) {
			/* Layers of two flat images, as many on each side, no corner negative. */
			blit = &data->regions.blits[index];
			source = entry->objects[0]->payload;
			destination = entry->objects[1]->payload;
			if (source->shape.volume || destination->shape.volume)
				return ENOTSUP;
			error = region_slices(source, blit->srcSubresource.mipLevel, blit->srcSubresource.baseArrayLayer, blit->srcSubresource.layerCount, 0, 1, &first, &count);
			if (error == 0)
				error = region_slices(destination, blit->dstSubresource.mipLevel, blit->dstSubresource.baseArrayLayer, blit->dstSubresource.layerCount, 0, 1, &first, &other);
			if (error == 0 && other != count)
				error = EINVAL;
			if (error == 0 && (blit->srcOffsets[0].x < 0 || blit->srcOffsets[0].y < 0 || blit->srcOffsets[1].x < 0 || blit->srcOffsets[1].y < 0 ||
			    blit->dstOffsets[0].x < 0 || blit->dstOffsets[0].y < 0 || blit->dstOffsets[1].x < 0 || blit->dstOffsets[1].y < 0))
				error = EINVAL;
			if (error == 0)
				error = blit_formats(source, destination, blit, data->filter);
		} else if (opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE || opcode == GPU_OP_CMD_COPY_IMAGE_TO_BUFFER) {
			/* The image's layers or slices, but a plane's (planar_regions). */
			buffer = &data->regions.buffers[index];
			destination = entry->objects[opcode == GPU_OP_CMD_COPY_BUFFER_TO_IMAGE ? 1 : 0]->payload;
			if (destination->plane_count == 0)
				error = region_slices(destination, buffer->imageSubresource.mipLevel, buffer->imageSubresource.baseArrayLayer, buffer->imageSubresource.layerCount,
				    buffer->imageOffset.z, buffer->imageExtent.depth, &first, &count);
			if (error == 0 && (buffer->imageOffset.x < 0 || buffer->imageOffset.y < 0))
				error = EINVAL;
		}
	}
	if (error != 0)
		return error;

	/* Succeeded: every region lies in its images. */
	return 0;
}

/*
 * Refuses a transfer that touches a multi-planar image (ENOTSUP) but a
 * copy from one to a buffer whose every region names one plane's aspect
 * at level 0.  Object 0 is the source and object 1 the destination; a
 * clear was checked when its image was held.
 */
static int
planar_regions(
	uint32_t opcode,
	const struct powervr_vk_record *entry,
	const struct transfer_data *data)
{
	const struct powervr_vk_image *source;
	const struct powervr_vk_image *destination;
	const VkBufferImageCopy *region;
	uint32_t texel_bytes;
	uint32_t aspect;
	uint32_t row;
	uint32_t index;

	/* The images of the copy: the source unless it is a buffer, the destination unless it is one. */
	source = NULL;
	destination = NULL;
	if (opcode != GPU_OP_CMD_COPY_BUFFER && opcode != GPU_OP_CMD_COPY_BUFFER_TO_IMAGE)
		source = entry->objects[0]->payload;
	if (opcode != GPU_OP_CMD_COPY_BUFFER && opcode != GPU_OP_CMD_COPY_IMAGE_TO_BUFFER)
		destination = entry->objects[1]->payload;

	/* Nothing is written into a multi-planar image. */
	if (destination != NULL && destination->plane_count != 0)
		return ENOTSUP;

	/* A copy of none needs nothing more. */
	if (source == NULL || source->plane_count == 0)
		return 0;

	/* Only to a buffer. */
	if (opcode != GPU_OP_CMD_COPY_IMAGE_TO_BUFFER)
		return ENOTSUP;

	/*
	 * Each region one plane of level 0, in whole 32-bit words of the
	 * plane's texels (plane_words): its start and width in the plane, its
	 * row and offset in the buffer.
	 */
	for (index = 0; index < data->count; index++) {
		region = &data->regions.buffers[index];
		aspect = region->imageSubresource.aspectMask;
		if (aspect == VK_IMAGE_ASPECT_PLANE_0_BIT && source->plane_count > 0) {
			texel_bytes = source->planes[0].format->texel_bytes;
		} else if (aspect == VK_IMAGE_ASPECT_PLANE_1_BIT && source->plane_count > 1) {
			texel_bytes = source->planes[1].format->texel_bytes;
		} else {
			return ENOTSUP;
		}
		if (region->imageSubresource.mipLevel != 0)
			return ENOTSUP;
		row = region->bufferRowLength != 0 ? region->bufferRowLength : region->imageExtent.width;
		if (((uint32_t)region->imageOffset.x * texel_bytes) % 4U != 0 ||
		    (region->imageExtent.width * texel_bytes) % 4U != 0 ||
		    (row * texel_bytes) % 4U != 0 ||
		    (region->bufferOffset % 4U) != 0)
			return ENOTSUP;
	}

	/* Succeeded: a copy of planes to a buffer. */
	return 0;
}

/*
 * Describes the plane of a multi-planar image an aspect names as a source
 * view: a linear image of the plane's own format and edges at the plane's
 * offset.  EINVAL for an aspect that names no plane of the image, EIO for
 * an image not bound.
 */
static int
plane_source(
	const struct powervr_vk_image *image,
	uint32_t aspect,
	struct powervr_view_shape *view)
{
	const struct powervr_image_plane *plane;
	uint64_t address;
	int error;

	/* The plane the aspect names. */
	if (aspect == VK_IMAGE_ASPECT_PLANE_0_BIT && image->plane_count > 0) {
		plane = &image->planes[0];
	} else if (aspect == VK_IMAGE_ASPECT_PLANE_1_BIT && image->plane_count > 1) {
		plane = &image->planes[1];
	} else {
		return EINVAL;
	}

	/* The image's memory. */
	error = powervr_vk_image_address(image, &address);
	if (error != 0)
		return error;

	/* All of the plane, the identity mapping. */
	kern_memset(view, 0, sizeof(*view));
	view->format = plane->format;
	view->layout = &plane->layout;
	view->address = address + plane->offset;
	view->width = plane->width;
	view->height = plane->height;
	view->image_levels = 1;
	view->level_count = 1;
	view->layer_count = 1;
	view->linear = true;

	/* Succeeded: the plane's view. */
	return 0;
}

/*
 * Turns a plane's source view into one of 32-bit words, the texel size
 * the transfer copies raw (transfer-program.c, powervr_transfer_raw_format,
 * has no one- or two-byte form): the plane's rows read as words of
 * per_word texels each, the same bytes in the same place.  layout gets
 * the words' layout.  ENOTSUP for a plane whose row is not whole words;
 * the region's own word alignment was checked when it was recorded
 * (planar_regions).
 */
static int
plane_words(
	struct powervr_view_shape *view,
	struct powervr_image_layout *layout,
	uint32_t *per_word)
{
	const struct powervr_format *words;
	uint32_t texel_bytes;

	/* A word format, and how many of the plane's texels fill one word. */
	words = powervr_format_find(VK_FORMAT_R8G8B8A8_UNORM);
	texel_bytes = view->format->texel_bytes;
	if (words == NULL || texel_bytes == 0 || 4U % texel_bytes != 0)
		return ENOTSUP;
	if ((view->width * texel_bytes) % 4U != 0 || (view->layout->row_pitch % 4U) != 0)
		return ENOTSUP;
	*per_word = 4U / texel_bytes;

	/* The same rows as words. */
	*layout = *view->layout;
	layout->padded_width = layout->row_pitch / 4U;
	view->layout = layout;
	view->format = words;
	view->width /= *per_word;

	/* Succeeded: the plane's view of words. */
	return 0;
}

/*
 * Works out a level of a twiddled image as an image of its own: the
 * level's edges (halved per level, at least one) and its padded row,
 * which must be the power of two the level's own width rounds up to
 * (the core twiddles a surface by its edges rounded up), else ENOTSUP;
 * a volume's level's depth the same.
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
	uint32_t padded_depth;
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

	/* A volume's level's depth twiddles as its padded depth too. */
	if (image->shape.volume) {
		padded_depth = image->layout.padded_depth >> level;
		if (padded_depth == 0)
			padded_depth = 1;
		power = 1;
		while (power < level_depth(image, level))
			power <<= 1;
		if (power != padded_depth)
			return ENOTSUP;
	}

	/* Succeeded: the level's edges. */
	return 0;
}

/* Reports the slices of a level of a volume (halved per level, at least one); 1 for any other image. */
static uint32_t
level_depth(
	const struct powervr_vk_image *image,
	uint32_t level)
{
	uint32_t depth;

	/* A flat image's level is one slice deep. */
	if (!image->shape.volume)
		return 1;

	/* The volume's depth halved per level, at least one. */
	depth = image->shape.depth >> level;
	if (depth == 0)
		depth = 1;
	return depth;
}

/*
 * Describes a layer of one level of an image as a destination, or a
 * slice of a level of a volume: the level's place (a layer's offset
 * past it), edges and rows, and a volume's depth and slice.
 */
static int
image_surface(
	const struct powervr_vk_image *image,
	uint32_t level,
	uint32_t slice,
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

	/* A volume's slice by the back end's slice, another image's layer by its offset. */
	surface->depth = 0;
	surface->slice = 0;
	if (image->shape.volume) {
		surface->depth = level_depth(image, level);
		surface->slice = slice;
		if (slice >= surface->depth)
			return EINVAL;
	} else {
		if (slice >= image->shape.layers)
			return EINVAL;
		surface->address += (uint64_t)slice * image->layout.layer_bytes;
	}

	/* Succeeded: the surface. */
	return 0;
}

/*
 * Describes a layer of one level of an image as a source view: level 0
 * as the image, a further level as an image of one level of its own at
 * the level's offset, its layout written to level_layout; a layer of a
 * flat image at the layer's offset.  A volume's view is all of the
 * level's slices, *z the slice read (0 for a flat image).
 */
static int
image_view(
	const struct powervr_vk_image *image,
	uint32_t level,
	uint32_t slice,
	struct powervr_view_shape *view,
	struct powervr_image_layout *level_layout,
	uint32_t *z)
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
		level_layout->padded_depth = image->layout.padded_depth >> level;
		if (level_layout->padded_depth == 0)
			level_layout->padded_depth = 1;
		level_layout->layer_bytes = (uint64_t)level_layout->row_pitch * level_layout->padded_height;
		level_layout->bytes = level_layout->layer_bytes;
		view->layout = level_layout;
		view->address = address + image->layout.level_offset[level];
		view->width = width;
		view->height = height;
		view->image_levels = 1;
	}

	/* A volume of the level's slices, the slice read; another image's layer at its offset. */
	*z = 0;
	if (image->shape.volume) {
		view->kind = POWERVR_VIEW_VOLUME;
		view->depth = level_depth(image, level);
		if (slice >= view->depth)
			return EINVAL;
		*z = slice;
	} else {
		if (slice >= image->shape.layers)
			return EINVAL;
		view->address += (uint64_t)slice * image->layout.layer_bytes;
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
		kern_logf("powervr: vk: a transfer did not run (%d): kind %u, %ux%u of format %u, area %ux%u, source %ux%u of format %u and %u sample(s)\n",
			  error, (unsigned)request->kind, (unsigned)request->destination.width, (unsigned)request->destination.height,
			  (unsigned)(request->destination.format != NULL ? request->destination.format->vk_format : 0U),
			  (unsigned)request->area.width, (unsigned)request->area.height,
			  (unsigned)(request->source != NULL ? request->source->width : 0U), (unsigned)(request->source != NULL ? request->source->height : 0U),
			  (unsigned)(request->source != NULL && request->source->format != NULL ? request->source->format->vk_format : 0U),
			  (unsigned)(request->source != NULL && request->source->layout != NULL ? request->source->layout->samples : 0U));
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

/*
 * Records vkCmdClearDepthStencilImage: [image][layout][present][float
 * depth][u32 stencil][u32 count][u64 count]{range}.  The clear runs as
 * an image's clear (the record's opcode becomes
 * GPU_OP_CMD_CLEAR_COLOR_IMAGE, which the submission runs as a
 * transfer) of the depth's value in the first channel and the
 * stencil's in the second, which the depth format's clear words take
 * (powervr_format_clear_words, or for the packed depth and stencil its
 * texel's bits, powervr_format_texel_words).  Refuses an image without
 * depth, a stencil aspect of a depth format without one, and one aspect
 * alone of a packed depth and stencil (ENOTSUP), and a range outside the
 * image (EINVAL).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/rogue/pvr_blit.c
 * (pvr_clear_depth_stencil_image: the depth as the first float of a
 * clear colour, cleared as an image range by a fill transfer).
 */
static int
record_depth_clear(
	struct powervr_vk_session *session,
	struct i915_wire_reader *reader,
	struct powervr_vk_record *entry,
	struct transfer_data **made)
{
	struct transfer_data *data;
	const struct powervr_vk_image *image;
	uint64_t destination;
	uint64_t count;
	uint64_t index;
	int error;

	/* The image, the depth and the ranges. */
	data = *made;
	destination = drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	data->colour[0] = drv_i915_wire_read_u32(reader);
	data->colour[1] = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;
	error = regions_make(entry, made, count);
	if (error != 0)
		return error;
	data = *made;
	for (index = 0; index < count; index++)
		i915_vkc_dec_VkImageSubresourceRange(reader, &session->arena, &data->regions.ranges[index]);
	if (reader->error != 0)
		return EINVAL;

	/* The image, held, and run as an image's clear. */
	error = hold(session, entry, I915_VK_OBJ_IMAGE, destination);
	if (error != 0)
		return error;
	entry->opcode = GPU_OP_CMD_CLEAR_COLOR_IMAGE;

	/* A depth image. */
	image = entry->objects[0]->payload;
	if (image->format == NULL || (image->format->kind & POWERVR_FORMAT_DEPTH) == 0)
		return ENOTSUP;

	/*
	 * Each range: the depth of a depth format, both aspects of a packed
	 * depth and stencil (the clear writes whole texels: one aspect alone
	 * is not taken), of levels and layers of the image.
	 */
	for (index = 0; index < count; index++) {
		if ((image->format->kind & POWERVR_FORMAT_STENCIL) != 0) {
			if (data->regions.ranges[index].aspectMask != (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
				return ENOTSUP;
		} else if ((data->regions.ranges[index].aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) != 0) {
			return ENOTSUP;
		}
	}
	error = record_regions(GPU_OP_CMD_CLEAR_COLOR_IMAGE, entry, data);
	if (error != 0)
		return error;

	/* Succeeded: the clear is recorded. */
	return 0;
}
