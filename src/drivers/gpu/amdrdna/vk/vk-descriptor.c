/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The descriptors a draw's resource table holds for what it reads
 * (amdrdna-vk-exec.h): the image descriptor of a linear image of four,
 * two or one bytes a texel, read as 2D, as layers of 2D, as a cube or as
 * 3D (a plane of a two-plane image is read as an image of its own), the
 * image descriptor of the depth or the stencil of a depth image in the
 * depth unit's swizzle, the image descriptor of a storage image, the
 * sampler descriptor of a VkSampler, and the formats of the vertex
 * buffers and the texel buffers.  The buffer descriptors themselves are
 * the compiler's (gfx10-program.h).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/vk/amdrdna-vk-exec.h"

/*
 * The image formats a descriptor reads: four 8-bit unorm channels, and the
 * same read as sRGB.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10-rsrc.json (the
 * enum GFX10_FORMAT: GFX10_FORMAT_8_8_8_8_UNORM 56,
 * GFX10_FORMAT_8_8_8_8_SRGB 130).
 */
#define DESCRIPTOR_FORMAT_UNORM		56U
#define DESCRIPTOR_FORMAT_SRGB		130U

/*
 * The image formats of one and of two 8-bit unorm channels (an image of
 * one or two bytes a texel, and the planes of a two-plane image).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10-rsrc.json (the
 * enum GFX10_FORMAT: GFX10_FORMAT_8_UNORM 1, GFX10_FORMAT_8_8_UNORM 14).
 */
#define DESCRIPTOR_FORMAT_UNORM_1	1U
#define DESCRIPTOR_FORMAT_UNORM_2	14U

/*
 * The image formats a depth image's aspects are read in: a 16-bit
 * normalized depth, a 32-bit float depth and the 8-bit stencil as the
 * shaders sample them, or the bits of each as an unsigned integer of their
 * size (a copy reads them so, unchanged).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10-rsrc.json (the
 * enum GFX10_FORMAT: GFX10_FORMAT_8_UINT 5, GFX10_FORMAT_16_UNORM 7,
 * GFX10_FORMAT_16_UINT 11, GFX10_FORMAT_32_UINT 20, GFX10_FORMAT_32_FLOAT
 * 22) and src/amd/common/ac_formats.c (ac_get_gfx10_format_table: D16_UNORM
 * read as 16_UNORM, D32_SFLOAT as 32_FLOAT, S8_UINT as 8_UINT).
 */
#define DESCRIPTOR_FORMAT_DEPTH_16	7U
#define DESCRIPTOR_FORMAT_DEPTH_32	22U
#define DESCRIPTOR_FORMAT_STENCIL	5U
#define DESCRIPTOR_FORMAT_BITS_8	5U
#define DESCRIPTOR_FORMAT_BITS_16	11U
#define DESCRIPTOR_FORMAT_BITS_32	20U

/*
 * One storage format: its VkFormat, the image and buffer descriptor format
 * (the two share the GFX10 numbering), its channels and its bytes a texel.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10-rsrc.json (the
 * enum GFX10_FORMAT: 32_UINT 20, 32_SINT 21, 32_FLOAT 22, 8_8_8_8_UNORM
 * 56, 8_8_8_8_SNORM 57, 8_8_8_8_UINT 60, 8_8_8_8_SINT 61, 32_32_UINT 62,
 * 32_32_SINT 63, 32_32_FLOAT 64, 16_16_16_16_UINT 69, 16_16_16_16_SINT
 * 70, 16_16_16_16_FLOAT 71, 32_32_32_32_UINT 75, 32_32_32_32_SINT 76,
 * 32_32_32_32_FLOAT 77) and src/amd/common/ac_formats.c
 * (ac_get_gfx10_format_table: the formats of those VkFormats).
 */
struct descriptor_storage_format {
	uint32_t format;
	uint32_t code;
	uint32_t channels;
	uint32_t bytes;
};

static const struct descriptor_storage_format descriptor_storage_formats[] = {
	{ VK_FORMAT_R32_UINT, 20U, 1U, 4U },
	{ VK_FORMAT_R32_SINT, 21U, 1U, 4U },
	{ VK_FORMAT_R32_SFLOAT, 22U, 1U, 4U },
	{ VK_FORMAT_R8G8B8A8_UNORM, 56U, 4U, 4U },
	{ VK_FORMAT_R8G8B8A8_SNORM, 57U, 4U, 4U },
	{ VK_FORMAT_R8G8B8A8_UINT, 60U, 4U, 4U },
	{ VK_FORMAT_R8G8B8A8_SINT, 61U, 4U, 4U },
	{ VK_FORMAT_R32G32_UINT, 62U, 2U, 8U },
	{ VK_FORMAT_R32G32_SINT, 63U, 2U, 8U },
	{ VK_FORMAT_R32G32_SFLOAT, 64U, 2U, 8U },
	{ VK_FORMAT_R16G16B16A16_UINT, 69U, 4U, 8U },
	{ VK_FORMAT_R16G16B16A16_SINT, 70U, 4U, 8U },
	{ VK_FORMAT_R16G16B16A16_SFLOAT, 71U, 4U, 8U },
	{ VK_FORMAT_R32G32B32A32_UINT, 75U, 4U, 16U },
	{ VK_FORMAT_R32G32B32A32_SINT, 76U, 4U, 16U },
	{ VK_FORMAT_R32G32B32A32_SFLOAT, 77U, 4U, 16U },
};

/* The channels of the four-byte formats, which have all of red, green, blue and alpha. */
#define DESCRIPTOR_CHANNELS_ALL		4U

