/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decode of a sink's EDID (amdrdna-sink.h): the base block's vendor,
 * product, version, input and screen size, its detailed timings, its
 * established and standard timings, the display name and the range limits'
 * highest pixel clock, and the detailed timings of the CTA-861 extensions.
 *
 * The decode is pure: it reads the bytes a caller read over DDC or AUX
 * and copies the values out.  A block whose checksum is wrong ends the
 * decode at it (the blocks before it are kept).  The established and
 * standard timings carry a size and a nominal refresh; their totals and
 * pixel clock come from the VESA monitor timing list when it has them, and
 * a mode without them is listed with a pixel clock of 0 (the caller cannot
 * light it until its timing is known).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/amdrdna/amdrdna-sink.h"

/*
 * The base block: the 8-byte header, the vendor letters (bytes 8 and 9,
 * three 5-bit letters from 'A' = 1), the product code (10, 11) and the
 * serial number (12 to 15), the version and revision (18, 19), the video
 * input (20, bit 7 digital), the screen size in cm (21, 22), the
 * established timings (35 to 37), the eight standard timings (38 to 53),
 * the four 18-byte descriptors (54 on), the extension count (126) and the
 * checksum (127, all 128 bytes summing to 0).
 *
 * Values referenced: VESA Enhanced Extended Display Identification Data
 * Standard (E-EDID) Release A, Revision 2, section 3 (the base block,
 * table 3.1).
 */
#define EDID_HEADER_BYTES		8U
#define EDID_VENDOR_AT			8U
#define EDID_PRODUCT_AT			10U
#define EDID_SERIAL_AT			12U
#define EDID_VERSION_AT			18U
#define EDID_REVISION_AT		19U
#define EDID_INPUT_AT			20U
#define EDID_INPUT_DIGITAL		0x80U
#define EDID_WIDTH_CM_AT		21U
#define EDID_HEIGHT_CM_AT		22U
#define EDID_ESTABLISHED_AT		35U
#define EDID_STANDARD_AT		38U
#define EDID_STANDARD_COUNT		8U
#define EDID_DESCRIPTORS_AT		54U
#define EDID_DESCRIPTOR_COUNT		4U
#define EDID_DESCRIPTOR_BYTES		18U
#define EDID_EXTENSIONS_AT		126U
#define EDID_LETTER_BITS		5U
#define EDID_LETTER_MASK		0x1fU

/*
 * The base block's header.
 *
 * Values referenced: VESA E-EDID Release A, Revision 2, section 3.3
 * (table 3.3, the header 00 FF FF FF FF FF FF 00).
 */
static const uint8_t edid_header[EDID_HEADER_BYTES] = { 0x00U, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0x00U };

/*
 * A detailed timing descriptor: its pixel clock in 10 kHz (bytes 0 and 1,
 * 0 for a display descriptor), the active and blanking pixels and lines
 * (the low bytes 2, 3, 5, 6 and their high nibbles in 4 and 7), the sync
 * offsets and widths (8 to 11), the image size in mm (12 to 14) and the
 * flags (17: bit 7 interlaced, bits 4:3 = 3 digital separate sync with the
 * vertical (bit 2) and horizontal (bit 1) polarities).  A display
 * descriptor has its tag in byte 3: 0xfc the name, 0xfd the range limits
 * (the highest pixel clock in 10 MHz in byte 9).
 *
 * Values referenced: VESA E-EDID Release A, Revision 2, section 3.10
 * (tables 3.21, 3.22 the detailed timing, 3.25 to 3.28 the display
 * descriptors).
 */
#define DETAIL_CLOCK_AT			0U
#define DETAIL_H_ACTIVE_AT		2U
#define DETAIL_H_BLANK_AT		3U
#define DETAIL_H_HIGH_AT		4U
#define DETAIL_V_ACTIVE_AT		5U
#define DETAIL_V_BLANK_AT		6U
#define DETAIL_V_HIGH_AT		7U
#define DETAIL_H_FRONT_AT		8U
#define DETAIL_H_SYNC_AT		9U
#define DETAIL_V_SYNC_AT		10U
#define DETAIL_SYNC_HIGH_AT		11U
#define DETAIL_WIDTH_MM_AT		12U
#define DETAIL_HEIGHT_MM_AT		13U
#define DETAIL_SIZE_HIGH_AT		14U
#define DETAIL_FLAGS_AT			17U
#define DETAIL_INTERLACED		0x80U
#define DETAIL_SYNC_KIND		0x18U
#define DETAIL_SYNC_SEPARATE		0x18U
#define DETAIL_V_POSITIVE		0x04U
#define DETAIL_H_POSITIVE		0x02U
#define DISPLAY_TAG_AT			3U
#define DISPLAY_TEXT_AT			5U
#define DISPLAY_TEXT_BYTES		13U
#define DISPLAY_NAME			0xfcU
#define DISPLAY_RANGE			0xfdU
#define DISPLAY_RANGE_CLOCK_AT		9U
#define DISPLAY_RANGE_CLOCK_KHZ		10000U

