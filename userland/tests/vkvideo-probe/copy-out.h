/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The copy-out of vkvideo-probe (--copy, copy-out.c): a decoded picture
 * copied into a buffer of plain rows for the hash.
 */

#ifndef VKVIDEO_PROBE_COPY_OUT_H
#define VKVIDEO_PROBE_COPY_OUT_H

#include "frame.h"

#include <stdint.h>

struct probe;

int probe_copy_out(struct probe *probe, uint32_t slot, struct frame_planes *planes);
void probe_copy_out_close(struct probe *probe);

#endif
