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
#include "alpha.h"
#include "output.h"
#include "riff.h"
#include "vp8.h"
#include "vp8-header.h"
#include "vp8l.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The version WebPGetDecoderVersion reports: libwebp 1.5.0's, 0xMMmmrr. */
#define DECODE_VERSION		0x010500

/* The widest row of an output the library allocates, in bytes (as libwebp limits it). */
#define DECODE_ROW_BYTES_MAX	((uint64_t)1 << 31)

/*
 * The bytes a pixel of each colour space takes (a plane's sample for the
 * YUV spaces), indexed by WEBP_CSP_MODE.
 *
 * Values referenced: libwebp 1.5.0 (BSD-3-Clause) src/dec/buffer_dec.c,
 * kModeBpp (the bytes of each colour space, for the buffer checks).
 */
static const unsigned decode_mode_bytes[MODE_LAST] = { 3, 4, 3, 4, 4, 2, 2, 4, 4, 4, 2, 1, 1 };

static VP8StatusCode decode_status(int error);
static int decode_layout(WEBP_CSP_MODE mode, struct webpc_layout *layout);
static VP8StatusCode decode_headers(const uint8_t *data, size_t size, struct webpc_container *container);
static VP8StatusCode decode_prepare(WebPDecBuffer *buffer, const WebPDecoderOptions *options, int lossless, unsigned width, unsigned height);
static VP8StatusCode decode_allocate(WebPDecBuffer *buffer);
static VP8StatusCode decode_check(const WebPDecBuffer *buffer);
static VP8StatusCode decode_into(const uint8_t *data, size_t size, const WebPDecoderOptions *options, WebPDecBuffer *buffer);
static VP8StatusCode decode_lossy(const struct webpc_container *container, const WebPDecoderOptions *options, WebPDecBuffer *buffer);
static VP8StatusCode decode_lossless(const struct webpc_container *container, const WebPDecoderOptions *options, WebPDecBuffer *buffer);
static void decode_copy_plane(const uint8_t *source, ptrdiff_t source_stride, uint8_t *dst, int dst_stride, unsigned width, unsigned height, int flip);
static uint8_t *decode_simple(WEBP_CSP_MODE mode, const uint8_t *data, size_t size, int *width, int *height, WebPDecBuffer *kept);
static uint8_t *decode_rgb_into(WEBP_CSP_MODE mode, const uint8_t *data, size_t size, uint8_t *output, size_t output_size, int stride);

/*
 * Allocates size bytes as the library allocates its outputs.
 */
void *
WebPMalloc(
	size_t size)
{
	void *memory;

	/* The C library's allocation, which WebPFree undoes. */
	memory = malloc(size);
	return memory;
}

/*
 * Frees a buffer the library returned or WebPMalloc gave.
 */
void
WebPFree(
	void *ptr)
{
	/* The C library's free (NULL is ignored). */
	free(ptr);
}

/*
 * Reports the library's version.
 */
int
WebPGetDecoderVersion(
	void)
{
	/* The release whose interface this is. */
	return DECODE_VERSION;
}

/*
 * Reads the size of a WebP image from its first bytes.
 */
int
WebPGetInfo(
	const uint8_t *data,
	size_t data_size,
	int *width,
	int *height)
{
	WebPBitstreamFeatures features;
	VP8StatusCode status;

	/* The features; any failure is no answer. */
	status = WebPGetFeatures(data, data_size, &features);
	if (status != VP8_STATUS_OK)
		return 0;

	/* The sizes the caller asked for. */
	if (width != NULL)
		*width = features.width;
	if (height != NULL)
		*height = features.height;

	/* Succeeded: the size is known. */
	return 1;
}

/*
 * Decodes a whole WebP image into a new RGBA buffer.
 */
uint8_t *
WebPDecodeRGBA(
	const uint8_t *data,
	size_t data_size,
	int *width,
	int *height)
{
	uint8_t *pixels;

	/* The shared simple decoding, in this layout. */
	pixels = decode_simple(MODE_RGBA, data, data_size, width, height, NULL);
	return pixels;
}

/*
 * Decodes a whole WebP image into a new ARGB buffer.
 */
uint8_t *
WebPDecodeARGB(
	const uint8_t *data,
	size_t data_size,
	int *width,
	int *height)
{
	uint8_t *pixels;

	/* The shared simple decoding, in this layout. */
	pixels = decode_simple(MODE_ARGB, data, data_size, width, height, NULL);
	return pixels;
}

/*
 * Decodes a whole WebP image into a new BGRA buffer.
 */
