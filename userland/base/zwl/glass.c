/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The glass look of window mode (ws035-p059, a proof of the look and feel):
 * its images and the shapes it draws.  The windows, title bars and system
 * bar that use them are in shell.c.
 *
 * The wallpaper is a picture given with --wallpaper, or a pale blue
 * landscape the CPU draws once.  A quarter-size, blurred copy of it is the
 * frosted glass: a glass panel samples it at its own place on the output and
 * whitens it (like Windows' Mica, only the wallpaper shows through; windows
 * under a panel do not).
 *
 * Text is drawn from a glyph atlas made with libtruetype from the font at
 * server->font_path (printable ASCII and the multiplication sign); without a
 * font the look is drawn without text.
 */

#include "glass.h"

#include <truetype.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The atlas holds printable ASCII and the multiplication sign (the close button) at three sizes. */
#define GLASS_GLYPHS		96U
#define GLASS_SIZES		3U
#define GLASS_ATLAS_WIDTH	1024U
#define GLASS_ATLAS_HEIGHT	128U
#define GLASS_FILE_MAX		(16U * 1024U * 1024U)

/* The blurred wallpaper is this many times smaller than the output. */
#define GLASS_BLUR_SCALE	4U
#define GLASS_BLUR_RADIUS	6U
#define GLASS_BLUR_PASSES	3U

/* One glyph's place in the atlas and its metrics in pixels. */
struct glass_glyph {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	int32_t left;
	int32_t top;
	int32_t advance;
};

/* The look's images and glyphs. */
struct zwl_glass {
	struct zwl_import wallpaper;
	struct zwl_import blurred;
	struct zwl_import atlas;
	struct glass_glyph glyphs[GLASS_SIZES][GLASS_GLYPHS];
	unsigned text;
};

static const unsigned glass_pixels[GLASS_SIZES] = { 14U, 15U, 20U };

static int wallpaper_create(struct zwl_server *server, struct zwl_glass *glass);
static void wallpaper_pixel(uint32_t x, uint32_t y, uint32_t width, uint32_t height, float *rgb);
static float ridge(float x, float base, float amplitude, float phase);
static int blur_create(struct zwl_server *server, struct zwl_glass *glass, const float *pixels);
static void blur_pass(float *pixels, float *scratch, uint32_t width, uint32_t height, int horizontal);
static uint32_t pack_pixel(const float *rgb);
static int atlas_create(struct zwl_server *server, struct zwl_glass *glass);
static int atlas_fill(struct zwl_glass *glass, struct truetype_face *face);
static void *file_read(const char *path, size_t *size);
static float *wallpaper_load(const char *path, uint32_t width, uint32_t height);
static int ppm_number(const unsigned char *data, size_t size, size_t *at, uint32_t *number);

/*
 * Makes the wallpaper, its blurred copy and the glyph atlas.
 */
int
zwl_glass_open(
	struct zwl_server *server)
{
	struct zwl_glass *glass;
	int error;

	/* The look's state lives as long as window mode's device. */
	glass = calloc(1, sizeof(*glass));
	if (glass == NULL)
		return ENOMEM;
	server->compose->glass = glass;

	/* The wallpaper and the frosted glass made from it. */
	error = wallpaper_create(server, glass);
	if (error != 0)
		return error;

	/* The glyphs; the look is drawn without text when the font cannot be read. */
	error = atlas_create(server, glass);
	if (error != 0)
		printf("ZWL GLASS no text: font=%s errno=%d\n", server->font_path, error);

	/* Succeeded. */
	printf("ZWL GLASS ready text=%u\n", glass->text);
	return 0;
}

/*
 * Releases the look's images.
 */
void
zwl_glass_close(
	struct zwl_server *server)
{
	struct zwl_glass *glass;

	/* Nothing was made. */
	if (server->compose == NULL || server->compose->glass == NULL)
		return;

	/* The images, then the record. */
	glass = server->compose->glass;
	zwl_host_image_release(server->compose, &glass->wallpaper);
	zwl_host_image_release(server->compose, &glass->blurred);
	zwl_host_image_release(server->compose, &glass->atlas);
	free(glass);
	server->compose->glass = NULL;
}

/* Draws the wallpaper once, and its blurred copy. */
static int
wallpaper_create(
	struct zwl_server *server,
	struct zwl_glass *glass)
{
	uint32_t *row;
	float *pixels;
	uint32_t width;
	uint32_t height;
	uint32_t x;
	uint32_t y;
	VkResult result;
	int error;

