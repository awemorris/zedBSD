/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bit reader of libmedia's software video decoders: the descriptors
 * u(n), ue(v), se(v) and te(v) and the RBSP queries more_rbsp_data() and
 * byte_aligned() over one NAL unit's RBSP, after its emulation prevention
 * bytes are removed.  ITU-T H.264 7.2 and 9.1 and ITU-T H.265 7.2 and 9.2
 * define the same descriptors (H.265 has no te(v)).
 *
 * This is the H.264 decoder's reader under a codec-neutral name, with the
 * same behaviour, plus vcodec_bits_byte_alignment for H.265.  Errors are
 * sticky: a read past the end of the RBSP, or an Exp-Golomb code longer
 * than 32 leading zero bits, sets error, returns 0 and every later read
 * also returns 0.  A caller checks error once after a group of reads and
 * before it uses any value read as an index or a count.
 */

#ifndef LIBMEDIA_VCODEC_BITS_H
#define LIBMEDIA_VCODEC_BITS_H

#include "vcodec.h"

/*
 * A position in one borrowed RBSP.
 *
 * data and size name the RBSP bytes (emulation prevention removed); the
 * reader never writes them and never reads beyond them.  position counts
 * bits from the most significant bit of data[0].  stop is the bit
 * position of the rbsp_stop_one_bit (the last bit equal to 1 in the RBSP),
 * found when the reader is made; it is size * 8 when the RBSP has no bit
 * equal to 1.  error is zero until a read fails.
 *
 * The owner of the bytes keeps them alive while the reader is used; the
 * reader owns nothing.
 */
struct vcodec_bits {
	const uint8_t *data;
	size_t size;
	size_t position;
	size_t stop;
	int error;
};

/*
 * Starts a reader at the first bit of an RBSP.
 *
 * data is NULL only when size is 0.  Finds stop by scanning back over
 * trailing zero bytes (cabac_zero_word bytes of either codec).
 */
void vcodec_bits_init(struct vcodec_bits *bits, const uint8_t *data, size_t size);

/* Reads u(n) (count 0..32, 0 reads nothing); a count above 32 or a read past size * 8 sets error and returns 0. */
uint32_t vcodec_bits_u(struct vcodec_bits *bits, unsigned count);

/* Reads u(1). */
uint32_t vcodec_bits_u1(struct vcodec_bits *bits);

/* Returns the next count bits (0..32) without moving; bits beyond the end read as 0 without an error. */
uint32_t vcodec_bits_peek(struct vcodec_bits *bits, unsigned count);

/* Moves position by count bits; moving past size * 8 sets error and leaves position at size * 8. */
void vcodec_bits_skip(struct vcodec_bits *bits, size_t count);

/* Reads ue(v) (H.264 9.1, H.265 9.2); 32 or more leading zero bits set error and return 0. */
uint32_t vcodec_bits_ue(struct vcodec_bits *bits);

/* Reads se(v) (H.264 9.1.1, H.265 9.2.2): codeNum k gives (-1)^(k+1) * Ceil( k / 2 ), always within int32_t. */
int32_t vcodec_bits_se(struct vcodec_bits *bits);

/* Reads te(v) with largest value range (H.264 9.1); range 0 sets error.  H.265 does not use it. */
uint32_t vcodec_bits_te(struct vcodec_bits *bits, uint32_t range);

/* Reports byte_aligned(). */
int vcodec_bits_aligned(const struct vcodec_bits *bits);

/* Returns the bits left before size * 8. */
size_t vcodec_bits_left(const struct vcodec_bits *bits);

/* Reports more_rbsp_data(): whether position is before stop; 0 after an error. */
int vcodec_bits_more_rbsp_data(const struct vcodec_bits *bits);

/*
 * Reads rbsp_trailing_bits(): the stop bit at stop and zero bits to the
 * byte boundary.  Returns 0, else EINVAL.  Bytes after that boundary
 * (trailing zero bytes, cabac_zero_word) are not checked.
 */
int vcodec_bits_trailing(struct vcodec_bits *bits);

/*
 * Reads byte_alignment() of ITU-T H.265 7.3.2.12: one bit equal to 1
 * (alignment_bit_equal_to_one) and then bits equal to 0 up to the next
 * byte boundary.  Returns 0, else EINVAL (a bit of the wrong value, or
 * the end of the RBSP).  Unlike vcodec_bits_trailing, the 1 bit need not
 * be the stop bit: slice segment data follow.
 */
int vcodec_bits_byte_alignment(struct vcodec_bits *bits);

#endif
