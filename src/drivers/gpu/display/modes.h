/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The modes one output can show: the sink's timings that its EDID's range,
 * its link and its controller all take, without repeats, the default first.
 */

#ifndef DRIVERS_GPU_DISPLAY_MODES_H
#define DRIVERS_GPU_DISPLAY_MODES_H

#include <stdint.h>

#include "drivers/gpu/display/edid.h"
#include "drivers/gpu/display/link.h"
#include "drivers/gpu/display/timing.h"

/* The modes a list holds at most (every timing a decode keeps). */
#define GPU_MODES_MAX			GPU_EDID_TIMINGS_MAX

/* The lowest refresh listed (23.976 Hz film stays), and the lowest a default is chosen at, in millihertz. */
#define GPU_MODES_RATE_MIN_MHZ		23000U
#define GPU_MODES_DEFAULT_MIN_MHZ	50000U

/* What gpu_modes_check answers: the timing is listed, or the first reason it is not. */
#define GPU_MODES_FITS			0U
#define GPU_MODES_SLOW			1U	/* below GPU_MODES_RATE_MIN_MHZ */
#define GPU_MODES_RANGE			2U	/* outside the EDID's range limits */
#define GPU_MODES_CLOCK			3U	/* above the controller's pixel clock */
#define GPU_MODES_SIZE			4U	/* larger than the controller shows */
#define GPU_MODES_DRIVER		5U	/* the driver's own check refused it */
#define GPU_MODES_LINK			16U	/* GPU_MODES_LINK plus the GPU_LINK_ code: the link refused it */

/*
 * A driver's own check of a timing, for limits the shared code does not
 * know (clock dividers, buffer sizes, a fixed table of modes): 0 takes the
 * timing, anything else refuses it.  It must not sleep for long; it is
 * called once per timing while the list is built.
 */
typedef int (*gpu_modes_fits_fn)(void *context, const struct gpu_display_timing *timing);

/*
 * The modes of one output, built for one output generation.
 *
 * modes[0] is the default when count is not zero; the rest follow by
 * pixels, largest first, then by refresh, highest first.  The driver owns
 * the list (it is a few kilobytes: allocate it) and keeps it under its
 * display lock until the output's next generation.
 */
struct gpu_mode_list {
	uint32_t count;
	struct gpu_display_timing modes[GPU_MODES_MAX];
};

uint32_t gpu_modes_check(const struct gpu_edid *edid, const struct gpu_link_caps *caps, gpu_modes_fits_fn fits, void *context, const struct gpu_display_timing *timing);
int gpu_modes_build(const struct gpu_edid *edid, const struct gpu_link_caps *caps, gpu_modes_fits_fn fits, void *context, struct gpu_mode_list *list);
int gpu_modes_find(const struct gpu_mode_list *list, uint32_t width, uint32_t height, uint32_t refresh_mhz, uint32_t *index);

#endif
