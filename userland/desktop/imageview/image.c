/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The images of Image Viewer (ws091): a PNG (libpng-compat), a JPEG
 * (libjpeg-compat, turned as its EXIF orientation says) or a GIF
 * (libgif-compat, every frame of an animated one composed) decoded into
 * premultiplied 0xAARRGGBB words.  The kind is told by the file's first
 * bytes, not by its name.
 *
 * An image larger than the viewer keeps (the presenter's largest texture,
 * IV_IMAGE_PIXELS_MAX pixels) is halved until it fits.  Transparent parts
 * are laid over a light checkerboard, so that every pixel the presenter
 * gets is opaque.  A still image then gets its levels of halves, each the
 * box average of the one before, down to a side of IV_LEVEL_SMALLEST, for
 * showing it small without shimmering.
 *
 * The JPEG decoding, the EXIF orientation and the drawing of a GIF's frame
 * are shared with the file manager's thumbnails (userland/desktop/picture,
 * ws094-p013).
 */

#include "imageview.h"

#include "../picture/picture.h"

#include <compat/gif_lib.h>
#include <compat/png/png.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* The smallest level side made (a level whose longer side is this or less is the last). */
#define IV_LEVEL_SMALLEST	256

/* The checkerboard under transparent parts: its square's side and its two colours (opaque 0xRRGGBB). */
#define IMAGE_CHECKER_SIDE	8
#define IMAGE_CHECKER_LIGHT	0xf2f4f7U
#define IMAGE_CHECKER_DARK	0xe3e7ecU

/* The shortest frame an animated GIF is shown for, and what a shorter delay is taken for, in milliseconds (as browsers do). */
#define IMAGE_DELAY_SHORTEST	20U
#define IMAGE_DELAY_DEFAULT	100U

/* The longest side any decoder accepts before the image is refused as too large. */
#define IMAGE_SIDE_MAX		32768U

/*
 * The kind of file an image is, told by its first bytes.
 *
 * The kind picks the decoder; UNREADABLE and OTHER are refused with their
 * own reasons.
 */
enum image_kind {
	IMAGE_KIND_UNREADABLE,
	IMAGE_KIND_OTHER,
	IMAGE_KIND_PNG,
	IMAGE_KIND_JPEG,
	IMAGE_KIND_GIF
};

static enum image_kind image_kind(const char *path, const char **format, int *error);
static int image_png(struct iv_image *image, struct kl_picture *picture);
static void image_jpeg_refuse(struct iv_image *image, int error);
static int image_jpeg(struct iv_image *image, struct kl_picture *picture);
static int image_gif(struct iv_image *image, struct kl_picture *picture);
static int image_gif_frames(struct iv_image *image, GifFileType *gif, uint32_t *screen);
static void image_gif_clear(GifFileType *gif, int index, uint32_t *screen);
static int image_halve(const uint32_t *pixels, int width, int height, uint32_t **result, int *result_width, int *result_height);
static int image_fit(struct iv_image *image, struct kl_picture *picture, int max_dimension);
static void image_checker(uint32_t *pixels, int width, int height);
static int image_levels(struct iv_image *image);
static void image_refuse(struct iv_image *image, int error, const char *reason);

/*
 * Decodes the image in a file, making it no larger than max_dimension on
 * either side (0: no limit on a side) and IV_IMAGE_PIXELS_MAX in all.
 *
 * Returns 0 with the image ready to show, or an errno value with
 * image->error and image->reason saying why (the image is then empty but
 * still has its path, and may be freed as usual).
 */
