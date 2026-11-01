/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Private standard Vulkan Video ownership; no GPU hardware format is exposed to the media backend. */
#ifndef LIBMEDIA_VKVIDEO_RUNTIME_H
#define LIBMEDIA_VKVIDEO_RUNTIME_H
#include "h264-dpb.h"
#include "picture.h"

/* The codecs a context decodes. */
#define VKVIDEO_H264 0U
#define VKVIDEO_H265 1U

/*
 * What a context is made for: its codec, its profile and level (the
 * codec's StdVideo enumerants), the sequence's coded size, and the
 * references it keeps at most.
 */
struct vkvideo_shape {
	unsigned codec;
	uint32_t profile;
	uint32_t level;
	uint32_t width;
	uint32_t height;
	uint32_t references;
};

/*
 * One picture's decode as a codec-neutral job: the slice segments (each
 * at slice_offsets[i] in data, slice_sizes[i] bytes after its start code),
 * the slot it is decoded into (setup), whether the coding scope is reset,
 * the slots it references with their StdVideo reference records (an array
 * of the codec's StdVideoDecode*ReferenceInfo), the slots the scope
 * deactivates, the codec's StdVideo records of the written slot and of
 * the picture, the coded size and the crop of the sequence.
 */
struct vkvideo_job {
	const uint8_t *data;
	uint32_t slice_count;
	const size_t *slice_offsets;
	const size_t *slice_sizes;
	int32_t setup;
	int reset;
	uint32_t reference_count;
	const int32_t *references;
	const void *reference_info;
	uint32_t deactivate_count;
	const int32_t *deactivate;
	const void *setup_info;
	const void *picture_info;
	uint32_t width;
	uint32_t height;
	uint32_t crop_x;
	uint32_t crop_y;
};

struct vkvideo_runtime;
int media_vkvideo_runtime_open(const StdVideoH264SequenceParameterSet *sps, unsigned reorder, struct vkvideo_runtime **runtime);
int media_vkvideo_runtime_open_h265(const struct vkvideo_shape *shape, unsigned reorder, struct vkvideo_runtime **runtime);
int media_vkvideo_runtime_gpu(const struct vkvideo_runtime *runtime);
int media_vkvideo_runtime_parameters(struct vkvideo_runtime *runtime, const struct h264_stream *stream);
int media_vkvideo_runtime_parameters_h265(struct vkvideo_runtime *runtime, const VkVideoDecodeH265SessionParametersAddInfoKHR *add);
int media_vkvideo_runtime_reserve(struct vkvideo_runtime *runtime);
int media_vkvideo_runtime_submit(struct vkvideo_runtime *runtime, const struct h264_stream *stream, const struct h264_picture *picture, const struct h264_dpb_plan *plan, struct media_picture *output, uint32_t *ticket);
int media_vkvideo_runtime_submit_h265(struct vkvideo_runtime *runtime, const struct vkvideo_job *job, struct media_picture *output, uint32_t *ticket);
int media_vkvideo_runtime_finish(struct vkvideo_runtime *runtime, uint32_t ticket);
void media_vkvideo_runtime_discard(struct vkvideo_runtime *runtime);
uint32_t media_vkvideo_runtime_busy(const struct vkvideo_runtime *runtime);
uint32_t media_vkvideo_runtime_slots(const struct vkvideo_runtime *runtime);
uint32_t media_vkvideo_runtime_flights(const struct vkvideo_runtime *runtime);
int media_vkvideo_runtime_skips_failed(const struct vkvideo_runtime *runtime);
void media_vkvideo_runtime_close(struct vkvideo_runtime *runtime);
void media_vkvideo_runtime_reset(struct vkvideo_runtime *runtime);
#endif
