/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The NAL units of the software H.264 decoder: the byte stream (Annex B),
 * the NAL unit header (7.3.1) and the emulation prevention (7.4.1).
 */

#include "h264sw-nal.h"
#include "bits.h"

#include <errno.h>
#include <stddef.h>

/* The length of a start code prefix: 0x00, 0x00, 0x01 (B.1.1). */
#define NAL_START_CODE_LENGTH 3U

/* The forbidden_zero_bit in the first byte of a NAL unit (7.3.1). */
#define NAL_HEADER_FORBIDDEN 0x80U

/* The value that follows two zero bytes to form an emulation prevention byte (7.4.1). */
#define NAL_EMULATION_PREVENTION 0x03U

/* Static function declarations. */
static size_t nal_start_code_find(const uint8_t *data, size_t size, size_t from);
static int nal_has_extension_header(unsigned type);

/*
 * Finds the next NAL unit of a byte stream (B.1.1, B.2).
 *
 * The NAL unit lies between a start code prefix and the next one.  The
 * zero bytes just before the next prefix are trailing_zero_8bits or the
 * zero_byte of a four-byte start code; a NAL unit never ends in a zero
 * byte (7.4.1), so they are removed from its end.
 */
int
h264sw_nal_find(
	const uint8_t *data,
	size_t size,
	size_t *cursor,
	const uint8_t **nal,
	size_t *nal_size)
{
	size_t from;
	size_t prefix;
	size_t begin;
	size_t next;
	size_t end;
	int found;

	/* Without a place to report to, no NAL unit can be delivered. */
	if (cursor == NULL)
		return 0;
	if (nal == NULL ||
	    nal_size == NULL ||
	    data == NULL) {
		*cursor = size;
		return 0;
	}

	/* A cursor beyond the data has nothing left to scan. */
	from = *cursor;
	if (from > size)
		from = size;

	/* Looks for a non-empty NAL unit, skipping the empty ones between adjacent start codes. */
	found = 0;
	while (found == 0) {
		/* Finds the start code prefix; bytes before it are skipped (B.3). */
		prefix = nal_start_code_find(data, size, from);
		if (prefix >= size) {
			*cursor = size;
			return 0;
		}

		/* The NAL unit begins after the prefix and runs to the next prefix or the end. */
		begin = prefix + NAL_START_CODE_LENGTH;
		next = nal_start_code_find(data, size, begin);

		/* Removes the zero bytes that precede the next start code. */
		end = next;
		while (end > begin && data[end - 1U] == 0U)
			end--;

		/* Continues the scan from the next prefix. */
		from = next;

		/* Two start codes in a row delimit a NAL unit of 0 bytes, which is skipped. */
		if (end > begin)
			found = 1;
	}

	/* Reports the NAL unit and where the next scan starts. */
	*nal = data + begin;
	*nal_size = end - begin;
	*cursor = from;

	/* Succeeded: a NAL unit was found. */
	return 1;
}

/*
 * Reads a NAL unit's header and copies its RBSP into buffer (7.3.1, 7.4.1).
 *
 * A NAL unit may end in 0x000003 when a cabac_zero_word precedes it; that
 * final 0x03 is not followed by anything, which media_rbsp_unescape treats
 * as a broken escape, so it is dropped before the bytes are unescaped.
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
	const uint8_t *escaped;
	size_t escaped_size;
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

	/* The RBSP bytes, escaped, follow the one-byte header. */
	escaped = nal + 1;
	escaped_size = size - 1U;

	/* Drops the 0x03 that follows a final cabac_zero_word (7.4.1). */
	if (escaped_size >= 3U &&
	    escaped[escaped_size - 3U] == 0U &&
	    escaped[escaped_size - 2U] == 0U &&
	    escaped[escaped_size - 1U] == NAL_EMULATION_PREVENTION)
		escaped_size--;

	/* Removes the emulation prevention bytes while copying the RBSP into buffer. */
	written = 0U;
	error = media_rbsp_unescape(escaped, escaped_size, buffer, capacity, &written);
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

/*
 * Returns the index of the first start code prefix (0x000001) at or after
 * from, or size when there is none.
 *
 * The scan looks at the third byte of each candidate first: when it is
 * greater than 1, no prefix can start at the candidate or at the two
 * bytes after it, because every prefix has a 0 or a 1 in that place.
 */
static size_t
nal_start_code_find(
	const uint8_t *data,
	size_t size,
	size_t from)
{
	size_t index;

	/* Walks the candidates while three bytes remain. */
	index = from;
	while (index + NAL_START_CODE_LENGTH <= size) {
		/* A third byte above 1 rules out the next three starting places. */
		if (data[index + 2U] > 1U) {
			index += 3U;
			continue;
		}

		/* The prefix is two zero bytes and a one. */
		if (data[index] == 0U && data[index + 1U] == 0U && data[index + 2U] == 1U)
			return index;

		/* Moves to the next starting place. */
		index++;
	}

	/* No prefix remains. */
	return size;
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