int
iv_image_load(
	struct iv_image *image,
	const char *path,
	int max_dimension)
{
	struct kl_picture picture;
	enum image_kind kind;
	uint64_t started;
	uint64_t elapsed;
	int error;

	/* Nothing decoded yet; the path is kept even when decoding fails.  The time it takes is logged. */
	started = iv_clock();
	memset(image, 0, sizeof(*image));
	snprintf(image->path, sizeof(image->path), "%s", path);
	memset(&picture, 0, sizeof(picture));

	/* The kind of image, by the file's first bytes. */
	error = 0;
	kind = image_kind(path, &image->format, &error);

	/* Decodes it by its kind. */
	switch (kind) {
	case IMAGE_KIND_PNG:
		error = image_png(image, &picture);
		break;
	case IMAGE_KIND_JPEG:
		error = image_jpeg(image, &picture);
		break;
	case IMAGE_KIND_GIF:
		error = image_gif(image, &picture);
		break;
	case IMAGE_KIND_UNREADABLE:
		image_refuse(image, error, "The file cannot be read");
		error = image->error;
		break;
	default:
		error = EINVAL;
		image_refuse(image, error, "This is not a PNG, JPEG or GIF image");
		break;
	}

	/* A picture that could not be decoded leaves the image empty with its reason. */
	if (error != 0) {
		free(picture.pixels);
		return error;
	}

	/* The picture as the image's level 0, halved when it is larger than the viewer keeps. */
	error = image_fit(image, &picture, max_dimension);
	if (error != 0) {
		free(picture.pixels);
		iv_image_free(image);
		image_refuse(image, error, "There is not enough memory for this image");
		return error;
	}

	/* An animated image is ready as its frames; the rest is for a still image. */
	if (image->frame_count == 0U) {
		/* Transparent parts are laid over the checkerboard once, here. */
		if (image->has_alpha)
			image_checker(image->levels[0].pixels, image->width, image->height);

		/* The levels of halves for showing the image small. */
		error = image_levels(image);
		if (error != 0) {
			iv_image_free(image);
			image_refuse(image, error, "There is not enough memory for this image");
			return error;
		}
	}

	/* Logs the decoded image and the time it took. */
	elapsed = iv_clock() - started;
	iv_log("IMAGE path=%s format=%s width=%d height=%d file=%dx%d levels=%lu frames=%lu ms=%llu",
	       image->path,
	       image->format,
	       image->width,
	       image->height,
	       image->file_width,
	       image->file_height,
	       (unsigned long)image->level_count,
	       (unsigned long)image->frame_count,
	       (unsigned long long)elapsed);

	/* Succeeded: the image can be shown. */
	return 0;
}

/*
 * Frees an image's pixels, levels and frames, and leaves it empty (its
 * path and its reason, if any, stay).
 */
void
iv_image_free(
	struct iv_image *image)
{
	size_t index;
	size_t first;

	/* An animated image's level 0 is its first frame, freed with the frames. */
	first = 0;
	if (image->frame_count != 0U)
		first = 1;

	/* The levels the image owns. */
	for (index = first; index < image->level_count; index++)
		free(image->levels[index].pixels);

	/* The frames. */
	for (index = 0; index < image->frame_count; index++)
		free(image->frames[index]);

	/* The tables of the frames and their delays. */
	free(image->frames);
	free(image->delays);

	/* Nothing is left to show. */
	memset(image->levels, 0, sizeof(image->levels));
	image->level_count = 0;
	image->frames = NULL;
	image->delays = NULL;
	image->frame_count = 0;
	image->width = 0;
	image->height = 0;
}

/*
 * Reads the orientation (1 to 8) of an EXIF block (a JPEG's APP1 data,
 * from its "Exif" header); 1 (as stored) when it has none or cannot be
 * read.
 */
int
iv_image_orientation(
	const unsigned char *data,
	size_t size)
{
	int orientation;

	/* The shared reading (userland/desktop/picture). */
	orientation = kl_picture_exif_orientation(data, size);

	/* Succeeded: the orientation the file gives. */
	return orientation;
}

/*
 * Tells whether a file name is one the viewer shows (by its extension,
 * any case): 1 for .png, .jpg, .jpeg, .jpe and .gif, 0 otherwise.
 */
int
iv_image_is_name(
	const char *name)
{
	static const char *const extensions[] = { ".png", ".jpg", ".jpeg", ".jpe", ".gif" };
	const char *dot;
	size_t index;
	int match;

	/* The last dot starts the extension; a hidden file's leading dot does not. */
	dot = strrchr(name, '.');
	if (dot == NULL || dot == name)
		return 0;

	/* Compares it with each extension shown. */
	for (index = 0; index < sizeof(extensions) / sizeof(extensions[0]); index++) {
		/* The extension, in any case. */
		match = strcasecmp(dot, extensions[index]);
		if (match == 0)
			return 1;
	}

	/* Another kind of file. */
	return 0;
}