	/* The output-sized image. */
	width = server->width;
	height = server->height;
	result = zwl_host_image_create(server->compose, width, height, server->compose->sampler, &glass->wallpaper);
	if (result != VK_SUCCESS)
		return EIO;

	/* The picture given, in floating point and kept for the blur. */
	pixels = NULL;
	if (server->wallpaper_path != NULL) {
		pixels = wallpaper_load(server->wallpaper_path, width, height);
		if (pixels == NULL)
			printf("ZWL GLASS no wallpaper: path=%s errno=%d\n", server->wallpaper_path, errno);
	}

	/* Otherwise the landscape drawn here. */
	if (pixels == NULL) {
		pixels = malloc((size_t)width * height * 3U * sizeof(float));
		if (pixels == NULL)
			return ENOMEM;
		for (y = 0; y < height; y++) {
			for (x = 0; x < width; x++)
				wallpaper_pixel(x, y, width, height, &pixels[((size_t)y * width + x) * 3U]);
		}
	}

	/* Into the image. */
	for (y = 0; y < height; y++) {
		row = (uint32_t *)((unsigned char *)glass->wallpaper.map + y * glass->wallpaper.row_pitch);
		for (x = 0; x < width; x++)
			row[x] = pack_pixel(&pixels[((size_t)y * width + x) * 3U]);
	}

	/* The frosted glass. */
	error = blur_create(server, glass, pixels);
	free(pixels);
	return error;
}

/*
 * Colors one pixel of the landscape: a sky that pales towards the horizon
 * with a soft sun, three ranges of misty mountains, and a still lake that
 * reflects them.
 */
static void
wallpaper_pixel(
	uint32_t x,
	uint32_t y,
	uint32_t width,
	uint32_t height,
	float *rgb)
{
	static const float ranges[3][5] = {
		/* base, amplitude, phase, and the color's green and blue (red is below). */
		{ 0.50f, 0.10f, 0.3f, 0.82f, 0.94f },
		{ 0.58f, 0.09f, 2.1f, 0.74f, 0.90f },
		{ 0.66f, 0.07f, 4.7f, 0.64f, 0.85f }
	};
	static const float reds[3] = { 0.74f, 0.62f, 0.50f };
	float u;
	float v;
	float mirror;
	float top;
	float mist;
	float sun;
	float lake;
	unsigned index;

	/* The place, as fractions of the output. */
	u = (float)x / (float)width;
	v = (float)y / (float)height;
	lake = 0.80f;

	/* Under the lake's edge the landscape is seen in the water. */
	mirror = v;
	if (v > lake)
		mirror = 2.0f * lake - v;

	/* The sky: pale blue above, nearly white at the horizon, a glow to the upper right. */
	rgb[0] = 0.78f + 0.18f * mirror;
	rgb[1] = 0.86f + 0.11f * mirror;
	rgb[2] = 0.97f + 0.02f * mirror;
	sun = expf(-((u - 0.78f) * (u - 0.78f) * 18.0f + (mirror - 0.18f) * (mirror - 0.18f) * 30.0f));
	rgb[0] += 0.12f * sun;
	rgb[1] += 0.09f * sun;
	rgb[2] += 0.03f * sun;

	/* Each range in front of the ones behind it, misty towards its foot. */
	for (index = 0; index < 3U; index++) {
		top = ridge(u, ranges[index][0], ranges[index][1], ranges[index][2]);
		if (mirror < top)
			continue;
		mist = (mirror - top) / 0.18f;
		if (mist > 1.0f)
			mist = 1.0f;
		rgb[0] = reds[index] + (0.92f - reds[index]) * mist * 0.55f;
		rgb[1] = ranges[index][3] + (0.95f - ranges[index][3]) * mist * 0.55f;
		rgb[2] = ranges[index][4] + (0.99f - ranges[index][4]) * mist * 0.55f;
	}

	/* The water is darker and bluer than what it reflects, deeper towards the bottom. */
	if (v > lake) {
		rgb[0] = rgb[0] * 0.88f - (v - lake) * 0.35f;
		rgb[1] = rgb[1] * 0.92f - (v - lake) * 0.25f;
		rgb[2] = rgb[2] * 0.98f - (v - lake) * 0.08f;
		rgb[0] += 0.006f * sinf((float)y * 0.9f + u * 3.0f);
		rgb[1] += 0.006f * sinf((float)y * 0.9f + u * 3.0f);
	}

	/* Each channel within 0..1. */
	for (index = 0; index < 3U; index++) {
		if (rgb[index] > 1.0f)
			rgb[index] = 1.0f;
		if (rgb[index] < 0.0f)
			rgb[index] = 0.0f;
	}
}

