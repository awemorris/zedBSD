/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sink descriptions of the display 12 and 13 code (see xd-edid.h):
 * the check of an EDID block and the decode of an EDID.
 */

#include "display-internal.h"
#include "output-timing.h"
#include "xd-edid.h"

#include <kern/kcrt.h>

#include <uapi/errno.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The colour depth codes of a digital input (bits 6:4 of the input
 * byte, EDID 1.4): 1..6 stand for 6, 8, 10, 12, 14 and 16 bits; 0 and 7
 * say nothing.
 *
 * Values referenced: VESA E-EDID Release A, Revision 2, section 3.6.1
 * (Video Input Definition).
 */
#define I915_XD_EDID_DEPTH_FIRST	1U
#define I915_XD_EDID_DEPTH_LAST		6U
#define I915_XD_EDID_DEPTH_FIRST_BPC	6U
#define I915_XD_EDID_DEPTH_STEP		2U

/* The EDID version from which the input byte says the colour depth. */
#define I915_XD_EDID_DEPTH_REVISION	4U

/*
 * The patience of reading an EDID: the reads of one block before it
 * counts as damaged, and the I2C transfers of one read before it counts
 * as failed (spurious refusals and timeouts are common on a loaded
 * bus).
 *
 * Values referenced: Linux v6.8.12 drivers/gpu/drm/drm_edid.c
 * (edid_block_read: the four tries; drm_do_probe_ddc_edid: the five
 * retries).
 */
#define I915_XD_EDID_BLOCK_TRIES	4U
#define I915_XD_EDID_TRANSFER_TRIES	5U

/*
 * The CTA data block header (tag in bits 7:5, length in 4:0), the tags
 * of the video and vendor blocks, the HDMI vendor block's OUI as it is
 * stored (least significant byte first), its least length and the
 * payload byte of its TMDS clock (in 5 MHz units); and the short video
 * descriptor codes that stand for a native format (129..192 are VIC
 * 1..64 marked native).
 *
 * Values referenced: CTA-861 (data block collection, Video Data Block,
 * Vendor-Specific Data Block) and the HDMI 1.4 specification (HDMI
 * Vendor-Specific Data Block: IEEE OUI 0x000C03, Max_TMDS_Clock).
 */
#define I915_XD_CTA_TAG_SHIFT		5U
#define I915_XD_CTA_LENGTH		0x1fU
#define I915_XD_CTA_TAG_VIDEO		2U
#define I915_XD_CTA_TAG_VENDOR		3U
#define I915_XD_HDMI_OUI_0		0x03U
#define I915_XD_HDMI_OUI_1		0x0cU
#define I915_XD_HDMI_OUI_2		0x00U
#define I915_XD_HDMI_VSDB_MIN_LENGTH	5U
#define I915_XD_HDMI_VSDB_TMDS		6U
#define I915_XD_HDMI_TMDS_UNIT_KHZ	5000U
#define I915_XD_SVD_NATIVE_FIRST	129U
#define I915_XD_SVD_NATIVE_LAST		192U
#define I915_XD_SVD_VIC			0x7fU

static int i915_xd_edid_read_one(const struct i915_xd_edid_ddc *ddc, unsigned index, uint8_t *block);
static int i915_xd_edid_transfer(const struct i915_xd_edid_ddc *ddc, unsigned index, uint8_t *block);
static void i915_xd_edid_base(const uint8_t *base, struct i915_xd_edid *out);
static void i915_xd_edid_cta(const uint8_t *block, struct i915_xd_edid *out);
static void i915_xd_edid_cta_block(const uint8_t *payload, unsigned tag, unsigned length, struct i915_xd_edid *out);

/*
 * Checks one EDID block: its bytes sum to 0 modulo 256, and the base
 * block (index 0) starts with the fixed header.  Returns 0 or EINVAL.
 *
 * Values referenced: VESA E-EDID Release A, Revision 2, section 3.3
 * (Header) and section 3.11 (Extension Flag and Checksum).
 */
int
drv_i915_xd_edid_block_check(
	const uint8_t *block,
	unsigned index)
{
	static const uint8_t header[I915_XD_EDID_HEADER_BYTES] = { 0x00U, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0x00U };
	unsigned sum;
	unsigned at;
	int compared;

	/* The base block starts with the header. */
	if (index == 0U) {
		compared = kern_memcmp(block, header, sizeof(header));
		if (compared != 0)
			return EINVAL;
	}

	/* Sums the block's bytes. */
	sum = 0U;
	for (at = 0U; at < I915_XD_EDID_BLOCK_BYTES; at++)
		sum += block[at];

	/* A block whose bytes do not sum to 0 is damaged. */
	if ((sum & 0xffU) != 0U)
		return EINVAL;

	/* Succeeded: the block checks. */
	return 0;
}

