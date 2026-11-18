/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Whether a link carries a timing: the signal between a display controller's
 * output and the sink (DVI, HDMI, DisplayPort, an embedded panel, or none
 * for a virtual output), its limits on the source's side, and what the
 * sink's EDID says of its own.
 */

#ifndef DRIVERS_GPU_DISPLAY_LINK_H
#define DRIVERS_GPU_DISPLAY_LINK_H

#include <stdint.h>

#include "drivers/gpu/display/edid.h"
#include "drivers/gpu/display/timing.h"

/* gpu_link_caps.kind: the signal the source's output sends. */
#define GPU_LINK_DVI			1U
#define GPU_LINK_HDMI			2U
#define GPU_LINK_DP			3U
#define GPU_LINK_EDP			4U
#define GPU_LINK_VIRTUAL		5U

/* gpu_link_caps.flags. */
#define GPU_LINK_DUAL_LINK		1U	/* the DVI output and its connector have the second link */
#define GPU_LINK_SCRAMBLING		2U	/* the HDMI output scrambles TMDS above 340 MHz */

/* The TMDS clocks of the standards, in kHz: DVI single link, its dual link, HDMI without scrambling. */
#define GPU_LINK_DVI_SINGLE_KHZ		165000U
#define GPU_LINK_DVI_DUAL_KHZ		330000U
#define GPU_LINK_HDMI_KHZ		340000U

/* What gpu_link_check answers: the timing fits, or the first limit it breaks. */
#define GPU_LINK_FITS			0U
#define GPU_LINK_NO_LINK		1U	/* the caps name no known signal, or a DP link with no lane */
#define GPU_LINK_TMDS_SOURCE		2U	/* above the source's TMDS limit (DVI, HDMI) */
#define GPU_LINK_TMDS_SINK		3U	/* above the sink's TMDS limit (its HDMI blocks) */
#define GPU_LINK_DP_BANDWIDTH		4U	/* more bits than the DP lanes carry */
#define GPU_LINK_PANEL			5U	/* an embedded panel's timing it did not name in detail */
#define GPU_LINK_DVI_CLOCK		6U	/* above the DVI link's clock (a DVI sink, or a DVI output) */

/*
 * What one output's link can carry, as its driver knows it.
 *
 * The driver fills it for each output (from the connector, the encoder
 * and, for DisplayPort, the receiver's capabilities) and passes it by
 * pointer; the shared code only reads it.  A zero limit is no limit, except
 * for the DisplayPort lanes and symbol clock, which a DP link needs.
 */
struct gpu_link_caps {
	uint32_t kind;
	uint32_t flags;

	/* The source's TMDS limit in kHz (DVI, HDMI): 165000, 340000, or up to 600000 with scrambling. */
	uint32_t tmds_max_khz;

	/* DisplayPort: the lanes both ends take, and the symbol clock of one lane in kHz (162000, 270000, 540000, 810000). */
	uint32_t dp_lanes;
	uint32_t dp_symbol_khz;

	/* The controller's limits at its present clocks: the pixel clock in kHz, and the size. */
	uint32_t pixel_max_khz;
	uint32_t width_max;
	uint32_t height_max;
};

uint32_t gpu_link_check(const struct gpu_link_caps *caps, const struct gpu_edid *edid, const struct gpu_display_timing *timing);

#endif
