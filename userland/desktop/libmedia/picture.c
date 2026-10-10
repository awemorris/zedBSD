/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Linear video pictures and their CPU scaler; image tiling remains entirely inside the Vulkan implementation. */
#include "picture.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* One decoder's reusable picture storage; live includes checked-out pictures and its idle list. */
struct media_picture_pool {
	pthread_mutex_t lock;
	struct media_picture *idle;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	unsigned live;
	unsigned free_count;
	unsigned free_max;
	int closed;
};

/* Scaler-owned horizontal mapping and two converted row caches, independent of any decoder lifetime. */
struct picture_scaler {
	unsigned *indices;
	unsigned *fractions;
	uint32_t *rows;
	uint32_t source_width;
	int width;
};

/* Integer colour contributions, constructed once and immutable for all simultaneous video decoders. */
static int32_t picture_colours[3][2][5][256];

/* Publishes mathematical colour coefficients before any picture is scaled. */
static pthread_once_t picture_once = PTHREAD_ONCE_INIT;

static void picture_initialize(void);
static void picture_release(struct media_picture *picture);
static void picture_pool_destroy(struct media_picture_pool *pool);
static uint32_t picture_pixel(const struct media_picture *picture, uint32_t x, uint32_t y);
static unsigned picture_clamp(int32_t value);
static uint32_t picture_blend(uint32_t a, uint32_t b, unsigned fraction);
static int picture_scaler_prepare(struct picture_scaler *scaler, uint32_t source_width, int width);

/* Create a pool whose visible dimensions and even NV12 pitch are fixed for its lifetime. */
struct media_picture_pool *
media_picture_pool_create(
	uint32_t width,
	uint32_t height,
	unsigned free_max)
{
	struct media_picture_pool *pool;
	int error;

	/* The bounded CPU picture size also keeps every NV12 allocation and scaler arithmetic representable. */
	if (width == 0U || width > 8192U)
		return NULL;
	if (height == 0U || height > 8192U)
		return NULL;
	pool = calloc(1U, sizeof(*pool));
	if (pool == NULL)
		return NULL;
	error = pthread_mutex_init(&pool->lock, NULL);
	if (error != 0) {
		free(pool);
		return NULL;
	}

	/* Empty pools own no pixel allocation until the first decode completes. */
	pool->width = width;
	pool->height = height;
	pool->pitch = (width + 1U) & ~1U;
	pool->free_max = free_max;
	return pool;
}

/* Obtain a picture without a fixed checked-out limit; slow consumers cannot exhaust a decoder's tiny ring. */
struct media_picture *
media_picture_pool_get(
	struct media_picture_pool *pool)
{
	struct media_picture *picture;
	size_t luma;
	size_t chroma;

	/* The decoder owns the pool while get is permitted; close ends that right. */
	pthread_mutex_lock(&pool->lock);
	if (pool->closed != 0) {
		pthread_mutex_unlock(&pool->lock);
		return NULL;
	}
	picture = pool->idle;
	if (picture != NULL) {
		pool->idle = picture->next;
		pool->free_count--;
	} else {
		/* Fresh storage is checked out under the pool lock, keeping live accounting exact. */
		picture = calloc(1U, sizeof(*picture));
		if (picture == NULL) {
			pthread_mutex_unlock(&pool->lock);
			return NULL;
		}
		luma = (size_t)pool->pitch * pool->height;
		chroma = (size_t)pool->pitch * ((pool->height + 1U) / 2U);
		picture->luma = malloc(luma + chroma);
		if (picture->luma == NULL) {
			free(picture);
			pthread_mutex_unlock(&pool->lock);
			return NULL;
		}
		picture->chroma = picture->luma + luma;
		picture->pool = pool;
		picture->width = pool->width;
		picture->height = pool->height;
		picture->pitch = pool->pitch;
		pool->live++;
	}

	/* Reuse resets all per-picture metadata rather than retaining a previous stream's colour or aspect. */
	picture->next = NULL;
	picture->refs = 1U;
	picture->aspect_num = 1U;
	picture->aspect_den = 1U;
	picture->colour = MEDIA_COLOUR_601;
	picture->full_range = 0;
	pthread_mutex_unlock(&pool->lock);
	return picture;
}

/* Retire idle storage now while allowing exported pictures to retain their pool after decoder close. */
void
media_picture_pool_close(
	struct media_picture_pool *pool)
{
	struct media_picture *picture;
	struct media_picture *next;
	unsigned live;

