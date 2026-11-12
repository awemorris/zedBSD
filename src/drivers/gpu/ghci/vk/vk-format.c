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
 * The features come from the FMT block (one VkFormat at a time through
 * FMT_INDEX) when the device has it, and from the specification's table
 * of formats narrowed by the capability flags when it does not.  Either
 * way the driver then drops what it cannot do itself: an sRGB format
 * without EXT_CAP_FMT0's sRGB bit, a storage image the compiler cannot
 * address, and the blits of a device without the blit engine are what
 * the driver's own shaders can do instead (sampled sources, colour
 * attachment destinations, no depth).
 */

#include <stddef.h>
#include <stdint.h>

#include <kern/kcrt.h>
#include <kern/lock.h>
#include <uapi/errno.h>

#include "vk.h"

/*
 * The registers of the FMT block, at the block's offset.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 4.1,
 * FMT: FMT_INDEX, FMT_OPTIMAL, FMT_LINEAR, FMT_BUFFER).
 */
#define FMT_INDEX			0x0U
#define FMT_OPTIMAL			0x4U
#define FMT_LINEAR			0x8U
#define FMT_BUFFER			0xCU

/*
 * The bits of FMT_OPTIMAL, FMT_LINEAR and FMT_BUFFER.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 4.1,
 * FMT_OPTIMAL bits 0 to 8).
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

/* What a row of the table says about its format beyond the features. */
#define ROW_BLOCK			(1U << 0)
#define ROW_INTEGER			(1U << 1)
#define ROW_FLOAT			(1U << 2)
#define ROW_FLOAT32			(1U << 3)
#define ROW_SRGB			(1U << 4)
#define ROW_DEPTH			(1U << 5)

/* The guards of the table: none, an EXT_CAP_FMT0 bit (BC1 to BC7 are bits 0 to 6), or ETC2. */
#define GUARD_NONE			0xFFU
#define GUARD_ETC2			7U

/* The features most rows share, as FMT_OPTIMAL's bits. */
#define COLOR_FILTER_BLEND		(HW_SAMPLE | HW_FILTER_LINEAR | HW_COLOR_ATTACHMENT | HW_BLEND)
#define SAMPLE_FILTER			(HW_SAMPLE | HW_FILTER_LINEAR)
#define SAMPLE_COLOR			(HW_SAMPLE | HW_COLOR_ATTACHMENT)

/*
 * One format of the table: its VkFormat, its texel (or 4x4 block) bytes,
 * the EXT_CAP_FMT0 bit it needs, the features the table gives it
 * (FMT_OPTIMAL's bits), and what kind of format it is.
 */
struct format_row {
	uint16_t format;
	uint8_t bytes;
	uint8_t guard;
	uint16_t features;
	uint16_t kind;
};

/*
 * The formats the hardware takes and the features the table gives them,
 * before the capability flags narrow them: CAP_ROP_PACK the colour
 * attachments of integer and 32-bit float formats, CAP_TEX_FFILT the
 * linear filter of 16- and 32-bit float formats (CAP_FFILT_FP32 too for
 * the 32-bit ones).  A8B8G8R8 (51, 52, 55,
 * 56, 57) is taken as R8G8B8A8 (37, 38, 41, 42, 43) is.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 8.5.1,
 * the table of formats, and its notes on the packed 32-bit formats).
 */