/* Tells the kind of image by the file's first bytes; an unreadable file leaves its errno value in *error. */
static enum image_kind
image_kind(
	const char *path,
	const char **format,
	int *error)
{
	static const unsigned char png[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	unsigned char head[8];
	size_t count;
	FILE *file;
	int match;

	/* Opens the file; a file that cannot be opened is unreadable, never a success. */
	*format = "unknown";
	file = fopen(path, "rb");
	if (file == NULL) {
		*error = errno;
		if (*error == 0)
			*error = EIO;
		return IMAGE_KIND_UNREADABLE;
	}

	/* Reads the first eight bytes, which tell every kind shown. */
	memset(head, 0, sizeof(head));
	count = fread(head, 1U, sizeof(head), file);
	fclose(file);

	/* PNG's signature. */
	match = memcmp(head, png, sizeof(png));
	if (count == sizeof(head) && match == 0) {
		*format = "PNG";
		return IMAGE_KIND_PNG;
	}

	/* A JPEG starts with SOI and another marker. */
	if (count >= 3U &&
	    head[0] == 0xff &&
	    head[1] == 0xd8 &&
	    head[2] == 0xff) {
		*format = "JPEG";
		return IMAGE_KIND_JPEG;
	}

	/* A GIF starts GIF87a or GIF89a. */
	match = memcmp(head, "GIF8", 4U);
	if (count >= 6U && match == 0) {
		*format = "GIF";
		return IMAGE_KIND_GIF;
	}

	/* Another kind of file. */
	return IMAGE_KIND_OTHER;
}

/* Decodes a PNG with libpng's simplified API into a picture. */
static int
image_png(
	struct iv_image *image,
	struct kl_picture *picture)
{
	png_image png;
	const char *interlaced;
	unsigned char *bytes;
	size_t count;
	size_t index;
	int status;

	/* The header: the size and the kind. */
	memset(&png, 0, sizeof(png));
	png.version = PNG_IMAGE_VERSION;
	status = png_image_begin_read_from_file(&png, image->path);
	if (status == 0) {
		iv_log("IMAGE png refused path=%s message=%s", image->path, png.message);

		/* libpng's simplified API refuses an interlaced file by name. */
		interlaced = strstr(png.message, "nterlace");
		if (interlaced != NULL) {
			image_refuse(image, ENOTSUP, "Interlaced PNG images are not supported");
			return ENOTSUP;
		}

		/* Any other refusal is a damaged file. */
		image_refuse(image, EINVAL, "The PNG image is damaged");
		return EINVAL;
	}

	/* A size the viewer can hold at all. */
	if (png.width == 0U ||
	    png.height == 0U ||
	    png.width > IMAGE_SIDE_MAX ||
	    png.height > IMAGE_SIDE_MAX) {
		png_image_free(&png);
		image_refuse(image, E2BIG, "The image is too large");
		return E2BIG;
	}

	/* Room for the pixels as 8-bit RGBA. */
	png.format = PNG_FORMAT_RGBA;
	count = (size_t)png.width * (size_t)png.height;
	bytes = malloc(count * 4U);
	if (bytes == NULL) {
		png_image_free(&png);
		image_refuse(image, ENOMEM, "There is not enough memory for this image");
		return ENOMEM;
	}

	/* The pixels. */
	status = png_image_finish_read(&png, NULL, bytes, 0, NULL);
	if (status == 0) {
		iv_log("IMAGE png failed path=%s message=%s", image->path, png.message);
		free(bytes);
		png_image_free(&png);
		image_refuse(image, EINVAL, "The PNG image is damaged");
		return EINVAL;
	}

	/* The picture, premultiplied. */
	picture->pixels = malloc(count * sizeof(uint32_t));
	if (picture->pixels == NULL) {
		free(bytes);
		image_refuse(image, ENOMEM, "There is not enough memory for this image");
		return ENOMEM;
	}

	/* Its size; opaque until a pixel says otherwise. */
	picture->width = (int)png.width;
	picture->height = (int)png.height;
	picture->has_alpha = 0;

	/* Each pixel's straight RGBA into a premultiplied word. */
	for (index = 0; index < count; index++) {
		picture->pixels[index] = kl_picture_premultiply(bytes[index * 4U], bytes[index * 4U + 1U], bytes[index * 4U + 2U], bytes[index * 4U + 3U]);

		/* A pixel that lets anything through makes the picture need the checkerboard. */
		if (bytes[index * 4U + 3U] != 255U)
			picture->has_alpha = 1;
	}

	/* The RGBA rows are no longer needed. */
	free(bytes);

	/* Succeeded: the picture as the file has it. */
	image->file_width = picture->width;
	image->file_height = picture->height;
	return 0;
}

/* Tells why a JPEG could not be decoded: damaged or not supported, too large, or out of memory. */
static void
image_jpeg_refuse(
	struct iv_image *image,
	int error)
{
	/* A damaged file, or a kind not supported. */
	if (error == EINVAL) {
		image_refuse(image, EINVAL, "The JPEG image is damaged or of a kind not supported");
		return;
	}

	/* One too large to hold. */
	if (error == E2BIG) {
		image_refuse(image, E2BIG, "The image is too large");
		return;
	}

	/* Any other failure is memory running out. */
	image_refuse(image, error, "There is not enough memory for this image");

	/* Succeeded: the reason is told. */
	return;
}

/* Decodes a JPEG with libjpeg into a picture, turned as its EXIF orientation says. */
static int
image_jpeg(
	struct iv_image *image,
	struct kl_picture *picture)
{
	int orientation;
	int error;
	FILE *file;

	/* The file. */
	file = fopen(image->path, "rb");
	if (file == NULL) {
		error = errno;
		if (error == 0)
			error = EIO;
		image_refuse(image, error, "The file cannot be read");
		return error;
	}

	/* The picture as stored and its orientation (the shared decoding, userland/desktop/picture). */
	error = kl_picture_jpeg(file, NULL, 0U, IMAGE_SIDE_MAX, 0UL, picture, &orientation);
	if (error != 0) {
		fclose(file);
		image_jpeg_refuse(image, error);
		return error;
	}

	/* The file is not needed any more. */
	fclose(file);

	/* The picture turned upright. */
	image->file_width = picture->width;
	image->file_height = picture->height;
	error = kl_picture_orient(picture, orientation);
	if (error != 0) {
		image_refuse(image, error, "There is not enough memory for this image");
		return error;
	}

	/* The file's size, turned the same way. */
	if (orientation >= 5) {
		image->file_width = picture->width;
		image->file_height = picture->height;
	}

	/* Succeeded: the picture upright. */
	return 0;
}

/* Decodes a GIF into a picture: its first frame, and every frame of an animated one into the image's frames. */
static int
image_gif(
	struct iv_image *image,
	struct kl_picture *picture)
{
	GifFileType *gif;
	uint32_t *screen;
	size_t count;
	int status;
	int error;

	/* The file and its records. */
	error = 0;
	gif = DGifOpenFileName(image->path, &error);
	if (gif == NULL) {
		image_refuse(image, EINVAL, "The GIF image is damaged");
		return EINVAL;
	}

	/* Reads every frame's record at once. */
	status = DGifSlurp(gif);
	if (status != GIF_OK || gif->ImageCount < 1) {
		(void)DGifCloseFile(gif, &error);
		image_refuse(image, EINVAL, "The GIF image is damaged");
		return EINVAL;
	}

	/* A screen of a size the viewer can hold at all. */
	if (gif->SWidth <= 0 ||
	    gif->SHeight <= 0 ||
	    gif->SWidth > (int)IMAGE_SIDE_MAX ||
	    gif->SHeight > (int)IMAGE_SIDE_MAX) {
		(void)DGifCloseFile(gif, &error);
		image_refuse(image, E2BIG, "The image is too large");
		return E2BIG;
	}

	/* The screen the frames are composed on, clear to start with. */
	count = (size_t)gif->SWidth * (size_t)gif->SHeight;
	screen = calloc(count, sizeof(uint32_t));
	if (screen == NULL) {
		(void)DGifCloseFile(gif, &error);
		image_refuse(image, ENOMEM, "There is not enough memory for this image");
		return ENOMEM;
	}

	/* Every frame (an animated GIF), or the first frame only. */
	error = image_gif_frames(image, gif, screen);
	if (error != 0) {
		free(screen);
		(void)DGifCloseFile(gif, &error);
		image_refuse(image, ENOMEM, "There is not enough memory for this image");
		return ENOMEM;
	}

	/* A still GIF's picture is the screen after its one frame; an animated one's is its first frame. */
	picture->width = gif->SWidth;
	picture->height = gif->SHeight;
	picture->has_alpha = 1;

	/* The screen is the still picture, or only the frames' scratch. */
	if (image->frame_count == 0U) {
		picture->pixels = screen;
	} else {
		free(screen);
		picture->pixels = NULL;
	}

	/* The file is done with. */
	(void)DGifCloseFile(gif, &error);

	/* Succeeded: the picture, or the frames. */
	image->file_width = picture->width;
	image->file_height = picture->height;
	return 0;
}

/*
 * Composes a GIF's frames on the screen.  With one frame, or frames too
 * large to keep, only the first is composed (on the screen); otherwise
 * each composed frame is kept, over the checkerboard, with its delay.
 */
static int
image_gif_frames(
	struct iv_image *image,
	GifFileType *gif,
	uint32_t *screen)
{
	GraphicsControlBlock control;
	uint32_t *saved;
	size_t count;
	size_t bytes;
	int index;
	int status;
	int first_only;

	/*
	 * One frame, or frames too many to keep (their total overflowing, or
	 * over the limit), leave the first frame only.
	 */
	count = (size_t)gif->SWidth * (size_t)gif->SHeight;
	bytes = count * sizeof(uint32_t) * (size_t)gif->ImageCount;
	first_only = 0;
	if (gif->ImageCount == 1) {
		first_only = 1;
	} else if (bytes / (size_t)gif->ImageCount / sizeof(uint32_t) != count) {
		first_only = 1;
	} else if (bytes > IV_FRAMES_BYTES_MAX) {
		first_only = 1;
	}

	/* Composes the first frame on the screen, with its transparent colour if it has one. */
	if (first_only) {
		memset(&control, 0, sizeof(control));
		control.TransparentColor = NO_TRANSPARENT_COLOR;
		(void)DGifSavedExtensionToGCB(gif, 0, &control);
		kl_picture_gif_draw(gif, 0, control.TransparentColor, screen);

		/* Succeeded: one frame on the screen. */
		return 0;
	}

	/* The table of the composed frames. */
	image->frames = calloc((size_t)gif->ImageCount, sizeof(image->frames[0]));
	if (image->frames == NULL)
		return ENOMEM;

	/* The table of the frames' delays. */
	image->delays = calloc((size_t)gif->ImageCount, sizeof(image->delays[0]));
	if (image->delays == NULL)
		return ENOMEM;

	/* A copy of the screen for a frame that DISPOSE_PREVIOUS undoes. */
	saved = malloc(count * sizeof(uint32_t));
	if (saved == NULL)
		return ENOMEM;

	/* Each frame: drawn over what the one before left, kept, then disposed of. */
	for (index = 0; index < gif->ImageCount; index++) {
		/* The frame's control block; a frame without one is drawn over and kept for the default time. */
		memset(&control, 0, sizeof(control));
		control.TransparentColor = NO_TRANSPARENT_COLOR;
		status = DGifSavedExtensionToGCB(gif, index, &control);
		if (status != GIF_OK) {
			control.DisposalMode = DISPOSAL_UNSPECIFIED;
			control.DelayTime = 0;
		}

		/* A frame to be undone keeps the screen before it. */
		if (control.DisposalMode == DISPOSE_PREVIOUS)
			memcpy(saved, screen, count * sizeof(uint32_t));

		/* The frame over what the frames before it left. */
		kl_picture_gif_draw(gif, index, control.TransparentColor, screen);

		/* The composed frame over the checkerboard. */
		image->frames[index] = malloc(count * sizeof(uint32_t));
		if (image->frames[index] == NULL) {
			free(saved);
			return ENOMEM;
		}

		/* The composed frame, kept. */
		memcpy(image->frames[index], screen, count * sizeof(uint32_t));
		image_checker(image->frames[index], gif->SWidth, gif->SHeight);
		image->frame_count = (size_t)index + 1U;

		/* Its delay, in milliseconds, no shorter than browsers show one. */
		image->delays[index] = (unsigned)control.DelayTime * 10U;
		if (image->delays[index] < IMAGE_DELAY_SHORTEST)
			image->delays[index] = IMAGE_DELAY_DEFAULT;

		/* What the frame leaves for the next one. */
		if (control.DisposalMode == DISPOSE_BACKGROUND)
			image_gif_clear(gif, index, screen);
		else if (control.DisposalMode == DISPOSE_PREVIOUS)
			memcpy(screen, saved, count * sizeof(uint32_t));
	}

	/* The saved screen is no longer needed. */
	free(saved);

	/* Succeeded: every frame composed. */
	return 0;
}

/* Clears a frame's rectangle of the screen to transparent (DISPOSE_BACKGROUND, as browsers treat it). */
static void
image_gif_clear(
	GifFileType *gif,
	int index,
	uint32_t *screen)
{
	const GifImageDesc *frame;
	int x;
	int y;

	/* Each pixel of the frame's rectangle on the screen. */
	frame = &gif->SavedImages[index].ImageDesc;
	for (y = frame->Top; y < frame->Top + frame->Height; y++) {
		if (y < 0 || y >= gif->SHeight)
			continue;

		/* The row, within the screen. */
		for (x = frame->Left; x < frame->Left + frame->Width; x++) {
			/* A part of the rectangle off the screen has nothing to clear. */
			if (x >= 0 && x < gif->SWidth)
				screen[(size_t)y * (size_t)gif->SWidth + (size_t)x] = 0U;
		}
	}
}

/* Makes a picture of half the size (rounded up), each pixel the average of the two by two it covers. */
static int
image_halve(
	const uint32_t *pixels,
	int width,
	int height,
	uint32_t **result,
	int *result_width,
	int *result_height)
{
	uint32_t *half;
	uint32_t sum[4];
	uint32_t pixel;
	int half_width;
	int half_height;
	int source_x;
	int source_y;
	int x;
	int y;
	int dx;
	int dy;
	int channel;

	/* The half size, at least one pixel. */
	half_width = (width + 1) / 2;
	half_height = (height + 1) / 2;

	/* Room for the half. */
	half = malloc((size_t)half_width * (size_t)half_height * sizeof(uint32_t));
	if (half == NULL)
		return ENOMEM;

	/* Each pixel of the half, from the four it covers (an odd edge repeats its last row or column). */
	for (y = 0; y < half_height; y++) {
		for (x = 0; x < half_width; x++) {
			memset(sum, 0, sizeof(sum));

			/* The two by two, clamped to the picture. */
			for (dy = 0; dy < 2; dy++) {
				source_y = y * 2 + dy;
				if (source_y >= height)
					source_y = height - 1;

				/* The two of the row. */
				for (dx = 0; dx < 2; dx++) {
					source_x = x * 2 + dx;
					if (source_x >= width)
						source_x = width - 1;

					/* The covered pixel. */
					pixel = pixels[(size_t)source_y * (size_t)width + (size_t)source_x];

					/* Each channel of the pixel into its sum. */
					for (channel = 0; channel < 4; channel++)
						sum[channel] += (pixel >> (channel * 8)) & 0xffU;
				}
			}

			/* The average of each channel, rounded. */
			pixel = 0;
			for (channel = 0; channel < 4; channel++)
				pixel |= ((sum[channel] + 2U) / 4U) << (channel * 8);
			half[(size_t)y * (size_t)half_width + (size_t)x] = pixel;
		}
	}

	/* Succeeded: the half. */
	*result = half;
	*result_width = half_width;
	*result_height = half_height;
	return 0;
}

/*
 * Makes a decoded picture the image's level 0 (or its frames the image's
 * frames), halving it while it is larger than max_dimension on a side or
 * IV_IMAGE_PIXELS_MAX in all.
 */
static int
image_fit(
	struct iv_image *image,
	struct kl_picture *picture,
	int max_dimension)
{
	uint32_t *half;
	size_t index;
	int width;
	int height;
	int too_large;
	int error;

	/* The picture as it came. */
	width = picture->width;
	height = picture->height;

	/* Halves it while it is too large (an animated image's frames each). */
	for (;;) {
		/* Too many pixels in all, or a side longer than the caller allows. */
		too_large = 0;
		if ((size_t)width * (size_t)height > IV_IMAGE_PIXELS_MAX) {
			too_large = 1;
		} else if (max_dimension > 0 && width > max_dimension) {
			too_large = 1;
		} else if (max_dimension > 0 && height > max_dimension) {
			too_large = 1;
		}

		/* A picture that fits, or a single pixel, is not halved again. */
		if (!too_large)
			break;
		if (width == 1 && height == 1)
			break;

		/* An animated image's frames each, or the still picture. */
		if (image->frame_count != 0U) {
			for (index = 0; index < image->frame_count; index++) {
				/* The frame's half. */
				error = image_halve(image->frames[index], width, height, &half, &picture->width, &picture->height);
				if (error != 0)
					return error;

				/* The half replaces the frame. */
				free(image->frames[index]);
				image->frames[index] = half;
			}
		} else {
			/* The picture's half. */
			error = image_halve(picture->pixels, width, height, &half, &picture->width, &picture->height);
			if (error != 0)
				return error;

			/* The half replaces the picture. */
			free(picture->pixels);
			picture->pixels = half;
		}

		/* The next round halves the halved picture if it is still too large. */
		width = picture->width;
		height = picture->height;
		iv_log("IMAGE halved path=%s width=%d height=%d", image->path, width, height);
	}

	/* Level 0: the picture, or the first frame. */
	image->width = width;
	image->height = height;
	image->has_alpha = picture->has_alpha;
	image->levels[0].width = width;
	image->levels[0].height = height;
	image->level_count = 1;

	/* An animated image's level 0 is its first frame; a still one's is the picture, now the image's. */
	if (image->frame_count != 0U) {
		image->levels[0].pixels = image->frames[0];
	} else {
		image->levels[0].pixels = picture->pixels;
		picture->pixels = NULL;
	}

	/* Succeeded: level 0 is the image. */
	return 0;
}

/* Lays a picture's transparent parts over the checkerboard (premultiplied source over opaque squares). */
static void
image_checker(
	uint32_t *pixels,
	int width,
	int height)
{
	uint32_t square;
	uint32_t pixel;
	uint32_t alpha;
	uint32_t out;
	int channel;
	int x;
	int y;

	/* Each pixel that is not opaque. */
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			pixel = pixels[(size_t)y * (size_t)width + (size_t)x];
			alpha = pixel >> 24;
			if (alpha == 255U)
				continue;

			/* The square under it: light and dark alternate along both axes. */
			if (((x / IMAGE_CHECKER_SIDE) + (y / IMAGE_CHECKER_SIDE)) % 2 != 0) {
				square = IMAGE_CHECKER_DARK;
			} else {
				square = IMAGE_CHECKER_LIGHT;
			}

			/* Each colour: the pixel's plus what it lets through of the square. */
			out = 0xff000000U;
			for (channel = 0; channel < 3; channel++) {
				out |= (((pixel >> (channel * 8)) & 0xffU) +
				    (((square >> (channel * 8)) & 0xffU) * (255U - alpha) + 127U) / 255U) << (channel * 8);
			}

			/* The pixel over its square, opaque. */
			pixels[(size_t)y * (size_t)width + (size_t)x] = out;
		}
	}
}

