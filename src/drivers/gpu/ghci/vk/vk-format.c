/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The formats of the Vulkan executor: the formats the hardware takes,
 * their sizes, and their Vulkan format features.
 *
 * The features come from the format table of the capability block (one
 * word per VkFormat), which reports what the GPU implements; a format it
 * reports nothing for has no feature, and no format is assumed.  The
 * driver then drops what it cannot do itself: an sRGB format without
 * EXT_CAP_FMT0's sRGB bit, a storage image the compiler cannot address,
 * and the blits of a device without the blit engine are what the
 * driver's own shaders can do instead (sampled sources, colour
 * attachment destinations, no depth).  A format Vulkan 1.0 requires to
 * filter linearly says it does whenever it is sampled: where the texture
 * unit cannot filter it, the sampler takes the nearest texel instead.
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/lock.h>
#include <uapi/errno.h>

#include "vk.h"

/*
 * The bits of a format word's OPTIMAL and LINEAR fields.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 4.1,
 * FMT_OPTIMAL bits 0 to 8, which the format table's word keeps).
 */
#define HW_SAMPLE			(1U << 0)
#define HW_FILTER_LINEAR		(1U << 1)
#define HW_COLOR_ATTACHMENT		(1U << 2)
#define HW_BLEND			(1U << 3)
#define HW_DEPTH_STENCIL		(1U << 4)
#define HW_VERTEX_BUFFER		(1U << 5)
#define HW_BLIT_SRC			(1U << 6)
#define HW_BLIT_DST			(1U << 7)
#define HW_STORAGE_IMAGE		(1U << 8)

/*
 * The bits of the buffer features, once moved from the format word's
 * BUFFER field to their place: the buffer half of the hardware's
 * VkFormatFeatureFlags, at Vulkan's positions (not OPTIMAL's).
 *
 * Values referenced: amaterrace-gpu 203075d6 rtl/gpu/cp/atg_cp_fmt.v
 * (FMT_BUFFER is {19'd0, feat[25:13]}) and include/atg_fmt_info.vh
 * (atg_fmt_feat: {buffer[12:0], optimal[12:0]} as VkFormatFeatureFlags);
 * Vulkan 1.0 VkFormatFeatureFlagBits (bits 3 to 6).
 */
#define HW_BUFFER_UNIFORM_TEXEL		(1U << 3)
#define HW_BUFFER_STORAGE_TEXEL		(1U << 4)
#define HW_BUFFER_STORAGE_ATOMIC	(1U << 5)
#define HW_BUFFER_VERTEX		(1U << 6)

/*
 * What a row of the table says about its format: a 4x4 block, integer,
 * float, sRGB or depth, and whether the compiler addresses it as a
 * storage image (ROW_STORAGE, the driver's own limit).
 */
#define ROW_BLOCK			(1U << 0)
#define ROW_INTEGER			(1U << 1)
#define ROW_FLOAT			(1U << 2)
#define ROW_FLOAT32			(1U << 3)
#define ROW_SRGB			(1U << 4)
#define ROW_DEPTH			(1U << 5)
#define ROW_STORAGE			(1U << 6)

/*
 * The formats Vulkan 1.0 requires to be sampled with the linear filter on
 * optimal tiling (of those the hardware takes), which the driver reports
 * as filtering whenever they are sampled, and draws with the nearest
 * texel where the texture unit has no filter for them.
 *
 * Values referenced: the Vulkan 1.0 specification (section 31.3.3, the
 * required format support tables: SAMPLED_IMAGE_FILTER_LINEAR_BIT).
 */
#define ROW_FILTER_REQUIRED		(1U << 7)

/* One format of the table: its VkFormat, its texel (or 4x4 block) bytes, and what kind of format it is. */
struct format_row {
	uint16_t format;
	uint8_t bytes;
	uint16_t kind;
};

