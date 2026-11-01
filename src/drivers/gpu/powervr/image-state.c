/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PowerVR core's view of images: the format table, the layout of an
 * image in memory, the texture unit's state words of a view and of a
 * sampler, and the bytes of a descriptor.  Nothing here touches the core
 * or memory; the executor's image commands (vk-image.c) call it, and the
 * host check builds it unchanged.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/powervr/image-state.h"

/* A format code a unit does not have. */
#define NO_CODE				0xFFU

/* The features of an 8-bit colour format a render pass draws to and samples. */
#define FEATURES_RENDER_COLOR		(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | \
					 VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | \
					 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | \
					 VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | \
					 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
					 VK_FORMAT_FEATURE_TRANSFER_DST_BIT | \
					 VK_FORMAT_FEATURE_BLIT_SRC_BIT | \
					 VK_FORMAT_FEATURE_BLIT_DST_BIT)

/* The features of a one- or two-channel format that is sampled and copied only. */
#define FEATURES_SAMPLED_COLOR		(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | \
					 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
					 VK_FORMAT_FEATURE_TRANSFER_DST_BIT | \
					 VK_FORMAT_FEATURE_BLIT_SRC_BIT | \
					 VK_FORMAT_FEATURE_BLIT_DST_BIT)

/* The features of a depth format. */
#define FEATURES_DEPTH			(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | \
					 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
					 VK_FORMAT_FEATURE_TRANSFER_DST_BIT)

/*
 * The features of a float format a shader samples and the transfer
 * copies and clears; a 16-bit float one filters too, and is a colour
 * attachment that blends (the compiler packs half floats into two pixel
 * output registers).
 */
#define FEATURES_SAMPLED_F32		(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | \
					 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
					 VK_FORMAT_FEATURE_TRANSFER_DST_BIT)
#define FEATURES_SAMPLED_F16		(FEATURES_SAMPLED_F32 | \
					 VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
#define FEATURES_RENDER_F16		(FEATURES_SAMPLED_F16 | \
					 VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | \
					 VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)

/*
 * The features of a two-plane 4:2:0 format: its planes are copied out
 * (and sampled through views of their own one-plane formats, which an
 * image made with the extended usage flag takes).
 */
#define FEATURES_PLANAR			VK_FORMAT_FEATURE_TRANSFER_SRC_BIT

/* The features of the copies of a linear image. */
#define FEATURES_TRANSFER		(VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
					 VK_FORMAT_FEATURE_TRANSFER_DST_BIT)

/* The texture unit's kinds of image (the low field of the first view word). */
#define VIEW_KIND_TWIDDLED_2D		1U
#define VIEW_KIND_STRIDED		4U

/* The texture unit's channel selections: a source channel 0 to 3, one or zero. */
#define SELECT_ONE			4U
#define SELECT_ZERO			5U

/* The texture unit's filters, and its address modes. */
#define FILTER_POINT			0U
#define FILTER_LINEAR			1U
#define WRAP_REPEAT			0U
#define WRAP_MIRROR			1U
#define WRAP_CLAMP_EDGE			2U
#define WRAP_CLAMP_BORDER		4U

/*
 * The fixed-point forms of a sampler's level of detail: six fraction
 * bits and a largest value for the clamps, eight fraction bits and a
 * biased zero for the bias.
 */
#define LOD_FRACTION_BITS		6U
#define LOD_LARGEST			959
#define BIAS_FRACTION_BITS		8U
#define BIAS_ZERO			4095
#define BIAS_LARGEST			8191

/* The bytes of each kind of descriptor in a set. */
#define DESCRIPTOR_BUFFER_BYTES		16U
#define DESCRIPTOR_IMAGE_BYTES		32U
#define DESCRIPTOR_SAMPLER_BYTES	48U

/*
 * The formats the executor takes, with each one's texture and pixel back
 * end codes, the source of each output channel and its features.
 *
 * Values referenced: Mesa 26.2.5 src/imagination/vulkan/pvr_arch_formats.c
 * (the texture format table and the pixel back end format table, the rows
 * of these formats), src/imagination/csbgen/rogue/texstate.xml (enum
 * FORMAT) and src/imagination/csbgen/rogue/pbestate.xml (enum PACKMODE).
 * The channel sources are those of the formats' own channel orders, and a
 * depth format has its value in the first channel only, as in
 * src/imagination/vulkan/pvr_formats.c (pvr_get_format_swizzle_for_tpu).
 * The table never changes.
 */