uint8_t *
WebPDecodeBGRA(
	const uint8_t *data,
	size_t data_size,
	int *width,
	int *height)
{
	uint8_t *pixels;

	/* The shared simple decoding, in this layout. */
	pixels = decode_simple(MODE_BGRA, data, data_size, width, height, NULL);
	return pixels;
}

/*
 * Decodes a whole WebP image into a new RGB buffer.
 */
uint8_t *
WebPDecodeRGB(
	const uint8_t *data,
	size_t data_size,
	int *width,
	int *height)
{
	uint8_t *pixels;

	/* The shared simple decoding, in this layout. */
	pixels = decode_simple(MODE_RGB, data, data_size, width, height, NULL);
	return pixels;
}

/*
 * Decodes a whole WebP image into a new BGR buffer.
 */
uint8_t *
WebPDecodeBGR(
	const uint8_t *data,
	size_t data_size,
	int *width,
	int *height)
{
	uint8_t *pixels;

	/* The shared simple decoding, in this layout. */
	pixels = decode_simple(MODE_BGR, data, data_size, width, height, NULL);
	return pixels;
}

/*
 * Decodes a whole lossy WebP image into its Y'CbCr planes.
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
	WebPDecBuffer kept;
	uint8_t *luma;

	/* Every output pointer is needed. */
	if (u == NULL ||
	    v == NULL ||
	    stride == NULL ||
	    uv_stride == NULL)
		return NULL;

	/* The shared simple decoding as planes, keeping where the planes are. */
	luma = decode_simple(MODE_YUV, data, data_size, width, height, &kept);
	if (luma == NULL)
		return NULL;

	/* Succeeded: the planes are in one allocation from luma on. */
	*u = kept.u.YUVA.u;
	*v = kept.u.YUVA.v;
	*stride = kept.u.YUVA.y_stride;
	*uv_stride = kept.u.YUVA.u_stride;
	return luma;
}

/*
 * Decodes a whole WebP image into the caller's RGBA buffer.
 */
uint8_t *
WebPDecodeRGBAInto(
	const uint8_t *data,
	size_t data_size,
	uint8_t *output_buffer,
	size_t output_buffer_size,
	int output_stride)
{
	uint8_t *result;

	/* The shared decoding into external memory, in this layout. */
	result = decode_rgb_into(MODE_RGBA, data, data_size, output_buffer, output_buffer_size, output_stride);
	return result;
}

/*
 * Decodes a whole WebP image into the caller's ARGB buffer.
 */
uint8_t *
WebPDecodeARGBInto(
	const uint8_t *data,
	size_t data_size,
	uint8_t *output_buffer,
	size_t output_buffer_size,
	int output_stride)
{
	uint8_t *result;

	/* The shared decoding into external memory, in this layout. */
	result = decode_rgb_into(MODE_ARGB, data, data_size, output_buffer, output_buffer_size, output_stride);
	return result;
}

/*
 * Decodes a whole WebP image into the caller's BGRA buffer.
 */
uint8_t *
WebPDecodeBGRAInto(
	const uint8_t *data,
	size_t data_size,
	uint8_t *output_buffer,
	size_t output_buffer_size,
	int output_stride)
{
	uint8_t *result;

	/* The shared decoding into external memory, in this layout. */
	result = decode_rgb_into(MODE_BGRA, data, data_size, output_buffer, output_buffer_size, output_stride);
	return result;
}

/*
 * Decodes a whole WebP image into the caller's RGB buffer.
 */
uint8_t *
WebPDecodeRGBInto(
	const uint8_t *data,
	size_t data_size,
	uint8_t *output_buffer,
	size_t output_buffer_size,
	int output_stride)
{
	uint8_t *result;

	/* The shared decoding into external memory, in this layout. */
	result = decode_rgb_into(MODE_RGB, data, data_size, output_buffer, output_buffer_size, output_stride);
	return result;
}

/*
 * Decodes a whole WebP image into the caller's BGR buffer.
 */
uint8_t *
WebPDecodeBGRInto(
	const uint8_t *data,
	size_t data_size,
	uint8_t *output_buffer,
	size_t output_buffer_size,
	int output_stride)
{
	uint8_t *result;

	/* The shared decoding into external memory, in this layout. */
	result = decode_rgb_into(MODE_BGR, data, data_size, output_buffer, output_buffer_size, output_stride);
	return result;
}

