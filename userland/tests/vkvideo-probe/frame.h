/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The frame hash of vkvideo-probe: a decoded NV12 picture read out of its
 * Y tiles, cropped to the sequence's display window, and hashed as ffmpeg
 * hashes a raw NV12 frame (the Y rows, then the interleaved CbCr rows), so
 * the probe's lines compare with the reference hashes of
 * plan/ws083/tests/streams/.
 */

#ifndef VKVIDEO_PROBE_FRAME_H
#define VKVIDEO_PROBE_FRAME_H

#include <stddef.h>
#include <stdint.h>

/* Where the planes of a decoded picture are, as vkGetImageSubresourceLayout reports them. */
struct frame_planes {
	const uint8_t *luma;
	size_t luma_pitch;
	const uint8_t *chroma;
	size_t chroma_pitch;
	int linear;
};

/* The display window of a picture in pixels: its origin and extent. */
struct frame_window {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
};

size_t frame_tile_y_offset(size_t pitch, uint32_t x, uint32_t y);
void frame_hash(const struct frame_planes *planes, const struct frame_window *window, char text[65]);

#endif
