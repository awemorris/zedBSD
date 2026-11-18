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

/*
 * The features of a normalised or small float format that is sampled,
 * filtered, copied and cleared only: the transfer moves its texels raw
 * (transfer-program.c powervr_transfer_raw_format) and the blit takes
 * four 8-bit unorm channels only, so it is no blit source or
 * destination.
 */
#define FEATURES_SAMPLED_FILTER		(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | \
					 VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | \
					 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
					 VK_FORMAT_FEATURE_TRANSFER_DST_BIT)

/*
 * The features of a normalised or small float format that is also a
 * colour attachment that blends, of an integer one that is a colour
 * attachment (Vulkan blends no integer format), and of a 32-bit float
 * one that is a colour attachment that blends: the compiler packs each
 * one's fragment output into its pixel output registers
 * (powervr_shader_pixel_layout) and the pixel back end stores it.
 *
 * Procedure referenced: Mesa 26.2.6 src/imagination/vulkan/
 * pvr_arch_formats.c (the render target bind of these formats) and
 * pvr_formats.c (pvr_get_image_format_features2: COLOR_ATTACHMENT for a
 * render target, COLOR_ATTACHMENT_BLEND unless the format is integer).
 */
#define FEATURES_RENDER_FILTER		(FEATURES_SAMPLED_FILTER | \
					 VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | \
					 VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)
#define FEATURES_RENDER_INTEGER		(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | \
					 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
					 VK_FORMAT_FEATURE_TRANSFER_DST_BIT | \
					 VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
#define FEATURES_RENDER_F32		(FEATURES_RENDER_INTEGER | \
					 VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)

/*
 * The features of a depth format: an attachment, copied and cleared, and
 * sampled unfiltered (its depth the first channel; the depth load and
 * store keep an optimal image twiddled, as the texture unit reads it).
 *
 * Procedure referenced: Mesa 26.2.6 src/imagination/vulkan/pvr_formats.c
 * (pvr_get_image_format_features2: a depth format of the sampler views is
 * sampled and transferred, never filtered linearly).
 */
#define FEATURES_DEPTH			(VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | \
					 VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | \
					 VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | \
					 VK_FORMAT_FEATURE_TRANSFER_DST_BIT)

/*
 * The features of a depth format without a stencil: those of a depth
 * format, and blitted onto itself (the transfer moves its raw texels,
 * point sampled).  A packed depth and stencil is not blitted: a blit of
 * one of its aspects would mix the other's bits.
 *
 * Procedure referenced: Mesa 26.2.6 src/imagination/vulkan/pvr_formats.c
 * (pvr_get_image_format_features2: a depth format of the sampler views a
 * blit's source, of the depth and stencil binds a blit's destination).
 */
#define FEATURES_DEPTH_BLIT		(FEATURES_DEPTH | \
					 VK_FORMAT_FEATURE_BLIT_SRC_BIT | \
					 VK_FORMAT_FEATURE_BLIT_DST_BIT)

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
#define VIEW_KIND_TWIDDLED_3D		2U
#define VIEW_KIND_CUBE			3U
#define VIEW_KIND_STRIDED		4U

/* The faces of a cube. */
#define CUBE_FACES			6U

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

