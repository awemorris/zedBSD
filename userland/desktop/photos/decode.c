/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pictures of the photos (ws157-p003, ws157-p015): a photo's file is
 * decoded and made a size by keiland-preview in its sandbox
 * (preview/client.h; zedBSD's sandbox_spawn), never in Photos' or Phone's
 * own process -- a square thumbnail cut from its middle (cover), or the
 * whole picture shrunk to fit a side (contain) -- and a picture is turned
 * by quarter turns as the user asked.  A video has no picture here (its
 * first bytes tell it, as the library's kind is told).  These run on the
 * thumbnails' thread (thumbs.c), the media picker's thread (Phone) or the
 * window's; they share nothing.
 */

#include "app.h"

#include "userland/desktop/picture/media-kind.h"
#include "userland/desktop/preview/client.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The first bytes read to tell a video. */
#define DECODE_HEAD		16U

static int decode_video(const char *path);

/*
 * Makes a photo's picture of a side: its middle square (cover) or the
 * whole picture no larger than the side (contain).  Returns 0 (the image's
 * pixels are the caller's, kl_image_release), ENOTSUP for a video, EINVAL
 * for a file that is not a picture or is damaged, EFBIG, ENOMEM,
 * ETIMEDOUT for one the sandbox took too long over, or an errno value of
 * the file's.
 */
int
ph_preview(
	const char *path,
	int side,
	int cover,
	struct kl_image *image)
{
	struct preview_request request;
	struct preview_picture picture;
	int error;

	/* A video has no picture to make. */
	memset(image, 0, sizeof(*image));
	error = decode_video(path);
	if (error != 0)
		return error;

	/* Made in the sandbox at the size. */
	memset(&request, 0, sizeof(request));
	request.width = side;
	request.height = side;
	request.cover = cover;
	error = preview_picture(path, &request, &picture);
	if (error != 0)
		return error;

	/* Succeeded: the image takes the pixels. */
	ph_preview_adopt(&picture, image);
	return 0;
}

/* Gives a picture's pixels (opaque, so premultiplied as they are) to an image. */
void
ph_preview_adopt(
	struct preview_picture *picture,
	struct kl_image *image)
{
	/* The image takes the pixels; the picture no longer owns them. */
	image->pixels = picture->pixels;
	image->width = picture->width;
	image->height = picture->height;
	image->stride = (size_t)picture->width;
	picture->pixels = NULL;
}

/*
 * Makes a picture turned clockwise by quarter turns (0 to 3).  Returns 0
 * or ENOMEM; the picture given is the caller's.
 */
int
ph_turn(
	const struct kl_image *picture,
	int turns,
	struct kl_image *turned)
{
	const uint32_t *row;
	uint32_t *out;
	int width;
	int height;
	int x;
	int y;
	int error;

	/* The size: a quarter or three swap the sides. */
	turns &= 3;
	width = picture->width;
	height = picture->height;
	if (turns == 1 || turns == 3) {
		width = picture->height;
		height = picture->width;
	}

	/* The turned picture. */
	error = kl_image_create(turned, width, height);
	if (error != 0)
		return error;

	/* Each source pixel to its place. */
	for (y = 0; y < picture->height; y++) {
		row = picture->pixels + (size_t)y * picture->stride;
		for (x = 0; x < picture->width; x++) {
			switch (turns) {
			case 1:
				out = turned->pixels + (size_t)x * turned->stride + (size_t)(picture->height - 1 - y);
				break;
			case 2:
				out = turned->pixels + (size_t)(picture->height - 1 - y) * turned->stride + (size_t)(picture->width - 1 - x);
				break;
			case 3:
				out = turned->pixels + (size_t)(picture->width - 1 - x) * turned->stride + (size_t)y;
				break;
			default:
				out = turned->pixels + (size_t)y * turned->stride + (size_t)x;
				break;
			}

			/* The pixel. */
			*out = row[x];
		}
	}

	/* Turned. */
	return 0;
}

/* Tells a video by its first bytes: ENOTSUP for one, 0 for any other file, or an errno value of the file's. */
static int
decode_video(
	const char *path)
{
	unsigned char head[DECODE_HEAD];
	ssize_t got;
	int descriptor;
	int error;
	int kind;

	/* The first bytes. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0) {
		error = errno;
		return error;
	}

	/* As many of them as there are. */
	got = pread(descriptor, head, sizeof(head), 0);
	error = errno;
	(void)close(descriptor);
	if (got < 0)
		return error;

	/* Their kind. */
	kind = kl_media_kind(head, (size_t)got);
	if (kind == KL_MEDIA_VIDEO)
		return ENOTSUP;

	/* Succeeded: not a video. */
	return 0;
}