/*
 * A CTA-861 extension block: its tag (byte 0, 0x02) and the offset of its
 * detailed timings (byte 2; 0 for none, 4 when no data blocks come before
 * them), after which descriptors run to the checksum.
 *
 * Values referenced: CTA-861-G section 7.5 (the extension's tag, revision
 * and detailed timing offset).
 */
#define CTA_TAG				0x02U
#define CTA_DETAIL_OFFSET_AT		2U
#define CTA_DETAIL_FIRST		4U

/*
 * A standard timing: the active width as (byte 0 + 31) * 8 (byte 0 of 1
 * is unused), and byte 1's aspect ratio in bits 7:6 (16:10, 4:3, 5:4,
 * 16:9; 1:1 before revision 3) and refresh less 60 in bits 5:0.
 *
 * Values referenced: VESA E-EDID Release A, Revision 2, section 3.9
 * (table 3.19).
 */
#define STANDARD_UNUSED			0x01U
#define STANDARD_ASPECT_SHIFT		6U
#define STANDARD_REFRESH_MASK		0x3fU
#define STANDARD_REFRESH_BASE		60U

/*
 * The VESA monitor timings the established and standard timings name:
 * the size, the refresh, the totals, the pixel clock in kHz, the front
 * porches and sync widths (in pixels and lines) and the sync polarities
 * (positive horizontal: bit 0, positive vertical: bit 1).  The
 * established timings' bits, from byte 35 bit 7 down to byte 37 bit 7,
 * index this list (an entry of 0 width is a bit the driver does not list:
 * the interlaced 1024x768).
 *
 * Values referenced: VESA E-EDID Release A, Revision 2, section 3.8
 * (table 3.18, the established timings I and II and the manufacturer's
 * timing), VESA and Industry Standards and Guidelines for Computer
 * Display Monitor Timing (DMT) Version 1.0, Revision 13 (the totals,
 * pixel clocks, porches, sync widths and polarities; 1280x720 is the same
 * as CTA-861-G's), and the IBM VGA and Apple timings the established bits
 * name (720x400 at 70 and 88 Hz, 640x480 at 67 Hz, 832x624 at 75 Hz,
 * 1152x870 at 75 Hz).
 */
struct monitor_timing {
	uint16_t width;
	uint16_t height;
	uint16_t refresh;
	uint16_t h_total;
	uint16_t v_total;
	uint32_t pixel_khz;
	uint16_t h_front;
	uint16_t h_sync;
	uint16_t v_front;
	uint16_t v_sync;
	uint8_t polarity;
};

/* The polarity bits of a monitor timing. */
#define MONITOR_H_POSITIVE		1U
#define MONITOR_V_POSITIVE		2U
#define MONITOR_BOTH_POSITIVE		3U

#define ESTABLISHED_COUNT		17U