static const struct powervr_format format_table[] = {
	{ VK_FORMAT_R8_UNORM, 1, POWERVR_FORMAT_COLOR, 0, 0x2F,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_SAMPLED_COLOR, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R8G8_UNORM, 2, POWERVR_FORMAT_COLOR, 7, 0x23,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_SAMPLED_COLOR, FEATURES_TRANSFER, 0 },
	/* RGBA8 is not a vertex format: the vertex fetch does not normalise its bytes. */
	{ VK_FORMAT_R8G8B8A8_UNORM, 4, POWERVR_FORMAT_COLOR, 12, 0x00,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_3 },
	  FEATURES_RENDER_COLOR, FEATURES_RENDER_COLOR, 0 },
	{ VK_FORMAT_R8G8B8A8_SRGB, 4, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_SRGB, 12, 0x00,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_3 },
	  FEATURES_RENDER_COLOR, FEATURES_RENDER_COLOR, 0 },
	{ VK_FORMAT_B8G8R8A8_UNORM, 4, POWERVR_FORMAT_COLOR, 12, 0x00,
	  { POWERVR_CHANNEL_2, POWERVR_CHANNEL_1, POWERVR_CHANNEL_0, POWERVR_CHANNEL_3 },
	  FEATURES_RENDER_COLOR, FEATURES_RENDER_COLOR, 0 },
	{ VK_FORMAT_B8G8R8A8_SRGB, 4, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_SRGB, 12, 0x00,
	  { POWERVR_CHANNEL_2, POWERVR_CHANNEL_1, POWERVR_CHANNEL_0, POWERVR_CHANNEL_3 },
	  FEATURES_RENDER_COLOR, FEATURES_RENDER_COLOR, 0 },
	{ VK_FORMAT_D16_UNORM, 2, POWERVR_FORMAT_DEPTH, 9, 0x31,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_ABSENT, POWERVR_CHANNEL_ABSENT, POWERVR_CHANNEL_ABSENT },
	  FEATURES_DEPTH, 0, 0 },
	{ VK_FORMAT_D32_SFLOAT, 4, POWERVR_FORMAT_DEPTH, 18, 0x36,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_ABSENT, POWERVR_CHANNEL_ABSENT, POWERVR_CHANNEL_ABSENT },
	  FEATURES_DEPTH, 0, 0 },
	{ VK_FORMAT_R16G16B16A16_SFLOAT, 8, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_FLOAT, 28, 0x0A,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_3 },
	  FEATURES_RENDER_F16, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R32_SFLOAT, 4, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_FLOAT, 18, 0x36,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_SAMPLED_F32, FEATURES_TRANSFER, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT },
	{ VK_FORMAT_R32G32_SFLOAT, 8, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_FLOAT, 34, 0x2A,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_SAMPLED_F32, FEATURES_TRANSFER, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT },
	{ VK_FORMAT_R32G32B32_SFLOAT, 12, 0, NO_CODE, 0x1F,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_ONE },
	  0, 0, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT },
	{ VK_FORMAT_R32G32B32A32_SFLOAT, 16, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_FLOAT, 61, 0x0D,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_3 },
	  FEATURES_SAMPLED_F32, FEATURES_TRANSFER, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT },
	/* NV12: neither unit reads it whole, only its planes as R8 and R8G8 (powervr_image_lay_out_planes). */
	{ VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, 1, POWERVR_FORMAT_PLANAR, NO_CODE, NO_CODE,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_ONE },
	  FEATURES_PLANAR, FEATURES_PLANAR, 0 }
};

static uint64_t place(uint64_t value, unsigned low, unsigned high);
static uint32_t round_up_power(uint32_t value);
static uint32_t round_up(uint32_t value, uint32_t unit);
static uint32_t common_multiple(uint32_t a, uint32_t b);
static uint32_t channel_select(const struct powervr_format *format, uint32_t mapping, unsigned output);
static int sampler_wrap(uint32_t mode, uint32_t *wrap);
static int32_t fixed_from_bits(uint32_t bits, unsigned fraction, int32_t smallest, int32_t largest);
static uint32_t half_from_float(uint32_t bits);
static uint32_t unorm8_from_float(uint32_t bits);

/* Finds the executor's entry of a Vulkan format, or NULL when it does not take the format. */
const struct powervr_format *
powervr_format_find(
	uint32_t vk_format)
{
	size_t index;

	/* Looks the format up in the table. */
	for (index = 0; index < sizeof(format_table) / sizeof(format_table[0]); index++) {
		if (format_table[index].vk_format == vk_format)
			return &format_table[index];
	}

	/* Not one the executor takes. */
	return NULL;
}

/*
 * Reports the features a format has on linear images: all of them, or
 * all but sampling while the shared path's switch is off
 * (POWERVR_FORMAT_LINEAR_SAMPLED).
 */
uint32_t
powervr_format_linear_features(
	const struct powervr_format *format)
{
	uint32_t sampled;

	/* Linear images report no sampling while the switch is off. */
	sampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
	if (POWERVR_FORMAT_LINEAR_SAMPLED == 0)
		return format->linear_features & ~sampled;

	/* Succeeded: the format's linear features. */
	return format->linear_features;
}

/*
 * Lays an image of format out in memory.
 *
 * A linear image is one level and one layer of rows whose bytes are a
 * multiple of 32 and whose texels are a multiple of two.  An optimal one
 * is twiddled: its edges are rounded up to powers of two, a volume's
 * edges then to multiples of four, its levels follow each other in a
 * layer, and with more than one level a layer holds room for the levels
 * the core counts down towards one texel.  Returns EINVAL for a shape
 * outside the core's limits and ENOTSUP for a linear image of more than
 * one level or layer, or a format without texels.
 */
