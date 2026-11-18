/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decode of a sink's EDID: the base block (identity, size, established
 * and standard timings, detailed timings and display descriptors) and the
 * CTA-861 extensions (detailed timings, video formats, and the HDMI and
 * HDMI Forum vendor blocks).
 *
 * The decode is pure: it reads the bytes a driver read over DDC, AUX, a
 * firmware mailbox or a virtual device, and writes only the caller's
 * structure.  It never decides what an output can show; that is the mode
 * list's work (modes.c).
 */

#include "drivers/gpu/display/edid.h"

#include <stddef.h>

#include <uapi/errno.h>

/*
 * The base block: the header (bytes 0 to 7), the vendor letters (8 and 9,
 * three 5-bit letters from 'A' = 1, the first in the high bits), the
 * product code (10 and 11, little-endian), the serial number (12 to 15,
 * little-endian), the version and revision (18, 19), the video input (20,
 * bit 7 digital), the screen size in cm (21, 22), the established timings
 * (35 to 37), the eight standard timings (38 to 53), the four 18-byte
 * descriptors (54 on), and the extension count (126).
 *
 * Values referenced: VESA Enhanced Extended Display Identification Data
 * Standard (E-EDID), Release A, Revision 2, section 3 (table 3.1, the base
 * block's layout).
 */
#define BASE_VENDOR			8U
#define BASE_PRODUCT			10U
#define BASE_SERIAL			12U
#define BASE_VERSION			18U
#define BASE_REVISION			19U
#define BASE_INPUT			20U
#define BASE_INPUT_DIGITAL		0x80U
#define BASE_WIDTH_CM			21U
#define BASE_HEIGHT_CM			22U
#define BASE_ESTABLISHED		35U
#define BASE_STANDARD			38U
#define BASE_STANDARD_COUNT		8U
#define BASE_DESCRIPTORS		54U
#define BASE_DESCRIPTOR_COUNT		4U
#define BASE_EXTENSIONS			126U
#define VENDOR_LETTER_BITS		5U
#define VENDOR_LETTER_MASK		0x1fU

/*
 * A detailed timing descriptor: the pixel clock in 10 kHz (bytes 0 and 1,
 * little-endian; 0 for a display descriptor), the active and blanking
 * pixels (2, 3, high nibbles in 4), the active and blanking lines (5, 6,
 * high nibbles in 7), the horizontal front porch and sync width (8, 9),
 * the vertical front porch and sync width (the nibbles of 10), the high
 * bits of those four (11), and the flags (17: bit 7 interlaced; bits 4 and
 * 3 both set for digital separate sync, with bit 2 the vertical and bit 1
 * the horizontal polarity).
 *
 * Values referenced: VESA E-EDID Release A, Revision 2, section 3.10.2
 * (tables 3.21 and 3.22, the detailed timing definition).
 */
#define DESCRIPTOR_BYTES		18U
#define DETAIL_CLOCK_UNIT_KHZ		10U
#define DETAIL_FLAGS			17U
#define DETAIL_INTERLACED		0x80U
#define DETAIL_SYNC_KIND		0x18U
#define DETAIL_SYNC_SEPARATE		0x18U
#define DETAIL_V_POSITIVE		0x04U
#define DETAIL_H_POSITIVE		0x02U

/* The smallest active area this decode takes as a picture. */
#define DETAIL_ACTIVE_MIN		16U

/*
 * A display descriptor: its tag (byte 3) and, for the text tags, 13
 * characters from byte 5 ended by a line feed and padded with spaces.  The
 * range limits (0xfd) have the offset flags in byte 4 (bit 0 and bit 1:
 * 255 more on the least and greatest vertical rate; bits 2 and 3: the same
 * on the horizontal rate), the vertical rates in hertz (5, 6), the
 * horizontal rates in kHz (7, 8) and the highest pixel clock in 10 MHz
 * (9).  The standard timings descriptor (0xfa) holds six more standard
 * timings from byte 5.
 *
 * Values referenced: VESA E-EDID Release A, Revision 2, section 3.10.3
 * (tables 3.25 to 3.28: the serial number text 0xff, the name 0xfc, the
 * range limits 0xfd and the standard timing identifiers 0xfa).
 */