/*
 * The image descriptor's fields: word 1 the address's bits 47:40 (7:0),
 * the format (28:20) and the width less one's low two bits (31:30); word 2
 * the rest of the width less one (11:0), the height less one (27:14) and
 * the resource level (31); word 3 the channel selects (3 bits each from
 * 0), the border colour swizzle (27:25) and the type (31:28); word 5 the
 * performance modulation (22:20).  The base and last level, the swizzle
 * mode (linear), the depth and the first layer are 0 (the levels and the
 * slices are set after).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10-rsrc.json
 * (SQ_IMG_RSRC_WORD1: BASE_ADDRESS_HI, FORMAT, WIDTH_LO;
 * SQ_IMG_RSRC_WORD2: WIDTH_HI, HEIGHT, RESOURCE_LEVEL; SQ_IMG_RSRC_WORD3:
 * DST_SEL_X .. W, BASE_LEVEL, LAST_LEVEL, SW_MODE, BC_SWIZZLE, TYPE; the
 * enum SQ_IMG_RSRC_WORD3__BC_SWIZZLE: BC_SWIZZLE_XYZW 0, BC_SWIZZLE_ZYXW
 * 4; SQ_IMG_RSRC_WORD5: PERF_MOD), src/amd/registers/gfx9.json (the
 * enums SQ_RSRC_IMG_TYPE: SQ_RSRC_IMG_2D 9; SQ_SEL_XYZW01: SQ_SEL_0 0,
 * SQ_SEL_1 1, SQ_SEL_X 4 .. SQ_SEL_W 7) and src/amd/common/ac_descriptors.c
 * (ac_build_gfx10_texture_descriptor: RESOURCE_LEVEL 1 below GFX11,
 * PERF_MOD 4, word 0 the address from bit 8).
 */
#define DESCRIPTOR_ADDRESS_SHIFT	8U
#define DESCRIPTOR_ADDRESS_HIGH_SHIFT	40U
#define DESCRIPTOR_FORMAT_SHIFT		20U
#define DESCRIPTOR_WIDTH_LOW_SHIFT	30U
#define DESCRIPTOR_HEIGHT_SHIFT		14U
#define DESCRIPTOR_RESOURCE_LEVEL	(1U << 31)
#define DESCRIPTOR_SELECT_BITS		3U
#define DESCRIPTOR_SELECT_ZERO		0U
#define DESCRIPTOR_SELECT_ONE		1U
#define DESCRIPTOR_SELECT_X		4U
#define DESCRIPTOR_BORDER_SWIZZLE_SHIFT	25U
#define DESCRIPTOR_BORDER_SWIZZLE_ZYXW	4U
#define DESCRIPTOR_TYPE_2D		(9U << 28)
#define DESCRIPTOR_PERFORMANCE		(4U << 20)

/*
 * The levels of an image of several: word 3 the first and the last level
 * read (15:12, 19:16), word 5 the image's last level (7:4), which places
 * the levels of the linear mip chain from the descriptor's address.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10-rsrc.json
 * (SQ_IMG_RSRC_WORD3: BASE_LEVEL, LAST_LEVEL; SQ_IMG_RSRC_WORD5: MAX_MIP)
 * and src/amd/common/ac_descriptors.c (ac_build_gfx10_texture_descriptor:
 * MAX_MIP the resource's levels less one).
 */
#define DESCRIPTOR_BASE_LEVEL_SHIFT	12U
#define DESCRIPTOR_LAST_LEVEL_SHIFT	16U
#define DESCRIPTOR_MAX_LEVEL_SHIFT	4U
#define DESCRIPTOR_LEVEL_MASK		0xfU

/*
 * The image types a view reads (word 3, 31:28) and the last slice it
 * reads (word 4, 12:0): a 3D image's depth less one, or the last of the
 * layers of an array of 2D images or of a cube's six faces, counted from
 * the first the descriptor's address names; the first layer (word 4,
 * 28:16) is 0.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx9.json (the enum
 * SQ_RSRC_IMG_TYPE: SQ_RSRC_IMG_2D 9, SQ_RSRC_IMG_3D 10,
 * SQ_RSRC_IMG_CUBE 11, SQ_RSRC_IMG_2D_ARRAY 13),
 * src/amd/registers/gfx10-rsrc.json (SQ_IMG_RSRC_WORD4: DEPTH,
 * BASE_ARRAY) and src/amd/common/ac_descriptors.c
 * (ac_build_gfx10_texture_descriptor: DEPTH the last accessible layer on
 * GFX9 and later, a 3D image's depth less one).
 */
#define DESCRIPTOR_TYPE_MASK		(0xfU << 28)
#define DESCRIPTOR_TYPE_3D		(10U << 28)
#define DESCRIPTOR_TYPE_CUBE		(11U << 28)
#define DESCRIPTOR_TYPE_2D_ARRAY	(13U << 28)
#define DESCRIPTOR_DEPTH_MASK		0x1fffU

/*
 * An image of samples: the types of one (14) and of an array (15); its
 * render swizzle (SW_64KB_R_X 27 in word 3's 24:20); its samples' power
 * as the last level and the image's last level (the base level 0); the
 * first layer read (word 4, 28:16) from the image's start, the last in
 * DEPTH.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx9.json (the enum
 * SQ_RSRC_IMG_TYPE: SQ_RSRC_IMG_2D_MSAA 14, SQ_RSRC_IMG_2D_MSAA_ARRAY 15),
 * src/amd/registers/gfx10-rsrc.json (SQ_IMG_RSRC_WORD3: SW_MODE;
 * SQ_IMG_RSRC_WORD4: BASE_ARRAY), src/amd/addrlib/inc/addrtypes.h
 * (ADDR_SW_64KB_R_X 27) and src/amd/common/ac_descriptors.c
 * (ac_build_gfx10_texture_descriptor: BASE_LEVEL 0, LAST_LEVEL and
 * MAX_MIP the log2 of the samples, BASE_ARRAY the first layer, DEPTH the
 * last).
 */
