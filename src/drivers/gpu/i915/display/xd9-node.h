/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The external displays of a display 9 node (xd9-node.c).
 *
 * While the node's step takes the mode set (xd9-step.h), and the panel flipped
 * (xd9-flip.c), a node offers a display for every sink attached to ports
 * B to D beside the panel's display: display id 1 + port, one plane, the
 * mode chosen from the sink's EDID.  A claim lights the output on a free
 * pipe (xd9-outputs.c) showing black; a presentation is drawn by the GPU
 * into the buffer the plane does not show and flipped to; the release
 * darkens it.  A work reads the ports' hotplug state every second: a
 * sink that comes or goes moves the display events' sequence, and a lit
 * output whose sink went is darkened and its lease ended.  A mode set or a
 * flip of an output that fails, or the panel's flips failing, withdraws
 * every external display for the rest of the boot (xd9-step.h).
 *
 * bootfb.c, the panel's display, hands the operations on these display
 * ids and leases to the functions here.  None of them is called with the
 * panel's lease mutex held: the node takes its own mutex first, then the
 * panel's while it moves the panel's watermarks.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD9_NODE_H
#define DRIVERS_GPU_I915_DISPLAY_XD9_NODE_H

#include <stdint.h>

struct gpu_display_claim;
struct gpu_display_info;
struct gpu_display_mode;
struct gpu_display_present;
struct gpu_display_release;
struct gpu_display_wait;
struct i915_device;

/* Starts the external displays when the boot line asks for the mode set and the panel is flipped. */
void drv_i915_xd9_node_start(struct i915_device *device);

/* Darkens every external display and stops the hotplug work. */
void drv_i915_xd9_node_stop(struct i915_device *device);

/* Reports how many external displays have a sink now (0 when the node is not started). */
uint32_t drv_i915_xd9_node_count(void);

/* Reports whether a display id or a lease is an external display's. */
int drv_i915_xd9_node_owns_display(uint32_t display_id);
int drv_i915_xd9_node_owns_lease(uint64_t lease);

/* Reports the display events' sequence, or 0 when the node is not started. */
uint64_t drv_i915_xd9_node_sequence(void);

/* The display operations on an external display (index counts the external displays with a sink from 0). */
int drv_i915_xd9_node_query(void *device, uint32_t index, struct gpu_display_info *request);
int drv_i915_xd9_node_mode(void *device, void *session, struct gpu_display_mode *request);
int drv_i915_xd9_node_claim(void *device, void *session, struct gpu_display_claim *request);
int drv_i915_xd9_node_release(void *device, void *session, const struct gpu_display_release *request);
int drv_i915_xd9_node_present(void *device, void *session, void *object, struct gpu_display_present *request);
int drv_i915_xd9_node_wait(void *device, void *session, struct gpu_display_wait *request);

/* Ends the leases a closing session still holds. */
void drv_i915_xd9_node_session_close(struct i915_device *device, void *session);

/*
 * Darkens the lit external displays for a sleep, keeping their leases and
 * buffers, and lights them again after it on the surface they showed
 * (the device stays in D0, so nothing else is lost).
 */
void drv_i915_xd9_node_suspend(struct i915_device *device);
void drv_i915_xd9_node_resume(struct i915_device *device);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD9_NODE_H */
