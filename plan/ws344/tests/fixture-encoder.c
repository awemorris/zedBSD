/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A host encoder of test files for libwebp-compat, over the real libwebp
 * (used here only as a test oracle on the host, never in the tree): it
 * sets the WebPConfig fields cwebp has no option for (the token partitions
 * above all) and writes one file.
 *
 *   fixture-encoder IN.pam OUT.webp [key=value...]
 *
 * keys: q, method, partitions (0..3), segments, sns, filter_strength,
 * filter_sharpness, filter_type (0 simple, 1 strong), lossless, exact,
 * alpha_quality, alpha_filtering, alpha_compression, preprocessing,
 * near_lossless, use_sharp_yuv.  IN.pam is an RGB_ALPHA PAM as
 * gen-sources.py writes it.  Exit 0 on success.
 */

#include <webp/encode.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *read_pam(const char *path, int *width, int *height);
static int set_option(WebPConfig *config, const char *option);

/*
 * Encodes one file.
 */
int
main(
	int argc,
	char **argv)
{
	WebPConfig config;
	WebPPicture picture;
	WebPMemoryWriter writer;
	unsigned char *rgba;
	FILE *file;
	int width;
	int height;
	int index;
	int ok;

	/* The arguments and the image. */
	if (argc < 3) {
		fprintf(stderr, "usage: fixture-encoder IN.pam OUT.webp [key=value...]\n");
		return 2;
	}
	rgba = read_pam(argv[1], &width, &height);
	if (rgba == NULL)
		return 1;

	/* The configuration: libwebp's defaults, then the options. */
	if (!WebPConfigInit(&config) || !WebPPictureInit(&picture))
		return 1;
	for (index = 3; index < argc; index++) {
		if (set_option(&config, argv[index]) != 0) {
			fprintf(stderr, "fixture-encoder: bad option %s\n", argv[index]);
			return 2;
		}
	}
	if (!WebPValidateConfig(&config)) {
		fprintf(stderr, "fixture-encoder: invalid configuration\n");
		return 2;
	}

	/* The picture and the encoding into memory. */
	picture.use_argb = config.lossless;
	picture.width = width;
	picture.height = height;
	if (!WebPPictureImportRGBA(&picture, rgba, width * 4))
		return 1;
	WebPMemoryWriterInit(&writer);
	picture.writer = WebPMemoryWrite;
	picture.custom_ptr = &writer;
	ok = WebPEncode(&config, &picture);
	WebPPictureFree(&picture);
	free(rgba);
	if (!ok) {
		fprintf(stderr, "fixture-encoder: encoding failed (%d)\n", (int)picture.error_code);
		return 1;
	}

	/* The file. */
	file = fopen(argv[2], "wb");
	if (file == NULL || fwrite(writer.mem, 1, writer.size, file) != writer.size || fclose(file) != 0) {
		fprintf(stderr, "fixture-encoder: cannot write %s\n", argv[2]);
		return 1;
	}
	WebPMemoryWriterClear(&writer);
	return 0;
}

/* Reads an RGB_ALPHA PAM into a new buffer of RGBA bytes. */
static unsigned char *
read_pam(
	const char *path,
	int *width,
	int *height)
{
	char line[256];
	unsigned char *rgba;
	FILE *file;
	size_t size;
	int depth;

	/* The header lines up to ENDHDR. */
	file = fopen(path, "rb");
	if (file == NULL) {
		fprintf(stderr, "fixture-encoder: cannot open %s\n", path);
		return NULL;
	}
	*width = 0;
	*height = 0;
	depth = 0;
	while (fgets(line, sizeof(line), file) != NULL) {
		if (strncmp(line, "ENDHDR", 6) == 0)
			break;
		sscanf(line, "WIDTH %d", width);
		sscanf(line, "HEIGHT %d", height);
		sscanf(line, "DEPTH %d", &depth);
	}
	if (*width <= 0 || *height <= 0 || depth != 4) {
		fprintf(stderr, "fixture-encoder: %s is not an RGBA PAM\n", path);
		fclose(file);
		return NULL;
	}

	/* The pixels. */
	size = (size_t)*width * (size_t)*height * 4U;
	rgba = malloc(size);
	if (rgba == NULL || fread(rgba, 1, size, file) != size) {
		fprintf(stderr, "fixture-encoder: short %s\n", path);
		fclose(file);
		free(rgba);
		return NULL;
	}
	fclose(file);
	return rgba;
}

/* Sets one key=value option of the configuration; 0, or 1 for an unknown key. */
static int
set_option(
	WebPConfig *config,
	const char *option)
{
	const char *equals;
	char key[64];
	double value;
	size_t length;

	/* The key and its number. */
	equals = strchr(option, '=');
	if (equals == NULL)
		return 1;
	length = (size_t)(equals - option);
	if (length >= sizeof(key))
		return 1;
	memcpy(key, option, length);
	key[length] = '\0';
	value = atof(equals + 1);

	/* The field it names. */
	if (strcmp(key, "q") == 0)
		config->quality = (float)value;
	else if (strcmp(key, "method") == 0)
		config->method = (int)value;
	else if (strcmp(key, "partitions") == 0)
		config->partitions = (int)value;
	else if (strcmp(key, "segments") == 0)
		config->segments = (int)value;
	else if (strcmp(key, "sns") == 0)
		config->sns_strength = (int)value;
	else if (strcmp(key, "filter_strength") == 0)
		config->filter_strength = (int)value;
	else if (strcmp(key, "filter_sharpness") == 0)
		config->filter_sharpness = (int)value;
	else if (strcmp(key, "filter_type") == 0)
		config->filter_type = (int)value;
	else if (strcmp(key, "lossless") == 0)
		config->lossless = (int)value;
	else if (strcmp(key, "exact") == 0)
		config->exact = (int)value;
	else if (strcmp(key, "alpha_quality") == 0)
		config->alpha_quality = (int)value;
	else if (strcmp(key, "alpha_filtering") == 0)
		config->alpha_filtering = (int)value;
	else if (strcmp(key, "alpha_compression") == 0)
		config->alpha_compression = (int)value;
	else if (strcmp(key, "preprocessing") == 0)
		config->preprocessing = (int)value;
	else if (strcmp(key, "near_lossless") == 0)
		config->near_lossless = (int)value;
	else if (strcmp(key, "use_sharp_yuv") == 0)
		config->use_sharp_yuv = (int)value;
	else
		return 1;
	return 0;
}