#define DISPLAY_TAG			3U
#define DISPLAY_TEXT			5U
#define DISPLAY_TEXT_CHARACTERS		13U
#define DISPLAY_SERIAL			0xffU
#define DISPLAY_NAME			0xfcU
#define DISPLAY_RANGE			0xfdU
#define DISPLAY_STANDARD		0xfaU
#define DISPLAY_STANDARD_COUNT		6U
#define RANGE_OFFSETS			4U
#define RANGE_V_MIN_OFFSET		0x01U
#define RANGE_V_MAX_OFFSET		0x02U
#define RANGE_H_MIN_OFFSET		0x04U
#define RANGE_H_MAX_OFFSET		0x08U
#define RANGE_OFFSET			255U
#define RANGE_V_MIN			5U
#define RANGE_V_MAX			6U
#define RANGE_H_MIN			7U
#define RANGE_H_MAX			8U
#define RANGE_CLOCK			9U
#define RANGE_CLOCK_UNIT_KHZ		10000U

/*
 * A standard timing: the active width as (byte 0 + 31) * 8, and in byte 1
 * the aspect ratio (bits 7 and 6: 16:10, 4:3, 5:4, 16:9; 1:1 instead of
 * 16:10 before revision 3) and the refresh less 60 (bits 5 to 0).  Bytes
 * 01 01 mark an unused slot.
 *
 * Values referenced: VESA E-EDID Release A, Revision 2, section 3.9
 * (table 3.19, the standard timing identifier).
 */
#define STANDARD_UNUSED			0x01U
#define STANDARD_WIDTH_BASE		31U
#define STANDARD_WIDTH_UNIT		8U
#define STANDARD_ASPECT_SHIFT		6U
#define STANDARD_REFRESH_MASK		0x3fU
#define STANDARD_REFRESH_BASE		60U
#define STANDARD_ASPECT_16_10		0U
#define STANDARD_ASPECT_4_3		1U
#define STANDARD_ASPECT_5_4		2U
#define STANDARD_RATIO_REVISION		3U

/* The refresh, in hertz, that a standard timing missing from the monitor list is computed for. */
#define STANDARD_REDUCED_HZ		60U

/*
 * A CTA-861 extension: its tag (byte 0), its revision (1; data blocks come
 * from revision 3), the offset of its detailed timings (2; 0 for none),
 * and the data block collection from byte 4.  A data block's header holds
 * its tag (bits 7 to 5: 2 video, 3 vendor-specific) and the bytes that
 * follow it (bits 4 to 0).  A short video descriptor of 129 to 192 marks a
 * native format: the number is its low 7 bits; any other value is the
 * number itself.
 *
 * Values referenced: CTA-861-G, section 7.5 (table 52, the extension's
 * header), section 7.5.1 (table 54, the data block header's tag codes)
 * and section 7.5.1 (table 56, the short video descriptor and its native
 * bit).
 */
#define CTA_TAG				0x02U
#define CTA_REVISION			1U
#define CTA_DETAIL_OFFSET		2U
#define CTA_BLOCKS_FROM			4U
#define CTA_BLOCKS_REVISION		3U
#define CTA_BLOCK_TAG_SHIFT		5U
#define CTA_BLOCK_LENGTH_MASK		0x1fU
#define CTA_BLOCK_VIDEO			2U
#define CTA_BLOCK_VENDOR		3U
#define CTA_NATIVE_FIRST		129U
#define CTA_NATIVE_LAST			192U
#define CTA_NATIVE_MASK			0x7fU

/*
 * The vendor-specific data blocks: the IEEE OUI in bytes 1 to 3 (least
 * significant first).  The HDMI block (OUI 00-0C-03) holds the highest TMDS
 * clock in 5 MHz in byte 7, when the block is that long.  The HDMI Forum
 * block (OUI C4-5D-D8) holds the highest TMDS character rate in 5 MHz in
 * byte 5 (0: no more than 340 MHz) and, in byte 6, bit 7 for the status and
 * control data channel.
 *
 * Values referenced: HDMI Specification 1.4b, section 8.3.2 (table 8-16,
 * the HDMI vendor-specific data block); HDMI Specification 2.0, section
 * 10.3.2 (table 10-6, the HDMI Forum vendor-specific data block).
 */
#define VENDOR_OUI			1U
#define HDMI_OUI_0			0x03U
#define HDMI_OUI_1			0x0cU
#define HDMI_OUI_2			0x00U
#define HDMI_TMDS_MAX			7U
#define FORUM_OUI_0			0xd8U
#define FORUM_OUI_1			0x5dU
#define FORUM_OUI_2			0xc4U
#define FORUM_TMDS_MAX			5U
#define FORUM_FLAGS			6U
#define FORUM_SCDC			0x80U
#define VENDOR_CLOCK_UNIT_KHZ		5000U

