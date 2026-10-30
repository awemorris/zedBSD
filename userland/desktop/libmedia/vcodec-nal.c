/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The NAL units of libmedia's software video decoders: the byte stream
 * (H.264 Annex B) and the emulation prevention (H.264 7.4.1).
 */

#include "vcodec-nal.h"
#include "bits.h"

#include <errno.h>
#include <stddef.h>

/* The length of a start code prefix: 0x00, 0x00, 0x01 (B.1.1). */
#define NAL_START_CODE_LENGTH 3U

/* The value that follows two zero bytes to form an emulation prevention byte (7.4.1). */
#define NAL_EMULATION_PREVENTION 0x03U

/* Static function declarations. */
static size_t nal_start_code_find(const uint8_t *data, size_t size, size_t from);

/*
 * Finds the next NAL unit of a byte stream (B.1.1, B.2).
 *
 * The NAL unit lies between a start code prefix and the next one.  The
 * zero bytes just before the next prefix are trailing_zero_8bits or the
 * zero_byte of a four-byte start code; a NAL unit never ends in a zero
 * byte (7.4.1), so they are removed from its end.
 */
int
vcodec_nal_find(
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
 * Copies a NAL unit's payload into buffer as the RBSP (7.4.1).
 *
 * A NAL unit may end in 0x000003 when a cabac_zero_word precedes it; that
 * final 0x03 is not followed by anything, which media_rbsp_unescape treats
 * as a broken escape, so it is dropped before the bytes are unescaped.
 */
int
vcodec_nal_unescape(
	const uint8_t *escaped,
	size_t size,
	uint8_t *buffer,
	size_t capacity,
	size_t *rbsp_size)
{
	size_t escaped_size;
	size_t written;
	int error;

	/* Without a place to report the length to, no RBSP can be delivered. */
	if (rbsp_size == NULL)
		return EINVAL;

	/* Drops the 0x03 that follows a final cabac_zero_word (7.4.1). */
	escaped_size = size;
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

	/* Reports the RBSP's length. */
	*rbsp_size = written;

	/* Succeeded: the RBSP is in buffer. */
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