/* The kinds of the channels of a format of equal channels (channel_layout). */
#define CHANNEL_UNORM			0U
#define CHANNEL_SNORM			1U
#define CHANNEL_INTEGER			2U
#define CHANNEL_FLOAT			3U

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
	  FEATURES_RENDER_FILTER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R8G8_UNORM, 2, POWERVR_FORMAT_COLOR, 7, 0x23,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_FILTER, FEATURES_TRANSFER, 0 },
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
	  FEATURES_DEPTH_BLIT, 0, 0 },
	{ VK_FORMAT_D32_SFLOAT, 4, POWERVR_FORMAT_DEPTH, 18, 0x36,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_ABSENT, POWERVR_CHANNEL_ABSENT, POWERVR_CHANNEL_ABSENT },
	  FEATURES_DEPTH_BLIT, 0, 0 },
	/*
	 * The packed 24-bit depth and 8-bit stencil, the stencil in the top
	 * byte: the depth load and store's 24BITINT, its depth read as X8U24
	 * and stored as ST8U24.  Copies and clears move whole texels (both
	 * aspects).
	 *
	 * Values referenced: Mesa 26.2.6 src/imagination/vulkan/
	 * pvr_arch_formats.c (Z24_UNORM_S8_UINT: depth texture format X8U24,
	 * pack mode ST8U24); src/imagination/csbgen/rogue/texstate.xml
	 * (FORMAT X8U24 21) and pbestate.xml (PACKMODE ST8U24 0x2c).
	 */
	{ VK_FORMAT_D24_UNORM_S8_UINT, 4, POWERVR_FORMAT_DEPTH | POWERVR_FORMAT_STENCIL, 21, 0x2C,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_ABSENT, POWERVR_CHANNEL_ABSENT, POWERVR_CHANNEL_ABSENT },
	  FEATURES_DEPTH, 0, 0 },
	{ VK_FORMAT_R16G16B16A16_SFLOAT, 8, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_FLOAT, 28, 0x0A,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_3 },
	  FEATURES_RENDER_F16, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R32_SFLOAT, 4, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_FLOAT, 18, 0x36,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_F32, FEATURES_TRANSFER, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT },
	{ VK_FORMAT_R32G32_SFLOAT, 8, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_FLOAT, 34, 0x2A,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_F32, FEATURES_TRANSFER, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT },
	{ VK_FORMAT_R32G32B32_SFLOAT, 12, 0, NO_CODE, 0x1F,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_ONE },
	  0, 0, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT },
	/*
	 * The three-channel 32-bit integer vertex formats: vertex buffers only,
	 * a word a channel as the 32-bit float one (the integer 32-bit
	 * formats of one, two and four channels are vertex formats too, in
	 * their rows below).
	 *
	 * Procedure referenced: Khronos Vulkan 1.3 specification, "Vertex
	 * Input Extraction" (an integer format's channels reach an integer
	 * input as they are, the missing alpha an integer 1) and "Required
	 * Format Support" (VERTEX_BUFFER for the 32-bit UINT and SINT formats
	 * of one to four channels).
	 */
	{ VK_FORMAT_R32G32B32_UINT, 12, 0, NO_CODE, NO_CODE,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_ONE },
	  0, 0, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT },
	{ VK_FORMAT_R32G32B32_SINT, 12, 0, NO_CODE, NO_CODE,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_ONE },
	  0, 0, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT },
	{ VK_FORMAT_R32G32B32A32_SFLOAT, 16, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_FLOAT, 61, 0x0D,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_3 },
	  FEATURES_RENDER_F32, FEATURES_TRANSFER, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT },
	/*
	 * The formats sampled, copied and cleared (the transfer moves their
	 * texels raw), and colour attachments: an integer one sampled
	 * unfiltered (Vulkan filters no integer format) and not blended.
	 * The texture unit and pixel back end codes are Mesa's rows of the
	 * same formats.
	 *
	 * Values referenced: Mesa 26.2.6 src/imagination/vulkan/
	 * pvr_arch_formats.c (pvr_format_table and pvr_pbe_format_table:
	 * the rows of these formats, where Mesa's R10G10B10A2 and
	 * R11G11B10_FLOAT are Vulkan's A2B10G10R10 and B10G11R11);
	 * src/imagination/csbgen/rogue/texstate.xml (enum FORMAT) and
	 * pbestate.xml (enum PACKMODE); src/imagination/vulkan/
	 * pvr_formats.c (pvr_get_image_format_features2: sampled and
	 * transferred, linear filtering for a format neither integer nor of
	 * 32-bit channels).
	 */
	{ VK_FORMAT_R8_SNORM, 1, POWERVR_FORMAT_COLOR, 1, 0x30,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_FILTER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R8_UINT, 1, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 0, 0x2F,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_INTEGER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R8_SINT, 1, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 1, 0x30,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_INTEGER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R8G8_SNORM, 2, POWERVR_FORMAT_COLOR, 8, 0x24,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_FILTER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R8G8_UINT, 2, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 7, 0x23,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_INTEGER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R8G8_SINT, 2, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 8, 0x24,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_INTEGER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R8G8B8A8_SNORM, 4, POWERVR_FORMAT_COLOR, 13, 0x01,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_3 },
	  FEATURES_RENDER_FILTER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R8G8B8A8_UINT, 4, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 12, 0x00,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_3 },
	  FEATURES_RENDER_INTEGER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R8G8B8A8_SINT, 4, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 13, 0x01,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_3 },
	  FEATURES_RENDER_INTEGER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, POWERVR_FORMAT_COLOR, 14, 0x0E,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_3 },
	  FEATURES_RENDER_FILTER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_A2B10G10R10_UINT_PACK32, 4, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 14, 0x34,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_3 },
	  FEATURES_RENDER_INTEGER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R16_UNORM, 2, POWERVR_FORMAT_COLOR, 9, 0x31,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_FILTER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R16_SNORM, 2, POWERVR_FORMAT_COLOR, 10, 0x32,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_FILTER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R16_UINT, 2, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 9, 0x31,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_INTEGER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R16_SINT, 2, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 10, 0x32,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_INTEGER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R16_SFLOAT, 2, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_FLOAT, 11, 0x33,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_FILTER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R16G16_UNORM, 4, POWERVR_FORMAT_COLOR, 15, 0x25,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_FILTER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R16G16_SNORM, 4, POWERVR_FORMAT_COLOR, 16, 0x26,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_FILTER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R16G16_UINT, 4, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 15, 0x25,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_INTEGER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R16G16_SINT, 4, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 16, 0x26,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_INTEGER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R16G16_SFLOAT, 4, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_FLOAT, 17, 0x27,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_FILTER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R16G16B16A16_UNORM, 8, POWERVR_FORMAT_COLOR, 29, 0x08,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_3 },
	  FEATURES_RENDER_FILTER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R16G16B16A16_SNORM, 8, POWERVR_FORMAT_COLOR, 30, 0x09,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_3 },
	  FEATURES_RENDER_FILTER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R16G16B16A16_UINT, 8, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 29, 0x08,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_3 },
	  FEATURES_RENDER_INTEGER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R16G16B16A16_SINT, 8, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 30, 0x09,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_3 },
	  FEATURES_RENDER_INTEGER, FEATURES_TRANSFER, 0 },
	{ VK_FORMAT_R32_UINT, 4, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 24, 0x34,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_INTEGER, FEATURES_TRANSFER, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT },
	{ VK_FORMAT_R32_SINT, 4, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 25, 0x35,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_INTEGER, FEATURES_TRANSFER, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT },
	{ VK_FORMAT_R32G32_UINT, 8, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 35, 0x28,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_INTEGER, FEATURES_TRANSFER, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT },
	{ VK_FORMAT_R32G32_SINT, 8, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 36, 0x29,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_INTEGER, FEATURES_TRANSFER, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT },
	{ VK_FORMAT_R32G32B32A32_UINT, 16, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 62, 0x0B,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_3 },
	  FEATURES_RENDER_INTEGER, FEATURES_TRANSFER, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT },
	{ VK_FORMAT_R32G32B32A32_SINT, 16, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_INTEGER, 63, 0x0C,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_3 },
	  FEATURES_RENDER_INTEGER, FEATURES_TRANSFER, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT },
	{ VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4, POWERVR_FORMAT_COLOR | POWERVR_FORMAT_FLOAT, 88, 0x1D,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_ONE },
	  FEATURES_RENDER_FILTER, FEATURES_TRANSFER, 0 },
	/* NV12: neither unit reads it whole, only its planes as R8 and R8G8 (powervr_image_lay_out_planes). */
	{ VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, 1, POWERVR_FORMAT_PLANAR, NO_CODE, NO_CODE,
	  { POWERVR_CHANNEL_0, POWERVR_CHANNEL_1, POWERVR_CHANNEL_2, POWERVR_CHANNEL_ONE },
	  FEATURES_PLANAR, FEATURES_PLANAR, 0 }
};