/*
 * The established timings, bit by bit from byte 35 bit 7 down to byte 37
 * bit 7, as size and nominal refresh in hertz; the zero row is the
 * interlaced 1024x768 at 87 Hz, which is not decoded.
 *
 * Values referenced: VESA E-EDID Release A, Revision 2, section 3.8
 * (table 3.18, established timings I and II and the manufacturer's
 * timing).
 */
#define ESTABLISHED_COUNT		17U

/*
 * One established timing bit's mode: the size and the nominal refresh.
 *
 * The table below is constant.
 */
struct established_mode {
	uint16_t width;
	uint16_t height;
	uint16_t refresh_hz;
};

static const struct established_mode established_modes[ESTABLISHED_COUNT] = {
	{ 720U, 400U, 70U },
	{ 720U, 400U, 88U },
	{ 640U, 480U, 60U },
	{ 640U, 480U, 67U },
	{ 640U, 480U, 72U },
	{ 640U, 480U, 75U },
	{ 800U, 600U, 56U },
	{ 800U, 600U, 60U },
	{ 800U, 600U, 72U },
	{ 800U, 600U, 75U },
	{ 832U, 624U, 75U },
	{ 0U, 0U, 0U },
	{ 1024U, 768U, 60U },
	{ 1024U, 768U, 70U },
	{ 1024U, 768U, 75U },
	{ 1280U, 1024U, 75U },
	{ 1152U, 870U, 75U }
};

/*
 * The fixed pattern every EDID base block starts with.
 *
 * Values referenced: VESA E-EDID Release A, Revision 2, section 3.3
 * (table 3.3, the header).
 */
static const uint8_t edid_header[8] = { 0x00U, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0x00U };

static void edid_clear(struct gpu_edid *edid);
static int edid_block_good(const uint8_t *block);
static void edid_identity(const uint8_t *base, struct gpu_edid *edid);
static void edid_descriptors(const uint8_t *base, struct gpu_edid *edid);
static void edid_text(const uint8_t *descriptor, char *text);
static void edid_range(const uint8_t *descriptor, struct gpu_edid *edid);
static int edid_detailed(const uint8_t *descriptor, struct gpu_display_timing *timing);
static void edid_cta_detailed(const uint8_t *block, struct gpu_edid *edid);
static void edid_cta_blocks(const uint8_t *block, struct gpu_edid *edid);
static void edid_cta_video(const uint8_t *data, uint32_t length, struct gpu_edid *edid);
static void edid_cta_vendor(const uint8_t *data, uint32_t length, struct gpu_edid *edid);
static void edid_standard(const uint8_t *pair, uint32_t revision, struct gpu_edid *edid);
static void edid_established(const uint8_t *base, struct gpu_edid *edid);
static void edid_add(struct gpu_edid *edid, const struct gpu_display_timing *timing, uint32_t source);

/*
 * Decodes the EDID in bytes (length bytes; whole blocks are read, a partial
 * last block is ignored): the base block, then up to three extensions.
 *
 * The timings are kept in this order: the base block's detailed timings,
 * the CTA-861 extensions' detailed timings, their video formats, the
 * standard timings, the established timings.  An extension whose checksum
 * is wrong is dropped and counted in bad_blocks.
 *
 * Returns 0 with edid filled, or EINVAL when there is no whole base block,
 * it has no EDID header, its version is not 1, or its checksum is wrong
 * (edid is then empty).
 */
int
gpu_edid_decode(
	const uint8_t *bytes,
	uint32_t length,
	struct gpu_edid *edid)
{
	const uint8_t *block;
	uint32_t available;
	uint32_t extensions;
	uint32_t index;
	uint32_t slot;
	int good;

	/* Nothing is decoded yet. */
	edid_clear(edid);

	/* The base block must be whole and carry the header. */
	if (bytes == NULL || length < GPU_EDID_BLOCK_BYTES)
		return EINVAL;
	for (index = 0U; index < sizeof(edid_header); index++) {
		/* A byte off the header is not an EDID. */
		if (bytes[index] != edid_header[index])
			return EINVAL;
	}

	/* Only EDID version 1 has this layout. */
	if (bytes[BASE_VERSION] != 1U)
		return EINVAL;

	/* A base block whose checksum is wrong is not trusted at all. */
	good = edid_block_good(bytes);
	if (!good)
		return EINVAL;
	edid->blocks = 1U;

	/* The identity, the size and the display descriptors of the base block. */
	edid_identity(bytes, edid);
	edid_descriptors(bytes, edid);