/*
 * Decodes a whole lossy WebP image into the caller's three planes.
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
	WebPDecBuffer buffer;
	VP8StatusCode status;

	/* Refuses a missing luma plane. */
	if (luma == NULL)
		return NULL;

	/* Describes the planes as external memory of MODE_YUV. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.colorspace = MODE_YUV;
	buffer.is_external_memory = 1;
	buffer.u.YUVA.y = luma;
	buffer.u.YUVA.y_stride = luma_stride;
	buffer.u.YUVA.y_size = luma_size;
	buffer.u.YUVA.u = u;
	buffer.u.YUVA.u_stride = u_stride;
	buffer.u.YUVA.u_size = u_size;
	buffer.u.YUVA.v = v;
	buffer.u.YUVA.v_stride = v_stride;
	buffer.u.YUVA.v_size = v_size;

	/* The decoding into them. */
	status = decode_into(data, data_size, NULL, &buffer);
	if (status != VP8_STATUS_OK)
		return NULL;

	/* Succeeded: the caller's planes hold the image. */
	return luma;
}

/*
 * Clears a buffer for use.
 */
int
WebPInitDecBufferInternal(
	WebPDecBuffer *buffer,
	int version)
{
	int incompatible;

	/* Refuses another major version's structures, or no buffer. */
	incompatible = WEBP_ABI_IS_INCOMPATIBLE(version, WEBP_DECODER_ABI_VERSION);
	if (incompatible)
		return 0;
	if (buffer == NULL)
		return 0;

	/* Succeeded: MODE_RGB, nothing allocated. */
	memset(buffer, 0, sizeof(*buffer));
	return 1;
}

/*
 * Frees the memory the decoding allocated into a buffer.
 */
void
WebPFreeDecBuffer(
	WebPDecBuffer *buffer)
{
	/* No buffer. */
	if (buffer == NULL)
		return;

	/* The library's memory (never the caller's), and no pointer left at it. */
	if (buffer->is_external_memory <= 0)
		free(buffer->private_memory);
	buffer->private_memory = NULL;
}

/*
 * Reads an image's features from its first bytes.
 */
VP8StatusCode
WebPGetFeaturesInternal(
	const uint8_t *data,
	size_t data_size,
	WebPBitstreamFeatures *features,
	int version)
{
	struct webpc_container container;
	VP8StatusCode status;
	int incompatible;
	int error;

	/* Refuses another major version, or nothing to fill or read. */
	incompatible = WEBP_ABI_IS_INCOMPATIBLE(version, WEBP_DECODER_ABI_VERSION);
	if (incompatible)
		return VP8_STATUS_INVALID_PARAM;
	if (features == NULL)
		return VP8_STATUS_INVALID_PARAM;
	if (data == NULL)
		return VP8_STATUS_INVALID_PARAM;

	/* The container read as far as the data goes. */
	memset(features, 0, sizeof(*features));
	error = webpc_riff_parse(data, data_size, 0, &container);
	if (error != 0) {
		status = decode_status(error);
		return status;
	}

	/* Succeeded: what the headers say. */
	features->width = (int)container.width;
	features->height = (int)container.height;
	features->has_alpha = container.has_alpha;
	features->has_animation = container.has_animation;
	features->format = container.format;
	return VP8_STATUS_OK;
}

/*
 * Clears a configuration.
 */
int
WebPInitDecoderConfigInternal(
	WebPDecoderConfig *config,
	int version)
{
	int incompatible;
	int ok;

	/* Refuses another major version, or no configuration. */
	incompatible = WEBP_ABI_IS_INCOMPATIBLE(version, WEBP_DECODER_ABI_VERSION);
	if (incompatible)
		return 0;
	if (config == NULL)
		return 0;

	/* Every field 0, the output as WebPInitDecBuffer leaves it. */
	memset(config, 0, sizeof(*config));
	ok = WebPInitDecBuffer(&config->output);
	if (!ok)
		return 0;

	/* Succeeded: the configuration is ready. */
	return 1;
}

/*
 * Decodes a whole WebP image as a configuration says.
 *
 * Procedure referenced: libwebp 1.5.0 (BSD-3-Clause) src/dec/webp_dec.c
 * (WebPDecode, DecodeInto): the features first, their NOT_ENOUGH_DATA
 * reported as BITSTREAM_ERROR, then the decoding into the output.
 */
VP8StatusCode
WebPDecode(
	const uint8_t *data,
	size_t data_size,
	WebPDecoderConfig *config)
{
	VP8StatusCode status;

	/* No configuration. */
	if (config == NULL)
		return VP8_STATUS_INVALID_PARAM;

	/* The features into the configuration; data that ends inside the headers is a broken file here. */
	status = WebPGetFeatures(data, data_size, &config->input);
	if (status == VP8_STATUS_NOT_ENOUGH_DATA)
		return VP8_STATUS_BITSTREAM_ERROR;
	if (status != VP8_STATUS_OK)
		return status;

	/* The decoding into the output with the options. */
	status = decode_into(data, data_size, &config->options, &config->output);
	if (status != VP8_STATUS_OK)
		return status;

	/* Succeeded: the output holds the image. */
	return VP8_STATUS_OK;
}

