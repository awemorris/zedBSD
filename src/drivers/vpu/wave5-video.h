/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Wave5 decoder as the PowerVR render node's video engine: Vulkan
 * Video's H.264 and H.265 decode run on the core's instances (wave5-video.c).
 */

#ifndef KERN_DRIVERS_VPU_WAVE5_VIDEO_H
#define KERN_DRIVERS_VPU_WAVE5_VIDEO_H

struct drv_wave5;

/*
 * Registers a booted core with the render node as its video engine, for
 * as long as the system runs.  EBUSY when an engine is registered
 * already, ENOMEM.
 */
int drv_wave5_video_register(struct drv_wave5 *core);

#endif