	/* The extensions to read: as many as the base block counts, the bytes hold and the decode keeps. */
	extensions = bytes[BASE_EXTENSIONS];
	available = length / GPU_EDID_BLOCK_BYTES - 1U;
	if (extensions > available)
		extensions = available;
	if (extensions > GPU_EDID_BLOCKS_MAX - 1U)
		extensions = GPU_EDID_BLOCKS_MAX - 1U;

	/* The detailed timings of each good CTA-861 extension. */
	for (index = 1U; index <= extensions; index++) {
		block = bytes + index * GPU_EDID_BLOCK_BYTES;

		/* An extension whose checksum is wrong is dropped, and the rest still read. */
		good = edid_block_good(block);
		if (!good) {
			edid->bad_blocks++;
			continue;
		}
		edid->blocks++;

		/* Only a CTA-861 extension is decoded; a block map or a DisplayID block is skipped. */
		if (block[0] != CTA_TAG)
			continue;

		/* Records the extension and keeps its detailed timings. */
		edid->cta = 1U;
		edid_cta_detailed(block, edid);
	}

	/* The video formats and vendor blocks of each good CTA-861 extension. */
	for (index = 1U; index <= extensions; index++) {
		block = bytes + index * GPU_EDID_BLOCK_BYTES;

		/* Skips what the first pass dropped or did not decode. */
		good = edid_block_good(block);
		if (!good)
			continue;
		if (block[0] != CTA_TAG)
			continue;

		/* The data block collection. */
		edid_cta_blocks(block, edid);
	}

	/* The standard timings of the base block. */
	for (index = 0U; index < BASE_STANDARD_COUNT; index++)
		edid_standard(bytes + BASE_STANDARD + index * 2U, edid->revision, edid);

	/* The standard timings in a standard timings descriptor of the base block. */
	for (index = 0U; index < BASE_DESCRIPTOR_COUNT; index++) {
		block = bytes + BASE_DESCRIPTORS + index * DESCRIPTOR_BYTES;

		/* Only a display descriptor (no clock) with the standard timings tag holds them. */
		if (block[0] != 0U || block[1] != 0U)
			continue;
		if (block[DISPLAY_TAG] != DISPLAY_STANDARD)
			continue;

		/* Its six slots, as in the base block. */
		for (slot = 0U; slot < DISPLAY_STANDARD_COUNT; slot++)
			edid_standard(block + DISPLAY_TEXT + slot * 2U, edid->revision, edid);
	}

	/* The established timings. */
	edid_established(bytes, edid);

	/* Succeeded: edid holds everything the sink said that this decode reads. */
	return 0;
}

/* Empties a decode, so every field reads as not given. */
static void
edid_clear(
	struct gpu_edid *edid)
{
	unsigned char *byte;
	size_t index;

	/* Zeroes every byte; a zero field is a field the EDID did not give. */
	byte = (unsigned char *)edid;
	for (index = 0U; index < sizeof(*edid); index++)
		byte[index] = 0U;
}

/*
 * Checks a block's checksum: its 128 bytes sum to 0 modulo 256.
 *
 * Returns 1 for a good block, 0 for a bad one.
 */
static int
edid_block_good(
	const uint8_t *block)
{
	uint32_t sum;
	uint32_t index;

	/* Adds up the block, the checksum byte included. */
	sum = 0U;
	for (index = 0U; index < GPU_EDID_BLOCK_BYTES; index++)
		sum += block[index];

	/* A remainder means a damaged block. */
	if ((sum & 0xffU) != 0U)
		return 0;

	/* Reports a good block. */
	return 1;
}

/* Decodes the base block's identity, version, input and screen size. */
static void
edid_identity(
	const uint8_t *base,
	struct gpu_edid *edid)
{
	uint32_t letters;
	uint32_t letter;
	uint32_t index;

	/* The three vendor letters, the first in the highest bits; an out-of-range letter reads as '?'. */
	letters = ((uint32_t)base[BASE_VENDOR] << 8) | base[BASE_VENDOR + 1U];
	for (index = 0U; index < 3U; index++) {
		letter = (letters >> ((2U - index) * VENDOR_LETTER_BITS)) & VENDOR_LETTER_MASK;
		if (letter >= 1U && letter <= 26U) {
			edid->vendor[index] = (char)('A' + letter - 1U);
		} else {
			edid->vendor[index] = '?';
		}
	}
	edid->vendor[3] = '\0';

