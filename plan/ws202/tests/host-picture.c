/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
/* Retained picture/pool lifetime and fixed-point colour against independent floating-point equations. */
#include "userland/desktop/libmedia/picture.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

int
main(
	void)
{
	struct media_picture_pool *pool;
	struct media_picture *pictures[32];
	uint32_t pixels[64];
	void *scaler;
	unsigned colour;
	unsigned full;
	unsigned value;
	unsigned index;
	unsigned channel;
	unsigned actual;
	int expected;
	int difference;
	int error;
	int num;
	int den;
	double kr;
	double kb;
	double kg;
	double y;
	double u;
	double v;
	double channels[3];

	/* Check out more pictures than the cache limit and keep them after decoder-style pool close. */
	pool = media_picture_pool_create(3U, 3U, 2U);
	if (pool == NULL)
		return 1;
	for (index = 0U; index < 32U; index++) {
		pictures[index] = media_picture_pool_get(pool);
		if (pictures[index] == NULL)
			return 1;
	}

	/* Retain one extra reference before closing the owning decoder pool. */
	media_picture_ref(pictures[0]);
	media_picture_unref(pictures[0]);
	media_picture_pool_close(pool);
	scaler = NULL;
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

		/* Compute independent floating colour coefficients for comparison with production integer tables. */
		kg = 1.0 - kr - kb;
		for (full = 0U; full < 2U; full++) {
			for (value = 0U; value < 256U; value++) {
				pictures[0]->colour = colour;
				pictures[0]->full_range = (int)full;
				memset(pictures[0]->luma, (int)value, 12U);
				memset(pictures[0]->chroma, 0, 8U);
				pictures[0]->chroma[0] = (uint8_t)(255U - value);
				pictures[0]->chroma[1] = (uint8_t)value;
				error = media_picture_scale(pictures[0], &scaler, pixels, 12U, 3, 3);
				if (error != 0)
					return 1;
				y = value;
				u = 127.0 - value;
				v = (double)value - 128.0;
				if (full == 0U) {
					y = (y - 16.0) * 255.0 / 219.0;
					u *= 255.0 / 224.0;
					v *= 255.0 / 224.0;
				}

				/* Convert the independent colour components before clamping and pixel comparison. */
				channels[0] = y + 2.0 * (1.0 - kr) * v;
				channels[1] = y - 2.0 * kb * (1.0 - kb) / kg * u - 2.0 * kr * (1.0 - kr) / kg * v;
				channels[2] = y + 2.0 * (1.0 - kb) * u;
				for (channel = 0U; channel < 3U; channel++) {
					expected = (int)floor(channels[channel] + 0.5);
					if (expected < 0)
						expected = 0;
					if (expected > 255)
						expected = 255;
					actual = (pixels[0] >> (16U - 8U * channel)) & 255U;
					difference = (int)actual - expected;
					if (difference < -1 || difference > 1)
						return 1;
				}
			}
		}
	}

	/* Check non-square sample aspect metadata on an exported retained picture. */
	pictures[0]->aspect_num = 16U;
	pictures[0]->aspect_den = 15U;
	media_picture_aspect(pictures[0], &num, &den);
	if (num != 16 || den != 15)
		return 1;
	error = media_picture_scale(pictures[0], &scaler, pixels, 32U, 8, 8);
	if (error != 0)
		return 1;
	for (index = 0U; index < 32U; index++)
		media_picture_unref(pictures[index]);
	media_picture_scaler_free(scaler);
	puts("Picture PASS (32 held past pool close, odd NV12 size, six colour matrices, independent equations, scaling, SAR)");
	return 0;
}