/* Turns an error value of the inner modules into the interface's status. */
static VP8StatusCode
decode_status(
	int error)
{
	/* Each error value its status; anything else is a broken file. */
	switch (error) {
	case 0:
		return VP8_STATUS_OK;
	case ENOBUFS:
		return VP8_STATUS_NOT_ENOUGH_DATA;
	case ENOMEM:
		return VP8_STATUS_OUT_OF_MEMORY;
	case ENOTSUP:
		return VP8_STATUS_UNSUPPORTED_FEATURE;
	default:
		return VP8_STATUS_BITSTREAM_ERROR;
	}
}

/* Fills the byte layout of an RGB colour space; 1 for a space that is not one of the supported ones. */
static int
decode_layout(
	WEBP_CSP_MODE mode,
	struct webpc_layout *layout)
{
	/* The bytes of a pixel and where each component goes. */
	memset(layout, 0, sizeof(*layout));
	switch (mode) {
	case MODE_RGB:
		layout->bytes = 3;
		layout->red = 0;
		layout->green = 1;
		layout->blue = 2;
		layout->alpha = -1;
		break;
	case MODE_RGBA:
	case MODE_rgbA:
		layout->bytes = 4;
		layout->red = 0;
		layout->green = 1;
		layout->blue = 2;
		layout->alpha = 3;
		break;
	case MODE_BGR:
		layout->bytes = 3;
		layout->red = 2;
		layout->green = 1;
		layout->blue = 0;
		layout->alpha = -1;
		break;
	case MODE_BGRA:
	case MODE_bgrA:
		layout->bytes = 4;
		layout->red = 2;
		layout->green = 1;
		layout->blue = 0;
		layout->alpha = 3;
		break;
	case MODE_ARGB:
	case MODE_Argb:
		layout->bytes = 4;
		layout->red = 1;
		layout->green = 2;
		layout->blue = 3;
		layout->alpha = 0;
		break;
	default:
		/* Not one of the supported RGB colour spaces. */
		return 1;
	}

	/* The premultiplied spaces multiply the colour by alpha. */
	if (mode == MODE_rgbA ||
	    mode == MODE_bgrA ||
	    mode == MODE_Argb)
		layout->premultiply = 1;

	/* Succeeded: the layout is filled. */
	return 0;
}

/*
 * Parses a whole file's container and its bitstream's headers, in
 * libwebp's order (the container, an animation, the frame header or the
 * lossless header).
 *
 * Procedure referenced: libwebp 1.5.0 (BSD-3-Clause) src/dec/webp_dec.c
 * (DecodeInto, WebPParseHeaders): the order of these checks.
 */
static VP8StatusCode
decode_headers(
	const uint8_t *data,
	size_t size,
	struct webpc_container *container)
{
	struct webpc_vp8_header vp8_header;
	struct webpc_vp8l_header vp8l_header;
	struct webpc_bool first;
	VP8StatusCode status;
	int error;

	/* Refuses no data at all. */
	if (data == NULL)
		return VP8_STATUS_INVALID_PARAM;

	/* Parses the whole container. */
	error = webpc_riff_parse(data, size, 1, container);
	if (error != 0) {
		status = decode_status(error);
		return status;
	}

	/* An animation is not decoded. */
	if (container->has_animation)
		return VP8_STATUS_UNSUPPORTED_FEATURE;

	/* The lossless header. */
	if (container->format == WEBPC_FORMAT_LOSSLESS) {
		error = webpc_vp8l_read_header(container->bitstream, container->bitstream_size, &vp8l_header);
		if (error != 0) {
			status = decode_status(error);
			return status;
		}

		/* Succeeded: the lossless image can be decoded. */
		return VP8_STATUS_OK;
	}

	/* Reads the frame tag of the lossy header. */
	error = webpc_vp8_read_tag(container->bitstream, container->bitstream_size, &vp8_header);
	if (error != 0) {
		status = decode_status(error);
		return status;
	}

	/* Reads the rest of the frame header, through to the first macroblock. */
	error = webpc_vp8_read_header(container->bitstream, container->bitstream_size, &vp8_header, &first);
	if (error != 0) {
		status = decode_status(error);
		return status;
	}

	/* Succeeded: the image can be decoded. */
	return VP8_STATUS_OK;
}

/*
 * Makes a buffer ready for an image of width by height: the colour space
 * and the options the library supports, the library's memory allocated
 * (or reused) or the caller's checked.
 */