/*
 * The formats the hardware takes, which the driver can write into a
 * descriptor.  A8B8G8R8 (51, 52, 55, 56, 57) is taken as R8G8B8A8 (37,
 * 38, 41, 42, 43) is.  Storage comes from the specification's table
 * alone: the shader core writes storage images of these formats.
 *
 * Values referenced: amaterrace-gpu 93a7ac06 ghci.md (section 8.5.1,
 * the table of formats, and its notes on the packed 32-bit formats);
 * Vulkan 1.0 VkFormat (R5G6B5_UNORM_PACK16 is 4, A1R5G5B5_UNORM_PACK16
 * is 8, as section 8.5.1 has them, where section 8.3's example says 84).
 */
static const struct format_row format_rows[] = {
	{ 3U, 2U, 0U },
	{ 4U, 2U, ROW_FILTER_REQUIRED },
	{ 8U, 2U, ROW_FILTER_REQUIRED },
	{ 9U, 1U, ROW_FILTER_REQUIRED },
	{ 10U, 1U, ROW_FILTER_REQUIRED },
	{ 13U, 1U, ROW_INTEGER },
	{ 14U, 1U, ROW_INTEGER },
	{ 16U, 2U, ROW_FILTER_REQUIRED },
	{ 17U, 2U, ROW_FILTER_REQUIRED },
	{ 20U, 2U, ROW_INTEGER },
	{ 21U, 2U, ROW_INTEGER },
	{ 37U, 4U, ROW_STORAGE | ROW_FILTER_REQUIRED },
	{ 38U, 4U, ROW_FILTER_REQUIRED },
	{ 41U, 4U, ROW_INTEGER | ROW_STORAGE },
	{ 42U, 4U, ROW_INTEGER | ROW_STORAGE },
	{ 43U, 4U, ROW_SRGB | ROW_FILTER_REQUIRED },
	{ 44U, 4U, ROW_FILTER_REQUIRED },
	{ 50U, 4U, ROW_SRGB | ROW_FILTER_REQUIRED },
	{ 51U, 4U, ROW_STORAGE | ROW_FILTER_REQUIRED },
	{ 52U, 4U, ROW_FILTER_REQUIRED },
	{ 55U, 4U, ROW_INTEGER | ROW_STORAGE },
	{ 56U, 4U, ROW_INTEGER | ROW_STORAGE },
	{ 57U, 4U, ROW_SRGB | ROW_FILTER_REQUIRED },
	{ 64U, 4U, ROW_FILTER_REQUIRED },
	{ 68U, 4U, ROW_INTEGER },
	{ 70U, 2U, 0U },
	{ 71U, 2U, 0U },
	{ 74U, 2U, ROW_INTEGER },
	{ 75U, 2U, ROW_INTEGER },
	{ 76U, 2U, ROW_FLOAT | ROW_STORAGE | ROW_FILTER_REQUIRED },
	{ 77U, 4U, 0U },
	{ 78U, 4U, 0U },
	{ 81U, 4U, ROW_INTEGER },
	{ 82U, 4U, ROW_INTEGER },
	{ 83U, 4U, ROW_FLOAT | ROW_STORAGE | ROW_FILTER_REQUIRED },
	{ 91U, 8U, 0U },
	{ 92U, 8U, 0U },
	{ 95U, 8U, ROW_INTEGER | ROW_STORAGE },
	{ 96U, 8U, ROW_INTEGER | ROW_STORAGE },
	{ 97U, 8U, ROW_FLOAT | ROW_STORAGE | ROW_FILTER_REQUIRED },
	{ 98U, 4U, ROW_INTEGER | ROW_STORAGE },
	{ 99U, 4U, ROW_INTEGER | ROW_STORAGE },
	{ 100U, 4U, ROW_FLOAT | ROW_FLOAT32 | ROW_STORAGE },
	{ 101U, 8U, ROW_INTEGER | ROW_STORAGE },
	{ 102U, 8U, ROW_INTEGER | ROW_STORAGE },
	{ 103U, 8U, ROW_FLOAT | ROW_FLOAT32 },
	{ 104U, 12U, ROW_INTEGER },
	{ 105U, 12U, ROW_INTEGER },
	{ 106U, 12U, ROW_FLOAT | ROW_FLOAT32 },
	{ 107U, 16U, ROW_INTEGER | ROW_STORAGE },
	{ 108U, 16U, ROW_INTEGER | ROW_STORAGE },
	{ 109U, 16U, ROW_FLOAT | ROW_FLOAT32 | ROW_STORAGE },
	{ 122U, 4U, ROW_FILTER_REQUIRED },
	{ 123U, 4U, ROW_FILTER_REQUIRED },
	{ 124U, 2U, ROW_DEPTH },
	{ 125U, 4U, ROW_DEPTH },
	{ 126U, 4U, ROW_DEPTH },
	{ 129U, 4U, ROW_DEPTH },
	{ 130U, 8U, ROW_DEPTH },
	{ 131U, 8U, ROW_BLOCK },
	{ 133U, 8U, ROW_BLOCK },
	{ 135U, 16U, ROW_BLOCK },
	{ 137U, 16U, ROW_BLOCK },
	{ 139U, 8U, ROW_BLOCK },
	{ 141U, 16U, ROW_BLOCK },
	{ 145U, 16U, ROW_BLOCK },
	{ 147U, 8U, ROW_BLOCK },
	{ 148U, 8U, ROW_BLOCK | ROW_SRGB },
	{ 149U, 8U, ROW_BLOCK },
	{ 150U, 8U, ROW_BLOCK | ROW_SRGB },
	{ 151U, 16U, ROW_BLOCK },
	{ 152U, 16U, ROW_BLOCK | ROW_SRGB },
	{ 153U, 8U, ROW_BLOCK },
	{ 154U, 8U, ROW_BLOCK },
	{ 155U, 16U, ROW_BLOCK },
	{ 156U, 16U, ROW_BLOCK }
};