/* The height (as a fraction of the output) of a mountain range's ridge at x. */
static float
ridge(
	float x,
	float base,
	float amplitude,
	float phase)
{
	float wave;
	float peak;

	/* A few waves of falling length make saddles, and a sharp term the peaks. */
	wave = 0.55f * sinf(x * 5.1f + phase);
	wave += 0.30f * sinf(x * 11.3f + phase * 1.7f);
	wave += 0.15f * sinf(x * 23.9f + phase * 2.3f);
	peak = 1.0f - fabsf(sinf(x * 4.3f + phase * 0.7f));
	wave += 0.60f * peak * peak * peak;

	/* Higher waves are lower on the output. */
	return base - amplitude * wave;
}

/*
 * Makes the frosted glass: the wallpaper averaged down by GLASS_BLUR_SCALE
 * and blurred by repeated box passes (close to a Gaussian).
 */
static int
blur_create(
	struct zwl_server *server,
	struct zwl_glass *glass,
	const float *pixels)
{
	uint32_t *row;
	float *small;
	float *scratch;
	uint32_t width;
	uint32_t height;
	uint32_t x;
	uint32_t y;
	uint32_t dx;
	uint32_t dy;
	uint32_t channel;
	unsigned pass;
	size_t at;
	VkResult result;

	/* The small image, sampled linearly. */
	width = server->width / GLASS_BLUR_SCALE;
	height = server->height / GLASS_BLUR_SCALE;
	result = zwl_host_image_create(server->compose, width, height, server->compose->linear_sampler, &glass->blurred);
	if (result != VK_SUCCESS)
		return EIO;

	/* Its working copies. */
	small = calloc((size_t)width * height * 3U, sizeof(float));
	scratch = calloc((size_t)width * height * 3U, sizeof(float));
	if (small == NULL || scratch == NULL) {
		free(small);
		free(scratch);
		return ENOMEM;
	}

	/* Each small pixel is the average of the block it covers. */
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			for (dy = 0; dy < GLASS_BLUR_SCALE; dy++) {
				for (dx = 0; dx < GLASS_BLUR_SCALE; dx++) {
					at = ((size_t)(y * GLASS_BLUR_SCALE + dy) * server->width + x * GLASS_BLUR_SCALE + dx) * 3U;
					for (channel = 0; channel < 3U; channel++)
						small[((size_t)y * width + x) * 3U + channel] += pixels[at + channel];
				}
			}

			/* The sum becomes the average. */
			for (channel = 0; channel < 3U; channel++)
				small[((size_t)y * width + x) * 3U + channel] /= (float)(GLASS_BLUR_SCALE * GLASS_BLUR_SCALE);
		}
	}

	/* Box passes across and down. */
	for (pass = 0; pass < GLASS_BLUR_PASSES; pass++) {
		blur_pass(small, scratch, width, height, 1);
		blur_pass(small, scratch, width, height, 0);
	}

	/* Into the image. */
	for (y = 0; y < height; y++) {
		row = (uint32_t *)((unsigned char *)glass->blurred.map + y * glass->blurred.row_pitch);
		for (x = 0; x < width; x++)
			row[x] = pack_pixel(&small[((size_t)y * width + x) * 3U]);
	}

	/* Succeeded. */
	free(small);
	free(scratch);
	return 0;
}

