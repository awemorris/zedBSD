/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The check of what the firmware left on a display 9 display before the GT
 * starts (xd9-firmware.c).
 *
 * The driver does not light a display 9 display at the start: the firmware's
 * picture stays on, and the GT must neither overwrite it nor run its DMA
 * through a VT-d unit zedBSD does not drive.  The check only reads
 * registers.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD9_FIRMWARE_H
#define DRIVERS_GPU_I915_DISPLAY_XD9_FIRMWARE_H

struct i915_display;
struct i915_mmio;

int drv_i915_xd9_firmware_check(struct i915_display *display, struct i915_mmio *mmio, unsigned driver_ggtt_first);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD9_FIRMWARE_H */
