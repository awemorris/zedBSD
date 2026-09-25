/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The executor's images, image views and samplers (see image.h).
 *
 * Every command is decoded exactly as libvulkan encodes it: the records
 * through the generated codec, the framing around them as read from the
 * library's own senders (objects.c and resources.c of libvulkan).
 */

#include "image.h"
#include "codec.h"
#include "gfx.h"
#include "internal.h"
#include "object.h"
#include "reply.h"
#include <kern/kcrt.h>

#include <kern/klog.h>
#include <kern/kmem.h>

#include <libc/vulkan/vulkan_core.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

#include "vulkan-codec.inc"

/* The largest width and height of an image the executor lays out. */
#define I915_GFX_IMAGE_MAX_EXTENT	16384U

/* The most mip levels an image has: 16384 down to one texel. */
#define I915_GFX_IMAGE_MAX_LEVELS	15U

/*
 * The alignment of a mip level in the 2D mip layout, in texels, across and
 * down.  It is the Surface Horizontal and Vertical Alignment (HALIGN_4,
 * VALIGN_4) the surface state names (state.c), which isl picks for a linear
 * 32-bit colour surface on this generation (isl_gfx8.c,
 * isl_gfx8_choose_image_alignment_el()).
 */
#define I915_GFX_IMAGE_LEVEL_ALIGN	4U

static uint32_t i915_gfx_format_bytes(uint32_t format);
static int i915_gfx_image_supported(const VkImageCreateInfo *info);
static uint32_t i915_gfx_image_max_levels(uint32_t width, uint32_t height);
static uint32_t i915_gfx_minify(uint32_t extent, uint32_t level);
static uint32_t i915_gfx_level_align(uint32_t extent);
static void i915_gfx_level_origin(const struct i915_gfx_image *image, uint32_t level, uint32_t *x, uint32_t *y);
static void i915_gfx_float_bits(uint32_t *destination, const float *source);

/*
 * Creates a VkImage: vkCreateImage, a generic create.
 *
 * XXX: one kind of image -- 2D, one layer, one sample, linear, with any
 * number of mip levels down to one texel (drv_i915_gfx_image_layout()).
 * Anything else is refused here, by name, rather than laid out wrongly.  A
 * depth image has one level and is laid out in whole Y tiles.
 */
int
drv_i915_gfx_create_image(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageCreateInfo info;
	struct i915_gfx_image *image;
	uint64_t identity;
	int supported;
	int error;

	/* Decodes the create info behind the device and its presence marker. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkImageCreateInfo(reader, &session->arena, &info);
	identity = drv_i915_gfx_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Decides whether the image has the one layout the executor supports. */
	supported = i915_gfx_image_supported(&info);

	/* Refuses any other image by name, and allocates a supported one. */
	image = NULL;
	error = 0;
	if (supported == 0) {
		kern_logf("i915: vk: XXX vkCreateImage refused: type %u format %u %ux%ux%u levels %u layers %u samples %u\n",
			  (unsigned)info.imageType,
			  (unsigned)info.format,
			  info.extent.width,
			  info.extent.height,
			  info.extent.depth,
			  info.mipLevels,
			  info.arrayLayers,
			  (unsigned)info.samples);
		error = ENOTSUP;
	} else {
		image = kern_calloc(1U, sizeof(*image));
	}

	/* Lays the image out from its format, extent and levels; the check above makes the layout succeed. */
	if (image != NULL) {
		image->format = info.format;
		image->width = info.extent.width;
		image->height = info.extent.height;
		image->usage = info.usage;
		image->levels = info.mipLevels;
		(void)drv_i915_gfx_image_layout(image);
	}

	/* Publishes the image and answers; a refused or failed image is reported there. */
	drv_i915_gfx_create_reply(session, reply, I915_VK_OBJ_IMAGE, identity, image, error);

	/* Succeeded: the reply carries the result of the create. */
	return 0;
}

/*
 * Creates a VkImageView: vkCreateImageView, a generic create.
 *
 * The view keeps its range of mip levels (VK_REMAINING_MIP_LEVELS runs to
 * the last level).  A view of an unknown image, or of levels the image does
 * not have, fails.  XXX: swizzles and array layers are not applied; the
 * texels are read in the image's own format.
 */