/* Averages each pixel with its GLASS_BLUR_RADIUS neighbours on each side, across or down (clamped at the edges). */
static void
blur_pass(
	float *pixels,
	float *scratch,
	uint32_t width,
	uint32_t height,
	int horizontal)
{
	uint32_t x;
	uint32_t y;
	uint32_t channel;
	int32_t offset;
	int32_t sx;
	int32_t sy;
	float sum;

	/* Each output pixel from the input. */
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			for (channel = 0; channel < 3U; channel++) {
				sum = 0.0f;
				for (offset = -(int32_t)GLASS_BLUR_RADIUS; offset <= (int32_t)GLASS_BLUR_RADIUS; offset++) {
					/* The neighbour, clamped to the image. */
					sx = (int32_t)x;
					sy = (int32_t)y;
					if (horizontal)
						sx += offset;
					else
						sy += offset;
					if (sx < 0)
						sx = 0;
					if (sy < 0)
						sy = 0;
					if (sx >= (int32_t)width)
						sx = (int32_t)width - 1;
					if (sy >= (int32_t)height)
						sy = (int32_t)height - 1;
					sum += pixels[((size_t)sy * width + (size_t)sx) * 3U + channel];
				}

				/* The average of the window. */
				scratch[((size_t)y * width + x) * 3U + channel] = sum / (float)(2U * GLASS_BLUR_RADIUS + 1U);
			}
		}
	}

	/* The result replaces the input. */
	memcpy(pixels, scratch, (size_t)width * height * 3U * sizeof(float));
}

/* Packs a color (0..1 each) as an opaque BGRA pixel. */
static uint32_t
pack_pixel(
	const float *rgb)
{
	uint32_t red;
	uint32_t green;
	uint32_t blue;

	/* Each channel rounded to eight bits. */
	red = (uint32_t)(rgb[0] * 255.0f + 0.5f);
	green = (uint32_t)(rgb[1] * 255.0f + 0.5f);
	blue = (uint32_t)(rgb[2] * 255.0f + 0.5f);

	/* A, R, G, B from the top byte (B, G, R, A in memory). */
	return 0xff000000U | (red << 16) | (green << 8) | blue;
}

/*
 * Makes the glyph atlas from the font.  Returns 0 with text available, or an
 * error with the look to be drawn without text.
 */
static int
atlas_create(
	struct zwl_server *server,
	struct zwl_glass *glass)
{
	struct truetype_face *face;
	void *data;
	size_t size;
	VkResult result;
	int error;

	/* The font file. */
	data = file_read(server->font_path, &size);
	if (data == NULL)
		return errno;
	error = truetype_open(data, size, 0U, &face);
	if (error != 0) {
		free(data);
		return EINVAL;
	}

	/* The atlas image, transparent where nothing is drawn. */
	result = zwl_host_image_create(server->compose, GLASS_ATLAS_WIDTH, GLASS_ATLAS_HEIGHT, server->compose->sampler, &glass->atlas);
	if (result != VK_SUCCESS) {
		truetype_close(face);
		free(data);
		return EIO;
	}

	/* The glyphs at each size. */
	error = atlas_fill(glass, face);
	truetype_close(face);
	free(data);
	if (error != 0)
		return error;

	/* Succeeded. */
	glass->text = 1;
	return 0;
}

/* Renders every glyph at every size into the atlas, row by row. */
static int
atlas_fill(
	struct zwl_glass *glass,
	struct truetype_face *face)
{
	static uint8_t bitmap[64U * 64U];
	struct truetype_glyph metrics;
	struct glass_glyph *glyph;
	uint32_t *row;
	uint32_t codepoint;
	uint32_t pen_x;
	uint32_t pen_y;
	uint32_t line;
	uint32_t x;
	uint32_t y;
	uint32_t value;
	unsigned size;
	unsigned index;
	unsigned id;
	int error;

