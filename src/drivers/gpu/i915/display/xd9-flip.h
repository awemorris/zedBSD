/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The flip display of a display 9 node (xd9-flip.c).
 *
 * A display 9 node keeps the mode the firmware set.  When the
 * firmware lit exactly one pipe whose first plane scans out a linear
 * XRGB8888 surface, the boot framebuffer display (bootfb.c) shows the
 * desktop by flipping that plane between two buffers of the driver's own
 * instead of copying every frame into the firmware's framebuffer: the GPU
 * draws a frame into the buffer the plane does not show, and the plane's
 * surface register is pointed at it, which takes effect at the next
 * vertical blank.  Nothing else of the display is written: the mode, the
 * clocks, the watermarks and the plane's control stay the firmware's.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD9_FLIP_H
#define DRIVERS_GPU_I915_DISPLAY_XD9_FLIP_H

#include <stdint.h>

struct gpu_display_present;
struct i915_device;
struct i915_xd9_panel;

/* How long a flip may take to latch before the flips are given up: six frames at 60 Hz. */
#define I915_XD9_FLIP_LATCH_MS		100U

/* The scratch pages the display window keeps on each side of a buffer (the scanout buffer's VT-d guard). */
#define I915_XD9_FLIP_GUARD_PAGES	168U

/* The pages a buffer's 256 KiB alignment may skip in the display window. */
#define I915_XD9_FLIP_ALIGN_PAGES	64U

/*
 * ==== The arithmetic (host-tested) ====
 */

/* Reports the display window pages two buffers of the given size need. */
unsigned drv_i915_xd9_flip_window_pages(uint32_t buffer_bytes);

/*
 * ==== The node (bootfb.c calls these under the display's lease mutex) ====
 */

/* Reads the firmware's display and makes the two buffers; 0 when the flips are used. */
int drv_i915_xd9_flip_start(struct i915_device *device, uint64_t fb_physical);

/* Reports the extent and the refresh the flips show. */
void drv_i915_xd9_flip_extent(uint32_t *width, uint32_t *height, uint32_t *refresh_millihz);

/* Describes the flipped panel for the external outputs: 0, or ENOENT without flips or a measured pixel clock. */
int drv_i915_xd9_flip_panel(struct i915_xd9_panel *panel);

/* Readies the buffers and the flip interrupt for a new lease. */
void drv_i915_xd9_flip_claim(struct i915_device *device);

/* Draws a frame into the buffer the plane does not show and flips to it (see drv_i915_xd9_flip_failed for its errors). */
int drv_i915_xd9_flip_present(struct i915_device *device, void *session, void *object, const struct gpu_display_present *request);

/* Waits until the newest flip has latched; reports the tick it latched at. */
int drv_i915_xd9_flip_wait(uint64_t *latched_tick);

/* Reports nonzero when the error a presentation or a wait just returned was the flips' own. */
int drv_i915_xd9_flip_failed(void);

/* Checks after a sleep in D0 that the flipped panel still runs, and logs it. */
void drv_i915_xd9_flip_resume(struct i915_device *device);

/* Gives the plane back the firmware's surface at the end of a lease. */
int drv_i915_xd9_flip_release(struct i915_device *device);

/* Gives the flips up for the rest of the boot after a flip did not latch. */
void drv_i915_xd9_flip_stop(struct i915_device *device);

/* Frees the buffers once the plane is shown not to read them. */
void drv_i915_xd9_flip_fini(struct i915_device *device);

/* Acknowledges the flip's pipe interrupt and wakes the waiter; runs in the interrupt handler. */
void drv_i915_xd9_flip_irq(uint32_t master_ctl);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD9_FLIP_H */