#define DESCRIPTOR_TYPE_2D_MSAA		(14U << 28)
#define DESCRIPTOR_TYPE_2D_MSAA_ARRAY	(15U << 28)
#define DESCRIPTOR_SWIZZLE_RENDER	(27U << 20)
#define DESCRIPTOR_BASE_ARRAY_SHIFT	16U

/*
 * A depth image's surface: the depth unit's swizzle (SW_64KB_Z_X 24 in
 * word 3's 24:20), which the texture unit reads as the depth unit wrote
 * it; the field's mask.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10-rsrc.json
 * (SQ_IMG_RSRC_WORD3: SW_MODE 24:20), src/amd/addrlib/inc/addrtypes.h
 * (ADDR_SW_64KB_Z_X 24) and src/amd/common/ac_descriptors.c
 * (ac_build_gfx10_texture_descriptor: SW_MODE the surface's swizzle mode,
 * a depth surface's and a stencil plane's alike).
 */
#define DESCRIPTOR_SWIZZLE_DEPTH	(24U << 20)
#define DESCRIPTOR_SWIZZLE_MASK		(0x1fU << 20)

/*
 * The sampler descriptor's fields: word 0 the clamp of x, y and z (2:0,
 * 5:3, 8:6), the depth compare function (14:12), the unnormalized
 * coordinates (15) and the coordinate truncation (27); word 1 the minimum
 * and maximum level of detail (11:0, 23:12, unsigned 4.8); word 2 the
 * magnifying and minifying filters (21:20, 23:22) and the mipmap filter
 * (27:26); word 3 the border colour type (31:30).
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx10-rsrc.json
 * (SQ_IMG_SAMP_WORD0: CLAMP_X, CLAMP_Y, CLAMP_Z, DEPTH_COMPARE_FUNC,
 * FORCE_UNNORMALIZED, TRUNC_COORD; SQ_IMG_SAMP_WORD1: MIN_LOD, MAX_LOD;
 * SQ_IMG_SAMP_WORD2: XY_MAG_FILTER, XY_MIN_FILTER, MIP_FILTER;
 * SQ_IMG_SAMP_WORD3: BORDER_COLOR_TYPE) and src/amd/common/ac_descriptors.c
 * (ac_build_sampler_descriptor: the levels of detail clamped to 0 .. 15
 * in 4.8).
 */
#define SAMPLER_CLAMP_Y_SHIFT		3U
#define SAMPLER_CLAMP_Z_SHIFT		6U
#define SAMPLER_COMPARE_SHIFT		12U
#define SAMPLER_UNNORMALIZED		(1U << 15)
#define SAMPLER_TRUNCATE		(1U << 27)
#define SAMPLER_MAX_LOD			((15U << 8) << 12)
#define SAMPLER_MAG_SHIFT		20U
#define SAMPLER_MIN_SHIFT		22U
#define SAMPLER_MIP_SHIFT		26U
#define SAMPLER_BORDER_SHIFT		30U

/*
 * The clamps, the filters, the mipmap filters, the border colours and the
 * compare functions, as VkSampler's are set.
 *
 * Values referenced: Mesa 25.0.7 src/amd/registers/gfx9.json (the enums
 * SQ_TEX_CLAMP: SQ_TEX_WRAP 0, SQ_TEX_MIRROR 1, SQ_TEX_CLAMP_LAST_TEXEL 2,
 * SQ_TEX_MIRROR_ONCE_LAST_TEXEL 3, SQ_TEX_CLAMP_BORDER 6;
 * SQ_TEX_XY_FILTER: SQ_TEX_XY_FILTER_POINT 0, SQ_TEX_XY_FILTER_BILINEAR 1;
 * SQ_TEX_MIP_FILTER: SQ_TEX_MIP_FILTER_POINT 1, SQ_TEX_MIP_FILTER_LINEAR 2;
 * SQ_TEX_BORDER_COLOR: SQ_TEX_BORDER_COLOR_TRANS_BLACK 0,
 * SQ_TEX_BORDER_COLOR_OPAQUE_BLACK 1, SQ_TEX_BORDER_COLOR_OPAQUE_WHITE 2;
 * SQ_TEX_DEPTH_COMPARE: NEVER 0 .. ALWAYS 7 in VkCompareOp's order)
 * and src/amd/vulkan/radv_sampler.c (radv_tex_wrap, radv_tex_filter,
 * radv_tex_mipfilter, radv_tex_bordercolor, radv_tex_compare: which
 * Vulkan value maps to which; radv_init_sampler: truncation for a sampler
 * nearest both ways).
 */
#define SAMPLER_WRAP			0U
#define SAMPLER_MIRROR			1U
#define SAMPLER_CLAMP_EDGE		2U
#define SAMPLER_MIRROR_ONCE_EDGE	3U
#define SAMPLER_CLAMP_BORDER		6U
#define SAMPLER_POINT			0U
#define SAMPLER_BILINEAR		1U
#define SAMPLER_MIP_POINT		1U
#define SAMPLER_MIP_LINEAR		2U
#define SAMPLER_BORDER_TRANSPARENT	0U
#define SAMPLER_BORDER_BLACK		1U
#define SAMPLER_BORDER_WHITE		2U
#define SAMPLER_COMPARE_MASK		7U

static void descriptor_fill(uint64_t address, uint32_t width, uint32_t height, uint32_t code, bool alternate, uint32_t channels, const uint32_t *swizzle, uint32_t *words);
static uint32_t descriptor_select(uint32_t swizzle, uint32_t channel, bool alternate, uint32_t channels);
static uint32_t sampler_clamp(uint32_t mode);
static uint32_t sampler_filter(uint32_t filter);
static uint32_t sampler_border(uint32_t colour);
static const struct descriptor_storage_format *descriptor_storage(uint32_t format);