	/* The pen starts at the top left; each row is as tall as its tallest glyph. */
	memset(glass->atlas.map, 0, glass->atlas.row_pitch * GLASS_ATLAS_HEIGHT);
	pen_x = 0;
	pen_y = 0;
	line = 0;
	for (size = 0; size < GLASS_SIZES; size++) {
		error = truetype_set_pixel_size(face, glass_pixels[size]);
		if (error != 0)
			return EINVAL;

		/* Each glyph of this size. */
		for (index = 0; index < GLASS_GLYPHS; index++) {
			codepoint = 32U + index;
			if (index == GLASS_CLOSE_GLYPH)
				codepoint = 0xd7U;
			id = truetype_glyph_index(face, codepoint);
			error = truetype_glyph_metrics(face, id, &metrics);
			if (error != 0 || metrics.width > 64U || metrics.height > 64U)
				continue;

			/* A full row moves the pen down. */
			if (pen_x + metrics.width + 1U > GLASS_ATLAS_WIDTH) {
				pen_x = 0;
				pen_y += line + 1U;
				line = 0;
			}

			/* The atlas must hold it. */
			if (pen_y + metrics.height > GLASS_ATLAS_HEIGHT)
				return ENOSPC;

			/* The glyph's place and metrics. */
			glyph = &glass->glyphs[size][index];
			glyph->x = pen_x;
			glyph->y = pen_y;
			glyph->width = metrics.width;
			glyph->height = metrics.height;
			glyph->left = metrics.left;
			glyph->top = metrics.top;
			glyph->advance = metrics.advance;

			/* Its coverage as premultiplied white. */
			if (metrics.width != 0U && metrics.height != 0U) {
				memset(bitmap, 0, sizeof(bitmap));
				error = truetype_render_glyph(face, id, &metrics, bitmap, metrics.width, sizeof(bitmap));
				if (error != 0)
					continue;
				for (y = 0; y < metrics.height; y++) {
					row = (uint32_t *)((unsigned char *)glass->atlas.map + (pen_y + y) * glass->atlas.row_pitch);
					for (x = 0; x < metrics.width; x++) {
						value = bitmap[y * metrics.width + x];
						row[pen_x + x] = (value << 24) | (value << 16) | (value << 8) | value;
					}
				}
			}

			/* The pen moves past it. */
			pen_x += metrics.width + 1U;
			if (metrics.height > line)
				line = metrics.height;
		}
	}

	/* Succeeded. */
	return 0;
}

/* Reads a whole file of up to GLASS_FILE_MAX bytes; NULL with errno set when it cannot. */
static void *
file_read(
	const char *path,
	size_t *size)
{
	unsigned char *data;
	ssize_t count;
	size_t length;
	int descriptor;

	/* The file. */
	descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (descriptor < 0)
		return NULL;

	/* Its bytes, up to GLASS_FILE_MAX. */
	data = malloc(GLASS_FILE_MAX);
	if (data == NULL) {
		close(descriptor);
		errno = ENOMEM;
		return NULL;
	}

	/* Read to the end or the bound. */
	length = 0;
	for (;;) {
		count = read(descriptor, data + length, GLASS_FILE_MAX - length);
		if (count <= 0)
			break;
		length += (size_t)count;
		if (length == GLASS_FILE_MAX)
			break;
	}

	/* The file is no longer needed. */
	close(descriptor);

	/* An empty or unreadable file has nothing to use. */
	if (length == 0) {
		free(data);
		errno = EINVAL;
		return NULL;
	}

	/* Succeeded. */
	*size = length;
	return data;
}

/*
 * Reads a binary PPM (P6, maximum 255) as the wallpaper, scaled to the
 * output by the nearest pixel.  Returns the colors (0..1, three per pixel),
 * or NULL with errno set.
 */
static float *
wallpaper_load(
	const char *path,
	uint32_t width,
	uint32_t height)
{
	const unsigned char *pixel;
	unsigned char *data;
	float *pixels;
	uint32_t source_width;
	uint32_t source_height;
	uint32_t maximum;
	uint32_t x;
	uint32_t y;
	size_t size;
	size_t at;
	int error;

	/* The file, with the P6 magic. */
	data = file_read(path, &size);
	if (data == NULL)
		return NULL;
	if (size < 2U || data[0] != 'P' || data[1] != '6') {
		free(data);
		errno = EINVAL;
		return NULL;
	}

	/* The width, the height and the maximum value. */
	at = 2U;
	error = ppm_number(data, size, &at, &source_width);
	if (error == 0)
		error = ppm_number(data, size, &at, &source_height);
	if (error == 0)
		error = ppm_number(data, size, &at, &maximum);
	if (error != 0 || maximum != 255U || source_width == 0U || source_height == 0U) {
		free(data);
		errno = EINVAL;
		return NULL;
	}

	/* One whitespace byte, then three bytes a pixel. */
	at++;
	if (at > size || (size - at) / 3U / source_width < source_height) {
		free(data);
		errno = EINVAL;
		return NULL;
	}

	/* The output's pixels, each from the nearest source pixel. */
	pixels = malloc((size_t)width * height * 3U * sizeof(float));
	if (pixels == NULL) {
		free(data);
		errno = ENOMEM;
		return NULL;
	}

	/* Each output pixel takes the source pixel it falls on. */
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			pixel = data + at + ((size_t)(y * source_height / height) * source_width + (size_t)(x * source_width / width)) * 3U;
			pixels[((size_t)y * width + x) * 3U] = (float)pixel[0] / 255.0f;
			pixels[((size_t)y * width + x) * 3U + 1U] = (float)pixel[1] / 255.0f;
			pixels[((size_t)y * width + x) * 3U + 2U] = (float)pixel[2] / 255.0f;
		}
	}

	/* Succeeded. */
	free(data);
	return pixels;
}

