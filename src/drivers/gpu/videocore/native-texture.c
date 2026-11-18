/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Single-level strict UIF textures isolate native sampling layout from caller-owned raster image backing. */
#include <kern/kcrt.h>
#include <uapi/errno.h>

#include "drivers/gpu/videocore/native-state.h"

/* The current Vulkan image contract admits RGBA/BGRA 32-bpp extents up to 4096 pixels. */
#define NATIVE_TEXTURE_DIMENSION 4096U

static int utile_extent(uint32_t texel_bytes, uint32_t *utile_width, uint32_t *utile_height);
static void write_bits(uint8_t *bytes, uint32_t start, uint32_t count, uint32_t field);
static int view_swizzle(const uint32_t *swizzle, uint32_t *red, uint32_t *green, uint32_t *blue, uint32_t *alpha);

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
 * Measures one strictly UIF, non-XOR image of one-, two-, four-, eight- or sixteen-byte texels.
 *
 * A utile is 64 bytes: eight by eight one-byte texels, eight by four two-byte
 * texels, four by four four-byte texels, four by two eight-byte texels or two
 * by two sixteen-byte texels (Mesa 26.2.6 src/broadcom/common/v3d_tiling.c,
 * v3d_utile_width and v3d_utile_height).  A 256-byte UIF block holds two by
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
 * Fills the texture formatting unit's words for a raster-to-tiled
 * conversion on V3D 4.2: the output format in the output address register's
 * low bits (shifted there by the job), the texture type, and for a UIF output
 * the UIF blocks it has beyond those its rows need (OPAD).
 * Values referenced: Mesa 25.3.6 src/broadcom/common/v3d_tfu.h (V3D33_TFU_*)
 * and src/gallium/drivers/v3d/v3dx_tfu.c (V3D_VERSION == 42).
 */
int
bcm2711_native_conversion_words(
	const struct bcm2711_native_conversion *conversion,
	uint32_t *output_format,
	uint32_t *configuration)
{
	uint32_t utile_width;
	uint32_t utile_height;
	uint32_t block_height;
	uint32_t pad;
	int error;

	/* Missing words cannot be filled. */
	if (conversion == NULL || output_format == NULL || configuration == NULL)
		return EINVAL;

	/* An output tiling the unit writes, and a texture type of seven bits. */
	if (conversion->tiling < BCM2711_NATIVE_TILING_LINEARTILE || conversion->tiling > BCM2711_NATIVE_TILING_UIF_XOR)
		return EINVAL;
	if (conversion->type > 0x7fU)
		return EINVAL;

	/* A UIF output's rows: whole UIF blocks (two utiles tall), no fewer than the rows converted. */
	error = utile_extent(conversion->texel_bytes, &utile_width, &utile_height);
	if (error != 0)
		return error;
	block_height = utile_height * 2U;
	if ((conversion->tiling == BCM2711_NATIVE_TILING_UIF || conversion->tiling == BCM2711_NATIVE_TILING_UIF_XOR) &&
	    (conversion->padded_height % block_height != 0 || conversion->padded_height < conversion->height))
		return EINVAL;

	/* The format and the type; raster input is format 0. */
	*output_format = conversion->tiling;
	*configuration = conversion->type << 9;

	/* A UIF output's padding beyond its own rows. */
	if (conversion->tiling == BCM2711_NATIVE_TILING_UIF || conversion->tiling == BCM2711_NATIVE_TILING_UIF_XOR) {
		pad = (conversion->padded_height - (conversion->height + block_height - 1U) / block_height * block_height) / block_height;
		*configuration |= pad << 22;
	}

	/* Succeeded: both words. */
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
 * Each texel format's size in bytes, native texture type and channels (the
 * texel's red, green, blue and alpha in that order; absent ones read zero,
 * alpha one), and whether it is decoded from sRGB.
 * Values referenced: Mesa 26.2.6 src/broadcom/cle/v3d_packet.xml (Texture
 * Data Formats) and src/broadcom/vulkan/v3dvx_formats.c (the texture type
 * and swizzle of each Vulkan format).
 */