	/* The product code and the serial number, both little-endian. */
	edid->product = (uint32_t)base[BASE_PRODUCT] | ((uint32_t)base[BASE_PRODUCT + 1U] << 8);
	edid->serial = (uint32_t)base[BASE_SERIAL];
	edid->serial |= (uint32_t)base[BASE_SERIAL + 1U] << 8;
	edid->serial |= (uint32_t)base[BASE_SERIAL + 2U] << 16;
	edid->serial |= (uint32_t)base[BASE_SERIAL + 3U] << 24;

	/* The version, the revision and the kind of input. */
	edid->version = base[BASE_VERSION];
	edid->revision = base[BASE_REVISION];
	if ((base[BASE_INPUT] & BASE_INPUT_DIGITAL) != 0U)
		edid->digital = 1U;

	/* The screen size: two nonzero bytes are centimeters; one zero byte makes them an aspect ratio, not a size. */
	if (base[BASE_WIDTH_CM] != 0U && base[BASE_HEIGHT_CM] != 0U) {
		edid->width_mm = (uint32_t)base[BASE_WIDTH_CM] * 10U;
		edid->height_mm = (uint32_t)base[BASE_HEIGHT_CM] * 10U;
	}
}

/*
 * Decodes the base block's four descriptors: the detailed timings (the
 * first of them is the preferred timing) and the serial text, name and
 * range limits.
 */
static void
edid_descriptors(
	const uint8_t *base,
	struct gpu_edid *edid)
{
	struct gpu_display_timing timing;
	const uint8_t *descriptor;
	uint32_t index;
	int error;

	/* Walks the four descriptor slots. */
	for (index = 0U; index < BASE_DESCRIPTOR_COUNT; index++) {
		descriptor = base + BASE_DESCRIPTORS + index * DESCRIPTOR_BYTES;

		/* A clock makes the slot a detailed timing. */
		if (descriptor[0] != 0U || descriptor[1] != 0U) {
			error = edid_detailed(descriptor, &timing);
			if (error != 0)
				continue;

			/* The first detailed timing of the base block is the preferred one. */
			if (index == 0U) {
				edid->preferred = timing;
				edid->preferred.source = GPU_DISPLAY_TIMING_DETAILED;
				edid->has_preferred = 1U;
			}

			/* Keeps the timing in the EDID's order. */
			edid_add(edid, &timing, GPU_DISPLAY_TIMING_DETAILED);
			continue;
		}

		/* A display descriptor: the tag says what it holds. */
		switch (descriptor[DISPLAY_TAG]) {
		case DISPLAY_SERIAL:
			/* The serial number as text. */
			edid_text(descriptor, edid->serial_text);
			break;
		case DISPLAY_NAME:
			/* The product name. */
			edid_text(descriptor, edid->name);
			break;
		case DISPLAY_RANGE:
			/* The rates and clock the sink takes. */
			edid_range(descriptor, edid);
			break;
		default:
			/* Other descriptors (standard timings are read later) carry nothing kept here. */
			break;
		}
	}
}

/* Copies a text descriptor's characters, without the line feed and the padding, and ends them. */
static void
edid_text(
	const uint8_t *descriptor,
	char *text)
{
	uint32_t length;
	uint32_t index;
	uint8_t character;

	/* Copies up to the line feed, keeping only printable characters. */
	length = 0U;
	for (index = 0U; index < DISPLAY_TEXT_CHARACTERS; index++) {
		character = descriptor[DISPLAY_TEXT + index];
		if (character == 0x0aU)
			break;
		if (character < 0x20U || character > 0x7eU)
			character = '?';
		text[length] = (char)character;
		length++;
	}

	/* Drops the trailing spaces some sinks pad with before the line feed. */
	while (length > 0U && text[length - 1U] == ' ')
		length--;

	/* Ends the text. */
	text[length] = '\0';
}

/* Decodes a range limits descriptor. */
static void
edid_range(
	const uint8_t *descriptor,
	struct gpu_edid *edid)
{
	uint32_t offsets;

	/* The rates, each with the 255 an EDID 1.4 offset flag adds. */
	offsets = descriptor[RANGE_OFFSETS];
	edid->v_min_hz = descriptor[RANGE_V_MIN];
	if ((offsets & RANGE_V_MIN_OFFSET) != 0U)
		edid->v_min_hz += RANGE_OFFSET;
	edid->v_max_hz = descriptor[RANGE_V_MAX];
	if ((offsets & RANGE_V_MAX_OFFSET) != 0U)
		edid->v_max_hz += RANGE_OFFSET;
	edid->h_min_khz = descriptor[RANGE_H_MIN];
	if ((offsets & RANGE_H_MIN_OFFSET) != 0U)
		edid->h_min_khz += RANGE_OFFSET;
	edid->h_max_khz = descriptor[RANGE_H_MAX];
	if ((offsets & RANGE_H_MAX_OFFSET) != 0U)
		edid->h_max_khz += RANGE_OFFSET;

