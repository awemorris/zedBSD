/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libwebp-compat: zedBSD's own implementation of the decoding part of
 * libwebp's interface.  It holds no libwebp code; the names and values are
 * libwebp 1.5.0's <webp/decode.h>, declared here anew.  The structures are
 * written with libwebp's field names and are not promised to be laid out
 * like the real ones: only programs built against this header use the
 * library (its SONAME is libwebp-compat.so).  Programs of the packages use
 * the real libwebp (/usr/include/webp/decode.h); base programs include
 * <compat/webp/decode.h>.
 *
 * The subset: still images in every form a WebP file takes them (the
 * lossy VP8 key frame, the lossless VP8L image, the ALPH transparency of a
 * lossy image, the simple and the extended RIFF layouts, and bare VP8 or
 * VP8L bitstreams without RIFF), decoded whole from memory:
 *
 *   - WebPGetDecoderVersion, WebPGetInfo and WebPGetFeatures;
 *   - WebPDecodeRGBA, ARGB, BGRA, RGB, BGR and YUV, and their *Into forms;
 *   - WebPInitDecoderConfig, WebPDecode, WebPInitDecBuffer and
 *     WebPFreeDecBuffer, with the options bypass_filtering,
 *     no_fancy_upsampling and flip;
 *   - WebPMalloc and WebPFree (<compat/webp/types.h>).
 *
 * Differences from libwebp, each refused with VP8_STATUS_UNSUPPORTED_FEATURE
 * (the simple calls return NULL):
 *
 *   - an animated file (WebPGetFeatures reports it; decoding is refused);
 *   - the colour spaces MODE_RGBA_4444, MODE_RGB_565 and MODE_rgbA_4444;
 *   - YUV output (WebPDecodeYUV, MODE_YUV, MODE_YUVA) of a lossless image
 *     (libwebp converts its RGB to YUV; the lossy planes are given as
 *     decoded);
 *   - the options use_cropping and use_scaling.
 *
 * use_threads is accepted and ignored; dithering_strength and
 * alpha_dithering_strength are ignored (the output is the one libwebp
 * gives with both at 0, its default).  The incremental decoder
 * (WebPINewDecoder and the other WebPI calls) and WebPIDecode are not
 * provided.
 *
 * The output, for the same file and settings, is byte for byte the one
 * libwebp 1.5.0 gives for every file libwebp's encoder writes: the lossy
 * planes are decoded exactly, converted to RGB with libwebp's default rule
 * (ITU-R BT.601 with the limited ranges, in 14-bit fixed point, and the
 * chroma raised to full size by the 9-3-3-1 "fancy" filter unless
 * no_fancy_upsampling is set), and alpha is straight unless a
 * premultiplied mode (MODE_rgbA, MODE_bgrA, MODE_Argb) is asked for.  The
 * lossy decoding follows RFC 6386's reference decoder where libwebp
 * departs from it, which no file of libwebp's encoder shows: a key frame
 * with segmentation but no segment data (the frame's quantizer and filter
 * level, where libwebp uses index 0), the clamp of a segment's filter
 * level before the deltas, and the subblock edges of a macroblock whose
 * only tokens are zeros.  An ALPH chunk's reserved and pre-processing
 * bits are ignored (libwebp refuses some values).  A status for a damaged
 * file may differ from libwebp's; a damaged file never makes the library
 * read or write outside its buffers, and the time a decoding takes grows
 * with the image's pixels (a lossless image of 16384 by 16384 pixels can
 * take a few bytes).
 */

#ifndef KERN_COMPAT_WEBP_DECODE_H
#define KERN_COMPAT_WEBP_DECODE_H

#include <compat/webp/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The version of the decoding interface: libwebp 1.5.0's (major 2, minor 9). */
#define WEBP_DECODER_ABI_VERSION	0x0209

typedef struct WebPRGBABuffer WebPRGBABuffer;
typedef struct WebPYUVABuffer WebPYUVABuffer;
typedef struct WebPDecBuffer WebPDecBuffer;
typedef struct WebPBitstreamFeatures WebPBitstreamFeatures;
typedef struct WebPDecoderOptions WebPDecoderOptions;
typedef struct WebPDecoderConfig WebPDecoderConfig;

/*
 * Reports the library's version as 0xMMmmrr (major, minor, revision):
 * 0x010500, the libwebp release whose interface this is.
 */
WEBP_EXTERN int WebPGetDecoderVersion(void);

