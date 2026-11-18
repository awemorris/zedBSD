/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sink descriptions of the display 12 and 13 code (xd-edid.c): the
 * check of EDID blocks and the decode of what the display uses from an
 * EDID, for the built-in panel and for DisplayPort and HDMI sinks alike.
 *
 * The decode is pure: it reads the bytes a caller read over AUX or the
 * DDC bus and copies the values out.  The detailed timings are decoded
 * by the output code's own decoder (output.c), so the panel and the
 * external outputs read a timing the same way.
 */

#ifndef DRIVERS_GPU_I915_DISPLAY_XD_EDID_H
#define DRIVERS_GPU_I915_DISPLAY_XD_EDID_H

#include <stddef.h>
#include <stdint.h>

#include "display-internal.h"

/*
 * The EDID layout the code reads: the block size; in the base block the
 * header, the vendor and product id (ten bytes from the manufacturer to
 * the year, which the VBT's panel ids copy), the version, the video
 * input, the screen size in cm, the first of the four 18-byte
 * descriptors (detailed timings or display descriptors), the
 * extension count and the checksum; in a CTA-861 extension its tag, the
 * offset of its detailed timings, the support flags and the data blocks.
 *
 * Values referenced: VESA Enhanced Extended Display Identification Data
 * Standard (E-EDID) Release A, Revision 2, section 3 (the base block
 * layout), and CTA-861 (the extension's tag, header and data block
 * layout).
 */
#define I915_XD_EDID_BLOCK_BYTES	128U
#define I915_XD_EDID_HEADER_BYTES	8U
#define I915_XD_EDID_VENDOR		8U
#define I915_XD_EDID_VENDOR_BYTES	10U
#define I915_XD_EDID_PRODUCT		10U
#define I915_XD_EDID_VERSION		18U
#define I915_XD_EDID_REVISION		19U
#define I915_XD_EDID_INPUT		20U
#define I915_XD_EDID_INPUT_DIGITAL	0x80U
#define I915_XD_EDID_INPUT_DEPTH	0x70U
#define I915_XD_EDID_WIDTH_CM		21U
#define I915_XD_EDID_HEIGHT_CM		22U
#define I915_XD_EDID_FIRST_DTD		54U
#define I915_XD_EDID_DESCRIPTORS	4U
#define I915_XD_EDID_DESCRIPTOR_BYTES	18U
#define I915_XD_EDID_EXTENSIONS		126U
#define I915_XD_EDID_CHECKSUM		127U
#define I915_XD_EDID_CTA_TAG		0x02U
#define I915_XD_EDID_CTA_DTD_OFFSET	2U
#define I915_XD_EDID_CTA_FLAGS		3U
#define I915_XD_EDID_CTA_AUDIO		0x40U
#define I915_XD_EDID_CTA_YCBCR444	0x20U
#define I915_XD_EDID_CTA_YCBCR422	0x10U
#define I915_XD_EDID_CTA_BLOCKS		4U

/* The most video formats a decode keeps from the CTA extensions. */
#define I915_XD_EDID_MAX_VICS		32U

/*
 * What the display uses from one EDID.
 *
 * The decode fills it from the bytes; it holds no pointer into them.
 */
struct i915_xd_edid {
	/* How many blocks of the bytes were whole and checked (the base block first). */
	unsigned blocks;

	/* How many extension blocks the base block announces. */
	unsigned extensions;

	/* The vendor and product id as the base block holds it, the manufacturer's letters and the product code. */
	uint8_t vendor[I915_XD_EDID_VENDOR_BYTES];
	char manufacturer[4];
	uint16_t product;

	/* The EDID version and revision. */
	uint8_t version;
	uint8_t revision;

	/* A digital input, and its bits per colour (0: not said). */
	int digital;
	uint8_t bpc;

	/* The screen size in mm (0: not said). */
	uint16_t width_mm;
	uint16_t height_mm;

	/* The preferred timing (the first detailed timing), when it is one the display drives. */
	int has_preferred;
	struct i915_display_mode preferred;

	/* A CTA-861 extension, its HDMI vendor block and the TMDS clock it allows (kHz, 0: not said). */
	int cta;
	int hdmi;
	uint32_t hdmi_max_tmds_khz;

	/* The CTA support flags. */
	int audio;
	int ycbcr444;
	int ycbcr422;

	/* The video formats the CTA extensions list, and the first native one (0: none). */
	unsigned nvics;
	uint8_t vics[I915_XD_EDID_MAX_VICS];
	uint8_t native_vic;
};

/*
 * The DDC an EDID is read over: read_block runs the I2C messages that
 * read size bytes of a block (the segment pointer for a segment past the
 * first, the block's offset within its segment, the read) and answers 0,
 * ENXIO when there is nothing to ask (tried no more), or another error.
 */
struct i915_xd_edid_ddc {
	int (*read_block)(void *context, unsigned segment, unsigned offset, uint8_t *buffer, unsigned size);
	void *context;
};

int drv_i915_xd_edid_block_check(const uint8_t *block, unsigned index);
int drv_i915_xd_edid_read(const struct i915_xd_edid_ddc *ddc, uint8_t *edid, unsigned max_blocks, unsigned *blocks, unsigned *extensions);
int drv_i915_xd_edid_decode(const uint8_t *edid, size_t size, struct i915_xd_edid *out);
int drv_i915_xd_edid_first_timing(const uint8_t *base, struct i915_display_mode *mode, unsigned *index);

#endif /* DRIVERS_GPU_I915_DISPLAY_XD_EDID_H */