static uint64_t place(uint64_t value, unsigned low, unsigned high);
static uint32_t float_unit(uint32_t bits);
static bool alpha_on_msb(const struct powervr_format *format);
static uint32_t round_up_power(uint32_t value);
static uint32_t round_up(uint32_t value, uint32_t unit);
static uint32_t common_multiple(uint32_t a, uint32_t b);
static uint32_t channel_select(const struct powervr_format *format, uint32_t mapping, unsigned output);
static int sampler_wrap(uint32_t mode, uint32_t *wrap);
static int32_t fixed_from_bits(uint32_t bits, unsigned fraction, int32_t smallest, int32_t largest);
static uint32_t half_from_float(uint32_t bits);
static uint32_t unorm8_from_float(uint32_t bits);
static uint32_t unorm16_from_float(uint32_t bits);
static uint32_t unorm_from_float(uint32_t bits, uint32_t largest);
static uint32_t snorm_from_float(uint32_t bits, uint32_t largest);
static uint32_t small_float_from_float(uint32_t bits, uint32_t mantissa_bits);
static bool channel_layout(uint32_t vk_format, uint32_t *channels, uint32_t *width, uint32_t *kind);

/*
 * The stencil aspect of the packed 24-bit depth and 8-bit stencil, as a
 * view of that aspect alone is sampled: each texel's top byte as an
 * unsigned integer in the red channel (green and blue 0, alpha 1).  It is
 * only sampled: no attachment, copy or store reads it.
 *
 * Values referenced: Mesa 26.2.6 src/imagination/vulkan/
 * pvr_arch_formats.c (Z24_UNORM_S8_UINT: stencil texture format U8X24)
 * and src/imagination/csbgen/rogue/texstate.xml (FORMAT U8X24 23).
 */
static const struct powervr_format format_stencil_of_d24s8 = {
	VK_FORMAT_D24_UNORM_S8_UINT, 4, POWERVR_FORMAT_STENCIL | POWERVR_FORMAT_INTEGER, 23, 0,
	{ POWERVR_CHANNEL_0, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ZERO, POWERVR_CHANNEL_ONE },
	0, 0, 0
};