int
drv_i915_gfx_create_image_view(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageViewCreateInfo info;
	struct i915_gfx_view *view;
	struct i915_gfx_image *image;
	uint64_t identity;
	uint32_t base_level;
	uint32_t level_count;
	int error;

	/* Decodes the create info behind the device and its presence marker. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkImageViewCreateInfo(reader, &session->arena, &info);
	identity = drv_i915_gfx_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Resolves the image the view shows; the view of an unknown image fails. */
	view = NULL;
	error = 0;
	image = drv_i915_object_lookup(session->vk, I915_VK_OBJ_IMAGE, (uint64_t)(uintptr_t)info.image);
	if (image == NULL)
		error = EINVAL;

	/* Resolves the levels the view shows; the remaining levels run to the last one. */
	base_level = info.subresourceRange.baseMipLevel;
	level_count = info.subresourceRange.levelCount;
	if (image != NULL && level_count == VK_REMAINING_MIP_LEVELS && base_level < image->levels)
		level_count = image->levels - base_level;

	/* Refuses a first level the image does not have, an empty range and one past the last level. */
	if (image != NULL) {
		if (base_level >= image->levels) {
			error = EINVAL;
		} else if (level_count == 0U) {
			error = EINVAL;
		} else if (level_count > image->levels - base_level) {
			error = EINVAL;
		}
	}

	/* Says why the view of an existing image was refused, and allocates an accepted view. */
	if (image != NULL && error != 0) {
		kern_logf("i915: vk: vkCreateImageView refused: levels %u count %u of an image of %u levels\n",
			  info.subresourceRange.baseMipLevel,
			  info.subresourceRange.levelCount,
			  image->levels);
	} else if (image != NULL) {
		view = kern_calloc(1U, sizeof(*view));
	}

	/* Records the image, the format and the levels the view was created with. */
	if (view != NULL) {
		view->image = image;
		view->format = info.format;
		view->base_level = base_level;
		view->level_count = level_count;
	}

	/* Publishes the view and answers; a failed view is reported there. */
	drv_i915_gfx_create_reply(session, reply, I915_VK_OBJ_IMAGE_VIEW, identity, view, error);

	/* Succeeded: the reply carries the result of the create. */
	return 0;
}

/*
 * Creates a VkSampler: vkCreateSampler, a generic create.
 *
 * The sampler keeps the filters, the mipmap mode, the LOD bias and range
 * and the address modes along u and v.  XXX: anisotropy, depth comparison,
 * the border colour and unnormalized coordinates are not kept.
 */
int
drv_i915_gfx_create_sampler(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkSamplerCreateInfo info;
	struct i915_gfx_sampler *sampler;
	uint64_t identity;

	/* Decodes the create info behind the device and its presence marker. */
	kern_memset(&info, 0, sizeof(info));
	(void)drv_i915_wire_read_u64(reader);
	(void)drv_i915_wire_read_u64(reader);
	i915_vkc_dec_VkSamplerCreateInfo(reader, &session->arena, &info);
	identity = drv_i915_gfx_create_tail(reader);
	if (reader->error != 0)
		return EINVAL;

	/* Allocates the sampler and keeps what a texture read uses. */
	sampler = kern_calloc(1U, sizeof(*sampler));
	if (sampler != NULL) {
		sampler->mag_filter = info.magFilter;
		sampler->min_filter = info.minFilter;
		sampler->address_u = info.addressModeU;
		sampler->address_v = info.addressModeV;
		sampler->mipmap_mode = info.mipmapMode;
		i915_gfx_float_bits(&sampler->lod_bias, &info.mipLodBias);
		i915_gfx_float_bits(&sampler->min_lod, &info.minLod);
		i915_gfx_float_bits(&sampler->max_lod, &info.maxLod);
	}

	/* Publishes the sampler and answers; a failed allocation is reported there. */
	drv_i915_gfx_create_reply(session, reply, I915_VK_OBJ_SAMPLER, identity, sampler, 0);

	/* Succeeded: the reply carries the result of the create. */
	return 0;
}

