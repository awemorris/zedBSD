/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The calls of <compat/webp/decode.h> and <compat/webp/types.h>: the
 * container's parsing, the lossy or lossless decoding with the alpha
 * chunk, and the output into the caller's colour space and buffer.
 */

#include <compat/webp/decode.h>

#include "common.h"

#include <errno.h>
#include <stdlib.h>

/*
 * Allocates size bytes as the library allocates its outputs; NULL when the
 * memory is not there.
 */
void *
WebPMalloc(
	size_t size)
{
	UNUSED_PARAMETER(size);

	/* Not implemented yet. */
	return NULL;
}

/*
 * Frees a buffer the library returned (WebPDecodeRGBA and its kind) or
 * WebPMalloc gave; NULL is ignored.
 */
void
WebPFree(
	void *ptr)
{
	UNUSED_PARAMETER(ptr);

	/* Not implemented yet. */
}

/*
 * Reports the library's version as 0xMMmmrr (major, minor, revision):
 * 0x010500, the libwebp release whose interface this is.
 */
int
WebPGetDecoderVersion(
	void)
{
	/* Not implemented yet. */
	return 0;
}

/*
 * Reads the size of a WebP image from its first bytes: the canvas of an
 * extended file (an animation's too), else the bitstream's.
 */
int
WebPGetInfo(
	const uint8_t *data,
	size_t data_size,
	int *width,
	int *height)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(data_size);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);

	/* Not implemented yet. */
	return 0;
}

/*
 * Decodes a whole WebP image into a new buffer of 4 (RGBA, ARGB, BGRA) or 3
 * (RGB, BGR) bytes a pixel, rows top to bottom with no gap (the stride is
 * the width times the bytes a pixel).
 */
uint8_t *
WebPDecodeRGBA(
	const uint8_t *data,
	size_t data_size,
	int *width,
	int *height)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(data_size);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);

	/* Not implemented yet. */
	return NULL;
}

/*
 * Decodes a whole WebP image into a new buffer of 4 (RGBA, ARGB, BGRA) or 3
 * (RGB, BGR) bytes a pixel, rows top to bottom with no gap (the stride is
 * the width times the bytes a pixel).
 */
uint8_t *
WebPDecodeARGB(
	const uint8_t *data,
	size_t data_size,
	int *width,
	int *height)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(data_size);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);

	/* Not implemented yet. */
	return NULL;
}

/*
 * Decodes a whole WebP image into a new buffer of 4 (RGBA, ARGB, BGRA) or 3
 * (RGB, BGR) bytes a pixel, rows top to bottom with no gap (the stride is
 * the width times the bytes a pixel).
 */
uint8_t *
WebPDecodeBGRA(
	const uint8_t *data,
	size_t data_size,
	int *width,
	int *height)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(data_size);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);

	/* Not implemented yet. */
	return NULL;
}

/*
 * Decodes a whole WebP image into a new buffer of 4 (RGBA, ARGB, BGRA) or 3
 * (RGB, BGR) bytes a pixel, rows top to bottom with no gap (the stride is
 * the width times the bytes a pixel).
 */
uint8_t *
WebPDecodeRGB(
	const uint8_t *data,
	size_t data_size,
	int *width,
	int *height)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(data_size);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);

	/* Not implemented yet. */
	return NULL;
}

/*
 * Decodes a whole WebP image into a new buffer of 4 (RGBA, ARGB, BGRA) or 3
 * (RGB, BGR) bytes a pixel, rows top to bottom with no gap (the stride is
 * the width times the bytes a pixel).
 */
uint8_t *
WebPDecodeBGR(
	const uint8_t *data,
	size_t data_size,
	int *width,
	int *height)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(data_size);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);

	/* Not implemented yet. */
	return NULL;
}

/*
 * Decodes a whole lossy WebP image into its Y'CbCr planes, 4:2:0, as the VP8
 * decoding gives them (cropped to the image's size; alpha is dropped).
 */
uint8_t *
WebPDecodeYUV(
	const uint8_t *data,
	size_t data_size,
	int *width,
	int *height,
	uint8_t **u,
	uint8_t **v,
	int *stride,
	int *uv_stride)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(data_size);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(u);
	UNUSED_PARAMETER(v);
	UNUSED_PARAMETER(stride);
	UNUSED_PARAMETER(uv_stride);

	/* Not implemented yet. */
	return NULL;
}

/*
 * Decodes a whole WebP image into the caller's buffer, in the layout of the
 * call's name, output_stride bytes from one row to the next.
 */
uint8_t *
WebPDecodeRGBAInto(
	const uint8_t *data,
	size_t data_size,
	uint8_t *output_buffer,
	size_t output_buffer_size,
	int output_stride)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(data_size);
	UNUSED_PARAMETER(output_buffer);
	UNUSED_PARAMETER(output_buffer_size);
	UNUSED_PARAMETER(output_stride);

	/* Not implemented yet. */
	return NULL;
}

/*
 * Decodes a whole WebP image into the caller's buffer, in the layout of the
 * call's name, output_stride bytes from one row to the next.
 */
