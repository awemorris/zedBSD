/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * How far a display 9 node drives its display (xd9-step.c).
 *
 * A display 9 node takes every step at the start; each takes the ones
 * before it:
 *
 *   copy       every frame is copied into the firmware's framebuffer (bootfb.c)
 *   flip       the firmware's plane is flipped between the driver's buffers (xd9-flip.c)
 *   backlight  and the panel's brightness is offered as a backlight device (xd9-backlight.c)
 *   modeset    and external DP and HDMI displays are lit beside the panel, with hot-plug (xd9-node.c)
 *   full       the same as modeset: the sleep keeps the display at every step (the device sleeps in D0)
 *
 * A step that fails lowers the node's step below it for the rest of the
 * boot, with one line in the log ("i915: xd9-display: <step> failed
 * (<reason>); using <step>"): a flip the readout refuses or that later
 * does not latch brings the copies back and withdraws the external
 * displays; an external output that cannot be lit withdraws the external
 * displays, and the panel goes on flipped.  The panel's mode, the clocks
 * and the power wells stay the firmware's at every step.  The display is
 * narrowed down with i915.debug=display and i915.start=manual.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD9_STEP_H
#define DRIVERS_GPU_I915_DISPLAY_XD9_STEP_H

/* The steps, each taking the ones before it. */
#define I915_XD9_STEP_COPY		0
#define I915_XD9_STEP_FLIP		1
#define I915_XD9_STEP_BACKLIGHT		2
#define I915_XD9_STEP_MODESET		3
#define I915_XD9_STEP_FULL		4

const char *drv_i915_xd9_step_name(int step);
int drv_i915_xd9_step(void);
void drv_i915_xd9_step_fail(int step, const char *reason, int error);
void drv_i915_xd9_step_report(void);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD9_STEP_H */