/*
 * The components of the VkFormat values first to last: a run of formats
 * of the same channels.
 *
 * Values referenced: Vulkan 1.0 VkFormat (the order of the enumeration,
 * R4G4 at 1 to D32_SFLOAT_S8_UINT at 130).
 */
struct format_channels {
	uint16_t first;
	uint16_t last;
	uint32_t components;
};

/* The runs of formats by their components; the block formats have four. */
static const struct format_channels format_channel_runs[] = {
	{ 1U, 1U, 2U },
	{ 2U, 3U, 4U },
	{ 4U, 5U, 3U },
	{ 6U, 8U, 4U },
	{ 9U, 15U, 1U },
	{ 16U, 22U, 2U },
	{ 23U, 36U, 3U },
	{ 37U, 69U, 4U },
	{ 70U, 76U, 1U },
	{ 77U, 83U, 2U },
	{ 84U, 90U, 3U },
	{ 91U, 97U, 4U },
	{ 98U, 100U, 1U },
	{ 101U, 103U, 2U },
	{ 104U, 106U, 3U },
	{ 107U, 109U, 4U },
	{ 110U, 112U, 1U },
	{ 113U, 115U, 2U },
	{ 116U, 118U, 3U },
	{ 119U, 121U, 4U },
	{ 122U, 123U, 3U },
	{ 124U, 127U, 1U },
	{ 128U, 130U, 2U },
	{ 131U, 184U, 4U }
};

static const struct format_row *row_of(uint32_t format);
static uint32_t buffer_features(const struct format_row *row, uint32_t buffer, uint32_t driver);
static uint32_t vulkan_bits(uint32_t hw);

/*
 * Reports the bytes of a texel (or of a 4x4 block, block 1) of a format
 * the hardware takes.  Returns 0, or ENOTSUP for another format.
 */
int
ghci_vk_format_size(
	uint32_t format,
	uint32_t *bytes,
	uint32_t *block)
{
	const struct format_row *row;

	/* A format of the table. */
	row = row_of(format);
	if (row == NULL)
		return ENOTSUP;

	/* Its texel or block. */
	*bytes = row->bytes;
	*block = 0U;
	if ((row->kind & ROW_BLOCK) != 0U)
		*block = 1U;

	/* Succeeded: the size. */
	return 0;
}

/*
 * Reports the components of a format the hardware takes (0 for another
 * format) and what it is besides (GHCI_VK_FORMAT_*).
 */
