/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The tests of decode.c, the interface: parameter checks, the colour
 * spaces and options refused, the status rules, the buffers (external,
 * allocated, reused, freed), the layouts and flip, on small lossless
 * images built with lbitwriter.h from RFC 9649 3 (simple prefix codes, so
 * every pixel's value is known by construction), an animated file's
 * container, and one 16384 by 16384 image of zero-bit pixels.
 */

#include "check.h"
#include "lbitwriter.h"

#include <compat/webp/decode.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The most bytes a test file takes. */
#define FILE_BYTES	1024U

/* A test file: its bytes and size. */
struct test_file {
	uint8_t bytes[FILE_BYTES];
	size_t size;
};

static void put_simple_code(struct lbitwriter *writer, unsigned count, unsigned first, unsigned second);
static void make_lossless(struct test_file *file, unsigned width, unsigned height, unsigned green_count,
	unsigned green0, unsigned green1, unsigned red, unsigned blue, unsigned alpha);
static void make_animation(struct test_file *file);
static void test_calls(void);
static void test_layouts(void);
static void test_refusals(void);
static void test_buffers(void);
static void test_statuses(void);
static void test_large(void);

/*
 * Runs the tests of decode.c.
 */
int
main(
	void)
{
	int status;

	/* Each group in turn. */
	test_calls();
	test_layouts();
	test_refusals();
	test_buffers();
	test_statuses();
	test_large();

	/* The summary line. */
	status = check_finish("decode");
	return status;
}

/*
 * Writes one simple prefix code (RFC 9649 3.7.2.1.1): count 1 or 2
 * symbols, each written with 8 bits.  With two, the smaller symbol has
 * codeword 0 and the larger 1 (both length 1, canonical order).
 */
static void
put_simple_code(
	struct lbitwriter *writer,
	unsigned count,
	unsigned first,
	unsigned second)
{
	/* Simple, the number of symbols, 8-bit first symbol, the symbols. */
	lbitwriter_put(writer, 1U, 1U);
	lbitwriter_put(writer, count - 1U, 1U);
	lbitwriter_put(writer, 1U, 1U);
	lbitwriter_put(writer, first, 8U);
	if (count == 2U)
		lbitwriter_put(writer, second, 8U);
}

/*
 * Makes a RIFF file holding a lossless image of width by height pixels
 * (RFC 9649 2.6, 3.4, 3.8): no transform, no colour cache, one group.
 * Every pixel has red, blue and alpha as given; green is green0 for all
 * pixels (green_count 1), or with green_count 2 green0 for the pixels of
 * even index and green1 for the others (green0 < green1, one bit each).
 */
static void
make_lossless(
	struct test_file *file,
	unsigned width,
	unsigned height,
	unsigned green_count,
	unsigned green0,
	unsigned green1,
	unsigned red,
	unsigned blue,
	unsigned alpha)
{
	static struct lbitwriter writer;
	size_t payload;
	size_t index;
	size_t pixels;

	/* The header: signature, sizes, alpha_is_used, version 0. */
	lbitwriter_init(&writer);
	lbitwriter_put(&writer, 0x2fU, 8U);
	lbitwriter_put(&writer, width - 1U, 14U);
	lbitwriter_put(&writer, height - 1U, 14U);
	lbitwriter_put(&writer, alpha != 255U ? 1U : 0U, 1U);
	lbitwriter_put(&writer, 0U, 3U);

	/* No transform, no colour cache, no meta prefix codes, then the five codes. */
	lbitwriter_put(&writer, 0U, 1U);
	lbitwriter_put(&writer, 0U, 1U);
	lbitwriter_put(&writer, 0U, 1U);
	put_simple_code(&writer, green_count, green0, green1);
	put_simple_code(&writer, 1U, red, 0U);
	put_simple_code(&writer, 1U, blue, 0U);
	put_simple_code(&writer, 1U, alpha, 0U);
	put_simple_code(&writer, 1U, 0U, 0U);

	/* The pixels: one bit of green each when green has two symbols, else nothing. */
	pixels = (size_t)width * height;
	if (green_count == 2U) {
		for (index = 0; index < pixels; index++)
			lbitwriter_put(&writer, (uint32_t)(index & 1U), 1U);
	}

	/* The RIFF file around the stream (the chunk padded to even). */
	payload = lbitwriter_size(&writer);
	memcpy(file->bytes, "RIFF", 4);
	file->bytes[4] = (uint8_t)((4U + 8U + payload + (payload & 1U)) & 0xffU);
	file->bytes[5] = (uint8_t)(((4U + 8U + payload + (payload & 1U)) >> 8) & 0xffU);
	file->bytes[6] = 0U;
	file->bytes[7] = 0U;
	memcpy(file->bytes + 8, "WEBPVP8L", 8);
	file->bytes[16] = (uint8_t)(payload & 0xffU);
	file->bytes[17] = (uint8_t)((payload >> 8) & 0xffU);
	file->bytes[18] = 0U;
	file->bytes[19] = 0U;
	memcpy(file->bytes + 20, writer.bytes, payload);
	if (payload & 1U)
		file->bytes[20U + payload] = 0U;
	file->size = 20U + payload + (payload & 1U);
}

/* Makes an animated file's container: RIFF, VP8X (Animation and Alpha, canvas 20x10), ANIM (RFC 9649 2.7). */
static void
make_animation(
	struct test_file *file)
{
	static const uint8_t bytes[] = {
		'R', 'I', 'F', 'F', 36, 0, 0, 0, 'W', 'E', 'B', 'P',
		'V', 'P', '8', 'X', 10, 0, 0, 0, 0x12, 0, 0, 0, 19, 0, 0, 9, 0, 0,
		'A', 'N', 'I', 'M', 6, 0, 0, 0, 0, 0, 0, 0, 0, 0
	};

	/* The bytes as they are. */
	memcpy(file->bytes, bytes, sizeof(bytes));
	file->size = sizeof(bytes);
}

/* Checks the version, the memory calls, the size and the features of a small image. */
static void
test_calls(void)
{
	static struct test_file file;
	WebPBitstreamFeatures features;
	uint8_t *memory;
	int width;
	int height;

	/* The version and the memory calls. */
	CHECK(WebPGetDecoderVersion() == 0x010500);
	memory = WebPMalloc(16U);
	CHECK(memory != NULL);
	WebPFree(memory);
	WebPFree(NULL);

	/* The size and the features of a 3x2 lossless image with alpha. */
	make_lossless(&file, 3U, 2U, 1U, 20U, 0U, 10U, 30U, 128U);
	width = 0;
	height = 0;
	CHECK(WebPGetInfo(file.bytes, file.size, &width, &height) == 1);
	CHECK(width == 3 && height == 2);
	CHECK(WebPGetInfo(file.bytes, file.size, NULL, NULL) == 1);
	CHECK(WebPGetFeatures(file.bytes, file.size, &features) == VP8_STATUS_OK);
	CHECK(features.width == 3 && features.height == 2 && features.has_alpha == 1 && features.has_animation == 0);
	CHECK(features.format == 2);

	/* An animation: its canvas and flags, and no decoding. */
	make_animation(&file);
	CHECK(WebPGetFeatures(file.bytes, file.size, &features) == VP8_STATUS_OK);
	CHECK(features.width == 20 && features.height == 10 && features.has_animation == 1 && features.has_alpha == 1);
	CHECK(features.format == 0);
	CHECK(WebPGetInfo(file.bytes, file.size, &width, &height) == 1 && width == 20 && height == 10);
	CHECK(WebPDecodeRGBA(file.bytes, file.size, &width, &height) == NULL);
}

/* Checks the byte order of every layout, premultiplication and flip, on known pixels. */
static void
test_layouts(void)
{
	static struct test_file file;
	WebPDecoderConfig config;
	uint8_t *pixels;
	int width;
	int height;

	/* Every pixel (red 10, green 20, blue 30, alpha 128). */
	make_lossless(&file, 3U, 2U, 1U, 20U, 0U, 10U, 30U, 128U);
	pixels = WebPDecodeRGBA(file.bytes, file.size, &width, &height);
	CHECK(pixels != NULL);
	if (pixels != NULL) {
		CHECK(pixels[0] == 10U && pixels[1] == 20U && pixels[2] == 30U && pixels[3] == 128U);
		CHECK(pixels[20] == 10U && pixels[23] == 128U);
		WebPFree(pixels);
	}
	pixels = WebPDecodeARGB(file.bytes, file.size, &width, &height);
	CHECK(pixels != NULL && pixels[0] == 128U && pixels[1] == 10U && pixels[2] == 20U && pixels[3] == 30U);
	WebPFree(pixels);
	pixels = WebPDecodeBGRA(file.bytes, file.size, &width, &height);
	CHECK(pixels != NULL && pixels[0] == 30U && pixels[1] == 20U && pixels[2] == 10U && pixels[3] == 128U);
	WebPFree(pixels);
	pixels = WebPDecodeRGB(file.bytes, file.size, &width, &height);
	CHECK(pixels != NULL && pixels[0] == 10U && pixels[1] == 20U && pixels[2] == 30U && pixels[3] == 10U);
	WebPFree(pixels);
	pixels = WebPDecodeBGR(file.bytes, file.size, &width, &height);
	CHECK(pixels != NULL && pixels[0] == 30U && pixels[1] == 20U && pixels[2] == 10U && pixels[5] == 10U);
	WebPFree(pixels);

	/* Premultiplied: floor(x * 128 / 255) = 5, 10, 15. */
	CHECK(WebPInitDecoderConfig(&config) == 1);
	config.output.colorspace = MODE_rgbA;
	CHECK(WebPDecode(file.bytes, file.size, &config) == VP8_STATUS_OK);
	pixels = config.output.u.RGBA.rgba;
	CHECK(pixels[0] == 5U && pixels[1] == 10U && pixels[2] == 15U && pixels[3] == 128U);
	WebPFreeDecBuffer(&config.output);
	config.output.colorspace = MODE_Argb;
	CHECK(WebPDecode(file.bytes, file.size, &config) == VP8_STATUS_OK);
	pixels = config.output.u.RGBA.rgba;
	CHECK(pixels[0] == 128U && pixels[1] == 5U && pixels[2] == 10U && pixels[3] == 15U);
	WebPFreeDecBuffer(&config.output);

	/* Flip: a 1x2 image with green 40 on top and 60 below comes out 60 first. */
	make_lossless(&file, 1U, 2U, 2U, 40U, 60U, 0U, 0U, 255U);
	pixels = WebPDecodeRGB(file.bytes, file.size, &width, &height);
	CHECK(pixels != NULL && pixels[1] == 40U && pixels[4] == 60U);
	WebPFree(pixels);
	CHECK(WebPInitDecoderConfig(&config) == 1);
	config.output.colorspace = MODE_RGB;
	config.options.flip = 1;
	CHECK(WebPDecode(file.bytes, file.size, &config) == VP8_STATUS_OK);
	pixels = config.output.u.RGBA.rgba;
	CHECK(config.output.u.RGBA.stride == 3);
	CHECK(pixels[1] == 60U && pixels[4] == 40U);
	WebPFreeDecBuffer(&config.output);
}

/* Checks the colour spaces and options refused, and the parameter errors. */
static void
test_refusals(void)
{
	static struct test_file file;
	WebPDecoderConfig config;
	WebPBitstreamFeatures features;
	WebPDecBuffer buffer;
	uint8_t *u;
	uint8_t *v;
	int stride;
	int uv_stride;
	int width;
	int height;

	make_lossless(&file, 3U, 2U, 1U, 20U, 0U, 10U, 30U, 255U);

	/* The 4444 and 565 spaces, YUV of a lossless image, cropping and scaling. */
	CHECK(WebPInitDecoderConfig(&config) == 1);
	config.output.colorspace = MODE_RGBA_4444;
	CHECK(WebPDecode(file.bytes, file.size, &config) == VP8_STATUS_UNSUPPORTED_FEATURE);
	config.output.colorspace = MODE_RGB_565;
	CHECK(WebPDecode(file.bytes, file.size, &config) == VP8_STATUS_UNSUPPORTED_FEATURE);
	config.output.colorspace = MODE_rgbA_4444;
	CHECK(WebPDecode(file.bytes, file.size, &config) == VP8_STATUS_UNSUPPORTED_FEATURE);
	config.output.colorspace = MODE_YUV;
	CHECK(WebPDecode(file.bytes, file.size, &config) == VP8_STATUS_UNSUPPORTED_FEATURE);
	config.output.colorspace = MODE_YUVA;
	CHECK(WebPDecode(file.bytes, file.size, &config) == VP8_STATUS_UNSUPPORTED_FEATURE);
	CHECK(WebPDecodeYUV(file.bytes, file.size, &width, &height, &u, &v, &stride, &uv_stride) == NULL);
	config.output.colorspace = MODE_RGBA;
	config.options.use_cropping = 1;
	CHECK(WebPDecode(file.bytes, file.size, &config) == VP8_STATUS_UNSUPPORTED_FEATURE);
	config.options.use_cropping = 0;
	config.options.use_scaling = 1;
	CHECK(WebPDecode(file.bytes, file.size, &config) == VP8_STATUS_UNSUPPORTED_FEATURE);
	config.options.use_scaling = 0;
	CHECK(config.output.private_memory == NULL);

	/* Colour spaces out of range. */
	config.output.colorspace = MODE_LAST;
	CHECK(WebPDecode(file.bytes, file.size, &config) == VP8_STATUS_INVALID_PARAM);
	config.output.colorspace = (WEBP_CSP_MODE)-1;
	CHECK(WebPDecode(file.bytes, file.size, &config) == VP8_STATUS_INVALID_PARAM);

	/* NULL arguments and another major version. */
	CHECK(WebPDecode(file.bytes, file.size, NULL) == VP8_STATUS_INVALID_PARAM);
	CHECK(WebPGetFeatures(NULL, file.size, &features) == VP8_STATUS_INVALID_PARAM);
	CHECK(WebPGetFeatures(file.bytes, file.size, NULL) == VP8_STATUS_INVALID_PARAM);
	CHECK(WebPGetFeaturesInternal(file.bytes, file.size, &features, WEBP_DECODER_ABI_VERSION + 0x100) ==
		VP8_STATUS_INVALID_PARAM);
	CHECK(WebPGetFeaturesInternal(file.bytes, file.size, &features, WEBP_DECODER_ABI_VERSION + 1) == VP8_STATUS_OK);
	CHECK(WebPInitDecoderConfigInternal(&config, WEBP_DECODER_ABI_VERSION - 0x100) == 0);
	CHECK(WebPInitDecoderConfigInternal(NULL, WEBP_DECODER_ABI_VERSION) == 0);
	CHECK(WebPInitDecBufferInternal(&buffer, WEBP_DECODER_ABI_VERSION + 0x100) == 0);
	CHECK(WebPInitDecBufferInternal(NULL, WEBP_DECODER_ABI_VERSION) == 0);
	CHECK(WebPInitDecBuffer(&buffer) == 1 && buffer.colorspace == MODE_RGB && buffer.private_memory == NULL);
	CHECK(WebPGetInfo(NULL, 100U, &width, &height) == 0);
	CHECK(WebPDecodeYUV(file.bytes, file.size, &width, &height, NULL, &v, &stride, &uv_stride) == NULL);
	CHECK(WebPDecodeRGBAInto(file.bytes, file.size, NULL, 100U, 12) == NULL);
	CHECK(WebPDecodeYUVInto(file.bytes, file.size, NULL, 100U, 4, u, 100U, 4, v, 100U, 4) == NULL);
	WebPFreeDecBuffer(NULL);
}

/* Checks external memory (sizes, strides, padding left alone), reuse and freeing. */
static void
test_buffers(void)
{
	static struct test_file file;
	static uint8_t external[64];
	WebPDecoderConfig config;
	uint8_t *private_memory;
	size_t index;
	int intact;

	/* A 3x2 image into 13-byte rows (12 bytes of pixels and one of padding), 25 bytes in all. */
	make_lossless(&file, 3U, 2U, 1U, 20U, 0U, 10U, 30U, 255U);
	memset(external, 0xa5, sizeof(external));
	CHECK(WebPDecodeRGBAInto(file.bytes, file.size, external, 25U, 13) == external);
	CHECK(external[0] == 10U && external[11] == 255U && external[12] == 0xa5U && external[13] == 10U);
	intact = 1;
	for (index = 25U; index < sizeof(external); index++)
		if (external[index] != 0xa5U)
			intact = 0;
	CHECK(intact);

	/* One byte too few, a stride too short, a negative stride. */
	CHECK(WebPDecodeRGBAInto(file.bytes, file.size, external, 24U, 13) == NULL);
	CHECK(WebPDecodeRGBAInto(file.bytes, file.size, external, 64U, 11) == NULL);
	CHECK(WebPDecodeRGBAInto(file.bytes, file.size, external, 64U, -12) == NULL);
	CHECK(WebPDecodeRGBInto(file.bytes, file.size, external, 64U, 6) == NULL);
	CHECK(WebPDecodeRGBInto(file.bytes, file.size, external, 17U, 9) == NULL);
	CHECK(WebPDecodeRGBInto(file.bytes, file.size, external, 18U, 9) == external);

	/* External memory in WebPDecode: freeing leaves the caller's memory (ASan would see a bad free). */
	CHECK(WebPInitDecoderConfig(&config) == 1);
	config.output.colorspace = MODE_BGRA;
	config.output.is_external_memory = 1;
	config.output.u.RGBA.rgba = external;
	config.output.u.RGBA.stride = 12;
	config.output.u.RGBA.size = 24U;
	CHECK(WebPDecode(file.bytes, file.size, &config) == VP8_STATUS_OK);
	CHECK(config.output.width == 3 && config.output.height == 2 && external[0] == 30U);
	WebPFreeDecBuffer(&config.output);
	CHECK(config.output.private_memory == NULL);
	config.output.u.RGBA.size = 23U;
	CHECK(WebPDecode(file.bytes, file.size, &config) == VP8_STATUS_INVALID_PARAM);

	/* The library's memory, then the same buffer again: reused, not leaked (ASan's leak check). */
	CHECK(WebPInitDecoderConfig(&config) == 1);
	config.output.colorspace = MODE_RGBA;
	CHECK(WebPDecode(file.bytes, file.size, &config) == VP8_STATUS_OK);
	private_memory = config.output.private_memory;
	CHECK(private_memory != NULL && config.output.u.RGBA.rgba == private_memory);
	CHECK(WebPDecode(file.bytes, file.size, &config) == VP8_STATUS_OK);
	CHECK(config.output.private_memory == private_memory);
	WebPFreeDecBuffer(&config.output);
	CHECK(config.output.private_memory == NULL);
}

/* Checks the status rules of the headers. */
static void
test_statuses(void)
{
	static struct test_file file;
	WebPBitstreamFeatures features;
	WebPDecoderConfig config;
	size_t cut;
	int bad;

	make_lossless(&file, 3U, 2U, 1U, 20U, 0U, 10U, 30U, 255U);

	/* Fewer than 12 bytes, and the headers cut short: NOT_ENOUGH_DATA, which WebPDecode reports as BITSTREAM_ERROR. */
	CHECK(WebPGetFeatures(file.bytes, 11U, &features) == VP8_STATUS_NOT_ENOUGH_DATA);
	CHECK(WebPGetFeatures(file.bytes, 22U, &features) == VP8_STATUS_NOT_ENOUGH_DATA);
	CHECK(WebPInitDecoderConfig(&config) == 1);
	CHECK(WebPDecode(file.bytes, 22U, &config) == VP8_STATUS_BITSTREAM_ERROR);

	/* The headers whole but the file cut: the decoding reports NOT_ENOUGH_DATA (the RIFF size passes the data). */
	CHECK(WebPGetFeatures(file.bytes, 25U, &features) == VP8_STATUS_OK);
	CHECK(WebPDecode(file.bytes, file.size - 1U, &config) == VP8_STATUS_NOT_ENOUGH_DATA);
	CHECK(config.output.private_memory == NULL);

	/* Not a WebP file. */
	CHECK(WebPGetFeatures((const uint8_t *)"RIFF\x10\0\0\0WAVEfmt xxxxxxxx", 24U, &features) ==
		VP8_STATUS_BITSTREAM_ERROR);

	/* Every cut fails cleanly (and nothing leaks: ASan's leak check). */
	bad = 0;
	for (cut = 0; cut < file.size; cut++) {
		CHECK(WebPInitDecoderConfig(&config) == 1);
		if (WebPDecode(file.bytes, cut, &config) == VP8_STATUS_OK) {
			bad++;
			WebPFreeDecBuffer(&config.output);
		}
	}
	CHECK(bad == 0);

	/* An animation is refused whatever the output. */
	make_animation(&file);
	CHECK(WebPInitDecoderConfig(&config) == 1);
	CHECK(WebPDecode(file.bytes, file.size, &config) == VP8_STATUS_UNSUPPORTED_FEATURE);
}

/*
 * Decodes a 16384 by 16384 lossless image whose codes all have one symbol
 * (no bit a pixel; a few dozen bytes): it must end, with every pixel the
 * one colour.  Prints the time it took.
 */
static void
test_large(void)
{
	static struct test_file file;
	WebPDecoderConfig config;
	const uint8_t *pixels;
	clock_t start;
	size_t index;
	size_t count;
	int wrong;

	/* The file: 16384 x 16384, red 1, green 2, blue 3, alpha 255. */
	make_lossless(&file, 16384U, 16384U, 1U, 2U, 0U, 1U, 3U, 255U);
	CHECK(file.size < 64U);

	/* The decoding into RGB, timed. */
	start = clock();
	CHECK(WebPInitDecoderConfig(&config) == 1);
	config.output.colorspace = MODE_RGB;
	CHECK(WebPDecode(file.bytes, file.size, &config) == VP8_STATUS_OK);
	printf("large: 16384x16384 from %lu bytes in %.1f s of CPU\n", (unsigned long)file.size,
		(double)(clock() - start) / CLOCKS_PER_SEC);
	if (config.output.private_memory == NULL)
		return;

	/* Every pixel the colour (sampled every 4097 pixels, and the last). */
	pixels = config.output.u.RGBA.rgba;
	count = (size_t)16384U * 16384U;
	wrong = 0;
	for (index = 0; index < count; index += 4097U)
		if (pixels[index * 3U] != 1U || pixels[index * 3U + 1U] != 2U || pixels[index * 3U + 2U] != 3U)
			wrong++;
	if (pixels[(count - 1U) * 3U + 2U] != 3U)
		wrong++;
	CHECK(wrong == 0);
	WebPFreeDecBuffer(&config.output);
}