/* Finds the entry a view of a format's stencil aspect alone is sampled as, or NULL when the executor has none. */
const struct powervr_format *
powervr_format_stencil_aspect(
	const struct powervr_format *format)
{
	/* The packed depth and stencil's. */
	if (format != NULL && format->vk_format == VK_FORMAT_D24_UNORM_S8_UINT)
		return &format_stencil_of_d24s8;

	/* None. */
	return NULL;
}

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
 * Reports the storage image features of a format: the sixteen formats
 * Vulkan requires storage images of (Required Format Support, the 4-byte,
 * 16-bit and 32-bit tables), with atomics for R32_UINT and R32_SINT; none
 * for any other.  A storage image is linear (vk/vk-image.c) and the
 * shaders read and write it as a buffer of its texels
 * (DRV_GPU_SPIRV_FEATURE_LOWER_STORAGE_IMAGES's lowering).
 */
uint32_t
powervr_format_storage_features(
	const struct powervr_format *format)
{
	/* By the format. */
	switch (format->vk_format) {
	case VK_FORMAT_R32_UINT:
	case VK_FORMAT_R32_SINT:
		/* A storage image of atomics. */
		return VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT;
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SNORM:
	case VK_FORMAT_R8G8B8A8_UINT:
	case VK_FORMAT_R8G8B8A8_SINT:
	case VK_FORMAT_R16G16B16A16_UINT:
	case VK_FORMAT_R16G16B16A16_SINT:
	case VK_FORMAT_R16G16B16A16_SFLOAT:
	case VK_FORMAT_R32_SFLOAT:
	case VK_FORMAT_R32G32_UINT:
	case VK_FORMAT_R32G32_SINT:
	case VK_FORMAT_R32G32_SFLOAT:
	case VK_FORMAT_R32G32B32A32_UINT:
	case VK_FORMAT_R32G32B32A32_SINT:
	case VK_FORMAT_R32G32B32A32_SFLOAT:
		/* A storage image. */
		return VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
	default:
		break;
	}

	/* Not a storage image. */
	return 0;
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
	layout->samples = 1;

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
 * Lays an image of samples samples a texel out: one sample as
 * powervr_image_lay_out; four as a twiddled flat image of one level
 * whose layer holds four times the texels (each texel's samples
 * together, as the pixel back end stores them and the texture unit reads
 * them), each layer of an array starting on the image's boundary.
 * Returns ENOTSUP for another count, a linear image, a volume or more
 * than one level, which Vulkan does not let have samples either.
 *
 * Procedure referenced: Mesa 26.2.6 src/imagination/vulkan/pvr_image.c
 * (pvr_image_setup_mip_levels: a level's size and the layer's the
 * samples times the pitch, height and depth; layers of an array aligned
 * to the image's alignment).
 */
int
powervr_image_lay_out_samples(
	const struct powervr_image_shape *shape,
	const struct powervr_format *format,
	uint32_t samples,
	struct powervr_image_layout *layout)
{
	struct powervr_image_shape one_layer;
	int error;

	/* One sample: the plain layout. */
	if (samples == 1U)
		return powervr_image_lay_out(shape, format, layout);

	/* Four samples of a twiddled flat image of one level. */
	if (samples != 4U)
		return ENOTSUP;
	if (shape->linear || shape->volume || shape->depth != 1U || shape->levels != 1U)
		return ENOTSUP;

	/* One layer laid out, its bytes four times, then the layers on the boundary. */
	one_layer = *shape;
	one_layer.layers = 1;
	error = powervr_image_lay_out(&one_layer, format, layout);
	if (error != 0)
		return error;
	if (shape->layers == 0 || shape->layers > POWERVR_IMAGE_LAYERS_MAX)
		return EINVAL;
	layout->layer_bytes *= samples;
	if (shape->layers > 1)
		layout->layer_bytes = (layout->layer_bytes + POWERVR_IMAGE_ALIGN - 1U) & ~(uint64_t)(POWERVR_IMAGE_ALIGN - 1U);
	layout->bytes = layout->layer_bytes * shape->layers;
	layout->samples = samples;

	/* Succeeded: the multisampled layout. */
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
	if (format == NULL ||
	    (format->kind & POWERVR_FORMAT_PLANAR) == 0 ||
	    format->vk_format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
		return ENOTSUP;
	if (shape->levels != 1 ||
	    shape->layers != 1 ||
	    shape->depth != 1 ||
	    shape->volume)
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
	layout->samples = 1;
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
 * A twiddled image is a two-dimensional, volume or cube texture whose
 * words name the view's first level and level count, and its layers
 * (a cube's six faces) or a volume's depth; a linear one is a strided
 * texture whose second word holds its row in texels.  Returns EINVAL for
 * a view the words cannot hold: an address off the four-byte boundary or
 * past forty bits, a format the texture unit does not read, a linear
 * view that is not flat, a cube of other than six layers, or a volume of
 * more than one layer or a depth outside the field.
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

	/* A strided image is flat; a cube is six faces; a volume one layer of its depth. */
	if (view->kind != POWERVR_VIEW_FLAT && view->kind != POWERVR_VIEW_VOLUME && view->kind != POWERVR_VIEW_CUBE)
		return EINVAL;
	if (view->linear && view->kind != POWERVR_VIEW_FLAT)
		return EINVAL;
	if (view->kind == POWERVR_VIEW_CUBE && view->layer_count != CUBE_FACES)
		return EINVAL;
	if (view->kind == POWERVR_VIEW_VOLUME &&
	    (view->layer_count != 1 || view->base_layer != 0 || view->depth == 0 || view->depth > POWERVR_IMAGE_LAYERS_MAX))
		return EINVAL;

	/*
	 * The first word: the kind of image, its gamma, the four channel
	 * selections, the format and the edges less one.
	 *
	 * Values referenced: Mesa 26.2.5 src/imagination/csbgen/rogue/
	 * texstate.xml (IMAGE_WORD0: textype 0-2, gamma 3, swiz3 5-7, swiz2
	 * 8-10, swiz1 11-13, swiz0 14-16, texformat 27-33, width 34-47,
	 * height 48-61; TEXTYPE 2D 1, 3D 2, CUBE 3 and STRIDE 4; GAMMA ON
	 * 1) and Mesa 26.2.6 src/imagination/vulkan/pvr_arch_tex_state.c
	 * (pvr_arch_pack_tex_state: a linear 2D view STRIDE, a twiddled one
	 * 2D, a cube view CUBE, a view of a 3D-twiddled image 3D).
	 */
	if (view->linear) {
		first = place(VIEW_KIND_STRIDED, 0, 2);
	} else if (view->kind == POWERVR_VIEW_VOLUME) {
		first = place(VIEW_KIND_TWIDDLED_3D, 0, 2);
	} else if (view->kind == POWERVR_VIEW_CUBE) {
		first = place(VIEW_KIND_CUBE, 0, 2);
	} else {
		first = place(VIEW_KIND_TWIDDLED_2D, 0, 2);
	}
	if ((format->kind & POWERVR_FORMAT_SRGB) != 0)
		first |= place(1, 3, 3);
	for (output = 0; output < 4; output++)
		first |= place(channel_select(format, view->mapping[output], output), 14U - 3U * output, 16U - 3U * output);
	first |= place(format->sample_code, 27, 33);
	first |= place(view->width - 1U, 34, 47);
	first |= place(view->height - 1U, 48, 61);

	/*
	 * A multisampled image's samples, log2: four samples a texel the unit
	 * reads by the sample number of a lookup.  Only a twiddled flat image
	 * has them.
	 *
	 * Values referenced: Mesa 26.2.6 src/imagination/csbgen/rogue/
	 * texstate.xml (IMAGE_WORD0 smpcnt 62-63) and
	 * src/imagination/vulkan/pvr_arch_tex_state.c (pvr_arch_pack_tex_state:
	 * smpcnt util_logbase2 of the sample count).
	 */
	if (view->layout->samples == 4U) {
		if (view->linear || view->kind != POWERVR_VIEW_FLAT)
			return EINVAL;
		first |= place(2, 62, 63);
	}

	/*
	 * The second word of a linear image: its level count, the address in
	 * four-byte units, its row in texels less one, and whether its alpha
	 * is the last channel (alpha_on_msb).
	 *
	 * Values referenced: Mesa 26.2.5 src/imagination/csbgen/rogue/
	 * texstate.xml (STRIDE_IMAGE_WORD1: stride 0-14, mipmaps_present 15,
	 * texaddr 16-53 shifted by 2, num_mip_levels 60-63) and Mesa 26.2.6
	 * (STRIDE_IMAGE_WORD1 and IMAGE_WORD1: alpha_msb 59).
	 */
	if (view->linear) {
		second = place(view->layout->padded_width - 1U, 0, 14);
		second |= place(address >> 2, 16, 53);
		if (alpha_on_msb(format))
			second |= place(1, 59, 59);
		second |= place(view->level_count, 60, 63);
		words[0] = first;
		words[1] = second;
		return 0;
	}

	/*
	 * The second word of a twiddled image: its level count, the layers
	 * less one (a cube's six faces, not divided) or a volume's level-0
	 * depth less one, whether it has levels, the address in four-byte
	 * units, whether its alpha is the last channel (alpha_on_msb) and
	 * the first level.
	 *
	 * Values referenced: Mesa 26.2.5 src/imagination/csbgen/rogue/
	 * texstate.xml (IMAGE_WORD1: num_mip_levels 0-3, depth 4-14,
	 * mipmaps_present 15, texaddr 16-53 shifted by 2, baselevel 60-63)
	 * and Mesa 26.2.6 src/imagination/vulkan/pvr_arch_tex_state.c
	 * (pvr_arch_pack_tex_state: depth the extent's depth less one for a
	 * 3D view, else the array size less one, divided by six only for a
	 * cube array).
	 */
	second = place(view->level_count, 0, 3);
	if (view->kind == POWERVR_VIEW_VOLUME) {
		second |= place(view->depth - 1U, 4, 14);
	} else {
		second |= place(view->layer_count - 1U, 4, 14);
	}
	if (view->image_levels > 1)
		second |= place(1, 15, 15);
	second |= place(address >> 2, 16, 53);
	if (alpha_on_msb(format))
		second |= place(1, 59, 59);
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

	/*
	 * The anisotropic filter's most samples: the largest of 16, 8, 4 and
	 * 2 not above the sampler's (a positive float's bits order as the
	 * value does), none below 2.
	 *
	 * Values referenced: Mesa 26.2.5 src/imagination/csbgen/rogue/
	 * texstate.xml (SAMPLER_WORD0 anisoctl 33-35; ANISOCTL DISABLED 0, X2
	 * 1, X4 2, X8 3, X16 4) and src/imagination/vulkan/pvr_arch_sampler.c
	 * (pvr_CreateSampler: the anisoctl by maxAnisotropy).
	 */
	if (sampler->anisotropy && (sampler->max_anisotropy_bits & 0x80000000U) == 0) {
		if (sampler->max_anisotropy_bits >= 0x41800000U) {
			words[0] |= place(4, 33, 35);
		} else if (sampler->max_anisotropy_bits >= 0x41000000U) {
			words[0] |= place(3, 33, 35);
		} else if (sampler->max_anisotropy_bits >= 0x40800000U) {
			words[0] |= place(2, 33, 35);
		} else if (sampler->max_anisotropy_bits >= 0x40000000U) {
			words[0] |= place(1, 33, 35);
		}
	}

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
 * word; the 32-bit float ones and the 32-bit float depth as their bits,
 * a word a channel; the 16-bit unorm depth as its 16 bits in the first
 * word (the colour's first channel the depth).  Other
 * formats are not taken (ENOTSUP).
 *
 * Procedure referenced:
 * Mesa 26.2.5 src/imagination/vulkan/pvr_arch_job_transfer.c
 * (pvr_pack_clear_color: PACKMODE_U8U8U8U8 with normalised channels,
 * pvr_float_to_ufixed: NaN, infinity and 1.0 or more to 255, below 0 to
 * 0, else floor(value * 255 + 0.5); PACKMODE_F32 the bits, PACKMODE_U16
 * normalised pvr_float_to_ufixed(value, 16)).
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

	/* 32-bit float channels and the float depth: the raw bits, one word a channel. */
	switch (format->vk_format) {
	case VK_FORMAT_R32_SFLOAT:
	case VK_FORMAT_R32G32_SFLOAT:
	case VK_FORMAT_R32G32B32A32_SFLOAT:
	case VK_FORMAT_D32_SFLOAT:
		for (channel = 0; channel < 4; channel++)
			packed[channel] = channel < format->texel_bytes / 4U ? colour[channel] : 0;
		return 0;

	/* The 16-bit unorm depth: the depth in 65535 steps in the first word. */
	case VK_FORMAT_D16_UNORM:
		packed[0] = unorm16_from_float(colour[0]);
		packed[1] = 0;
		packed[2] = 0;
		packed[3] = 0;
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
	if (!powervr_format_is_unorm8x4(format))
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

/*
 * Says whether a format is one of four 8-bit unsigned normalised
 * channels written as such (RGBA or BGRA order, not sRGB): the formats
 * the blit, the load of an attachment and the packed clear take.
 */
bool
powervr_format_is_unorm8x4(
	const struct powervr_format *format)
{
	/* The pixel back end's U8U8U8U8, four bytes, neither sRGB nor integer. */
	if (format == NULL || format->store_code != 0 || format->texel_bytes != 4)
		return false;
	if ((format->kind & (POWERVR_FORMAT_SRGB | POWERVR_FORMAT_INTEGER)) != 0)
		return false;

	/* Succeeded: it is. */
	return true;
}

/*
 * Packs a clear colour (four words: float bits for a normalised or float
 * format, the integers for an integer one) as the bits of one texel of
 * the format, in memory order: a texel of up to 32 bits in the low bits
 * of the first word, a larger one over the words in turn.  The transfer
 * writes them as the raw format of the texel's size, so it clears the
 * formats whose pixel back end mode no packed clear fills.  Normalised
 * channels round to the nearest step (clamped), half floats as
 * powervr_format_clear_words, 11- and 10-bit unsigned floats cut towards
 * zero (negative values and -0 to 0, values past the largest finite to
 * it), integers keep their low bits; the packed depth and stencil takes
 * the depth's float and the stencil's integer as its first two words.  Formats without such a packing
 * are not taken (ENOTSUP).
 *
 * Procedure referenced: the Vulkan 1.4 specification, "Fixed-Point Data
 * Conversions" (the conversion of a float to an unsigned and a signed
 * normalised value), "Unsigned 11-Bit Floating-Point Numbers" and
 * "Unsigned 10-Bit Floating-Point Numbers" (their exponent and mantissa
 * fields), and the formats' bit layouts ("Packed Formats":
 * A2B10G10R10 red in bits 0..9, B10G11R11 red in bits 0..10;
 * "Depth/Stencil Formats": D24_UNORM_S8_UINT the depth in bits 0..23).
 */
int
powervr_format_texel_words(
	const struct powervr_format *format,
	const uint32_t colour[4],
	uint32_t packed[4])
{
	uint32_t channels;
	uint32_t width;
	uint32_t kind;
	uint32_t value;
	uint32_t bit;
	unsigned channel;

	/* Refuses missing input or result. */
	if (format == NULL || colour == NULL || packed == NULL)
		return EINVAL;
	packed[0] = 0;
	packed[1] = 0;
	packed[2] = 0;
	packed[3] = 0;

	/* The packed formats of their own layouts. */
	switch (format->vk_format) {
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		packed[0] = unorm_from_float(colour[0], 1023U) | (unorm_from_float(colour[1], 1023U) << 10) |
		    (unorm_from_float(colour[2], 1023U) << 20) | (unorm_from_float(colour[3], 3U) << 30);
		return 0;
	case VK_FORMAT_A2B10G10R10_UINT_PACK32:
		packed[0] = (colour[0] & 0x3FFU) | ((colour[1] & 0x3FFU) << 10) | ((colour[2] & 0x3FFU) << 20) | (colour[3] << 30);
		return 0;
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
		packed[0] = small_float_from_float(colour[0], 6U) | (small_float_from_float(colour[1], 6U) << 11) |
		    (small_float_from_float(colour[2], 5U) << 22);
		return 0;
	case VK_FORMAT_D24_UNORM_S8_UINT:
		packed[0] = unorm_from_float(colour[0], 0xFFFFFFU) | (colour[1] << 24);
		return 0;
	default:
		break;
	}

	/* The formats of equal channels: their count, bits and kind. */
	if (!channel_layout(format->vk_format, &channels, &width, &kind))
		return ENOTSUP;

	/* Each channel at its place: channel c at bit c * width of the texel. */
	for (channel = 0; channel < channels; channel++) {
		if (kind == CHANNEL_UNORM) {
			value = unorm_from_float(colour[channel], (uint32_t)((1ULL << width) - 1U));
		} else if (kind == CHANNEL_SNORM) {
			value = snorm_from_float(colour[channel], (1U << (width - 1U)) - 1U);
		} else if (kind == CHANNEL_FLOAT && width == 16U) {
			value = half_from_float(colour[channel]);
		} else {
			value = colour[channel];
		}
		if (width < 32U)
			value &= (1U << width) - 1U;
		bit = channel * width;
		packed[bit / 32U] |= value << (bit % 32U);
	}

	/* Succeeded: the texel. */
	return 0;
}

/*
 * Reports how many pixel output register words a colour attachment of a
 * format takes (powervr_format_output_words), 0 for a format that is no
 * colour attachment: four 8-bit unorm channels, the formats of up to 32
 * bits a texel and their halves one; four half floats (sRGB, 16-bit
 * float, the 10-bit unorm packed) and three (the 11- and 10-bit float
 * packed), and texels of 64 bits two; texels of 128 bits four.
 */
uint32_t
powervr_format_output_word_count(
	const struct powervr_format *format)
{
	uint32_t packed[4];
	uint32_t zero[4];
	int error;

	/* A format without output words has none. */
	zero[0] = 0;
	zero[1] = 0;
	zero[2] = 0;
	zero[3] = 0;
	error = powervr_format_output_words(format, zero, packed);
	if (error != 0)
		return 0;

	/* The half-float forms of three or four channels. */
	if ((format->kind & POWERVR_FORMAT_SRGB) != 0)
		return 2;
	switch (format->vk_format) {
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
		return 2;
	default:
		break;
	}

	/* Succeeded: a word for each four bytes of the texel. */
	return ((uint32_t)format->texel_bytes + 3U) / 4U;
}

/*
 * Packs a clear colour (four words: float bits for a normalised or float
 * format, the integers for an integer one) as the words of the pixel
 * output registers a colour attachment of the format holds, the form the
 * compiler writes a fragment output in and the pixel back end stores
 * from: four 8-bit unorm channels as powervr_format_clear_words (red in
 * the low byte, a BGRA format's too); sRGB, the 16-bit float formats of
 * four channels and A2B10G10R10_UNORM (clamped to 0 to 1) as half
 * floats, two a word (sRGB clamped too); B10G11R11 as three half
 * floats; every other colour
 * attachment format as its texel's bits (powervr_format_texel_words).
 * Other formats are not taken (ENOTSUP).
 *
 * Procedure referenced: Mesa 26.2.6 src/imagination/pco/pco_nir_pvfio.c
 * (to_pbe_format: B8G8R8A8 written as R8G8B8A8, the sRGB formats and
 * R10G10B10A2_UNORM as R16G16B16A16_FLOAT, the sRGB ones saturated,
 * R11G11B10_FLOAT as R16G16B16_FLOAT, every other format as itself;
 * pack_to_format: the channels packed in the format's own layout) and
 * src/imagination/vulkan/pvr_usc.c (pvr_uscgen_loadop: a clear's words
 * written to the attachment's output registers as they are).
 */
int
powervr_format_output_words(
	const struct powervr_format *format,
	const uint32_t colour[4],
	uint32_t packed[4])
{
	uint32_t unit[4];
	unsigned channel;
	int error;

	/* Refuses missing input or result, and a format that is no colour attachment. */
	if (format == NULL || colour == NULL || packed == NULL)
		return EINVAL;
	if ((format->kind & POWERVR_FORMAT_COLOR) == 0 || (format->optimal_features & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0)
		return ENOTSUP;

	/* The packed formats written as half floats: the 10-bit unorm one clamped to 0 to 1, the small float one as it is. */
	switch (format->vk_format) {
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		for (channel = 0; channel < 4; channel++)
			unit[channel] = float_unit(colour[channel]);
		packed[0] = half_from_float(unit[0]) | (half_from_float(unit[1]) << 16);
		packed[1] = half_from_float(unit[2]) | (half_from_float(unit[3]) << 16);
		packed[2] = 0;
		packed[3] = 0;
		return 0;
	case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
		packed[0] = half_from_float(colour[0]) | (half_from_float(colour[1]) << 16);
		packed[1] = half_from_float(colour[2]);
		packed[2] = 0;
		packed[3] = 0;
		return 0;
	default:
		break;
	}

	/* sRGB: the clear's half floats of the values clamped to 0 to 1, as the compiler saturates them. */
	if ((format->kind & POWERVR_FORMAT_SRGB) != 0) {
		for (channel = 0; channel < 4; channel++)
			unit[channel] = float_unit(colour[channel]);
		error = powervr_format_clear_words(format, unit, packed);
		return error;
	}

	/* The four 8-bit unorm channels and the four half floats: the clear's own words. */
	if (powervr_format_is_unorm8x4(format) || format->vk_format == VK_FORMAT_R16G16B16A16_SFLOAT) {
		error = powervr_format_clear_words(format, colour, packed);
		return error;
	}

	/* Succeeded or not: every other format as its texel. */
	error = powervr_format_texel_words(format, colour, packed);
	return error;
}

/*
 * Clamps a float's bits to 0 to 1: negative values, -0 and NaN to 0,
 * values of 1 and more to 1 (the conversion to an unsigned normalised
 * value, Vulkan's "Fixed-Point Data Conversions").
 */
static uint32_t
float_unit(
	uint32_t bits)
{
	/* Negative, or not a number. */
	if ((bits & 0x80000000U) != 0 || bits > 0x7f800000U)
		return 0;

	/* One and more, infinity included. */
	if (bits >= 0x3f800000U)
		return 0x3f800000U;

	/* Succeeded: the value itself. */
	return bits;
}

/*
 * Tells whether a colour format's alpha is its last channel, the most
 * significant of its texel: the texture unit then takes the other
 * channels as the colour, which an sRGB format's gamma decodes; without
 * it the unit takes the first channel as the alpha and leaves red
 * undecoded (on the board an SRGB8_ALPHA8 texel of 0x80 read back red
 * 0.5, green and blue 0.216).
 *
 * Procedure referenced: Mesa 26.2.6 src/imagination/vulkan/
 * pvr_arch_tex_state.c (pvr_arch_pack_tex_state: alpha_msb when
 * vk_format_is_alpha_on_msb) and src/vulkan/util/vk_format.h
 * (vk_format_is_alpha_on_msb: an RGB or sRGB format whose alpha is its
 * fourth channel, W, on a little-endian host).
 */
static bool
alpha_on_msb(
	const struct powervr_format *format)
{
	/* A colour format whose alpha comes from its fourth channel. */
	return (format->kind & POWERVR_FORMAT_COLOR) != 0 && format->channels[3] == POWERVR_CHANNEL_3;
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

/*
 * Converts a float, as its bits, to 16-bit unorm without the processor's
 * floating point: NaN and below zero to 0, 1.0 or more to 65535, else
 * the value times 65535 rounded to the nearest, ties to even (the same
 * conversion as render-depth.c's background depth).
 */
static uint32_t
unorm16_from_float(
	uint32_t bits)
{
	uint64_t scaled;
	uint64_t whole;
	uint64_t rest;
	uint32_t exponent;
	uint32_t shift;

	/* NaN, below zero, 1.0 or more. */
	exponent = (bits >> 23) & 0xffU;
	if (exponent == 0xffU && (bits & 0x7fffffU) != 0)
		return 0;
	if ((bits & 0x80000000U) != 0)
		return 0;
	if (exponent >= 127U)
		return 65535;

	/* Denormals and tiny values round to zero. */
	shift = 150U - exponent;
	if (exponent == 0 || shift > 60U)
		return 0;

	/* value * 65535 = mantissa * 65535 / 2^shift, rounded to nearest, ties to even. */
	scaled = (uint64_t)((bits & 0x7fffffU) | 0x800000U) * 65535U;
	whole = scaled >> shift;
	rest = scaled & ((1ULL << shift) - 1U);
	if (rest > (1ULL << (shift - 1U)) || (rest == (1ULL << (shift - 1U)) && (whole & 1U) != 0))
		whole++;

	/* The value. */
	return (uint32_t)whole;
}

/*
 * Converts a float's bits to an unsigned normalised value of largest
 * steps: NaN and below zero to 0, 1.0 or more (and infinity) to largest,
 * else floor(value * largest + 0.5) computed exactly.
 */
static uint32_t
unorm_from_float(
	uint32_t bits,
	uint32_t largest)
{
	uint64_t scaled;
	uint32_t exponent;
	uint32_t shift;

	/* NaN, below zero, 1.0 or more. */
	exponent = (bits >> 23) & 0xffU;
	if (exponent == 0xffU && (bits & 0x7fffffU) != 0)
		return 0;
	if ((bits & 0x80000000U) != 0)
		return 0;
	if (exponent >= 127U)
		return largest;

	/* Denormals and tiny values round to zero. */
	shift = 150U - exponent;
	if (exponent == 0 || shift > 60U)
		return 0;

	/* floor(mantissa * largest / 2^shift + 0.5). */
	scaled = (uint64_t)((bits & 0x7fffffU) | 0x800000U) * largest;
	return (uint32_t)((scaled + (1ULL << (shift - 1U))) >> shift);
}

/*
 * Converts a float's bits to a signed normalised value of largest steps
 * each way, as a two's complement word: NaN to 0, the magnitude as
 * unorm_from_float (so -1.0 or less to -largest), the sign applied.
 */
static uint32_t
snorm_from_float(
	uint32_t bits,
	uint32_t largest)
{
	uint32_t magnitude;

	/* The magnitude, rounded and clamped. */
	magnitude = unorm_from_float(bits & 0x7fffffffU, largest);

	/* The sign. */
	if ((bits & 0x80000000U) != 0)
		return 0U - magnitude;
	return magnitude;
}

/*
 * Converts a float's bits to an unsigned float of a five-bit exponent
 * (bias 15) and mantissa_bits mantissa bits (6 for 11 bits, 5 for 10):
 * NaN to a NaN, negative values to 0, infinity to infinity, values past
 * the largest finite to it, the rest cut towards zero, denormalised
 * below 2^-14.
 */
static uint32_t
small_float_from_float(
	uint32_t bits,
	uint32_t mantissa_bits)
{
	uint32_t exponent;
	uint32_t mantissa;
	int32_t biased;
	uint32_t shift;

	/* NaN, the negative values, infinity. */
	exponent = (bits >> 23) & 0xffU;
	mantissa = bits & 0x7fffffU;
	if (exponent == 0xffU && mantissa != 0)
		return (0x1fU << mantissa_bits) | 1U;
	if ((bits & 0x80000000U) != 0)
		return 0;
	if (exponent == 0xffU)
		return 0x1fU << mantissa_bits;

	/* Past the largest finite value. */
	biased = (int32_t)exponent - 127 + 15;
	if (biased >= 31)
		return (30U << mantissa_bits) | ((1U << mantissa_bits) - 1U);

	/* Below the smallest normal: the mantissa with its hidden bit shifted down, exponent 0. */
	if (biased <= 0) {
		if (exponent == 0)
			return 0;
		shift = (uint32_t)(1 - biased);
		if (shift >= 24U)
			return 0;
		return ((mantissa | 0x800000U) >> shift) >> (23U - mantissa_bits);
	}

	/* A normal value. */
	return ((uint32_t)biased << mantissa_bits) | (mantissa >> (23U - mantissa_bits));
}

/*
 * Reports the layout of a format of equal channels in channel order:
 * their count, bits and kind.  False for a format of other channels (a
 * packed one, BGRA order, sRGB, depth).
 */
static bool
channel_layout(
	uint32_t vk_format,
	uint32_t *channels,
	uint32_t *width,
	uint32_t *kind)
{
	/* The kind and the bits. */
	switch (vk_format) {
	case VK_FORMAT_R8_UNORM:
	case VK_FORMAT_R8G8_UNORM:
	case VK_FORMAT_R8G8B8A8_UNORM:
		*kind = CHANNEL_UNORM;
		*width = 8;
		break;
	case VK_FORMAT_R8_SNORM:
	case VK_FORMAT_R8G8_SNORM:
	case VK_FORMAT_R8G8B8A8_SNORM:
		*kind = CHANNEL_SNORM;
		*width = 8;
		break;
	case VK_FORMAT_R8_UINT:
	case VK_FORMAT_R8_SINT:
	case VK_FORMAT_R8G8_UINT:
	case VK_FORMAT_R8G8_SINT:
	case VK_FORMAT_R8G8B8A8_UINT:
	case VK_FORMAT_R8G8B8A8_SINT:
		*kind = CHANNEL_INTEGER;
		*width = 8;
		break;
	case VK_FORMAT_R16_UNORM:
	case VK_FORMAT_R16G16_UNORM:
	case VK_FORMAT_R16G16B16A16_UNORM:
		*kind = CHANNEL_UNORM;
		*width = 16;
		break;
	case VK_FORMAT_R16_SNORM:
	case VK_FORMAT_R16G16_SNORM:
	case VK_FORMAT_R16G16B16A16_SNORM:
		*kind = CHANNEL_SNORM;
		*width = 16;
		break;
	case VK_FORMAT_R16_UINT:
	case VK_FORMAT_R16_SINT:
	case VK_FORMAT_R16G16_UINT:
	case VK_FORMAT_R16G16_SINT:
	case VK_FORMAT_R16G16B16A16_UINT:
	case VK_FORMAT_R16G16B16A16_SINT:
		*kind = CHANNEL_INTEGER;
		*width = 16;
		break;
	case VK_FORMAT_R16_SFLOAT:
	case VK_FORMAT_R16G16_SFLOAT:
	case VK_FORMAT_R16G16B16A16_SFLOAT:
		*kind = CHANNEL_FLOAT;
		*width = 16;
		break;
	case VK_FORMAT_R32_UINT:
	case VK_FORMAT_R32_SINT:
	case VK_FORMAT_R32G32_UINT:
	case VK_FORMAT_R32G32_SINT:
	case VK_FORMAT_R32G32B32A32_UINT:
	case VK_FORMAT_R32G32B32A32_SINT:
		*kind = CHANNEL_INTEGER;
		*width = 32;
		break;
	case VK_FORMAT_R32_SFLOAT:
	case VK_FORMAT_R32G32_SFLOAT:
	case VK_FORMAT_R32G32B32A32_SFLOAT:
		*kind = CHANNEL_FLOAT;
		*width = 32;
		break;
	default:
		return false;
	}

	/* The count: the formats are named by their channels. */
	switch (vk_format) {
	case VK_FORMAT_R8_UNORM:
	case VK_FORMAT_R8_SNORM:
	case VK_FORMAT_R8_UINT:
	case VK_FORMAT_R8_SINT:
	case VK_FORMAT_R16_UNORM:
	case VK_FORMAT_R16_SNORM:
	case VK_FORMAT_R16_UINT:
	case VK_FORMAT_R16_SINT:
	case VK_FORMAT_R16_SFLOAT:
	case VK_FORMAT_R32_UINT:
	case VK_FORMAT_R32_SINT:
	case VK_FORMAT_R32_SFLOAT:
		*channels = 1;
		break;
	case VK_FORMAT_R8G8_UNORM:
	case VK_FORMAT_R8G8_SNORM:
	case VK_FORMAT_R8G8_UINT:
	case VK_FORMAT_R8G8_SINT:
	case VK_FORMAT_R16G16_UNORM:
	case VK_FORMAT_R16G16_SNORM:
	case VK_FORMAT_R16G16_UINT:
	case VK_FORMAT_R16G16_SINT:
	case VK_FORMAT_R16G16_SFLOAT:
	case VK_FORMAT_R32G32_UINT:
	case VK_FORMAT_R32G32_SINT:
	case VK_FORMAT_R32G32_SFLOAT:
		*channels = 2;
		break;
	default:
		*channels = 4;
		break;
	}

	/* Succeeded: the layout. */
	return true;
}