static const struct format_row format_rows[] = {
	{ 9U, 1U, GUARD_NONE, COLOR_FILTER_BLEND | HW_VERTEX_BUFFER, 0U },
	{ 10U, 1U, GUARD_NONE, SAMPLE_FILTER | HW_VERTEX_BUFFER, 0U },
	{ 13U, 1U, GUARD_NONE, SAMPLE_COLOR | HW_VERTEX_BUFFER, ROW_INTEGER },
	{ 14U, 1U, GUARD_NONE, SAMPLE_COLOR | HW_VERTEX_BUFFER, ROW_INTEGER },
	{ 16U, 2U, GUARD_NONE, COLOR_FILTER_BLEND | HW_VERTEX_BUFFER, 0U },
	{ 17U, 2U, GUARD_NONE, SAMPLE_FILTER | HW_VERTEX_BUFFER, 0U },
	{ 20U, 2U, GUARD_NONE, SAMPLE_COLOR | HW_VERTEX_BUFFER, ROW_INTEGER },
	{ 21U, 2U, GUARD_NONE, SAMPLE_COLOR | HW_VERTEX_BUFFER, ROW_INTEGER },
	{ 37U, 4U, GUARD_NONE, COLOR_FILTER_BLEND | HW_VERTEX_BUFFER | HW_STORAGE_IMAGE, 0U },
	{ 38U, 4U, GUARD_NONE, SAMPLE_FILTER | HW_VERTEX_BUFFER, 0U },
	{ 41U, 4U, GUARD_NONE, SAMPLE_COLOR | HW_VERTEX_BUFFER | HW_STORAGE_IMAGE, ROW_INTEGER },
	{ 42U, 4U, GUARD_NONE, SAMPLE_COLOR | HW_VERTEX_BUFFER | HW_STORAGE_IMAGE, ROW_INTEGER },
	{ 43U, 4U, GUARD_NONE, COLOR_FILTER_BLEND, ROW_SRGB },
	{ 44U, 4U, GUARD_NONE, COLOR_FILTER_BLEND | HW_VERTEX_BUFFER, 0U },
	{ 50U, 4U, GUARD_NONE, COLOR_FILTER_BLEND, ROW_SRGB },
	{ 51U, 4U, GUARD_NONE, COLOR_FILTER_BLEND | HW_VERTEX_BUFFER | HW_STORAGE_IMAGE, 0U },
	{ 52U, 4U, GUARD_NONE, SAMPLE_FILTER | HW_VERTEX_BUFFER, 0U },
	{ 55U, 4U, GUARD_NONE, SAMPLE_COLOR | HW_VERTEX_BUFFER | HW_STORAGE_IMAGE, ROW_INTEGER },
	{ 56U, 4U, GUARD_NONE, SAMPLE_COLOR | HW_VERTEX_BUFFER | HW_STORAGE_IMAGE, ROW_INTEGER },
	{ 57U, 4U, GUARD_NONE, COLOR_FILTER_BLEND, ROW_SRGB },
	{ 64U, 4U, GUARD_NONE, COLOR_FILTER_BLEND | HW_VERTEX_BUFFER, 0U },
	{ 70U, 2U, GUARD_NONE, COLOR_FILTER_BLEND | HW_VERTEX_BUFFER, 0U },
	{ 71U, 2U, GUARD_NONE, SAMPLE_FILTER | HW_VERTEX_BUFFER, 0U },
	{ 74U, 2U, GUARD_NONE, SAMPLE_COLOR | HW_VERTEX_BUFFER, ROW_INTEGER },
	{ 75U, 2U, GUARD_NONE, SAMPLE_COLOR | HW_VERTEX_BUFFER, ROW_INTEGER },
	{ 76U, 2U, GUARD_NONE, COLOR_FILTER_BLEND | HW_VERTEX_BUFFER | HW_STORAGE_IMAGE, ROW_FLOAT },
	{ 77U, 4U, GUARD_NONE, COLOR_FILTER_BLEND | HW_VERTEX_BUFFER, 0U },
	{ 78U, 4U, GUARD_NONE, SAMPLE_FILTER | HW_VERTEX_BUFFER, 0U },
	{ 81U, 4U, GUARD_NONE, SAMPLE_COLOR | HW_VERTEX_BUFFER, ROW_INTEGER },
	{ 82U, 4U, GUARD_NONE, SAMPLE_COLOR | HW_VERTEX_BUFFER, ROW_INTEGER },
	{ 83U, 4U, GUARD_NONE, COLOR_FILTER_BLEND | HW_VERTEX_BUFFER | HW_STORAGE_IMAGE, ROW_FLOAT },
	{ 84U, 2U, GUARD_NONE, COLOR_FILTER_BLEND | HW_VERTEX_BUFFER, 0U },
	{ 86U, 2U, GUARD_NONE, COLOR_FILTER_BLEND | HW_VERTEX_BUFFER, 0U },
	{ 91U, 8U, GUARD_NONE, COLOR_FILTER_BLEND | HW_VERTEX_BUFFER, 0U },
	{ 92U, 8U, GUARD_NONE, SAMPLE_FILTER | HW_VERTEX_BUFFER, 0U },
	{ 95U, 8U, GUARD_NONE, SAMPLE_COLOR | HW_VERTEX_BUFFER | HW_STORAGE_IMAGE, ROW_INTEGER },
	{ 96U, 8U, GUARD_NONE, SAMPLE_COLOR | HW_VERTEX_BUFFER | HW_STORAGE_IMAGE, ROW_INTEGER },
	{ 97U, 8U, GUARD_NONE, COLOR_FILTER_BLEND | HW_VERTEX_BUFFER | HW_STORAGE_IMAGE, ROW_FLOAT },
	{ 98U, 4U, GUARD_NONE, SAMPLE_COLOR | HW_VERTEX_BUFFER | HW_STORAGE_IMAGE, ROW_INTEGER },
	{ 99U, 4U, GUARD_NONE, SAMPLE_COLOR | HW_VERTEX_BUFFER | HW_STORAGE_IMAGE, ROW_INTEGER },
	{ 100U, 4U, GUARD_NONE, COLOR_FILTER_BLEND | HW_VERTEX_BUFFER | HW_STORAGE_IMAGE, ROW_FLOAT | ROW_FLOAT32 },
	{ 101U, 8U, GUARD_NONE, SAMPLE_COLOR | HW_VERTEX_BUFFER | HW_STORAGE_IMAGE, ROW_INTEGER },
	{ 102U, 8U, GUARD_NONE, SAMPLE_COLOR | HW_VERTEX_BUFFER | HW_STORAGE_IMAGE, ROW_INTEGER },
	{ 103U, 8U, GUARD_NONE, SAMPLE_FILTER | HW_COLOR_ATTACHMENT | HW_VERTEX_BUFFER, ROW_FLOAT | ROW_FLOAT32 },
	{ 104U, 12U, GUARD_NONE, HW_VERTEX_BUFFER, ROW_INTEGER },
	{ 105U, 12U, GUARD_NONE, HW_VERTEX_BUFFER, ROW_INTEGER },
	{ 106U, 12U, GUARD_NONE, HW_VERTEX_BUFFER, ROW_FLOAT | ROW_FLOAT32 },
	{ 107U, 16U, GUARD_NONE, SAMPLE_COLOR | HW_VERTEX_BUFFER | HW_STORAGE_IMAGE, ROW_INTEGER },
	{ 108U, 16U, GUARD_NONE, SAMPLE_COLOR | HW_VERTEX_BUFFER | HW_STORAGE_IMAGE, ROW_INTEGER },
	{ 109U, 16U, GUARD_NONE, COLOR_FILTER_BLEND | HW_VERTEX_BUFFER | HW_STORAGE_IMAGE, ROW_FLOAT | ROW_FLOAT32 },
	{ 122U, 4U, GUARD_NONE, SAMPLE_FILTER | HW_COLOR_ATTACHMENT | HW_VERTEX_BUFFER, 0U },
	{ 124U, 2U, GUARD_NONE, HW_SAMPLE | HW_DEPTH_STENCIL, ROW_DEPTH },
	{ 126U, 4U, GUARD_NONE, HW_SAMPLE | HW_DEPTH_STENCIL, ROW_DEPTH },
	{ 129U, 4U, GUARD_NONE, HW_SAMPLE | HW_DEPTH_STENCIL, ROW_DEPTH },
	{ 130U, 8U, GUARD_NONE, HW_SAMPLE | HW_DEPTH_STENCIL, ROW_DEPTH },
	{ 131U, 8U, 0U, SAMPLE_FILTER, ROW_BLOCK },
	{ 133U, 8U, 0U, SAMPLE_FILTER, ROW_BLOCK },
	{ 135U, 16U, 1U, SAMPLE_FILTER, ROW_BLOCK },
	{ 137U, 16U, 2U, SAMPLE_FILTER, ROW_BLOCK },
	{ 139U, 8U, 3U, SAMPLE_FILTER, ROW_BLOCK },
	{ 141U, 16U, 4U, SAMPLE_FILTER, ROW_BLOCK },
	{ 145U, 16U, 6U, SAMPLE_FILTER, ROW_BLOCK },
	{ 147U, 8U, GUARD_ETC2, SAMPLE_FILTER, ROW_BLOCK },
	{ 148U, 8U, GUARD_ETC2, SAMPLE_FILTER, ROW_BLOCK | ROW_SRGB },
	{ 149U, 8U, GUARD_ETC2, SAMPLE_FILTER, ROW_BLOCK },
	{ 150U, 8U, GUARD_ETC2, SAMPLE_FILTER, ROW_BLOCK | ROW_SRGB },
	{ 151U, 16U, GUARD_ETC2, SAMPLE_FILTER, ROW_BLOCK },
	{ 152U, 16U, GUARD_ETC2, SAMPLE_FILTER, ROW_BLOCK | ROW_SRGB },
	{ 153U, 8U, GUARD_ETC2, SAMPLE_FILTER, ROW_BLOCK },
	{ 154U, 8U, GUARD_ETC2, SAMPLE_FILTER, ROW_BLOCK },
	{ 155U, 16U, GUARD_ETC2, SAMPLE_FILTER, ROW_BLOCK },
	{ 156U, 16U, GUARD_ETC2, SAMPLE_FILTER, ROW_BLOCK }
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
static uint32_t table_bits(const struct ghci_caps *caps, const struct format_row *row);
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
 * Makes the features of every format from the specification's table
 * and the capability flags, for a device without the FMT block.
 */
void
ghci_vk_formats_table(
	const struct ghci_caps *caps,
	struct ghci_vk_formats *formats)
{
	uint32_t bits;
	size_t index;

	/* No format has a feature the table does not give it. */
	kern_memset(formats, 0, sizeof(*formats));

	/* Each row: the same features on both tilings, and the buffer's from the same row. */
	for (index = 0U; index < sizeof(format_rows) / sizeof(format_rows[0]); index++) {
		bits = table_bits(caps, &format_rows[index]);
		ghci_vk_formats_from_bits(caps, format_rows[index].format, bits, bits, bits, formats);
	}
}

/*
 * Makes one format's Vulkan features from FMT's bits for optimal and
 * linear tiling and for buffers, and drops what the driver cannot do: a
 * format outside the table, an sRGB format without EXT_CAP_FMT0's bit, a
 * storage image the table does not give, the linear filter of a texture
 * unit without the bilinear filter, and a blit the driver's shaders
 * replace where the executor does not use the blit engine.  The driver
 * lays every image out in linear rows, so an image made with optimal
 * tiling is a linear one: optimal tiling has only what both tilings have.
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
	uint32_t texel;
	unsigned index;
	int blitter;
	int bilinear;
	int srgb;

	/* A format the driver cannot write into a descriptor has no feature. */
	row = row_of(format);
	if (row == NULL || format >= GHCI_VK_FORMATS)
		return;

	/* An sRGB format needs the sRGB bit of EXT_CAP_FMT0. */
	srgb = ghci_caps_has(caps, GHCI_HAS_SRGB);
	if ((row->kind & ROW_SRGB) != 0U && !srgb)
		return;

	/* What the driver itself can do with the format (storage only where the table gives it); optimal images are linear. */
	driver = ~(uint32_t)HW_STORAGE_IMAGE;
	if ((row->features & HW_STORAGE_IMAGE) != 0U)
		driver = ~0U;
	blitter = ghci_vk_caps_blitter(caps);
	tilings[0] = optimal & linear & driver;
	tilings[1] = linear & driver;

	/*
	 * Without the bilinear filter the texture unit samples the nearest
	 * texel only, so no format filters, whatever FMT says.
	 *
	 * Values referenced: amaterrace-gpu ad23fe0 docs/ghci.md (section
	 * 3.2.2, EXT_CAP_FMT0 [16] CAP_TU_BILINEAR; section 4.1, FMT_OPTIMAL
	 * and FMT_LINEAR report no FILTER_LINEAR without it).
	 */
	bilinear = ghci_caps_has(caps, GHCI_HAS_TU_BILINEAR);
	for (index = 0U; index < 2U && !bilinear; index++)
		tilings[index] &= ~(uint32_t)HW_FILTER_LINEAR;

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

	/* Buffers: vertices, texel buffers the texture unit samples, and storage texel buffers. */
	texel = 0U;
	if ((buffer & HW_VERTEX_BUFFER) != 0U)
		texel |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
	if ((buffer & HW_SAMPLE) != 0U && (row->kind & (ROW_BLOCK | ROW_DEPTH)) == 0U)
		texel |= VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
	if ((buffer & driver & HW_STORAGE_IMAGE) != 0U)
		texel |= VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT;
	formats->buffer[format] = texel;
}