static const struct monitor_timing established_timings[ESTABLISHED_COUNT] = {
	{ 720U, 400U, 70U, 900U, 449U, 28322U, 18U, 108U, 12U, 2U, MONITOR_V_POSITIVE },
	{ 720U, 400U, 88U, 900U, 449U, 35500U, 18U, 108U, 12U, 2U, MONITOR_V_POSITIVE },
	{ 640U, 480U, 60U, 800U, 525U, 25175U, 16U, 96U, 10U, 2U, 0U },
	{ 640U, 480U, 67U, 864U, 525U, 30240U, 64U, 64U, 3U, 3U, 0U },
	{ 640U, 480U, 72U, 832U, 520U, 31500U, 24U, 40U, 9U, 3U, 0U },
	{ 640U, 480U, 75U, 840U, 500U, 31500U, 16U, 64U, 1U, 3U, 0U },
	{ 800U, 600U, 56U, 1024U, 625U, 36000U, 24U, 72U, 1U, 2U, MONITOR_BOTH_POSITIVE },
	{ 800U, 600U, 60U, 1056U, 628U, 40000U, 40U, 128U, 1U, 4U, MONITOR_BOTH_POSITIVE },
	{ 800U, 600U, 72U, 1040U, 666U, 50000U, 56U, 120U, 37U, 6U, MONITOR_BOTH_POSITIVE },
	{ 800U, 600U, 75U, 1056U, 625U, 49500U, 16U, 80U, 1U, 3U, MONITOR_BOTH_POSITIVE },
	{ 832U, 624U, 75U, 1152U, 667U, 57284U, 32U, 64U, 1U, 3U, 0U },
	{ 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U },
	{ 1024U, 768U, 60U, 1344U, 806U, 65000U, 24U, 136U, 3U, 6U, 0U },
	{ 1024U, 768U, 70U, 1328U, 806U, 75000U, 24U, 136U, 3U, 6U, 0U },
	{ 1024U, 768U, 75U, 1312U, 800U, 78750U, 16U, 96U, 1U, 3U, MONITOR_BOTH_POSITIVE },
	{ 1280U, 1024U, 75U, 1688U, 1066U, 135000U, 16U, 144U, 1U, 3U, MONITOR_BOTH_POSITIVE },
	{ 1152U, 870U, 75U, 1456U, 915U, 100000U, 32U, 128U, 3U, 3U, 0U }
};

#define STANDARD_KNOWN			12U

static const struct monitor_timing standard_timings[STANDARD_KNOWN] = {
	{ 1152U, 864U, 75U, 1600U, 900U, 108000U, 64U, 128U, 1U, 3U, MONITOR_BOTH_POSITIVE },
	{ 1280U, 720U, 60U, 1650U, 750U, 74250U, 110U, 40U, 5U, 5U, MONITOR_BOTH_POSITIVE },
	{ 1280U, 800U, 60U, 1680U, 831U, 83500U, 72U, 128U, 3U, 6U, MONITOR_V_POSITIVE },
	{ 1280U, 960U, 60U, 1800U, 1000U, 108000U, 96U, 112U, 1U, 3U, MONITOR_BOTH_POSITIVE },
	{ 1280U, 1024U, 60U, 1688U, 1066U, 108000U, 48U, 112U, 1U, 3U, MONITOR_BOTH_POSITIVE },
	{ 1366U, 768U, 60U, 1792U, 798U, 85500U, 70U, 143U, 3U, 3U, MONITOR_BOTH_POSITIVE },
	{ 1440U, 900U, 60U, 1904U, 934U, 106500U, 80U, 152U, 3U, 6U, MONITOR_V_POSITIVE },
	{ 1600U, 900U, 60U, 1800U, 1000U, 108000U, 24U, 80U, 1U, 3U, MONITOR_BOTH_POSITIVE },
	{ 1600U, 1200U, 60U, 2160U, 1250U, 162000U, 64U, 192U, 1U, 3U, MONITOR_BOTH_POSITIVE },
	{ 1680U, 1050U, 60U, 2240U, 1089U, 146250U, 104U, 176U, 3U, 6U, MONITOR_V_POSITIVE },
	{ 1920U, 1080U, 60U, 2200U, 1125U, 148500U, 88U, 44U, 4U, 5U, MONITOR_BOTH_POSITIVE },
	{ 1920U, 1200U, 60U, 2080U, 1235U, 154000U, 48U, 32U, 3U, 6U, MONITOR_H_POSITIVE }
};

/* The millihertz of a hertz, and the hertz of a kHz. */
#define EDID_MILLI			1000U

static bool edid_block_good(const uint8_t *block);
static void edid_vendor(const uint8_t *block, struct amdrdna_edid *edid);
static void edid_descriptor(const uint8_t *descriptor, struct amdrdna_edid *edid, bool preferred);
static void edid_established(const uint8_t *block, struct amdrdna_edid *edid);
static void edid_standard(const uint8_t *block, struct amdrdna_edid *edid);
static void edid_add(struct amdrdna_edid *edid, const struct amdrdna_timing *timing);
static void edid_monitor_timing(const struct monitor_timing *known, uint8_t source, struct amdrdna_timing *timing);
static uint32_t edid_refresh(uint32_t pixel_khz, uint32_t h_total, uint32_t v_total);

