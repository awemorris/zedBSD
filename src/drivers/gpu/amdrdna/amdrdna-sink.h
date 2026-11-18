/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sink of a display output: its EDID's decode (edid.c, pure) and the
 * timings it lists.
 */

#ifndef DRIVERS_GPU_AMDRDNA_AMDRDNA_SINK_H
#define DRIVERS_GPU_AMDRDNA_AMDRDNA_SINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The bytes of an EDID block, the blocks the driver reads at most, and the modes it lists at most. */
#define AMDRDNA_EDID_BLOCK_BYTES	128U
#define AMDRDNA_EDID_BLOCKS		4U
#define AMDRDNA_EDID_MODES		32U

/* Where a listed mode came from: the preferred detailed timing, another detailed timing, an established or a standard timing. */
#define AMDRDNA_TIMING_PREFERRED	1U
#define AMDRDNA_TIMING_DETAILED		2U
#define AMDRDNA_TIMING_ESTABLISHED	3U
#define AMDRDNA_TIMING_STANDARD		4U

/*
 * One timing: the pixel clock in kHz (0 when unknown), the active,
 * blanking, front porch and sync of a line in pixels and of a frame in
 * lines, the image size in mm, whether it is interlaced and the sync
 * polarities, the refresh in millihertz, and where it came from.
 */
struct amdrdna_timing {
	uint32_t pixel_khz;
	uint16_t width;
	uint16_t h_blanking;
	uint16_t h_porch;
	uint16_t h_pulse;
	uint16_t height;
	uint16_t v_blanking;
	uint16_t v_porch;
	uint16_t v_pulse;
	uint16_t width_mm;
	uint16_t height_mm;
	bool interlaced;
	bool h_positive;
	bool v_positive;
	uint32_t refresh_millihz;
	uint8_t source;
};

/*
 * What an EDID says: the blocks that were whole, the extensions the base
 * block announces, the vendor letters, product code and serial number,
 * the version and revision, a digital input, the screen size in mm, the
 * display name, the range limits' highest pixel clock in kHz (0 when not
 * given), and the modes.
 */
struct amdrdna_edid {
	unsigned blocks;
	unsigned extensions;
	char vendor[4];
	uint16_t product;
	uint32_t serial;
	uint8_t version;
	uint8_t revision;
	bool digital;
	uint32_t width_mm;
	uint32_t height_mm;
	char name[14];
	uint32_t max_pixel_khz;
	struct amdrdna_timing modes[AMDRDNA_EDID_MODES];
	unsigned mode_count;
};

int
amdrdna_edid_decode(
	const uint8_t *bytes,
	size_t length,
	struct amdrdna_edid *edid);

unsigned
amdrdna_edid_find(
	const struct amdrdna_edid *edid,
	uint32_t width,
	uint32_t height,
	uint32_t refresh_millihz);

#endif