uint8_t *
WebPDecodeARGBInto(
	const uint8_t *data,
	size_t data_size,
	uint8_t *output_buffer,
	size_t output_buffer_size,
	int output_stride)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(data_size);
	UNUSED_PARAMETER(output_buffer);
	UNUSED_PARAMETER(output_buffer_size);
	UNUSED_PARAMETER(output_stride);

	/* Not implemented yet. */
	return NULL;
}

/*
 * Decodes a whole WebP image into the caller's buffer, in the layout of the
 * call's name, output_stride bytes from one row to the next.
 */
uint8_t *
WebPDecodeBGRAInto(
	const uint8_t *data,
	size_t data_size,
	uint8_t *output_buffer,
	size_t output_buffer_size,
	int output_stride)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(data_size);
	UNUSED_PARAMETER(output_buffer);
	UNUSED_PARAMETER(output_buffer_size);
	UNUSED_PARAMETER(output_stride);

	/* Not implemented yet. */
	return NULL;
}

/*
 * Decodes a whole WebP image into the caller's buffer, in the layout of the
 * call's name, output_stride bytes from one row to the next.
 */
uint8_t *
WebPDecodeRGBInto(
	const uint8_t *data,
	size_t data_size,
	uint8_t *output_buffer,
	size_t output_buffer_size,
	int output_stride)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(data_size);
	UNUSED_PARAMETER(output_buffer);
	UNUSED_PARAMETER(output_buffer_size);
	UNUSED_PARAMETER(output_stride);

	/* Not implemented yet. */
	return NULL;
}

/*
 * Decodes a whole WebP image into the caller's buffer, in the layout of the
 * call's name, output_stride bytes from one row to the next.
 */
uint8_t *
WebPDecodeBGRInto(
	const uint8_t *data,
	size_t data_size,
	uint8_t *output_buffer,
	size_t output_buffer_size,
	int output_stride)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(data_size);
	UNUSED_PARAMETER(output_buffer);
	UNUSED_PARAMETER(output_buffer_size);
	UNUSED_PARAMETER(output_stride);

	/* Not implemented yet. */
	return NULL;
}

/*
 * Decodes a whole lossy WebP image into the caller's three planes, each with
 * its size and stride (chroma (width + 1) / 2 by (height + 1) / 2).
 */
uint8_t *
WebPDecodeYUVInto(
	const uint8_t *data,
	size_t data_size,
	uint8_t *luma,
	size_t luma_size,
	int luma_stride,
	uint8_t *u,
	size_t u_size,
	int u_stride,
	uint8_t *v,
	size_t v_size,
	int v_stride)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(data_size);
	UNUSED_PARAMETER(luma);
	UNUSED_PARAMETER(luma_size);
	UNUSED_PARAMETER(luma_stride);
	UNUSED_PARAMETER(u);
	UNUSED_PARAMETER(u_size);
	UNUSED_PARAMETER(u_stride);
	UNUSED_PARAMETER(v);
	UNUSED_PARAMETER(v_size);
	UNUSED_PARAMETER(v_stride);

	/* Not implemented yet. */
	return NULL;
}

/*
 * Clears a buffer for use (colorspace MODE_RGB, nothing allocated).
 */
int
WebPInitDecBufferInternal(
	WebPDecBuffer *buffer,
	int version)
{
	UNUSED_PARAMETER(buffer);
	UNUSED_PARAMETER(version);

	/* Not implemented yet. */
	return 0;
}

/*
 * Frees the memory the decoding allocated into a buffer (nothing for
 * external memory) and clears private_memory; NULL is ignored.
 */
void
WebPFreeDecBuffer(
	WebPDecBuffer *buffer)
{
	UNUSED_PARAMETER(buffer);

	/* Not implemented yet. */
}

/*
 * Reads an image's features from its first bytes (the data need not be
 * complete).
 */
VP8StatusCode
WebPGetFeaturesInternal(
	const uint8_t *data,
	size_t data_size,
	WebPBitstreamFeatures *features,
	int version)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(data_size);
	UNUSED_PARAMETER(features);
	UNUSED_PARAMETER(version);

	/* Not implemented yet. */
	return VP8_STATUS_UNSUPPORTED_FEATURE;
}

/*
 * Clears a configuration (every option 0, the output as WebPInitDecBuffer
 * leaves it).
 */
int
WebPInitDecoderConfigInternal(
	WebPDecoderConfig *config,
	int version)
{
	UNUSED_PARAMETER(config);
	UNUSED_PARAMETER(version);

	/* Not implemented yet. */
	return 0;
}

/*
 * Decodes a whole WebP image as config says: config->input receives the
 * features, config->output the pixels in its colorspace (allocated, or the
 * caller's when is_external_memory is set).
 */
VP8StatusCode
WebPDecode(
	const uint8_t *data,
	size_t data_size,
	WebPDecoderConfig *config)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(data_size);
	UNUSED_PARAMETER(config);

	/* Not implemented yet. */
	return VP8_STATUS_UNSUPPORTED_FEATURE;
}