/*
 * Reports the layout of one subresource of an image:
 * vkGetImageSubresourceLayout.
 *
 * The command is [device][image][present][VkImageSubresource][present] and
 * the reply [present][VkSubresourceLayout].  A level starts at its place in
 * the mip layout and has the image's pitch; the array layer is not
 * consulted (every image has one).  An unknown image or level reports an
 * empty layout.
 */
int
drv_i915_gfx_subresource_layout(
	struct i915_render_session *session,
	struct i915_wire_reader *reader,
	struct i915_wire_writer *reply)
{
	VkImageSubresource subresource;
	VkSubresourceLayout layout;
	struct i915_gfx_image *image;
	uint64_t image_id;
	uint64_t present;
	uint32_t level_x;
	uint32_t level_y;
	uint32_t level_width;
	uint32_t level_height;

	/* Reads the image behind the device and resolves it. */
	(void)drv_i915_wire_read_u64(reader);
	image_id = drv_i915_wire_read_u64(reader);
	image = drv_i915_object_lookup(session->vk, I915_VK_OBJ_IMAGE, image_id);

	/* Decodes the subresource when it is present; an absent one is level 0. */
	kern_memset(&subresource, 0, sizeof(subresource));
	present = drv_i915_wire_read_u64(reader);
	if (present != 0U)
		i915_vkc_dec_VkImageSubresource(reader, &session->arena, &subresource);

	/* Skips the output's presence marker. */
	(void)drv_i915_wire_read_u64(reader);
	if (reader->error != 0)
		return EINVAL;

	/*
	 * Describes the level: its first texel in the mip layout, its rows at
	 * the image's pitch up to its last texel, and the whole image as the
	 * array and depth pitch.  A single-level image is its whole allocation,
	 * padding rows of a depth image included.
	 */
	kern_memset(&layout, 0, sizeof(layout));
	if (image != NULL && subresource.mipLevel < image->levels) {
		i915_gfx_level_origin(image, subresource.mipLevel, &level_x, &level_y);
		level_width = i915_gfx_minify(image->width, subresource.mipLevel);
		level_height = i915_gfx_minify(image->height, subresource.mipLevel);
		layout.offset = (uint64_t)level_y * image->pitch + (uint64_t)level_x * 4U;
		layout.size = (uint64_t)(level_height - 1U) * image->pitch + (uint64_t)level_width * 4U;
		if (image->levels == 1U)
			layout.size = image->bytes;
		layout.rowPitch = image->pitch;
		layout.arrayPitch = image->bytes;
		layout.depthPitch = image->bytes;
	}

	/* Writes the layout behind its presence marker. */
	drv_i915_wire_reply_u64(reply, 1U);
	i915_vkc_enc_VkSubresourceLayout(reply, &layout);

	/* Succeeded: the reply carries the layout. */
	return 0;
}

/*
 * Lays an image out from its format, width, height and levels: sets its
 * pitch and the bytes it occupies.
 *
 * One colour level is linear rows of width * 4 bytes, as isl leaves a
 * single-level linear surface unpadded.  A depth image is Y-tiled: a Gen9+
 * depth buffer always is (isl_emit_depth_stencil.c), whole 4 KiB tiles of
 * 128 bytes by 32 rows; nothing reads depth texel by texel here (the clear
 * is one value), so only the extent matters.  XXX: a depth image can not be
 * copied or sampled.
 *
 * Several colour levels take the 2D mip layout the hardware samples (isl.c,
 * isl_calc_phys_slice0_extent_sa_gfx4_2d(), ISL_DIM_LAYOUT_GFX4_2D, which isl
 * uses for every 2D surface on Gen9+): level 0 at the top, level 1 below it
 * at the left edge, level 2 to the right of level 1 and every further level
 * below the one before, each level's extent rounded up to the level
 * alignment; all levels share one pitch.  Returns EINVAL for an image the
 * executor does not lay out.
 */
int
drv_i915_gfx_image_layout(
	struct i915_gfx_image *image)
{
	uint32_t texel_bytes;
	uint32_t max_levels;
	uint32_t level;
	uint32_t level_width;
	uint32_t level_height;
	uint32_t top_width;
	uint32_t bottom_width;
	uint32_t left_height;
	uint32_t right_height;
	uint32_t layout_width;
	uint32_t layout_height;