/*
 * Reads an EDID over a DDC: the base block, then the extension blocks it
 * announces, as many as max_blocks holds.  Each block is read up to four
 * times until it checks; a base block whose first read is all zero is no
 * EDID.  Returns 0 with the blocks that checked (at least the base
 * block) and the extensions the base block announces (whether they fit
 * or not), or for a base block that did not read: EIO when it could not
 * be transferred, ENXIO when it was all zero, EPROTO when no read checked.
 * An extension that does not read ends the EDID before it.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/drm_edid.c
 * (_drm_do_get_edid, edid_block_read, edid_block_status).
 */
int
drv_i915_xd_edid_read(
	const struct i915_xd_edid_ddc *ddc,
	uint8_t *edid,
	unsigned max_blocks,
	unsigned *blocks,
	unsigned *extensions)
{
	unsigned total;
	unsigned index;
	int error;

	/* Nothing read yet. */
	*blocks = 0U;
	*extensions = 0U;

	/* Refuses an EDID with no room. */
	if (max_blocks == 0U)
		return EINVAL;

	/* The base block, which says how many follow. */
	error = i915_xd_edid_read_one(ddc, 0U, edid);
	if (error != 0)
		return error;

	/* The base block is read; it names the extensions. */
	*blocks = 1U;
	*extensions = edid[I915_XD_EDID_EXTENSIONS];
	total = 1U + *extensions;

	/* The extensions that fit, until one does not read. */
	for (index = 1U; index < total && index < max_blocks; index++) {
		error = i915_xd_edid_read_one(ddc, index, edid + index * I915_XD_EDID_BLOCK_BYTES);
		if (error != 0)
			break;

		/* Counts the block read. */
		*blocks = index + 1U;
	}

	/* Succeeded: the blocks that checked. */
	return 0;
}

/*
 * Decodes an EDID: the base block, then each CTA-861 extension among the
 * blocks that follow, up to the first block that is missing or damaged.
 * Returns 0, or EINVAL when the base block is missing or damaged
 * (nothing is decoded then).
 */
int
drv_i915_xd_edid_decode(
	const uint8_t *edid,
	size_t size,
	struct i915_xd_edid *out)
{
	const uint8_t *block;
	unsigned index;
	int error;

	/* Starts from nothing known. */
	kern_memset(out, 0, sizeof(*out));

	/* Refuses an EDID without a whole base block. */
	if (edid == NULL || size < I915_XD_EDID_BLOCK_BYTES)
		return EINVAL;

	/* Refuses a damaged base block. */
	error = drv_i915_xd_edid_block_check(edid, 0U);
	if (error != 0)
		return EINVAL;

	/* Decodes the base block. */
	i915_xd_edid_base(edid, out);
	out->blocks = 1U;

	/* Decodes each announced extension that is whole and checks, in order. */
	for (index = 1U; index <= out->extensions; index++) {
		/* An extension past the bytes was not read. */
		if ((size_t)(index + 1U) * I915_XD_EDID_BLOCK_BYTES > size)
			break;

		/* A damaged extension ends the decode. */
		block = edid + index * I915_XD_EDID_BLOCK_BYTES;
		error = drv_i915_xd_edid_block_check(block, index);
		if (error != 0)
			break;

		/* Reads a CTA extension; other extensions are counted and passed over. */
		out->blocks++;
		if (block[0] == I915_XD_EDID_CTA_TAG)
			i915_xd_edid_cta(block, out);
	}

	/* Succeeded: the EDID is decoded. */
	return 0;
}

/*
 * Decodes the first timing descriptor of an EDID base block.
 *
 * The four 18-byte descriptors are looked at in order; the first one with
 * a pixel clock is a timing (the others hold names, ranges or limits),
 * and it is decoded.  index tells which descriptor it was.  Returns 0, or
 * EINVAL when no descriptor is a timing or the timing is not one the
 * display drives (an interlaced one, a zero size).
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/drm_edid.c (the
 * preferred mode taken from the first detailed timing of the base block).
 */
int
drv_i915_xd_edid_first_timing(
	const uint8_t *base,
	struct i915_display_mode *mode,
	unsigned *index)
{
	const uint8_t *descriptor;
	unsigned position;
	int error;

	/* Finds the first descriptor whose pixel clock is not zero. */
	for (position = 0U; position < I915_XD_EDID_DESCRIPTORS; position++) {
		descriptor = base + I915_XD_EDID_FIRST_DTD + I915_XD_EDID_DESCRIPTOR_BYTES * position;
		if (descriptor[0] != 0U || descriptor[1] != 0U)
			break;
	}

	/* A block without a timing descriptor has no first timing. */
	if (position == I915_XD_EDID_DESCRIPTORS)
		return EINVAL;

	/* Decodes it. */
	error = drv_i915_output_edid_timing(descriptor, mode);
	if (error != 0)
		return EINVAL;

	/* Succeeded: the mode and the descriptor it came from. */
	mode->descriptor_index = position;
	if (index != NULL)
		*index = position;
	return 0;
}