/*
 * Reports the descriptor format of an image format, and whether its
 * channels lie blue first in memory (the descriptor's channel selects then
 * swap red and blue).
 */
bool
amdrdna_vk_image_format(
	uint32_t format,
	uint32_t *code,
	bool *alternate)
{
	/* The four formats of four 8-bit channels, those of one and of two, and the storage formats. */
	switch (format) {
	case VK_FORMAT_R8_UNORM:
		*code = DESCRIPTOR_FORMAT_UNORM_1;
		*alternate = false;
		break;
	case VK_FORMAT_R8G8_UNORM:
		*code = DESCRIPTOR_FORMAT_UNORM_2;
		*alternate = false;
		break;
	case VK_FORMAT_R8G8B8A8_UNORM:
		*code = DESCRIPTOR_FORMAT_UNORM;
		*alternate = false;
		break;
	case VK_FORMAT_B8G8R8A8_UNORM:
		*code = DESCRIPTOR_FORMAT_UNORM;
		*alternate = true;
		break;
	case VK_FORMAT_R8G8B8A8_SRGB:
		*code = DESCRIPTOR_FORMAT_SRGB;
		*alternate = false;
		break;
	case VK_FORMAT_B8G8R8A8_SRGB:
		*code = DESCRIPTOR_FORMAT_SRGB;
		*alternate = true;
		break;
	default:
		/* A storage format. */
		if (descriptor_storage(format) == NULL)
			return false;
		*code = descriptor_storage(format)->code;
		*alternate = false;
		break;
	}

	/* Succeeded: the format is read. */
	return true;
}

/*
 * Writes the eight words of the image descriptor of a linear 2D image at
 * address (256-byte aligned), its size and format, read through a view's
 * swizzle (VkComponentSwizzle of red, green, blue and alpha, identity
 * resolved; NULL for the identity).  Its rows are its width's bytes
 * rounded up to 256 apart.  Returns 0, or EINVAL for a format, a size
 * or an address the descriptor cannot hold.
 */
int
amdrdna_vk_image_descriptor(
	uint64_t address,
	uint32_t width,
	uint32_t height,
	uint32_t format,
	const uint32_t *swizzle,
	uint32_t *words)
{
	uint32_t code;
	uint32_t channels;
	uint32_t texel_bytes;
	bool alternate;
	bool narrow;

	/* A format, size and address the descriptor holds. */
	if (!amdrdna_vk_image_format(format, &code, &alternate))
		return EINVAL;

	/* The channels the format has: a format of one or two bytes has one channel a byte. */
	channels = DESCRIPTOR_CHANNELS_ALL;
	texel_bytes = 0U;
	narrow = amdrdna_vk_format_narrow(format, &texel_bytes);
	if (narrow)
		channels = texel_bytes;
	if (descriptor_storage(format) != NULL)
		channels = descriptor_storage(format)->channels;
	if (width == 0U || height == 0U || width > AMDRDNA_VK_IMAGE_SIDE_MAX || height > AMDRDNA_VK_IMAGE_SIDE_MAX)
		return EINVAL;
	if ((address & (AMDRDNA_VK_RESOURCE_ALIGN - 1U)) != 0U)
		return EINVAL;

	/* The address, the format, the size, the channels, a linear 2D image. */
	descriptor_fill(address, width, height, code, alternate, channels, swizzle, words);

	/* Succeeded: the descriptor reads the image. */
	return 0;
}

/*
 * Writes the eight words of the image descriptor of one aspect of a depth
 * image (one level and one layer, in the depth unit's swizzle): its depth
 * (VK_IMAGE_ASPECT_DEPTH_BIT, D16_UNORM's 16-bit normalized or a 32-bit
 * float) from the image's start, or its stencil
 * (VK_IMAGE_ASPECT_STENCIL_BIT, 8 bits) from its stencil plane; raw reads
 * the aspect's bits as an unsigned integer of their size instead, as a
 * copy does.  One channel, read through a view's swizzle (NULL for the
 * identity: the other colours 0, alpha 1).  An image of samples is read
 * as one (a 2D image of samples, its samples' power as its levels).
 * Returns 0, EINVAL for an image that is not a depth image or an aspect it
 * does not have, or EIO for an image without storage.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_image_view.c
 * (radv_image_view_make_descriptor: a depth view's descriptor from the
 * depth surface, a stencil view's from the stencil plane at its offset in
 * the stencil's swizzle mode) and src/amd/common/ac_surface.c
 * (gfx9_compute_surface: the stencil plane in the depth's swizzle mode).
 */
int
amdrdna_vk_depth_descriptor(
	const struct amdrdna_vk_image *image,
	uint32_t aspect,
	bool raw,
	const uint32_t *swizzle,
	uint32_t *words)
{
	uint64_t address;
	uint8_t *cpu;
	uint32_t code;
	int error;

	/* A depth image, with its storage. */
	if (!image->depth)
		return EINVAL;
	error = amdrdna_vk_image_address(image, &address, &cpu);
	if (error != 0)
		return error;

	/*
	 * The stencil from the stencil plane, 8 bits; or the depth from the
	 * start, 16 bits normalized or a 32-bit float (or their bits).
	 */
	if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT) {
		if (!image->stencil)
			return EINVAL;
		address += image->stencil_offset;
		code = DESCRIPTOR_FORMAT_STENCIL;
	} else if (image->format == VK_FORMAT_D16_UNORM) {
		code = DESCRIPTOR_FORMAT_DEPTH_16;
		if (raw)
			code = DESCRIPTOR_FORMAT_BITS_16;
	} else {
		code = DESCRIPTOR_FORMAT_DEPTH_32;
		if (raw)
			code = DESCRIPTOR_FORMAT_BITS_32;
	}

	/* The descriptor of the aspect's one channel, a 2D image in the depth unit's swizzle. */
	descriptor_fill(address, image->width, image->height, code, false, 1U, swizzle, words);
	words[3] |= DESCRIPTOR_SWIZZLE_DEPTH;

	/* An image of samples is read as one, in the same swizzle. */
	if (image->samples > 1U)
		amdrdna_vk_image_descriptor_samples(words, image->samples, false, 0U, 1U);

	/* Succeeded: the descriptor reads the aspect. */
	return 0;
}