/*
 * Reads the size of a WebP image from its first bytes: the canvas of an
 * extended file (an animation's too), else the bitstream's.  width and
 * height may be NULL.  The data need not be complete (WebPGetFeatures
 * says how much is needed).  Returns 1 and sets the sizes when
 * WebPGetFeatures would give VP8_STATUS_OK, else 0 (sizes unset).
 */
WEBP_NODISCARD WEBP_EXTERN int WebPGetInfo(const uint8_t *data, size_t data_size, int *width, int *height);

/*
 * Decodes a whole WebP image into a new buffer of 4 (RGBA, ARGB, BGRA) or
 * 3 (RGB, BGR) bytes a pixel, rows top to bottom with no gap (the stride
 * is the width times the bytes a pixel).  width and height, when not
 * NULL, receive the size.  Returns the buffer, which the caller frees with
 * WebPFree, or NULL on any failure (nothing to free; the sizes may have
 * been set from the headers).  Alpha is straight; an image without alpha
 * gives 255.
 */
WEBP_NODISCARD WEBP_EXTERN uint8_t *WebPDecodeRGBA(const uint8_t *data, size_t data_size, int *width, int *height);
WEBP_NODISCARD WEBP_EXTERN uint8_t *WebPDecodeARGB(const uint8_t *data, size_t data_size, int *width, int *height);
WEBP_NODISCARD WEBP_EXTERN uint8_t *WebPDecodeBGRA(const uint8_t *data, size_t data_size, int *width, int *height);
WEBP_NODISCARD WEBP_EXTERN uint8_t *WebPDecodeRGB(const uint8_t *data, size_t data_size, int *width, int *height);
WEBP_NODISCARD WEBP_EXTERN uint8_t *WebPDecodeBGR(const uint8_t *data, size_t data_size, int *width, int *height);

/*
 * Decodes a whole lossy WebP image into its Y'CbCr planes, 4:2:0, as the
 * VP8 decoding gives them (cropped to the image's size; alpha is
 * dropped).  The Y plane is returned; *u and *v point into the same
 * allocation, which the caller frees with WebPFree(returned pointer).
 * *stride is the width, *uv_stride is (width + 1) / 2; the chroma planes
 * have (height + 1) / 2 rows.  NULL on any failure, and for a lossless
 * image (see the differences above).  No argument may be NULL except
 * width and height.
 */
WEBP_NODISCARD WEBP_EXTERN uint8_t *WebPDecodeYUV(const uint8_t *data, size_t data_size, int *width, int *height,
	uint8_t **u, uint8_t **v, int *stride, int *uv_stride);

/*
 * Decodes a whole WebP image into the caller's buffer, in the layout of
 * the call's name, output_stride bytes from one row to the next.  The
 * buffer must hold output_stride * (height - 1) + width * bytes-a-pixel
 * bytes and output_stride must be at least width * bytes-a-pixel.
 * Returns output_buffer on success, NULL on failure (the buffer's content
 * is then unspecified).
 */
WEBP_NODISCARD WEBP_EXTERN uint8_t *WebPDecodeRGBAInto(const uint8_t *data, size_t data_size,
	uint8_t *output_buffer, size_t output_buffer_size, int output_stride);
WEBP_NODISCARD WEBP_EXTERN uint8_t *WebPDecodeARGBInto(const uint8_t *data, size_t data_size,
	uint8_t *output_buffer, size_t output_buffer_size, int output_stride);
WEBP_NODISCARD WEBP_EXTERN uint8_t *WebPDecodeBGRAInto(const uint8_t *data, size_t data_size,
	uint8_t *output_buffer, size_t output_buffer_size, int output_stride);
WEBP_NODISCARD WEBP_EXTERN uint8_t *WebPDecodeRGBInto(const uint8_t *data, size_t data_size,
	uint8_t *output_buffer, size_t output_buffer_size, int output_stride);
WEBP_NODISCARD WEBP_EXTERN uint8_t *WebPDecodeBGRInto(const uint8_t *data, size_t data_size,
	uint8_t *output_buffer, size_t output_buffer_size, int output_stride);

/*
 * Decodes a whole lossy WebP image into the caller's three planes, each
 * with its size and stride (chroma (width + 1) / 2 by (height + 1) / 2).
 * Returns luma on success, NULL on failure and for a lossless image.
 */
WEBP_NODISCARD WEBP_EXTERN uint8_t *WebPDecodeYUVInto(const uint8_t *data, size_t data_size,
	uint8_t *luma, size_t luma_size, int luma_stride,
	uint8_t *u, size_t u_size, int u_stride,
	uint8_t *v, size_t v_size, int v_stride);