static VP8StatusCode
decode_prepare(
	WebPDecBuffer *buffer,
	const WebPDecoderOptions *options,
	int lossless,
	unsigned width,
	unsigned height)
{
	WEBP_CSP_MODE mode;
	VP8StatusCode status;
	int rgb_mode;

	/* Refuses a colour space out of range. */
	mode = buffer->colorspace;
	if ((int)mode < (int)MODE_RGB || (int)mode >= (int)MODE_LAST)
		return VP8_STATUS_INVALID_PARAM;

	/* Refuses the colour spaces and options this library does not provide. */
	if (mode == MODE_RGBA_4444 ||
	    mode == MODE_RGB_565 ||
	    mode == MODE_rgbA_4444)
		return VP8_STATUS_UNSUPPORTED_FEATURE;
	rgb_mode = WebPIsRGBMode(mode);
	if (lossless && !rgb_mode)
		return VP8_STATUS_UNSUPPORTED_FEATURE;
	if (options != NULL &&
	    (options->use_cropping ||
	     options->use_scaling))
		return VP8_STATUS_UNSUPPORTED_FEATURE;

	/* Records the image's size in the buffer. */
	buffer->width = (int)width;
	buffer->height = (int)height;

	/* Allocates the library's memory when the buffer is the library's and has none yet. */
	if (buffer->is_external_memory <= 0 && buffer->private_memory == NULL) {
		status = decode_allocate(buffer);
		if (status != VP8_STATUS_OK)
			return status;
	}

	/* The buffer, whoever's it is, against the image. */
	status = decode_check(buffer);
	if (status != VP8_STATUS_OK)
		return status;

	/* Succeeded: the buffer takes the image. */
	return VP8_STATUS_OK;
}

/*
 * Allocates a buffer's memory for its colour space and size: one block,
 * the planes one after another, each without padding.
 *
 * Procedure referenced: libwebp 1.5.0 (BSD-3-Clause) src/dec/buffer_dec.c
 * (AllocateBuffer): the layout of the memory the library allocates.
 */
static VP8StatusCode
decode_allocate(
	WebPDecBuffer *buffer)
{
	uint64_t row_bytes;
	uint64_t size;
	uint64_t uv_size;
	uint64_t a_size;
	uint8_t *memory;
	int uv_stride;
	int rgb_mode;

	/* A row too wide for an int stride. */
	row_bytes = (uint64_t)buffer->width * decode_mode_bytes[buffer->colorspace];
	if (row_bytes >= DECODE_ROW_BYTES_MAX)
		return VP8_STATUS_INVALID_PARAM;

	/* The sizes: the RGB rows or the luma plane, then the chroma and alpha planes. */
	size = row_bytes * (uint64_t)buffer->height;
	uv_stride = (buffer->width + 1) / 2;
	uv_size = 0;
	a_size = 0;
	rgb_mode = WebPIsRGBMode(buffer->colorspace);
	if (!rgb_mode)
		uv_size = (uint64_t)uv_stride * (uint64_t)((buffer->height + 1) / 2);
	if (buffer->colorspace == MODE_YUVA)
		a_size = (uint64_t)buffer->width * (uint64_t)buffer->height;
	if (size + 2U * uv_size + a_size > (uint64_t)(size_t)-1)
		return VP8_STATUS_OUT_OF_MEMORY;

	/* The block. */
	memory = malloc((size_t)(size + 2U * uv_size + a_size));
	if (memory == NULL)
		return VP8_STATUS_OUT_OF_MEMORY;
	buffer->private_memory = memory;

	/* RGB: one stride of width pixels. */
	if (rgb_mode) {
		buffer->u.RGBA.rgba = memory;
		buffer->u.RGBA.stride = (int)row_bytes;
		buffer->u.RGBA.size = (size_t)size;
		return VP8_STATUS_OK;
	}

	/* YUV: luma, U, V and for YUVA alpha, one after another. */
	buffer->u.YUVA.y = memory;
	buffer->u.YUVA.y_stride = buffer->width;
	buffer->u.YUVA.y_size = (size_t)size;
	buffer->u.YUVA.u = memory + size;
	buffer->u.YUVA.u_stride = uv_stride;
	buffer->u.YUVA.u_size = (size_t)uv_size;
	buffer->u.YUVA.v = memory + size + uv_size;
	buffer->u.YUVA.v_stride = uv_stride;
	buffer->u.YUVA.v_size = (size_t)uv_size;
	buffer->u.YUVA.a_stride = 0;
	if (buffer->colorspace == MODE_YUVA) {
		buffer->u.YUVA.a = memory + size + 2U * uv_size;
		buffer->u.YUVA.a_stride = buffer->width;
	}

	/* The alpha plane's size, 0 when there is none. */
	buffer->u.YUVA.a_size = (size_t)a_size;

	/* Succeeded: the planes are laid out. */
	return VP8_STATUS_OK;
}