/*
 * Writes the eight words of the image descriptor of a linear 2D surface of
 * one unsigned channel of texel_bytes (1, 2 or 4) at address (256-byte
 * aligned): its rows its width's bytes rounded up to 256 apart, as every
 * linear image's.  A copy reads the bits of a depth or stencil aspect
 * staged so.  Returns 0, or EINVAL for a size of texel, a size or an
 * address the descriptor cannot hold.
 */
int
amdrdna_vk_bits_descriptor(
	uint64_t address,
	uint32_t width,
	uint32_t height,
	uint32_t texel_bytes,
	uint32_t *words)
{
	uint32_t code;

	/* An unsigned format of the size. */
	if (texel_bytes == 1U) {
		code = DESCRIPTOR_FORMAT_BITS_8;
	} else if (texel_bytes == 2U) {
		code = DESCRIPTOR_FORMAT_BITS_16;
	} else if (texel_bytes == 4U) {
		code = DESCRIPTOR_FORMAT_BITS_32;
	} else {
		return EINVAL;
	}

	/* A size and address the descriptor holds. */
	if (width == 0U || height == 0U || width > AMDRDNA_VK_IMAGE_SIDE_MAX || height > AMDRDNA_VK_IMAGE_SIDE_MAX)
		return EINVAL;
	if ((address & (AMDRDNA_VK_RESOURCE_ALIGN - 1U)) != 0U)
		return EINVAL;

	/* The descriptor of one channel, a linear 2D image. */
	descriptor_fill(address, width, height, code, false, 1U, NULL, words);

	/* Succeeded: the descriptor reads the surface. */
	return 0;
}

/*
 * Sets the levels an image descriptor reads: from base to last of an
 * image of levels levels, its address the start of the mip chain.
 */
void
amdrdna_vk_image_descriptor_levels(
	uint32_t *words,
	uint32_t base,
	uint32_t last,
	uint32_t levels)
{
	/* The first and the last level read. */
	words[3] |= (base & DESCRIPTOR_LEVEL_MASK) << DESCRIPTOR_BASE_LEVEL_SHIFT;
	words[3] |= (last & DESCRIPTOR_LEVEL_MASK) << DESCRIPTOR_LAST_LEVEL_SHIFT;

	/* The image's last level. */
	words[5] |= ((levels - 1U) & DESCRIPTOR_LEVEL_MASK) << DESCRIPTOR_MAX_LEVEL_SHIFT;
}

/*
 * Sets the type of image a descriptor reads (VkImageViewType: 2D, an array
 * of 2D layers, a cube, 3D) and its slices: the layers from the
 * descriptor's address on, or a 3D image's depth.
 */
void
amdrdna_vk_image_descriptor_slices(
	uint32_t *words,
	uint32_t type,
	uint32_t slices)
{
	uint32_t code;

	/* The type. */
	code = DESCRIPTOR_TYPE_2D;
	if (type == VK_IMAGE_VIEW_TYPE_3D)
		code = DESCRIPTOR_TYPE_3D;
	else if (type == VK_IMAGE_VIEW_TYPE_CUBE)
		code = DESCRIPTOR_TYPE_CUBE;
	else if (type == VK_IMAGE_VIEW_TYPE_2D_ARRAY)
		code = DESCRIPTOR_TYPE_2D_ARRAY;
	words[3] = (words[3] & ~DESCRIPTOR_TYPE_MASK) | code;

	/* The last slice read. */
	if (slices != 0U)
		words[4] = (words[4] & ~DESCRIPTOR_DEPTH_MASK) | ((slices - 1U) & DESCRIPTOR_DEPTH_MASK);
}

/*
 * Makes an image descriptor read an image of samples (the descriptor's
 * address its start): its type (arrayed or not), its swizzle (the render
 * swizzle, or the depth unit's that a depth image's descriptor already
 * names), its samples as its levels, and its layers from base_layer,
 * layers of them.
 */
void
amdrdna_vk_image_descriptor_samples(
	uint32_t *words,
	uint32_t samples,
	bool arrayed,
	uint32_t base_layer,
	uint32_t layers)
{
	uint32_t power;
	uint32_t last;

	/* The samples' power. */
	power = 0U;
	while ((1U << power) < samples)
		power++;

	/* The type, the swizzle (a colour image's the render swizzle), the levels from 0 to the samples' power. */
	words[3] &= ~(DESCRIPTOR_TYPE_MASK | (DESCRIPTOR_LEVEL_MASK << DESCRIPTOR_BASE_LEVEL_SHIFT) |
		      (DESCRIPTOR_LEVEL_MASK << DESCRIPTOR_LAST_LEVEL_SHIFT));
	words[3] |= arrayed ? DESCRIPTOR_TYPE_2D_MSAA_ARRAY : DESCRIPTOR_TYPE_2D_MSAA;
	if ((words[3] & DESCRIPTOR_SWIZZLE_MASK) != DESCRIPTOR_SWIZZLE_DEPTH)
		words[3] = (words[3] & ~DESCRIPTOR_SWIZZLE_MASK) | DESCRIPTOR_SWIZZLE_RENDER;
	words[3] |= power << DESCRIPTOR_LAST_LEVEL_SHIFT;
	words[5] = (words[5] & ~(DESCRIPTOR_LEVEL_MASK << DESCRIPTOR_MAX_LEVEL_SHIFT)) | (power << DESCRIPTOR_MAX_LEVEL_SHIFT);

	/* The first layer and the last. */
	last = base_layer + layers - 1U;
	words[4] = ((base_layer & DESCRIPTOR_DEPTH_MASK) << DESCRIPTOR_BASE_ARRAY_SHIFT) | (last & DESCRIPTOR_DEPTH_MASK);
}