	/* The highest pixel clock, in kHz. */
	edid->range_pixel_max_khz = (uint32_t)descriptor[RANGE_CLOCK] * RANGE_CLOCK_UNIT_KHZ;
	edid->has_range = 1U;
}

/*
 * Decodes a detailed timing descriptor.
 *
 * Returns 0 with timing filled (source 0, for the caller to set), or EINVAL
 * for a display descriptor, an interlaced timing, or one whose fields do
 * not make a frame.
 */
static int
edid_detailed(
	const uint8_t *descriptor,
	struct gpu_display_timing *timing)
{
	uint32_t clock;
	uint32_t h_blank;
	uint32_t v_blank;
	uint32_t flags;

	/* The clock; none makes the slot a display descriptor. */
	clock = (uint32_t)descriptor[0] | ((uint32_t)descriptor[1] << 8);
	if (clock == 0U)
		return EINVAL;

	/* An interlaced timing has no place in a progressive scanout. */
	flags = descriptor[DETAIL_FLAGS];
	if ((flags & DETAIL_INTERLACED) != 0U)
		return EINVAL;

	/* The line: 12-bit active and blank, 10-bit front porch and sync width. */
	timing->pixel_khz = clock * DETAIL_CLOCK_UNIT_KHZ;
	timing->h_active = (uint32_t)descriptor[2] | (((uint32_t)descriptor[4] & 0xf0U) << 4);
	h_blank = (uint32_t)descriptor[3] | (((uint32_t)descriptor[4] & 0x0fU) << 8);
	timing->h_front = (uint32_t)descriptor[8] | (((uint32_t)descriptor[11] & 0xc0U) << 2);
	timing->h_sync = (uint32_t)descriptor[9] | (((uint32_t)descriptor[11] & 0x30U) << 4);

	/* The frame: 12-bit active and blank, 6-bit front porch and sync width. */
	timing->v_active = (uint32_t)descriptor[5] | (((uint32_t)descriptor[7] & 0xf0U) << 4);
	v_blank = (uint32_t)descriptor[6] | (((uint32_t)descriptor[7] & 0x0fU) << 8);
	timing->v_front = ((uint32_t)descriptor[10] >> 4) | (((uint32_t)descriptor[11] & 0x0cU) << 2);
	timing->v_sync = ((uint32_t)descriptor[10] & 0x0fU) | (((uint32_t)descriptor[11] & 0x03U) << 4);

	/* A picture too small, or a blank shorter than its porch and sync, is no timing. */
	if (timing->h_active < DETAIL_ACTIVE_MIN || timing->v_active < DETAIL_ACTIVE_MIN)
		return EINVAL;
	if (timing->h_front + timing->h_sync >= h_blank)
		return EINVAL;
	if (timing->v_front + timing->v_sync >= v_blank)
		return EINVAL;
	timing->h_back = h_blank - timing->h_front - timing->h_sync;
	timing->v_back = v_blank - timing->v_front - timing->v_sync;

	/* The polarities: separate digital sync gives both; any other kind gives the horizontal one only. */
	timing->flags = 0U;
	if ((flags & DETAIL_H_POSITIVE) != 0U)
		timing->flags |= GPU_DISPLAY_TIMING_H_POSITIVE;
	if ((flags & DETAIL_SYNC_KIND) == DETAIL_SYNC_SEPARATE &&
	    (flags & DETAIL_V_POSITIVE) != 0U)
		timing->flags |= GPU_DISPLAY_TIMING_V_POSITIVE;

	/* The caller's source and the resulting refresh. */
	timing->source = 0U;
	timing->refresh_mhz = gpu_display_timing_refresh(timing);
	if (timing->refresh_mhz == 0U)
		return EINVAL;

	/* Succeeded: the descriptor is a progressive timing. */
	return 0;
}

/* Keeps the detailed timings of a CTA-861 extension, which run from its offset to its checksum. */
static void
edid_cta_detailed(
	const uint8_t *block,
	struct gpu_edid *edid)
{
	struct gpu_display_timing timing;
	uint32_t offset;
	int error;

	/* No offset, or one inside the header, means no detailed timings. */
	offset = block[CTA_DETAIL_OFFSET];
	if (offset < CTA_BLOCKS_FROM)
		return;

