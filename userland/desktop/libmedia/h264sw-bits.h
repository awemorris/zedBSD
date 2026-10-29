/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bit reader of the software H.264 decoder: the descriptors of 7.2
 * (u(n), ue(v), se(v), te(v)) and the RBSP queries (more_rbsp_data(),
 * byte_aligned()) over one NAL unit's RBSP, after its emulation
 * prevention bytes are removed.
 *
 * The reader is used for the parameter sets, the slice headers and the
 * CAVLC slice data, so its reads are on the decoding path of every
 * macroblock and are meant to be fast; the CABAC engine (h264sw-cabac.h)
 * takes the bytes over from a byte-aligned position instead.
 *
 * Errors are sticky: a read past the end of the RBSP, or an Exp-Golomb
 * code longer than 32 leading zero bits, sets error, returns 0 and every
 * later read also returns 0.  A caller checks error once after a group of
 * reads and before it uses any value read as an index or a count.
 */

#ifndef LIBMEDIA_H264SW_BITS_H
#define LIBMEDIA_H264SW_BITS_H

#include "h264sw.h"

/*
 * A position in one borrowed RBSP.
 *
 * data and size name the RBSP bytes (emulation prevention removed); the
 * reader never writes them and never reads beyond them.  position counts
 * bits from the most significant bit of data[0].  stop is the bit
 * position of the rbsp_stop_one_bit (the last bit equal to 1 in the RBSP,
 * 7.3.2.11), found when the reader is made; it is size * 8 when the RBSP
 * has no bit equal to 1.  error is zero until a read fails.
 *
 * The owner of the bytes keeps them alive while the reader is used; the
 * reader owns nothing.
 */
struct h264sw_bits {
	const uint8_t *data;
	size_t size;
	size_t position;
	size_t stop;
	int error;
};

/*
 * Starts a reader at the first bit of an RBSP.
 *
 * data is NULL only when size is 0.  Finds stop (the last bit equal to 1,
 * scanning back over the trailing zero bytes, which are cabac_zero_word
 * bytes or trailing bits).  A reader over an empty RBSP, or over one with
 * no bit equal to 1, starts with error clear; its first read fails.
 */
void h264sw_bits_init(struct h264sw_bits *bits, const uint8_t *data, size_t size);

/*
 * Reads u(n): count bits, most significant first, as an unsigned number (7.2).
 *
 * count is 0 to 32; 0 reads nothing and returns 0 without an error (u(v)
 * fields whose length is 0).  A count above 32 is a caller's mistake and
 * sets error.  Reading past size * 8 sets error and returns 0 without
 * moving position.
 */
uint32_t h264sw_bits_u(struct h264sw_bits *bits, unsigned count);

/* Reads u(1), the commonest read (flags), as h264sw_bits_u(bits, 1). */
uint32_t h264sw_bits_u1(struct h264sw_bits *bits);

/*
 * Returns the next count bits (0 to 32) without moving position.
 *
 * Bits beyond the end of the RBSP read as 0 and do not set error: the
 * CAVLC table lookups (h264sw-cavlc.h) peek a fixed width and then skip
 * only the length of the code they found, which h264sw_bits_skip checks.
 */
uint32_t h264sw_bits_peek(struct h264sw_bits *bits, unsigned count);

/* Moves position by count bits; moving past size * 8 sets error and leaves position at size * 8. */
void h264sw_bits_skip(struct h264sw_bits *bits, size_t count);

/*
 * Reads ue(v), an unsigned Exp-Golomb code (9.1).
 *
 * leadingZeroBits is at most 31 for the values this decoder accepts
 * (codeNum up to 2^32 - 2); a code with 32 or more leading zero bits sets
 * error and returns 0.  The caller checks the value against the range of
 * the syntax element before using it.
 */
uint32_t h264sw_bits_ue(struct h264sw_bits *bits);

/*
 * Reads se(v), a signed Exp-Golomb code (9.1.1, Table 9-3).
 *
 * codeNum k maps to (-1)^(k+1) * Ceil( k / 2 ); a codeNum of 2^32 - 2 or
 * less gives a value from -(2^31 - 1) to 2^31 - 1, so the result always
 * fits in int32_t.  The error rules are those of h264sw_bits_ue.
 */
int32_t h264sw_bits_se(struct h264sw_bits *bits);

/*
 * Reads te(v), a truncated Exp-Golomb code whose largest value is range (9.1).
 *
 * range is the largest value the element can take (for ref_idx_l0 and
 * ref_idx_l1 of a frame macroblock: num_ref_idx_lX_active_minus1,
 * 7.4.5.1).  When
 * range is greater than 1 the code is ue(v); when range is 1 it is one
 * bit, inverted (!read_bits(1)).  range 0 is a caller's mistake (the
 * element is not present then) and sets error.  The caller checks that a
 * ue(v) result is not greater than range.
 */
uint32_t h264sw_bits_te(struct h264sw_bits *bits, uint32_t range);

/* Reports whether position is on a byte boundary (byte_aligned(), 7.2). */
int h264sw_bits_aligned(const struct h264sw_bits *bits);

/* Returns the bits left before size * 8 (0 after an error at the end). */
size_t h264sw_bits_left(const struct h264sw_bits *bits);

/*
 * Reports more_rbsp_data() (7.2): whether position is before stop.
 *
 * Returns 0 after an error, so a loop on it ends on a broken RBSP.
 */
int h264sw_bits_more_rbsp_data(const struct h264sw_bits *bits);

/*
 * Reads rbsp_trailing_bits() (7.3.2.11): the stop bit and the zero bits to the byte boundary.
 *
 * Returns 0 when position is at stop and the bits after it to the next
 * byte boundary are 0, else EINVAL.  Bytes after that boundary (trailing
 * zero bytes, cabac_zero_word) are not checked.
 */
int h264sw_bits_trailing(struct h264sw_bits *bits);

#endif