int
powervr_image_lay_out(
	const struct powervr_image_shape *shape,
	const struct powervr_format *format,
	struct powervr_image_layout *layout)
{
	uint64_t level_bytes;
	uint32_t unit;
	uint32_t align;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t largest;
	uint32_t chain;
	uint32_t level;

	/* Edges, levels and layers the core can address. */
	if (shape->width == 0 || shape->height == 0 || shape->depth == 0)
		return EINVAL;
	if (shape->width > POWERVR_IMAGE_EDGE_MAX || shape->height > POWERVR_IMAGE_EDGE_MAX || shape->depth > POWERVR_IMAGE_EDGE_MAX)
		return EINVAL;
	if (shape->layers == 0 || shape->layers > POWERVR_IMAGE_LAYERS_MAX)
		return EINVAL;

	/* No more levels than halvings of the longest edge allow. */
	largest = shape->width;
	if (shape->height > largest)
		largest = shape->height;
	if (shape->depth > largest)
		largest = shape->depth;
	chain = 1;
	while ((largest >> chain) != 0)
		chain++;
	if (shape->levels == 0 || shape->levels > chain || shape->levels > POWERVR_IMAGE_LEVELS_MAX)
		return EINVAL;

	/* A format with texels, and a linear image of one level and one layer. */
	if (format->texel_bytes == 0 || format->sample_code == NO_CODE)
		return ENOTSUP;
	if (shape->linear && (shape->levels != 1 || shape->layers != 1 || shape->volume))
		return ENOTSUP;
	for (level = 0; level < POWERVR_IMAGE_LEVELS_MAX; level++)
		layout->level_offset[level] = 0;

	/*
	 * A linear image's rows: the fewest whole texels that are a multiple
	 * of the 32 bytes of a row and of the pixel back end's two-texel unit
	 * of a line.  Its memory runs on to a whole number of 32-row tiles:
	 * a transfer shades whole tiles, and one was seen on the board reading
	 * 4032 bytes past the end of a 1920x1080 linear image.
	 *
	 * Value referenced: Mesa 26.2.5 src/imagination/csbgen/rogue/
	 * pbestate.xml (the unit of the line stride of the first register
	 * word, two pixels on a core without the one-pixel line feature).
	 */
	if (shape->linear) {
		unit = common_multiple(POWERVR_IMAGE_ROW_ALIGN, 2U * format->texel_bytes);
		layout->row_pitch = round_up(shape->width * format->texel_bytes, unit);
		layout->padded_width = layout->row_pitch / format->texel_bytes;
		layout->padded_height = shape->height;
		layout->padded_depth = 1;
		layout->layer_bytes = (uint64_t)layout->row_pitch * shape->height;
		layout->bytes = (uint64_t)layout->row_pitch * round_up(shape->height, POWERVR_IMAGE_LINEAR_ROWS);
		return 0;
	}

	/*
	 * A twiddled image's edges and levels.
	 *
	 * Procedure referenced: Mesa 26.2.5 src/imagination/vulkan/pvr_image.c
	 * (pvr_image_plane_init_physical_extent and
	 * pvr_image_plane_setup_mip_levels: edges rounded up to powers of
	 * two, a volume's edges aligned to four, and with more than one level
	 * the layer extended down to one texel as the core counts it).
	 */
	align = 1;
	if (shape->volume)
		align = 4;
	layout->padded_width = round_up_power(shape->width);
	layout->padded_height = round_up_power(shape->height);
	layout->padded_depth = round_up_power(shape->depth);
	layout->row_pitch = format->texel_bytes * round_up(layout->padded_width, align);
	width = layout->padded_width;
	height = layout->padded_height;
	depth = layout->padded_depth;
	layout->layer_bytes = 0;
	for (level = 0; level < shape->levels; level++) {
		level_bytes = (uint64_t)format->texel_bytes * round_up(width, align);
		level_bytes *= round_up(height, align);
		level_bytes *= round_up(depth, align);
		layout->level_offset[level] = layout->layer_bytes;
		layout->layer_bytes += level_bytes;
		width = width > 1 ? width >> 1 : 1;
		height = height > 1 ? height >> 1 : 1;
		depth = depth > 1 ? depth >> 1 : 1;
	}

	/* With more than one level, the levels below the image's last, down towards one texel. */
	while (shape->levels > 1 && (width != 1 || height != 1 || depth != 1)) {
		level_bytes = (uint64_t)format->texel_bytes * round_up(width, align);
		level_bytes *= round_up(height, align);
		level_bytes *= round_up(depth, align);
		layout->layer_bytes += level_bytes;
		width = width > 1 ? width >> 1 : 1;
		height = height > 1 ? height >> 1 : 1;
		depth = depth > 1 ? depth >> 1 : 1;
	}

	/* Layers of an array start on the image's boundary. */
	if (shape->layers > 1)
		layout->layer_bytes = (layout->layer_bytes + POWERVR_IMAGE_ALIGN - 1U) & ~(uint64_t)(POWERVR_IMAGE_ALIGN - 1U);
	layout->bytes = layout->layer_bytes * shape->layers;

	/* Succeeded: the image's layout. */
	return 0;
}

