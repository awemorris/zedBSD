/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Display timings shared by the display controllers' drivers: one progressive
 * timing, the refresh it gives, and the timings that the standards name by
 * size and rate (the VESA monitor timings, the CTA-861 video formats and the
 * CVT reduced-blanking formula).
 *
 * Everything here is a pure function of its arguments: no lock, no
 * allocation, no hardware.
 */

#ifndef DRIVERS_GPU_DISPLAY_TIMING_H
#define DRIVERS_GPU_DISPLAY_TIMING_H

#include <stdint.h>

/* gpu_display_timing.flags: the sync polarities. */
#define GPU_DISPLAY_TIMING_H_POSITIVE	1U
#define GPU_DISPLAY_TIMING_V_POSITIVE	2U

/* gpu_display_timing.source: where the sink named the timing. */
#define GPU_DISPLAY_TIMING_DETAILED	1U	/* a detailed timing of the base block */
#define GPU_DISPLAY_TIMING_CTA_DETAILED	2U	/* a detailed timing of a CTA-861 extension */
#define GPU_DISPLAY_TIMING_CTA_FORMAT	3U	/* a video format (VIC) of a CTA-861 extension */
#define GPU_DISPLAY_TIMING_STANDARD	4U	/* a standard timing (size and rate) */
#define GPU_DISPLAY_TIMING_ESTABLISHED	5U	/* an established timing bit */

/*
 * Two refreshes closer than this, in millihertz, are the same rate: 59.94 and
 * 60.00 Hz differ, 60.000 and 60.020 Hz do not.
 */
#define GPU_DISPLAY_TIMING_SAME_MHZ	50U

/*
 * One progressive timing of an output: the pixel clock, and the active area,
 * front porch, sync width and back porch of a line and of a frame.
 *
 * A timing is a value: it is copied, never shared, and holds no pointer.
 */
struct gpu_display_timing {
	uint32_t pixel_khz;
	uint32_t h_active;
	uint32_t h_front;
	uint32_t h_sync;
	uint32_t h_back;
	uint32_t v_active;
	uint32_t v_front;
	uint32_t v_sync;
	uint32_t v_back;
	uint32_t flags;
	uint32_t source;

	/* The refresh the clock and totals give, in millihertz (gpu_display_timing_refresh). */
	uint32_t refresh_mhz;
};

uint32_t gpu_display_timing_refresh(const struct gpu_display_timing *timing);
int gpu_display_timing_same_rate(uint32_t first_mhz, uint32_t second_mhz);
int gpu_display_timing_monitor(uint32_t width, uint32_t height, uint32_t refresh_hz, struct gpu_display_timing *timing);
int gpu_display_timing_format(uint32_t format, struct gpu_display_timing *timing);
int gpu_display_timing_reduced(uint32_t width, uint32_t height, uint32_t refresh_hz, struct gpu_display_timing *timing);

#endif