/*
 * Decodes the EDID in bytes (length bytes, whole blocks): the base block,
 * then each CTA-861 extension's detailed timings.
 *
 * Returns 0 with edid filled, or EINVAL when the base block is short, has
 * no header or a wrong checksum.
 */
int
amdrdna_edid_decode(
	const uint8_t *bytes,
	size_t length,
	struct amdrdna_edid *edid)
{
	const uint8_t *block;
	unsigned index;
	unsigned offset;
	unsigned extension;
	bool good;

	/* Nothing read yet. */
	*edid = (struct amdrdna_edid){ 0 };

	/* The base block: whole, with its header and checksum. */
	if (length < AMDRDNA_EDID_BLOCK_BYTES)
		return EINVAL;
	for (index = 0U; index < EDID_HEADER_BYTES; index++) {
		if (bytes[index] != edid_header[index])
			return EINVAL;
	}
	good = edid_block_good(bytes);
	if (!good)
		return EINVAL;
	edid->blocks = 1U;

	/* What the base block says of the sink. */
	edid_vendor(bytes, edid);
	edid->version = bytes[EDID_VERSION_AT];
	edid->revision = bytes[EDID_REVISION_AT];
	edid->digital = false;
	if ((bytes[EDID_INPUT_AT] & EDID_INPUT_DIGITAL) != 0U)
		edid->digital = true;
	edid->width_mm = (uint32_t)bytes[EDID_WIDTH_CM_AT] * 10U;
	edid->height_mm = (uint32_t)bytes[EDID_HEIGHT_CM_AT] * 10U;
	edid->extensions = bytes[EDID_EXTENSIONS_AT];

	/* The descriptors (the first detailed timing is the preferred mode), then the established and standard timings. */
	for (index = 0U; index < EDID_DESCRIPTOR_COUNT; index++)
		edid_descriptor(bytes + EDID_DESCRIPTORS_AT + index * EDID_DESCRIPTOR_BYTES, edid, index == 0U);
	edid_established(bytes, edid);
	edid_standard(bytes, edid);

	/* Each whole extension block in order; a damaged one ends the decode. */
	for (extension = 1U; extension <= edid->extensions; extension++) {
		if ((size_t)(extension + 1U) * AMDRDNA_EDID_BLOCK_BYTES > length)
			break;
		block = bytes + (size_t)extension * AMDRDNA_EDID_BLOCK_BYTES;
		good = edid_block_good(block);
		if (!good)
			break;
		edid->blocks++;

		/* A CTA-861 block's detailed timings, from its offset to the checksum. */
		if (block[0] != CTA_TAG)
			continue;
		offset = block[CTA_DETAIL_OFFSET_AT];
		if (offset < CTA_DETAIL_FIRST)
			continue;
		while (offset + EDID_DESCRIPTOR_BYTES < AMDRDNA_EDID_BLOCK_BYTES) {
			edid_descriptor(block + offset, edid, false);
			offset += EDID_DESCRIPTOR_BYTES;
		}
	}

	/* Succeeded: the sink is described. */
	return 0;
}

/*
 * Finds the listed mode of a size and refresh (0 asks for the best
 * refresh: the preferred mode's when it is that size, else the highest).
 * Returns its index, or AMDRDNA_EDID_MODES when there is none.
 */
unsigned
amdrdna_edid_find(
	const struct amdrdna_edid *edid,
	uint32_t width,
	uint32_t height,
	uint32_t refresh_millihz)
{
	const struct amdrdna_timing *mode;
	unsigned best;
	unsigned index;
	uint32_t difference;

	/* Each mode of the size. */
	best = AMDRDNA_EDID_MODES;
	for (index = 0U; index < edid->mode_count; index++) {
		mode = &edid->modes[index];
		if (mode->width != width || mode->height != height)
			continue;

		/* A refresh asked for matches within half a hertz. */
		if (refresh_millihz != 0U) {
			if (mode->refresh_millihz > refresh_millihz)
				difference = mode->refresh_millihz - refresh_millihz;
			else
				difference = refresh_millihz - mode->refresh_millihz;
			if (difference <= EDID_MILLI / 2U)
				return index;
			continue;
		}

		/* No refresh asked for: the preferred mode, else the highest refresh. */
		if (mode->source == AMDRDNA_TIMING_PREFERRED)
			return index;
		if (best == AMDRDNA_EDID_MODES || mode->refresh_millihz > edid->modes[best].refresh_millihz)
			best = index;
	}

	/* Succeeded: the best of the size, or none. */
	return best;
}