	/* Refuses a format the executor does not lay out. */
	texel_bytes = i915_gfx_format_bytes(image->format);
	if (texel_bytes == 0U)
		return EINVAL;

	/* Refuses an empty image. */
	if (image->width == 0U || image->height == 0U)
		return EINVAL;

	/* Refuses no level, and more levels than halve the extent down to one texel. */
	max_levels = i915_gfx_image_max_levels(image->width, image->height);
	if (image->levels == 0U || image->levels > max_levels)
		return EINVAL;

	/* One level is linear rows of whole texels. */
	if (image->levels == 1U) {
		image->pitch = image->width * texel_bytes;
		image->bytes = (uint64_t)image->pitch * image->height;

		/* A depth image is rounded up to whole Y tiles. */
		if (image->format == VK_FORMAT_D32_SFLOAT) {
			image->pitch = (image->pitch + 127U) & ~127U;
			image->bytes = (uint64_t)image->pitch * ((image->height + 31U) & ~31U);
		}

		/* Succeeded: one level needs no mip layout. */
		return 0;
	}

	/* Refuses a mipmapped depth image. */
	if (image->format == VK_FORMAT_D32_SFLOAT)
		return EINVAL;

	/*
	 * Measures the two columns of the layout: level 0 spans the top; level
	 * 1 starts the left column below it, level 2 the right column, and every
	 * further level extends the right column downwards.
	 */
	top_width = 0U;
	bottom_width = 0U;
	left_height = 0U;
	right_height = 0U;
	for (level = 0U; level < image->levels; level++) {
		level_width = i915_gfx_level_align(i915_gfx_minify(image->width, level));
		level_height = i915_gfx_level_align(i915_gfx_minify(image->height, level));
		if (level == 0U) {
			/* Level 0 is above both columns. */
			top_width = level_width;
			left_height = level_height;
			right_height = level_height;
		} else if (level == 1U) {
			/* Level 1 heads the left column. */
			bottom_width = level_width;
			left_height += level_height;
		} else if (level == 2U) {
			/* Level 2 heads the right column, beside level 1. */
			bottom_width += level_width;
			right_height += level_height;
		} else {
			/* Every further level goes below the one before, in the right column. */
			right_height += level_height;
		}
	}

	/* The layout is as wide as its wider part and as tall as its taller column. */
	layout_width = top_width;
	if (bottom_width > layout_width)
		layout_width = bottom_width;
	layout_height = left_height;
	if (right_height > layout_height)
		layout_height = right_height;

	/* Every level shares the layout's pitch. */
	image->pitch = layout_width * texel_bytes;
	image->bytes = (uint64_t)image->pitch * layout_height;

	/* Succeeded: the image has its pitch and its size. */
	return 0;
}

/*
 * Describes one mip level of an image as the linear surface it is: its
 * first texel, its extent and the image's pitch, in the image's format.
 *
 * Returns EINVAL for a level the image does not have, or an image that is
 * not bound to storage.
 */
int
drv_i915_gfx_image_level(
	const struct i915_gfx_image *image,
	uint32_t level,
	struct i915_gfx_surface *surface)
{
	uint32_t level_x;
	uint32_t level_y;
	uint64_t offset;

	/* Refuses a level the image does not have. */
	if (level >= image->levels)
		return EINVAL;

	/* Finds the level's first texel in the mip layout. */
	i915_gfx_level_origin(image, level, &level_x, &level_y);
	offset = (uint64_t)level_y * image->pitch + (uint64_t)level_x * 4U;

	/* Takes the level's address, its extent, the shared pitch and the format. */
	surface->va = drv_i915_gfx_memory_va(image->memory, image->offset + offset);
	surface->width = i915_gfx_minify(image->width, level);
	surface->height = i915_gfx_minify(image->height, level);
	surface->pitch = image->pitch;
	surface->format = image->format;

	/* Refuses an image that is not bound to storage. */
	if (surface->va == 0U)
		return EINVAL;

	/* Succeeded: the surface describes the level. */
	return 0;
}

/* Reports the bytes to a texel of a format the executor lays out; 0 for any other. */
static uint32_t
i915_gfx_format_bytes(
	uint32_t format)
{
	/* Only these formats are laid out. */
	switch (format) {
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_D32_SFLOAT:
		/* A four-byte texel. */
		return 4U;
	default:
		/* Not a format the executor lays out. */
		return 0U;
	}
}