	/* Every whole descriptor before the checksum byte; one with no clock ends the list. */
	while (offset + DESCRIPTOR_BYTES < GPU_EDID_BLOCK_BYTES) {
		if (block[offset] == 0U && block[offset + 1U] == 0U)
			break;
		error = edid_detailed(block + offset, &timing);
		if (error == 0)
			edid_add(edid, &timing, GPU_DISPLAY_TIMING_CTA_DETAILED);
		offset += DESCRIPTOR_BYTES;
	}
}

/* Walks a CTA-861 extension's data block collection for its video formats and vendor blocks. */
static void
edid_cta_blocks(
	const uint8_t *block,
	struct gpu_edid *edid)
{
	uint32_t end;
	uint32_t offset;
	uint32_t tag;
	uint32_t length;

	/* Data blocks come from revision 3, and end where the detailed timings start. */
	if (block[CTA_REVISION] < CTA_BLOCKS_REVISION)
		return;
	end = block[CTA_DETAIL_OFFSET];
	if (end < CTA_BLOCKS_FROM)
		return;
	if (end > GPU_EDID_BLOCK_BYTES - 1U)
		end = GPU_EDID_BLOCK_BYTES - 1U;

	/* Each block: a header, then its bytes; one that would run past the collection ends the walk. */
	offset = CTA_BLOCKS_FROM;
	while (offset < end) {
		tag = (uint32_t)block[offset] >> CTA_BLOCK_TAG_SHIFT;
		length = (uint32_t)block[offset] & CTA_BLOCK_LENGTH_MASK;
		if (offset + 1U + length > end)
			break;

		/* The video formats, and the HDMI vendor blocks. */
		if (tag == CTA_BLOCK_VIDEO) {
			edid_cta_video(block + offset + 1U, length, edid);
		} else if (tag == CTA_BLOCK_VENDOR) {
			edid_cta_vendor(block + offset, length, edid);
		}

		/* The next block. */
		offset += 1U + length;
	}
}

/* Keeps the formats of a video data block that this decode knows the timing of. */
static void
edid_cta_video(
	const uint8_t *data,
	uint32_t length,
	struct gpu_edid *edid)
{
	struct gpu_display_timing timing;
	uint32_t index;
	uint32_t format;
	int error;

	/* Each short video descriptor names one format. */
	for (index = 0U; index < length; index++) {
		/* A native-marked descriptor carries the number in its low bits. */
		format = data[index];
		if (format >= CTA_NATIVE_FIRST && format <= CTA_NATIVE_LAST)
			format &= CTA_NATIVE_MASK;

		/* An unlisted format (interlaced, or not a desktop size) is skipped. */
		error = gpu_display_timing_format(format, &timing);
		if (error != 0)
			continue;

		/* Keeps the format's timing. */
		edid_add(edid, &timing, GPU_DISPLAY_TIMING_CTA_FORMAT);
	}
}

/*
 * Reads an HDMI or HDMI Forum vendor block (header at data[0], length
 * bytes after it).
 */
static void
edid_cta_vendor(
	const uint8_t *data,
	uint32_t length,
	struct gpu_edid *edid)
{
	/* A block too short for its OUI names no vendor. */
	if (length < 3U)
		return;

	/* The HDMI block: the sink is HDMI, and may state its highest TMDS clock. */
	if (data[VENDOR_OUI] == HDMI_OUI_0 &&
	    data[VENDOR_OUI + 1U] == HDMI_OUI_1 &&
	    data[VENDOR_OUI + 2U] == HDMI_OUI_2) {
		edid->hdmi = 1U;
		if (length >= HDMI_TMDS_MAX)
			edid->hdmi_tmds_max_khz = (uint32_t)data[HDMI_TMDS_MAX] * VENDOR_CLOCK_UNIT_KHZ;
		return;
	}

	/* The HDMI Forum block: the TMDS character rate above 340 MHz, and the channel scrambling needs. */
	if (data[VENDOR_OUI] == FORUM_OUI_0 &&
	    data[VENDOR_OUI + 1U] == FORUM_OUI_1 &&
	    data[VENDOR_OUI + 2U] == FORUM_OUI_2) {
		if (length >= FORUM_TMDS_MAX)
			edid->forum_tmds_max_khz = (uint32_t)data[FORUM_TMDS_MAX] * VENDOR_CLOCK_UNIT_KHZ;
		if (length >= FORUM_FLAGS && (data[FORUM_FLAGS] & FORUM_SCDC) != 0U)
			edid->scdc = 1U;
	}
}

/*
 * Keeps a standard timing: the monitor timing of its size and rate, or, at
 * 60 Hz for a size the monitor list lacks, the CVT reduced-blanking timing.
 */