uint32_t
ghci_vk_format_traits(
	uint32_t format,
	uint32_t *flags)
{
	const struct format_row *row;
	uint32_t components;
	size_t index;

	/* A format of the table. */
	*flags = 0U;
	row = row_of(format);
	if (row == NULL)
		return 0U;

	/* What its row says it is. */
	if ((row->kind & ROW_INTEGER) != 0U)
		*flags |= GHCI_VK_FORMAT_INTEGER;
	if ((row->kind & ROW_SRGB) != 0U)
		*flags |= GHCI_VK_FORMAT_SRGB;
	if ((row->kind & ROW_DEPTH) != 0U)
		*flags |= GHCI_VK_FORMAT_DEPTH;
	if ((row->kind & ROW_BLOCK) != 0U)
		*flags |= GHCI_VK_FORMAT_BLOCK;

	/* The run of formats it is in. */
	components = 0U;
	for (index = 0U; index < sizeof(format_channel_runs) / sizeof(format_channel_runs[0]); index++) {
		if (format >= format_channel_runs[index].first && format <= format_channel_runs[index].last) {
			components = format_channel_runs[index].components;
			break;
		}
	}

	/* Succeeded: its components. */
	return components;
}

/*
 * Makes one format's Vulkan features from its format word's fields for
 * optimal and linear tiling (OPTIMAL's bits) and for buffers (at
 * Vulkan's positions), and drops what the driver cannot do: a format
 * outside the table, an sRGB format without EXT_CAP_FMT0's bit, a
 * storage image the compiler does not address, and a blit the driver's
 * shaders replace where the executor does not use the blit engine.  The
 * linear filter is the texture unit's, but a sampled format Vulkan 1.0
 * requires to filter has it in any case (the nearest texel where the
 * texture unit has no filter).  The driver lays every image out in
 * linear rows, so an image made with optimal tiling is a linear one:
 * optimal tiling has only what both tilings have.
 */
void
ghci_vk_formats_from_bits(
	const struct ghci_caps *caps,
	uint32_t format,
	uint32_t optimal,
	uint32_t linear,
	uint32_t buffer,
	struct ghci_vk_formats *formats)
{
	const struct format_row *row;
	uint32_t driver;
	uint32_t tilings[2];
	unsigned index;
	int blitter;
	int bilinear;
	int required;
	int etc2;
	int srgb;

	/* A format the driver cannot write into a descriptor has no feature. */
	row = row_of(format);
	if (row == NULL || format >= GHCI_VK_FORMATS)
		return;

	/* An sRGB format needs the sRGB bit of EXT_CAP_FMT0. */
	srgb = ghci_caps_has(caps, GHCI_HAS_SRGB);
	if ((row->kind & ROW_SRGB) != 0U && !srgb)
		return;

	/* What the driver itself can do with the format (storage only where the compiler addresses it); optimal images are linear. */
	driver = ~(uint32_t)HW_STORAGE_IMAGE;
	if ((row->kind & ROW_STORAGE) != 0U)
		driver = ~0U;
	blitter = ghci_vk_caps_blitter(caps);
	tilings[0] = optimal & linear & driver;
	tilings[1] = linear & driver;

	/*
	 * Without the bilinear filter the texture unit samples the nearest
	 * texel only, so no format filters, whatever the table says.
	 *
	 * Values referenced: amaterrace-gpu ad23fe0 docs/ghci.md (section
	 * 3.2.2, EXT_CAP_FMT0 [16] CAP_TU_BILINEAR; section 4.1, FMT_OPTIMAL
	 * and FMT_LINEAR report no FILTER_LINEAR without it).
	 */
	bilinear = ghci_caps_has(caps, GHCI_HAS_TU_BILINEAR);
	for (index = 0U; index < 2U && !bilinear; index++)
		tilings[index] &= ~(uint32_t)HW_FILTER_LINEAR;

