/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The EDID detailed timing decode of the display outputs (output.c).
 *
 * It is a header of its own, with only the display's own types, so the xd
 * display files can decode a timing without the older display code's
 * headers.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_OUTPUT_TIMING_H
#define DRIVERS_GPU_I915_DISPLAY_OUTPUT_TIMING_H

#include "display-internal.h"

#include <stdint.h>

/*
 * Decodes one EDID detailed timing descriptor (18 bytes) into a mode: 0,
 * or EINVAL for a descriptor that is not a progressive detailed timing.
 */
int drv_i915_output_edid_timing(const uint8_t *descriptor, struct i915_display_mode *mode);

#endif /* DRIVERS_GPU_I915_DISPLAY_OUTPUT_TIMING_H */