static void
edid_standard(
	const uint8_t *pair,
	uint32_t revision,
	struct gpu_edid *edid)
{
	struct gpu_display_timing timing;
	uint32_t width;
	uint32_t height;
	uint32_t aspect;
	uint32_t refresh;
	int error;

	/* An unused slot. */
	if (pair[0] == STANDARD_UNUSED && pair[1] == STANDARD_UNUSED)
		return;
	if (pair[0] == 0U)
		return;

	/* The width, the refresh, and the height the aspect ratio gives. */
	width = ((uint32_t)pair[0] + STANDARD_WIDTH_BASE) * STANDARD_WIDTH_UNIT;
	refresh = ((uint32_t)pair[1] & STANDARD_REFRESH_MASK) + STANDARD_REFRESH_BASE;
	aspect = (uint32_t)pair[1] >> STANDARD_ASPECT_SHIFT;
	if (aspect == STANDARD_ASPECT_16_10) {
		/* Before revision 3 this code meant 1:1. */
		if (revision < STANDARD_RATIO_REVISION) {
			height = width;
		} else {
			height = width * 10U / 16U;
		}
	} else if (aspect == STANDARD_ASPECT_4_3) {
		height = width * 3U / 4U;
	} else if (aspect == STANDARD_ASPECT_5_4) {
		height = width * 4U / 5U;
	} else {
		height = width * 9U / 16U;
	}

	/* A 1360-pixel 16:9 standard timing names 1360x768: its exact height, 765, is no mode. */
	if (width == 1360U && height == 765U)
		height = 768U;

	/* The monitor timing of the size and rate. */
	error = gpu_display_timing_monitor(width, height, refresh, &timing);
	if (error == 0) {
		edid_add(edid, &timing, GPU_DISPLAY_TIMING_STANDARD);
		return;
	}

	/* A size the list lacks is computed at 60 Hz only; another rate is skipped. */
	if (refresh != STANDARD_REDUCED_HZ)
		return;
	error = gpu_display_timing_reduced(width, height, refresh, &timing);
	if (error != 0)
		return;

	/* Keeps the computed timing. */
	edid_add(edid, &timing, GPU_DISPLAY_TIMING_STANDARD);
}

/* Keeps the timings of the established timing bits that are set. */
static void
edid_established(
	const uint8_t *base,
	struct gpu_edid *edid)
{
	struct gpu_display_timing timing;
	uint32_t index;
	uint32_t byte;
	uint32_t bit;
	int error;

	/* Each bit, from byte 35 bit 7 on. */
	for (index = 0U; index < ESTABLISHED_COUNT; index++) {
		byte = base[BASE_ESTABLISHED + index / 8U];
		bit = 0x80U >> (index % 8U);
		if ((byte & bit) == 0U)
			continue;

		/* The interlaced row is not decoded. */
		if (established_modes[index].width == 0U)
			continue;

		/* The bit's timing from the monitor list. */
		error = gpu_display_timing_monitor(
			established_modes[index].width,
			established_modes[index].height,
			established_modes[index].refresh_hz,
			&timing);
		if (error != 0)
			continue;

		/* Keeps it. */
		edid_add(edid, &timing, GPU_DISPLAY_TIMING_ESTABLISHED);
	}
}

/*
 * Appends a timing with its source, unless the same timing (every field
 * but the source) is already kept or the list is full.
 */
static void
edid_add(
	struct gpu_edid *edid,
	const struct gpu_display_timing *timing,
	uint32_t source)
{
	const struct gpu_display_timing *kept;
	uint32_t index;

	/* A repeat of a kept timing adds nothing; the first naming keeps its source. */
	for (index = 0U; index < edid->timing_count; index++) {
		kept = &edid->timings[index];
		if (kept->pixel_khz != timing->pixel_khz ||
		    kept->h_active != timing->h_active ||
		    kept->h_front != timing->h_front ||
		    kept->h_sync != timing->h_sync ||
		    kept->h_back != timing->h_back)
			continue;
		if (kept->v_active != timing->v_active ||
		    kept->v_front != timing->v_front ||
		    kept->v_sync != timing->v_sync ||
		    kept->v_back != timing->v_back ||
		    kept->flags != timing->flags)
			continue;
		return;
	}

	/* A full list drops the rest, the latest in the EDID's order. */
	if (edid->timing_count >= GPU_EDID_TIMINGS_MAX)
		return;

	/* Appends the timing with its source. */
	edid->timings[edid->timing_count] = *timing;
	edid->timings[edid->timing_count].source = source;
	edid->timing_count++;
}