	/* A failed decoder open may have made no pool. */
	if (pool == NULL)
		return;
	pthread_mutex_lock(&pool->lock);
	pool->closed = 1;
	picture = pool->idle;
	pool->idle = NULL;
	pool->free_count = 0U;

	/* Checked-out storage retains live ownership and is unaffected by freeing the idle list. */
	while (picture != NULL) {
		next = picture->next;
		picture_release(picture);
		pool->live--;
		picture = next;
	}
	live = pool->live;
	pthread_mutex_unlock(&pool->lock);

	/* The last outstanding picture will destroy a pool that still has live storage. */
	if (live == 0U)
		picture_pool_destroy(pool);
	return;
}

/* Retain one exported picture under the same lock protecting idle recycling. */
void
media_picture_ref(
	struct media_picture *picture)
{
	pthread_mutex_lock(&picture->pool->lock);
	picture->refs++;
	pthread_mutex_unlock(&picture->pool->lock);
	return;
}

/* Return a picture to its bounded idle cache, or release the final closed-pool allocation. */
void
media_picture_unref(
	void *object)
{
	struct media_picture *picture;
	struct media_picture_pool *pool;
	int destroy;

	/* Taking no picture creates no release obligation. */
	picture = object;
	if (picture == NULL)
		return;
	pool = picture->pool;
	destroy = 0;
	pthread_mutex_lock(&pool->lock);
	picture->refs--;
	if (picture->refs != 0U) {
		pthread_mutex_unlock(&pool->lock);
		return;
	}

	/* Cached idle storage remains live only while its decoder-owned pool is open. */
	if (pool->closed == 0 && pool->free_count < pool->free_max) {
		picture->next = pool->idle;
		pool->idle = picture;
		pool->free_count++;
	} else {
		picture_release(picture);
		pool->live--;
		if (pool->closed != 0 && pool->live == 0U)
			destroy = 1;
	}
	pthread_mutex_unlock(&pool->lock);

	/* No decoder or picture can reach a closed pool after the last live allocation retires. */
	if (destroy != 0)
		picture_pool_destroy(pool);
	return;
}

/* Report visible cropped dimensions rather than padded GPU decode extents. */
void
media_picture_size(
	const void *object,
	int *width,
	int *height)
{
	const struct media_picture *picture;

	/* The copied CPU picture remains valid independently of its hardware session. */
	picture = object;
	*width = (int)picture->width;
	*height = (int)picture->height;
	return;
}

/* Report sample aspect without baking it into either the pixel data or scaler's dimensions. */
void
media_picture_aspect(
	const void *object,
	int *num,
	int *den)
{
	const struct media_picture *picture;

	/* Invalid absent metadata always has a square-pixel presentation default. */
	picture = object;
	*num = 1;
	*den = 1;
	if (picture->aspect_num != 0U && picture->aspect_num <= 2147483647U)
		*num = (int)picture->aspect_num;
	if (picture->aspect_den != 0U && picture->aspect_den <= 2147483647U)
		*den = (int)picture->aspect_den;
	return;
}

