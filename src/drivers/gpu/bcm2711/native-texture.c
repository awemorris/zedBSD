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

static void write_bits(uint8_t *bytes, uint32_t start, uint32_t count, uint32_t field);

/*
 * Measures one strictly UIF, non-XOR scratch image without changing the caller's raster layout.
 */
int
bcm2711_native_texture_size(
	uint32_t width,
	uint32_t height,
	uint32_t *bytes)
{
	uint32_t columns;
	uint32_t block_rows;

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

	/* One 32-pixel column holds four adjacent eight-by-eight, 256-byte UIF blocks per row. */
	columns = (width + 31U) / 32U;
	block_rows = (height + 7U) / 8U;
	*bytes = columns * block_rows * 1024U;

	/* Succeeded: explicit XOR disable permits zero UB padding for every single-level extent. */
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
	uint32_t red;
	uint32_t blue;
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

	/* Native storage covers the complete padded UIF image, not merely the visible pixel rectangle. */
	error = bcm2711_native_texture_size(texture->width, texture->height, &storage_bytes);
	if (error != 0)
		return error;

	/* The descriptor's layer stride describes exactly the conversion's chosen layout. */
	if (texture->bytes != storage_bytes)
		return EINVAL;

	/* Whole scratch storage stays inside the native 32-bit GPU virtual address space. */
	end = (uint64_t)texture->address + storage_bytes;
	if (end > ((uint64_t)1 << 32))
		return EINVAL;

	/* Component swapping is one exact BGRA choice, without arbitrary swizzle state. */
	if (texture->swap_red_blue > 1U)
		return EINVAL;

	/* RGBA scratch retains its bytes; BGRA changes only the shader descriptor's red/blue selection. */
	red = 2U;
	blue = 4U;
	if (texture->swap_red_blue != 0) {
		red = 4U;
		blue = 2U;
	}

	/* Defines an unflipped, single-layer, single-level normalized RGBA8 texture with its exact padded layer stride. */
	kern_memset(bytes, 0, BCM2711_NATIVE_TEXTURE_BYTES);
	write_bits(bytes, 0, 32, texture->address);
	write_bits(bytes, 32, 26, storage_bytes / 64U);
	write_bits(bytes, 58, 14, texture->width);
	write_bits(bytes, 72, 14, texture->height);
	write_bits(bytes, 86, 14, 1U);
	write_bits(bytes, 100, 7, 4U);

	/* Extended state explicitly selects strict UIF with XOR disabled, independent of small-image automatic tiling. */
	write_bits(bytes, 107, 1, 1U);
	write_bits(bytes, 108, 3, red);
	write_bits(bytes, 111, 3, 3U);
	write_bits(bytes, 114, 3, blue);
	write_bits(bytes, 117, 3, 5U);
	write_bits(bytes, 134, 1, 1U);
	write_bits(bytes, 135, 1, 1U);

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

	/* One mip level uses zero min/max LOD and bias, without anisotropy or depth comparison. */
	kern_memset(bytes, 0, BCM2711_NATIVE_SAMPLER_BYTES);
	write_bits(bytes, 0, 1, sampler->nearest_mag);
	write_bits(bytes, 1, 1, sampler->nearest_min);
	write_bits(bytes, 2, 1, 1U);
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
		for (x = 0; x < width; x++) {
			/* Resolves the column-major block before selecting its utile and exact pixel word. */
			column = x / 32U;
			block = column * block_rows * 4U + (y / 8U) * 4U + (x / 8U) % 4U;
			offset = block * 256U;
			offset += ((x / 4U) % 2U) * 64U;
			offset += ((y / 4U) % 2U) * 128U;
			offset += (y % 4U) * 16U + (x % 4U) * 4U;
			kern_memcpy(destination + offset, source + (size_t)y * pitch + (size_t)x * 4U, 4U);
		}
	}

	/* Succeeded: every visible word now occupies its exact non-XOR UIF address, with no source mutation. */
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
