/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The NAL unit header of the software H.264 decoder (7.3.1); the byte
 * stream and the emulation prevention are vcodec-nal.c's.
 */

#include "h264sw-nal.h"

#include <errno.h>
#include <stddef.h>

/* The forbidden_zero_bit in the first byte of a NAL unit (7.3.1). */
#define NAL_HEADER_FORBIDDEN 0x80U

/* Static function declarations. */
static int nal_has_extension_header(unsigned type);

/*
 * Reads a NAL unit's header and copies its RBSP into buffer (7.3.1, 7.4.1).
 *
 * The RBSP is the bytes after the one-byte header, unescaped by
 * vcodec_nal_unescape.
 */
int
h264sw_nal_parse(
	const uint8_t *nal,
	size_t size,
	uint8_t *buffer,
	size_t capacity,
	struct h264sw_nal *out)
{
	unsigned header;
	unsigned type;
	unsigned ref_idc;
	size_t written;
	int extension;
	int error;

	/* A NAL unit has at least its header byte. */
	if (nal == NULL ||
	    out == NULL ||
	    size == 0U)
		return EINVAL;

	/* Splits the header byte into forbidden_zero_bit, nal_ref_idc and nal_unit_type (7.3.1). */
	header = (unsigned)nal[0];
	type = header & 0x1FU;
	ref_idc = (header >> 5) & 0x03U;

	/* A NAL unit with forbidden_zero_bit set is not valid. */
	if ((header & NAL_HEADER_FORBIDDEN) != 0U)
		return EINVAL;

	/* Types with an extended header are not decoded, so only their header is read. */
	extension = nal_has_extension_header(type);
	if (extension != 0) {
		out->type = type;
		out->ref_idc = ref_idc;
		out->rbsp = buffer;
		out->rbsp_size = 0U;
		return 0;
	}

	/* Copies the RBSP out of the bytes after the one-byte header (7.4.1). */
	written = 0U;
	error = vcodec_nal_unescape(nal + 1, size - 1U, buffer, capacity, &written);
	if (error != 0)
		return error;

	/* Reports the header fields and the RBSP. */
	out->type = type;
	out->ref_idc = ref_idc;
	out->rbsp = buffer;
	out->rbsp_size = written;

	/* Succeeded: the NAL unit is parsed. */
	return 0;
}

/* Reports whether the nal_unit_type has the longer header of 14, 20 and 21 (7.3.1). */
static int
nal_has_extension_header(
	unsigned type)
{
	/* Prefix NAL units and the extension slices carry extra header bytes. */
	if (type == H264SW_NAL_PREFIX)
		return 1;
	if (type == H264SW_NAL_SLICE_EXTENSION)
		return 1;
	if (type == H264SW_NAL_SLICE_DEPTH)
		return 1;

	/* Every other type has the one-byte header. */
	return 0;
}