/* Decides whether an image has the one layout the executor supports. */
static int
i915_gfx_image_supported(
	const VkImageCreateInfo *info)
{
	uint32_t texel_bytes;
	uint32_t max_levels;

	/* Only a 2D image. */
	if (info->imageType != VK_IMAGE_TYPE_2D)
		return 0;

	/* Only one array layer. */
	if (info->arrayLayers != 1U)
		return 0;

	/* Only one sample. */
	if (info->samples != VK_SAMPLE_COUNT_1_BIT)
		return 0;

	/* Only a depth of one. */
	if (info->extent.depth != 1U)
		return 0;

	/* Refuses an empty image. */
	if (info->extent.width == 0U || info->extent.height == 0U)
		return 0;

	/* Refuses an image wider or taller than the executor lays out. */
	if (info->extent.width > I915_GFX_IMAGE_MAX_EXTENT || info->extent.height > I915_GFX_IMAGE_MAX_EXTENT)
		return 0;

	/* Only a format the executor lays out. */
	texel_bytes = i915_gfx_format_bytes(info->format);
	if (texel_bytes == 0U)
		return 0;

	/* Only levels down to one texel. */
	max_levels = i915_gfx_image_max_levels(info->extent.width, info->extent.height);
	if (info->mipLevels == 0U || info->mipLevels > max_levels)
		return 0;

	/* Only one level of a depth image. */
	if (info->format == VK_FORMAT_D32_SFLOAT && info->mipLevels != 1U)
		return 0;

	/* Succeeded: the image has the supported layout. */
	return 1;
}

/* Reports how many mip levels an extent has down to one texel, the larger side halving each time. */
static uint32_t
i915_gfx_image_max_levels(
	uint32_t width,
	uint32_t height)
{
	uint32_t largest;
	uint32_t levels;

	/* Halves the larger side until it is one texel, counting the levels. */
	largest = width;
	if (height > largest)
		largest = height;
	levels = 1U;
	while (largest > 1U && levels < I915_GFX_IMAGE_MAX_LEVELS) {
		largest >>= 1;
		levels++;
	}

	/* Succeeded: the number of levels. */
	return levels;
}

/* Reports the extent of a level: the level-0 extent halved `level` times, at least one texel. */
static uint32_t
i915_gfx_minify(
	uint32_t extent,
	uint32_t level)
{
	uint32_t minified;

	/* A level past the bits of the extent is one texel. */
	if (level >= 32U)
		return 1U;

	/* Halves the extent once for each level, stopping at one texel. */
	minified = extent >> level;
	if (minified == 0U)
		minified = 1U;

	/* Succeeded: the extent of the level. */
	return minified;
}

/* Rounds a level's width or height up to the level alignment. */
static uint32_t
i915_gfx_level_align(
	uint32_t extent)
{
	/* Succeeded: the extent in whole alignment units. */
	return (extent + I915_GFX_IMAGE_LEVEL_ALIGN - 1U) & ~(I915_GFX_IMAGE_LEVEL_ALIGN - 1U);
}

/*
 * Finds where a level starts in the 2D mip layout, in texels across and
 * rows down (isl.c, get_image_offset_sa_gfx4_2d()): below level 0 for level
 * 1, and from level 2 on to the right of level 1, below the levels between.
 */
static void
i915_gfx_level_origin(
	const struct i915_gfx_image *image,
	uint32_t level,
	uint32_t *x,
	uint32_t *y)
{
	uint32_t before;

	/* Moves past every level before this one: level 1 moves right, every other level down. */
	*x = 0U;
	*y = 0U;
	for (before = 0U; before < level; before++) {
		if (before == 1U) {
			*x += i915_gfx_level_align(i915_gfx_minify(image->width, before));
		} else {
			*y += i915_gfx_level_align(i915_gfx_minify(image->height, before));
		}
	}
}

/* Copies a float as its 32 bits; no floating-point register is involved. */
static void
i915_gfx_float_bits(
	uint32_t *destination,
	const float *source)
{
	/* The bytes of the float are its bit pattern. */
	kern_memcpy(destination, source, sizeof(*destination));
}
