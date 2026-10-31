/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Single-level strict UIF textures isolate native sampling layout from caller-owned raster image backing. */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/bcm2711/native-state.h"

/* The current Vulkan image contract admits RGBA/BGRA 32-bpp extents up to 4096 pixels. */
#define NATIVE_TEXTURE_DIMENSION 4096U

/*
 * A mip tree's layout (Mesa's v3d_setup_slices and v3d_get_ub_pad,
 * src/gallium/drivers/v3d/v3d_resource.c, with v3d_screen.h's sizes): UIF
 * pages of 4096 bytes, eight banks of them in the page cache, and rows of
 * four 256-byte UIF blocks; a four-byte texel's utile is four by four and its
 * UIF block eight by eight.
 */
#define MIP_PAGE_BYTES 4096U
#define MIP_PAGE_CACHE_BYTES (MIP_PAGE_BYTES * 8U)
#define MIP_UIF_ROW_BYTES 1024U
#define MIP_PAGE_UB_ROWS (MIP_PAGE_BYTES / MIP_UIF_ROW_BYTES)
#define MIP_PAGE_UB_ROWS_1_5 ((MIP_PAGE_UB_ROWS * 3U) >> 1)
#define MIP_PAGE_CACHE_UB_ROWS (MIP_PAGE_CACHE_BYTES / MIP_UIF_ROW_BYTES)
#define MIP_PAGE_CACHE_MINUS_1_5_UB_ROWS (MIP_PAGE_CACHE_UB_ROWS - MIP_PAGE_UB_ROWS_1_5)
#define MIP_UTILE 4U
#define MIP_UIF_BLOCK 8U

static int utile_extent(uint32_t texel_bytes, uint32_t *utile_width, uint32_t *utile_height);
static void write_bits(uint8_t *bytes, uint32_t start, uint32_t count, uint32_t field);
static uint32_t mip_ub_pad(uint32_t height);
static uint32_t mip_align(uint32_t value, uint32_t alignment);

/*
 * Measures one strictly UIF, non-XOR scratch image without changing the caller's raster layout.
 */
int
bcm2711_native_texture_size(
	uint32_t width,
	uint32_t height,
	uint32_t *bytes)
{
	int error;

	/* A four-byte texel's utile is four by four, so a 256-byte block covers eight by eight. */
	error = bcm2711_native_texture_size_texel(width, height, 4U, bytes);
	if (error != 0)
		return error;

	/* Succeeded: explicit XOR disable permits zero UB padding for every single-level extent. */
	return 0;
}

/*
 * Measures one strictly UIF, non-XOR image of one-, two- or four-byte texels.
 *
 * A utile is 64 bytes: eight by eight one-byte texels, eight by four two-byte
 * texels or four by four four-byte texels.  A 256-byte UIF block holds two by
 * two utiles, and a column is four blocks wide, so every column is 1024 bytes
 * a block row whatever the texel size.
 */
int
bcm2711_native_texture_size_texel(
	uint32_t width,
	uint32_t height,
	uint32_t texel_bytes,
	uint32_t *bytes)
{
	uint32_t utile_width;
	uint32_t utile_height;
	uint32_t columns;
	uint32_t block_rows;
	int error;

	/* A missing output cannot receive the complete native storage requirement. */
	if (bytes == NULL)
		return EINVAL;

	/* Failed extent checks publish no storage size. */
	*bytes = 0;
	if (width == 0 || width > NATIVE_TEXTURE_DIMENSION)
		return EINVAL;

	/* The existing image limit also bounds all native offset arithmetic below 64 MiB. */
	if (height == 0 || height > NATIVE_TEXTURE_DIMENSION)
		return EINVAL;

	/* Finds the utile of the texel size; other sizes have no layout here. */
	error = utile_extent(texel_bytes, &utile_width, &utile_height);
	if (error != 0)
		return error;

	/* One column is four blocks wide, and a block row is two utiles tall. */
	columns = (width + utile_width * 8U - 1U) / (utile_width * 8U);
	block_rows = (height + utile_height * 2U - 1U) / (utile_height * 2U);
	*bytes = columns * block_rows * 1024U;

	/* Succeeded: the image's whole columns and block rows. */
	return 0;
}

