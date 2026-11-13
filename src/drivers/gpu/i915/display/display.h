/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display part of the device start and stop (display.c).
 *
 * The device start calls these steps between its own:
 *
 *   drv_i915_display_start          before the interrupts: the display, its
 *                                   tables, the firmware check, the core
 *   drv_i915_display_irq_prepare    the PCH and the display interrupt half,
 *                                   before the interrupt install
 *   drv_i915_display_outputs        after the install: the outputs and the
 *                                   correction of the firmware's leftovers
 *   drv_i915_display_serve          after the GT: the hotplug path, the
 *                                   Type-C ports, the references given back
 *
 * The device stop calls drv_i915_display_stop_serving before the GT stop,
 * drv_i915_display_stop_outputs after it, drv_i915_display_stop after the
 * interrupt uninstall and drv_i915_display_destroy last.  The display
 * object is owned by the device (device->display); every function takes
 * the device and does nothing for a device without a display.  The header
 * is neutral: the device start and the GPU node include it without the
 * display's types.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_DISPLAY_H
#define DRIVERS_GPU_I915_DISPLAY_DISPLAY_H

#include <stdint.h>

struct drv_gpu_ops;
struct drv_pci_device;
struct i915_device;
struct i915_display;
struct i915_pch_state;

/*
 * ==== The device start ====
 */

/* Starts the display before the interrupts: 0, or an error with device->stage naming the failed step. */
int drv_i915_display_start(struct i915_device *device);

/* Frees the display, unless its resources are retained. */
void drv_i915_display_destroy(struct i915_device *device);

/* Reports the display topology's sequence (the display events operation). */
int drv_i915_display_events(void *device, void *session, uint64_t *sequence);

/* Tells the GPU core that the display topology's sequence moved; no lock of the sequence may be held. */
void drv_i915_display_changed(struct i915_device *device);
int drv_i915_display_resident_identity(struct i915_display *display, uint32_t *display_id, uint64_t *generation, int *connected);

/* Tells whether a display ID and generation name the resident output or another connector: 0, ENOENT or ESTALE. */
int drv_i915_display_which(struct i915_display *display, uint32_t display_id, uint64_t generation, int *resident, int *connected);

void drv_i915_display_output_back(struct i915_device *device);
void drv_i915_display_output_fail_back(struct i915_device *device);
void drv_i915_display_output_lost(struct i915_device *device);
int drv_i915_display_output_moved(const struct i915_display *display);

/* Detects the PCH and binds the display interrupt half in place of the interim hooks (before install). */
void drv_i915_display_irq_prepare(struct i915_device *device);

/* Logs the display interrupt masks the install computed. */
void drv_i915_display_irq_log(struct i915_device *device);

/* Builds the outputs and corrects what the firmware left unused (after the interrupt install). */
int drv_i915_display_outputs(struct i915_device *device);

/* Starts the hotplug path and the Type-C ports and gives the start's references back (after the GT). */
int drv_i915_display_serve(struct i915_device *device);

/* Fills the panel dependencies of the resident run; the node has a panel from here on. */
void drv_i915_display_resident_deps(struct i915_device *device);

/*
 * ==== The device stop ====
 */

/* Stops anything still scanned out and the hotplug path, and takes the references back (before the GT stop). */
void drv_i915_display_stop_serving(struct i915_device *device);
void drv_i915_display_suspend_begin(struct i915_device *device);
int drv_i915_display_suspend_end(struct i915_device *device);
int drv_i915_display_resume_begin(struct i915_device *device);
void drv_i915_display_resume_end(struct i915_device *device);

/* Releases the panel connector and the crtc, plane, DPLL and encoder records (before the interrupt uninstall). */
void drv_i915_display_stop_outputs(struct i915_device *device);

/* Takes the rest of the display apart in reverse order (after the interrupt uninstall). */
void drv_i915_display_stop(struct i915_device *device);

/* Reports whether a run kept the display's resources: DMA, scratch, BARs and bus mastering stay. */
int drv_i915_display_abandoned(struct i915_device *device);

/* Reports whether a run kept GPU objects the GPU was not shown to be done with. */
int drv_i915_display_gpu_retained(struct i915_device *device);

/* Reports whether a pipe's interrupt drain failed: the handler stays attached. */
int drv_i915_display_irq_sync_failed(struct i915_device *device);

/*
 * ==== The GPU node ====
 */

/* Binds the display and scanout operations and their capabilities when the node has a panel. */
void drv_i915_display_bind_ops(struct i915_device *device, struct drv_gpu_ops *ops);

/* Drops the display lease a closing session holds (the panel is stopped first). */
void drv_i915_display_session_close(struct i915_device *device, void *session);

/* Reads the panel's mode: 0, or ENXIO when the node has no panel. */
int drv_i915_display_panel(struct i915_display *display, uint32_t *width, uint32_t *height, uint32_t *refresh_millihz);

/*
 * ==== The PCH ====
 */

void drv_i915_detect_pch(struct i915_display *display, struct i915_pch_state *p, int display_ver, int is_alderlake, int has_display, int run_as_guest);

#endif /* DRIVERS_GPU_I915_DISPLAY_DISPLAY_H */