/* Writes the four words of the sampler descriptor of a VkSampler. */
void
amdrdna_vk_sampler_descriptor(
	const struct amdrdna_vk_sampler *sampler,
	uint32_t *words)
{
	/* The clamps, the compare, the coordinates in pixels. */
	kern_memset(words, 0, AMDRDNA_SAMPLER_WORDS * sizeof(uint32_t));
	words[0] = sampler_clamp(sampler->address_u);
	words[0] |= sampler_clamp(sampler->address_v) << SAMPLER_CLAMP_Y_SHIFT;
	words[0] |= sampler_clamp(sampler->address_w) << SAMPLER_CLAMP_Z_SHIFT;
	if (sampler->compare_enable != 0U)
		words[0] |= (sampler->compare_op & SAMPLER_COMPARE_MASK) << SAMPLER_COMPARE_SHIFT;
	if (sampler->unnormalized != 0U)
		words[0] |= SAMPLER_UNNORMALIZED;
	if (sampler->mag_filter == VK_FILTER_NEAREST && sampler->min_filter == VK_FILTER_NEAREST)
		words[0] |= SAMPLER_TRUNCATE;

	/* Every level of detail from 0 (the image's levels limit it). */
	words[1] = SAMPLER_MAX_LOD;

	/* The filters. */
	words[2] = sampler_filter(sampler->mag_filter) << SAMPLER_MAG_SHIFT;
	words[2] |= sampler_filter(sampler->min_filter) << SAMPLER_MIN_SHIFT;
	if (sampler->mipmap_mode == VK_SAMPLER_MIPMAP_MODE_LINEAR) {
		words[2] |= SAMPLER_MIP_LINEAR << SAMPLER_MIP_SHIFT;
	} else {
		words[2] |= SAMPLER_MIP_POINT << SAMPLER_MIP_SHIFT;
	}

	/* The border colour. */
	words[3] = sampler_border(sampler->border_color) << SAMPLER_BORDER_SHIFT;
}

/*
 * Writes the image and the sampler descriptors of a combined image
 * sampler binding.  A view of a depth image reads its depth or its
 * stencil in the depth unit's swizzle (amdrdna_vk_depth_descriptor).  A
 * view of a plane of a two-plane image reads that plane as an image of
 * its own (its start, its size, its format); a view
 * of layers reads them from its first (the descriptor's address that
 * layer's), a view of a 3D image its depth, a view of an image of samples
 * its samples and its layers from the image's start.
 * Returns 0, EINVAL for a binding without both or a view the descriptor
 * cannot read, or EIO for an image without storage.
 */
int
amdrdna_vk_texture_descriptors(
	const struct amdrdna_vk_descriptor_slot *slot,
	uint32_t *image_words,
	uint32_t *sampler_words)
{
	const struct amdrdna_vk_view *view;
	const struct amdrdna_vk_image *image;
	struct amdrdna_vk_image plane;
	uint64_t va;
	uint8_t *cpu;
	uint32_t slices;
	uint32_t aspect;
	int error;

	/* The view's image, and the sampler. */
	if (slot->view == NULL || slot->sampler == NULL)
		return EINVAL;
	view = slot->view->payload;
	image = view->image->payload;

	/* A depth image: the aspect the view reads (its depth when it names both), as the shaders sample it. */
	if (image->depth) {
		aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
		if (view->aspect == VK_IMAGE_ASPECT_STENCIL_BIT)
			aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
		error = amdrdna_vk_depth_descriptor(image, aspect, false, view->swizzle, image_words);
		if (error != 0)
			return error;
		amdrdna_vk_sampler_descriptor(slot->sampler->payload, sampler_words);
		return 0;
	}

	/* A plane of a two-plane image is the image the descriptor reads. */
	if (image->planar) {
		error = amdrdna_vk_image_part(image, view->aspect, 0U, &plane);
		if (error != 0)
			return error;
		image = &plane;
	}

	/* The image's storage, from the view's first layer (an image of samples is read from its start). */
	error = amdrdna_vk_image_address(image, &va, &cpu);
	if (error != 0)
		return error;
	if (image->samples > 1U) {
		error = amdrdna_vk_image_descriptor(va, image->width, image->height, view->format, view->swizzle, image_words);
		if (error != 0)
			return error;
		amdrdna_vk_image_descriptor_samples(image_words, image->samples, view->type == VK_IMAGE_VIEW_TYPE_2D_ARRAY, view->base_layer,
						    view->layer_count);
		amdrdna_vk_sampler_descriptor(slot->sampler->payload, sampler_words);
		return 0;
	}
	va += (uint64_t)view->base_layer * image->slice_bytes;

	/*
	 * The two descriptors; the image's from the start of the first
	 * layer's mip chain, with the view's levels, type and layers (or the
	 * 3D image's depth).
	 */
	error = amdrdna_vk_image_descriptor(va, image->width, image->height, view->format, view->swizzle, image_words);
	if (error != 0)
		return error;
	amdrdna_vk_image_descriptor_levels(image_words, view->base_level, view->base_level + view->level_count - 1U, image->levels);
	slices = view->layer_count;
	if (view->type == VK_IMAGE_VIEW_TYPE_3D)
		slices = image->slices;
	amdrdna_vk_image_descriptor_slices(image_words, view->type, slices);
	amdrdna_vk_sampler_descriptor(slot->sampler->payload, sampler_words);

	/* Succeeded: the binding's descriptors are written. */
	return 0;
}