/*
 * Checks that a buffer's pointers, strides and sizes hold an image of its
 * width and height: each stride at least the row, each size at least
 * stride * (height - 1) + row; a negative stride is refused.
 *
 * Procedure referenced: libwebp 1.5.0 (BSD-3-Clause) src/dec/buffer_dec.c
 * (CheckDecBuffer, MIN_BUFFER_SIZE): the checks of the caller's memory.
 */
static VP8StatusCode
decode_check(
	const WebPDecBuffer *buffer)
{
	const WebPYUVABuffer *planes;
	const WebPRGBABuffer *rgb;
	uint64_t width;
	uint64_t height;
	uint64_t uv_width;
	uint64_t uv_height;
	uint64_t row;
	int rgb_mode;

	/* The image's size in the buffer. */
	width = (uint64_t)buffer->width;
	height = (uint64_t)buffer->height;
	rgb_mode = WebPIsRGBMode(buffer->colorspace);

	/* RGB: the stride and the size against the rows. */
	if (rgb_mode) {
		rgb = &buffer->u.RGBA;
		row = width * decode_mode_bytes[buffer->colorspace];
		if (rgb->rgba == NULL ||
		    rgb->stride < 0 ||
		    (uint64_t)rgb->stride < row)
			return VP8_STATUS_INVALID_PARAM;
		if ((uint64_t)rgb->stride * (height - 1U) + row > (uint64_t)rgb->size)
			return VP8_STATUS_INVALID_PARAM;

		/* Succeeded: the RGB memory holds the image. */
		return VP8_STATUS_OK;
	}

	/* YUV: each plane against its rows. */
	planes = &buffer->u.YUVA;
	uv_width = (width + 1U) / 2U;
	uv_height = (height + 1U) / 2U;
	if (planes->y == NULL ||
	    planes->u == NULL ||
	    planes->v == NULL)
		return VP8_STATUS_INVALID_PARAM;
	if (planes->y_stride < 0 ||
	    planes->u_stride < 0 ||
	    planes->v_stride < 0)
		return VP8_STATUS_INVALID_PARAM;
	if ((uint64_t)planes->y_stride < width ||
	    (uint64_t)planes->u_stride < uv_width ||
	    (uint64_t)planes->v_stride < uv_width)
		return VP8_STATUS_INVALID_PARAM;
	if ((uint64_t)planes->y_stride * (height - 1U) + width > (uint64_t)planes->y_size)
		return VP8_STATUS_INVALID_PARAM;
	if ((uint64_t)planes->u_stride * (uv_height - 1U) + uv_width > (uint64_t)planes->u_size)
		return VP8_STATUS_INVALID_PARAM;
	if ((uint64_t)planes->v_stride * (uv_height - 1U) + uv_width > (uint64_t)planes->v_size)
		return VP8_STATUS_INVALID_PARAM;

	/* YUVA: the alpha plane too. */
	if (buffer->colorspace == MODE_YUVA) {
		if (planes->a == NULL ||
		    planes->a_stride < 0 ||
		    (uint64_t)planes->a_stride < width)
			return VP8_STATUS_INVALID_PARAM;
		if ((uint64_t)planes->a_stride * (height - 1U) + width > (uint64_t)planes->a_size)
			return VP8_STATUS_INVALID_PARAM;
	}

	/* Succeeded: every plane holds the image. */
	return VP8_STATUS_OK;
}

/*
 * Decodes a whole file into a buffer: the headers, the buffer, the image.
 * On failure the library's memory in the buffer is freed (the caller's is
 * left, perhaps written).
 */
static VP8StatusCode
decode_into(
	const uint8_t *data,
	size_t size,
	const WebPDecoderOptions *options,
	WebPDecBuffer *buffer)
{
	struct webpc_container container;
	VP8StatusCode status;
	int lossless;

	/* Reads the headers. */
	status = decode_headers(data, size, &container);
	if (status != VP8_STATUS_OK)
		return status;

	/* The buffer for the image. */
	lossless = 0;
	if (container.format == WEBPC_FORMAT_LOSSLESS)
		lossless = 1;
	status = decode_prepare(buffer, options, lossless, container.width, container.height);
	if (status != VP8_STATUS_OK) {
		WebPFreeDecBuffer(buffer);
		return status;
	}

	/* The image. */
	if (lossless)
		status = decode_lossless(&container, options, buffer);
	else
		status = decode_lossy(&container, options, buffer);
	if (status != VP8_STATUS_OK) {
		WebPFreeDecBuffer(buffer);
		return status;
	}

	/* Succeeded: the buffer holds the image. */
	return VP8_STATUS_OK;
}

