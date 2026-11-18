/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#ifndef LIBMEDIA_PICTURE_H
#define LIBMEDIA_PICTURE_H
#include "media-os.h"
#include <stddef.h>
#include <stdint.h>

#define MEDIA_COLOUR_601 0U
#define MEDIA_COLOUR_709 1U
#define MEDIA_COLOUR_2020 2U

/* The images a decoding context whose pictures the GPU holds has at most. */
#define MEDIA_GPU_SLOTS 18U

struct media_picture_pool;
struct media_gpu_picture;

/*
 * The images of one Vulkan Video context whose pictures the GPU holds
 * (ws236-p011), shared by the context and the pictures decoded into them.
 *
 * Filled by the context before its first picture and then read only:
 * identity, the images' memory handles (fd, closed with the last
 * reference), their memory size and type and the images' extent.  Under
 * the lock: the references (the context's and one for each picture that
 * holds an image), and held, the images pictures hold, which the context
 * does not decode into until they are freed.
 */
struct media_gpu_slots {
	struct media_os_mutex lock;
	unsigned refs;
	uint32_t held;
	uint64_t identity;
	unsigned count;
	int fd[MEDIA_GPU_SLOTS];
	uint64_t memory_bytes;
	uint32_t memory_type;
	uint32_t image_width;
	uint32_t image_height;
};

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
	/* The image the picture holds when the GPU holds it (luma and chroma are then NULL), and its window. */
	struct media_gpu_slots *gpu;
	unsigned gpu_slot;
	uint32_t gpu_x;
	uint32_t gpu_y;
};

struct media_picture_pool *media_picture_pool_create(uint32_t width, uint32_t height, unsigned free_max);
struct media_picture_pool *media_picture_pool_create_gpu(uint32_t width, uint32_t height, unsigned free_max);
struct media_gpu_slots *media_gpu_slots_create(unsigned count);
void media_gpu_slots_hold(struct media_picture *picture, struct media_gpu_slots *slots, unsigned slot, uint32_t x, uint32_t y);
uint32_t media_gpu_slots_held(struct media_gpu_slots *slots);
void media_gpu_slots_unref(struct media_gpu_slots *slots);
int media_picture_gpu(const void *picture, struct media_gpu_picture *gpu);
struct media_picture *media_picture_pool_get(struct media_picture_pool *pool);
void media_picture_pool_close(struct media_picture_pool *pool);
void media_picture_ref(struct media_picture *picture);
void media_picture_unref(void *picture);
void media_picture_size(const void *picture, int *width, int *height);
void media_picture_aspect(const void *picture, int *num, int *den);
int media_picture_scale(const void *picture, void **scaler, uint32_t *pixels, size_t stride, int width, int height);
void media_picture_scaler_free(void *scaler);
#endif
