/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decode of a sink's EDID, shared by the display controllers' drivers:
 * the identity, the size, the range limits, what the CTA-861 extensions say
 * of HDMI, and every progressive timing the sink names, in the EDID's order.
 */

#ifndef DRIVERS_GPU_DISPLAY_EDID_H
#define DRIVERS_GPU_DISPLAY_EDID_H

#include <stdint.h>

#include "drivers/gpu/display/timing.h"

/* The bytes of one EDID block, and the blocks a decode reads at most. */
#define GPU_EDID_BLOCK_BYTES		128U
#define GPU_EDID_BLOCKS_MAX		4U

/* The timings a decode keeps at most; the rest are dropped in the EDID's order. */
#define GPU_EDID_TIMINGS_MAX		64U

/* The characters of a display descriptor's text, and the room for it with its end. */
#define GPU_EDID_TEXT_BYTES		14U

/*
 * What one EDID says, decoded.
 *
 * A decode fills the whole structure; the caller owns it.  It is large (a
 * few kilobytes), so a kernel caller allocates it rather than putting it on
 * the stack.
 */
struct gpu_edid {
	/* The blocks decoded (the base block and the good extensions), and the extensions dropped for a bad checksum. */
	uint32_t blocks;
	uint32_t bad_blocks;

	/* The identity: the three vendor letters, the product code, the serial number and its text (0xff descriptor). */
	char vendor[4];
	uint32_t product;
	uint32_t serial;
	char serial_text[GPU_EDID_TEXT_BYTES];
	char name[GPU_EDID_TEXT_BYTES];

	/* The version and revision, a digital input, and the screen size in mm (0 when not given). */
	uint32_t version;
	uint32_t revision;
	uint32_t digital;
	uint32_t width_mm;
	uint32_t height_mm;

	/*
	 * The range limits descriptor (0xfd), when has_range: the vertical rate
	 * in hertz, the horizontal rate in kHz, and the highest pixel clock in
	 * kHz (0 when not given).
	 */
	uint32_t has_range;
	uint32_t v_min_hz;
	uint32_t v_max_hz;
	uint32_t h_min_khz;
	uint32_t h_max_khz;
	uint32_t range_pixel_max_khz;

	/*
	 * HDMI: hdmi when a CTA-861 extension has the HDMI vendor block (a sink
	 * without it is a DVI sink), the highest TMDS clock that block gives in
	 * kHz (0 when not given), and from the HDMI Forum block the highest
	 * TMDS character rate in kHz (0 when not given) and whether the sink
	 * has the status and control channel that scrambling needs.
	 */
	uint32_t cta;
	uint32_t hdmi;
	uint32_t hdmi_tmds_max_khz;
	uint32_t forum_tmds_max_khz;
	uint32_t scdc;

	/* The first detailed timing of the base block, which is the preferred timing, when has_preferred. */
	uint32_t has_preferred;
	struct gpu_display_timing preferred;

	/* Every progressive timing the sink names, in the EDID's order, without repeats. */
	uint32_t timing_count;
	struct gpu_display_timing timings[GPU_EDID_TIMINGS_MAX];
};

int gpu_edid_decode(const uint8_t *bytes, uint32_t length, struct gpu_edid *edid);

#endif
