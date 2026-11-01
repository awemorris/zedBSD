/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The raw frames of vkvideo-probe: a decoded NV12 picture's display window
 * written as ffmpeg writes a raw NV12 frame (-f rawvideo -pix_fmt nv12),
 * so a run's file compares byte for byte with a reference decode.
 */

#ifndef VKVIDEO_PROBE_FRAME_RAW_H
#define VKVIDEO_PROBE_FRAME_RAW_H

#include "frame.h"

#include <stdio.h>

int frame_raw(FILE *file, const struct frame_planes *planes, const struct frame_window *window);

#endif
