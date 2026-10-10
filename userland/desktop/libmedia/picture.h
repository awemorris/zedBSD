/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBMEDIA_PICTURE_H
#define LIBMEDIA_PICTURE_H
#include <stddef.h>
#include <stdint.h>

#define MEDIA_COLOUR_601 0U
#define MEDIA_COLOUR_709 1U
#define MEDIA_COLOUR_2020 2U

struct media_picture_pool;

/* One refcounted linear NV12 picture; its pool survives decoder close until this picture is released. */
struct media_picture {
	struct media_picture_pool *pool;
	struct media_picture *next;
	unsigned refs;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t aspect_num;
	uint32_t aspect_den;
	unsigned colour;
	int full_range;
	uint8_t *luma;
	uint8_t *chroma;
};

struct media_picture_pool *media_picture_pool_create(uint32_t width, uint32_t height, unsigned free_max);
struct media_picture *media_picture_pool_get(struct media_picture_pool *pool);
void media_picture_pool_close(struct media_picture_pool *pool);
void media_picture_ref(struct media_picture *picture);
void media_picture_unref(void *picture);
void media_picture_size(const void *picture, int *width, int *height);
void media_picture_aspect(const void *picture, int *num, int *den);
int media_picture_scale(const void *picture, void **scaler, uint32_t *pixels, size_t stride, int width, int height);
void media_picture_scaler_free(void *scaler);
#endif