/* Reads one decimal number of a PPM header, after whitespace and comments. */
static int
ppm_number(
	const unsigned char *data,
	size_t size,
	size_t *at,
	uint32_t *number)
{
	uint32_t value;
	unsigned digits;

	/* Whitespace, and comments to the end of their line. */
	while (*at < size) {
		if (data[*at] == '#') {
			while (*at < size && data[*at] != '\n')
				(*at)++;
			continue;
		}

		/* The first byte that is not whitespace starts the number. */
		if (data[*at] != ' ' && data[*at] != '\t' && data[*at] != '\r' && data[*at] != '\n')
			break;
		(*at)++;
	}

	/* The digits, within a bound. */
	value = 0;
	digits = 0;
	while (*at < size && data[*at] >= '0' && data[*at] <= '9' && digits < 6U) {
		value = value * 10U + (uint32_t)(data[*at] - '0');
		(*at)++;
		digits++;
	}

	/* A number has at least one digit. */
	if (digits == 0U)
		return EINVAL;

	/* Succeeded. */
	*number = value;
	return 0;
}

/* Starts a shape over a box: the quad is the box, with no image and no color. */
void
glass_shape_init(
	struct glass_shape *shape,
	float x,
	float y,
	float width,
	float height)
{
	/* The quad and the box are the same until the caller widens the quad. */
	memset(shape, 0, sizeof(*shape));
	shape->quad[0] = x;
	shape->quad[1] = y;
	shape->quad[2] = width;
	shape->quad[3] = height;
	shape->box[0] = x;
	shape->box[1] = y;
	shape->box[2] = width;
	shape->box[3] = height;

	/* The whole image, not faded. */
	shape->uv[2] = 1.0f;
	shape->uv[3] = 1.0f;
	shape->opacity = 1.0f;
}

/* Records one shape: its constants for both shader stages and the strip. */
void
glass_shape_draw(
	struct zwl_server *server,
	VkCommandBuffer command,
	const struct glass_shape *shape)
{
	struct zwl_compose *compose;
	float constants[ZWL_PANEL_CONSTANTS];
	float width;
	float height;
	VkDescriptorSet set;

	/* The quad in normalized device coordinates. */
	compose = server->compose;
	width = (float)server->width;
	height = (float)server->height;
	constants[0] = 2.0f * shape->quad[0] / width - 1.0f;
	constants[1] = 2.0f * shape->quad[1] / height - 1.0f;
	constants[2] = 2.0f * (shape->quad[0] + shape->quad[2]) / width - 1.0f;
	constants[3] = 2.0f * (shape->quad[1] + shape->quad[3]) / height - 1.0f;

	/* The part of the image, the box, the color. */
	memcpy(&constants[4], shape->uv, sizeof(shape->uv));
	memcpy(&constants[8], shape->box, sizeof(shape->box));
	memcpy(&constants[12], shape->color, sizeof(shape->color));

	/* The shape and the output. */
	constants[16] = shape->radius;
	constants[17] = shape->mode;
	constants[18] = shape->soft;
	constants[19] = shape->opaque;
	constants[20] = width;
	constants[21] = height;
	constants[22] = shape->edge;
	constants[23] = shape->opacity;

	/* A shape without an image of its own is given the blurred wallpaper (it is not sampled). */
	set = shape->set;
	if (set == VK_NULL_HANDLE)
		set = compose->glass->blurred.set;

	/* The pipeline, the image and the strip. */
	vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, compose->panel_pipeline);
	vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, compose->panel_layout, 0U, 1U, &set, 0U, NULL);
	vkCmdPushConstants(command, compose->panel_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0U, sizeof(constants), constants);
	vkCmdDraw(command, 4U, 1U, 0U, 0U);
}

/* Draws a rounded rectangle in a solid color. */
void
glass_draw_solid(
	struct zwl_server *server,
	VkCommandBuffer command,
	float x,
	float y,
	float width,
	float height,
	float radius,
	const float *color)
{
	struct glass_shape shape;

	/* One solid shape. */
	glass_shape_init(&shape, x, y, width, height);
	shape.mode = MODE_SOLID;
	shape.radius = radius;
	memcpy(shape.color, color, sizeof(shape.color));
	glass_shape_draw(server, command, &shape);
}