/*
 * Finds the byte offset of one texel in a strictly UIF, non-XOR image.
 *
 * Blocks go down a four-block column before moving to the next column; the
 * four utiles of a block and the texels of a utile are in raster order.
 */
int
bcm2711_native_texture_offset(
	uint32_t x,
	uint32_t y,
	uint32_t height,
	uint32_t texel_bytes,
	uint32_t *offset)
{
	uint32_t utile_width;
	uint32_t utile_height;
	uint32_t block_rows;
	uint32_t column;
	uint32_t block;
	int error;

	/* A missing output cannot receive the offset. */
	if (offset == NULL)
		return EINVAL;

	/* Finds the utile of the texel size; other sizes have no layout here. */
	*offset = 0;
	error = utile_extent(texel_bytes, &utile_width, &utile_height);
	if (error != 0)
		return error;

	/* A texel outside the image's extent limit has no place in it. */
	if (x >= NATIVE_TEXTURE_DIMENSION ||
	    y >= height ||
	    height > NATIVE_TEXTURE_DIMENSION)
		return EINVAL;

	/* Resolves the column-major block. */
	block_rows = (height + utile_height * 2U - 1U) / (utile_height * 2U);
	column = x / (utile_width * 8U);
	block = column * block_rows * 4U;
	block += (y / (utile_height * 2U)) * 4U;
	block += (x / (utile_width * 2U)) % 4U;

	/* Selects the utile in the block, then the texel in the utile. */
	*offset = block * 256U;
	*offset += ((x / utile_width) % 2U) * 64U;
	*offset += ((y / utile_height) % 2U) * 128U;
	*offset += (y % utile_height) * utile_width * texel_bytes;
	*offset += (x % utile_width) * texel_bytes;

	/* Succeeded: the texel's byte offset from the image's start. */
	return 0;
}

/*
 * Gives a texel format's size in bytes and its native texture type.
 */