/* Convert colour and scale a retained linear picture into the caller's ARGB canvas. */
int
media_picture_scale(
	const void *object,
	void **state,
	uint32_t *pixels,
	size_t stride,
	int width,
	int height)
{
	const struct media_picture *picture;
	struct picture_scaler *scaler;
	uint32_t y0;
	uint32_t y1;
	uint32_t x;
	unsigned fraction;
	int y;
	double coordinate;
	uint32_t top;
	uint32_t bottom;
	int error;

	/* Validate destination shape before allocating a scaler or addressing source rows. */
	if (object == NULL || state == NULL || pixels == NULL)
		return EINVAL;
	if (width <= 0 || width > 8192)
		return EINVAL;
	if (height <= 0 || height > 8192)
		return EINVAL;
	if (stride < (size_t)width * sizeof(uint32_t))
		return EINVAL;
	picture = object;
	if (picture->colour > MEDIA_COLOUR_2020)
		return EINVAL;
	error = pthread_once(&picture_once, picture_initialize);
	if (error != 0)
		return error;

	/* Equal-size pictures use the exact nearest chroma conversion without interpolation storage. */
	if ((uint32_t)width == picture->width && (uint32_t)height == picture->height) {
		for (y = 0; y < height; y++) {
			for (x = 0U; x < picture->width; x++)
				pixels[x] = picture_pixel(picture, x, (uint32_t)y);
			pixels = (uint32_t *)((uint8_t *)pixels + stride);
		}
		return 0;
	}

	/* The scaler owns its mapping and scratch rows, surviving any decoder or picture change. */
	scaler = *state;
	if (scaler == NULL) {
		scaler = calloc(1U, sizeof(*scaler));
		if (scaler == NULL)
			return ENOMEM;
		*state = scaler;
	}
	error = picture_scaler_prepare(scaler, picture->width, width);
	if (error != 0)
		return error;

	/* Centered bilinear mapping clamps the image's edges instead of accessing padding. */
	for (y = 0; y < height; y++) {
		coordinate = ((double)y + 0.5) * picture->height / height - 0.5;
		if (coordinate < 0.0)
			coordinate = 0.0;
		if (coordinate > picture->height - 1U)
			coordinate = picture->height - 1U;
		y0 = (uint32_t)coordinate;
		y1 = y0 + 1U;
		if (y1 == picture->height)
			y1 = y0;
		fraction = (unsigned)((coordinate - y0) * 65536.0 + 0.5);

		/* Convert source rows once for all horizontal interpolation in this destination row. */
		for (x = 0U; x < picture->width; x++) {
			scaler->rows[x] = picture_pixel(picture, x, y0);
			scaler->rows[picture->width + x] = picture_pixel(picture, x, y1);
		}

		/* Interpolate already converted colour with bounded integer weights. */
		for (x = 0U; x < (uint32_t)width; x++) {
			top = picture_blend(scaler->rows[scaler->indices[x * 2U]], scaler->rows[scaler->indices[x * 2U + 1U]], scaler->fractions[x]);
			bottom = picture_blend(scaler->rows[picture->width + scaler->indices[x * 2U]], scaler->rows[picture->width + scaler->indices[x * 2U + 1U]], scaler->fractions[x]);
			pixels[x] = picture_blend(top, bottom, fraction);
		}
		pixels = (uint32_t *)((uint8_t *)pixels + stride);
	}
	return 0;
}

/* Release only scaler-owned scratch; no picture or Vulkan object is retained by this operation. */
void
media_picture_scaler_free(
	void *state)
{
	struct picture_scaler *scaler;

	/* The equal-size fast path may never have allocated a scaler. */
	scaler = state;
	if (scaler == NULL)
		return;
	free(scaler->indices);
	free(scaler->fractions);
	free(scaler->rows);
	free(scaler);
	return;
}

/* Generate integer BT.601/709/2020 contributions from their published luma coefficients. */
static void
picture_initialize(
	void)
{
	double kr;
	double kb;
	double kg;
	double yscale;
	double cscale;
	double offset;
	double chroma;
	unsigned colour;
	unsigned full;
	unsigned value;

	/* Every table entry is evaluated once rather than using per-pixel floating-point matrix arithmetic. */
	for (colour = 0U; colour < 3U; colour++) {
		kr = 0.299;
		kb = 0.114;
		if (colour == MEDIA_COLOUR_709) {
			kr = 0.2126;
			kb = 0.0722;
		} else if (colour == MEDIA_COLOUR_2020) {
			kr = 0.2627;
			kb = 0.0593;
		}
		kg = 1.0 - kr - kb;

		/* Limited-range luma/chroma use their different normative digital excursions. */
		for (full = 0U; full < 2U; full++) {
			yscale = 255.0 / 219.0;
			cscale = 255.0 / 224.0;
			offset = 16.0;
			if (full != 0U) {
				yscale = 1.0;
				cscale = 1.0;
				offset = 0.0;
			}

			/* Signed chroma contributions fit comfortably in 32-bit fixed-point arithmetic. */
			for (value = 0U; value < 256U; value++) {
				chroma = ((double)value - 128.0) * cscale * 65536.0;
				picture_colours[colour][full][0][value] = (int32_t)(((double)value - offset) * yscale * 65536.0);
				picture_colours[colour][full][1][value] = (int32_t)(2.0 * (1.0 - kr) * chroma);
				picture_colours[colour][full][2][value] = (int32_t)(-2.0 * kb * (1.0 - kb) / kg * chroma);
				picture_colours[colour][full][3][value] = (int32_t)(-2.0 * kr * (1.0 - kr) / kg * chroma);
				picture_colours[colour][full][4][value] = (int32_t)(2.0 * (1.0 - kb) * chroma);
			}
		}
	}
	return;
}

