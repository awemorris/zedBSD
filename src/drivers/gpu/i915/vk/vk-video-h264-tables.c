/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The H.264 tables the MFX builder uses: the frame zig-zag
 * scans and the default scaling lists, as Recommendation ITU-T H.264
 * defines them.
 */

#include "vk-video-mfx.h"

#include <stdint.h>

/*
 * The 4x4 frame zig-zag scan (H.264 Table 8-12, the zig-zag column of
 * figure 8-8): the raster position (row * 4 + column) of the coefficient at
 * each scan index.  A scaling list is sent in scan order; the hardware's
 * quantizer matrix is in raster order.
 */
const uint8_t drv_i915_video_zigzag4[16] = {
	0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};

/* The 8x8 frame zig-zag scan (H.264 Table 8-13): the raster position (row * 8 + column) at each scan index. */
const uint8_t drv_i915_video_zigzag8[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

/* Default_4x4_Intra (H.264 Table 7-3), in scan order. */
const uint8_t drv_i915_video_default4_intra[16] = {
	6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42
};

/* Default_4x4_Inter (H.264 Table 7-3), in scan order. */
const uint8_t drv_i915_video_default4_inter[16] = {
	10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34
};

/* Default_8x8_Intra (H.264 Table 7-4), in scan order. */
const uint8_t drv_i915_video_default8_intra[64] = {
	6, 10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23,
	23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27,
	27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31,
	31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42
};

/* Default_8x8_Inter (H.264 Table 7-4), in scan order. */
const uint8_t drv_i915_video_default8_inter[64] = {
	9, 13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21,
	21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24,
	24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27,
	27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35
};