/* Makes a still image's levels of halves after level 0, down to IV_LEVEL_SMALLEST. */
static int
image_levels(
	struct iv_image *image)
{
	struct iv_level *level;
	struct iv_level *half;
	int error;

	/* Halves the last level while it is larger than the smallest kept. */
	while (image->level_count < IV_LEVELS_MAX) {
		level = &image->levels[image->level_count - 1U];
		if (level->width <= IV_LEVEL_SMALLEST && level->height <= IV_LEVEL_SMALLEST)
			break;

		/* The next level, the last one's half. */
		half = &image->levels[image->level_count];
		error = image_halve(level->pixels, level->width, level->height, &half->pixels, &half->width, &half->height);
		if (error != 0)
			return error;

		/* The level counts only once its pixels exist, so that freeing the image frees it. */
		image->level_count++;
	}

	/* Succeeded: every level made. */
	return 0;
}

/* Records why an image is empty: an errno value (never 0) and the reason in the viewer's words. */
static void
image_refuse(
	struct iv_image *image,
	int error,
	const char *reason)
{
	/* A failure is never 0. */
	if (error == 0)
		error = EIO;

	/* The error and its reason, for the card that says the image cannot be shown. */
	image->error = error;
	snprintf(image->reason, sizeof(image->reason), "%s", reason);
	iv_log("IMAGE refused path=%s error=%d reason=%s", image->path, error, reason);
}
