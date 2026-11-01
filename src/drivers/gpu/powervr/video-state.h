/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The render node's view of a video engine: the one engine the board's
 * glue registers (drv_powervr_video_register), and what a session offers
 * of it -- the capset's native flags, the queue family's flags and codec
 * operations, the NV12 format's video features and usages -- worked out
 * from the operations word the session read when it opened, and the bytes
 * of one result status query's record.  This part holds no lock and no
 * memory, and it builds on the host too.
 */

#ifndef KERN_DRIVERS_GPU_POWERVR_VIDEO_STATE_H
#define KERN_DRIVERS_GPU_POWERVR_VIDEO_STATE_H

#include <stdint.h>

#include <libc/vulkan/vulkan_core.h>

#include "drivers/gpu/powervr/powervr.h"

/* The native capset's flags: H.264 decode (the only codec bit libvulkan defines). */
#define POWERVR_VIDEO_CAPSET_DECODE_H264	1U

/* The most bytes one query's record takes: a 64-bit status and a 64-bit availability. */
#define POWERVR_VIDEO_QUERY_RECORD_MAX		16U

uint32_t powervr_video_operations(void);
const struct drv_powervr_video_ops *powervr_video_engine(void **engine);
uint32_t powervr_video_capset_flags(uint32_t operations);
uint32_t powervr_video_queue_flags(uint32_t operations);
uint32_t powervr_video_codec_operations(uint32_t operations);
uint32_t powervr_video_format_features(uint32_t operations);
uint32_t powervr_video_image_usages(uint32_t operations);
uint32_t powervr_video_query_record(int32_t status, uint32_t available, uint32_t flags, uint8_t record[POWERVR_VIDEO_QUERY_RECORD_MAX]);

#endif /* KERN_DRIVERS_GPU_POWERVR_VIDEO_STATE_H */