/* Reports whether a block's 128 bytes sum to 0. */
static bool
edid_block_good(
	const uint8_t *block)
{
	unsigned index;
	uint8_t sum;

	/* The bytes' sum. */
	sum = 0U;
	for (index = 0U; index < AMDRDNA_EDID_BLOCK_BYTES; index++)
		sum = (uint8_t)(sum + block[index]);

	/* A good block sums to 0. */
	if (sum != 0U)
		return false;

	/* Succeeded: the block is whole. */
	return true;
}

/* Reads the vendor's three letters, the product code and the serial number. */
static void
edid_vendor(
	const uint8_t *block,
	struct amdrdna_edid *edid)
{
	uint32_t word;
	unsigned letter;

	/* Three 5-bit letters, high to low, 1 for 'A'. */
	word = ((uint32_t)block[EDID_VENDOR_AT] << 8) | block[EDID_VENDOR_AT + 1U];
	for (letter = 0U; letter < 3U; letter++)
		edid->vendor[letter] = (char)('A' - 1 + ((word >> ((2U - letter) * EDID_LETTER_BITS)) & EDID_LETTER_MASK));
	edid->vendor[3] = '\0';

	/* The product code and the serial number, little-endian. */
	edid->product = (uint16_t)(block[EDID_PRODUCT_AT] | (block[EDID_PRODUCT_AT + 1U] << 8));
	edid->serial = (uint32_t)block[EDID_SERIAL_AT] | ((uint32_t)block[EDID_SERIAL_AT + 1U] << 8) |
		       ((uint32_t)block[EDID_SERIAL_AT + 2U] << 16) | ((uint32_t)block[EDID_SERIAL_AT + 3U] << 24);
}

/*
 * Reads one 18-byte descriptor: a detailed timing (a mode, the preferred
 * one when it is the base block's first), the display name or the range
 * limits' highest pixel clock.
 */