int
bcm2711_native_texel_layout(
	enum bcm2711_native_texel texel,
	uint32_t *texel_bytes,
	uint32_t *type)
{
	/* The native types are R8 0, RG8 2 and RGBA8 4. */
	switch (texel) {
	case BCM2711_NATIVE_TEXEL_RGBA8:
		*texel_bytes = 4U;
		*type = 4U;
		break;
	case BCM2711_NATIVE_TEXEL_R8:
		*texel_bytes = 1U;
		*type = 0U;
		break;
	case BCM2711_NATIVE_TEXEL_RG8:
		*texel_bytes = 2U;
		*type = 2U;
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: the texel's size and type. */
	return 0;
}

/*
 * Copies visible raster texels of one, two or four bytes into a complete zero-padded, strict UIF image.
 *
 * The same contract as bcm2711_native_texture_copy, for any admitted texel
 * size: one utile row at a time, at its place from texture_offset.  It is the
 * CPU's conversion when the texture formatting unit cannot convert.
 */
int
bcm2711_native_texture_copy_texel(
	uint32_t width,
	uint32_t height,
	uint32_t texel_bytes,
	const void *raster,
	uint32_t pitch,
	size_t raster_bytes,
	void *tiled,
	size_t tiled_bytes)
{
	const uint8_t *source;
	uint8_t *destination;
	uint32_t storage_bytes;
	uint32_t utile_width;
	uint32_t utile_height;
	uint32_t x;
	uint32_t y;
	uint32_t run;
	uint32_t offset;
	uint64_t source_bytes;
	uintptr_t source_first;
	uintptr_t destination_first;
	int error;

	/* A conversion never invents storage for either image representation. */
	if (raster == NULL || tiled == NULL)
		return EINVAL;

	/* Extent checks establish finite, overflow-free native row/column arithmetic. */
	error = bcm2711_native_texture_size_texel(width, height, texel_bytes, &storage_bytes);
	if (error != 0)
		return error;
	error = utile_extent(texel_bytes, &utile_width, &utile_height);
	if (error != 0)
		return error;

	/* Each raster row includes every visible texel, and the source holds the whole image. */
	if (pitch < width * texel_bytes)
		return EINVAL;
	source_bytes = (uint64_t)(height - 1U) * pitch + (uint64_t)width * texel_bytes;
	if (source_bytes > raster_bytes)
		return EINVAL;

	/* The destination holds whole native blocks, including all zero padding. */
	if (tiled_bytes < storage_bytes)
		return ENOSPC;

	/* Integer address checks precede every CPU pointer offset or overlap decision. */
	source_first = (uintptr_t)raster;
	destination_first = (uintptr_t)tiled;
	if (source_bytes > UINTPTR_MAX - source_first)
		return EINVAL;
	if (storage_bytes > UINTPTR_MAX - destination_first)
		return EINVAL;

	/* The two representations must not alias. */
	if (source_first < destination_first + storage_bytes && destination_first < source_first + source_bytes)
		return EINVAL;

	/* Deterministic padding prevents filtered edge accesses from consuming stale scratch contents. */
	source = raster;
	destination = tiled;
	kern_memset(destination, 0, storage_bytes);

	/* Copies every utile row of every visible row. */
	for (y = 0; y < height; y++) {
		/* One utile row is contiguous in the UIF image. */
		for (x = 0; x < width; x += utile_width) {
			error = bcm2711_native_texture_offset(x, y, height, texel_bytes, &offset);
			if (error != 0)
				return error;

			/* The visible texels of the utile row. */
			run = width - x;
			if (run > utile_width)
				run = utile_width;
			kern_memcpy(destination + offset, source + (size_t)y * pitch + (size_t)x * texel_bytes, (size_t)run * texel_bytes);
		}
	}

	/* Succeeded: every visible texel occupies its exact non-XOR UIF address. */
	return 0;
}

/*
 * Encodes one complete native texture record for independently owned strict UIF scratch storage.
 *
 * The owner supplies the exact size returned by texture_size and keeps the
 * copied image alive through confirmed native retirement.  This operation
 * neither copies pixels nor invents an alias of a raster image allocation.
 */
int
bcm2711_native_texture_encode(
	const struct bcm2711_native_texture *texture,
	uint8_t *bytes,
	size_t capacity)
{
	uint32_t storage_bytes;
	uint32_t texel_bytes;
	uint32_t type;
	uint32_t red;
	uint32_t green;
	uint32_t blue;
	uint32_t alpha;
	uint64_t end;
	int error;

	/* Missing caller storage cannot hold any valid native record. */
	if (texture == NULL || bytes == NULL)
		return EINVAL;

	/* A short reservation remains wholly unchanged. */
	if (capacity < BCM2711_NATIVE_TEXTURE_BYTES)
		return ENOSPC;

	/* Each base pointer leaves its six texture-control low bits clear. */
	if (texture->address == 0 || (texture->address & 63U) != 0)
		return EINVAL;

	/* The texel format's size and native type; an unknown one has no record. */
	error = bcm2711_native_texel_layout(texture->texel, &texel_bytes, &type);
	if (error != 0)
		return error;

	/* Native storage covers the complete padded UIF image, not merely the visible pixel rectangle. */
	error = bcm2711_native_texture_size_texel(texture->width, texture->height, texel_bytes, &storage_bytes);
	if (error != 0)
		return error;

	/* The descriptor's layer stride describes exactly the conversion's chosen layout; a mip tree's is the whole tree. */
	if (texture->levels > 1U) {
		if (texture->levels > BCM2711_NATIVE_TEXTURE_LEVELS || texture->texel != BCM2711_NATIVE_TEXEL_RGBA8)
			return EINVAL;
		if (texture->level0_tiling < BCM2711_NATIVE_TILING_LINEARTILE || texture->level0_tiling > BCM2711_NATIVE_TILING_UIF_XOR)
			return EINVAL;
		if (texture->level0_ub_pad > 15U || texture->bytes < storage_bytes || (texture->bytes & 63U) != 0)
			return EINVAL;
		storage_bytes = texture->bytes;
	} else if (texture->bytes != storage_bytes) {
		return EINVAL;
	}

	/* Whole scratch storage stays inside the native 32-bit GPU virtual address space. */
	end = (uint64_t)texture->address + storage_bytes;
	if (end > ((uint64_t)1 << 32))
		return EINVAL;

	/* Component swapping is one exact BGRA choice, without arbitrary swizzle state. */
	if (texture->swap_red_blue > 1U)
		return EINVAL;
	if (texture->swap_red_blue != 0 && texture->texel != BCM2711_NATIVE_TEXEL_RGBA8)
		return EINVAL;

	/* RGBA scratch retains its bytes; BGRA changes only the shader descriptor's red/blue selection. */
	red = 2U;
	green = 3U;
	blue = 4U;
	alpha = 5U;
	if (texture->swap_red_blue != 0) {
		red = 4U;
		blue = 2U;
	}

	/* An R8 or RG8 texel reads zero for its absent colours and one for alpha, as Vulkan defines them. */
	if (texture->texel == BCM2711_NATIVE_TEXEL_R8) {
		green = 0U;
		blue = 0U;
		alpha = 1U;
	} else if (texture->texel == BCM2711_NATIVE_TEXEL_RG8) {
		blue = 0U;
		alpha = 1U;
	}

	/* Defines an unflipped, single-layer, single-level normalized texture of the texel's type with its exact padded layer stride. */
	kern_memset(bytes, 0, BCM2711_NATIVE_TEXTURE_BYTES);
	write_bits(bytes, 0, 32, texture->address);
	write_bits(bytes, 32, 26, storage_bytes / 64U);
	write_bits(bytes, 58, 14, texture->width);
	write_bits(bytes, 72, 14, texture->height);
	write_bits(bytes, 86, 14, 1U);
	write_bits(bytes, 100, 7, type);

	/* Extended state explicitly selects strict UIF with XOR disabled, independent of small-image automatic tiling. */
	write_bits(bytes, 107, 1, 1U);
	write_bits(bytes, 108, 3, red);
	write_bits(bytes, 111, 3, green);
	write_bits(bytes, 114, 3, blue);
	write_bits(bytes, 117, 3, alpha);
	if (texture->levels <= 1U) {
		write_bits(bytes, 134, 1, 1U);
		write_bits(bytes, 135, 1, 1U);
	} else {
		/*
		 * A mip tree: levels 0 to the last, level 0's own tiling (strictly
		 * UIF when it is UIF, with its XOR and UB pad), and XOR left on so the
		 * hardware infers the smaller levels' tilings as the layout chose them
		 * (Mesa's v3dvx_image.c pack_texture_shader_state).
		 */
		write_bits(bytes, 120, 4, texture->levels - 1U);
		if (texture->level0_tiling == BCM2711_NATIVE_TILING_UIF || texture->level0_tiling == BCM2711_NATIVE_TILING_UIF_XOR) {
			write_bits(bytes, 134, 1, 1U);
			write_bits(bytes, 128, 4, texture->level0_ub_pad);
		}
		if (texture->level0_tiling == BCM2711_NATIVE_TILING_UIF_XOR)
			write_bits(bytes, 132, 1, 1U);
	}

	/* Succeeded: this complete record describes the same layout as the independently bounded pixel converter. */
	return 0;
}

/*
 * Encodes one normalized single-level nearest/linear sampler with admitted hardware wrap modes.
 */
int
bcm2711_native_sampler_encode(
	const struct bcm2711_native_sampler *sampler,
	uint8_t *bytes,
	size_t capacity)
{
	/* Missing inputs cannot be replaced with undocumented default sampler state. */
	if (sampler == NULL || bytes == NULL)
		return EINVAL;

	/* Whole output remains untouched on a short reservation. */
	if (capacity < BCM2711_NATIVE_SAMPLER_BYTES)
		return ENOSPC;

	/* Nearest/linear filters are exact bits rather than arbitrary nonzero values. */
	if (sampler->nearest_mag > 1U || sampler->nearest_min > 1U)
		return EINVAL;

	/* Only repeat, clamp-to-edge and mirrored-repeat have corresponding admitted Vulkan sampler modes. */
	if (sampler->wrap_u > 2U || sampler->wrap_v > 2U)
		return ENOTSUP;

	/* The largest level of detail is 4.8 fixed point below sixteen; a linear mip filter is an exact bit. */
	if (sampler->max_lod > 0xfffU || sampler->mip_linear > 1U)
		return EINVAL;

	/* Zero min LOD and bias up to max_lod (zero: level 0 alone), without anisotropy or depth comparison. */
	kern_memset(bytes, 0, BCM2711_NATIVE_SAMPLER_BYTES);
	write_bits(bytes, 0, 1, sampler->nearest_mag);
	write_bits(bytes, 1, 1, sampler->nearest_min);
	write_bits(bytes, 2, 1, sampler->mip_linear ^ 1U);
	write_bits(bytes, 20, 12, sampler->max_lod);
	write_bits(bytes, 7, 1, 1U);
	write_bits(bytes, 48, 3, sampler->wrap_u);
	write_bits(bytes, 51, 3, sampler->wrap_v);
	write_bits(bytes, 54, 3, 1U);

	/* Succeeded: normalized UNORM sampling consumes no border, comparison, mip or implicit sRGB policy. */
	return 0;
}

/*
 * Copies visible raster pixels into a complete zero-padded, strict UIF image without touching either reservation's tail.
 *
 * Call only at serialized native execution after all earlier writes to the
 * source image have retired and become CPU visible.  The copy is never a
 * descriptor snapshot taken before earlier queue work has executed.
 */
int
bcm2711_native_texture_copy(
	uint32_t width,
	uint32_t height,
	const void *raster,
	uint32_t pitch,
	size_t raster_bytes,
	void *tiled,
	size_t tiled_bytes)
{
	const uint8_t *source;
	uint8_t *destination;
	uint32_t storage_bytes;
	uint32_t block_rows;
	uint32_t x;
	uint32_t y;
	uint32_t run;
	uint32_t column;
	uint32_t block;
	uint32_t offset;
	uint64_t source_bytes;
	uintptr_t source_first;
	uintptr_t destination_first;
	int error;

	/* A conversion never invents storage for either image representation. */
	if (raster == NULL || tiled == NULL)
		return EINVAL;

	/* Extent checks establish finite, overflow-free native row/column arithmetic. */
	error = bcm2711_native_texture_size(width, height, &storage_bytes);
	if (error != 0)
		return error;

	/* Each raster row includes every visible four-byte pixel. */
	if (pitch < width * 4U)
		return EINVAL;

	/* Only visible bytes of the last row are read; caller padding is preserved. */
	source_bytes = (uint64_t)(height - 1U) * pitch + width * 4U;
	if (source_bytes > raster_bytes)
		return EINVAL;

	/* The destination holds whole native blocks, including all zero padding. */
	if (tiled_bytes < storage_bytes)
		return ENOSPC;

	/* Integer address checks precede every CPU pointer offset or overlap decision. */
	source_first = (uintptr_t)raster;
	destination_first = (uintptr_t)tiled;
	if (source_bytes > UINTPTR_MAX - source_first)
		return EINVAL;

	/* The destination's complete write interval must also be representable. */
	if (storage_bytes > UINTPTR_MAX - destination_first)
		return EINVAL;

	/* Zero padding cannot overwrite any still-needed source pixels through an alias. */
	if (source_first < destination_first + storage_bytes && destination_first < source_first + source_bytes)
		return EINVAL;

	/* Deterministic padding prevents filtered edge accesses from consuming stale scratch contents. */
	source = raster;
	destination = tiled;
	block_rows = (height + 7U) / 8U;
	kern_memset(destination, 0, storage_bytes);

	/* Four-block-wide columns advance through all block rows before moving to the next column. */
	for (y = 0; y < height; y++) {
		/* Each eight-by-eight UIF block contains four raster-ordered four-by-four utiles. */
		for (x = 0; x < width; x += 4U) {
			/* Resolves the column-major block before selecting its utile and exact pixel word. */
			column = x / 32U;
			block = column * block_rows * 4U + (y / 8U) * 4U + (x / 8U) % 4U;
			offset = block * 256U;
			offset += ((x / 4U) % 2U) * 64U;
			offset += ((y / 4U) % 2U) * 128U;
			offset += (y % 4U) * 16U + (x % 4U) * 4U;

			/* One utile row is contiguous; word copies avoid four uncached byte reads per pixel. */
			run = width - x;
			if (run > 4U)
				run = 4U;

			/* Compile a complete sixteen-byte utile row inline instead of calling the variable-size copy per four pixels. */
			if (run == 4U) {
				__builtin_memcpy(
					destination + offset,
					source + (size_t)y * pitch + (size_t)x * 4U,
					16U);
			} else {
				/* Copy only visible edge bytes, retaining support for unaligned and odd-sized images. */
				kern_memcpy(
					destination + offset,
					source + (size_t)y * pitch + (size_t)x * 4U,
					run * 4U);
			}
		}
	}

	/* Succeeded: every visible word now occupies its exact non-XOR UIF address, with no source mutation. */
	return 0;
}

/*
 * Copies the pixels of one box of a raster image into an earlier complete UIF conversion.
 *
 * box is x0, y0, x1, y1 with exclusive ends inside the image.  It is widened to
 * whole four-by-four utiles, so every 64-byte cache line the CPU writes is
 * written completely and no stale line of a device-written conversion goes back
 * to memory; utile parts outside the image become zero padding as in a whole
 * copy.  Pixels outside the widened box keep their earlier conversion.
 */
int
bcm2711_native_texture_copy_box(
	uint32_t width,
	uint32_t height,
	const void *raster,
	uint32_t pitch,
	size_t raster_bytes,
	void *tiled,
	size_t tiled_bytes,
	const uint32_t *box)
{
	const uint8_t *source;
	uint8_t *destination;
	uint32_t storage_bytes;
	uint32_t block_rows;
	uint32_t first_x;
	uint32_t first_y;
	uint32_t end_x;
	uint32_t end_y;
	uint32_t x;
	uint32_t y;
	uint32_t run;
	uint32_t block;
	uint32_t offset;
	uint64_t source_bytes;
	uintptr_t source_first;
	uintptr_t destination_first;
	int error;

	/* A partial conversion never invents storage or a box. */
	if (raster == NULL || tiled == NULL || box == NULL)
		return EINVAL;

	/* Extent checks establish finite, overflow-free native row/column arithmetic. */
	error = bcm2711_native_texture_size(width, height, &storage_bytes);
	if (error != 0)
		return error;

	/* The box is non-empty and inside the image. */
	if (box[0] >= box[2] || box[1] >= box[3])
		return EINVAL;
	if (box[2] > width || box[3] > height)
		return EINVAL;

	/* Each raster row includes every visible four-byte pixel, and the source holds the whole image. */
	if (pitch < width * 4U)
		return EINVAL;
	source_bytes = (uint64_t)(height - 1U) * pitch + width * 4U;
	if (source_bytes > raster_bytes)
		return EINVAL;

	/* The destination is a whole conversion of this image. */
	if (tiled_bytes < storage_bytes)
		return ENOSPC;

	/* Integer address checks precede every CPU pointer offset or overlap decision. */
	source_first = (uintptr_t)raster;
	destination_first = (uintptr_t)tiled;
	if (source_bytes > UINTPTR_MAX - source_first)
		return EINVAL;
	if (storage_bytes > UINTPTR_MAX - destination_first)
		return EINVAL;

	/* The two representations must not alias. */
	if (source_first < destination_first + storage_bytes && destination_first < source_first + source_bytes)
		return EINVAL;

	/* Widens the box to whole utiles, which the padded storage always holds. */
	source = raster;
	destination = tiled;
	block_rows = (height + 7U) / 8U;
	first_x = box[0] & ~3U;
	first_y = box[1] & ~3U;
	end_x = (box[2] + 3U) & ~3U;
	end_y = (box[3] + 3U) & ~3U;

	/* Writes every utile row of the widened box, with zero padding past the image's edges. */
	for (y = first_y; y < end_y; y++) {
		/* One utile row is sixteen contiguous bytes at its column-major block's address. */
		for (x = first_x; x < end_x; x += 4U) {
			block = (x / 32U) * block_rows * 4U + (y / 8U) * 4U + (x / 8U) % 4U;
			offset = block * 256U;
			offset += ((x / 4U) % 2U) * 64U;
			offset += ((y / 4U) % 2U) * 128U;
			offset += (y % 4U) * 16U;

			/* A row below the image is all padding; a row inside has up to four visible pixels. */
			run = 0;
			if (y < height) {
				run = width - x;
				if (run > 4U)
					run = 4U;
			}

			/* A whole visible row is one inline copy; a shorter one is padded with zeros. */
			if (run == 4U) {
				__builtin_memcpy(
					destination + offset,
					source + (size_t)y * pitch + (size_t)x * 4U,
					16U);
			} else {
				kern_memset(destination + offset, 0, 16U);
				if (run != 0) {
					kern_memcpy(
						destination + offset,
						source + (size_t)y * pitch + (size_t)x * 4U,
						run * 4U);
				}
			}
		}
	}

	/* Succeeded: the widened box now holds the raster's current pixels in its exact UIF places. */
	return 0;
}

/* Finds the width and height in texels of the 64-byte utile of one texel size. */
/*
 * Lays out a mip tree of four-byte texels as the texture unit reads it
 * (Mesa's v3d_setup_slices for a tiled texture whose level 0 need not be
 * UIF): from the smallest level up, each level tiled by its size (linear
 * tiles when a side fits a utile, rows of one or two UIF block columns when
 * the width fits them, else UIF with the UB pad that keeps page banks apart,
 * XOR when its block rows fill whole page caches), level 1 page-aligned when
 * a lower level could be UIF XOR, and the whole tree moved so that level 0
 * starts on a page.  Only power-of-two sides are laid out, where the
 * hardware's padding of the smaller levels to powers of two adds nothing.
 */
int
bcm2711_native_miptree_layout(
	uint32_t width,
	uint32_t height,
	uint32_t levels,
	struct bcm2711_native_miptree *tree)
{
	struct bcm2711_native_mip_level *level;
	uint32_t offset;
	uint32_t total;
	uint32_t pad;
	uint32_t padded_width;
	uint32_t index;

	/* A tree of two levels or more, within the levels a side has, of power-of-two sides. */
	if (tree == NULL)
		return EINVAL;
	kern_memset(tree, 0, sizeof(*tree));
	if (levels < 2U || levels > BCM2711_NATIVE_TEXTURE_LEVELS)
		return EINVAL;
	if (width == 0 || height == 0 || width > NATIVE_TEXTURE_DIMENSION || height > NATIVE_TEXTURE_DIMENSION)
		return EINVAL;
	if ((width & (width - 1U)) != 0 || (height & (height - 1U)) != 0)
		return ENOTSUP;
	if ((width >> (levels - 1U)) == 0 && (height >> (levels - 1U)) == 0)
		return EINVAL;

	/* From the smallest level to level 0. */
	offset = 0;
	index = levels;
	while (index > 0) {
		index--;
		level = &tree->level[index];
		level->width = width >> index;
		if (level->width == 0)
			level->width = 1;
		level->height = height >> index;
		if (level->height == 0)
			level->height = 1;

		/* The level's tiling and the padded extent it takes. */
		padded_width = level->width;
		level->padded_height = level->height;
		if (level->width <= MIP_UTILE || level->height <= MIP_UTILE) {
			level->tiling = BCM2711_NATIVE_TILING_LINEARTILE;
			padded_width = mip_align(padded_width, MIP_UTILE);
			level->padded_height = mip_align(level->padded_height, MIP_UTILE);
		} else if (level->width <= MIP_UIF_BLOCK) {
			level->tiling = BCM2711_NATIVE_TILING_UBLINEAR_1;
			padded_width = mip_align(padded_width, MIP_UIF_BLOCK);
			level->padded_height = mip_align(level->padded_height, MIP_UIF_BLOCK);
		} else if (level->width <= 2U * MIP_UIF_BLOCK) {
			level->tiling = BCM2711_NATIVE_TILING_UBLINEAR_2;
			padded_width = mip_align(padded_width, 2U * MIP_UIF_BLOCK);
			level->padded_height = mip_align(level->padded_height, MIP_UIF_BLOCK);
		} else {
			/* Four block columns wide, block rows high, and the UB pad. */
			padded_width = mip_align(padded_width, 4U * MIP_UIF_BLOCK);
			level->padded_height = mip_align(level->padded_height, MIP_UIF_BLOCK);
			level->ub_pad = mip_ub_pad(level->padded_height);
			level->padded_height += level->ub_pad * MIP_UIF_BLOCK;

			/* Rows of blocks that fill whole page caches use XOR to stay misaligned. */
			level->tiling = BCM2711_NATIVE_TILING_UIF;
			if ((level->padded_height / MIP_UIF_BLOCK) % MIP_PAGE_CACHE_UB_ROWS == 0)
				level->tiling = BCM2711_NATIVE_TILING_UIF_XOR;
		}

		/* The level's bytes, and level 1 page-aligned when a level below it could be UIF XOR. */
		level->bytes = padded_width * 4U * level->padded_height;
		level->offset = offset;
		total = level->bytes;
		if (index == 1U &&
		    padded_width > 4U * MIP_UIF_BLOCK &&
		    level->padded_height > MIP_PAGE_CACHE_MINUS_1_5_UB_ROWS * MIP_UIF_BLOCK)
			total = mip_align(total, MIP_PAGE_BYTES);
		offset += total;
	}

	/* Level 0 starts on a page; every level moves with it. */
	pad = mip_align(tree->level[0].offset, MIP_PAGE_BYTES) - tree->level[0].offset;
	for (index = 0; index < levels; index++)
		tree->level[index].offset += pad;
	tree->levels = levels;
	tree->bytes = mip_align(offset + pad, 64U);

	/* Succeeded: the tree's levels and size. */
	return 0;
}

/* The UIF block rows a level's height is padded by, so that page banks stay apart between block columns. */
static uint32_t
mip_ub_pad(
	uint32_t height)
{
	uint32_t rows;
	uint32_t in_cache;

	/* A height that fills whole page caches takes no pad (XOR misaligns it). */
	rows = height / MIP_UIF_BLOCK;
	in_cache = rows % MIP_PAGE_CACHE_UB_ROWS;
	if (in_cache == 0)
		return 0;

	/* Padded to at least one and a half pages off, unless the level fits in the page cache. */
	if (in_cache < MIP_PAGE_UB_ROWS_1_5) {
		if (rows < MIP_PAGE_CACHE_UB_ROWS)
			return 0;
		return MIP_PAGE_UB_ROWS_1_5 - in_cache;
	}

	/* Close to a whole page cache: rounded up to it, for XOR. */
	if (in_cache > MIP_PAGE_CACHE_MINUS_1_5_UB_ROWS)
		return MIP_PAGE_CACHE_UB_ROWS - in_cache;

	/* Far enough from both: no pad. */
	return 0;
}

/* Rounds value up to a power-of-two alignment. */
static uint32_t
mip_align(
	uint32_t value,
	uint32_t alignment)
{
	/* Succeeded: the next multiple. */
	return (value + alignment - 1U) & ~(alignment - 1U);
}

static int
utile_extent(
	uint32_t texel_bytes,
	uint32_t *utile_width,
	uint32_t *utile_height)
{
	/* Each admitted texel size fills 64 bytes with its own rectangle. */
	switch (texel_bytes) {
	case 1U:
		*utile_width = 8U;
		*utile_height = 8U;
		break;
	case 2U:
		*utile_width = 8U;
		*utile_height = 4U;
		break;
	case 4U:
		*utile_width = 4U;
		*utile_height = 4U;
		break;
	default:
		return EINVAL;
	}

	/* Succeeded: the utile's texel rectangle. */
	return 0;
}

/* Writes one already checked native field into a zeroed little-endian bit image. */
static void
write_bits(
	uint8_t *bytes,
	uint32_t start,
	uint32_t count,
	uint32_t field)
{
	uint32_t bit;
	uint32_t position;
	uint32_t selected;

	/* Explicit byte addressing keeps unaligned descriptor output independent of host word layout. */
	for (bit = 0; bit < count; bit++) {
		/* Every field contributes only its own bit positions to the initially zero image. */
		position = start + bit;
		selected = (field >> bit) & 1U;
		bytes[position / 8U] |= (uint8_t)(selected << (position % 8U));
	}

	/* Succeeded: the selected field has been encoded without changing neighbouring bits. */
	return;
}
