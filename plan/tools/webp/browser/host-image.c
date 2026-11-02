/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libbrowser's image module on WebP files, on the host: for each pair
 * FILE.webp EXPECT.pam, img_sniff must say IMG_WEBP and img_decode must
 * give the PAM's pixels (dwebp's RGBA) as 0xAARRGGBB, or for a file whose
 * EXPECT is "-", fail with EINVAL (an animation, a damaged file).
 *
 *   host-image FILE.webp EXPECT.pam|- ...
 *
 * Prints "ok NAME" or "mismatch NAME: ..." per pair and, last,
 * "host-image: PASS (<n> files)" or "host-image: FAIL (<m> of <n>)".
 */

#include "image/image.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *host_read(const char *path, size_t *size);
static int host_check(const char *file, const char *expect);

/*
 * Checks every pair of the command line.
 */
int
main(
	int argc,
	char **argv)
{
	int index;
	int total;
	int failed;

	total = 0;
	failed = 0;
	for (index = 1; index + 1 < argc; index += 2) {
		total++;
		if (host_check(argv[index], argv[index + 1]) != 0)
			failed++;
	}
	if (failed != 0 || total == 0) {
		printf("host-image: FAIL (%d of %d)\n", failed, total);
		return 1;
	}
	printf("host-image: PASS (%d files)\n", total);
	return 0;
}

/* Reads a whole file into a new buffer of exactly its size. */
static unsigned char *
host_read(
	const char *path,
	size_t *size)
{
	unsigned char *bytes;
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

/* Checks one pair; 0 when it matches. */
static int
host_check(
	const char *file,
	const char *expect)
{
	struct img_bitmap bitmap;
	unsigned char *bytes;
	unsigned char *pam;
	const unsigned char *rgba;
	char text[256];
	const char *end;
	size_t size;
	size_t pam_size;
	size_t index;
	size_t count;
	size_t wrong;
	unsigned width;
	unsigned height;
	uint32_t want;
	int error;

	/* The file and its kind. */
	bytes = host_read(file, &size);
	if (bytes == NULL) {
		printf("mismatch %s: unreadable\n", file);
		return 1;
	}
	if (img_sniff(bytes, size) != IMG_WEBP) {
		printf("mismatch %s: not sniffed as WebP\n", file);
		free(bytes);
		return 1;
	}

	/* The decoding; a file expected to fail must fail with EINVAL. */
	error = img_decode(bytes, size, &bitmap);
	free(bytes);
	if (strcmp(expect, "-") == 0) {
		if (error == 0)
			img_bitmap_release(&bitmap);
		if (error != EINVAL) {
			printf("mismatch %s: decoded with %d, expected EINVAL\n", file, error);
			return 1;
		}
		printf("ok %s (refused)\n", file);
		return 0;
	}
	if (error != 0) {
		printf("mismatch %s: img_decode %d\n", file, error);
		return 1;
	}

	/* The PAM's size and pixels. */
	pam = host_read(expect, &pam_size);
	if (pam == NULL) {
		printf("mismatch %s: no %s\n", file, expect);
		img_bitmap_release(&bitmap);
		return 1;
	}
	count = pam_size < sizeof(text) - 1U ? pam_size : sizeof(text) - 1U;
	memcpy(text, pam, count);
	text[count] = '\0';
	end = strstr(text, "ENDHDR\n");
	if (end == NULL || strstr(text, "WIDTH") == NULL || strstr(text, "HEIGHT") == NULL ||
	    sscanf(strstr(text, "WIDTH"), "WIDTH %u", &width) != 1 || sscanf(strstr(text, "HEIGHT"), "HEIGHT %u", &height) != 1 ||
	    (int)width != bitmap.width || (int)height != bitmap.height ||
	    pam_size < (size_t)(end - text) + 7U + (size_t)width * height * 4U) {
		printf("mismatch %s: size %dx%d differs from the PAM\n", file, bitmap.width, bitmap.height);
		free(pam);
		img_bitmap_release(&bitmap);
		return 1;
	}
	rgba = pam + (end - text) + 7;

	/* Every pixel. */
	count = (size_t)width * height;
	wrong = 0;
	for (index = 0; index < count; index++) {
		want = ((uint32_t)rgba[index * 4U + 3U] << 24) | ((uint32_t)rgba[index * 4U] << 16) |
			((uint32_t)rgba[index * 4U + 1U] << 8) | (uint32_t)rgba[index * 4U + 2U];
		if (bitmap.pixels[index] != want)
			wrong++;
	}
	free(pam);
	img_bitmap_release(&bitmap);
	if (wrong != 0) {
		printf("mismatch %s: %lu of %lu pixels\n", file, (unsigned long)wrong, (unsigned long)count);
		return 1;
	}
	printf("ok %s %ux%u\n", file, width, height);
	return 0;
}