/* The width of a line of text in pixels (0 without text). */
int32_t
glass_text_width(
	struct zwl_server *server,
	enum glass_size size,
	const char *text)
{
	struct zwl_glass *glass;
	int32_t width;
	unsigned index;

	/* The sum of the advances of the glyphs the atlas has. */
	glass = server->compose->glass;
	width = 0;
	if (!glass->text)
		return 0;
	for (; *text != '\0'; text++) {
		index = (unsigned char)*text - 32U;
		if (index < GLASS_CLOSE_GLYPH)
			width += glass->glyphs[size][index].advance;
	}

	/* Succeeded. */
	return width;
}

/*
 * Draws a line of text from x on a baseline, cut short (with an ellipsis of
 * dots) where it would pass x + limit.
 */
void
glass_draw_text(
	struct zwl_server *server,
	VkCommandBuffer command,
	enum glass_size size,
	int32_t x,
	int32_t baseline,
	const char *text,
	int32_t limit,
	const float *color)
{
	struct zwl_glass *glass;
	int32_t start;
	int32_t width;
	int32_t dots;
	unsigned index;

	/* Nothing without glyphs. */
	glass = server->compose->glass;
	if (!glass->text)
		return;

	/* Room is kept for the ellipsis when the text is too long. */
	dots = 3 * glass->glyphs[size]['.' - 32].advance;
	width = glass_text_width(server, size, text);
	if (width <= limit)
		dots = 0;

	/* Each glyph the atlas has, while there is room. */
	start = x;
	for (; *text != '\0'; text++) {
		index = (unsigned char)*text - 32U;
		if (index >= GLASS_CLOSE_GLYPH)
			continue;
		if (dots != 0 && x + glass->glyphs[size][index].advance > start + limit - dots)
			break;
		glass_draw_glyph(server, command, size, index, x, baseline, color);
		x += glass->glyphs[size][index].advance;
	}

	/* The ellipsis. */
	if (dots != 0) {
		for (index = 0; index < 3U; index++) {
			glass_draw_glyph(server, command, size, '.' - 32, x, baseline, color);
			x += glass->glyphs[size]['.' - 32].advance;
		}
	}
}

/* Draws one glyph of the atlas with its origin at x on the baseline. */
void
glass_draw_glyph(
	struct zwl_server *server,
	VkCommandBuffer command,
	enum glass_size size,
	unsigned index,
	int32_t x,
	int32_t baseline,
	const float *color)
{
	struct glass_shape shape;
	const struct glass_glyph *glyph;
	struct zwl_glass *glass;

	/* A space draws nothing. */
	glass = server->compose->glass;
	glyph = &glass->glyphs[size][index];
	if (glyph->width == 0U || glyph->height == 0U)
		return;

	/* The glyph's pixels, one to one with the atlas. */
	glass_shape_init(&shape, (float)(x + glyph->left), (float)(baseline - glyph->top), (float)glyph->width, (float)glyph->height);
	shape.mode = MODE_TEXT;
	shape.uv[0] = (float)glyph->x / (float)GLASS_ATLAS_WIDTH;
	shape.uv[1] = (float)glyph->y / (float)GLASS_ATLAS_HEIGHT;
	shape.uv[2] = (float)(glyph->x + glyph->width) / (float)GLASS_ATLAS_WIDTH;
	shape.uv[3] = (float)(glyph->y + glyph->height) / (float)GLASS_ATLAS_HEIGHT;
	memcpy(shape.color, color, sizeof(shape.color));
	shape.set = glass->atlas.set;
	glass_shape_draw(server, command, &shape);
}

/* The advance of one glyph of the atlas (0 without text). */
int32_t
glass_glyph_advance(
	struct zwl_server *server,
	enum glass_size size,
	unsigned index)
{
	struct zwl_glass *glass;

	/* Nothing without glyphs. */
	glass = server->compose->glass;
	if (!glass->text)
		return 0;

	/* Succeeded. */
	return glass->glyphs[size][index].advance;
}

/* The descriptor set of the wallpaper, for pictures of it (the desktops). */
VkDescriptorSet
glass_wallpaper_set(
	struct zwl_server *server)
{
	/* The full-size image. */
	return server->compose->glass->wallpaper.set;
}
