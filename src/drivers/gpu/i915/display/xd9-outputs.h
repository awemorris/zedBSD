/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The external outputs of a display 9 display beside the panel the
 * firmware lit (xd9-outputs.c): DDIs B to D, each found by its hotplug
 * sense, probed for a DisplayPort sink over AUX or an HDMI one over the
 * GMBUS, given a mode from its EDID, and lit on a pipe and a DPLL neither
 * the panel nor another output uses, with the display buffer moved so
 * that no two lit pipes' parts overlap.
 *
 * The register sequences are the xd code's (xd-output.c, xd-dbuf.c,
 * xd-hpd.c); the node (xd9-node.c) calls these under its mutex, and the
 * host tests on a model of the hardware.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD9_OUTPUTS_H
#define DRIVERS_GPU_I915_DISPLAY_XD9_OUTPUTS_H

#include <stdint.h>

#include "display-internal.h"
#include "xd-dbuf.h"
#include "xd-ddi.h"
#include "xd-io.h"
#include "xd-output.h"
#include "xd-platform.h"

/* The external DDIs: B, C and D, kept at index ddi - 1. */
#define I915_XD9_OUTPUTS		3U

/* The pipes display 9 has. */
#define I915_XD9_PIPES			3U

/* The largest mode an external output shows (the display window holds two buffers of it per output). */
#define I915_XD9_OUTPUT_MAX_WIDTH	4096U
#define I915_XD9_OUTPUT_MAX_HEIGHT	2160U

/* The bytes of EDID kept: the base block and one extension. */
#define I915_XD9_OUTPUT_EDID_BYTES	256U

/*
 * The panel the firmware lit, as the external outputs share the display
 * with it: its pipe, DDI and DPLL, and its plane's width, line total and
 * pixel clock, which its watermarks are computed from.
 */
struct i915_xd9_panel {
	unsigned pipe;
	unsigned ddi;
	unsigned dpll;
	uint32_t hdisplay;
	uint32_t htotal;
	uint32_t pixel_khz;
};

/*
 * One external DDI: whether a sink is attached, its kind (0 for none,
 * I915_XD9_OUTPUT_DP or I915_XD9_OUTPUT_HDMI) and the generation of its
 * attachment; its EDID and the mode chosen from it; a DisplayPort sink's
 * port (with its capabilities) and link limits; and whether it is lit,
 * with what.
 */
struct i915_xd9_ext {
	unsigned ddi;
	int connected;
	unsigned kind;
	uint64_t generation;
	uint8_t edid[I915_XD9_OUTPUT_EDID_BYTES];
	unsigned edid_bytes;
	struct i915_display_mode mode;
	struct i915_xd9_dp_port dp;
	uint32_t sink_max_khz;
	unsigned sink_lanes;
	int lit;
	struct i915_xd9_output out;
};

/*
 * The external outputs of one display: the registers, the platform's
 * row, the panel, the CD clock (kept as the firmware set it) and the
 * memory latencies, the DDIs the board has and those with a sink, the
 * last generation given, and each DDI.  Made by drv_i915_xd9_outputs_init()
 * and changed only through these functions, under the caller's lock.
 */
struct i915_xd9_outputs {
	struct i915_xd_io *io;
	const struct i915_xd_platform *platform;
	struct i915_xd9_panel panel;
	uint32_t cdclk_khz;
	uint16_t latency[I915_XD_WM_LEVELS];
	unsigned present;
	unsigned connected;
	uint64_t generation;
	struct i915_xd9_ext output[I915_XD9_OUTPUTS];
};

void drv_i915_xd9_wm_latency(uint32_t low, uint32_t high, int ipc, uint16_t latency[I915_XD_WM_LEVELS]);
int drv_i915_xd9_pick_mode(const uint8_t *edid, unsigned edid_bytes, uint32_t max_pixel_khz, struct i915_display_mode *mode);
void drv_i915_xd9_outputs_init(struct i915_xd9_outputs *outputs, struct i915_xd_io *io, const struct i915_xd_platform *platform, const struct i915_xd9_panel *panel, uint32_t cdclk_khz, const uint16_t latency[I915_XD_WM_LEVELS]);
unsigned drv_i915_xd9_outputs_poll(struct i915_xd9_outputs *outputs);
int drv_i915_xd9_outputs_light(struct i915_xd9_outputs *outputs, unsigned index, uint32_t surf, uint32_t stride_units, int rgbx);
void drv_i915_xd9_outputs_darken(struct i915_xd9_outputs *outputs, unsigned index);
void drv_i915_xd9_outputs_flip(struct i915_xd9_outputs *outputs, unsigned index, uint32_t surf);
uint32_t drv_i915_xd9_outputs_live(struct i915_xd9_outputs *outputs, unsigned index);
uint32_t drv_i915_xd9_ext_refresh(const struct i915_xd9_ext *output);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD9_OUTPUTS_H */