/*
 * Reads the features of every format of the table from the FMT block, or
 * makes them from the table on a device without it.  The caller holds
 * none of the device's locks; FMT_INDEX, the one register the host writes
 * to index the block, is written under the device's lock.
 */
void
ghci_vk_formats_read(
	struct ghci_device *device,
	struct ghci_vk_formats *formats)
{
	uint32_t block;
	uint32_t optimal;
	uint32_t linear;
	uint32_t buffer;
	size_t index;

	/* Without FMT, the table. */
	if ((device->caps.blocks & (1U << GHCI_EXTCAP_ID_FMT)) == 0U) {
		ghci_vk_formats_table(&device->caps, formats);
		return;
	}

	/* Each format of the table through FMT_INDEX. */
	kern_memset(formats, 0, sizeof(*formats));
	block = device->caps.block_offset[GHCI_EXTCAP_ID_FMT];
	mutex_lock(&device->lock);

	for (index = 0U; index < sizeof(format_rows) / sizeof(format_rows[0]); index++) {
		ghci_write(device, block + FMT_INDEX, format_rows[index].format);
		optimal = ghci_read(device, block + FMT_OPTIMAL);
		linear = ghci_read(device, block + FMT_LINEAR);
		buffer = ghci_read(device, block + FMT_BUFFER);
		ghci_vk_formats_from_bits(&device->caps, format_rows[index].format, optimal, linear, buffer, formats);
	}