static void
edid_descriptor(
	const uint8_t *descriptor,
	struct amdrdna_edid *edid,
	bool preferred)
{
	struct amdrdna_timing timing;
	uint32_t clock;
	unsigned index;
	uint8_t flags;

	/* A display descriptor: the name (to its line feed) or the range limits. */
	clock = (uint32_t)descriptor[DETAIL_CLOCK_AT] | ((uint32_t)descriptor[DETAIL_CLOCK_AT + 1U] << 8);
	if (clock == 0U) {
		if (descriptor[DISPLAY_TAG_AT] == DISPLAY_NAME) {
			for (index = 0U; index < DISPLAY_TEXT_BYTES; index++) {
				if (descriptor[DISPLAY_TEXT_AT + index] == '\n')
					break;
				edid->name[index] = (char)descriptor[DISPLAY_TEXT_AT + index];
			}
			edid->name[index] = '\0';
		} else if (descriptor[DISPLAY_TAG_AT] == DISPLAY_RANGE) {
			edid->max_pixel_khz = (uint32_t)descriptor[DISPLAY_RANGE_CLOCK_AT] * DISPLAY_RANGE_CLOCK_KHZ;
		}
		return;
	}

	/* A detailed timing: the clock, the active and blanking parts with their high nibbles. */
	timing = (struct amdrdna_timing){ 0 };
	timing.pixel_khz = clock * 10U;
	timing.width = (uint16_t)(descriptor[DETAIL_H_ACTIVE_AT] | ((descriptor[DETAIL_H_HIGH_AT] & 0xf0U) << 4));
	timing.h_blanking = (uint16_t)(descriptor[DETAIL_H_BLANK_AT] | ((descriptor[DETAIL_H_HIGH_AT] & 0x0fU) << 8));
	timing.height = (uint16_t)(descriptor[DETAIL_V_ACTIVE_AT] | ((descriptor[DETAIL_V_HIGH_AT] & 0xf0U) << 4));
	timing.v_blanking = (uint16_t)(descriptor[DETAIL_V_BLANK_AT] | ((descriptor[DETAIL_V_HIGH_AT] & 0x0fU) << 8));

	/* The sync offsets and widths: 10 bits horizontal, 6 vertical. */
	timing.h_porch = (uint16_t)(descriptor[DETAIL_H_FRONT_AT] | ((descriptor[DETAIL_SYNC_HIGH_AT] & 0xc0U) << 2));
	timing.h_pulse = (uint16_t)(descriptor[DETAIL_H_SYNC_AT] | ((descriptor[DETAIL_SYNC_HIGH_AT] & 0x30U) << 4));
	timing.v_porch = (uint16_t)((descriptor[DETAIL_V_SYNC_AT] >> 4) | ((descriptor[DETAIL_SYNC_HIGH_AT] & 0x0cU) << 2));
	timing.v_pulse = (uint16_t)((descriptor[DETAIL_V_SYNC_AT] & 0x0fU) | ((descriptor[DETAIL_SYNC_HIGH_AT] & 0x03U) << 4));

	/* The image size, the flags. */
	timing.width_mm = (uint16_t)(descriptor[DETAIL_WIDTH_MM_AT] | ((descriptor[DETAIL_SIZE_HIGH_AT] & 0xf0U) << 4));
	timing.height_mm = (uint16_t)(descriptor[DETAIL_HEIGHT_MM_AT] | ((descriptor[DETAIL_SIZE_HIGH_AT] & 0x0fU) << 8));
	flags = descriptor[DETAIL_FLAGS_AT];
	if ((flags & DETAIL_INTERLACED) != 0U)
		timing.interlaced = true;
	if ((flags & DETAIL_SYNC_KIND) == DETAIL_SYNC_SEPARATE && (flags & DETAIL_V_POSITIVE) != 0U)
		timing.v_positive = true;
	if ((flags & DETAIL_SYNC_KIND) == DETAIL_SYNC_SEPARATE && (flags & DETAIL_H_POSITIVE) != 0U)
		timing.h_positive = true;

	/* The refresh, the source, the list. */
	timing.refresh_millihz = edid_refresh(timing.pixel_khz, (uint32_t)timing.width + timing.h_blanking,
	    (uint32_t)timing.height + timing.v_blanking);
	timing.source = AMDRDNA_TIMING_DETAILED;
	if (preferred)
		timing.source = AMDRDNA_TIMING_PREFERRED;
	edid_add(edid, &timing);
}

/* Lists the established timings the three bytes name. */
static void
edid_established(
	const uint8_t *block,
	struct amdrdna_edid *edid)
{
	struct amdrdna_timing timing;
	unsigned bit;
	uint32_t bits;

	/* The 17 bits from byte 35 bit 7 on. */
	bits = ((uint32_t)block[EDID_ESTABLISHED_AT] << 16) | ((uint32_t)block[EDID_ESTABLISHED_AT + 1U] << 8) |
	       block[EDID_ESTABLISHED_AT + 2U];
	for (bit = 0U; bit < ESTABLISHED_COUNT; bit++) {
		if ((bits & (1U << (23U - bit))) == 0U || established_timings[bit].width == 0U)
			continue;
		edid_monitor_timing(&established_timings[bit], AMDRDNA_TIMING_ESTABLISHED, &timing);
		edid_add(edid, &timing);
	}
}

/* Lists the standard timings: their size and refresh, with the monitor timing when it is known. */
static void
edid_standard(
	const uint8_t *block,
	struct amdrdna_edid *edid)
{
	struct monitor_timing known;
	struct amdrdna_timing timing;
	unsigned index;
	unsigned entry;
	uint32_t width;
	uint32_t height;
	uint32_t refresh;
	uint8_t first;
	uint8_t second;

