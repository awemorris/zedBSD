/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The EDID read over a DDC adapter.
 *
 * The DDC transfer and the block checks are the Linux v6.8.12
 * drivers/gpu/drm/drm_edid.c (sha256 a01138078180d234149ac4403839a99b9c85232955a9830ad5552637cd8661a7)
 * rewritten in this tree's style: edid_header[], drm_edid_header_is_valid(),
 * edid_block_compute_checksum(), edid_block_get_checksum() and
 * drm_do_probe_ddc_edid().  drv_i915_drm_edid_read() reads the base block and
 * its extensions with them, following the contract of the Linux
 * edid_block_read(), without the header repair.
 *
 * The eDP reads its panel's EDID through the I2C-over-AUX adapter of its AUX
 * channel; the HDMI detection reads through a GMBUS adapter.
 *
 * The Linux original is under the MIT licence:
 *
 * Copyright (c) 2006 Luc Verhaegen (quirks list)
 * Copyright (c) 2007-2008 Intel Corporation
 *   Jesse Barnes <jesse.barnes@intel.com>
 * Copyright 2010 Red Hat, Inc.
 *
 * DDC probing routines (drm_ddc_read & drm_do_probe_ddc_edid) originally from
 * FB layer.
 *   Copyright (C) 2006 Dennis Munsie <dmunsie@cecropia.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "dp-internal.h"
#include <kern/kcrt.h>

/* The size of one EDID block (Linux drm_edid.h). */
#define EDID_LENGTH 128

/* The DDC address the EDID is read from (Linux drm_edid.h). */
#define DDC_ADDR 0x50

/* The E-DDC segment pointer address. */
#define DDC_SEGMENT_ADDR 0x30

/* How many times one EDID block is read before it counts as damaged. */
#define I915_EDID_BLOCK_TRIES 4u

/*
 * The fixed header of a base EDID block.
 *
 * It is constant for the life of the kernel; the header check counts the
 * bytes of a block that match it.
 */
static const u8 i915_edid_header[] = {
	0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
};

static int i915_drm_edid_header_is_valid(const void *raw_edid);
static int i915_edid_block_compute_checksum(const void *raw_block);
static int i915_edid_block_get_checksum(const void *raw_block);
static int i915_drm_do_probe_ddc_edid(void *data, u8 *buf, unsigned int block, size_t len);
static int i915_edid_read_block(struct i2c_adapter *ddc, u8 *buf, unsigned block);

/*
 * Reads the base EDID block and its extension blocks over a DDC adapter.
 *
 * Each block is tried up to four times; a read failure or an all-zero first
 * answer stops at once, and a block counts only with a correct checksum and,
 * for the base block, a perfect header.  Unlike the Linux edid_block_read()
 * no header repair is attempted: a damaged block is reported, not patched.
 *
 * It returns the number of valid blocks stored (at least 1), or a negative
 * Linux errno when the base block could not be read: -EIO for a read
 * failure, -EPROTO for an invalid block, -ENXIO for an all-zero answer, and
 * -EINVAL for missing arguments.  *extensions receives the base block's
 * extension count even when max_blocks is too small to hold them all.
 */
int
drv_i915_drm_edid_read(
	struct i2c_adapter *ddc,
	u8 *buf,
	unsigned max_blocks,
	unsigned *extensions)
{
	unsigned block;
	unsigned total;
	u8 *raw_block;
	int read;

	/* No extension is known before the base block is read. */
	if (extensions != 0)
		*extensions = 0;

	/* Refuses a read with nowhere to read from or to. */
	if (ddc == 0 || buf == 0 || max_blocks == 0)
		return -I915_DP_EINVAL;

	/* Reads the blocks the base block announces, as many as fit. */
	total = 1;
	for (block = 0; block < total && block < max_blocks; block++) {
		raw_block = buf + block * EDID_LENGTH;

		/* Reads one block; a failure keeps the blocks read before it. */
		read = i915_edid_read_block(ddc, raw_block, block);
		if (read != 0) {
			if (block == 0)
				return read;

			return (int)block;
		}

		/* The base block says how many extensions follow. */
		if (block == 0) {
			total = 1u + ((const struct edid *)raw_block)->extensions;
			if (extensions != 0)
				*extensions = total - 1u;
		}
	}

	/* Succeeded: reports the number of valid blocks stored. */
	return (int)block;
}

/* Scores the header of a base EDID block: 8 when perfect, down to 0. */
static int
i915_drm_edid_header_is_valid(
	const void *raw_edid)
{
	const struct edid *edid;
	int i;
	int score;

	/* Counts the header bytes that match. */
	edid = raw_edid;
	score = 0;
	for (i = 0; i < (int)sizeof(i915_edid_header); i++) {
		if (edid->header[i] == i915_edid_header[i])
			score++;
	}

	/* Succeeded: reports the score. */
	return score;
}

