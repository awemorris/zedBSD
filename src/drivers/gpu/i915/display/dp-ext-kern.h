/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display's external DP ports (dp-ext-kern.c): the Type-C ports bound
 * to the DP environment's AUX transfer, and the probe of their sinks that
 * the hotplug path's DP detection asks.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_DP_EXT_KERN_H
#define DRIVERS_GPU_I915_DISPLAY_DP_EXT_KERN_H

#include "internal.h"
#include "dp-ext.h"

void drv_i915_dp_ext_start(struct i915_display *display);
int drv_i915_dp_ext_pulse(struct i915_display *display, int port);
enum i915_dp_ext_status drv_i915_dp_ext_probe(struct i915_display *display, int port, uint8_t *edid, size_t edid_size, unsigned *edid_bytes);

/*
 * Copies what the last probe of an external DP port (the Type-C port of a
 * DDI port) found: 0, or ENXIO when the ports are not bound or the port is
 * not a declared Type-C port.
 */
int drv_i915_dp_ext_sink_copy(struct i915_display *display, int port, struct i915_dp_ext_sink *sink);

/*
 * Reports the link limits a failed link training left on an external DP
 * port (0: the sink's own): 0, or ENXIO for a port that is not one.
 */
int drv_i915_dp_ext_link_limits(struct i915_display *display, int port, int *max_rate, int *max_lanes);

/*
 * Lowers an external DP port's link limits after a link training failed
 * at a rate and lane count (the Linux
 * intel_dp_get_link_train_fallback_values()): 0, ENOSPC when no lower
 * link is left, or ENXIO for a port that is not one.
 */
int drv_i915_dp_ext_link_fallback(struct i915_display *display, int port, int rate, int lanes);

/* Gives an external DP port its sink's own link limits back (a long hot plug pulse). */
void drv_i915_dp_ext_link_reset(struct i915_display *display, int port);

/*
 * Tells whether the resident output's trained link on an external DP port
 * must be trained again (ws051-p005b): 1 when the port's DisplayPort
 * display is the lit resident output and its sink's link status lost
 * alignment or a lane's lock, 0 otherwise.
 */
int drv_i915_dp_ext_link_check(struct i915_display *display, int port);

/*
 * Gives the DPCD access of an external DP port's sink (the Type-C port of
 * a DDI port) for a modeset object of that sink: NULL when the external
 * ports are not bound or the port is not a declared Type-C port.  It
 * stays valid for the device's lifetime and refuses once the ports stop.
 */
const struct i915_lcd_aux_emit *drv_i915_dp_ext_aux_emit(struct i915_display *display, int port);

#endif