	mutex_unlock(&device->lock);

	/* Succeeded: the features are the device's. */
	formats->from_registers = 1;
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
 * Reports the FMT_OPTIMAL bits the table gives a row on a device with
 * these capabilities, with the blits of a device that has the blit
 * engine: sampled formats (depth too) as sources and colour attachments
 * as destinations.
 */
static uint32_t
table_bits(
	const struct ghci_caps *caps,
	const struct format_row *row)
{
	uint32_t bits;
	int etc2;
	int filter;
	int filter32;
	int packer;

	/* A BC or ETC2 format needs its bit of EXT_CAP_FMT0. */
	etc2 = ghci_caps_has(caps, GHCI_HAS_ETC2);
	if (row->guard == GUARD_ETC2 && !etc2)
		return 0U;
	if (row->guard < GUARD_ETC2 && (caps->fmt0 & (1U << row->guard)) == 0U)
		return 0U;

	/*
	 * Without the float filter, no linear filter of a float format;
	 * without the filter of 32-bit floats as well, none of a 32-bit one.
	 *
	 * Values referenced: amaterrace-gpu ad23fe0 docs/ghci.md (section
	 * 3.2.1, DEVICE_CAPS [25] CAP_TEX_FFILT; section 3.2.2, EXT_CAP_FMT0
	 * [11] CAP_FFILT_FP32).
	 */
	bits = row->features;
	filter = ghci_caps_has(caps, GHCI_HAS_TEX_FFILT);
	filter32 = ghci_caps_has(caps, GHCI_HAS_FFILT_FP32);
	if ((row->kind & ROW_FLOAT) != 0U && !filter)
		bits &= ~HW_FILTER_LINEAR;
	if ((row->kind & ROW_FLOAT32) != 0U && !filter32)
		bits &= ~HW_FILTER_LINEAR;

	/* Without the packer, no integer and no 32-bit float colour attachment. */
	packer = ghci_caps_has(caps, GHCI_HAS_ROP_PACK);
	if ((row->kind & (ROW_INTEGER | ROW_FLOAT32)) != 0U && !packer)
		bits &= ~(HW_COLOR_ATTACHMENT | HW_BLEND);

	/* The blit engine's sources and destinations. */
	if ((bits & HW_SAMPLE) != 0U)
		bits |= HW_BLIT_SRC;
	if ((bits & HW_COLOR_ATTACHMENT) != 0U)
		bits |= HW_BLIT_DST;

	/* Succeeded: the bits. */
	return bits;
}

/*
 * Reports the Vulkan format features of FMT's bits.
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
