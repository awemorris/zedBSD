/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The formats the executor takes, as the GPU names them: a texture's and
 * a vertex attribute's pixel format, and a render target's internal and
 * writeback formats.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/mali/vk/vk.h"

/*
 * A pixel format word: the format number in bits 12..19, sRGB in bit
 * 20, the component order in the low bits; and the order numbers used.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/lib/pan_format.h (MALI_PACK_FMT for PAN_ARCH
 * 7 and on); src/panfrost/genxml/v10.xml (enum "RGB Component Order":
 * RGBA 0, BGRA 4, RGB1 16).
 */
#define PIXEL(format, order, srgb)	(((uint32_t)(format) << 12) | ((uint32_t)(srgb) << 20) | (uint32_t)(order))
#define ORDER_RGBA			0U
#define ORDER_BGRA			4U
#define ORDER_RGB1			16U

/*
 * A render target's component swizzle, three bits a channel (R 0, G 1,
 * B 2, A 3), the inverse of the format's: identity, and red and blue
 * exchanged.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (enum "Channel");
 * src/panfrost/lib/pan_desc.c (get_rt_formats: the inverted swizzle).
 */
#define SWIZZLE_IDENTITY		0x688U
#define SWIZZLE_SWAP_RB			0x60aU

/*
 * The table.
 *
 * Values referenced:
 * Mesa 26.2.6 src/panfrost/genxml/v10.xml (enum "Format": R8 UNORM 163,
 * RG8 UNORM 171, RGBA8 UNORM 187, RGBA16F 223, R32UI 133, R32F 167,
 * RG32F 175, RGB32F 183, RGBA32F 191, RGBA8UI 155, R16F 199, RG16F 207,
 * R32I 197, Z16 UNORM 75, Z32 76;
 * enum "Color Buffer Internal Format": R8G8B8A8 1; enum "Color Format":
 * R8 16, R8G8 17, R8G8B8A8 19); src/panfrost/lib/pan_format.c (the
 * Valhall format and blendable tables: the order of each format; the
 * depth formats sampled as Z16 UNORM and Z32 in RGBA order on v9 and
 * later); the Vulkan specification (the formats a storage image must
 * take, and R32_UINT and R32_SINT the atomic ones).  X8_D24 and
 * D24_UNORM_S8 are only depth and stencil attachments (no pixel format:
 * never sampled).
 */
static const struct mali_vk_format formats[] = {
	{ VK_FORMAT_R8_UNORM, 1, PIXEL(163, ORDER_RGB1, 0), 1, 16, SWIZZLE_IDENTITY, true, true, true, false, false, false },
	{ VK_FORMAT_R8G8_UNORM, 2, PIXEL(171, ORDER_RGB1, 0), 1, 17, SWIZZLE_IDENTITY, true, true, true, false, false, false },
	{ VK_FORMAT_R8G8B8A8_UNORM, 4, PIXEL(187, ORDER_RGBA, 0), 1, 19, SWIZZLE_IDENTITY, true, true, true, true, false, false },
	{ VK_FORMAT_R8G8B8A8_SRGB, 4, PIXEL(187, ORDER_RGBA, 1), 1, 19, SWIZZLE_IDENTITY, true, true, false, false, false, false },
	{ VK_FORMAT_B8G8R8A8_UNORM, 4, PIXEL(187, ORDER_BGRA, 0), 1, 19, SWIZZLE_SWAP_RB, true, true, true, false, false, false },
	{ VK_FORMAT_B8G8R8A8_SRGB, 4, PIXEL(187, ORDER_BGRA, 1), 1, 19, SWIZZLE_SWAP_RB, true, true, false, false, false, false },
	{ VK_FORMAT_R8G8B8A8_UINT, 4, PIXEL(155, ORDER_RGBA, 0), 0, 0, 0, false, true, true, true, false, false },
	{ VK_FORMAT_R16_SFLOAT, 2, PIXEL(199, ORDER_RGB1, 0), 0, 0, 0, false, true, true, false, false, false },
	{ VK_FORMAT_R16G16_SFLOAT, 4, PIXEL(207, ORDER_RGB1, 0), 0, 0, 0, false, true, true, false, false, false },
	{ VK_FORMAT_R16G16B16A16_SFLOAT, 8, PIXEL(223, ORDER_RGBA, 0), 0, 0, 0, false, true, true, true, false, false },
	{ VK_FORMAT_R32_UINT, 4, PIXEL(133, ORDER_RGB1, 0), 0, 0, 0, false, true, true, true, true, false },
	{ VK_FORMAT_R32_SINT, 4, PIXEL(197, ORDER_RGB1, 0), 0, 0, 0, false, true, false, true, true, false },
	{ VK_FORMAT_R32_SFLOAT, 4, PIXEL(167, ORDER_RGB1, 0), 0, 0, 0, false, true, true, true, false, false },
	{ VK_FORMAT_R32G32_SFLOAT, 8, PIXEL(175, ORDER_RGB1, 0), 0, 0, 0, false, true, true, true, false, false },
	{ VK_FORMAT_R32G32B32_SFLOAT, 12, PIXEL(183, ORDER_RGB1, 0), 0, 0, 0, false, false, true, false, false, false },
	{ VK_FORMAT_R32G32B32A32_SFLOAT, 16, PIXEL(191, ORDER_RGBA, 0), 0, 0, 0, false, true, true, true, false, false },
	{ VK_FORMAT_D16_UNORM, 2, PIXEL(75, ORDER_RGBA, 0), 0, 0, 0, false, true, false, false, false, true },
	{ VK_FORMAT_D32_SFLOAT, 4, PIXEL(76, ORDER_RGBA, 0), 0, 0, 0, false, true, false, false, false, true },
	{ VK_FORMAT_X8_D24_UNORM_PACK32, 4, 0, 0, 0, 0, false, false, false, false, false, true },
	{ VK_FORMAT_D24_UNORM_S8_UINT, 4, 0, 0, 0, 0, false, false, false, false, false, true },
};

/* Finds a format the executor takes, or NULL. */
const struct mali_vk_format *
mali_vk_format_find(
	uint32_t format)
{
	size_t index;

	/* A walk of the table. */
	for (index = 0; index < sizeof(formats) / sizeof(formats[0]); index++) {
		if (formats[index].vk == format)
			return &formats[index];
	}

	/* Not one of them. */
	return NULL;
}

/*
 * Reports a format's features in an image (every image is linear, so the
 * optimal and linear features are the same) or, with linear false, its
 * buffer features.
 */
uint32_t
mali_vk_format_features(
	const struct mali_vk_format *format,
	bool linear)
{
	uint32_t features;

	/* No format, no feature. */
	if (format == NULL)
		return 0;

	/* A buffer's: a vertex attribute. */
	if (!linear) {
		features = 0;
		if (format->vertex)
			features |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
		return features;
	}

	/* Every image format is copied; the others as the table says. */
	features = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
	if (format->sample && format->depth)
		features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
	if (format->sample && !format->depth)
		features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
	if (format->storage)
		features |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
	if (format->atomic)
		features |= VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT;
	if (format->render)
		features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
	if (mali_surface_zs_find(format->vk) != NULL)
		features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;

	/* Succeeded: the features. */
	return features;
}