/* Decodes a lossy image (with its ALPH chunk) into a prepared buffer. */
static VP8StatusCode
decode_lossy(
	const struct webpc_container *container,
	const WebPDecoderOptions *options,
	WebPDecBuffer *buffer)
{
	struct webpc_vp8_picture picture;
	struct webpc_yuv_view view;
	struct webpc_layout layout;
	const WebPYUVABuffer *planes;
	const WebPRGBABuffer *rgb;
	VP8StatusCode status;
	uint8_t *alpha;
	uint8_t *first_row;
	ptrdiff_t row_step;
	unsigned width;
	unsigned height;
	unsigned row;
	int bypass;
	int fancy;
	int flip;
	int rgb_mode;
	int error;

	/* Reads the options. */
	bypass = 0;
	fancy = 1;
	flip = 0;
	if (options != NULL && options->bypass_filtering != 0)
		bypass = 1;
	if (options != NULL && options->no_fancy_upsampling != 0)
		fancy = 0;
	if (options != NULL && options->flip != 0)
		flip = 1;

	/* The frame. */
	error = webpc_vp8_decode(container->bitstream, container->bitstream_size, bypass, &picture);
	if (error != 0) {
		status = decode_status(error);
		return status;
	}

	/* The decoded picture's size. */
	width = picture.width;
	height = picture.height;

	/* The alpha chunk, decoded whenever there is one (its failure fails the image, as in libwebp). */
	alpha = NULL;
	if (container->alpha != NULL) {
		alpha = malloc((size_t)width * height);
		if (alpha == NULL) {
			webpc_vp8_picture_release(&picture);
			return VP8_STATUS_OUT_OF_MEMORY;
		}

		/* Decodes the alpha chunk into the plane. */
		error = webpc_alpha_decode(container->alpha, container->alpha_size, width, height, alpha);
		if (error != 0) {
			free(alpha);
			webpc_vp8_picture_release(&picture);

			/* An allocation failure keeps its status; any other failure is a broken file. */
			if (error == ENOMEM)
				return VP8_STATUS_OUT_OF_MEMORY;
			return VP8_STATUS_BITSTREAM_ERROR;
		}
	}

	/* RGB: the conversion, rows bottom to top with flip. */
	rgb_mode = WebPIsRGBMode(buffer->colorspace);
	if (rgb_mode) {
		view.y = picture.y;
		view.u = picture.u;
		view.v = picture.v;
		view.y_stride = picture.y_stride;
		view.uv_stride = picture.uv_stride;
		view.width = width;
		view.height = height;
		(void)decode_layout(buffer->colorspace, &layout);
		rgb = &buffer->u.RGBA;
		first_row = rgb->rgba;
		row_step = rgb->stride;
		if (flip) {
			first_row = rgb->rgba + (ptrdiff_t)(height - 1U) * rgb->stride;
			row_step = -(ptrdiff_t)rgb->stride;
		}

		/* Converts the planes to the layout's pixels. */
		webpc_output_from_yuv(&view, alpha, (ptrdiff_t)width, fancy, &layout, first_row, row_step);
	} else {
		/* YUV: the planes as decoded, and for YUVA the alpha or 255. */
		planes = &buffer->u.YUVA;
		decode_copy_plane(picture.y, picture.y_stride, planes->y, planes->y_stride, width, height, flip);
		decode_copy_plane(
			picture.u,
			picture.uv_stride,
			planes->u,
			planes->u_stride,
			(width + 1U) / 2U,
			(height + 1U) / 2U,
			flip);
		decode_copy_plane(
			picture.v,
			picture.uv_stride,
			planes->v,
			planes->v_stride,
			(width + 1U) / 2U,
			(height + 1U) / 2U,
			flip);

		/* The alpha plane: the decoded alpha, or opaque 255 when the image has none. */
		if (buffer->colorspace == MODE_YUVA && alpha != NULL)
			decode_copy_plane(alpha, (ptrdiff_t)width, planes->a, planes->a_stride, width, height, flip);
		if (buffer->colorspace == MODE_YUVA && alpha == NULL) {
			for (row = 0; row < height; row++)
				memset(planes->a + (ptrdiff_t)row * planes->a_stride, 255, width);
		}
	}

	/* Succeeded: the intermediate picture and alpha are done with. */
	free(alpha);
	webpc_vp8_picture_release(&picture);
	return VP8_STATUS_OK;
}