/*
 * Writes the image descriptor of a storage image binding: its view's one
 * level (the first; a storage view has one) of its layers from the first,
 * in the view's format with the channels it lacks read as 0, 0, 0, 1 (a
 * storage view's swizzle is the identity), a 2D view as 2D, an array's or
 * a cube's layers as an array of 2D images and a 3D view as 3D.  Returns
 * 0, EINVAL for a binding without a view or a view the descriptor cannot
 * read (a plane, an image of samples, a depth image), or EIO for an image
 * without storage.
 *
 * Procedure referenced: Mesa 25.0.7 src/amd/vulkan/radv_image_view.c
 * (radv_tex_dim: a cube's storage view as SQ_RSRC_IMG_2D_ARRAY;
 * radv_image_view_make_descriptor: a storage descriptor of the view's base
 * level alone) and src/amd/common/ac_shader_util.c (ac_get_image_dim: the
 * shader's cube as a 2D array to match).
 */
int
amdrdna_vk_storage_descriptor(
	const struct amdrdna_vk_descriptor_slot *slot,
	uint32_t *words)
{
	const struct amdrdna_vk_view *view;
	const struct amdrdna_vk_image *image;
	uint64_t va;
	uint8_t *cpu;
	uint32_t type;
	uint32_t slices;
	int error;

	/* A view of an image of one sample, of colour texels. */
	if (slot == NULL || slot->view == NULL)
		return EINVAL;
	view = slot->view->payload;
	image = view->image->payload;
	if (image->planar || image->depth || image->samples > 1U)
		return EINVAL;

	/* The image's storage from the view's first layer. */
	error = amdrdna_vk_image_address(image, &va, &cpu);
	if (error != 0)
		return error;
	va += (uint64_t)view->base_layer * image->slice_bytes;

	/* The descriptor of the view's first level, of its layers or the 3D image's depth. */
	error = amdrdna_vk_image_descriptor(va, image->width, image->height, view->format, NULL, words);
	if (error != 0)
		return error;
	amdrdna_vk_image_descriptor_levels(words, view->base_level, view->base_level, image->levels);
	type = view->type;
	slices = view->layer_count;
	if (type == VK_IMAGE_VIEW_TYPE_CUBE)
		type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	if (type == VK_IMAGE_VIEW_TYPE_3D)
		slices = image->slices;
	amdrdna_vk_image_descriptor_slices(words, type, slices);

	/* Succeeded: the binding's descriptor is written. */
	return 0;
}

/*
 * Reports the buffer descriptor format of a texel buffer's view format,
 * its channels and its bytes: a vertex format, or a storage format; false
 * for a format the executor does not fetch.
 */
bool
amdrdna_vk_texel_format(
	uint32_t format,
	uint32_t *code,
	uint32_t *components,
	uint32_t *bytes)
{
	const struct descriptor_storage_format *storage;

	/* A vertex format. */
	if (amdrdna_vk_vertex_format(format, code, components, bytes))
		return true;

	/* A storage format. */
	storage = descriptor_storage(format);
	if (storage == NULL)
		return false;
	*code = storage->code;
	*components = storage->channels;
	*bytes = storage->bytes;
	return true;
}

/*
 * Reports the buffer descriptor format of a vertex attribute format, its
 * channels and its bytes; false for a format the executor does not fetch.
 */
bool
amdrdna_vk_vertex_format(
	uint32_t format,
	uint32_t *code,
	uint32_t *components,
	uint32_t *bytes)
{
	/* Floats, unsigned integers and four unorm bytes. */
	switch (format) {
	case VK_FORMAT_R32_SFLOAT:
		*code = AMDRDNA_GFX10_VERTEX_FLOAT1;
		*components = 1U;
		break;
	case VK_FORMAT_R32G32_SFLOAT:
		*code = AMDRDNA_GFX10_VERTEX_FLOAT2;
		*components = 2U;
		break;
	case VK_FORMAT_R32G32B32_SFLOAT:
		*code = AMDRDNA_GFX10_VERTEX_FLOAT3;
		*components = 3U;
		break;
	case VK_FORMAT_R32G32B32A32_SFLOAT:
		*code = AMDRDNA_GFX10_VERTEX_FLOAT4;
		*components = 4U;
		break;
	case VK_FORMAT_R32_UINT:
		*code = AMDRDNA_GFX10_VERTEX_UINT1;
		*components = 1U;
		break;
	case VK_FORMAT_R32G32_UINT:
		*code = AMDRDNA_GFX10_VERTEX_UINT2;
		*components = 2U;
		break;
	case VK_FORMAT_R32G32B32_UINT:
		*code = AMDRDNA_GFX10_VERTEX_UINT3;
		*components = 3U;
		break;
	case VK_FORMAT_R32G32B32A32_UINT:
		*code = AMDRDNA_GFX10_VERTEX_UINT4;
		*components = 4U;
		break;
	case VK_FORMAT_R8G8B8A8_UNORM:
		*code = AMDRDNA_GFX10_VERTEX_UNORM8X4;
		*components = 4U;
		*bytes = 4U;
		return true;
	default:
		return false;
	}

	/* Succeeded: four bytes a channel. */
	*bytes = *components * 4U;
	return true;
}