/*
 * Decodes the base block: the id, the version, the input and its colour
 * depth, the screen size, the preferred timing and the extension count.
 */
static void
i915_xd_edid_base(
	const uint8_t *base,
	struct i915_xd_edid *out)
{
	uint32_t letters;
	unsigned depth;
	int error;

	/* The vendor and product id, and the manufacturer's three 5-bit letters (big endian, 'A' is 1). */
	kern_memcpy(out->vendor, base + I915_XD_EDID_VENDOR, sizeof(out->vendor));
	letters = ((uint32_t)base[I915_XD_EDID_VENDOR] << 8) | base[I915_XD_EDID_VENDOR + 1U];
	out->manufacturer[0] = (char)('A' - 1 + ((letters >> 10) & 0x1fU));
	out->manufacturer[1] = (char)('A' - 1 + ((letters >> 5) & 0x1fU));
	out->manufacturer[2] = (char)('A' - 1 + (letters & 0x1fU));
	out->manufacturer[3] = '\0';
	out->product = (uint16_t)(base[I915_XD_EDID_PRODUCT] | (base[I915_XD_EDID_PRODUCT + 1U] << 8));

	/* The version, the input, and the extension count. */
	out->version = base[I915_XD_EDID_VERSION];
	out->revision = base[I915_XD_EDID_REVISION];
	out->extensions = base[I915_XD_EDID_EXTENSIONS];
	if ((base[I915_XD_EDID_INPUT] & I915_XD_EDID_INPUT_DIGITAL) != 0U)
		out->digital = 1;

	/* A digital input of EDID 1.4 on says its colour depth. */
	depth = (base[I915_XD_EDID_INPUT] & I915_XD_EDID_INPUT_DEPTH) >> 4;
	if (out->digital != 0 &&
	    out->revision >= I915_XD_EDID_DEPTH_REVISION &&
	    depth >= I915_XD_EDID_DEPTH_FIRST &&
	    depth <= I915_XD_EDID_DEPTH_LAST)
		out->bpc = (uint8_t)(I915_XD_EDID_DEPTH_FIRST_BPC + (depth - I915_XD_EDID_DEPTH_FIRST) * I915_XD_EDID_DEPTH_STEP);

	/* The screen size, in cm in the block. */
	out->width_mm = (uint16_t)(base[I915_XD_EDID_WIDTH_CM] * 10U);
	out->height_mm = (uint16_t)(base[I915_XD_EDID_HEIGHT_CM] * 10U);

	/* The first detailed timing is the preferred one, when the display drives it. */
	error = drv_i915_output_edid_timing(base + I915_XD_EDID_FIRST_DTD, &out->preferred);
	if (error == 0) {
		out->has_preferred = 1;
		out->preferred.edid_bpc = out->bpc;
	}
}

/*
 * Decodes one CTA-861 extension: its support flags, and its data blocks
 * from byte 4 to the first detailed timing.  A data block that runs past
 * that ends the walk.
 */
static void
i915_xd_edid_cta(
	const uint8_t *block,
	struct i915_xd_edid *out)
{
	unsigned end;
	unsigned at;
	unsigned tag;
	unsigned length;

	/* The support flags. */
	out->cta = 1;
	if ((block[I915_XD_EDID_CTA_FLAGS] & I915_XD_EDID_CTA_AUDIO) != 0U)
		out->audio = 1;
	if ((block[I915_XD_EDID_CTA_FLAGS] & I915_XD_EDID_CTA_YCBCR444) != 0U)
		out->ycbcr444 = 1;
	if ((block[I915_XD_EDID_CTA_FLAGS] & I915_XD_EDID_CTA_YCBCR422) != 0U)
		out->ycbcr422 = 1;

	/* An extension whose timing offset is outside it has no data blocks to read. */
	end = block[I915_XD_EDID_CTA_DTD_OFFSET];
	if (end < I915_XD_EDID_CTA_BLOCKS || end > I915_XD_EDID_CHECKSUM)
		return;

	/* Walks the data blocks: a header byte, then the payload. */
	at = I915_XD_EDID_CTA_BLOCKS;
	while (at < end) {
		/* Reads the header; a block running past the collection ends the walk. */
		tag = (unsigned)block[at] >> I915_XD_CTA_TAG_SHIFT;
		length = (unsigned)block[at] & I915_XD_CTA_LENGTH;
		if (at + 1U + length > end)
			return;

		/* Reads the block and steps over it. */
		i915_xd_edid_cta_block(block + at + 1U, tag, length, out);
		at += 1U + length;
	}
}