static const struct texel_format {
	uint8_t bytes;
	uint8_t type;
	uint8_t channels;
	uint8_t srgb;
} texel_formats[BCM2711_NATIVE_TEXELS] = {
	[BCM2711_NATIVE_TEXEL_RGBA8] = { 4, 4, 4, 0 },
	[BCM2711_NATIVE_TEXEL_R8] = { 1, 0, 1, 0 },
	[BCM2711_NATIVE_TEXEL_RG8] = { 2, 2, 2, 0 },
	[BCM2711_NATIVE_TEXEL_DEPTH16] = { 2, 21, 1, 0 },
	[BCM2711_NATIVE_TEXEL_DEPTH24] = { 4, 24, 1, 0 },
	[BCM2711_NATIVE_TEXEL_DEPTH32F] = { 4, 23, 1, 0 },
	[BCM2711_NATIVE_TEXEL_RGBA8_SRGB] = { 4, 4, 4, 1 },
	[BCM2711_NATIVE_TEXEL_R8_SNORM] = { 1, 1, 1, 0 },
	[BCM2711_NATIVE_TEXEL_RG8_SNORM] = { 2, 3, 2, 0 },
	[BCM2711_NATIVE_TEXEL_RGBA8_SNORM] = { 4, 5, 4, 0 },
	[BCM2711_NATIVE_TEXEL_RGB565] = { 2, 6, 3, 0 },
	[BCM2711_NATIVE_TEXEL_RGBA4] = { 2, 7, 4, 0 },
	[BCM2711_NATIVE_TEXEL_RGB5_A1] = { 2, 8, 4, 0 },
	[BCM2711_NATIVE_TEXEL_RGB10_A2] = { 4, 9, 4, 0 },
	[BCM2711_NATIVE_TEXEL_R16] = { 2, 10, 1, 0 },
	[BCM2711_NATIVE_TEXEL_R16_SNORM] = { 2, 11, 1, 0 },
	[BCM2711_NATIVE_TEXEL_RG16] = { 4, 12, 2, 0 },
	[BCM2711_NATIVE_TEXEL_RG16_SNORM] = { 4, 13, 2, 0 },
	[BCM2711_NATIVE_TEXEL_RGBA16] = { 8, 14, 4, 0 },
	[BCM2711_NATIVE_TEXEL_RGBA16_SNORM] = { 8, 15, 4, 0 },
	[BCM2711_NATIVE_TEXEL_R16F] = { 2, 16, 1, 0 },
	[BCM2711_NATIVE_TEXEL_RG16F] = { 4, 17, 2, 0 },
	[BCM2711_NATIVE_TEXEL_RGBA16F] = { 8, 18, 4, 0 },
	[BCM2711_NATIVE_TEXEL_R11G11B10F] = { 4, 19, 3, 0 },
	[BCM2711_NATIVE_TEXEL_RGB9E5] = { 4, 20, 3, 0 },
	[BCM2711_NATIVE_TEXEL_R32F] = { 4, 29, 1, 0 },
	[BCM2711_NATIVE_TEXEL_RG32F] = { 8, 30, 2, 0 },
	[BCM2711_NATIVE_TEXEL_RGBA32F] = { 16, 31, 4, 0 },
	[BCM2711_NATIVE_TEXEL_R8I] = { 1, 96, 1, 0 },
	[BCM2711_NATIVE_TEXEL_R8UI] = { 1, 97, 1, 0 },
	[BCM2711_NATIVE_TEXEL_RG8I] = { 2, 98, 2, 0 },
	[BCM2711_NATIVE_TEXEL_RG8UI] = { 2, 99, 2, 0 },
	[BCM2711_NATIVE_TEXEL_RGBA8I] = { 4, 100, 4, 0 },
	[BCM2711_NATIVE_TEXEL_RGBA8UI] = { 4, 101, 4, 0 },
	[BCM2711_NATIVE_TEXEL_R16I] = { 2, 102, 1, 0 },
	[BCM2711_NATIVE_TEXEL_R16UI] = { 2, 103, 1, 0 },
	[BCM2711_NATIVE_TEXEL_RG16I] = { 4, 104, 2, 0 },
	[BCM2711_NATIVE_TEXEL_RG16UI] = { 4, 105, 2, 0 },
	[BCM2711_NATIVE_TEXEL_RGBA16I] = { 8, 106, 4, 0 },
	[BCM2711_NATIVE_TEXEL_RGBA16UI] = { 8, 107, 4, 0 },
	[BCM2711_NATIVE_TEXEL_R32I] = { 4, 108, 1, 0 },
	[BCM2711_NATIVE_TEXEL_R32UI] = { 4, 109, 1, 0 },
	[BCM2711_NATIVE_TEXEL_RG32I] = { 8, 110, 2, 0 },
	[BCM2711_NATIVE_TEXEL_RG32UI] = { 8, 111, 2, 0 },
	[BCM2711_NATIVE_TEXEL_RGBA32I] = { 16, 112, 4, 0 },
	[BCM2711_NATIVE_TEXEL_RGBA32UI] = { 16, 113, 4, 0 },
	[BCM2711_NATIVE_TEXEL_RGB10_A2UI] = { 4, 114, 4, 0 }
};