/*
 * Writes the eight words of the image descriptor of a 2D image at address
 * of width by height in descriptor format code, of channels channels (blue
 * first in memory when alternate), read through a view's swizzle (NULL for
 * the identity): linear, its one level and layer, the modulation every
 * descriptor has.
 */
static void
descriptor_fill(
	uint64_t address,
	uint32_t width,
	uint32_t height,
	uint32_t code,
	bool alternate,
	uint32_t channels,
	const uint32_t *swizzle,
	uint32_t *words)
{
	uint32_t channel;
	uint32_t source;

	/* The address, the format and the size. */
	kern_memset(words, 0, AMDRDNA_IMAGE_WORDS * sizeof(uint32_t));
	words[0] = (uint32_t)(address >> DESCRIPTOR_ADDRESS_SHIFT);
	words[1] = (uint32_t)(address >> DESCRIPTOR_ADDRESS_HIGH_SHIFT) & 0xffU;
	words[1] |= code << DESCRIPTOR_FORMAT_SHIFT;
	words[1] |= ((width - 1U) & 3U) << DESCRIPTOR_WIDTH_LOW_SHIFT;
	words[2] = ((width - 1U) >> 2) & 0xfffU;
	words[2] |= (height - 1U) << DESCRIPTOR_HEIGHT_SHIFT;
	words[2] |= DESCRIPTOR_RESOURCE_LEVEL;

	/* The channel each of red, green, blue and alpha reads. */
	for (channel = 0U; channel < 4U; channel++) {
		source = VK_COMPONENT_SWIZZLE_R + channel;
		if (swizzle != NULL)
			source = swizzle[channel];
		words[3] |= descriptor_select(source, channel, alternate, channels) << (DESCRIPTOR_SELECT_BITS * channel);
	}

	/* A blue-first format's border colour swizzled too; the type; the modulation. */
	if (alternate)
		words[3] |= DESCRIPTOR_BORDER_SWIZZLE_ZYXW << DESCRIPTOR_BORDER_SWIZZLE_SHIFT;
	words[3] |= DESCRIPTOR_TYPE_2D;
	words[5] = DESCRIPTOR_PERFORMANCE;
}

/*
 * Reports the channel select of a descriptor for one view channel: the
 * swizzle's source, with red and blue swapped for a blue-first format
 * (the descriptor's X is the first byte in memory).  A format of fewer
 * channels than four reads a missing colour channel as zero and a missing
 * alpha as one.
 *
 * Values referenced: the Vulkan 1.3 specification (Conversion to RGBA: a
 * missing R, G or B is 0 and a missing A is 1).
 */
static uint32_t
descriptor_select(
	uint32_t swizzle,
	uint32_t channel,
	bool alternate,
	uint32_t channels)
{
	uint32_t source;

	/* Zero and one are constants; identity is the channel itself. */
	switch (swizzle) {
	case VK_COMPONENT_SWIZZLE_ZERO:
		return DESCRIPTOR_SELECT_ZERO;
	case VK_COMPONENT_SWIZZLE_ONE:
		return DESCRIPTOR_SELECT_ONE;
	case VK_COMPONENT_SWIZZLE_R:
	case VK_COMPONENT_SWIZZLE_G:
	case VK_COMPONENT_SWIZZLE_B:
	case VK_COMPONENT_SWIZZLE_A:
		source = swizzle - VK_COMPONENT_SWIZZLE_R;
		break;
	default:
		source = channel;
		break;
	}

	/* A channel the format does not have: alpha is one, a colour zero. */
	if (source >= channels) {
		if (source == 3U)
			return DESCRIPTOR_SELECT_ONE;
		return DESCRIPTOR_SELECT_ZERO;
	}

	/* Blue first in memory: red is the third byte, blue the first. */
	if (alternate && source != 1U && source != 3U)
		source = 2U - source;

	/* Succeeded: the select of the memory channel. */
	return DESCRIPTOR_SELECT_X + source;
}

/* Reports the clamp of a VkSamplerAddressMode. */
static uint32_t
sampler_clamp(
	uint32_t mode)
{
	/* Each mode's clamp; repeat for any other. */
	switch (mode) {
	case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
		return SAMPLER_MIRROR;
	case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:
		return SAMPLER_CLAMP_EDGE;
	case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:
		return SAMPLER_CLAMP_BORDER;
	case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE:
		return SAMPLER_MIRROR_ONCE_EDGE;
	default:
		return SAMPLER_WRAP;
	}
}

/* Reports the filter of a VkFilter. */
static uint32_t
sampler_filter(
	uint32_t filter)
{
	/* Linear is bilinear; anything else the nearest texel. */
	if (filter == VK_FILTER_LINEAR)
		return SAMPLER_BILINEAR;
	return SAMPLER_POINT;
}

/* Reports the border colour type of a VkBorderColor. */
static uint32_t
sampler_border(
	uint32_t colour)
{
	/* Opaque black, opaque white, or transparent black. */
	switch (colour) {
	case VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK:
	case VK_BORDER_COLOR_INT_OPAQUE_BLACK:
		return SAMPLER_BORDER_BLACK;
	case VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE:
	case VK_BORDER_COLOR_INT_OPAQUE_WHITE:
		return SAMPLER_BORDER_WHITE;
	default:
		return SAMPLER_BORDER_TRANSPARENT;
	}
}

/* Finds a storage format's record; NULL for a format that is not one. */
static const struct descriptor_storage_format *
descriptor_storage(
	uint32_t format)
{
	uint32_t index;

	/* Each record. */
	for (index = 0U; index < sizeof(descriptor_storage_formats) / sizeof(descriptor_storage_formats[0]); index++) {
		if (descriptor_storage_formats[index].format == format)
			return &descriptor_storage_formats[index];
	}

	/* No record names it. */
	return NULL;
}
