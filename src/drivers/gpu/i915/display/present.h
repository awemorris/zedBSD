/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The present path of the resident node (present.c).
 *
 * A presentation is queued to the request worker and waited for.  The
 * first one makes the worker enter the display window: the panel is lit
 * and the worker keeps serving every request from inside the window, so a
 * frame is shown by writing the back buffer (a CPU copy, or a GPU copy of
 * a shared blob) and flipping.  A release does not leave the window at
 * once: the output holds the last picture for the next lease, whose first
 * frame is only a flip.  A hold that runs out, or the shutdown, makes the
 * worker leave the window, and the panel is stopped through the Linux stop
 * path.
 *
 * The header is neutral: the request worker (../worker.c) includes it
 * without the display's types.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_PRESENT_H
#define DRIVERS_GPU_I915_DISPLAY_PRESENT_H

#include <stdint.h>

struct i915_device;
struct i915_display;
struct i915_worker_present;
struct gpu_display_present;
struct gpu_display_release;
struct gpu_display_wait;

/*
 * ==== The side that asks (any thread) ====
 */

/* Shows a CPU frame: queued to the worker and waited for. */
int drv_i915_present(struct i915_device *device, const void *pixels, uint32_t width, uint32_t height, uint32_t stride, int bgra);

/* Gives the panel back: the worker holds the last picture for the next lease, or stops the panel. */
int drv_i915_present_release(struct i915_device *device);

/* Ends a hold of the last picture before the machine goes down: the panel is stopped (the PCI shutdown). */
void drv_i915_present_shutdown(struct i915_device *device);
int drv_i915_present_cut(struct i915_device *device);

/* The present, wait and release operations of the node's display (private data: the device). */
int drv_i915_present_display_present(void *device, void *session, void *object, struct gpu_display_present *request);
int drv_i915_present_display_wait(void *device, void *session, struct gpu_display_wait *request);
int drv_i915_present_display_release(void *device, void *session, const struct gpu_display_release *request);

/* Leaves the window to light the resident output again for two pipes, for a second output's first frame (the worker, IRQ lock held). */
int drv_i915_present_relight_begin(struct i915_device *device);
void drv_i915_present_retrain_request(struct i915_display *display);

/* Prepares the display lease once (its mutex and numbering). */
void drv_i915_present_lease_init(struct i915_display *display);

/* Ends the lease a closing session still holds; the panel is stopped first. */
void drv_i915_present_lease_close(struct i915_device *device, void *session);

/*
 * ==== The side that serves (the request worker) ====
 */

/* Reports whether a presentation should enter the display window (a panel exists and has not failed). */
int drv_i915_present_window_ready(struct i915_device *device);

/*
 * Runs the display window: lights the panel, serves every request from
 * inside it until a release, and stops the panel.  A panel that did not
 * come up, or did not stop cleanly, makes every later presentation fail.
 */
void drv_i915_present_window(struct i915_device *device);

/*
 * The hold of the last picture between two leases.  With the device IRQ
 * lock held: start one for a release inside the window (0 when the
 * shutdown refuses it), and tell whether it is over (ran out, or ended by
 * the shutdown).  Without the lock: give up the ended lease's mappings of
 * the panel buffers before its release completes.
 */
int drv_i915_present_hold_start(struct i915_device *device);
int drv_i915_present_hold_over(struct i915_device *device);
void drv_i915_present_hold_prepare(struct i915_device *device);

/* Shows one frame from inside the window: a CPU copy, or a GPU copy of a shared blob, then the flip. */
int drv_i915_present_frame(struct i915_device *device, const struct i915_worker_present *frame);
int drv_i915_present_blob_frame(struct i915_device *device, const struct i915_worker_present *frame);

/* Reports how many presentations completed. */
unsigned drv_i915_present_count(struct i915_device *device);

/* Flips the panel to the back buffer of the resident run. */
int drv_i915_lcd_resident_flip(struct i915_display *display);

#endif /* DRIVERS_GPU_I915_DISPLAY_PRESENT_H */
