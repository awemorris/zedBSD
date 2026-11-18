/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display device as a client of the VGA arbiter, and the legacy VGA
 * plane (vga.c).
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_VGA_H
#define DRIVERS_GPU_I915_DISPLAY_VGA_H

#include "internal.h"

struct drv_pci_device;

/* The arbiter client: the host bridge's decode, the registration and its end. */
int drv_i915_vga_register(struct i915_vga_client *c, struct drv_pci_device *gpu, unsigned display_ver, struct i915_trace *trace);
void drv_i915_vga_unregister(struct i915_vga_client *c);

/* The legacy VGA I/O: a host test's accessor, and the reset of the Misc Output register. */
void drv_i915_vga_reset_io_mem(struct i915_vga_client *c);

/* Switches the VGA plane off: 0, 1 when it already was, or EIO. */
int drv_i915_vga_disable(struct i915_display *display);

#endif /* DRIVERS_GPU_I915_DISPLAY_VGA_H */