	/*
	 * A format Vulkan 1.0 requires to filter, and the ETC2 and EAC
	 * formats where the device names textureCompressionETC2 (which
	 * requires them to filter), filters whenever it is sampled: the
	 * texture unit without a filter for it (no bilinear filter, or a
	 * float format without the float filter) samples the nearest texel,
	 * and the sampler asks for nothing else.
	 *
	 * Values referenced: amaterrace-gpu 93a7ac06 ghci.md (section 3.2.2,
	 * EXT_CAP_FMT0 [16] CAP_TU_BILINEAR, nearest sampling without it;
	 * section 12.9, FP16 and FP32 textures sampled at the nearest texel
	 * without CAP_TEX_FFILT); the Vulkan 1.0 specification (section
	 * 31.3.3, textureCompressionETC2's formats filter linearly).
	 */
	etc2 = ghci_caps_has(caps, GHCI_HAS_ETC2);
	required = 0;
	if ((row->kind & ROW_FILTER_REQUIRED) != 0U) {
		required = 1;
	} else if (format >= (uint32_t)VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && format <= (uint32_t)VK_FORMAT_EAC_R11G11_SNORM_BLOCK) {
		/* Named with the sRGB bit too (the feature has sRGB formats). */
		if (etc2 && srgb)
			required = 1;
	}
	for (index = 0U; index < 2U && required; index++) {
		if ((tilings[index] & HW_SAMPLE) != 0U)
			tilings[index] |= HW_FILTER_LINEAR;
	}

	/* Without the executor's blit engine the blits are the driver's draws: a sampled source, a colour destination. */
	for (index = 0U; index < 2U && !blitter; index++) {
		tilings[index] &= ~(HW_BLIT_SRC | HW_BLIT_DST);
		if ((tilings[index] & HW_SAMPLE) != 0U && (row->kind & ROW_DEPTH) == 0U)
			tilings[index] |= HW_BLIT_SRC;
		if ((tilings[index] & HW_COLOR_ATTACHMENT) != 0U)
			tilings[index] |= HW_BLIT_DST;
	}

	/* The tilings' features, and copies of any format that has one. */
	formats->optimal[format] = vulkan_bits(tilings[0]);
	formats->linear[format] = vulkan_bits(tilings[1]);
	if (formats->optimal[format] != 0U)
		formats->optimal[format] |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
	if (formats->linear[format] != 0U)
		formats->linear[format] |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

	/* The buffers' features. */
	formats->buffer[format] = buffer_features(row, buffer, driver);
}

/*
 * Makes the features of every format of the table from the format table
 * of the capability block: a format the block reports nothing for, or
 * one past its table, has no feature.
 */
void
ghci_vk_formats_make(
	const struct ghci_caps *caps,
	struct ghci_vk_formats *formats)
{
	uint32_t format;
	uint32_t word;
	uint32_t optimal;
	uint32_t linear;
	uint32_t buffer;
	size_t index;

	/* No format has a feature the block does not report. */
	kern_memset(formats, 0, sizeof(*formats));

	/*
	 * Each format of the table from its word: OPTIMAL, LINEAR, and the
	 * buffer features moved to their Vulkan positions (bits 3 to 6).
	 *
	 * Values referenced: amaterrace-gpu 93a7ac06 ghci.md (section 3.2.5,
	 * the format word: [8:0] OPTIMAL, [17:9] LINEAR, [21:18] the buffer
	 * features 3 to 6).
	 */
	for (index = 0U; index < sizeof(format_rows) / sizeof(format_rows[0]); index++) {
		/* A format past the block's table has no feature. */
		format = format_rows[index].format;
		if (format >= caps->format_count)
			continue;

		/* The word's three fields. */
		word = caps->formats[format];
		optimal = word & GHCI_CAPS_FMT_OPTIMAL_MASK;
		linear = (word >> GHCI_CAPS_FMT_LINEAR_SHIFT) & GHCI_CAPS_FMT_OPTIMAL_MASK;
		buffer = ((word >> GHCI_CAPS_FMT_BUFFER_SHIFT) & GHCI_CAPS_FMT_BUFFER_MASK) << GHCI_CAPS_FMT_BUFFER_FIRST;

		/* Its Vulkan features, less what the driver cannot do. */
		ghci_vk_formats_from_bits(caps, format, optimal, linear, buffer, formats);
	}
}

