/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The second output, the head (head.c).
 *
 * While Keiland holds the resident output's lease, a claim of another
 * connected connector makes it the head: an HDMI or an external
 * DisplayPort display on pipe B beside the built-in panel on pipe A.  Its
 * first frame lights it from inside the display window, its release and
 * the window's end stop it, and the output goes dark (D-RELEASE).
 *
 * The header is neutral: the request worker (../worker.c) includes it
 * without the display's types.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_HEAD_H
#define DRIVERS_GPU_I915_DISPLAY_HEAD_H

#include <stdint.h>

struct i915_device;
struct i915_display;
struct i915_worker_present;

/*
 * ==== The side that asks (any thread) ====
 */

/* Prepares the head's lease once (its mutex). */
void drv_i915_head_init(struct i915_display *display);

/* Makes a connected connector the head under the resident output's lease: 0 with the lease, or the refusal. */
int drv_i915_head_claim(struct i915_device *device, void *session, unsigned connector, uint64_t generation, uint64_t *lease);

/* Tells whether a session's lease is the head's. */
int drv_i915_head_owns(struct i915_display *display, void *session, uint64_t lease);

/* Ends the head's lease of a session: the head is stopped first.  0, or EINVAL for another lease. */
int drv_i915_head_release(struct i915_device *device, void *session, uint64_t lease);

/* Ends the head's lease a closing session still holds. */
void drv_i915_head_lease_close(struct i915_device *device, void *session);

/* The query's state of a connector that is not the resident output: whether it is the head, lit, or the limit. */
void drv_i915_head_connector_state(struct i915_display *display, unsigned connector, int *head, int *lit, int *limited);

/* The pipes besides the resident output's that its run leaves room for: BIT(the claimed head's pipe), or 0. */
unsigned drv_i915_head_run_pipes(struct i915_display *display);

/* Latches the claimed head's connector limited until the topology moves (D-LIMIT). */
void drv_i915_head_limit(struct i915_display *display, const char *why);

/*
 * ==== The worker's side (inside the display window) ====
 */

/* Tells whether a head frame needs the resident output lit again for two pipes first. */
int drv_i915_head_needs_relight(struct i915_device *device);

/* Shows one frame of the head (lit first when it is not): 0, ENXIO when it cannot be lit, or EIO. */
int drv_i915_head_frame(struct i915_device *device, const struct i915_worker_present *frame);

/* Stops the head when it is lit: the release (its buffers back), or the window's end (keep: its picture kept). */
void drv_i915_head_stop(struct i915_display *display, int keep);

/* Lights a head kept over the last window's end again, with its last picture, at a window's start. */
void drv_i915_head_resume(struct i915_display *display);

#endif
