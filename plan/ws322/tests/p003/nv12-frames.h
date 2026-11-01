/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* The client side of the NV12, query and video report host check (nv12-frames.c), in plain types only. */

#ifndef WS322_P003_NV12_FRAMES_H
#define WS322_P003_NV12_FRAMES_H

#include <stddef.h>
#include <stdint.h>

/* The NV12 image the frames make: 1920x1080, bound at offset 0 of its own host visible memory. */
#define NV12_WIDTH		1920U
#define NV12_HEIGHT		1080U

/* The wire identities the kernel side looks objects up by. */
#define NV12_ID_IMAGE		100U
#define NV12_ID_MEMORY		101U
#define NV12_ID_LUMA		102U
#define NV12_ID_CHROMA		103U
#define NV12_ID_SET		104U
#define NV12_ID_POOL		105U
#define NV12_ID_IMPORT_IMAGE	106U

/* What the report frame's replies are checked against: the session's video or none. */
struct nv12_expect {
	uint32_t queue_flags;
	uint32_t codecs;
	uint32_t nv12_optimal_features;
	int video_usage_allowed;
};

size_t nv12_frame_reports(uint32_t reply_blob, uint8_t **bytes);
void nv12_replies_reports(uint8_t *reply, const struct nv12_expect *expect);
size_t nv12_frame_image(uint32_t reply_blob, uint8_t **bytes);
void nv12_replies_image(uint8_t *reply, uint64_t *image_bytes);
size_t nv12_frame_memory(uint32_t reply_blob, uint64_t image_bytes, uint8_t **bytes);
void nv12_replies_memory(uint8_t *reply);
size_t nv12_frame_commands(uint32_t reply_blob, uint8_t **bytes);
void nv12_replies_commands(uint8_t *reply);
size_t nv12_frame_results(uint32_t reply_blob, uint32_t query, uint32_t flags, uint64_t stride, uint64_t data_bytes, uint8_t **bytes);
void nv12_replies_results(uint8_t *reply, uint32_t expected_result, const uint8_t *expected, size_t expected_bytes, const char *what);
void nv12_frame_counts(int *passed, int *failed);

#endif