	/* Each of the eight that is used. */
	for (index = 0U; index < EDID_STANDARD_COUNT; index++) {
		first = block[EDID_STANDARD_AT + index * 2U];
		second = block[EDID_STANDARD_AT + index * 2U + 1U];
		if (first == STANDARD_UNUSED || first == 0U)
			continue;

		/* The width, the height from the aspect ratio, the refresh. */
		width = ((uint32_t)first + 31U) * 8U;
		switch (second >> STANDARD_ASPECT_SHIFT) {
		case 0U:
			/* 16:10 from revision 3, 1:1 before. */
			height = width * 10U / 16U;
			if (block[EDID_VERSION_AT] == 1U && block[EDID_REVISION_AT] < 3U)
				height = width;
			break;
		case 1U:
			height = width * 3U / 4U;
			break;
		case 2U:
			height = width * 4U / 5U;
			break;
		default:
			height = width * 9U / 16U;
			break;
		}
		refresh = (second & STANDARD_REFRESH_MASK) + STANDARD_REFRESH_BASE;

		/* The monitor timing of the size and refresh, or the size alone. */
		known = (struct monitor_timing){ (uint16_t)width, (uint16_t)height, (uint16_t)refresh, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U };
		for (entry = 0U; entry < STANDARD_KNOWN; entry++) {
			if (standard_timings[entry].width == width &&
			    standard_timings[entry].height == height &&
			    standard_timings[entry].refresh == refresh) {
				known = standard_timings[entry];
				break;
			}
		}
		edid_monitor_timing(&known, AMDRDNA_TIMING_STANDARD, &timing);
		edid_add(edid, &timing);
	}
}

/* Adds a mode to the list unless it is full or lists the same size and refresh already. */
static void
edid_add(
	struct amdrdna_edid *edid,
	const struct amdrdna_timing *timing)
{
	unsigned index;
	uint32_t difference;

	/* A mode of the same size within half a hertz is there already. */
	for (index = 0U; index < edid->mode_count; index++) {
		if (edid->modes[index].width != timing->width || edid->modes[index].height != timing->height)
			continue;
		if (edid->modes[index].refresh_millihz > timing->refresh_millihz)
			difference = edid->modes[index].refresh_millihz - timing->refresh_millihz;
		else
			difference = timing->refresh_millihz - edid->modes[index].refresh_millihz;
		if (difference <= EDID_MILLI / 2U)
			return;
	}

	/* A full list keeps what it has. */
	if (edid->mode_count >= AMDRDNA_EDID_MODES)
		return;
	edid->modes[edid->mode_count] = *timing;
	edid->mode_count++;
}

/* Makes a mode of a monitor timing: its size, totals, porches, syncs and clock, or its nominal refresh when the clock is unknown. */
static void
edid_monitor_timing(
	const struct monitor_timing *known,
	uint8_t source,
	struct amdrdna_timing *timing)
{
	/* The size, the totals as blanking, the clock. */
	*timing = (struct amdrdna_timing){ 0 };
	timing->width = known->width;
	timing->height = known->height;
	timing->pixel_khz = known->pixel_khz;
	timing->source = source;
	if (known->h_total > known->width)
		timing->h_blanking = (uint16_t)(known->h_total - known->width);
	if (known->v_total > known->height)
		timing->v_blanking = (uint16_t)(known->v_total - known->height);

	/* The front porches, the syncs and their polarities. */
	timing->h_porch = known->h_front;
	timing->h_pulse = known->h_sync;
	timing->v_porch = known->v_front;
	timing->v_pulse = known->v_sync;
	timing->h_positive = false;
	if ((known->polarity & MONITOR_H_POSITIVE) != 0U)
		timing->h_positive = true;
	timing->v_positive = false;
	if ((known->polarity & MONITOR_V_POSITIVE) != 0U)
		timing->v_positive = true;

	/* The refresh from the totals, or the nominal one. */
	timing->refresh_millihz = (uint32_t)known->refresh * EDID_MILLI;
	if (known->pixel_khz != 0U)
		timing->refresh_millihz = edid_refresh(known->pixel_khz, known->h_total, known->v_total);
}

/* Reports the refresh in millihertz of a pixel clock over its totals (0 for no totals). */
static uint32_t
edid_refresh(
	uint32_t pixel_khz,
	uint32_t h_total,
	uint32_t v_total)
{
	uint64_t frame;
	uint64_t refresh;

	/* The pixels of a frame. */
	frame = (uint64_t)h_total * v_total;
	if (frame == 0U)
		return 0U;

	/* Pixels a second over pixels a frame, in millihertz, rounded. */
	refresh = ((uint64_t)pixel_khz * EDID_MILLI * EDID_MILLI + frame / 2U) / frame;

	/* Succeeded: the refresh. */
	return (uint32_t)refresh;
}
