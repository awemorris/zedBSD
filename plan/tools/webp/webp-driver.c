/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The libwebp-compat test driver.  The same source is built three ways:
 * for the host with the library's sources (the default), for the host
 * against the real libwebp (-DWEBP_DRIVER_LIBWEBP, the reference; libwebp
 * is a test oracle on the host only), and for zedBSD against
 * libwebp-compat.so:
 *
 *   webp-driver [--dump DIR] FILE...
 *
 * For each FILE it calls every call of the subset libwebp-compat provides,
 * in every colour space and option it supports, and prints one line per
 * call: the status or the result, the sizes, and a 64-bit FNV-1a hash of
 * the pixels (the rows' pixels only, not their padding), plus whether the
 * padding of the caller's buffers was left alone.  Calls outside the
 * subset (the 4444 and 565 spaces, YUV of a lossless image, cropping and
 * scaling) are not made.  Two builds agree when their outputs are equal
 * line for line.  --dump writes the simple calls' raw outputs into DIR as
 * <file name>.<call>.raw, for looking at a difference.
 */

#ifdef WEBP_DRIVER_LIBWEBP
#include <webp/decode.h>
#else
#include <compat/webp/decode.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The value padding bytes are filled with before a decoding into the caller's buffer. */
#define DRIVER_PAD	0xa5U

/* The extra bytes a row of the caller's buffer gets beyond its pixels. */
#define DRIVER_EXTRA	5

/* One simple decoding call and its bytes a pixel. */
struct driver_simple {
	const char *name;
	uint8_t *(*decode)(const uint8_t *, size_t, int *, int *);
	uint8_t *(*into)(const uint8_t *, size_t, uint8_t *, size_t, int);
	int bytes;
};

/* One configuration of WebPDecode: its name, colour space and options. */
struct driver_config {
	const char *name;
	WEBP_CSP_MODE mode;
	int no_fancy;
	int bypass;
	int flip;
	int external;
	int lossy_only;
};

/* The simple calls, in the order they are reported. */
static const struct driver_simple driver_simples[] = {
	{ "RGBA", WebPDecodeRGBA, WebPDecodeRGBAInto, 4 },
	{ "ARGB", WebPDecodeARGB, WebPDecodeARGBInto, 4 },
	{ "BGRA", WebPDecodeBGRA, WebPDecodeBGRAInto, 4 },
	{ "RGB", WebPDecodeRGB, WebPDecodeRGBInto, 3 },
	{ "BGR", WebPDecodeBGR, WebPDecodeBGRInto, 3 }
};

/* The configurations of WebPDecode, in the order they are reported. */
static const struct driver_config driver_configs[] = {
	{ "RGBA", MODE_RGBA, 0, 0, 0, 0, 0 },
	{ "BGRA", MODE_BGRA, 0, 0, 0, 0, 0 },
	{ "ARGB", MODE_ARGB, 0, 0, 0, 0, 0 },
	{ "RGB", MODE_RGB, 0, 0, 0, 0, 0 },
	{ "BGR", MODE_BGR, 0, 0, 0, 0, 0 },
	{ "rgbA", MODE_rgbA, 0, 0, 0, 0, 0 },
	{ "bgrA", MODE_bgrA, 0, 0, 0, 0, 0 },
	{ "Argb", MODE_Argb, 0, 0, 0, 0, 0 },
	{ "RGBA-nofancy", MODE_RGBA, 1, 0, 0, 0, 0 },
	{ "RGBA-nofilter", MODE_RGBA, 0, 1, 0, 0, 0 },
	{ "RGBA-flip", MODE_RGBA, 0, 0, 1, 0, 0 },
	{ "rgbA-all", MODE_rgbA, 1, 1, 1, 0, 0 },
	{ "RGB-flip", MODE_RGB, 0, 0, 1, 0, 0 },
	{ "RGBA-external", MODE_RGBA, 0, 0, 0, 1, 0 },
	{ "BGR-external-flip", MODE_BGR, 0, 0, 1, 1, 0 },
	{ "YUV", MODE_YUV, 0, 0, 0, 0, 1 },
	{ "YUVA", MODE_YUVA, 0, 0, 0, 0, 1 },
	{ "YUVA-nofilter", MODE_YUVA, 0, 1, 0, 0, 1 },
	{ "YUVA-flip", MODE_YUVA, 0, 0, 1, 0, 1 },
	{ "YUV-external-flip", MODE_YUV, 0, 0, 1, 1, 1 },
	{ "YUVA-external", MODE_YUVA, 0, 0, 0, 1, 1 }
};

static const char *driver_dump_dir;

static uint64_t driver_hash(uint64_t hash, const uint8_t *bytes, size_t count);
static uint64_t driver_hash_rows(const uint8_t *first, int stride, int rows, size_t row_bytes);
static int driver_pad_intact(const uint8_t *buffer, int stride, int rows, size_t row_bytes, size_t size);
static void driver_dump(const char *file, const char *call, const uint8_t *bytes, size_t size);
static void driver_file(const char *path);
static void driver_simple_calls(const char *path, const uint8_t *data, size_t size, int width, int height);
static void driver_yuv_calls(const uint8_t *data, size_t size, int width, int height);
static void driver_config_calls(const uint8_t *data, size_t size, int lossy);

/*
 * Reports every file of the command line.
 */
int
main(
	int argc,
	char **argv)
{
	int index;

	/* The options, then the files. */
	index = 1;
	if (argc > 2 && strcmp(argv[1], "--dump") == 0) {
		driver_dump_dir = argv[2];
		index = 3;
	}
	if (index >= argc) {
		fprintf(stderr, "usage: webp-driver [--dump DIR] FILE...\n");
		return 2;
	}
	printf("version %06x\n", (unsigned)WebPGetDecoderVersion());
	for (; index < argc; index++)
		driver_file(argv[index]);
	return 0;
}

/* Hashes bytes into hash (FNV-1a, 64 bits). */
static uint64_t
driver_hash(
	uint64_t hash,
	const uint8_t *bytes,
	size_t count)
{
	size_t index;

	for (index = 0; index < count; index++) {
		hash ^= bytes[index];
		hash *= 0x100000001b3ULL;
	}
	return hash;
}

/* Hashes the pixels of rows, row_bytes of each, stride apart. */
static uint64_t
driver_hash_rows(
	const uint8_t *first,
	int stride,
	int rows,
	size_t row_bytes)
{
	uint64_t hash;
	int row;

	hash = 0xcbf29ce484222325ULL;
	for (row = 0; row < rows; row++)
		hash = driver_hash(hash, first + (ptrdiff_t)row * stride, row_bytes);
	return hash;
}

/* Tells whether every byte of buffer outside the rows' pixels still holds DRIVER_PAD. */
static int
driver_pad_intact(
	const uint8_t *buffer,
	int stride,
	int rows,
	size_t row_bytes,
	size_t size)
{
	size_t index;
	size_t offset;

	for (index = 0; index < size; index++) {
		offset = index % (size_t)stride;
		if ((int)(index / (size_t)stride) < rows && offset < row_bytes)
			continue;
		if (buffer[index] != DRIVER_PAD)
			return 0;
	}
	return 1;
}

/* Writes a raw output when --dump is given. */
static void
driver_dump(
	const char *file,
	const char *call,
	const uint8_t *bytes,
	size_t size)
{
	char path[4096];
	const char *base;
	FILE *out;

	if (driver_dump_dir == NULL)
		return;
	base = strrchr(file, '/');
	base = base == NULL ? file : base + 1;
	snprintf(path, sizeof(path), "%s/%s.%s.raw", driver_dump_dir, base, call);
	out = fopen(path, "wb");
	if (out == NULL)
		return;
	fwrite(bytes, 1, size, out);
	fclose(out);
}

/* Reports one file. */
static void
driver_file(
	const char *path)
{

	WebPBitstreamFeatures features;
	WebPBitstreamFeatures partial;
	WebPDecoderConfig abi_config;
	VP8StatusCode status;
	uint8_t *data;
	FILE *file;
	long length;
	size_t size;
	size_t index;
	int width;
	int height;
	int ok;
	int lossy;

	/* The file's bytes. */
	file = fopen(path, "rb");
	if (file == NULL || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
		printf("file %s unreadable\n", path);
		return;
	}
	size = (size_t)length;
	data = malloc(size > 0U ? size : 1U);
	if (data == NULL || fread(data, 1, size, file) != size) {
		printf("file %s unreadable\n", path);
		fclose(file);
		free(data);
		return;
	}
	fclose(file);
	printf("file %s %lu\n", strrchr(path, '/') == NULL ? path : strrchr(path, '/') + 1, (unsigned long)size);

	/* The size and the features, of the whole file and of its first bytes. */
	width = -1;
	height = -1;
	ok = WebPGetInfo(data, size, &width, &height);
	printf("info %d %d %d\n", ok, ok ? width : 0, ok ? height : 0);
	memset(&features, 0, sizeof(features));
	status = WebPGetFeatures(data, size, &features);
	if (status == VP8_STATUS_OK)
		printf("features %d %d %d %d %d %d\n", (int)status, features.width, features.height, features.has_alpha,
			features.has_animation, features.format);
	else
		printf("features %d\n", (int)status);

	/* A lossy image also gets the YUV calls. */
	lossy = 0;
	if (status == VP8_STATUS_OK && features.format == 1)
		lossy = 1;
	for (index = 0; index < size && index <= 160U; index++) {
		memset(&partial, 0, sizeof(partial));
		status = WebPGetFeatures(data, index, &partial);
		if (status == VP8_STATUS_OK)
			printf("partial %lu %d %d %d %d %d %d\n", (unsigned long)index, (int)status, partial.width,
				partial.height, partial.has_alpha, partial.has_animation, partial.format);
		else
			printf("partial %lu %d\n", (unsigned long)index, (int)status);
	}

	/* The decodings. */
	if (ok) {
		driver_simple_calls(path, data, size, width, height);
		if (lossy)
			driver_yuv_calls(data, size, width, height);
	}
	driver_config_calls(data, size, lossy);

	/* The parameter checks. */
	status = WebPDecode(data, size, NULL);
	printf("null-config %d\n", (int)status);
	ok = WebPInitDecoderConfigInternal(&abi_config, WEBP_DECODER_ABI_VERSION + 0x100);
	printf("abi-mismatch %d\n", ok);
	free(data);
}

/* Reports the simple calls and their Into forms. */
static void
driver_simple_calls(
	const char *path,
	const uint8_t *data,
	size_t size,
	int width,
	int height)
{
	size_t index;
	uint8_t *pixels;
	uint8_t *buffer;
	uint8_t *result;
	size_t buffer_size;
	int stride;
	int got_width;
	int got_height;

	for (index = 0; index < sizeof(driver_simples) / sizeof(driver_simples[0]); index++) {
		/* The call that allocates. */
		got_width = -1;
		got_height = -1;
		pixels = driver_simples[index].decode(data, size, &got_width, &got_height);
		if (pixels == NULL) {
			printf("decode %s null\n", driver_simples[index].name);
		} else {
			printf("decode %s %d %d %016llx\n", driver_simples[index].name, got_width, got_height,
				(unsigned long long)driver_hash_rows(pixels, got_width * driver_simples[index].bytes, got_height,
				(size_t)got_width * (size_t)driver_simples[index].bytes));
			driver_dump(path, driver_simples[index].name, pixels,
				(size_t)got_width * (size_t)got_height * (size_t)driver_simples[index].bytes);
			WebPFree(pixels);
		}

		/* The call into a padded buffer, and into one a byte too small. */
		stride = width * driver_simples[index].bytes + DRIVER_EXTRA;
		buffer_size = (size_t)stride * (size_t)height;
		buffer = malloc(buffer_size);
		if (buffer == NULL)
			continue;
		memset(buffer, DRIVER_PAD, buffer_size);
		result = driver_simples[index].into(data, size, buffer, buffer_size, stride);
		if (result == NULL) {
			printf("into %s null\n", driver_simples[index].name);
		} else {
			printf("into %s %d %016llx pad %d\n", driver_simples[index].name, result == buffer,
				(unsigned long long)driver_hash_rows(buffer, stride, height,
				(size_t)width * (size_t)driver_simples[index].bytes),
				driver_pad_intact(buffer, stride, height, (size_t)width * (size_t)driver_simples[index].bytes,
				buffer_size));
		}
		result = driver_simples[index].into(data, size, buffer,
			(size_t)stride * (size_t)(height - 1) + (size_t)width * (size_t)driver_simples[index].bytes - 1U, stride);
		printf("into-small %s %s\n", driver_simples[index].name, result == NULL ? "null" : "decoded");
		free(buffer);
	}
}

/* Reports WebPDecodeYUV and WebPDecodeYUVInto (lossy images only). */
static void
driver_yuv_calls(
	const uint8_t *data,
	size_t size,
	int width,
	int height)
{
	uint8_t *y;
	uint8_t *u;
	uint8_t *v;
	uint8_t *planes;
	int got_width;
	int got_height;
	int stride;
	int uv_stride;
	int uv_width;
	int uv_height;
	size_t y_size;
	size_t uv_size;

	/* The call that allocates. */
	uv_width = (width + 1) / 2;
	uv_height = (height + 1) / 2;
	u = NULL;
	v = NULL;
	y = WebPDecodeYUV(data, size, &got_width, &got_height, &u, &v, &stride, &uv_stride);
	if (y == NULL) {
		printf("yuv null\n");
	} else {
		printf("yuv %d %d %d %d %016llx %016llx %016llx\n", got_width, got_height, stride, uv_stride,
			(unsigned long long)driver_hash_rows(y, stride, got_height, (size_t)got_width),
			(unsigned long long)driver_hash_rows(u, uv_stride, uv_height, (size_t)uv_width),
			(unsigned long long)driver_hash_rows(v, uv_stride, uv_height, (size_t)uv_width));
		WebPFree(y);
	}

	/* The call into padded planes. */
	stride = width + DRIVER_EXTRA;
	uv_stride = uv_width + DRIVER_EXTRA;
	y_size = (size_t)stride * (size_t)height;
	uv_size = (size_t)uv_stride * (size_t)uv_height;
	planes = malloc(y_size + 2U * uv_size);
	if (planes == NULL)
		return;
	memset(planes, DRIVER_PAD, y_size + 2U * uv_size);
	y = WebPDecodeYUVInto(data, size, planes, y_size, stride, planes + y_size, uv_size, uv_stride,
		planes + y_size + uv_size, uv_size, uv_stride);
	if (y == NULL) {
		printf("yuv-into null\n");
	} else {
		printf("yuv-into %d %016llx %016llx %016llx pad %d %d %d\n", y == planes,
			(unsigned long long)driver_hash_rows(planes, stride, height, (size_t)width),
			(unsigned long long)driver_hash_rows(planes + y_size, uv_stride, uv_height, (size_t)uv_width),
			(unsigned long long)driver_hash_rows(planes + y_size + uv_size, uv_stride, uv_height, (size_t)uv_width),
			driver_pad_intact(planes, stride, height, (size_t)width, y_size),
			driver_pad_intact(planes + y_size, uv_stride, uv_height, (size_t)uv_width, uv_size),
			driver_pad_intact(planes + y_size + uv_size, uv_stride, uv_height, (size_t)uv_width, uv_size));
	}
	free(planes);
}

/* Reports WebPDecode in each configuration. */
static void
driver_config_calls(
	const uint8_t *data,
	size_t size,
	int lossy)
{
	static const int mode_bytes[MODE_LAST] = { 3, 4, 3, 4, 4, 2, 2, 4, 4, 4, 2, 1, 1 };
	const struct driver_config *entry;
	const WebPYUVABuffer *planes;
	WebPDecoderConfig config;
	VP8StatusCode status;
	uint8_t *external;
	size_t index;
	size_t external_size;
	int width;
	int height;
	int bytes;
	int uv_stride;
	int uv_width;
	int uv_height;
	size_t y_size;
	size_t uv_size;

	for (index = 0; index < sizeof(driver_configs) / sizeof(driver_configs[0]); index++) {
		/* The configuration. */
		entry = &driver_configs[index];
		if (entry->lossy_only && !lossy)
			continue;
		if (!WebPInitDecoderConfig(&config)) {
			printf("config %s init-failed\n", entry->name);
			continue;
		}
		config.output.colorspace = entry->mode;
		config.options.no_fancy_upsampling = entry->no_fancy;
		config.options.bypass_filtering = entry->bypass;
		config.options.flip = entry->flip;
		external = NULL;
		external_size = 0;
		uv_stride = 0;
		y_size = 0;
		uv_size = 0;
		bytes = mode_bytes[entry->mode];

		/* External memory needs the size first. */
		if (entry->external) {
			if (WebPGetFeatures(data, size, &config.input) != VP8_STATUS_OK) {
				printf("config %s no-features\n", entry->name);
				continue;
			}
			width = config.input.width;
			height = config.input.height;
			config.output.is_external_memory = 1;
			if (!WebPIsRGBMode(entry->mode)) {
				uv_stride = (width + 1) / 2 + DRIVER_EXTRA;
				y_size = (size_t)(width + DRIVER_EXTRA) * (size_t)height;
				uv_size = (size_t)uv_stride * (size_t)((height + 1) / 2);
				external_size = 2U * y_size + 2U * uv_size;
				external = malloc(external_size);
				if (external == NULL)
					continue;
				memset(external, DRIVER_PAD, external_size);
				config.output.u.YUVA.y = external;
				config.output.u.YUVA.y_stride = width + DRIVER_EXTRA;
				config.output.u.YUVA.y_size = y_size;
				config.output.u.YUVA.u = external + y_size;
				config.output.u.YUVA.u_stride = uv_stride;
				config.output.u.YUVA.u_size = uv_size;
				config.output.u.YUVA.v = external + y_size + uv_size;
				config.output.u.YUVA.v_stride = uv_stride;
				config.output.u.YUVA.v_size = uv_size;
				config.output.u.YUVA.a = external + y_size + 2U * uv_size;
				config.output.u.YUVA.a_stride = width + DRIVER_EXTRA;
				config.output.u.YUVA.a_size = y_size;
			} else {
				config.output.u.RGBA.stride = width * bytes + DRIVER_EXTRA;
				external_size = (size_t)config.output.u.RGBA.stride * (size_t)height;
				external = malloc(external_size);
				if (external == NULL)
					continue;
				memset(external, DRIVER_PAD, external_size);
				config.output.u.RGBA.rgba = external;
				config.output.u.RGBA.size = external_size;
			}
		}

		/* The decoding and what it gave. */
		status = WebPDecode(data, size, &config);
		if (status != VP8_STATUS_OK) {
			printf("config %s %d\n", entry->name, (int)status);
		} else if (WebPIsRGBMode(entry->mode)) {
			printf("config %s 0 %d %d %016llx\n", entry->name, config.output.width, config.output.height,
				(unsigned long long)driver_hash_rows(config.output.u.RGBA.rgba, config.output.u.RGBA.stride,
				config.output.height, (size_t)config.output.width * (size_t)bytes));
		} else {
			planes = &config.output.u.YUVA;
			uv_width = (config.output.width + 1) / 2;
			uv_height = (config.output.height + 1) / 2;
			printf("config %s 0 %d %d %016llx %016llx %016llx", entry->name, config.output.width,
				config.output.height,
				(unsigned long long)driver_hash_rows(planes->y, planes->y_stride, config.output.height,
				(size_t)config.output.width),
				(unsigned long long)driver_hash_rows(planes->u, planes->u_stride, uv_height, (size_t)uv_width),
				(unsigned long long)driver_hash_rows(planes->v, planes->v_stride, uv_height, (size_t)uv_width));
			if (entry->mode == MODE_YUVA)
				printf(" %016llx", (unsigned long long)driver_hash_rows(planes->a, planes->a_stride,
					config.output.height, (size_t)config.output.width));
			printf("\n");
		}
		if (entry->external && status == VP8_STATUS_OK && WebPIsRGBMode(entry->mode))
			printf("config %s pad %d\n", entry->name, driver_pad_intact(external, config.output.u.RGBA.stride,
				config.output.height, (size_t)config.output.width * (size_t)bytes, external_size));
		if (entry->external && status == VP8_STATUS_OK && !WebPIsRGBMode(entry->mode))
			printf("config %s pad %d %d %d %d\n", entry->name,
				driver_pad_intact(external, config.input.width + DRIVER_EXTRA, config.input.height,
				(size_t)config.input.width, y_size),
				driver_pad_intact(external + y_size, uv_stride, (config.input.height + 1) / 2,
				(size_t)((config.input.width + 1) / 2), uv_size),
				driver_pad_intact(external + y_size + uv_size, uv_stride, (config.input.height + 1) / 2,
				(size_t)((config.input.width + 1) / 2), uv_size),
				entry->mode == MODE_YUVA ? driver_pad_intact(external + y_size + 2U * uv_size,
				config.input.width + DRIVER_EXTRA, config.input.height, (size_t)config.input.width, y_size) : 1);
		WebPFreeDecBuffer(&config.output);
		free(external);
	}
}