/* Decodes a lossless image into a prepared RGB buffer. */
static VP8StatusCode
decode_lossless(
	const struct webpc_container *container,
	const WebPDecoderOptions *options,
	WebPDecBuffer *buffer)
{
	struct webpc_layout layout;
	const WebPRGBABuffer *rgb;
	uint32_t *pixels;
	uint8_t *first_row;
	ptrdiff_t row_step;
	unsigned width;
	unsigned height;
	VP8StatusCode status;
	int error;

	/* Decodes the ARGB pixels. */
	error = webpc_vp8l_decode(container->bitstream, container->bitstream_size, &pixels, &width, &height);
	if (error != 0) {
		status = decode_status(error);
		return status;
	}

	/* The rows of the layout, bottom to top with flip. */
	(void)decode_layout(buffer->colorspace, &layout);
	rgb = &buffer->u.RGBA;
	first_row = rgb->rgba;
	row_step = rgb->stride;
	if (options != NULL && options->flip) {
		first_row = rgb->rgba + (ptrdiff_t)(height - 1U) * rgb->stride;
		row_step = -(ptrdiff_t)rgb->stride;
	}

	/* Writes the pixels in the layout's byte order. */
	webpc_output_from_argb(pixels, width, height, &layout, first_row, row_step);

	/* Succeeded: the ARGB copy is done with. */
	free(pixels);
	return VP8_STATUS_OK;
}

/* Copies a plane's rows (width bytes each) into the caller's plane, bottom to top with flip. */
static void
decode_copy_plane(
	const uint8_t *source,
	ptrdiff_t source_stride,
	uint8_t *dst,
	int dst_stride,
	unsigned width,
	unsigned height,
	int flip)
{
	unsigned row;
	unsigned target;

	/* Copies each row to its place. */
	for (row = 0; row < height; row++) {
		target = row;
		if (flip)
			target = height - 1U - row;
		memcpy(dst + (ptrdiff_t)target * dst_stride, source + (ptrdiff_t)row * source_stride, width);
	}
}

/*
 * The simple calls: the size from WebPGetInfo (written before the
 * decoding, as libwebp writes it), then the decoding into the library's
 * memory in mode.  kept, when not NULL, receives the buffer's description.
 *
 * Procedure referenced: libwebp 1.5.0 (BSD-3-Clause) src/dec/webp_dec.c
 * (Decode): the size reported before the decoding.
 */
static uint8_t *
decode_simple(
	WEBP_CSP_MODE mode,
	const uint8_t *data,
	size_t size,
	int *width,
	int *height,
	WebPDecBuffer *kept)
{
	WebPDecBuffer buffer;
	VP8StatusCode status;
	int info_width;
	int info_height;
	int rgb_mode;
	int known;

	/* Reads the size first. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.colorspace = mode;
	known = WebPGetInfo(data, size, &info_width, &info_height);
	if (!known)
		return NULL;
	if (width != NULL)
		*width = info_width;
	if (height != NULL)
		*height = info_height;

	/* The decoding into memory of the library's. */
	status = decode_into(data, size, NULL, &buffer);
	if (status != VP8_STATUS_OK)
		return NULL;

	/* Hands the caller the buffer's description when it asked for one. */
	if (kept != NULL)
		*kept = buffer;

	/* An RGB buffer's pixels are the first plane of the buffer. */
	rgb_mode = WebPIsRGBMode(mode);
	if (rgb_mode)
		return buffer.u.RGBA.rgba;

	/* Succeeded: the pixels (one allocation, freed with WebPFree). */
	return buffer.u.YUVA.y;
}

/* The Into calls of the RGB layouts: the caller's memory as external memory of mode. */
static uint8_t *
decode_rgb_into(
	WEBP_CSP_MODE mode,
	const uint8_t *data,
	size_t size,
	uint8_t *output,
	size_t output_size,
	int stride)
{
	WebPDecBuffer buffer;
	VP8StatusCode status;

	/* Refuses a missing caller's buffer. */
	if (output == NULL)
		return NULL;

	/* Describes the caller's buffer as external memory. */
	memset(&buffer, 0, sizeof(buffer));
	buffer.colorspace = mode;
	buffer.is_external_memory = 1;
	buffer.u.RGBA.rgba = output;
	buffer.u.RGBA.stride = stride;
	buffer.u.RGBA.size = output_size;

	/* The decoding into it. */
	status = decode_into(data, size, NULL, &buffer);
	if (status != VP8_STATUS_OK)
		return NULL;

	/* Succeeded: the caller's buffer holds the image. */
	return output;
}
