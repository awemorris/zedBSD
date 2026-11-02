/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The hostile-input target of libwebp-compat: one input goes through
 * every call of the interface (features, info, the simple decodings, the
 * YUV one, WebPDecode with options and with too small external memory).
 * Nothing is checked but that the library returns: ASan and UBSan, and
 * the time limit of the runner, catch the rest.  An input whose image has
 * more than FUZZ_PIXELS_MAX pixels is not decoded (only its features are
 * read): the decoding time grows with the pixels, and a lossless image of
 * 2^28 pixels takes a few bytes.
 *
 * Built two ways: with -fsanitize=fuzzer as a libFuzzer target
 * (fuzz.sh), and with -DFUZZ_MUTATE as a program that runs each file of
 * its command line and a fixed set of its mutations (mutate.sh):
 *
 *   fuzz-mutate FILE...
 *
 * printing "mutate: <file> <mutations> done" per file; a crash names the
 * file and the mutation on the line before it.
 */

#include <compat/webp/decode.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef FUZZ_MUTATE
#include <unistd.h>
#endif

/* The most pixels an input is decoded at (as libwebp's own fuzzers limit them). */
#define FUZZ_PIXELS_MAX		((size_t)1 << 22)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Runs one input through the interface. */
int
LLVMFuzzerTestOneInput(
	const uint8_t *data,
	size_t size)
{
	WebPBitstreamFeatures features;
	WebPDecoderConfig config;
	uint8_t small[64];
	uint8_t *pixels;
	uint8_t *u;
	uint8_t *v;
	int width;
	int height;
	int stride;
	int uv_stride;

	/* The features; an image too large to decode in the runner's time is left there. */
	(void)WebPGetFeatures(data, size, &features);
	if (WebPGetInfo(data, size, &width, &height) && (size_t)width * (size_t)height > FUZZ_PIXELS_MAX)
		return 0;

	/* The simple decodings. */
	pixels = WebPDecodeRGBA(data, size, &width, &height);
	WebPFree(pixels);
	pixels = WebPDecodeBGR(data, size, &width, &height);
	WebPFree(pixels);
	pixels = WebPDecodeYUV(data, size, &width, &height, &u, &v, &stride, &uv_stride);
	WebPFree(pixels);
	(void)WebPDecodeRGBAInto(data, size, small, sizeof(small), 16);

	/* WebPDecode with every option it takes, and premultiplied output. */
	if (WebPInitDecoderConfig(&config)) {
		config.output.colorspace = MODE_rgbA;
		config.options.no_fancy_upsampling = 1;
		config.options.bypass_filtering = 1;
		config.options.flip = 1;
		(void)WebPDecode(data, size, &config);
		WebPFreeDecBuffer(&config.output);
	}
	if (WebPInitDecoderConfig(&config)) {
		config.output.colorspace = MODE_YUVA;
		(void)WebPDecode(data, size, &config);
		WebPFreeDecBuffer(&config.output);
	}
	return 0;
}

#ifdef FUZZ_MUTATE
/* Reads a whole file into a new buffer. */
static uint8_t *
mutate_read(
	const char *path,
	size_t *size)
{
	uint8_t *bytes;
	FILE *file;
	long length;

	file = fopen(path, "rb");
	if (file == NULL)
		return NULL;
	if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		return NULL;
	}
	bytes = malloc(length > 0 ? (size_t)length : 1U);
	if (bytes == NULL || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
		fclose(file);
		free(bytes);
		return NULL;
	}
	fclose(file);
	*size = (size_t)length;
	return bytes;
}

/*
 * Runs each file and its mutations: every cut at 48 positions spread over
 * the file (and every one of the first 40 bytes), 96 single bytes
 * replaced, 32 runs of 1 to 8 bytes replaced, all from a generator seeded
 * by the file's size, so a run is the same each time.
 */
int
main(
	int argc,
	char **argv)
{
	uint8_t *original;
	uint8_t *copy;
	uint8_t *exact;
	size_t size;
	size_t cut;
	size_t position;
	unsigned long state;
	unsigned count;
	unsigned index;
	unsigned run;
	int file;

	for (file = 1; file < argc; file++) {
		original = mutate_read(argv[file], &size);
		if (original == NULL || size == 0) {
			fprintf(stderr, "mutate: cannot read %s\n", argv[file]);
			free(original);
			continue;
		}
		copy = malloc(size);
		if (copy == NULL)
			return 1;
		alarm(300);
		count = 0;

		/* The file itself. */
		fprintf(stderr, "mutate: %s whole\n", argv[file]);
		LLVMFuzzerTestOneInput(original, size);
		count++;

		/* The cuts (the cut copy is exactly its size, so a read past it is caught). */
		for (index = 0; index < 88U; index++) {
			cut = index < 40U ? index : (size * (index - 40U)) / 48U;
			if (cut >= size)
				continue;
			exact = malloc(cut > 0U ? cut : 1U);
			if (exact == NULL)
				return 1;
			memcpy(exact, original, cut);
			fprintf(stderr, "mutate: %s cut %lu\n", argv[file], (unsigned long)cut);
			LLVMFuzzerTestOneInput(exact, cut);
			free(exact);
			count++;
		}

		/* The replaced bytes. */
		state = (unsigned long)size * 2654435761UL + 1UL;
		for (index = 0; index < 128U; index++) {
			memcpy(copy, original, size);
			state = (state * 1103515245UL + 12345UL) & 0xffffffffUL;
			position = (size_t)(state >> 8) % size;
			run = index < 96U ? 1U : 1U + (unsigned)((state >> 4) & 7UL);
			while (run > 0U && position < size) {
				state = (state * 1103515245UL + 12345UL) & 0xffffffffUL;
				copy[position] = (uint8_t)(state >> 16);
				position++;
				run--;
			}
			fprintf(stderr, "mutate: %s replace %u\n", argv[file], index);
			LLVMFuzzerTestOneInput(copy, size);
			count++;
		}
		alarm(0);
		printf("mutate: %s %u done\n", argv[file], count);
		free(copy);
		free(original);
	}
	return 0;
}
#endif