/*
 * Reads one CTA data block: the video formats of a video block, and the
 * HDMI vendor block with the TMDS clock it allows.
 */
static void
i915_xd_edid_cta_block(
	const uint8_t *payload,
	unsigned tag,
	unsigned length,
	struct i915_xd_edid *out)
{
	unsigned index;
	unsigned code;
	uint8_t vic;

	/* A video block lists one format a byte; 129..192 mark a native one. */
	if (tag == I915_XD_CTA_TAG_VIDEO) {
		for (index = 0U; index < length; index++) {
			/* Takes the format number out of the code. */
			code = payload[index];
			vic = (uint8_t)code;
			if (code >= I915_XD_SVD_NATIVE_FIRST && code <= I915_XD_SVD_NATIVE_LAST) {
				vic = (uint8_t)(code & I915_XD_SVD_VIC);
				if (out->native_vic == 0U)
					out->native_vic = vic;
			}

			/* Keeps the format while there is room. */
			if (out->nvics < I915_XD_EDID_MAX_VICS) {
				out->vics[out->nvics] = vic;
				out->nvics++;
			}
		}

		/* Nothing else is read from a video block. */
		return;
	}

	/* Only the HDMI vendor block is read among the vendor blocks. */
	if (tag != I915_XD_CTA_TAG_VENDOR)
		return;
	if (length < I915_XD_HDMI_VSDB_MIN_LENGTH)
		return;
	if (payload[0] != I915_XD_HDMI_OUI_0 ||
	    payload[1] != I915_XD_HDMI_OUI_1 ||
	    payload[2] != I915_XD_HDMI_OUI_2)
		return;

	/* The sink is HDMI; a longer block says its TMDS clock limit. */
	out->hdmi = 1;
	if (length > I915_XD_HDMI_VSDB_TMDS)
		out->hdmi_max_tmds_khz = (uint32_t)payload[I915_XD_HDMI_VSDB_TMDS] * I915_XD_HDMI_TMDS_UNIT_KHZ;
}

/*
 * Reads one EDID block until it checks: up to four reads, each into a
 * cleared block.  Returns 0, EIO when the block could not be
 * transferred, ENXIO for a block whose first read was all zero, or
 * EPROTO when no read checked.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/drm_edid.c
 * (edid_block_read).
 */
static int
i915_xd_edid_read_one(
	const struct i915_xd_edid_ddc *ddc,
	unsigned index,
	uint8_t *block)
{
	unsigned try;
	unsigned byte;
	int zero;
	int error;

	/* Reads the block until a read checks. */
	for (try = 0U; try < I915_XD_EDID_BLOCK_TRIES; try++) {
		kern_memset(block, 0, I915_XD_EDID_BLOCK_BYTES);
		error = i915_xd_edid_transfer(ddc, index, block);
		if (error != 0)
			return EIO;

		/* A block whose first read is all zero is not there. */
		zero = 1;
		for (byte = 0U; byte < I915_XD_EDID_BLOCK_BYTES; byte++) {
			if (block[byte] != 0U)
				zero = 0;
		}

		/* A first try that read only zeros finds no sink. */
		if (try == 0U && zero)
			return ENXIO;

		/* A block that checks is read. */
		error = drv_i915_xd_edid_block_check(block, index);
		if (error == 0)
			return 0;
	}

	/* No read checked. */
	return EPROTO;
}

/*
 * Transfers one EDID block over the DDC, up to five times until a
 * transfer works; a DDC with nothing to ask is not asked again.  Blocks
 * go two to a 256-byte segment.
 *
 * Procedure referenced: Linux v6.8.12 drivers/gpu/drm/drm_edid.c
 * (drm_do_probe_ddc_edid).
 */
static int
i915_xd_edid_transfer(
	const struct i915_xd_edid_ddc *ddc,
	unsigned index,
	uint8_t *block)
{
	unsigned segment;
	unsigned offset;
	unsigned try;
	int error;

	/* The block's segment and its offset there. */
	segment = index / 2U;
	offset = (index % 2U) * I915_XD_EDID_BLOCK_BYTES;

	/* Transfers until one works. */
	error = EIO;
	for (try = 0U; try < I915_XD_EDID_TRANSFER_TRIES; try++) {
		error = ddc->read_block(ddc->context, segment, offset, block, I915_XD_EDID_BLOCK_BYTES);

		/* A transfer that worked ends the tries. */
		if (error == 0)
			break;

		/* A bus with nothing on it is not tried again. */
		if (error == ENXIO)
			break;
	}

	/* Reports the last transfer's failure. */
	if (error != 0)
		return error;

	/* Succeeded: the block is read. */
	return 0;
}
