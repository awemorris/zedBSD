/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's buffers, buffer views, images, image views, samplers and
 * query pools.  A buffer view reads a buffer's range as texels of a
 * format the buffer descriptors fetch (amdrdna_vk_texel_format()).
 *
 * An image is 2D of one or more layers, or 3D, of one sample, laid out
 * pitch linear: each level's rows are its width's bytes rounded up to
 * AMDRDNA_VK_PITCH_ALIGN, and each slice (a layer, or a 3D image's depth
 * slice) holds every level, one slice after the other, as GFX10's linear
 * surfaces lie (only a colour image, or one of one or two bytes a texel,
 * has more than one slice).
 * A four-byte colour image serves the display, the texture header and the
 * colour target in that one layout, so it is never converted.  An image of
 * one or two bytes a texel is sampled and copied only; an image of a
 * storage format (amdrdna_vk_format_storage()) is a storage image, copied
 * by the command processor's moves.  A two-plane 4:2:0
 * image (NV12) of one level holds its luma plane (one byte a texel) and
 * after it, on AMDRDNA_VK_PLANE_ALIGN, its chroma plane (two bytes a texel,
 * half as wide and half as tall), both at the same row pitch, which is
 * what a plane view's descriptor and a video engine read and write.
 * Anything else is refused by name (VK_ERROR_FEATURE_NOT_PRESENT) rather
 * than laid out wrongly.
 *
 * A query pool holds result status queries only, the status a video
 * decode reports into; its queries are written by the records of the
 * submissions that run, on the CPU and in order, so its results never
 * wait for the GPU.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/klog.h>
#include <kern/kmem.h>
#include <uapi/errno.h>
#include <uapi/gpu-op.h>

#include "drivers/gpu/amdrdna/vk/amdrdna-vk-objects.h"
#include "drivers/gpu/amdrdna/vk/amdrdna-vk-exec.h"

#include "drivers/gpu/i915/vk/vk-codec.inc"

/*
 * The create flags a two-plane image may have: views of its planes in
 * their own formats, and usages its planes' formats have that the
 * two-plane format does not (sampling).
 */
#define RESOURCE_PLANAR_FLAGS		(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)

/*
 * The usages a two-plane image may have: copied from and to, its planes
 * sampled, and written and referenced by a video decode.
 */
#define RESOURCE_PLANAR_USAGES		(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | \
					 VK_IMAGE_USAGE_TRANSFER_DST_BIT | \
					 VK_IMAGE_USAGE_SAMPLED_BIT | \
					 VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | \
					 VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR)

/* The usages of a video engine's pictures, whose planes take whole groups of AMDRDNA_VK_VIDEO_ROWS rows. */
#define RESOURCE_VIDEO_USAGES		(VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR)

/* The usages an image of one or two bytes a texel may have: copied from and to, and sampled. */
#define RESOURCE_NARROW_USAGES		(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)

/* The usages of an image of a storage format: a storage image, a colour attachment of one sample, copied. */
#define RESOURCE_STORAGE_USAGES		(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | \
					 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)

/*
 * The usages a depth image is made with: a depth attachment, sampled
 * (its depth or its stencil), and copied and blitted from and to.
 */
#define RESOURCE_DEPTH_USAGES		(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | \
					 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)

/*
 * The usages an image of AMDRDNA_VK_SAMPLES_MAX samples may have: drawn
 * into as a colour or a depth attachment, sampled (its samples fetched),
 * resolved from (a transfer source) and named as a transfer destination
 * (no copy into it is recorded).
 */
#define RESOURCE_SAMPLED_USAGES		(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | \
					 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | \
					 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT)

/*
 * A buffer view's offset alignment (minTexelBufferOffsetAlignment) and
 * its most texels (maxTexelBufferElements), as the device reports them.
 */
#define RESOURCE_TEXEL_OFFSET_ALIGN	16U
#define RESOURCE_TEXELS_MAX		(1U << 27)

/* The bytes of a texel of a two-plane image's luma plane and of its chroma plane. */
#define RESOURCE_LUMA_BYTES		1U
#define RESOURCE_CHROMA_BYTES		2U

/* The most bytes of one query's record, a 64-bit status and a 64-bit availability. */
#define RESOURCE_QUERY_RECORD_MAX	16U

/*
 * The bit a depth unit sets in each count of an occlusion query it
 * writes.
 *
 * Values referenced: Mesa 25.0.7 src/amd/vulkan/radv_query.c
 * (radv_GetQueryPoolResults: start and end valid with 1ull << 63).
 */
#define RESOURCE_QUERY_WRITTEN		(1ULL << 63)

/* The most bytes one vkGetQueryPoolResults answers with, and the zeros written at a time. */
#define RESOURCE_QUERY_REPLY_MAX	(AMDRDNA_VK_QUERIES_MAX * RESOURCE_QUERY_RECORD_MAX)
#define RESOURCE_QUERY_ZEROS		64U

