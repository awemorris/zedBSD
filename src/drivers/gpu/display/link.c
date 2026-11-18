/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Whether one link carries one timing.  The answer comes from three sides:
 * the signal's own limit (DVI's link clock, HDMI's TMDS clock without
 * scrambling, DisplayPort's lane bandwidth), the source's limits as its
 * driver states them, and the sink's limits as its EDID states them.
 *
 * Only 8 bits per color RGB is considered: the TMDS clock equals the pixel
 * clock, and a DisplayPort pixel is 24 bits.
 */

#include "drivers/gpu/display/link.h"

#include <stddef.h>

/*
 * DisplayPort with 8b/10b coding moves 8 bits of data per lane at each
 * symbol clock; one 8-bit RGB pixel is 24 bits.
 *
 * Values referenced: VESA DisplayPort Standard, Version 1.4, section 3.5
 * (the main link's 8b/10b symbols and the link rates RBR, HBR, HBR2 and
 * HBR3).
 */
#define DP_BITS_PER_SYMBOL		8U
#define DP_BITS_PER_PIXEL		24U

static uint32_t link_dvi(const struct gpu_link_caps *caps, const struct gpu_display_timing *timing);
static uint32_t link_hdmi(const struct gpu_link_caps *caps, const struct gpu_edid *edid, const struct gpu_display_timing *timing);
static uint32_t link_dp(const struct gpu_link_caps *caps, const struct gpu_display_timing *timing);

/*
 * Checks whether an output's link carries a timing to its sink.
 *
 * A sink whose EDID has no HDMI vendor block (or that has no EDID) is a
 * DVI sink, so an HDMI output sends it DVI: a single link of at most
 * 165 MHz.  An HDMI sink takes up to 340 MHz, less when its HDMI block says
 * so, and more only when the output scrambles and the sink's HDMI Forum
 * block allows the rate and has the control channel.  A DisplayPort link
 * carries what its lanes carry.  An embedded panel takes only the timings
 * its EDID names in detail.  A virtual output takes anything.
 *
 * Values referenced: DVI 1.0, section 2.2.4 (a single link up to
 * 165 MHz, a dual link above); HDMI Specification 1.4b, section 6.1 (TMDS
 * up to 340 MHz); HDMI Specification 2.0, section 6.1 (TMDS character
 * rates above 340 MHz need scrambling and the status and control data
 * channel).
 *
 * Returns GPU_LINK_FITS, or the GPU_LINK_ code of the first limit broken.
 */
uint32_t
gpu_link_check(
	const struct gpu_link_caps *caps,
	const struct gpu_edid *edid,
	const struct gpu_display_timing *timing)
{
	uint32_t answer;

	/* Each signal has its own rule. */
	switch (caps->kind) {
	case GPU_LINK_VIRTUAL:
		/* A virtual output has no wire to limit it. */
		answer = GPU_LINK_FITS;
		break;
	case GPU_LINK_DVI:
		/* A DVI output. */
		answer = link_dvi(caps, timing);
		break;
	case GPU_LINK_HDMI:
		/* An HDMI output sends HDMI to an HDMI sink and DVI to any other. */
		if (edid == NULL || edid->hdmi == 0U) {
			answer = link_dvi(caps, timing);
		} else {
			answer = link_hdmi(caps, edid, timing);
		}
		break;
	case GPU_LINK_DP:
		/* An external DisplayPort sink. */
		answer = link_dp(caps, timing);
		break;
	case GPU_LINK_EDP:
		/* An embedded panel: only its own detailed timings, and only what the lanes carry. */
		if (timing->source != GPU_DISPLAY_TIMING_DETAILED &&
		    timing->source != GPU_DISPLAY_TIMING_CTA_DETAILED) {
			answer = GPU_LINK_PANEL;
		} else {
			answer = link_dp(caps, timing);
		}
		break;
	default:
		/* A signal this code does not know carries nothing. */
		answer = GPU_LINK_NO_LINK;
		break;
	}

	/* Reports the answer. */
	return answer;
}

/*
 * Checks a timing against a DVI link: the dual link only for a DVI output
 * that has it, the single link otherwise, and never above the source's own
 * TMDS limit.
 */
static uint32_t
link_dvi(
	const struct gpu_link_caps *caps,
	const struct gpu_display_timing *timing)
{
	uint32_t link_khz;

	/* The source's own TMDS limit comes first. */
	if (caps->tmds_max_khz != 0U && timing->pixel_khz > caps->tmds_max_khz)
		return GPU_LINK_TMDS_SOURCE;

	/* The link's clock: the second link only on a DVI output whose connector carries it. */
	link_khz = GPU_LINK_DVI_SINGLE_KHZ;
	if (caps->kind == GPU_LINK_DVI && (caps->flags & GPU_LINK_DUAL_LINK) != 0U)
		link_khz = GPU_LINK_DVI_DUAL_KHZ;
	if (timing->pixel_khz > link_khz)
		return GPU_LINK_DVI_CLOCK;

	/* The link carries the timing. */
	return GPU_LINK_FITS;
}

/* Checks a timing against an HDMI link to an HDMI sink. */
static uint32_t
link_hdmi(
	const struct gpu_link_caps *caps,
	const struct gpu_edid *edid,
	const struct gpu_display_timing *timing)
{
	uint32_t source_khz;

	/* The source's limit: 340 MHz at most unless it scrambles. */
	source_khz = caps->tmds_max_khz;
	if (source_khz == 0U)
		source_khz = GPU_LINK_HDMI_KHZ;
	if ((caps->flags & GPU_LINK_SCRAMBLING) == 0U && source_khz > GPU_LINK_HDMI_KHZ)
		source_khz = GPU_LINK_HDMI_KHZ;
	if (timing->pixel_khz > source_khz)
		return GPU_LINK_TMDS_SOURCE;

	/* Up to 340 MHz, the sink's HDMI block may set a lower limit. */
	if (timing->pixel_khz <= GPU_LINK_HDMI_KHZ) {
		if (edid->hdmi_tmds_max_khz != 0U && timing->pixel_khz > edid->hdmi_tmds_max_khz)
			return GPU_LINK_TMDS_SINK;
		return GPU_LINK_FITS;
	}

	/* Above 340 MHz the sink must allow the rate and have the channel scrambling is set up on. */
	if (edid->forum_tmds_max_khz < timing->pixel_khz)
		return GPU_LINK_TMDS_SINK;
	if (edid->scdc == 0U)
		return GPU_LINK_TMDS_SINK;

	/* The link carries the timing, scrambled. */
	return GPU_LINK_FITS;
}

/*
 * Checks a timing against a DisplayPort link: the lanes times the symbol
 * clock times 8 bits must cover the pixel clock times 24 bits.
 */
static uint32_t
link_dp(
	const struct gpu_link_caps *caps,
	const struct gpu_display_timing *timing)
{
	uint64_t link_bits;
	uint64_t pixel_bits;

	/* A link without lanes or a rate carries nothing. */
	if (caps->dp_lanes == 0U || caps->dp_symbol_khz == 0U)
		return GPU_LINK_NO_LINK;

	/* The data rate of the lanes, and the data rate of the pixels, both in kbit/s. */
	link_bits = (uint64_t)caps->dp_lanes * caps->dp_symbol_khz * DP_BITS_PER_SYMBOL;
	pixel_bits = (uint64_t)timing->pixel_khz * DP_BITS_PER_PIXEL;
	if (pixel_bits > link_bits)
		return GPU_LINK_DP_BANDWIDTH;

	/* The lanes carry the timing. */
	return GPU_LINK_FITS;
}
