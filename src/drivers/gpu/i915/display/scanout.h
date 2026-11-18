/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Scanout buffers and the panel buffers of the resident node (scanout.c).
 *
 * A scanout buffer's life: create -> pin -> [publish after CPU writes] ->
 * begin ... end -> unpin -> destroy, or abandon when the stop of the
 * display reading it could not be confirmed.  The resident node keeps two
 * full-panel buffers; the present path maps them into the presenting
 * session's address space, and the scanout operations answer the device
 * and constraint queries of the display-only pairing.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_SCANOUT_H
#define DRIVERS_GPU_I915_DISPLAY_SCANOUT_H

#include "display-internal.h"

struct i915_display;
struct i915_ppgtt;
struct drv_gpu_scanout_ops;

/*
 * ==== One scanout buffer ====
 */

/*
 * 0, EINVAL (unsupported layout), ENOMEM (backing) or EBUSY (the storage
 * holds a buffer).  The storage must start zeroed; destroy returns it to
 * that state.  Nothing stays allocated on failure.
 */
int drv_i915_scanout_create(struct i915_gt_mem *gm, uint32_t width, uint32_t height, uint32_t format, uint64_t modifier, struct i915_scanout *so);

/* Reserves the GGTT range (alignment and guards), writes the PTEs and flushes the CPU cache for the display. */
int drv_i915_scanout_pin(struct i915_scanout *so, const char *owner);

/* Makes pixels the CPU changed visible to the display engine. */
void drv_i915_scanout_publish(struct i915_scanout *so);

/* The display engine starts, or has provably stopped, reading the buffer. */
int drv_i915_scanout_begin(struct i915_scanout *so);
void drv_i915_scanout_end(struct i915_scanout *so);

/* EBUSY while the display reads it. */
int drv_i915_scanout_unpin(struct i915_scanout *so);

/* EBUSY while pinned or in use. */
int drv_i915_scanout_destroy(struct i915_scanout *so);

/* Keeps pages, mapping and PTEs for ever: the stop could not be confirmed. */
void drv_i915_scanout_abandon(struct i915_scanout *so);

/*
 * ==== The panel buffers of the resident node ====
 */

/* One of the two resident buffers, and the one not on the panel (the back buffer). */
struct i915_scanout *drv_i915_lcd_resident_buffer(struct i915_display *display, unsigned i);
struct i915_scanout *drv_i915_lcd_resident_back(struct i915_display *display);

/* Maps both panel buffers uncached into a presenting session's address space, and unmaps them. */
int drv_i915_scanout_map_panel(struct i915_display *display, struct i915_ppgtt *vm);
void drv_i915_scanout_unmap_panel(struct i915_display *display);

/* The display-only pairing operations of the resident node (private data: the device). */
extern const struct drv_gpu_scanout_ops drv_i915_scanout_ops;

/* The display-only pairing operations of the capture node and the boot framebuffer display (private data: the device). */
extern const struct drv_gpu_scanout_ops drv_i915_capture_scanout_ops;

#endif /* DRIVERS_GPU_I915_DISPLAY_SCANOUT_H */