/* Computes the checksum byte a block should carry. */
static int
i915_edid_block_compute_checksum(
	const void *raw_block)
{
	const u8 *block;
	int i;
	u8 csum;
	u8 crc;

	/* Sums every byte but the checksum itself. */
	block = raw_block;
	csum = 0;
	for (i = 0; i < EDID_LENGTH - 1; i++)
		csum += block[i];

	/* The checksum makes the whole block sum to zero. */
	crc = 0x100 - csum;

	/* Succeeded: reports the checksum. */
	return crc;
}

/* Reports the checksum byte a block carries. */
static int
i915_edid_block_get_checksum(
	const void *raw_block)
{
	const struct edid *block;

	/* The checksum is the block's last byte. */
	block = raw_block;

	/* Succeeded: reports it. */
	return block->checksum;
}

/*
 * Reads one EDID block through an I2C adapter (the Linux
 * drm_do_probe_ddc_edid()).
 *
 * The transfer is retried up to five times, because bit-banged transfers
 * on a loaded machine see spurious NAKs and timeouts; an adapter that does
 * not exist is not retried.  The segment pointer is only sent for the
 * blocks past the first two, so as not to upset non-compliant DDC monitors.
 * It returns 0, or -1 when the block could not be read.
 */
static int
i915_drm_do_probe_ddc_edid(
	void *data,
	u8 *buf,
	unsigned int block,
	size_t len)
{
	struct i2c_adapter *adapter;
	unsigned char start;
	unsigned char segment;
	unsigned char xfers;
	int ret;
	int retries;
	struct i2c_msg msgs[3];

	/* Finds the adapter and the segment, offset and message count of the block. */
	adapter = data;
	start = (unsigned char)(block * EDID_LENGTH);
	segment = (unsigned char)(block >> 1);
	if (segment != 0) {
		xfers = 3;
	} else {
		xfers = 2;
	}

	/* Transfers the block, retrying a failed transfer. */
	retries = 5;
	do {
		/* The segment pointer. */
		msgs[0].addr = DDC_SEGMENT_ADDR;
		msgs[0].flags = 0;
		msgs[0].len = 1;
		msgs[0].buf = &segment;

		/* The offset of the block within its segment. */
		msgs[1].addr = DDC_ADDR;
		msgs[1].flags = 0;
		msgs[1].len = 1;
		msgs[1].buf = &start;

		/* The block itself. */
		msgs[2].addr = DDC_ADDR;
		msgs[2].flags = I2C_M_RD;
		msgs[2].len = len;
		msgs[2].buf = buf;

		/* Sends the messages, without the segment pointer for the first two blocks. */
		ret = i915_i2c_transfer(adapter, &msgs[3 - xfers], xfers);

		/* An adapter that does not exist is not retried. */
		if (ret == -I915_DP_ENXIO) {
			I915_DP_DRM_DEBUG_KMS("drm: skipping non-existent adapter %s\n",
					      adapter->name);
			break;
		}

		retries--;
	} while (ret != xfers && retries != 0);

	/* Reports a block that could not be read. */
	if (ret != xfers)
		return -1;

	/* Succeeded: the block is in buf. */
	return 0;
}

/*
 * Reads one EDID block and checks it.
 *
 * It returns 0 for a valid block, or the negative Linux errno that ends the
 * read: -EIO when the block could not be read, -ENXIO when the first answer
 * was all zero, -EPROTO when no try gave a valid block.
 */
static int
i915_edid_read_block(
	struct i2c_adapter *ddc,
	u8 *buf,
	unsigned block)
{
	unsigned try;
	unsigned i;
	int zero;
	int failed;
	int computed;
	int carried;
	int header;
	int valid;

	/* Tries the block until one read is valid. */
	valid = 0;
	for (try = 0; try < I915_EDID_BLOCK_TRIES; try++) {
		/* Reads the block into a cleared buffer. */
		kern_memset(buf, 0, EDID_LENGTH);
		failed = i915_drm_do_probe_ddc_edid(ddc, buf, block, EDID_LENGTH);
		if (failed != 0)
			return -I915_DP_EIO;

		/* An all-zero first answer means no EDID. */
		zero = 1;
		for (i = 0; i < EDID_LENGTH; i++) {
			if (buf[i] != 0)
				zero = 0;
		}

		if (try == 0 && zero)
			return -I915_DP_ENXIO;

		/* A block with a wrong checksum is read again. */
		computed = i915_edid_block_compute_checksum(buf);
		carried = i915_edid_block_get_checksum(buf);
		if (computed != carried)
			continue;

		/* An extension block with a correct checksum is valid. */
		if (block != 0) {
			valid = 1;
			break;
		}

		/* A base block needs a perfect header too. */
		header = i915_drm_edid_header_is_valid(buf);
		if (header == 8) {
			valid = 1;
			break;
		}
	}

	/* Reports a block no try read validly. */
	if (!valid)
		return -I915_DP_EPROTO;

	/* Succeeded: the block is valid. */
	return 0;
}
