/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The boot framebuffer display of a Gen9 node (bootfb.c).
 *
 * The mode of the display controller of Gen9 and Gen9.5 is the firmware's:
 * the firmware's picture stays on the screen and the node has no panel.
 * The node offers one display on the firmware's framebuffer instead, so the
 * desktop can be shown at the resolution the firmware chose.  Every
 * presentation is copied by the GPU into a staging area of RAM and from
 * there by the CPU into the framebuffer.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_BOOTFB_H
#define DRIVERS_GPU_I915_DISPLAY_BOOTFB_H

#include <stdint.h>

struct drv_gpu_ops;
struct i915_device;
struct kern_boot_framebuffer;

/* The refresh the display reports: the firmware's mode is not known, and 60 Hz is what laptop panels run at. */
#define I915_BOOTFB_REFRESH_MILLIHZ	60000U

/* A staging row starts on a 64-byte boundary, as a linear render target's does. */
#define I915_BOOTFB_STRIDE_ALIGN	64U

/* The largest framebuffer side the display takes (the scanout layout's limit). */
#define I915_BOOTFB_MAX_SIDE		8192U

/*
 * What the firmware's framebuffer is, as the display uses it.
 *
 * Filled from the boot handoff by drv_i915_bootfb_layout(); every field
 * is in bytes unless it says pixels.
 */
struct i915_bootfb_layout {
	/* The visible extent in pixels, and the bytes from one row to the next. */
	uint32_t width;
	uint32_t height;
	uint32_t fb_stride_bytes;

	/* Nonzero when a pixel is R, G, B, X in memory; zero for B, G, R, X. */
	int rgbx;

	/* The physical start and the bytes the visible rows span. */
	uint64_t fb_physical;
	uint64_t fb_bytes;

	/* The staging area the GPU copies a frame into: its row bytes and its size. */
	uint32_t staging_stride_bytes;
	uint64_t staging_bytes;
};

/*
 * ==== The GPU node (display.c calls these) ====
 */

/* Binds the boot framebuffer display when the node is a Gen9 one without a display. */
void drv_i915_bootfb_bind_ops(struct i915_device *device, struct drv_gpu_ops *ops);

/* Reports nonzero when the node's display is the boot framebuffer display. */
int drv_i915_bootfb_bound(struct i915_device *device);

/* Ends the lease a closing session still holds. */
void drv_i915_bootfb_session_close(struct i915_device *device, void *session);

/* Frees the staging area and the framebuffer's mapping once nothing maps them. */
void drv_i915_bootfb_fini(struct i915_device *device);

/*
 * ==== The arithmetic (host-tested) ====
 */

/* Checks the boot handoff's framebuffer and lays out the display's use of it. */
int drv_i915_bootfb_layout(const struct kern_boot_framebuffer *fb, struct i915_bootfb_layout *layout);

/* Clips a presented frame to the framebuffer; reports the extent that is copied. */
int drv_i915_bootfb_clip(const struct i915_bootfb_layout *layout, uint32_t width, uint32_t height, uint32_t *copy_width, uint32_t *copy_height);

/* Reports nonzero when a frame of the presented format needs its red and blue swapped on the framebuffer. */
int drv_i915_bootfb_swap(const struct i915_bootfb_layout *layout, uint32_t format);

/* Copies one row of pixels to the framebuffer, swapping red and blue when asked. */
void drv_i915_bootfb_copy_row(volatile uint32_t *destination, const uint32_t *source, uint32_t columns, int swap);

#endif /* DRIVERS_GPU_I915_DISPLAY_BOOTFB_H */
