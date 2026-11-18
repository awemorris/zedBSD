/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The making of a preview (WS168 p004; preview.h): the input read whole
 * from a descriptor (64 MiB at most), decoded (decode.c) and scaled
 * (scale.c), and the PPM written to another descriptor.  keiland-preview
 * runs it in its sandbox; on FreeBSD, until the program confines itself
 * with Capsicum, the callers run it in their own process (freebsd/spawn.c).
 */

#include "preview.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* The piece read at once. */
#define MAKE_CHUNK		(256U * 1024U)

static int make_read(int fd, unsigned char **data, size_t *size);
static int make_write_all(int fd, const void *data, size_t size);
static void make_escape(int escape, int output);

/*
 * Makes the preview of an input descriptor into an output descriptor as
 * asked.  Returns the exit status (PREVIEW_*).
 */
int
preview_make(
	int input,
	int output,
	const struct preview_request *request)
{
	struct preview_image decoded;
	struct preview_image scaled;
	unsigned char *data;
	size_t size;
	int error;
	int result;

	/* What the file is, asked instead of a picture (info.c). */
	if (request->info) {
		result = preview_info(input, output);
		return result;
	}

	/* The input, whole. */
	error = make_read(input, &data, &size);
	if (error == EFBIG)
		return PREVIEW_TOO_LARGE;
	if (error == ENOMEM)
		return PREVIEW_NO_MEMORY;
	if (error != 0)
		return PREVIEW_DAMAGED;

	/* A test build's escape, where planted code would run. */
	if (PREVIEW_TEST_ESCAPE && request->escape != PREVIEW_ESCAPE_NONE)
		make_escape(request->escape, output);

	/* Decoded. */
	result = preview_decode(data, size, request, &decoded);
	free(data);
	if (result != PREVIEW_OK)
		return result;

	/* Scaled. */
	error = preview_scale(&decoded, request, &scaled);
	preview_image_release(&decoded);
	if (error != 0)
		return PREVIEW_NO_MEMORY;

	/* Written. */
	error = preview_write_ppm(output, &scaled, request->stamp);
	preview_image_release(&scaled);
	if (error != 0)
		return PREVIEW_NO_OUTPUT;
	return PREVIEW_OK;
}

/*
 * Writes a picture as a binary PPM (its comment "# STAMP" when a stamp is
 * given; the premultiplied colours as they are).  Returns 0 or an errno
 * value.
 */
int
preview_write_ppm(
	int fd,
	const struct preview_image *image,
	const char *stamp)
{
	unsigned char *row;
	char header[PREVIEW_STAMP_MAX + 64U];
	uint32_t pixel;
	int length;
	int x;
	int y;
	int error;

	/* The header. */
	if (stamp != NULL && stamp[0] != '\0')
		length = snprintf(header, sizeof(header), "P6\n# %s\n%d %d\n255\n", stamp, image->width, image->height);
	else
		length = snprintf(header, sizeof(header), "P6\n%d %d\n255\n", image->width, image->height);
	if (length < 0 || (size_t)length >= sizeof(header))
		return EINVAL;
	error = make_write_all(fd, header, (size_t)length);
	if (error != 0)
		return error;

	/* Each row as bytes. */
	row = malloc((size_t)image->width * 3U);
	if (row == NULL)
		return ENOMEM;
	for (y = 0; y < image->height; y++) {
		for (x = 0; x < image->width; x++) {
			pixel = image->pixels[(size_t)y * (size_t)image->width + (size_t)x];
			row[x * 3] = (unsigned char)(pixel >> 16);
			row[x * 3 + 1] = (unsigned char)(pixel >> 8);
			row[x * 3 + 2] = (unsigned char)pixel;
		}

		/* Written. */
		error = make_write_all(fd, row, (size_t)image->width * 3U);
		if (error != 0)
			break;
	}

	/* The row's room goes. */
	free(row);
	return error;
}

/* Reads a descriptor to its end; 0 with the bytes (the caller's), EFBIG, ENOMEM, or EIO. */
static int
make_read(
	int fd,
	unsigned char **data,
	size_t *size)
{
	unsigned char *buffer;
	unsigned char *grown;
	size_t length;
	size_t capacity;
	ssize_t got;

	/* Piece by piece, the room grown as it fills. */
	buffer = NULL;
	length = 0;
	capacity = 0;
	for (;;) {
		if (capacity - length < MAKE_CHUNK) {
			if (capacity >= PREVIEW_INPUT_MAX) {
				free(buffer);
				return EFBIG;
			}

			/* More room. */
			capacity += MAKE_CHUNK * 8U;
			grown = realloc(buffer, capacity);
			if (grown == NULL) {
				free(buffer);
				return ENOMEM;
			}

			/* Grown. */
			buffer = grown;
		}

		/* The next piece. */
		got = read(fd, buffer + length, MAKE_CHUNK);
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0) {
			free(buffer);
			return EIO;
		}

		/* The end. */
		if (got == 0)
			break;
		length += (size_t)got;
		if (length > PREVIEW_INPUT_MAX) {
			free(buffer);
			return EFBIG;
		}
	}

	/* Read through. */
	*data = buffer;
	*size = length;
	return 0;
}

/* Writes all the bytes; 0, or an errno value. */
static int
make_write_all(
	int fd,
	const void *data,
	size_t size)
{
	const unsigned char *cursor;
	ssize_t wrote;

	/* Until every byte is out. */
	cursor = data;
	while (size > 0U) {
		wrote = write(fd, cursor, size);
		if (wrote < 0 && errno == EINTR)
			continue;
		if (wrote <= 0)
			return EIO;
		cursor += wrote;
		size -= (size_t)wrote;
	}

	/* Written. */
	return 0;
}

/* Tries a call the confinement refuses (a test build's); the process should end in it.  Writes what came of it to the output. */
static void
make_escape(
	int escape,
	int output)
{
	char line[64];
	int result;
	int length;

	/* The call. */
	result = -1;
	if (escape == PREVIEW_ESCAPE_OPEN)
		result = open("/etc/passwd", O_RDONLY | O_CLOEXEC);
	else if (escape == PREVIEW_ESCAPE_SOCKET)
		result = socket(AF_INET, SOCK_STREAM, 0);
	else if (escape == PREVIEW_ESCAPE_FORK)
		result = (int)fork();

	/* Still here: the confinement let it through or refused it without ending the process. */
	length = snprintf(line, sizeof(line), "ESCAPED result=%d errno=%d\n", result, errno);
	if (length > 0)
		(void)make_write_all(output, line, (size_t)length);
	_exit(PREVIEW_NO_SANDBOX);
}
