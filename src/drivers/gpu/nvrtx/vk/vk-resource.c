/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's buffers, images, image views and samplers.
 *
 * An image is 2D, of one layer and one sample.  A colour image of one
 * level is laid out pitch linear (its rows the width times four bytes
 * rounded up to NVRTX_VK_PITCH_ALIGN), except an optimal-tiling one drawn
 * to as a colour attachment, which is block linear like the levels of a
 * mipmapped image: the 3D engine draws with a depth target only into a
 * block-linear colour target, so such an image needs no shadow.  A
 * vkCreateImage may name the layout itself (GPU_OP_RECORD_IMAGE_LAYOUT,
 * linear rows or NVIDIA's block linear in one-GOB blocks), as a shared
 * image's importer does.  Anything else is refused by name
 * (VK_ERROR_FEATURE_NOT_PRESENT) rather than laid out wrongly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>
#include <uapi/gpu.h>

#include "drivers/gpu/nvrtx/nvrtx-gsp.h"
#include "drivers/gpu/nvrtx/vk/nvrtx-vk-objects.h"
#include "drivers/gpu/nvrtx/vk/nvrtx-video.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/*
 * The records a vkCreateImage chains at most: the external-memory
 * declaration and zedBSD's layout record.
 */
#define RESOURCE_CHAIN_MAX		2U

/*
 * The layout a vkCreateImage named with zedBSD's record
 * (GPU_OP_RECORD_IMAGE_LAYOUT), if it chained one: present, the layout
 * (GPU_IMAGE_*) and its parameter.
 */
struct resource_layout_record {
	bool present;
	uint32_t tiling;
	uint32_t parameter;
};

static int resource_create_buffer(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int resource_create_image(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int resource_image_decode(struct i915_wire_reader *reader, struct i915_wire_arena *arena, VkImageCreateInfo *info, struct resource_layout_record *record);
static int resource_image_chain(struct i915_wire_reader *reader, struct resource_layout_record *record, uint32_t depth);
static int resource_image_supported(const VkImageCreateInfo *info);
static int resource_image_block_linear(const VkImageCreateInfo *info, const struct resource_layout_record *record, bool *block_linear);
static void resource_image_layout(struct nvrtx_vk_image *image, bool block_linear);
static int resource_layout(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int resource_create_view(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static uint32_t resource_swizzle(VkComponentSwizzle swizzle, uint32_t identity);
static uint32_t resource_view_plane(const VkImageViewCreateInfo *info);
static int resource_create_sampler(struct nvrtx_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int resource_buffer_destroy(struct nvrtx_vk_session *session, void *payload);
static int resource_image_destroy(struct nvrtx_vk_session *session, void *payload);
static int resource_view_destroy(struct nvrtx_vk_session *session, void *payload);

/*
 * Runs a buffer, image, view or sampler command; *handled stays clear for
 * an opcode of another handler's.
 */
int
nvrtx_vk_resource_route(
	struct nvrtx_vk_session *session,
	uint32_t opcode,
	uint32_t requested,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply,
	int *handled)
{
	int error;

	UNUSED_PARAMETER(requested);

	/* Picks the command, or leaves it to another handler. */
	*handled = 1;
	switch (opcode) {
	case GPU_OP_CREATE_BUFFER:
		error = resource_create_buffer(session, reader, reply);
		break;
	case GPU_OP_DESTROY_BUFFER:
		error = nvrtx_vk_destroy_plain(session, reader, I915_VK_OBJ_BUFFER);
		break;
	case GPU_OP_CREATE_IMAGE:
		error = resource_create_image(session, reader, reply);
		break;
	case GPU_OP_DESTROY_IMAGE:
		error = nvrtx_vk_destroy_plain(session, reader, I915_VK_OBJ_IMAGE);
		break;
	case GPU_OP_GET_IMAGE_SUBRESOURCE_LAYOUT:
		error = resource_layout(session, reader, reply);
		break;
	case GPU_OP_CREATE_IMAGE_VIEW:
		error = resource_create_view(session, reader, reply);
		break;
	case GPU_OP_DESTROY_IMAGE_VIEW:
		error = nvrtx_vk_destroy_plain(session, reader, I915_VK_OBJ_IMAGE_VIEW);
		break;
	case GPU_OP_CREATE_SAMPLER:
		error = resource_create_sampler(session, reader, reply);
		break;
	case GPU_OP_DESTROY_SAMPLER:
		error = nvrtx_vk_destroy_plain(session, reader, I915_VK_OBJ_SAMPLER);
		break;
	default:
		*handled = 0;
		return 0;
	}

	/* Reports a command that could not be decoded. */
	if (error != 0)
		return error;

	/* Succeeded: the command ran and wrote its reply. */
	return 0;
}

/*
 * Reports whether a format is one of the four-byte colour formats every
 * image of the executor is in.
 */
bool
nvrtx_vk_format_colour(
	uint32_t format)
{
	/* B8G8R8A8 and R8G8B8A8, as UNORM and as SRGB. */
	switch (format) {
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB:
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB:
		return true;
	default:
		break;
	}

	/* Any other format is not one. */
	return false;
}

/*
 * Reports whether a format is one of the depth formats a depth attachment
 * is in: D32_SFLOAT, and D24_UNORM_S8_UINT with a stencil (four bytes a
 * pixel either way).
 */
bool
nvrtx_vk_format_depth(
	uint32_t format)
{
	/* The two depth formats. */
	switch (format) {
	case VK_FORMAT_D32_SFLOAT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
		return true;
	default:
		break;
	}

	/* Any other format is not one. */
	return false;
}

/* vkCreateBuffer: [device][present][VkBufferCreateInfo] and the create's tail -> the create's reply. */
static int
resource_create_buffer(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkBufferCreateInfo info;
	struct nvrtx_vk_buffer *buffer;
	uint64_t identity;
	int error;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkBufferCreateInfo(reader, &session->arena, &info);
	identity = nvrtx_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A buffer of some bytes. */
	error = 0;
	if (info.size == 0U)
		error = EINVAL;

	/* The buffer keeps its size and usage; it has no storage until it is bound. */
	buffer = NULL;
	if (error == 0) {
		buffer = kern_calloc(1U, sizeof(*buffer));
		if (buffer != NULL) {
			buffer->size = info.size;
			buffer->usage = info.usage;
		}
	}

	/* Publishes the buffer and answers. */
	nvrtx_vk_create_reply(session, reply, I915_VK_OBJ_BUFFER, identity, buffer, resource_buffer_destroy, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/* vkCreateImage: [device][present][VkImageCreateInfo] and the create's tail -> the create's reply. */
static int
resource_create_image(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageCreateInfo info;
	struct resource_layout_record record;
	struct nvrtx_vk_image *image;
	uint64_t identity;
	bool video;
	bool block_linear;
	int supported;
	int error;

	/* The create info behind the device and its presence word, with the layout it may name. */
	kern_memset(&info, 0, sizeof(info));
	kern_memset(&record, 0, sizeof(record));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	error = resource_image_decode(reader, &session->arena, &info, &record);
	if (error != 0)
		return error;
	identity = nvrtx_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Refuses by name an image the executor does not lay out (a video image only with the video decoder). */
	error = 0;
	supported = resource_image_supported(&info);

	/* A video picture is laid out only by a device that decodes video. */
	if (supported == 2) {
		video = nvrtx_vk_video_available(session->owner);
		if (!video)
			supported = 0;
	}
	if (supported == 0) {
		kern_logf("nvrtx: vk: vkCreateImage refused: type %u format %u %ux%ux%u levels %u layers %u samples %u\n",
			  (unsigned)info.imageType,
			  (unsigned)info.format,
			  info.extent.width,
			  info.extent.height,
			  info.extent.depth,
			  info.mipLevels,
			  info.arrayLayers,
			  (unsigned)info.samples);
		error = ENOTSUP;
	}

	/* The layout: the one the record names, else block linear for an optimal colour attachment. */
	block_linear = false;
	if (error == 0) {
		error = resource_image_block_linear(&info, &record, &block_linear);
		if (error != 0) {
			kern_logf("nvrtx: vk: vkCreateImage refused: layout %08x parameter %08x for format %u levels %u\n",
				  record.tiling,
				  record.parameter,
				  (unsigned)info.format,
				  info.mipLevels);
		}
	}

	/* Lays the image out. */
	image = NULL;
	if (error == 0) {
		image = kern_calloc(1U, sizeof(*image));
		if (image != NULL) {
			image->format = info.format;
			image->width = info.extent.width;
			image->height = info.extent.height;
			image->usage = info.usage;
			image->levels = info.mipLevels;
			resource_image_layout(image, block_linear);
		}
	}

	/* Publishes the image and answers. */
	nvrtx_vk_create_reply(session, reply, I915_VK_OBJ_IMAGE, identity, image, resource_image_destroy, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * Reads a vkCreateImage's VkImageCreateInfo as the shared decoder does,
 * but with its chain read record by record: the external-memory
 * declaration is skipped and zedBSD's layout record kept.  EINVAL for a
 * chain of another record or longer than the two.
 */
static int
resource_image_decode(
	struct i915_wire_reader *reader,
	struct i915_wire_arena *arena,
	VkImageCreateInfo *info,
	struct resource_layout_record *record)
{
	uint64_t count;
	uint64_t index;
	uint32_t family;
	uint32_t *families;
	int error;

	/* The structure type, then the chain. */
	info->sType = (VkStructureType)drv_i915_wire_read_u32(reader);
	error = resource_image_chain(reader, record, 0U);
	if (error != 0)
		return error;

	/* The fields, in the order of VkImageCreateInfo. */
	info->flags = drv_i915_wire_read_u32(reader);
	info->imageType = (VkImageType)drv_i915_wire_read_u32(reader);
	info->format = (VkFormat)drv_i915_wire_read_u32(reader);
	i915_vkc_dec_VkExtent3D(reader, arena, &info->extent);
	info->mipLevels = drv_i915_wire_read_u32(reader);
	info->arrayLayers = drv_i915_wire_read_u32(reader);
	info->samples = (VkSampleCountFlagBits)drv_i915_wire_read_u32(reader);
	info->tiling = (VkImageTiling)drv_i915_wire_read_u32(reader);
	info->usage = drv_i915_wire_read_u32(reader);
	info->sharingMode = (VkSharingMode)drv_i915_wire_read_u32(reader);
	info->queueFamilyIndexCount = drv_i915_wire_read_u32(reader);

	/* The queue families, counted, into the session's arena. */
	count = drv_i915_wire_read_u64(reader);
	info->pQueueFamilyIndices = NULL;
	if (count != 0U) {
		families = i915_vkc_array(reader, arena, count, 4U);
		for (index = 0U; families != NULL && index < count && reader->error == 0; index++) {
			family = drv_i915_wire_read_u32(reader);
			families[index] = family;
		}
		info->pQueueFamilyIndices = families;
	}
	info->initialLayout = (VkImageLayout)drv_i915_wire_read_u32(reader);

	/* A short or malformed stream. */
	if (reader->error != 0)
		return EINVAL;

	/* Succeeded: the create info and the layout it names, if any. */
	return 0;
}

/*
 * Reads one record of a vkCreateImage's chain and the records after it:
 * [u64 present], then [u32 sType], the next record, and the record's own
 * fields.  The external-memory declaration ([u32 handle types]) is
 * skipped, zedBSD's layout record ([u32 tiling][u32 parameter]) kept.
 * EINVAL for another record, a second layout record or a chain longer
 * than RESOURCE_CHAIN_MAX.
 */
static int
resource_image_chain(
	struct i915_wire_reader *reader,
	struct resource_layout_record *record,
	uint32_t depth)
{
	uint64_t present;
	uint32_t type;
	int error;

	/* The end of the chain. */
	present = drv_i915_wire_read_u64(reader);
	if (present == 0U)
		return 0;
	if (depth >= RESOURCE_CHAIN_MAX)
		return EINVAL;

	/* The record's type, then the records chained after it. */
	type = drv_i915_wire_read_u32(reader);
	error = resource_image_chain(reader, record, depth + 1U);
	if (error != 0)
		return error;

	/* The external-memory declaration says nothing the layout needs. */
	if (type == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO) {
		(void)drv_i915_wire_read_u32(reader);
		return 0;
	}

	/* Any other record but one layout record is not this executor's. */
	if (type != GPU_OP_RECORD_IMAGE_LAYOUT || record->present)
		return EINVAL;

	/* Succeeded: the layout the image is asked to have. */
	record->tiling = drv_i915_wire_read_u32(reader);
	record->parameter = drv_i915_wire_read_u32(reader);
	record->present = true;
	return 0;
}

/*
 * Decides whether an image is laid out block linear: as its layout record
 * says (linear rows, or NVIDIA's block linear in one-GOB blocks of the
 * ordinary kind, for a colour image of optimal tiling), else for a colour
 * image of one level and optimal tiling drawn to as a colour attachment
 * (an image of more levels is tiled anyway).  ENOTSUP for a layout the
 * executor does not make.
 */
static int
resource_image_block_linear(
	const VkImageCreateInfo *info,
	const struct resource_layout_record *record,
	bool *block_linear)
{
	bool colour;

	/* Without a record the executor chooses. */
	*block_linear = false;
	colour = nvrtx_vk_format_colour(info->format);
	if (!record->present) {
		if (colour &&
		    info->tiling == VK_IMAGE_TILING_OPTIMAL &&
		    info->mipLevels == 1U &&
		    (info->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0U)
			*block_linear = true;
		return 0;
	}

	/* A named layout is a colour image's, of optimal tiling. */
	if (!colour || info->tiling != VK_IMAGE_TILING_OPTIMAL)
		return ENOTSUP;

	/* Linear rows: one level, as the display and the importers read them. */
	if (record->tiling == GPU_IMAGE_LINEAR && record->parameter == 0U) {
		/* Mipmapped levels are tiled; only one level can be linear rows. */
		if (info->mipLevels != 1U)
			return ENOTSUP;

		/* Succeeded: linear rows, as asked. */
		return 0;
	}

	/* Block linear in one-GOB blocks of the ordinary kind, the one variant the executor makes. */
	if (record->tiling != GPU_IMAGE_NV_BLOCK_LINEAR || record->parameter != 0U)
		return ENOTSUP;

	/* Succeeded: block linear, as asked. */
	*block_linear = true;
	return 0;
}

/*
 * Lays an image out.  A colour image of one level is pitch linear, its rows
 * of its width's pixels on the pitch boundary, unless block_linear asks
 * for it to be tiled.  A tiled colour image (of more levels, or of one so
 * asked) has each level block linear in one-GOB blocks (its rows a whole
 * number of GOBs wide, its height a whole number of GOBs) right after the
 * one before.  A depth image is block linear in one-GOB blocks.
 *
 * Procedure referenced: Mesa 25.0.7 src/nouveau/nil/image.rs (Image::new,
 * the levels of a tiled image one after another, each a whole number of
 * blocks).
 */
static void
resource_image_layout(
	struct nvrtx_vk_image *image,
	bool block_linear)
{
	struct nvrtx_video_layout video;
	uint64_t at;
	uint32_t level;
	uint32_t width;
	uint32_t height;
	uint32_t row;

	/* A video image: two block-linear planes in blocks two GOBs high. */
	if (image->format == NVRTX_VK_FORMAT_NV12) {
		nvrtx_video_layout(image->width, image->height, &video);
		image->video = true;
		image->pitch = video.pitch;
		image->plane_offset[0] = video.plane_offset[0];
		image->plane_offset[1] = video.plane_offset[1];
		image->plane_rows[0] = video.plane_rows[0];
		image->plane_rows[1] = video.plane_rows[1];
		image->bytes = video.bytes;
		image->level_offset[0] = 0U;
		image->level_pitch[0] = image->pitch;
		return;
	}

	/* A depth image: GOB rows and GOB heights (with its stencil, the same four bytes a pixel). */
	if (nvrtx_vk_format_depth(image->format)) {
		row = image->width * NVRTX_VK_PIXEL_BYTES;
		image->depth = true;
		image->stencil = (image->format == VK_FORMAT_D24_UNORM_S8_UINT);
		image->pitch = (row + NVRTX_VK_GOB_WIDTH - 1U) & ~(NVRTX_VK_GOB_WIDTH - 1U);
		height = (image->height + NVRTX_VK_GOB_HEIGHT - 1U) & ~(NVRTX_VK_GOB_HEIGHT - 1U);
		image->bytes = (uint64_t)image->pitch * height;
		if (image->stencil)
			image->bytes = (image->bytes + NVRTX_VK_KIND_ALIGN - 1U) & ~((uint64_t)NVRTX_VK_KIND_ALIGN - 1U);
		image->level_offset[0] = 0U;
		image->level_pitch[0] = image->pitch;
		return;
	}

	/* A colour image of one level not asked to be block linear: pitch linear. */
	if (image->levels == 1U && !block_linear) {
		row = image->width * NVRTX_VK_PIXEL_BYTES;
		image->pitch = (row + NVRTX_VK_PITCH_ALIGN - 1U) & ~(NVRTX_VK_PITCH_ALIGN - 1U);
		image->bytes = (uint64_t)image->pitch * image->height;
		image->level_offset[0] = 0U;
		image->level_pitch[0] = image->pitch;
		return;
	}

	/* A tiled image (of one level, or more): each level halved from the one before, one pixel at least, in whole GOBs. */
	image->tiled = true;
	at = 0U;
	width = image->width;
	height = image->height;
	for (level = 0U; level < image->levels; level++) {
		row = width * NVRTX_VK_PIXEL_BYTES;
		image->level_offset[level] = at;
		image->level_pitch[level] = (row + NVRTX_VK_GOB_WIDTH - 1U) & ~(NVRTX_VK_GOB_WIDTH - 1U);
		at += (uint64_t)image->level_pitch[level] * ((height + NVRTX_VK_GOB_HEIGHT - 1U) & ~(NVRTX_VK_GOB_HEIGHT - 1U));
		if (width > 1U)
			width >>= 1;
		if (height > 1U)
			height >>= 1;
	}
	image->pitch = image->level_pitch[0];
	image->bytes = at;
}

/*
 * Reports where a level of an image starts, its size and its row pitch
 * (a level past the image's last is its last).
 */
void
nvrtx_vk_image_level(
	const struct nvrtx_vk_image *image,
	uint32_t level,
	uint64_t *offset,
	uint32_t *width,
	uint32_t *height,
	uint32_t *pitch)
{
	uint32_t w;
	uint32_t h;
	uint32_t index;

	/* One of the image's levels. */
	if (image->levels != 0U && level >= image->levels)
		level = image->levels - 1U;

	/* The size halved per level, one pixel at least. */
	w = image->width;
	h = image->height;
	for (index = 0U; index < level; index++) {
		if (w > 1U)
			w >>= 1;
		if (h > 1U)
			h >>= 1;
	}

	/* Succeeded: the level's place and shape. */
	*offset = image->level_offset[level];
	*width = w;
	*height = h;
	*pitch = image->level_pitch[level];
}

/*
 * Reports whether an image is one the executor lays out: 2D, one layer,
 * one sample, within the largest side; a colour format of any number of
 * levels, or a depth format (D32_SFLOAT, D24_UNORM_S8_UINT) of one level
 * as a depth attachment (sampled or copied it is not); 2 for an NV12 video picture, which only a device
 * with the video decoder lays out.
 */
static int
resource_image_supported(
	const VkImageCreateInfo *info)
{
	uint32_t side;
	uint32_t levels;
	uint32_t allowed;
	bool colour;

	/* A 2D image of one layer and one sample, of a level at least. */
	if (info->imageType != VK_IMAGE_TYPE_2D)
		return 0;
	if (info->mipLevels == 0U || info->mipLevels > NVRTX_VK_LEVELS_MAX)
		return 0;
	if (info->arrayLayers != 1U)
		return 0;
	if (info->samples != VK_SAMPLE_COUNT_1_BIT)
		return 0;
	if (info->extent.depth != 1U)
		return 0;

	/* Of a size the colour target and the texture header take. */
	if (info->extent.width == 0U || info->extent.width > NVRTX_VK_IMAGE_SIDE_MAX)
		return 0;
	if (info->extent.height == 0U || info->extent.height > NVRTX_VK_IMAGE_SIDE_MAX)
		return 0;

	/*
	 * A video picture of one level and even sides, decoded into, a
	 * reference, read out by a copy and its planes sampled, nothing else
	 * (2: the caller asks whether the device decodes video).
	 */
	if (info->format == NVRTX_VK_FORMAT_NV12) {
		/* One level, in optimal tiling. */
		if (info->mipLevels != 1U || info->tiling != VK_IMAGE_TILING_OPTIMAL)
			return 0;

		/* Even sides, as the half-size chroma plane needs. */
		if ((info->extent.width & 1U) != 0U || (info->extent.height & 1U) != 0U)
			return 0;

		/* Only the uses a video picture has. */
		allowed = VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR |
			  VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR |
			  VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
			  VK_IMAGE_USAGE_SAMPLED_BIT;
		if ((info->usage & ~allowed) != 0U)
			return 0;

		/* Succeeded: a video picture, which only a device with the decoder lays out. */
		return 2;
	}

	/* A depth attachment of one level, nothing else done with it. */
	if (nvrtx_vk_format_depth(info->format)) {
		if (info->mipLevels != 1U)
			return 0;
		if ((info->usage & ~(uint32_t)(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)) != 0U)
			return 0;
		return 1;
	}

	/* In one of the colour formats. */
	colour = nvrtx_vk_format_colour(info->format);
	if (!colour)
		return 0;

	/* No more levels than halving the longer side down to one pixel gives. */
	side = info->extent.width;
	if (info->extent.height > side)
		side = info->extent.height;
	levels = 1U;
	while (side > 1U) {
		side >>= 1;
		levels++;
	}
	if (info->mipLevels > levels)
		return 0;

	/* Succeeded: the image is laid out pitch linear. */
	return 1;
}

/*
 * vkGetImageSubresourceLayout: [device][image][present]
 * [VkImageSubresource][present] -> [present][VkSubresourceLayout].  The one
 * subresource of a colour image of one level starts at 0 with the image's
 * pitch (a block-linear one's too: zedBSD's shared images name its rows of
 * GOBs so, though Vulkan defines the layout of linear images only); a
 * mipmapped, depth, video or unknown image or another subresource reports
 * an empty layout.
 */
static int
resource_layout(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageSubresource subresource;
	VkSubresourceLayout layout;
	const struct nvrtx_vk_image *image;
	uint64_t identity;
	uint64_t present;
	uint64_t offset;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;

	/* The image behind the device, then the subresource when present (level 0 when absent). */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	kern_memset(&subresource, 0, sizeof(subresource));
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkImageSubresource(reader, &session->arena, &subresource);

	/* Skips the output's presence word. */
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/*
	 * Level 0 of layer 0 of a colour image of one level: where it starts,
	 * its rows pitch bytes apart (rows of GOBs for a block-linear one, as
	 * its shared description names them), and its bytes.
	 */
	kern_memset(&layout, 0, sizeof(layout));
	image = nvrtx_vk_payload(session, I915_VK_OBJ_IMAGE, identity);
	if (image != NULL &&
	    !image->depth &&
	    !image->video &&
	    (!image->tiled || image->levels == 1U) &&
	    subresource.mipLevel == 0U &&
	    subresource.arrayLayer == 0U) {
		nvrtx_vk_image_level(image, subresource.mipLevel, &offset, &width, &height, &pitch);
		layout.offset = offset;
		layout.size = (uint64_t)pitch * height;
		if (image->tiled)
			layout.size = image->bytes;
		layout.rowPitch = pitch;
		layout.arrayPitch = image->bytes;
		layout.depthPitch = layout.size;
	}

	/* The layout behind its presence word. */
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkSubresourceLayout(reply, &layout);

	/* Succeeded: the reply carries the layout. */
	return 0;
}

/*
 * vkCreateImageView: [device][present][VkImageViewCreateInfo] and the
 * create's tail -> the create's reply.  A 2D view of the one level and
 * layer of a known image, in a colour format.
 */
static int
resource_create_view(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageViewCreateInfo info;
	struct nvrtx_vk_object *image;
	const struct nvrtx_vk_image *picture;
	struct nvrtx_vk_view *view;
	uint64_t identity;
	uint32_t plane;
	bool colour;
	int error;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkImageViewCreateInfo(reader, &session->arena, &info);
	identity = nvrtx_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/*
	 * A view of a known image, 2D, of one of its levels and layer 0, in a
	 * colour format, or the depth image in its own format.
	 */
	error = 0;
	image = nvrtx_vk_object_find(session, I915_VK_OBJ_IMAGE, (uint64_t)(uintptr_t)info.image);
	picture = NULL;
	if (image != NULL)
		picture = image->payload;
	colour = nvrtx_vk_format_colour(info.format);
	plane = 0U;
	if (picture == NULL) {
		error = EINVAL;
	} else if (info.viewType != VK_IMAGE_VIEW_TYPE_2D) {
		error = ENOTSUP;
	} else if (info.subresourceRange.baseMipLevel >= picture->levels || info.subresourceRange.baseArrayLayer != 0U) {
		error = EINVAL;
	} else if (picture->depth) {
		if (info.format != picture->format)
			error = ENOTSUP;
	} else if (picture->video) {
		plane = resource_view_plane(&info);
		if (info.format != NVRTX_VK_FORMAT_NV12 && plane == 0U)
			error = ENOTSUP;
	} else if (!colour) {
		error = ENOTSUP;
	}

	/* The view holds its image. */
	view = NULL;
	if (error == 0)
		error = nvrtx_vk_object_retain(image);
	if (error == 0) {
		view = kern_calloc(1U, sizeof(*view));
		if (view == NULL)
			(void)nvrtx_vk_object_release(image);
	}

	/* The image, the format, and where each channel comes from. */
	if (view != NULL) {
		view->image = image;
		view->format = info.format;
		view->base_level = info.subresourceRange.baseMipLevel;
		view->level_count = picture->levels - view->base_level;
		if (info.subresourceRange.levelCount != VK_REMAINING_MIP_LEVELS && info.subresourceRange.levelCount != 0U &&
		    info.subresourceRange.levelCount < view->level_count)
			view->level_count = info.subresourceRange.levelCount;
		view->swizzle[0] = resource_swizzle(info.components.r, VK_COMPONENT_SWIZZLE_R);
		view->swizzle[1] = resource_swizzle(info.components.g, VK_COMPONENT_SWIZZLE_G);
		view->swizzle[2] = resource_swizzle(info.components.b, VK_COMPONENT_SWIZZLE_B);
		view->swizzle[3] = resource_swizzle(info.components.a, VK_COMPONENT_SWIZZLE_A);
		view->plane = plane;
	}

	/* Publishes the view and answers. */
	nvrtx_vk_create_reply(session, reply, I915_VK_OBJ_IMAGE_VIEW, identity, view, resource_view_destroy, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * Reports the plane a view of a video image reads: 1 the luma plane as
 * R8_UNORM, 2 the chroma plane as R8G8_UNORM, 0 for any other view.
 */
static uint32_t
resource_view_plane(
	const VkImageViewCreateInfo *info)
{
	/* The luma plane, then the chroma plane. */
	if (info->format == VK_FORMAT_R8_UNORM && info->subresourceRange.aspectMask == VK_IMAGE_ASPECT_PLANE_0_BIT)
		return 1U;
	if (info->format == VK_FORMAT_R8G8_UNORM && info->subresourceRange.aspectMask == VK_IMAGE_ASPECT_PLANE_1_BIT)
		return 2U;

	/* Succeeded: not a plane's view. */
	return 0U;
}

/* Reports where a channel of a view comes from: the identity swizzle is the channel's own. */
static uint32_t
resource_swizzle(
	VkComponentSwizzle swizzle,
	uint32_t identity)
{
	/* The identity names the channel itself. */
	if (swizzle == VK_COMPONENT_SWIZZLE_IDENTITY)
		return identity;

	/* Succeeded: the channel the view names. */
	return (uint32_t)swizzle;
}

/* vkCreateSampler: [device][present][VkSamplerCreateInfo] and the create's tail -> the create's reply. */
static int
resource_create_sampler(
	struct nvrtx_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkSamplerCreateInfo info;
	struct nvrtx_vk_sampler *sampler;
	uint64_t identity;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkSamplerCreateInfo(reader, &session->arena, &info);
	identity = nvrtx_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The sampler keeps what a texture read uses. */
	sampler = kern_calloc(1U, sizeof(*sampler));
	if (sampler != NULL) {
		sampler->mag_filter = info.magFilter;
		sampler->min_filter = info.minFilter;
		sampler->mipmap_mode = info.mipmapMode;
		kern_memcpy(&sampler->min_lod, &info.minLod, sizeof(sampler->min_lod));
		kern_memcpy(&sampler->max_lod, &info.maxLod, sizeof(sampler->max_lod));
		sampler->address_u = info.addressModeU;
		sampler->address_v = info.addressModeV;
		sampler->address_w = info.addressModeW;
		sampler->border_color = info.borderColor;
		sampler->unnormalized = info.unnormalizedCoordinates;
	}

	/* Publishes the sampler and answers. */
	nvrtx_vk_create_reply(session, reply, I915_VK_OBJ_SAMPLER, identity, sampler, nvrtx_vk_free_payload, 0);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/* Destroys a buffer nothing holds: it lets its memory go. */
static int
resource_buffer_destroy(
	struct nvrtx_vk_session *session,
	void *payload)
{
	struct nvrtx_vk_buffer *buffer;
	int error;

	UNUSED_PARAMETER(session);

	buffer = payload;

	/* The memory may go with it when nothing else holds the memory. */
	error = nvrtx_vk_object_release(buffer->memory);
	kern_free(buffer);

	/* Reports a memory whose destruction failed. */
	if (error != 0)
		return error;

	/* Succeeded: the buffer is gone. */
	return 0;
}

/*
 * Destroys an image nothing holds: a depth image with a stencil gives its
 * pages their pitch kind back (the work that drew into it has ended, as
 * Vulkan has its destruction wait for), then it lets its memory go.
 */
static int
resource_image_destroy(
	struct nvrtx_vk_session *session,
	void *payload)
{
	const struct nvrtx_vk_memory *memory;
	struct nvrtx_vk_image *image;
	uint64_t va;
	uint8_t *cpu;
	int error;

	image = payload;

	/* The stencil image's pages back to the pitch kind, while its memory is still there. */
	if (image->kinded) {
		memory = image->memory->payload;
		error = nvrtx_vk_image_address(image, &va, &cpu);
		if (error == 0)
			error = nvrtx_vk_memory_kind(session->owner, memory->backing, image->offset, va, image->bytes, NVRTX_MMU_KIND_PITCH);
		if (error != 0)
			kern_logf("nvrtx: vk: a stencil image's pages kept their kind (%d)\n", error);
		image->kinded = false;
	}

	/* The memory may go with it when nothing else holds the memory. */
	error = nvrtx_vk_object_release(image->memory);
	kern_free(image);

	/* Reports a memory whose destruction failed. */
	if (error != 0)
		return error;

	/* Succeeded: the image is gone. */
	return 0;
}

/* Destroys a view nothing holds: it lets its image go. */
static int
resource_view_destroy(
	struct nvrtx_vk_session *session,
	void *payload)
{
	struct nvrtx_vk_view *view;
	int error;

	UNUSED_PARAMETER(session);

	view = payload;

	/* The image may go with it when nothing else holds the image. */
	error = nvrtx_vk_object_release(view->image);
	kern_free(view);

	/* Reports an image whose destruction failed. */
	if (error != 0)
		return error;

	/* Succeeded: the view is gone. */
	return 0;
}