/*
 * Gives a texel format's size in bytes and its native texture type.
 */
int
bcm2711_native_texel_layout(
	enum bcm2711_native_texel texel,
	uint32_t *texel_bytes,
	uint32_t *type)
{
	/* A format of the table, which every one of the enumeration has. */
	if ((uint32_t)texel >= BCM2711_NATIVE_TEXELS || texel_formats[texel].bytes == 0)
		return EINVAL;
	*texel_bytes = texel_formats[texel].bytes;
	*type = texel_formats[texel].type;

	/* Succeeded: the texel's size and type. */
	return 0;
}

/*
 * Copies visible raster texels of one, two, four, eight or sixteen bytes into a complete zero-padded, strict UIF image.
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
	uint32_t layers;
	uint32_t stride;
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

	/* The 4.2 compiler samples no 3D texture. */
	if (texture->volume != 0)
		return ENOTSUP;

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

	/*
	 * The layers follow one another at the array stride, each a whole
	 * converted image (one layer's stride is its own storage).
	 */
	layers = texture->layers;
	if (layers == 0)
		layers = 1;
	stride = storage_bytes;
	if (layers > 1U) {
		if (texture->array_stride < storage_bytes || (texture->array_stride & 63U) != 0 || layers > 0x3fffU)
			return EINVAL;
		stride = texture->array_stride;
	}

	/* Whole scratch storage stays inside the native 32-bit GPU virtual address space. */
	end = (uint64_t)texture->address + (uint64_t)stride * layers;
	if (end > ((uint64_t)1 << 32))
		return EINVAL;

	/* Component swapping is one exact BGRA choice, without arbitrary swizzle state. */
	if (texture->swap_red_blue > 1U)
		return EINVAL;
	if (texture->swap_red_blue != 0 && texture->texel != BCM2711_NATIVE_TEXEL_RGBA8 && texture->texel != BCM2711_NATIVE_TEXEL_RGBA8_SRGB)
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

	/* A texel of fewer than four channels reads zero for its absent colours and one for alpha, as Vulkan defines them. */
	if (texel_formats[texture->texel].channels < 4U)
		alpha = 1U;
	if (texel_formats[texture->texel].channels < 3U)
		blue = 0U;
	if (texel_formats[texture->texel].channels < 2U)
		green = 0U;

	/* sRGB decoding has no lowering in this 4.2 record. */
	if (texel_formats[texture->texel].srgb != 0)
		return ENOTSUP;

	/* The view's swizzle picks among the texel's channels, zero and one. */
	error = view_swizzle(texture->swizzle, &red, &green, &blue, &alpha);
	if (error != 0)
		return error;

	/*
	 * Defines an unflipped normalized texture of the texel's type, its
	 * layers (the Image Depth of a 2D array or a cube, whose face is the
	 * layer) at its exact padded array stride.
	 * Values referenced: Mesa 25.0.7 src/broadcom/cle/v3d_packet.xml
	 * (Texture Shader State: Array Stride, Image Depth) and
	 * vulkan/v3dvx_image.c pack_texture_shader_state (the depth is the
	 * view's layer count, the stride the distance between layers).
	 */
	kern_memset(bytes, 0, BCM2711_NATIVE_TEXTURE_BYTES);
	write_bits(bytes, 0, 32, texture->address);
	write_bits(bytes, 32, 26, stride / 64U);
	write_bits(bytes, 58, 14, texture->width);
	write_bits(bytes, 72, 14, texture->height);
	write_bits(bytes, 86, 14, layers);
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

	/* Repeat, clamp-to-edge, mirrored-repeat, clamp-to-border and mirror-once are the hardware's wrap codes 0 to 4. */
	if (sampler->wrap_u > 4U || sampler->wrap_v > 4U || sampler->wrap_w > 4U)
		return ENOTSUP;

	/* The levels of detail are 4.8 fixed point below sixteen, the bias sixteen bits; a linear mip filter is an exact bit. */
	if (sampler->max_lod > 0xfffU || sampler->min_lod > sampler->max_lod || sampler->bias > 0xffffU || sampler->mip_linear > 1U)
		return EINVAL;

	/* The border is transparent black, opaque black or opaque white; the compare function has three bits. */
	if (sampler->border > 2U || sampler->compare > 7U)
		return EINVAL;

	/*
	 * The filters, the depth compare, the levels of detail and bias, the
	 * wraps and the border, without anisotropy.  The sampler's sRGB Disable
	 * (bit 7) stays clear, as Mesa leaves it: set, it turns off the decode
	 * the texture's format asks for.
	 * Values referenced: Mesa 26.2.6 src/broadcom/cle/v3d_packet.xml
	 * (Sampler State) and vulkan/v3dvx_device.c pack_sampler_state.
	 */
	kern_memset(bytes, 0, BCM2711_NATIVE_SAMPLER_BYTES);
	write_bits(bytes, 0, 1, sampler->nearest_mag);
	write_bits(bytes, 1, 1, sampler->nearest_min);
	write_bits(bytes, 2, 1, sampler->mip_linear ^ 1U);
	write_bits(bytes, 4, 3, sampler->compare);
	write_bits(bytes, 8, 12, sampler->min_lod);
	write_bits(bytes, 20, 12, sampler->max_lod);
	write_bits(bytes, 32, 16, sampler->bias);
	write_bits(bytes, 48, 3, sampler->wrap_u);
	write_bits(bytes, 51, 3, sampler->wrap_v);
	write_bits(bytes, 54, 3, sampler->wrap_w);
	write_bits(bytes, 58, 3, sampler->border);

	/* Succeeded: the record holds every sampling choice. */
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
	case 8U:
		*utile_width = 4U;
		*utile_height = 2U;
		break;
	case 16U:
		*utile_width = 2U;
		*utile_height = 2U;
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