/*
 * The colour spaces of the output.  A lower-case letter names a component
 * premultiplied by alpha.  The 4444 and 565 spaces are named for
 * compatibility and refused.
 */
typedef enum WEBP_CSP_MODE {
	MODE_RGB = 0,
	MODE_RGBA = 1,
	MODE_BGR = 2,
	MODE_BGRA = 3,
	MODE_ARGB = 4,
	MODE_RGBA_4444 = 5,
	MODE_RGB_565 = 6,
	MODE_rgbA = 7,
	MODE_bgrA = 8,
	MODE_Argb = 9,
	MODE_rgbA_4444 = 10,
	MODE_YUV = 11,
	MODE_YUVA = 12,
	MODE_LAST = 13
} WEBP_CSP_MODE;

/*
 * The questions libwebp's inline helpers answer about a colour space,
 * written as macros (each names its argument more than once).
 */
#define WebPIsPremultipliedMode(mode) \
	((mode) == MODE_rgbA || (mode) == MODE_bgrA || (mode) == MODE_Argb || (mode) == MODE_rgbA_4444)
#define WebPIsAlphaMode(mode) \
	((mode) == MODE_RGBA || (mode) == MODE_BGRA || (mode) == MODE_ARGB || (mode) == MODE_RGBA_4444 || \
	 (mode) == MODE_YUVA || WebPIsPremultipliedMode(mode))
#define WebPIsRGBMode(mode)		((mode) < MODE_YUV)

/* An output of 3 or 4 bytes a pixel: its first byte, its stride and its size in bytes. */
struct WebPRGBABuffer {
	uint8_t *rgba;
	int stride;
	size_t size;
};

/*
 * An output as planes: luma, the two chroma planes and alpha (a is
 * written for MODE_YUVA only; a lossy image without alpha gives 255), each
 * with its stride and its size in bytes.
 */
struct WebPYUVABuffer {
	uint8_t *y, *u, *v, *a;
	int y_stride;
	int u_stride, v_stride;
	int a_stride;
	size_t y_size;
	size_t u_size, v_size;
	size_t a_size;
};

/*
 * Where WebPDecode puts the pixels.  colorspace is chosen by the caller.
 * With is_external_memory above 0 the caller fills u (pointers, strides,
 * sizes) and width and height are set by the decoding.  With it 0 or
 * negative the memory is the library's: when private_memory is NULL the
 * decoding allocates it and fills u, width and height; when it is already
 * set (a buffer an earlier decoding filled and the caller did not free),
 * it is used as it is, with the u its fields describe, as libwebp uses
 * it.  WebPFreeDecBuffer frees the library's memory.  A negative stride in
 * external memory is refused.
 */
struct WebPDecBuffer {
	WEBP_CSP_MODE colorspace;
	int width, height;
	int is_external_memory;
	union {
		WebPRGBABuffer RGBA;
		WebPYUVABuffer YUVA;
	} u;
	uint32_t pad[4];
	uint8_t *private_memory;
};

/*
 * Clears a buffer for use (colorspace MODE_RGB, nothing allocated).
 * Returns 0 when version's major byte is not this header's or buffer is
 * NULL, 1 otherwise.  Called through WebPInitDecBuffer.
 */
WEBP_NODISCARD WEBP_EXTERN int WebPInitDecBufferInternal(WebPDecBuffer *buffer, int version);
#define WebPInitDecBuffer(buffer)	WebPInitDecBufferInternal((buffer), WEBP_DECODER_ABI_VERSION)

/*
 * Frees the memory the decoding allocated into a buffer (nothing for
 * external memory) and clears private_memory; NULL is ignored.
 */
WEBP_EXTERN void WebPFreeDecBuffer(WebPDecBuffer *buffer);

/* What a decoding call reports. */
typedef enum VP8StatusCode {
	VP8_STATUS_OK = 0,
	VP8_STATUS_OUT_OF_MEMORY,
	VP8_STATUS_INVALID_PARAM,
	VP8_STATUS_BITSTREAM_ERROR,
	VP8_STATUS_UNSUPPORTED_FEATURE,
	VP8_STATUS_SUSPENDED,
	VP8_STATUS_USER_ABORT,
	VP8_STATUS_NOT_ENOUGH_DATA
} VP8StatusCode;

/*
 * What the headers say about an image: its size (the canvas of an
 * extended file), whether it has alpha and animation, and its format (0
 * for an animation, 1 lossy, 2 lossless).
 */