/*
 * Reports how many formats have a feature on either tiling or in
 * buffers.
 */
uint32_t
ghci_vk_formats_count(
	const struct ghci_vk_formats *formats)
{
	uint32_t count;
	uint32_t format;

	/* Each VkFormat the executor keeps. */
	count = 0U;
	for (format = 0U; format < GHCI_VK_FORMATS; format++) {
		if (formats->optimal[format] != 0U ||
		    formats->linear[format] != 0U ||
		    formats->buffer[format] != 0U)
			count++;
	}

	/* Succeeded: the formats with a feature. */
	return count;
}

/* Finds the row of a format; NULL for a format the hardware does not take. */
static const struct format_row *
row_of(
	uint32_t format)
{
	size_t index;

	/* The table in order. */
	for (index = 0U; index < sizeof(format_rows) / sizeof(format_rows[0]); index++) {
		if (format_rows[index].format == format)
			return &format_rows[index];
	}

	/* Succeeded: none. */
	return NULL;
}

/*
 * Reports the Vulkan buffer features of a format word's buffer bits for
 * a format of the table, with what the driver cannot do dropped: no
 * texel buffer of a block or depth format, a storage texel buffer only
 * where the compiler addresses the format as storage (driver), and its
 * atomics only on the 32-bit integer formats the compiler's image
 * atomics take.
 */
static uint32_t
buffer_features(
	const struct format_row *row,
	uint32_t buffer,
	uint32_t driver)
{
	uint32_t features;
	uint32_t components;
	uint32_t flags;

	/* Vertices. */
	features = 0U;
	if ((buffer & HW_BUFFER_VERTEX) != 0U)
		features |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;

	/* A block or depth format has no texel buffer. */
	if ((row->kind & (ROW_BLOCK | ROW_DEPTH)) != 0U)
		return features;

	/* Texel buffers the texture unit samples, and storage texel buffers the table allows. */
	if ((buffer & HW_BUFFER_UNIFORM_TEXEL) != 0U)
		features |= VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
	if ((buffer & HW_BUFFER_STORAGE_TEXEL) == 0U || (driver & HW_STORAGE_IMAGE) == 0U)
		return features;
	features |= VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;

	/*
	 * Atomics on a storage texel buffer: the compiler lowers an image
	 * atomic only on a single 32-bit integer component.
	 */
	components = ghci_vk_format_traits(row->format, &flags);
	if ((buffer & HW_BUFFER_STORAGE_ATOMIC) == 0U || (flags & GHCI_VK_FORMAT_INTEGER) == 0U)
		return features;
	if (components != 1U || row->bytes != 4U)
		return features;
	features |= VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT;

	/* Succeeded: the features. */
	return features;
}

/*
 * Reports the Vulkan format features of OPTIMAL's (or LINEAR's) bits.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 4.1,
 * the Vulkan feature each bit of FMT_OPTIMAL stands for).
 */
static uint32_t
vulkan_bits(
	uint32_t hw)
{
	uint32_t features;

	/* Each bit to its Vulkan feature; the filter and the blend only with what they qualify. */
	features = 0U;
	if ((hw & HW_SAMPLE) != 0U)
		features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
	if ((hw & HW_FILTER_LINEAR) != 0U && (hw & HW_SAMPLE) != 0U)
		features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
	if ((hw & HW_COLOR_ATTACHMENT) != 0U)
		features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
	if ((hw & HW_BLEND) != 0U && (hw & HW_COLOR_ATTACHMENT) != 0U)
		features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
	if ((hw & HW_DEPTH_STENCIL) != 0U)
		features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
	if ((hw & HW_BLIT_SRC) != 0U)
		features |= VK_FORMAT_FEATURE_BLIT_SRC_BIT;
	if ((hw & HW_BLIT_DST) != 0U)
		features |= VK_FORMAT_FEATURE_BLIT_DST_BIT;
	if ((hw & HW_STORAGE_IMAGE) != 0U)
		features |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;

	/* Succeeded: the features. */
	return features;
}