/* Release an individual CPU allocation; callers maintain the corresponding pool's live accounting. */
static void
picture_release(
	struct media_picture *picture)
{
	free(picture->luma);
	free(picture);
	return;
}

/* Destroy a pool only when its decoder is closed and its final live picture has retired. */
static void
picture_pool_destroy(
	struct media_picture_pool *pool)
{
	pthread_mutex_destroy(&pool->lock);
	free(pool);
	return;
}

/* Convert one visible NV12 sample, using its interleaved 4:2:0 chroma pair. */
static uint32_t
picture_pixel(
	const struct media_picture *picture,
	uint32_t x,
	uint32_t y)
{
	int32_t (*table)[256];
	int32_t luma;
	unsigned u;
	unsigned v;
	unsigned red;
	unsigned green;
	unsigned blue;

	/* The source pitch includes odd-width chroma padding but visible pixels never expose it. */
	table = picture_colours[picture->colour][0];
	if (picture->full_range != 0)
		table = picture_colours[picture->colour][1];
	luma = table[0][picture->luma[(size_t)y * picture->pitch + x]];
	u = picture->chroma[(size_t)(y / 2U) * picture->pitch + (x & ~1U)];
	v = picture->chroma[(size_t)(y / 2U) * picture->pitch + (x & ~1U) + 1U];
	red = picture_clamp(luma + table[1][v]);
	green = picture_clamp(luma + table[2][u] + table[3][v]);
	blue = picture_clamp(luma + table[4][u]);
	return 0xff000000U | (red << 16U) | (green << 8U) | blue;
}

/* Round and clamp a signed fixed-point digital colour channel. */
static unsigned
picture_clamp(
	int32_t value)
{
	/* Negative and over-range colour conversion results saturate before any unsigned shift. */
	if (value <= 0)
		return 0U;
	if (value >= 255 * 65536)
		return 255U;
	return (unsigned)(value + 32768) >> 16U;
}

/* Interpolate two opaque canvas pixels with one exact fixed-point weight. */
static uint32_t
picture_blend(
	uint32_t a,
	uint32_t b,
	unsigned fraction)
{
	uint32_t result;
	unsigned shift;
	unsigned channel;

	/* Independent byte accumulation avoids carries between adjacent colour channels. */
	result = 0xff000000U;
	for (shift = 0U; shift < 24U; shift += 8U) {
		channel = (((a >> shift) & 255U) * (65536U - fraction) + ((b >> shift) & 255U) * fraction + 32768U) >> 16U;
		result |= channel << shift;
	}
	return result;
}

/* Rebuild horizontal maps transactionally when destination size or source geometry changes. */
static int
picture_scaler_prepare(
	struct picture_scaler *scaler,
	uint32_t source_width,
	int width)
{
	unsigned *indices;
	unsigned *fractions;
	uint32_t *rows;
	unsigned x0;
	unsigned x1;
	int x;
	double coordinate;

	/* A fixed geometry reuses all mapping and row storage across successive pictures. */
	if (scaler->width == width && scaler->source_width == source_width)
		return 0;
	indices = malloc((size_t)width * 2U * sizeof(*indices));
	fractions = malloc((size_t)width * sizeof(*fractions));
	rows = malloc((size_t)source_width * 2U * sizeof(*rows));
	if (indices == NULL || fractions == NULL || rows == NULL) {
		free(indices);
		free(fractions);
		free(rows);
		return ENOMEM;
	}

	/* Evaluate pixel-center mapping once, clamping either source edge. */
	for (x = 0; x < width; x++) {
		coordinate = ((double)x + 0.5) * source_width / width - 0.5;
		if (coordinate < 0.0)
			coordinate = 0.0;
		if (coordinate > source_width - 1U)
			coordinate = source_width - 1U;
		x0 = (unsigned)coordinate;
		x1 = x0 + 1U;
		if (x1 == source_width)
			x1 = x0;
		indices[x * 2U] = x0;
		indices[x * 2U + 1U] = x1;
		fractions[x] = (unsigned)((coordinate - x0) * 65536.0 + 0.5);
	}
	free(scaler->indices);
	free(scaler->fractions);
	free(scaler->rows);
	scaler->indices = indices;
	scaler->fractions = fractions;
	scaler->rows = rows;
	scaler->source_width = source_width;
	scaler->width = width;
	return 0;
}