struct WebPBitstreamFeatures {
	int width;
	int height;
	int has_alpha;
	int has_animation;
	int format;
	uint32_t pad[5];
};

/*
 * Reads an image's features from its first bytes (the data need not be
 * complete): features is cleared, then filled.  Returns VP8_STATUS_OK;
 * VP8_STATUS_INVALID_PARAM for NULL data or features, or another major
 * version; VP8_STATUS_NOT_ENOUGH_DATA for fewer than 12 bytes, or when the
 * data ends before the bitstream's first bytes (10 lossy, 5 lossless) --
 * except in an extended file, whose features are those of its VP8X chunk
 * (and of the chunks read after it) as soon as that chunk is complete;
 * VP8_STATUS_BITSTREAM_ERROR when the bytes are not a WebP image (the
 * fields of features are then unspecified).  An animation's features are
 * its canvas, its alpha flag, has_animation 1 and format 0.  Called
 * through WebPGetFeatures.
 */
WEBP_EXTERN VP8StatusCode WebPGetFeaturesInternal(const uint8_t *data, size_t data_size,
	WebPBitstreamFeatures *features, int version);
#define WebPGetFeatures(data, data_size, features) \
	WebPGetFeaturesInternal((data), (data_size), (features), WEBP_DECODER_ABI_VERSION)

/*
 * The options of WebPDecode.  bypass_filtering skips the lossy loop
 * filter; no_fancy_upsampling repeats each chroma sample over its 2x2
 * pixels instead of the 9-3-3-1 filter; flip gives the rows bottom to top.
 * The others are described with the differences at the top.
 */
struct WebPDecoderOptions {
	int bypass_filtering;
	int no_fancy_upsampling;
	int use_cropping;
	int crop_left, crop_top;
	int crop_width, crop_height;
	int use_scaling;
	int scaled_width, scaled_height;
	int use_threads;
	int dithering_strength;
	int flip;
	int alpha_dithering_strength;
	uint32_t pad[5];
};

/* The input's features, the output and the options of one WebPDecode. */
struct WebPDecoderConfig {
	WebPBitstreamFeatures input;
	WebPDecBuffer output;
	WebPDecoderOptions options;
};

/*
 * Clears a configuration (every option 0, the output as WebPInitDecBuffer
 * leaves it).  Returns 0 for a NULL config or another major version, 1
 * otherwise.  Called through WebPInitDecoderConfig.
 */
WEBP_NODISCARD WEBP_EXTERN int WebPInitDecoderConfigInternal(WebPDecoderConfig *config, int version);
#define WebPInitDecoderConfig(config)	WebPInitDecoderConfigInternal((config), WEBP_DECODER_ABI_VERSION)

/*
 * Decodes a whole WebP image as config says: config->input receives the
 * features, config->output the pixels in its colorspace (allocated, or
 * the caller's when is_external_memory is set).  Returns VP8_STATUS_OK;
 * VP8_STATUS_INVALID_PARAM for a NULL argument, a colorspace out of
 * range, or external memory too small, with a stride too short or
 * negative, or with a NULL plane; VP8_STATUS_UNSUPPORTED_FEATURE for what
 * the differences at the top list (an animation, whatever follows its
 * VP8X chunk); VP8_STATUS_BITSTREAM_ERROR for a damaged image, and when
 * the data ends inside the headers (WebPGetFeatures' NOT_ENOUGH_DATA);
 * VP8_STATUS_NOT_ENOUGH_DATA when it ends inside a chunk or the image
 * data; VP8_STATUS_OUT_OF_MEMORY.  On failure nothing is left allocated in
 * config->output (external memory may have been written).  With flip the
 * rows of every plane go bottom to top (for YUV and YUVA, Y and A by
 * image rows and U and V by chroma rows) and the strides stay positive.
 *
 * The checks come in libwebp's order: config NULL; the features
 * (WebPGetFeatures' failure, NOT_ENOUGH_DATA made BITSTREAM_ERROR); an
 * animation; the bitstream's headers (the lossy frame header, the
 * lossless header); then the output (colorspace, the external memory or
 * the allocation); then the decoding.  The first that fails gives the
 * status.
 *
 * Procedure referenced: libwebp 1.5.0 (BSD-3-Clause) src/dec/webp_dec.c
 * (WebPDecode, DecodeInto) and src/dec/buffer_dec.c (the buffer checks):
 * the statuses and their order.
 */
WEBP_EXTERN VP8StatusCode WebPDecode(const uint8_t *data, size_t data_size, WebPDecoderConfig *config);

#ifdef __cplusplus
}
#endif

#endif
