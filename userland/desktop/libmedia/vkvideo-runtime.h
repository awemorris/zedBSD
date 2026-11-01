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

struct vkvideo_runtime;
int media_vkvideo_runtime_open(const StdVideoH264SequenceParameterSet *sps, unsigned reorder, struct vkvideo_runtime **runtime);
int media_vkvideo_runtime_gpu(const struct vkvideo_runtime *runtime);
int media_vkvideo_runtime_parameters(struct vkvideo_runtime *runtime, const struct h264_stream *stream);
int media_vkvideo_runtime_reserve(struct vkvideo_runtime *runtime);
int media_vkvideo_runtime_submit(struct vkvideo_runtime *runtime, const struct h264_stream *stream, const struct h264_picture *picture, const struct h264_dpb_plan *plan, struct media_picture *output, uint32_t *ticket);
int media_vkvideo_runtime_finish(struct vkvideo_runtime *runtime, uint32_t ticket);
void media_vkvideo_runtime_discard(struct vkvideo_runtime *runtime);
uint32_t media_vkvideo_runtime_busy(const struct vkvideo_runtime *runtime);
uint32_t media_vkvideo_runtime_slots(const struct vkvideo_runtime *runtime);
uint32_t media_vkvideo_runtime_flights(const struct vkvideo_runtime *runtime);
int media_vkvideo_runtime_skips_failed(const struct vkvideo_runtime *runtime);
void media_vkvideo_runtime_close(struct vkvideo_runtime *runtime);
void media_vkvideo_runtime_reset(struct vkvideo_runtime *runtime);
#endif
