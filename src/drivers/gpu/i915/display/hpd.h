/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display's connectors and their hotplug (hpd.c).
 *
 * The connectors are made from the display's ports and their pins armed
 * when the display is probed; the start opens the interrupt entry and the
 * works that take the pulses and detect the connectors; the stop closes
 * them.  The display inventory and the display sessions read the
 * connectors' state and the topology's sequence from here.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_HPD_H
#define DRIVERS_GPU_I915_DISPLAY_HPD_H

#include <stdint.h>

struct i915_display;

/* A connector's kind as the display inventory sees it. */
#define I915_HPD_OUTPUT_OTHER	0U
#define I915_HPD_OUTPUT_EDP	1U
#define I915_HPD_OUTPUT_HDMI	2U
#define I915_HPD_OUTPUT_DP	3U

/*
 * One connector as the display inventory sees it: its kind, port, name,
 * whether a sink is connected, its generation (which moves with every
 * change the sessions are told of) and its sink's preferred mode and size.
 */
struct i915_hpd_output {
	unsigned kind;
	int port;
	int connected;
	uint64_t generation;
	const char *name;
	uint32_t width;
	uint32_t height;
	uint32_t refresh_millihz;
	uint32_t width_mm;
	uint32_t height_mm;
};

/* Whether the hotplug runs, how many connectors it has, and which is the first HDMI one (-1 for none). */
struct i915_hpd_summary {
	int started;
	unsigned num_connectors;
	int hdmi_connector;
};

int drv_i915_hpd_world_create(struct i915_display *display);
void drv_i915_hpd_world_destroy(struct i915_display *display);
void drv_i915_hpd_arm(struct i915_display *display, int interrupts);
int drv_i915_hpd_start(struct i915_display *display);
void drv_i915_hpd_pch_irq(struct i915_display *display, uint32_t sde_iir);
void drv_i915_hpd_de_irq(struct i915_display *display, uint32_t de_hpd_iir);
void drv_i915_hpd_suspend(struct i915_display *display);
void drv_i915_hpd_resume(struct i915_display *display);
void drv_i915_hpd_stop(struct i915_display *display);
int drv_i915_hpd_probe_connector(struct i915_display *display, unsigned idx);
void drv_i915_hpd_summary(struct i915_display *display, struct i915_hpd_summary *s);
uint64_t drv_i915_hpd_topology_sequence(struct i915_display *display);
void drv_i915_hpd_topology_touch(struct i915_display *display);
int drv_i915_hpd_output(struct i915_display *display, unsigned idx, struct i915_hpd_output *output);
unsigned drv_i915_hpd_output_count(struct i915_display *display);
void drv_i915_hpd_output_take(struct i915_display *display, unsigned idx);
const uint8_t *drv_i915_hpd_edid_bytes(struct i915_display *display, unsigned idx, unsigned *size);
int drv_i915_hpd_edid_copy(struct i915_display *display, unsigned idx, uint8_t *buffer, unsigned capacity, unsigned *length, unsigned *total);

#endif /* DRIVERS_GPU_I915_DISPLAY_HPD_H */