/*
 * Lays a multi-planar image of format out in memory: NV12 as a plane of
 * R8 texels of the image's edges, then a plane of R8G8 texels of half
 * its edges, each laid out as a linear image of its own (rows of the
 * same bytes, run on to whole 32-row tiles), the second on the image
 * boundary after the first.  The layout is linear whatever the tiling,
 * so an image made with one usage and its import made with another lie
 * the same way.  layout gets the whole image (its row is plane 0's, its
 * bytes both planes').  Returns ENOTSUP for a format that is not
 * multi-planar or a shape of more than one level, layer or depth, and
 * EINVAL for odd or out-of-range edges.
 */
int
powervr_image_lay_out_planes(
	const struct powervr_image_shape *shape,
	const struct powervr_format *format,
	struct powervr_image_plane planes[POWERVR_IMAGE_PLANES_MAX],
	uint32_t *count,
	struct powervr_image_layout *layout)
{
	struct powervr_image_shape plane_shape;
	uint64_t chroma_offset;
	uint32_t level;
	int error;

	/* NV12, one level and one layer of a flat image. */
	*count = 0;
	if (format == NULL || (format->kind & POWERVR_FORMAT_PLANAR) == 0 || format->vk_format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
		return ENOTSUP;
	if (shape->levels != 1 || shape->layers != 1 || shape->depth != 1 || shape->volume)
		return ENOTSUP;

	/* Even edges, as the half-size chroma plane needs, within the core's reach. */
	if (shape->width == 0 || shape->height == 0)
		return EINVAL;
	if ((shape->width & 1U) != 0 || (shape->height & 1U) != 0)
		return EINVAL;
	if (shape->width > POWERVR_IMAGE_EDGE_MAX || shape->height > POWERVR_IMAGE_EDGE_MAX)
		return EINVAL;

	/* The luma plane: R8 texels of the image's edges, linear. */
	plane_shape.width = shape->width;
	plane_shape.height = shape->height;
	plane_shape.depth = 1;
	plane_shape.levels = 1;
	plane_shape.layers = 1;
	plane_shape.linear = true;
	plane_shape.volume = false;
	planes[0].format = powervr_format_find(VK_FORMAT_R8_UNORM);
	planes[0].width = shape->width;
	planes[0].height = shape->height;
	planes[0].offset = 0;
	error = powervr_image_lay_out(&plane_shape, planes[0].format, &planes[0].layout);
	if (error != 0)
		return error;

	/*
	 * The chroma plane: R8G8 texels (Cb then Cr) of half the edges,
	 * linear, on the image boundary after the luma plane.  Its row is the
	 * luma plane's: the same bytes of an even width, rounded the same way.
	 */
	plane_shape.width = shape->width / 2U;
	plane_shape.height = shape->height / 2U;
	planes[1].format = powervr_format_find(VK_FORMAT_R8G8_UNORM);
	planes[1].width = plane_shape.width;
	planes[1].height = plane_shape.height;
	chroma_offset = (planes[0].layout.bytes + POWERVR_IMAGE_ALIGN - 1U) & ~(uint64_t)(POWERVR_IMAGE_ALIGN - 1U);
	planes[1].offset = chroma_offset;
	error = powervr_image_lay_out(&plane_shape, planes[1].format, &planes[1].layout);
	if (error != 0)
		return error;

	/* The whole image: plane 0's rows, both planes' bytes. */
	for (level = 0; level < POWERVR_IMAGE_LEVELS_MAX; level++)
		layout->level_offset[level] = 0;
	layout->row_pitch = planes[0].layout.row_pitch;
	layout->padded_width = planes[0].layout.padded_width;
	layout->padded_height = shape->height;
	layout->padded_depth = 1;
	layout->bytes = chroma_offset + planes[1].layout.bytes;
	layout->layer_bytes = layout->bytes;
	*count = 2;

	/* Succeeded: the image's planes and its whole layout. */
	return 0;
}

/*
 * Builds the texture unit's two state words of a view.
 *
 * A twiddled image is a two-dimensional texture whose words name the
 * view's first level and level count; a linear one is a strided texture
 * whose second word holds its row in texels.  Returns EINVAL for a view
 * the words cannot hold: an address off the four-byte boundary or past
 * forty bits, or a format the texture unit does not read.
 */
int
powervr_view_words(
	const struct powervr_view_shape *view,
	uint64_t words[2])
{
	const struct powervr_format *format;
	uint64_t address;
	uint64_t first;
	uint64_t second;
	unsigned output;

	/* A format the unit reads, at an address the words hold. */
	format = view->format;
	if (format->sample_code == NO_CODE)
		return EINVAL;
	address = view->address + (uint64_t)view->base_layer * view->layout->layer_bytes;
	if ((address & 3U) != 0 || (address >> 40) != 0)
		return EINVAL;
	if (view->level_count == 0 || view->level_count > POWERVR_IMAGE_LEVELS_MAX || view->base_level >= POWERVR_IMAGE_LEVELS_MAX)
		return EINVAL;
	if (view->layer_count == 0 || view->layer_count > POWERVR_IMAGE_LAYERS_MAX)
		return EINVAL;
	if (view->linear && view->layout->padded_width > 32768U)
		return EINVAL;

	/*
	 * The first word: the kind of image, its gamma, the four channel
	 * selections, the format and the edges less one.
	 *
	 * Values referenced: Mesa 26.2.5 src/imagination/csbgen/rogue/
	 * texstate.xml (IMAGE_WORD0: textype 0-2, gamma 3, swiz3 5-7, swiz2
	 * 8-10, swiz1 11-13, swiz0 14-16, texformat 27-33, width 34-47,
	 * height 48-61; TEXTYPE 2D 1 and STRIDE 4; GAMMA ON 1).
	 */
	first = place(view->linear ? VIEW_KIND_STRIDED : VIEW_KIND_TWIDDLED_2D, 0, 2);
	if ((format->kind & POWERVR_FORMAT_SRGB) != 0)
		first |= place(1, 3, 3);
	for (output = 0; output < 4; output++)
		first |= place(channel_select(format, view->mapping[output], output), 14U - 3U * output, 16U - 3U * output);
	first |= place(format->sample_code, 27, 33);
	first |= place(view->width - 1U, 34, 47);
	first |= place(view->height - 1U, 48, 61);

	/*
	 * The second word of a linear image: its level count, the address in
	 * four-byte units and its row in texels less one.
	 *
	 * Values referenced: Mesa 26.2.5 src/imagination/csbgen/rogue/
	 * texstate.xml (STRIDE_IMAGE_WORD1: stride 0-14, mipmaps_present 15,
	 * texaddr 16-53 shifted by 2, num_mip_levels 60-63).
	 */
	if (view->linear) {
		second = place(view->layout->padded_width - 1U, 0, 14);
		second |= place(address >> 2, 16, 53);
		second |= place(view->level_count, 60, 63);
		words[0] = first;
		words[1] = second;
		return 0;
	}

	/*
	 * The second word of a twiddled image: its level count, the layers
	 * less one, whether it has levels, the address in four-byte units and
	 * the first level.
	 *
	 * Values referenced: Mesa 26.2.5 src/imagination/csbgen/rogue/
	 * texstate.xml (IMAGE_WORD1: num_mip_levels 0-3, depth 4-14,
	 * mipmaps_present 15, texaddr 16-53 shifted by 2, baselevel 60-63).
	 */
	second = place(view->level_count, 0, 3);
	second |= place(view->layer_count - 1U, 4, 14);
	if (view->image_levels > 1)
		second |= place(1, 15, 15);
	second |= place(address >> 2, 16, 53);
	second |= place(view->base_level, 60, 63);
	words[0] = first;
	words[1] = second;

	/* Succeeded: the view's words. */
	return 0;
}

/*
 * Builds the texture unit's two state words of a sampler.
 *
 * Every sampler uses the first border colour, transparent black, and
 * neither anisotropy nor comparison.  Returns ENOTSUP for a filter or an
 * address mode the executor does not take.
 */
int
powervr_sampler_words(
	const struct powervr_sampler_shape *sampler,
	uint64_t words[2])
{
	uint32_t wrap_u;
	uint32_t wrap_v;
	uint32_t wrap_w;
	uint32_t mag;
	uint32_t min;
	int32_t bias;
	int32_t lod_low;
	int32_t lod_high;
	int error;

	/* Nearest or linear filters. */
	if (sampler->mag_filter > VK_FILTER_LINEAR || sampler->min_filter > VK_FILTER_LINEAR)
		return ENOTSUP;
	if (sampler->mipmap_mode > VK_SAMPLER_MIPMAP_MODE_LINEAR)
		return ENOTSUP;
	mag = sampler->mag_filter == VK_FILTER_LINEAR ? FILTER_LINEAR : FILTER_POINT;
	min = sampler->min_filter == VK_FILTER_LINEAR ? FILTER_LINEAR : FILTER_POINT;

	/* The address mode of each coordinate. */
	error = sampler_wrap(sampler->address_u, &wrap_u);
	if (error != 0)
		return error;
	error = sampler_wrap(sampler->address_v, &wrap_v);
	if (error != 0)
		return error;
	error = sampler_wrap(sampler->address_w, &wrap_w);
	if (error != 0)
		return error;

	/*
	 * The bias and the level clamps in the unit's fixed point.
	 *
	 * Values referenced: Mesa 26.2.5 src/imagination/csbgen/rogue/
	 * texstate.xml (enum DADJUST: zero 4095, largest 8191, eight fraction
	 * bits; enum CLAMP: largest 959, six fraction bits).
	 */
	bias = fixed_from_bits(sampler->lod_bias_bits, BIAS_FRACTION_BITS, -BIAS_ZERO, BIAS_LARGEST - BIAS_ZERO);
	lod_low = fixed_from_bits(sampler->min_lod_bits, LOD_FRACTION_BITS, 0, LOD_LARGEST);
	lod_high = fixed_from_bits(sampler->max_lod_bits, LOD_FRACTION_BITS, 0, LOD_LARGEST);

	/*
	 * The first word; the second is all zero (no plane addresses, cube
	 * edges filtered).
	 *
	 * Values referenced: Mesa 26.2.5 src/imagination/csbgen/rogue/
	 * texstate.xml (SAMPLER_WORD0: dadjust 0-12, minlod 13-22, maxlod
	 * 23-32, anisoctl 33-35, magfilter 36-37, minfilter 38-39, mipfilter
	 * 40, addrmode_u 41-43, addrmode_v 44-46, non_normalized_coords 49,
	 * bordercolor_index 50-55, addrmode_w 56-58).
	 */
	words[0] = place((uint32_t)(bias + BIAS_ZERO), 0, 12);
	words[0] |= place((uint32_t)lod_low, 13, 22);
	words[0] |= place((uint32_t)lod_high, 23, 32);
	words[0] |= place(mag, 36, 37);
	words[0] |= place(min, 38, 39);
	if (sampler->mipmap_mode == VK_SAMPLER_MIPMAP_MODE_LINEAR)
		words[0] |= place(1, 40, 40);
	words[0] |= place(wrap_u, 41, 43);
	words[0] |= place(wrap_v, 44, 46);
	if (sampler->unnormalized)
		words[0] |= place(1, 49, 49);
	words[0] |= place(wrap_w, 56, 58);
	words[1] = 0;

	/* Succeeded: the sampler's words. */
	return 0;
}

/*
 * Reports the bytes one descriptor of a type occupies in a set, or 0 for
 * a type the executor does not take.
 *
 * A buffer is its address, size and offset; an image is its two state
 * words and four words the shader reads; a sampler is its two words,
 * four words the shader reads and the two words of its gather form; a
 * combined image and sampler is the image followed by the sampler.
 *
 * Values referenced: Mesa 26.2.5 src/imagination/vulkan/pvr_common.h (the
 * sizes of the buffer, image, sampler and combined descriptors) and
 * src/imagination/pco/pco_data.h (the counts of the image and sampler
 * meta words).
 */
uint32_t
powervr_descriptor_bytes(
	uint32_t type)
{
	/* The size of each kind. */
	switch (type) {
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
		return DESCRIPTOR_BUFFER_BYTES;
	case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		return DESCRIPTOR_IMAGE_BYTES;
	case VK_DESCRIPTOR_TYPE_SAMPLER:
		return DESCRIPTOR_SAMPLER_BYTES;
	case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
		return DESCRIPTOR_IMAGE_BYTES + DESCRIPTOR_SAMPLER_BYTES;
	default:
		break;
	}

	/* Not a type the executor takes. */
	return 0;
}

/*
 * Reports how many words powervr_format_clear_words writes for a format
 * (the pixel output registers a clear of it fills): one for four 8-bit
 * unorm channels and the one-channel formats of up to 32 bits, two for
 * the half-float packs of more channels and R32G32_SFLOAT, four for
 * R32G32B32A32_SFLOAT; 0 for a format it does not take.
 */
uint32_t
powervr_format_clear_word_count(
	const struct powervr_format *format)
{
	uint32_t packed[4];
	uint32_t zero[4];
	int error;

	/* A format the pack does not take has none. */
	zero[0] = 0;
	zero[1] = 0;
	zero[2] = 0;
	zero[3] = 0;
	error = powervr_format_clear_words(format, zero, packed);
	if (error != 0)
		return 0;

	/* The 32-bit float formats: a word a channel. */
	if (format->vk_format == VK_FORMAT_R32G32_SFLOAT)
		return 2;
	if (format->vk_format == VK_FORMAT_R32G32B32A32_SFLOAT)
		return 4;

	/* The half-float packs of three or four channels. */
	if ((format->kind & POWERVR_FORMAT_SRGB) != 0 || format->vk_format == VK_FORMAT_R16G16B16A16_SFLOAT)
		return 2;

	/* Succeeded: one word. */
	return 1;
}

/*
 * Packs a clear colour (four float bit patterns) as the words a clear
 * writes for the format, in channel order: a format of four 8-bit unorm
 * channels as one word, red in the low byte, each channel rounded to the
 * nearest of 255 steps and clamped; the one- and two-channel 8-bit unorm
 * formats, the sRGB ones and the 16-bit float one as half floats, two a
 * word; the 32-bit float ones as their bits, a word a channel.  Other
 * formats are not taken (ENOTSUP).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_transfer.c
 * (pvr_pack_clear_color: PACKMODE_U8U8U8U8 with normalised channels,
 * pvr_float_to_ufixed: NaN, infinity and 1.0 or more to 255, below 0 to
 * 0, else floor(value * 255 + 0.5)).
 */
int
powervr_format_clear_words(
	const struct powervr_format *format,
	const uint32_t colour[4],
	uint32_t packed[4])
{
	unsigned channel;

	/* Refuses missing input or result. */
	if (format == NULL || colour == NULL || packed == NULL)
		return EINVAL;

	/* 32-bit float channels: the raw bits, one word a channel. */
	switch (format->vk_format) {
	case VK_FORMAT_R32_SFLOAT:
	case VK_FORMAT_R32G32_SFLOAT:
	case VK_FORMAT_R32G32B32A32_SFLOAT:
		for (channel = 0; channel < 4; channel++)
			packed[channel] = channel < format->texel_bytes / 4U ? colour[channel] : 0;
		return 0;

	/*
	 * The one- and two-channel 8-bit unorm formats, the sRGB ones and
	 * the 16-bit float one: half floats, two a word.
	 */
	case VK_FORMAT_R8_UNORM:
	case VK_FORMAT_R8G8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_B8G8R8A8_SRGB:
	case VK_FORMAT_R16G16B16A16_SFLOAT:
		packed[0] = half_from_float(colour[0]) | (half_from_float(colour[1]) << 16);
		packed[1] = half_from_float(colour[2]) | (half_from_float(colour[3]) << 16);
		if (format->vk_format == VK_FORMAT_R8_UNORM)
			packed[0] &= 0xFFFFU;
		if (format->texel_bytes <= 2U)
			packed[1] = 0;
		packed[2] = 0;
		packed[3] = 0;
		return 0;
	default:
		break;
	}

	/* Otherwise takes the four-channel 8-bit unorm formats only. */
	if (format->store_code != 0 || format->texel_bytes != 4 || (format->kind & POWERVR_FORMAT_SRGB) != 0)
		return ENOTSUP;

	/* The four bytes in channel order, the other words zero. */
	packed[0] = 0;
	for (channel = 0; channel < 4; channel++)
		packed[0] |= unorm8_from_float(colour[channel]) << (channel * 8U);
	packed[1] = 0;
	packed[2] = 0;
	packed[3] = 0;

	/* Succeeded: the packed colour. */
	return 0;
}

/* Puts value in the bits low to high of a word; bits of value beyond them are dropped. */
static uint64_t
place(
	uint64_t value,
	unsigned low,
	unsigned high)
{
	uint64_t mask;

	/* The field's mask, below its place. */
	mask = ~(uint64_t)0 >> (63U - (high - low));

	/* Succeeded: the value at its place. */
	return (value & mask) << low;
}

/* Reports the smallest power of two not below value (value at most 2^31). */
static uint32_t
round_up_power(
	uint32_t value)
{
	uint32_t power;

	/* Doubles from one until value fits. */
	power = 1;
	while (power < value)
		power <<= 1;

	/* Succeeded: the power. */
	return power;
}

/* Reports the smallest multiple of unit not below value. */
static uint32_t
round_up(
	uint32_t value,
	uint32_t unit)
{
	/* The next whole number of units. */
	return (value + unit - 1U) / unit * unit;
}

/* Reports the least common multiple of a and b (both nonzero). */
static uint32_t
common_multiple(
	uint32_t a,
	uint32_t b)
{
	uint32_t x;
	uint32_t y;
	uint32_t rest;

	/* The greatest common divisor, by Euclid. */
	x = a;
	y = b;
	while (y != 0) {
		rest = x % y;
		x = y;
		y = rest;
	}

	/* Succeeded: the multiple. */
	return a / x * b;
}

/*
 * Reports the texture unit's selection for one output of a view: the
 * client's mapping (VkComponentSwizzle) applied over the format's own
 * channels.  A channel the format lacks reads as zero, or as one for
 * alpha.
 *
 * Procedure referenced: Mesa 26.2.5 src/imagination/vulkan/
 * pvr_arch_tex_state.c (pvr_get_hw_swizzle: an absent channel is zero,
 * alpha one).  Values referenced: Mesa 26.2.5 src/imagination/csbgen/
 * rogue/texstate.xml (enum SWIZ: source channels 0-3, one 4, zero 5).
 */
static uint32_t
channel_select(
	const struct powervr_format *format,
	uint32_t mapping,
	unsigned output)
{
	uint8_t source;

	/* The format's channel the mapping picks, or a constant. */
	if (mapping == VK_COMPONENT_SWIZZLE_ZERO)
		return SELECT_ZERO;
	if (mapping == VK_COMPONENT_SWIZZLE_ONE)
		return SELECT_ONE;
	source = format->channels[output];
	if (mapping >= VK_COMPONENT_SWIZZLE_R && mapping <= VK_COMPONENT_SWIZZLE_A)
		source = format->channels[mapping - VK_COMPONENT_SWIZZLE_R];

	/* The unit's code of that source. */
	if (source == POWERVR_CHANNEL_ZERO)
		return SELECT_ZERO;
	if (source == POWERVR_CHANNEL_ONE)
		return SELECT_ONE;
	if (source == POWERVR_CHANNEL_ABSENT) {
		if (output == 3)
			return SELECT_ONE;
		return SELECT_ZERO;
	}

	/* Succeeded: a source channel. */
	return source;
}

/*
 * Reports the texture unit's address mode of a Vulkan one; ENOTSUP for
 * the mirrored clamp, which the executor does not take.
 *
 * Values referenced: Mesa 26.2.5 src/imagination/csbgen/rogue/
 * texstate.xml (enum ADDRMODE: REPEAT 0, FLIP 1, CLAMP_TO_EDGE 2,
 * CLAMP_TO_BORDER 4), matched to the Vulkan modes as in
 * src/imagination/vulkan/pvr_arch_sampler.c
 * (pvr_sampler_get_hw_addr_mode_from_vk).
 */
static int
sampler_wrap(
	uint32_t mode,
	uint32_t *wrap)
{
	/* The core's mode of each Vulkan one. */
	switch (mode) {
	case VK_SAMPLER_ADDRESS_MODE_REPEAT:
		*wrap = WRAP_REPEAT;
		return 0;
	case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
		*wrap = WRAP_MIRROR;
		return 0;
	case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:
		*wrap = WRAP_CLAMP_EDGE;
		return 0;
	case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
		*wrap = WRAP_CLAMP_BORDER;
		return 0;
	default:
		break;
	}

	/* A mode the executor does not take. */
	return ENOTSUP;
}

/*
 * Reports a single-precision float, given as its bits, in fixed point of
 * fraction bits, cut towards zero and clamped to smallest and largest.
 * A NaN reads as zero.
 */
static int32_t
fixed_from_bits(
	uint32_t bits,
	unsigned fraction,
	int32_t smallest,
	int32_t largest)
{
	uint64_t magnitude;
	uint32_t exponent;
	uint32_t mantissa;
	int64_t value;
	int shift;

	/* The parts of the float. */
	exponent = (bits >> 23) & 0xFFU;
	mantissa = bits & 0x7FFFFFU;

	/* The magnitude scaled by the fraction: zero, saturated, or the mantissa shifted. */
	magnitude = 0;
	if (exponent == 0xFFU && mantissa != 0) {
		magnitude = 0;
	} else if (exponent == 0xFFU) {
		magnitude = (uint64_t)1 << 40;
	} else if (exponent != 0) {
		mantissa |= 0x800000U;
		shift = (int)exponent - 150 + (int)fraction;
		if (shift > 16) {
			magnitude = (uint64_t)1 << 40;
		} else if (shift >= 0) {
			magnitude = (uint64_t)mantissa << shift;
		} else if (shift > -32) {
			magnitude = mantissa >> -shift;
		}
	}

	/* The sign, then the clamp. */
	value = (int64_t)magnitude;
	if ((bits >> 31) != 0)
		value = -value;
	if (value < smallest)
		value = smallest;
	if (value > largest)
		value = largest;

	/* Succeeded: the fixed-point value. */
	return (int32_t)value;
}

/*
 * Converts a float, as its bits, to a half float without float
 * arithmetic (the kernel is soft float): clamped to the largest half,
 * denormalised below the smallest normal, rounded half up.
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_transfer.c pvr_float_to_f16
 * (round_to_even false), used by pvr_pack_clear_color.
 */
static uint32_t
half_from_float(
	uint32_t bits)
{
	uint32_t sign;
	uint32_t mantissa;
	int32_t exponent;
	uint32_t result;

	/* The sign apart; zero (either sign) is zero. */
	sign = (bits >> 16) & 0x8000U;
	if ((bits & 0x7FFFFFFFU) == 0)
		return 0;

	/* Clamps to 131008, the largest half (and infinities and NaNs). */
	if ((bits & 0x7FFFFFFFU) > 0x47FFE000U)
		bits = (bits & 0x80000000U) | 0x47FFE000U;

	/* The half's exponent and the mantissa with its hidden bit. */
	exponent = (int32_t)((bits >> 23) & 0xFFU) - 127 + 15;
	mantissa = (bits & 0x7FFFFFU) | 0x800000U;

	/* Below the smallest normal: shifts the mantissa down, exponent 0. */
	if (exponent <= 0) {
		if (1 - exponent >= 24)
			mantissa = 0;
		else
			mantissa >>= (uint32_t)(1 - exponent);
		exponent = 0;
	} else {
		mantissa &= 0x7FFFFFU;
	}

	/* Rounds up on the first dropped bit. */
	result = ((uint32_t)exponent << 10) + (mantissa >> 13);
	if ((mantissa & 0x1000U) != 0)
		result++;

	/* Succeeded: the half with its sign. */
	return sign | result;
}

/*
 * Converts a float's bits to an 8-bit unorm channel: NaN, infinity and
 * 1.0 or more to 255, below zero to 0, else floor(value * 255 + 0.5)
 * computed exactly.
 */
static uint32_t
unorm8_from_float(
	uint32_t bits)
{
	uint64_t mantissa;
	uint32_t exponent;
	uint32_t shift;

	/* NaN, infinity and 1.0 or more (a positive exponent of 127 or above). */
	exponent = (bits >> 23) & 0xffU;
	if (exponent == 0xffU)
		return 255;
	if ((bits & 0x80000000U) != 0)
		return 0;
	if (exponent >= 127U)
		return 255;

	/* Denormals and values below 2^-40 round to zero. */
	shift = 150U - exponent;
	if (exponent == 0 || shift > 60U)
		return 0;

	/* floor((mantissa * 255) / 2^shift + 0.5). */
	mantissa = (uint64_t)((bits & 0x7fffffU) | 0x800000U) * 255U;
	return (uint32_t)((mantissa + (1ULL << (shift - 1U))) >> shift);
}