/*
 * Composes a view's swizzle (VkComponentSwizzle numbers, zero the
 * identity) with the texel's channel selectors red to alpha (the Texture
 * Shader State's codes: zero 0, one 1, red 2 to alpha 5).
 */
static int
view_swizzle(
	const uint32_t *swizzle,
	uint32_t *red,
	uint32_t *green,
	uint32_t *blue,
	uint32_t *alpha)
{
	uint32_t texel[4];
	uint32_t chosen[4];
	uint32_t index;

	/* Each channel: its own, zero, one or another of the texel's. */
	texel[0] = *red;
	texel[1] = *green;
	texel[2] = *blue;
	texel[3] = *alpha;
	for (index = 0; index < 4U; index++) {
		if (swizzle[index] > 6U)
			return EINVAL;
		if (swizzle[index] == 0U)
			chosen[index] = texel[index];
		else if (swizzle[index] == 1U)
			chosen[index] = 0U;
		else if (swizzle[index] == 2U)
			chosen[index] = 1U;
		else
			chosen[index] = texel[swizzle[index] - 3U];
	}

	/* The composed selectors replace the texel's. */
	*red = chosen[0];
	*green = chosen[1];
	*blue = chosen[2];
	*alpha = chosen[3];

	/* Succeeded: the selectors are composed. */
	return 0;
}