static int resource_create_buffer(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int resource_create_buffer_view(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int resource_buffer_view_destroy(struct amdrdna_vk_session *session, void *payload);
static int resource_create_image(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int resource_image_supported(const VkImageCreateInfo *info);
static int resource_planar_supported(const VkImageCreateInfo *info);
static void resource_lay_out(struct amdrdna_vk_image *image);
static void resource_block(uint32_t texel_bytes, uint32_t samples, uint32_t *width, uint32_t *height);
static uint32_t resource_log2(uint32_t value);
static uint32_t resource_pitch(uint32_t width, uint32_t texel_bytes);
static uint32_t resource_luma_rows(const struct amdrdna_vk_image *image);
static uint32_t resource_level_side(uint32_t side, uint32_t level);
static int resource_layout(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int resource_create_view(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int resource_view_format(const struct amdrdna_vk_image *image, const VkImageViewCreateInfo *info);
static int resource_view_type(const struct amdrdna_vk_image *image, uint32_t type, uint32_t layers);
static uint32_t resource_swizzle(VkComponentSwizzle swizzle, uint32_t identity);
static int resource_create_sampler(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int resource_buffer_destroy(struct amdrdna_vk_session *session, void *payload);
static int resource_image_destroy(struct amdrdna_vk_session *session, void *payload);
static int resource_view_destroy(struct amdrdna_vk_session *session, void *payload);
static int resource_create_query_pool(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static int resource_query_results(struct amdrdna_vk_session *session, struct i915_wire_reader *reader, struct i915_wire_writer *reply);
static uint32_t resource_query_record(uint64_t value, uint32_t available, uint32_t flags, uint8_t *record);
static void resource_query_value(const struct amdrdna_vk_query_pool *pool, uint32_t query, uint64_t *value, uint32_t *available);
static int resource_query_pool_destroy(struct amdrdna_vk_session *session, void *payload);
static void resource_reply_zeros(struct i915_wire_writer *reply, uint64_t bytes);

/*
 * Runs a buffer, image, view, sampler or query pool command; *handled
 * stays clear for an opcode of another handler's.
 */
int
amdrdna_vk_resource_route(
	struct amdrdna_vk_session *session,
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
		error = amdrdna_vk_destroy_plain(session, reader, I915_VK_OBJ_BUFFER);
		break;
	case GPU_OP_CREATE_BUFFER_VIEW:
		error = resource_create_buffer_view(session, reader, reply);
		break;
	case GPU_OP_DESTROY_BUFFER_VIEW:
		error = amdrdna_vk_destroy_plain(session, reader, I915_VK_OBJ_BUFFER_VIEW);
		break;
	case GPU_OP_CREATE_IMAGE:
		error = resource_create_image(session, reader, reply);
		break;
	case GPU_OP_DESTROY_IMAGE:
		error = amdrdna_vk_destroy_plain(session, reader, I915_VK_OBJ_IMAGE);
		break;
	case GPU_OP_GET_IMAGE_SUBRESOURCE_LAYOUT:
		error = resource_layout(session, reader, reply);
		break;
	case GPU_OP_CREATE_IMAGE_VIEW:
		error = resource_create_view(session, reader, reply);
		break;
	case GPU_OP_DESTROY_IMAGE_VIEW:
		error = amdrdna_vk_destroy_plain(session, reader, I915_VK_OBJ_IMAGE_VIEW);
		break;
	case GPU_OP_CREATE_SAMPLER:
		error = resource_create_sampler(session, reader, reply);
		break;
	case GPU_OP_DESTROY_SAMPLER:
		error = amdrdna_vk_destroy_plain(session, reader, I915_VK_OBJ_SAMPLER);
		break;
	case GPU_OP_CREATE_QUERY_POOL:
		error = resource_create_query_pool(session, reader, reply);
		break;
	case GPU_OP_DESTROY_QUERY_POOL:
		error = amdrdna_vk_destroy_plain(session, reader, I915_VK_OBJ_QUERY_POOL);
		break;
	case GPU_OP_GET_QUERY_POOL_RESULTS:
		error = resource_query_results(session, reader, reply);
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
amdrdna_vk_format_colour(
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
 * Reports whether a format is one the colour unit draws into: a colour
 * format, or (in an image of one sample) a storage format.
 */
bool
amdrdna_vk_format_drawn(
	uint32_t format)
{
	uint32_t texel_bytes;

	/* A colour format, or a storage format. */
	if (amdrdna_vk_format_colour(format))
		return true;
	if (amdrdna_vk_format_storage(format, &texel_bytes))
		return true;

	/* Any other format is not drawn into. */
	return false;
}

/*
 * Reports whether a format is a depth format the executor draws into:
 * D16_UNORM, D32_SFLOAT, or D32_SFLOAT_S8_UINT with its stencil.
 */
bool
amdrdna_vk_format_depth(
	uint32_t format)
{
	/* A 16-bit normalized depth, or a 32-bit float depth without stencil or with it. */
	if (format == VK_FORMAT_D16_UNORM)
		return true;
	if (format == VK_FORMAT_D32_SFLOAT)
		return true;
	if (format == VK_FORMAT_D32_SFLOAT_S8_UINT)
		return true;

	/* Any other format is not one. */
	return false;
}

/* Reports whether a format is the depth format with stencil the executor draws into: D32_SFLOAT_S8_UINT. */
bool
amdrdna_vk_format_stencil(
	uint32_t format)
{
	/* A 32-bit float depth with an 8-bit stencil. */
	if (format == VK_FORMAT_D32_SFLOAT_S8_UINT)
		return true;

	/* Any other format has no stencil the depth unit writes. */
	return false;
}

/*
 * Reports whether a format is one of the formats of one or two bytes a
 * texel (an image of them is sampled and copied, and they are the formats
 * of a two-plane image's planes), and its bytes a texel.
 */
bool
amdrdna_vk_format_narrow(
	uint32_t format,
	uint32_t *texel_bytes)
{
	/* One 8-bit channel, or two. */
	switch (format) {
	case VK_FORMAT_R8_UNORM:
		*texel_bytes = 1U;
		return true;
	case VK_FORMAT_R8G8_UNORM:
		*texel_bytes = 2U;
		return true;
	default:
		break;
	}

	/* Any other format is not one. */
	return false;
}

/*
 * Reports whether a format is one of the formats only a storage image (or
 * a texel buffer) has, and its bytes a texel: the formats of four or
 * eight bytes or of sixteen Vulkan requires for storage images, but the
 * colour format R8G8B8A8_UNORM, which is a colour image's.
 *
 * Values referenced: the Vulkan 1.3 specification (Required Format
 * Support: the formats with VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, which
 * the shaders name without shaderStorageImageExtendedFormats).
 */
bool
amdrdna_vk_format_storage(
	uint32_t format,
	uint32_t *texel_bytes)
{
	/* Four bytes, eight, or sixteen a texel. */
	switch (format) {
	case VK_FORMAT_R8G8B8A8_SNORM:
	case VK_FORMAT_R8G8B8A8_UINT:
	case VK_FORMAT_R8G8B8A8_SINT:
	case VK_FORMAT_R32_UINT:
	case VK_FORMAT_R32_SINT:
	case VK_FORMAT_R32_SFLOAT:
		*texel_bytes = 4U;
		return true;
	case VK_FORMAT_R16G16B16A16_UINT:
	case VK_FORMAT_R16G16B16A16_SINT:
	case VK_FORMAT_R16G16B16A16_SFLOAT:
	case VK_FORMAT_R32G32_UINT:
	case VK_FORMAT_R32G32_SINT:
	case VK_FORMAT_R32G32_SFLOAT:
		*texel_bytes = 8U;
		return true;
	case VK_FORMAT_R32G32B32A32_UINT:
	case VK_FORMAT_R32G32B32A32_SINT:
	case VK_FORMAT_R32G32B32A32_SFLOAT:
		*texel_bytes = 16U;
		return true;
	default:
		break;
	}

	/* Any other format is not one. */
	return false;
}

/* Reports whether a format is the two-plane 4:2:0 format the executor lays out: NV12. */
bool
amdrdna_vk_format_planar(
	uint32_t format)
{
	/* An 8-bit luma plane and an 8-bit chroma pair plane at half the size. */
	if (format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
		return true;

	/* Any other format is not one. */
	return false;
}

/*
 * Reports whether an image is copied only by the command processor's
 * moves: a two-plane image, one of one or two bytes a texel, or one of a
 * storage format.  The executor's copy quads sample and draw the colour
 * formats' four-byte pixels only.
 */
bool
amdrdna_vk_image_moved(
	const struct amdrdna_vk_image *image)
{
	uint32_t texel_bytes;

	/* A two-plane image. */
	if (image->planar)
		return true;

	/* A colour image of another size of texel than four bytes (a depth image's copies are drawn: vk-execute.c). */
	if (!image->depth && image->texel_bytes != AMDRDNA_VK_PIXEL_BYTES)
		return true;

	/* An image of a storage format, whatever its size of texel. */
	if (amdrdna_vk_format_storage(image->format, &texel_bytes))
		return true;

	/* A four-byte image is drawn and sampled. */
	return false;
}

/*
 * Gives one level of a colour image as an image of its own: the level's
 * size and pitch, its offset in the memory, one level.  The levels lie in
 * GFX10's linear mip chain: the smallest first, each level's rows its
 * width's bytes rounded up to 256, its rows not padded.  Returns 0, or
 * EINVAL for a level the image does not have or a depth image.
 *
 * Procedure referenced: Mesa 25.0.7
 * src/amd/addrlib/src/gfx10/gfx10addrlib.cpp
 * (Gfx10Lib::HwlComputeSurfaceInfoLinear: the levels from the last to the
 * first, each at the sum of the ones before, its pitch the width aligned
 * to 256 bytes) and src/amd/addrlib/src/core/addrlib2.h (GetMipSize: a
 * side halved per level, at least 1).
 */
int
amdrdna_vk_image_level(
	const struct amdrdna_vk_image *image,
	uint32_t level,
	struct amdrdna_vk_image *part)
{
	uint64_t offset;
	uint32_t smaller;
	uint32_t width;
	uint32_t height;

	/* A level the linear colour image has (a two-plane image's are its planes; an image of samples has none linear). */
	if (image->depth || image->planar || image->samples > 1U || level >= image->levels)
		return EINVAL;

	/* The levels smaller than it lie before it. */
	offset = 0U;
	for (smaller = image->levels - 1U; smaller > level; smaller--) {
		width = resource_level_side(image->width, smaller);
		height = resource_level_side(image->height, smaller);
		offset += (uint64_t)resource_pitch(width, image->texel_bytes) * height;
	}

	/* The level: its size, its pitch, where it starts, one level. */
	*part = *image;
	part->width = resource_level_side(image->width, level);
	part->height = resource_level_side(image->height, level);
	part->pitch = resource_pitch(part->width, image->texel_bytes);
	part->bytes = (uint64_t)part->pitch * part->height;
	part->offset = image->offset + offset;
	part->levels = 1U;
	part->slices = 1U;
	part->slice_bytes = part->bytes;

	/* Succeeded: the level is an image of its own. */
	return 0;
}

/*
 * Gives one plane of a two-plane image as an image of its own, in the
 * plane's format: plane 0 the luma (R8_UNORM, the image's size) from the
 * image's start, plane 1 the chroma pairs (R8G8_UNORM, half the width and
 * half the height) from chroma_offset, both one level at the image's
 * pitch.  Returns 0, or EINVAL for a plane the image does not have.
 */
int
amdrdna_vk_image_plane(
	const struct amdrdna_vk_image *image,
	uint32_t plane,
	struct amdrdna_vk_image *part)
{
	/* One of the two planes of a two-plane image. */
	if (!image->planar || plane > 1U)
		return EINVAL;

	/* The luma plane: the image's size, one byte a texel, up to the chroma plane. */
	*part = *image;
	part->planar = false;
	part->levels = 1U;
	part->chroma_offset = 0U;
	part->align = AMDRDNA_VK_RESOURCE_ALIGN;
	part->format = VK_FORMAT_R8_UNORM;
	part->texel_bytes = RESOURCE_LUMA_BYTES;
	part->bytes = (uint64_t)image->pitch * resource_luma_rows(image);

	/* The chroma plane: half the size, two bytes a texel, from its offset to the image's end. */
	if (plane == 1U) {
		part->format = VK_FORMAT_R8G8_UNORM;
		part->texel_bytes = RESOURCE_CHROMA_BYTES;
		part->width = image->width / 2U;
		part->height = image->height / 2U;
		part->bytes = image->bytes - image->chroma_offset;
		part->offset = image->offset + image->chroma_offset;
	}

	/* Succeeded: the plane is an image of its own. */
	return 0;
}

/*
 * Gives the part of an image a copy or a layout names: a plane of a
 * two-plane image by its aspect (VK_IMAGE_ASPECT_PLANE_0_BIT or _1_BIT,
 * level 0), else a level of a colour image (its aspect is the image's
 * one).  Returns 0, or EINVAL for a part the image does not have.
 */
int
amdrdna_vk_image_part(
	const struct amdrdna_vk_image *image,
	uint32_t aspect,
	uint32_t level,
	struct amdrdna_vk_image *part)
{
	int error;

	/* A level of an image of one plane. */
	if (!image->planar) {
		error = amdrdna_vk_image_level(image, level, part);
		if (error != 0)
			return error;

		/* Succeeded: the level. */
		return 0;
	}

	/* A plane of a two-plane image, named by its aspect alone, at its one level. */
	if (level != 0U)
		return EINVAL;
	if (aspect == VK_IMAGE_ASPECT_PLANE_0_BIT) {
		error = amdrdna_vk_image_plane(image, 0U, part);
	} else if (aspect == VK_IMAGE_ASPECT_PLANE_1_BIT) {
		error = amdrdna_vk_image_plane(image, 1U, part);
	} else {
		error = EINVAL;
	}
	if (error != 0)
		return error;

	/* Succeeded: the plane. */
	return 0;
}

/*
 * Moves a part of an image (a level of slice 0, from
 * amdrdna_vk_image_level() or amdrdna_vk_image_part()) to the same level
 * of another slice: a layer of a 2D image, or a depth slice of a 3D one
 * (one the level has: a 3D image's level is half as deep per level).
 * Returns 0, or EINVAL for a slice the image's level does not have.
 *
 * Procedure referenced: Mesa 25.0.7
 * src/amd/addrlib/src/gfx10/gfx10addrlib.cpp
 * (Gfx10Lib::HwlComputeSurfaceInfoLinear: sliceSize the bytes of every
 * level, surfSize sliceSize times the slices).
 */
int
amdrdna_vk_image_slice(
	const struct amdrdna_vk_image *image,
	uint32_t level,
	uint32_t slice,
	struct amdrdna_vk_image *part)
{
	uint32_t slices;

	/* A slice the level has. */
	slices = image->slices;
	if (image->type == VK_IMAGE_TYPE_3D)
		slices = resource_level_side(image->slices, level);
	if (slice >= slices)
		return EINVAL;

	/* Succeeded: as many slices on. */
	part->offset += (uint64_t)slice * image->slice_bytes;
	return 0;
}

/*
 * Marks queries of a pool unavailable and not ready (a vkCmdResetQueryPool
 * as its submission runs).  The range was checked against the pool when
 * it was recorded.
 */
void
amdrdna_vk_query_reset(
	struct amdrdna_vk_query_pool *pool,
	uint32_t first,
	uint32_t count)
{
	uint32_t query;

	/* Each query of the range: unavailable, its status not ready. */
	for (query = first; query < first + count; query++) {
		pool->available[query] = 0U;
		pool->status[query] = 0;
	}
}

/* vkCreateBuffer: [device][present][VkBufferCreateInfo] and the create's tail -> the create's reply. */
static int
resource_create_buffer(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkBufferCreateInfo info;
	struct amdrdna_vk_buffer *buffer;
	uint64_t identity;
	int error;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkBufferCreateInfo(reader, &session->arena, &info);
	identity = amdrdna_vk_create_tail(reader);
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
	amdrdna_vk_create_reply(session, reply, I915_VK_OBJ_BUFFER, identity, buffer, resource_buffer_destroy, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * vkCreateBufferView: [device][present][VkBufferViewCreateInfo] and the
 * create's tail -> the create's reply.  A view of a known buffer in a
 * format the buffer descriptors fetch, its offset on the texel offset
 * alignment (16 bytes, as reported) and its range whole texels within the
 * buffer (VK_WHOLE_SIZE the rest of it, rounded down to whole texels), at
 * most maxTexelBufferElements (2^27) of them.
 */
static int
resource_create_buffer_view(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkBufferViewCreateInfo info;
	struct amdrdna_vk_object *buffer;
	struct amdrdna_vk_buffer_view *view;
	const struct amdrdna_vk_buffer *payload;
	uint64_t identity;
	uint64_t range;
	uint32_t code;
	uint32_t components;
	uint32_t bytes;
	bool fetched;
	int error;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkBufferViewCreateInfo(reader, &session->arena, &info);
	identity = amdrdna_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A known buffer, a format fetched, an aligned offset, whole texels within the buffer. */
	error = 0;
	range = 0U;
	bytes = 0U;
	buffer = amdrdna_vk_object_find(session, I915_VK_OBJ_BUFFER, (uint64_t)(uintptr_t)info.buffer);
	fetched = amdrdna_vk_texel_format(info.format, &code, &components, &bytes);
	if (buffer == NULL) {
		error = EINVAL;
	} else if (!fetched) {
		kern_logf("amdrdna: vk: vkCreateBufferView refused: format %u\n", (unsigned)info.format);
		error = ENOTSUP;
	} else if ((info.offset % RESOURCE_TEXEL_OFFSET_ALIGN) != 0U) {
		error = EINVAL;
	} else {
		payload = buffer->payload;
		range = info.range;
		if (info.offset > payload->size) {
			error = EINVAL;
		} else if (range == VK_WHOLE_SIZE) {
			range = (payload->size - info.offset) / bytes * bytes;
		} else if (range > payload->size - info.offset || (range % bytes) != 0U) {
			error = EINVAL;
		}
		if (error == 0 && range / bytes > RESOURCE_TEXELS_MAX)
			error = EINVAL;
	}

	/* The view holds its buffer. */
	view = NULL;
	if (error == 0)
		error = amdrdna_vk_object_retain(buffer);
	if (error == 0) {
		view = kern_calloc(1U, sizeof(*view));
		if (view == NULL)
			(void)amdrdna_vk_object_release(buffer);
	}
	if (view != NULL) {
		view->buffer = buffer;
		view->format = info.format;
		view->offset = info.offset;
		view->range = range;
	}

	/* Publishes the view and answers. */
	amdrdna_vk_create_reply(session, reply, I915_VK_OBJ_BUFFER_VIEW, identity, view, resource_buffer_view_destroy, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/* Destroys a buffer view nothing holds: it lets its buffer go. */
static int
resource_buffer_view_destroy(
	struct amdrdna_vk_session *session,
	void *payload)
{
	struct amdrdna_vk_buffer_view *view;
	int error;

	UNUSED_PARAMETER(session);

	view = payload;

	/* The buffer may go with it when nothing else holds the buffer. */
	error = amdrdna_vk_object_release(view->buffer);
	kern_free(view);

	/* Reports a buffer whose destruction failed. */
	if (error != 0)
		return error;

	/* Succeeded: the view is gone. */
	return 0;
}

/* vkCreateImage: [device][present][VkImageCreateInfo] and the create's tail -> the create's reply. */
static int
resource_create_image(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageCreateInfo info;
	struct amdrdna_vk_image *image;
	uint64_t identity;
	int supported;
	int error;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkImageCreateInfo(reader, &session->arena, &info);
	identity = amdrdna_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Refuses by name an image the executor does not lay out. */
	error = 0;
	supported = resource_image_supported(&info);
	if (supported == 0) {
		kern_logf("amdrdna: vk: vkCreateImage refused: type %u format %u %ux%ux%u levels %u layers %u samples %u usage 0x%x flags 0x%x\n",
			  (unsigned)info.imageType,
			  (unsigned)info.format,
			  info.extent.width,
			  info.extent.height,
			  info.extent.depth,
			  info.mipLevels,
			  info.arrayLayers,
			  (unsigned)info.samples,
			  (unsigned)info.usage,
			  (unsigned)info.flags);
		error = ENOTSUP;
	}

	/* Lays the image out: levels or planes pitch linear, or the depth unit's blocks. */
	image = NULL;
	if (error == 0) {
		image = kern_calloc(1U, sizeof(*image));
		if (image != NULL) {
			image->type = info.imageType;
			image->format = info.format;
			image->width = info.extent.width;
			image->height = info.extent.height;
			image->samples = (uint32_t)info.samples;
			image->slices = info.arrayLayers;
			if (info.imageType == VK_IMAGE_TYPE_3D)
				image->slices = info.extent.depth;
			image->cube = (info.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0U;
			image->slice_views = info.imageType == VK_IMAGE_TYPE_3D && (info.flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT) != 0U;
			image->usage = info.usage;
			image->levels = info.mipLevels;
			resource_lay_out(image);
		}
	}

	/* An image past the largest resource is refused. */
	if (image != NULL && image->bytes > AMDRDNA_VK_RESOURCE_MAX) {
		kern_free(image);
		image = NULL;
		error = ENOTSUP;
	}

	/* Publishes the image and answers. */
	amdrdna_vk_create_reply(session, reply, I915_VK_OBJ_IMAGE, identity, image, resource_image_destroy, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * Reports whether an image is one the executor lays out: of one sample
 * (or AMDRDNA_VK_SAMPLES_MAX: 2D, one level, no cube, a colour format of
 * four bytes or a depth format, drawn into, sampled and resolved),
 * within the largest side, with at least one level and levels down to one
 * pixel at most; a colour format, a format of one or two bytes a texel
 * sampled and copied only, or a storage format a storage image copied
 * only, 2D of up to AMDRDNA_VK_LAYERS_MAX layers (six
 * or more square ones for a cube) or 3D up to AMDRDNA_VK_DEPTH_MAX a
 * side; a depth format 2D of one level and one layer, drawn into,
 * sampled, copied and blitted; or the two-plane format as
 * resource_planar_supported says.
 *
 * Values referenced: the Vulkan 1.3 specification (VkImageCreateInfo: a
 * cube compatible image is 2D, square, of at least six layers; a 3D image
 * has one layer).
 */
static int
resource_image_supported(
	const VkImageCreateInfo *info)
{
	uint32_t largest;
	uint32_t texel_bytes;
	bool colour;
	bool depth;
	bool narrow;
	bool planar;
	bool cube;

	/* The two-plane format has rules of its own. */
	planar = amdrdna_vk_format_planar(info->format);
	if (planar) {
		if (resource_planar_supported(info) == 0)
			return 0;
		return 1;
	}

	/* A 2D or 3D image of one sample, with levels down to one pixel at most. */
	if (info->imageType != VK_IMAGE_TYPE_2D && info->imageType != VK_IMAGE_TYPE_3D)
		return 0;
	if (info->mipLevels == 0U || info->mipLevels > AMDRDNA_VK_LEVELS_MAX)
		return 0;
	largest = info->extent.width;
	if (info->extent.height > largest)
		largest = info->extent.height;
	if (info->imageType == VK_IMAGE_TYPE_3D && info->extent.depth > largest)
		largest = info->extent.depth;
	if ((largest >> (info->mipLevels - 1U)) == 0U)
		return 0;
	if (info->samples != VK_SAMPLE_COUNT_1_BIT && info->samples != (VkSampleCountFlagBits)AMDRDNA_VK_SAMPLES_MAX)
		return 0;

	/* An image of samples: 2D of one level, not a cube, drawn into, sampled and resolved; a colour or depth format of its own. */
	if (info->samples != VK_SAMPLE_COUNT_1_BIT) {
		if (info->imageType != VK_IMAGE_TYPE_2D || info->mipLevels != 1U)
			return 0;
		if ((info->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0U)
			return 0;
		if ((info->usage & ~(uint32_t)RESOURCE_SAMPLED_USAGES) != 0U)
			return 0;
		colour = amdrdna_vk_format_colour(info->format);
		depth = amdrdna_vk_format_depth(info->format);
		if (!colour && !depth)
			return 0;
	}

	/* Of a size the colour target and the texture header take. */
	if (info->extent.width == 0U || info->extent.width > AMDRDNA_VK_IMAGE_SIDE_MAX)
		return 0;
	if (info->extent.height == 0U || info->extent.height > AMDRDNA_VK_IMAGE_SIDE_MAX)
		return 0;

	/* A 2D image of its layers (a cube's square, six a cube), or a 3D image of one layer and its depth. */
	cube = (info->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0U;
	if (info->imageType == VK_IMAGE_TYPE_2D) {
		if (info->extent.depth != 1U || info->arrayLayers == 0U || info->arrayLayers > AMDRDNA_VK_LAYERS_MAX)
			return 0;
		if (cube && (info->extent.width != info->extent.height || info->arrayLayers < 6U))
			return 0;
	} else {
		if (info->arrayLayers != 1U || cube)
			return 0;
		if (info->extent.depth == 0U || info->extent.depth > AMDRDNA_VK_DEPTH_MAX)
			return 0;
		if (info->extent.width > AMDRDNA_VK_DEPTH_MAX || info->extent.height > AMDRDNA_VK_DEPTH_MAX)
			return 0;
	}

	/* A depth format: 2D, one level and one layer, a depth attachment, sampled and copied. */
	depth = amdrdna_vk_format_depth(info->format);
	if (depth) {
		if (info->imageType != VK_IMAGE_TYPE_2D || info->mipLevels != 1U || info->arrayLayers != 1U || cube)
			return 0;
		if ((info->usage & ~(uint32_t)RESOURCE_DEPTH_USAGES) != 0U)
			return 0;
		return 1;
	}

	/* A storage format: only a storage image, and copied. */
	if (amdrdna_vk_format_storage(info->format, &texel_bytes)) {
		if ((info->usage & ~(uint32_t)RESOURCE_STORAGE_USAGES) != 0U)
			return 0;
		return 1;
	}

	/* A format of one or two bytes a texel: only sampled and copied. */
	narrow = amdrdna_vk_format_narrow(info->format, &texel_bytes);
	if (narrow) {
		if ((info->usage & ~(uint32_t)RESOURCE_NARROW_USAGES) != 0U)
			return 0;
		return 1;
	}

	/* Otherwise one of the colour formats. */
	colour = amdrdna_vk_format_colour(info->format);
	if (!colour)
		return 0;

	/* Succeeded: the image is laid out pitch linear. */
	return 1;
}

/*
 * Reports whether a two-plane image is one the executor lays out: 2D, one
 * level, one layer, one sample, of an even width and height within the
 * largest side (its chroma plane is half of each); with only the plane
 * views' create flags; copied, its planes sampled (only with
 * VK_IMAGE_CREATE_EXTENDED_USAGE_BIT, the format itself is not sampled),
 * or written and referenced by a video decode.
 *
 * Values referenced: the Vulkan 1.3 specification (VkImageCreateInfo: a
 * format with 4:2:0 chroma subsampling has an even width and height; a
 * usage the format does not support needs EXTENDED_USAGE).
 */
static int
resource_planar_supported(
	const VkImageCreateInfo *info)
{
	/* A flat image of one level, layer and sample. */
	if (info->imageType != VK_IMAGE_TYPE_2D)
		return 0;
	if (info->mipLevels != 1U)
		return 0;
	if (info->arrayLayers != 1U)
		return 0;
	if (info->samples != VK_SAMPLE_COUNT_1_BIT)
		return 0;
	if (info->extent.depth != 1U)
		return 0;

	/* An even size the descriptors of both planes take. */
	if (info->extent.width == 0U || info->extent.width > AMDRDNA_VK_IMAGE_SIDE_MAX)
		return 0;
	if (info->extent.height == 0U || info->extent.height > AMDRDNA_VK_IMAGE_SIDE_MAX)
		return 0;
	if ((info->extent.width % 2U) != 0U || (info->extent.height % 2U) != 0U)
		return 0;

	/* Only the plane views' flags. */
	if ((info->flags & ~(uint32_t)RESOURCE_PLANAR_FLAGS) != 0U)
		return 0;

	/* Some usage, each one the planes have. */
	if (info->usage == 0U || (info->usage & ~(uint32_t)RESOURCE_PLANAR_USAGES) != 0U)
		return 0;

	/* Sampled only through its planes' views. */
	if ((info->usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0U && (info->flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) == 0U)
		return 0;

	/* Succeeded: the image is laid out as two planes. */
	return 1;
}

/*
 * Lays an image of a format, size, usage and levels out: a depth image
 * whole blocks of the depth unit's swizzle (its pitch the block-aligned
 * width's bytes), and its stencil plane's blocks after them when it has
 * stencil; a two-plane image its luma plane, then on
 * AMDRDNA_VK_PLANE_ALIGN its chroma plane, both at the luma width's pitch
 * (a video engine's picture with its rows in whole groups of
 * AMDRDNA_VK_VIDEO_ROWS); a colour image, or one of one or two bytes a
 * texel, its levels in the linear mip chain (its pitch level 0's) for
 * each of its slices, one after the other.  Sets the image's texel size,
 * pitch, slice's bytes, bytes and memory boundary.
 */
static void
resource_lay_out(
	struct amdrdna_vk_image *image)
{
	struct amdrdna_vk_image first;
	uint32_t block_width;
	uint32_t block_height;
	uint32_t width;
	uint32_t height;
	uint32_t texel_bytes;
	bool narrow;
	int error;

	/* What the format is: depth (with stencil or not), two planes, one or two bytes a texel, or four; one slice unless said. */
	if (image->slices == 0U)
		image->slices = 1U;
	image->depth = amdrdna_vk_format_depth(image->format);
	image->stencil = amdrdna_vk_format_stencil(image->format);
	image->planar = amdrdna_vk_format_planar(image->format);
	image->texel_bytes = AMDRDNA_VK_PIXEL_BYTES;
	image->align = AMDRDNA_VK_RESOURCE_ALIGN;
	image->chroma_offset = 0U;
	image->stencil_offset = 0U;
	texel_bytes = 0U;
	narrow = amdrdna_vk_format_narrow(image->format, &texel_bytes);
	if (narrow)
		image->texel_bytes = texel_bytes;
	if (amdrdna_vk_format_storage(image->format, &texel_bytes))
		image->texel_bytes = texel_bytes;

	if (image->samples == 0U)
		image->samples = 1U;

	/*
	 * A depth image: the width and the height rounded up to the blocks
	 * (of one sample 128 pixels a side of four bytes, or 256 wide and 128
	 * tall of two; a quarter of the pixels of four samples), every sample
	 * of a pixel in its block.
	 */
	if (image->depth) {
		if (image->format == VK_FORMAT_D16_UNORM)
			image->texel_bytes = 2U;
		resource_block(image->texel_bytes, image->samples, &block_width, &block_height);
		width = (image->width + block_width - 1U) & ~(block_width - 1U);
		height = (image->height + block_height - 1U) & ~(block_height - 1U);
		image->pitch = width * image->texel_bytes * image->samples;
		image->bytes = (uint64_t)image->pitch * height;
		image->align = AMDRDNA_VK_DEPTH_ALIGN;
		image->slice_bytes = image->bytes;
		if (!image->stencil)
			return;

		/* Its stencil plane after it (the depth's blocks end on a block): one byte a sample in its blocks. */
		resource_block(1U, image->samples, &block_width, &block_height);
		width = (image->width + block_width - 1U) & ~(block_width - 1U);
		height = (image->height + block_height - 1U) & ~(block_height - 1U);
		image->stencil_offset = image->bytes;
		image->bytes += (uint64_t)width * height * image->samples;
		image->slice_bytes = image->bytes;
		return;
	}

	/*
	 * A colour image of samples: one level of each layer in the render
	 * swizzle's 64 KiB blocks, every sample of a pixel in its block; the
	 * layers one after the other.
	 */
	if (image->samples > 1U) {
		resource_block(image->texel_bytes, image->samples, &block_width, &block_height);
		width = (image->width + block_width - 1U) & ~(block_width - 1U);
		height = (image->height + block_height - 1U) & ~(block_height - 1U);
		image->pitch = width * image->texel_bytes * image->samples;
		image->align = AMDRDNA_VK_BLOCK_BYTES;
		image->slice_bytes = (uint64_t)image->pitch * height;
		image->bytes = image->slice_bytes * image->slices;
		return;
	}

	/*
	 * A two-plane image: the luma plane's rows, then the chroma plane on
	 * a page after them with half as many rows; the chroma plane's
	 * texels are pairs of bytes over half the width, so both planes have
	 * the luma width's pitch.
	 */
	if (image->planar) {
		image->texel_bytes = RESOURCE_LUMA_BYTES;
		image->align = AMDRDNA_VK_PLANE_ALIGN;
		image->pitch = resource_pitch(image->width, RESOURCE_LUMA_BYTES);
		image->chroma_offset = (uint64_t)image->pitch * resource_luma_rows(image);
		image->chroma_offset = (image->chroma_offset + AMDRDNA_VK_PLANE_ALIGN - 1U) & ~(uint64_t)(AMDRDNA_VK_PLANE_ALIGN - 1U);
		image->bytes = image->chroma_offset + (uint64_t)image->pitch * (resource_luma_rows(image) / 2U);
		image->slice_bytes = image->bytes;
		return;
	}

	/* A colour image: level 0 lies after every smaller level, and ends the chain of a slice. */
	image->pitch = resource_pitch(image->width, image->texel_bytes);
	image->slice_bytes = (uint64_t)image->pitch * image->height;
	error = amdrdna_vk_image_level(image, 0U, &first);
	if (error == 0)
		image->slice_bytes = first.offset - image->offset + first.bytes;

	/* Succeeded: every slice's chain, one after the other. */
	image->bytes = image->slice_bytes * image->slices;
}

/*
 * Gives the pixels wide and tall of a 64 KiB block of a thin swizzle of
 * texel_bytes a sample and samples a pixel: the block's elements (64 KiB
 * over the bytes of a pixel's samples) split between the width and the
 * height, the width taking the odd power when the samples are a square's
 * power (1 or 4).
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/addrlib/src/core/addrlib2.cpp
 * (Lib::ComputeThinBlockDimension: log2 of the elements the block size's
 * less those of the element's bytes and of the samples; the width first
 * for 1xAA and 4xAA, (n + 1) / 2 of the n powers).
 */
static void
resource_block(
	uint32_t texel_bytes,
	uint32_t samples,
	uint32_t *width,
	uint32_t *height)
{
	uint32_t elements;
	uint32_t wide;

	/* The block's elements, the width's share (one more for a square's power of samples). */
	elements = resource_log2(AMDRDNA_VK_BLOCK_BYTES) - resource_log2(texel_bytes) - resource_log2(samples);
	wide = elements / 2U;
	if ((resource_log2(samples) & 1U) == 0U)
		wide = (elements + 1U) / 2U;

	/* Succeeded: the block's sides. */
	*width = 1U << wide;
	*height = 1U << (elements - wide);
}

/* Reports the power of two a power of two is. */
static uint32_t
resource_log2(
	uint32_t value)
{
	uint32_t power;

	/* Halved to 1. */
	power = 0U;
	while (value > 1U) {
		value >>= 1;
		power++;
	}

	/* Succeeded: the power. */
	return power;
}

/* Reports the row pitch of a linear image of a width and bytes a texel: the width's bytes rounded up to 256. */
static uint32_t
resource_pitch(
	uint32_t width,
	uint32_t texel_bytes)
{
	uint32_t bytes;

	/* The row's bytes, rounded up. */
	bytes = width * texel_bytes;

	/* Succeeded: the pitch. */
	return (bytes + AMDRDNA_VK_PITCH_ALIGN - 1U) & ~(AMDRDNA_VK_PITCH_ALIGN - 1U);
}

/*
 * Reports the rows a two-plane image's luma plane takes: its height, or
 * for a video engine's picture its height rounded up to whole groups of
 * AMDRDNA_VK_VIDEO_ROWS.  The chroma plane takes half as many.
 */
static uint32_t
resource_luma_rows(
	const struct amdrdna_vk_image *image)
{
	/* An image no video engine writes or reads: its own rows. */
	if ((image->usage & RESOURCE_VIDEO_USAGES) == 0U)
		return image->height;

	/* Succeeded: the rows a video engine may write. */
	return (image->height + AMDRDNA_VK_VIDEO_ROWS - 1U) & ~(AMDRDNA_VK_VIDEO_ROWS - 1U);
}

/* Reports a side of a level: the side halved per level, at least one pixel. */
static uint32_t
resource_level_side(
	uint32_t side,
	uint32_t level)
{
	uint32_t halved;

	/* Halved per level. */
	halved = side >> level;
	if (halved == 0U)
		return 1U;

	/* Succeeded: the level's side. */
	return halved;
}

/*
 * vkGetImageSubresourceLayout: [device][image][present]
 * [VkImageSubresource][present] -> [present][VkSubresourceLayout].  A level
 * of a colour image starts where the mip chain of its layer has it, with
 * its own pitch, the layers (or a 3D image's depth slices) a slice's bytes
 * apart; a plane of a two-plane image (its aspect
 * VK_IMAGE_ASPECT_PLANE_0_BIT or _1_BIT) where the image has it, at the
 * image's pitch; an unknown image or subresource, or a depth image,
 * reports an empty layout.
 */
static int
resource_layout(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageSubresource subresource;
	VkSubresourceLayout layout;
	const struct amdrdna_vk_image *image;
	struct amdrdna_vk_image part;
	uint64_t identity;
	uint64_t present;
	int level;

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

	/* A level or a plane of a layer: where it starts in the image, its rows pitch bytes apart, the slices a slice's bytes apart. */
	kern_memset(&layout, 0, sizeof(layout));
	image = amdrdna_vk_payload(session, I915_VK_OBJ_IMAGE, identity);
	level = EINVAL;
	if (image != NULL)
		level = amdrdna_vk_image_part(image, subresource.aspectMask, subresource.mipLevel, &part);
	if (level == 0 && image->type != VK_IMAGE_TYPE_3D)
		level = amdrdna_vk_image_slice(image, subresource.mipLevel, subresource.arrayLayer, &part);
	if (level == 0 && image->type == VK_IMAGE_TYPE_3D && subresource.arrayLayer != 0U)
		level = EINVAL;
	if (level == 0) {
		layout.offset = part.offset - image->offset;
		layout.size = part.bytes;
		layout.rowPitch = part.pitch;
		layout.arrayPitch = image->slice_bytes;
		layout.depthPitch = image->slice_bytes;
	}

	/* The layout behind its presence word. */
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkSubresourceLayout(reply, &layout);

	/* Succeeded: the reply carries the layout. */
	return 0;
}

/*
 * vkCreateImageView: [device][present][VkImageViewCreateInfo] and the
 * create's tail -> the create's reply.  A view of levels and layers of a
 * known image, in a format resource_view_format takes for it, of a type
 * resource_view_type takes for them.
 */
static int
resource_create_view(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageViewCreateInfo info;
	const struct amdrdna_vk_image *payload;
	struct amdrdna_vk_object *image;
	struct amdrdna_vk_view *view;
	uint64_t identity;
	uint32_t levels;
	uint32_t count;
	uint32_t layers;
	uint32_t slices;
	int format;
	int error;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkImageViewCreateInfo(reader, &session->arena, &info);
	identity = amdrdna_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A view of a known image, of levels and layers it has, in a format it may be read as, of a type it may be read as. */
	error = 0;
	image = amdrdna_vk_object_find(session, I915_VK_OBJ_IMAGE, (uint64_t)(uintptr_t)info.image);
	payload = NULL;
	levels = 0U;
	format = ENOTSUP;
	if (image != NULL) {
		payload = image->payload;
		levels = payload->levels;
		format = resource_view_format(payload, &info);
	}
	count = info.subresourceRange.levelCount;
	if (count == VK_REMAINING_MIP_LEVELS && info.subresourceRange.baseMipLevel < levels)
		count = levels - info.subresourceRange.baseMipLevel;
	/*
	 * The layers: a 2D image's from the first; a 3D view of a 3D image its
	 * first; a 2D or 2D array view of a 3D image's slices (one level)
	 * those of the level from the first, as the image's layers.
	 */
	layers = 0U;
	slices = 0U;
	if (payload != NULL) {
		layers = info.subresourceRange.layerCount;
		slices = payload->slices;
		if (payload->type == VK_IMAGE_TYPE_3D && info.viewType != VK_IMAGE_VIEW_TYPE_3D)
			slices = resource_level_side(payload->slices, info.subresourceRange.baseMipLevel);
		if (payload->type == VK_IMAGE_TYPE_3D && info.viewType == VK_IMAGE_VIEW_TYPE_3D)
			layers = 1U;
		else if (layers == VK_REMAINING_ARRAY_LAYERS && info.subresourceRange.baseArrayLayer < slices)
			layers = slices - info.subresourceRange.baseArrayLayer;
	}
	if (image == NULL) {
		error = EINVAL;
	} else if (info.subresourceRange.baseMipLevel >= levels || count == 0U || count > levels - info.subresourceRange.baseMipLevel) {
		error = EINVAL;
	} else if ((payload->type != VK_IMAGE_TYPE_3D || info.viewType != VK_IMAGE_VIEW_TYPE_3D) &&
		   (info.subresourceRange.baseArrayLayer >= slices || layers == 0U || layers > slices - info.subresourceRange.baseArrayLayer)) {
		error = EINVAL;
	} else if (payload->type == VK_IMAGE_TYPE_3D && info.viewType == VK_IMAGE_VIEW_TYPE_3D && info.subresourceRange.baseArrayLayer != 0U) {
		error = EINVAL;
	} else if (payload->type == VK_IMAGE_TYPE_3D && info.viewType != VK_IMAGE_VIEW_TYPE_3D && count != 1U) {
		error = EINVAL;
	} else if (format != 0) {
		error = format;
	} else {
		error = resource_view_type(payload, info.viewType, layers);
	}

	/* The view holds its image. */
	view = NULL;
	if (error == 0)
		error = amdrdna_vk_object_retain(image);
	if (error == 0) {
		view = kern_calloc(1U, sizeof(*view));
		if (view == NULL)
			(void)amdrdna_vk_object_release(image);
	}

	/* The image, the type, the format, where each channel comes from, the levels and the layers. */
	if (view != NULL) {
		view->image = image;
		view->type = info.viewType;
		view->format = info.format;
		view->aspect = info.subresourceRange.aspectMask;
		view->swizzle[0] = resource_swizzle(info.components.r, VK_COMPONENT_SWIZZLE_R);
		view->swizzle[1] = resource_swizzle(info.components.g, VK_COMPONENT_SWIZZLE_G);
		view->swizzle[2] = resource_swizzle(info.components.b, VK_COMPONENT_SWIZZLE_B);
		view->swizzle[3] = resource_swizzle(info.components.a, VK_COMPONENT_SWIZZLE_A);
		view->base_level = info.subresourceRange.baseMipLevel;
		view->level_count = count;
		view->base_layer = info.subresourceRange.baseArrayLayer;
		view->layer_count = layers;
	}

	/* Publishes the view and answers. */
	amdrdna_vk_create_reply(session, reply, I915_VK_OBJ_IMAGE_VIEW, identity, view, resource_view_destroy, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/*
 * Reports whether a view of a type may read layers of an image: a 2D view
 * one layer of a 2D image, a 2D array view layers of it, a cube view six
 * layers of a cube compatible image, a 3D view a 3D image; a 2D view of
 * one slice or a 2D array view of slices of a 3D image made 2D array
 * compatible (drawn into, as VK_KHR_maintenance1 has it).  Arrays of
 * cubes (imageCubeArray is not reported) and 1D views are not read.
 * Returns 0, or ENOTSUP.
 */
static int
resource_view_type(
	const struct amdrdna_vk_image *image,
	uint32_t type,
	uint32_t layers)
{
	/* Each type of view and the image it reads. */
	switch (type) {
	case VK_IMAGE_VIEW_TYPE_2D:
		if ((image->type != VK_IMAGE_TYPE_2D && !image->slice_views) || layers != 1U)
			return ENOTSUP;
		break;
	case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
		if (image->type != VK_IMAGE_TYPE_2D && !image->slice_views)
			return ENOTSUP;
		break;
	case VK_IMAGE_VIEW_TYPE_CUBE:
		if (image->type != VK_IMAGE_TYPE_2D || !image->cube || layers != 6U)
			return ENOTSUP;
		break;
	case VK_IMAGE_VIEW_TYPE_3D:
		if (image->type != VK_IMAGE_TYPE_3D)
			return ENOTSUP;
		break;
	default:
		return ENOTSUP;
	}

	/* Succeeded: the view reads the image so. */
	return 0;
}

/*
 * Reports whether a view may read an image in a format: a plane of a
 * two-plane image only in the plane's format (R8_UNORM for the luma plane,
 * R8G8_UNORM for the chroma plane), named by its aspect alone; a depth
 * image only in its own format, of aspects it has (its depth, its stencil
 * or both); an image of a storage format only in a storage format of the
 * same bytes; an image of one or two bytes a texel only in a format of the
 * same bytes; another image in a colour format.  Returns 0, or ENOTSUP.
 *
 * Values referenced: the Vulkan 1.3 specification (Format Compatibility
 * Classes: each depth/stencil format is compatible only with itself;
 * VkImageViewCreateInfo: the aspects a view of a depth/stencil image may
 * name).
 */
static int
resource_view_format(
	const struct amdrdna_vk_image *image,
	const VkImageViewCreateInfo *info)
{
	uint32_t texel_bytes;
	uint32_t aspect;
	bool narrow;
	bool colour;

	/*
	 * A plane of a two-plane image, in that plane's format; or a video
	 * engine's picture whole, in its own format (the picture a decode
	 * names, which nothing samples: a descriptor of it is refused).
	 */
	aspect = info->subresourceRange.aspectMask;
	if (image->planar) {
		if (aspect == VK_IMAGE_ASPECT_PLANE_0_BIT && info->format == VK_FORMAT_R8_UNORM)
			return 0;
		if (aspect == VK_IMAGE_ASPECT_PLANE_1_BIT && info->format == VK_FORMAT_R8G8_UNORM)
			return 0;
		if (aspect == VK_IMAGE_ASPECT_COLOR_BIT && info->format == image->format && (image->usage & RESOURCE_VIDEO_USAGES) != 0U)
			return 0;
		return ENOTSUP;
	}

	/* A depth image, in its format, its depth or its stencil (that a format with stencil has), or both. */
	if (image->depth) {
		if (info->format != image->format)
			return ENOTSUP;
		if (aspect == 0U || (aspect & ~(uint32_t)(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0U)
			return ENOTSUP;
		if ((aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0U && !image->stencil)
			return ENOTSUP;
		return 0;
	}

	/* An image of a storage format, in a storage format of as many bytes. */
	texel_bytes = 0U;
	if (amdrdna_vk_format_storage(image->format, &texel_bytes)) {
		if (!amdrdna_vk_format_storage(info->format, &texel_bytes) || texel_bytes != image->texel_bytes)
			return ENOTSUP;
		return 0;
	}

	/* An image of one or two bytes a texel, in a format of as many bytes. */
	narrow = amdrdna_vk_format_narrow(info->format, &texel_bytes);
	if (!image->depth && image->texel_bytes != AMDRDNA_VK_PIXEL_BYTES) {
		if (!narrow || texel_bytes != image->texel_bytes)
			return ENOTSUP;
		return 0;
	}

	/* Another image, in a colour format. */
	colour = amdrdna_vk_format_colour(info->format);
	if (!colour)
		return ENOTSUP;

	/* Succeeded: the view reads the image in its format. */
	return 0;
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
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkSamplerCreateInfo info;
	struct amdrdna_vk_sampler *sampler;
	uint64_t identity;

	/* The create info behind the device and its presence word. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkSamplerCreateInfo(reader, &session->arena, &info);
	identity = amdrdna_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The sampler keeps what a texture read uses. */
	sampler = kern_calloc(1U, sizeof(*sampler));
	if (sampler != NULL) {
		sampler->mag_filter = info.magFilter;
		sampler->min_filter = info.minFilter;
		sampler->mipmap_mode = info.mipmapMode;
		sampler->address_u = info.addressModeU;
		sampler->address_v = info.addressModeV;
		sampler->address_w = info.addressModeW;
		sampler->border_color = info.borderColor;
		sampler->unnormalized = info.unnormalizedCoordinates;
		sampler->compare_enable = info.compareEnable;
		sampler->compare_op = info.compareOp;
	}

	/* Publishes the sampler and answers. */
	amdrdna_vk_create_reply(session, reply, I915_VK_OBJ_SAMPLER, identity, sampler, amdrdna_vk_free_payload, 0);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/* Destroys a buffer nothing holds: it lets its memory go. */
static int
resource_buffer_destroy(
	struct amdrdna_vk_session *session,
	void *payload)
{
	struct amdrdna_vk_buffer *buffer;
	int error;

	UNUSED_PARAMETER(session);

	buffer = payload;

	/* The memory may go with it when nothing else holds the memory. */
	error = amdrdna_vk_object_release(buffer->memory);
	kern_free(buffer);

	/* Reports a memory whose destruction failed. */
	if (error != 0)
		return error;

	/* Succeeded: the buffer is gone. */
	return 0;
}

/* Destroys an image nothing holds: it lets its memory go. */
static int
resource_image_destroy(
	struct amdrdna_vk_session *session,
	void *payload)
{
	struct amdrdna_vk_image *image;
	int error;

	UNUSED_PARAMETER(session);

	image = payload;

	/* The memory may go with it when nothing else holds the memory. */
	error = amdrdna_vk_object_release(image->memory);
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
	struct amdrdna_vk_session *session,
	void *payload)
{
	struct amdrdna_vk_view *view;
	int error;

	UNUSED_PARAMETER(session);

	view = payload;

	/* The image may go with it when nothing else holds the image. */
	error = amdrdna_vk_object_release(view->image);
	kern_free(view);

	/* Reports an image whose destruction failed. */
	if (error != 0)
		return error;

	/* Succeeded: the view is gone. */
	return 0;
}

/*
 * vkCreateQueryPool: [device][present][sType][chain 0][flags][query type]
 * [count][statistics] and the create's tail -> the create's reply.  A pool
 * of at most AMDRDNA_VK_QUERIES_MAX queries is made: of result status
 * queries, every query unavailable and not ready; of occlusion queries
 * (each the depth units' begin and end counts) or timestamps, in GPU
 * memory of its own, zeroed (every query unavailable).  Another kind is
 * refused as a missing feature.
 *
 * Procedure referenced: zedBSD src/drivers/gpu/powervr/vk/vk-query.c
 * (create_pool: the same wire and the same refusal) and Mesa 25.0.7
 * src/amd/vulkan/radv_query.c (radv_create_query_pool: an occlusion
 * query's 16 bytes a render backend, a timestamp's 8).
 */
static int
resource_create_query_pool(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	struct amdrdna_vk_query_pool *pool;
	uint64_t identity;
	uint32_t type;
	uint32_t count;
	uint32_t backends;
	uint32_t stride;
	int error;

	/* The device, the record's header and flags, the type, the count and the statistics, then the identity. */
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u32(reader);
	type = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u32(reader);
	identity = amdrdna_vk_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* A result status, occlusion or timestamp pool of a bounded size; an occlusion pool's bytes for each backend there is. */
	error = 0;
	stride = 0U;
	backends = 0U;
	if (type == VK_QUERY_TYPE_OCCLUSION) {
		backends = amdrdna_vk_backends(session->owner);
		stride = AMDRDNA_VK_QUERY_BACKEND_BYTES * AMDRDNA_VK_QUERY_BACKENDS_MAX;
	} else if (type == VK_QUERY_TYPE_TIMESTAMP) {
		stride = AMDRDNA_VK_QUERY_TIMESTAMP_BYTES;
	}
	if ((type != VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR && stride == 0U) || count == 0U || count > AMDRDNA_VK_QUERIES_MAX ||
	    (type == VK_QUERY_TYPE_OCCLUSION && backends == 0U)) {
		kern_logf("amdrdna: vk: vkCreateQueryPool refused: type %u, %u queries\n", (unsigned)type, (unsigned)count);
		error = ENOTSUP;
	}

	/* The pool, every query unavailable and not ready; an occlusion or timestamp pool's memory zeroed. */
	pool = NULL;
	if (error == 0) {
		pool = kern_calloc(1U, sizeof(*pool));
		if (pool == NULL)
			error = ENOMEM;
	}
	if (pool != NULL) {
		pool->type = type;
		pool->count = count;
		pool->stride = stride;
		pool->backends = backends;
		if (stride != 0U)
			error = amdrdna_vk_gpu_alloc_system(session->owner, (uint64_t)stride * count, &pool->token, &pool->cpu, &pool->va);
		if (error == 0 && pool->cpu != NULL)
			kern_memset(pool->cpu, 0, (size_t)stride * count);
		if (error != 0) {
			kern_free(pool);
			pool = NULL;
		}
	}

	/* Publishes the pool and answers; a refused one is freed there. */
	amdrdna_vk_create_reply(session, reply, I915_VK_OBJ_QUERY_POOL, identity, pool, resource_query_pool_destroy, error);

	/* Succeeded: the reply carries the create's result. */
	return 0;
}

/* Destroys a query pool nothing holds: its GPU memory, then the pool. */
static int
resource_query_pool_destroy(
	struct amdrdna_vk_session *session,
	void *payload)
{
	struct amdrdna_vk_query_pool *pool;

	pool = payload;

	/* The memory an occlusion or timestamp pool has. */
	if (pool->token != NULL)
		amdrdna_vk_gpu_free(session->owner, pool->token);
	kern_free(pool);

	/* Succeeded: the pool is gone. */
	return 0;
}

/*
 * vkGetQueryPoolResults: [device][pool][first][count][data size][bytes]
 * [stride][flags] -> [result][bytes]{bytes}.  Each query's record
 * (resource_query_record) at its stride, zeros between and after the
 * records.  VK_NOT_READY when a query of the range is unavailable.  A
 * result status pool's statuses are final once the submissions that wrote
 * them ran, so a wait has nothing to wait for; an occlusion or timestamp
 * pool's queries are the GPU's to write, and VK_QUERY_RESULT_WAIT_BIT
 * waits for the session's newest runs first.  A range or a size outside
 * the pool or the reply answers the device lost, with no bytes.
 *
 * Procedure referenced: zedBSD src/drivers/gpu/powervr/vk/vk-query.c
 * (pool_results: the same wire and the same answers).
 */
static int
resource_query_results(
	struct amdrdna_vk_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	const struct amdrdna_vk_query_pool *pool;
	struct amdrdna_vk_ticket last[2];
	uint8_t record[RESOURCE_QUERY_RECORD_MAX];
	uint64_t identity;
	uint64_t bytes;
	uint64_t stride;
	uint64_t written;
	uint64_t end;
	uint64_t value;
	uint32_t record_bytes;
	uint32_t first;
	uint32_t count;
	uint32_t flags;
	uint32_t query;
	uint32_t result;
	uint32_t available;
	uint32_t queue;
	bool outside;
	int error;

	/* The device, the pool, the range, the sizes, the stride and the flags. */
	(void)drv_i915_wire_read_u64(reader);
	identity = drv_i915_wire_read_u64(reader);
	first = drv_i915_wire_read_u32(reader);
	count = drv_i915_wire_read_u32(reader);
	(void)drv_i915_wire_read_u64(reader);
	bytes = drv_i915_wire_read_u64(reader);
	stride = drv_i915_wire_read_u64(reader);
	flags = drv_i915_wire_read_u32(reader);
	if (reader->error != 0)
		return EINVAL;

	/* The bytes of one record, as its flags lay it out. */
	record_bytes = resource_query_record(0U, 0U, flags, record);

	/* An unknown pool, a range past it, or records that do not fit the reply or their stride. */
	pool = amdrdna_vk_payload(session, I915_VK_OBJ_QUERY_POOL, identity);
	outside = false;
	if (pool == NULL) {
		outside = true;
	} else if (first > pool->count || count > pool->count - first) {
		outside = true;
	} else if (bytes > RESOURCE_QUERY_REPLY_MAX || stride < record_bytes) {
		outside = true;
	} else if (count != 0U && (uint64_t)(count - 1U) * stride + record_bytes > bytes) {
		outside = true;
	}

	/* The GPU's queries waited for when asked: the session's newest runs on both queues ended. */
	error = 0;
	if (!outside && pool->token != NULL && (flags & VK_QUERY_RESULT_WAIT_BIT) != 0U) {
		amdrdna_vk_execute_last(session, last);
		for (queue = 0U; error == 0 && queue < 2U; queue++)
			error = amdrdna_vk_gpu_wait(session->owner, &last[queue]);
	}

	/* Answers such a request, or a wait that did not end, with the device lost and no bytes. */
	if (outside || error != 0) {
		drv_i915_wire_reply_u32(reply, (uint32_t)VK_ERROR_DEVICE_LOST);
		drv_i915_wire_reply_u64(reply, 0U);
		return 0;
	}

	/* VK_NOT_READY when any query of the range is unavailable. */
	result = (uint32_t)VK_SUCCESS;
	for (query = first; query < first + count; query++) {
		resource_query_value(pool, query, &value, &available);
		if (available == 0U)
			result = (uint32_t)VK_NOT_READY;
	}

	/* The result, then the bytes the records take. */
	drv_i915_wire_reply_u32(reply, result);
	drv_i915_wire_reply_u64(reply, bytes);

	/* Each record, then zeros to the next record's start (or the data's end). */
	written = 0U;
	for (query = first; query < first + count; query++) {
		resource_query_value(pool, query, &value, &available);
		record_bytes = resource_query_record(value, available, flags, record);
		drv_i915_wire_reply_bytes(reply, record, record_bytes);
		end = written + stride;
		if (end > bytes)
			end = bytes;
		resource_reply_zeros(reply, end - written - record_bytes);
		written = end;
	}

	/* Zeros to the end of the data. */
	resource_reply_zeros(reply, bytes - written);

	/* Succeeded: the records are written. */
	return 0;
}

/*
 * Gives a query's result and whether it is available: a result status
 * query's status (sign-extended); an occlusion query's samples, the sum
 * over the enabled depth units of the end count less the begin count,
 * available when every one of them wrote both (each count's bit 63 marks
 * it written); a timestamp, available when written (not zero, as the
 * pool's memory was made and is reset).
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_query.c
 * (radv_GetQueryPoolResults: the occlusion case's enabled render
 * backends, the valid bit 63 of both counts, the sum of end less start;
 * the timestamp case's value against its not-ready marker).
 */
static void
resource_query_value(
	const struct amdrdna_vk_query_pool *pool,
	uint32_t query,
	uint64_t *value,
	uint32_t *available)
{
	const uint8_t *at;
	uint64_t begin;
	uint64_t end;
	uint32_t backend;

	/* A result status query's, kept by the executor. */
	if (pool->token == NULL) {
		*value = (uint64_t)(int64_t)pool->status[query];
		*available = pool->available[query];
		return;
	}

	/* A timestamp's 64 bits, written or not. */
	at = pool->cpu + (size_t)query * pool->stride;
	if (pool->type == VK_QUERY_TYPE_TIMESTAMP) {
		kern_memcpy(value, at, sizeof(*value));
		*available = *value != 0U;
		return;
	}

	/* The occlusion counts of each enabled depth unit. */
	*value = 0U;
	*available = 1U;
	for (backend = 0U; backend < AMDRDNA_VK_QUERY_BACKENDS_MAX; backend++) {
		if ((pool->backends & (1U << backend)) == 0U)
			continue;
		kern_memcpy(&begin, at + backend * AMDRDNA_VK_QUERY_BACKEND_BYTES, sizeof(begin));
		kern_memcpy(&end, at + backend * AMDRDNA_VK_QUERY_BACKEND_BYTES + 8U, sizeof(end));
		if ((begin & RESOURCE_QUERY_WRITTEN) == 0U || (end & RESOURCE_QUERY_WRITTEN) == 0U) {
			*available = 0U;
			continue;
		}
		*value += end - begin;
	}
}

/*
 * Writes one query's record as vkGetQueryPoolResults lays it out: its
 * result (a result status sign-extended to 64 bits with
 * VK_QUERY_RESULT_64_BIT), then with
 * VK_QUERY_RESULT_WITH_AVAILABILITY_BIT whether the query is available,
 * each a word of 4 or 8 bytes, least significant byte first.  Returns the
 * record's bytes (at most RESOURCE_QUERY_RECORD_MAX).
 *
 * Values referenced: the Vulkan 1.3 specification (vkGetQueryPoolResults:
 * one result of a result status, occlusion or timestamp query, then the
 * availability word).
 */
static uint32_t
resource_query_record(
	uint64_t value,
	uint32_t available,
	uint32_t flags,
	uint8_t *record)
{
	uint64_t words[2];
	uint32_t width;
	uint32_t bytes;
	uint32_t index;

	/* Words of four bytes, or eight with the 64-bit flag. */
	width = 4U;
	if ((flags & VK_QUERY_RESULT_64_BIT) != 0U)
		width = 8U;

	/* The result, then the availability when asked for. */
	words[0] = value;
	words[1] = 0U;
	if (available != 0U)
		words[1] = 1U;
	bytes = width;
	if ((flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0U)
		bytes += width;

	/* Each word's bytes, least significant first, as the host reads them. */
	for (index = 0U; index < bytes; index++)
		record[index] = (uint8_t)(words[index / width] >> (8U * (index % width)));

	/* Succeeded: the record's bytes. */
	return bytes;
}

/* Writes bytes of zeros to a reply, RESOURCE_QUERY_ZEROS at a time. */
static void
resource_reply_zeros(
	struct i915_wire_writer *reply,
	uint64_t bytes)
{
	static const uint8_t zeros[RESOURCE_QUERY_ZEROS];
	uint64_t chunk;

	/* The zeros in chunks. */
	while (bytes != 0U) {
		chunk = bytes;
		if (chunk > RESOURCE_QUERY_ZEROS)
			chunk = RESOURCE_QUERY_ZEROS;
		drv_i915_wire_reply_bytes(reply, zeros, (size_t)chunk);
		bytes -= chunk;
	}
}

/* The video decode (its commands, sessions, parameters and records). */
#include "drivers/gpu/amdrdna/vk/vk-video.inc"
