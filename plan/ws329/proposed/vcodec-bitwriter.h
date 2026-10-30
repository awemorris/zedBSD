/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The bit writer of libmedia's software video encoders: the descriptors
 * u(n), ue(v), se(v) and te(v) written most significant bit first into an
 * RBSP, with the alignment and trailing bits the RBSP syntax needs.  It is
 * the writing counterpart of the decoders' bit reader and serves every
 * codec whose syntax uses these descriptors (H.264 7.2 and 9.1; H.265
 * 7.2 and 9.2 define the same ones).
 *
 * The writer owns a growable buffer.  Errors are sticky: a write that
 * would pass limit bytes, a failed allocation, or a value outside its
 * descriptor's range sets error (ENOBUFS, ENOMEM or EINVAL) and the write
 * and every later one do nothing.  A caller checks error once after a
 * group of writes.  No global state: two writers on two threads need no
 * lock.
 *
 * Arithmetic never relies on undefined behaviour: shifts are by less
 * than the width of their operand, and no signed value is shifted.
 */

#ifndef LIBMEDIA_VCODEC_BITWRITER_H
#define LIBMEDIA_VCODEC_BITWRITER_H

#include <stddef.h>
#include <stdint.h>

/*
 * One RBSP being written.
 *
 * data has capacity bytes; bytes of them are complete.  The bits not yet
 * forming a complete byte are kept in pending (the low pending_bits
 * bits, 0..7, first written is most significant).  limit is the most
 * bytes the buffer may grow to (a caller's bound on one NAL unit; growth
 * past it is ENOBUFS).  error is 0 or the first error.
 *
 * Owned by its maker: vcodec_bitwriter_init allocates nothing,
 * the first write allocates, vcodec_bitwriter_free frees.
 */
struct vcodec_bitwriter {
	uint8_t *data;
	size_t capacity;
	size_t bytes;
	size_t limit;
	uint32_t pending;
	unsigned pending_bits;
	int error;
};

/*
 * A position of a writer, to go back to (vcodec_bitwriter_rewind): the
 * complete bytes, the pending bits and the error at the time it was
 * taken.
 */
struct vcodec_bitmark {
	size_t bytes;
	uint32_t pending;
	unsigned pending_bits;
	int error;
};

/* Makes an empty writer with no buffer yet; limit is the largest size in bytes it may reach (at least 1). */
void vcodec_bitwriter_init(struct vcodec_bitwriter *writer, size_t limit);

/* Frees the buffer and makes the writer empty as after init (with the same limit).  NULL is ignored. */
void vcodec_bitwriter_free(struct vcodec_bitwriter *writer);

/* Empties the writer for a new RBSP: bytes, pending and error 0; the buffer is kept. */
void vcodec_bitwriter_reset(struct vcodec_bitwriter *writer);

/*
 * Writes u(n): the count low bits of value, most significant first.
 *
 * count is 0..32; 0 writes nothing.  A count above 32, or bits of value
 * above bit count - 1 set, is EINVAL (a caller's mistake: a field that
 * does not fit its length).
 */
void vcodec_bitwriter_u(struct vcodec_bitwriter *writer, uint32_t value, unsigned count);

/* Writes one bit: 0 for value 0, 1 otherwise. */
void vcodec_bitwriter_u1(struct vcodec_bitwriter *writer, unsigned value);

/*
 * Writes ue(v): value as an unsigned Exp-Golomb code (H.264 9.1:
 * leadingZeroBits zeros, a 1, then the leadingZeroBits low bits of
 * value + 1 - 2^leadingZeroBits).  value is at most 2^32 - 2 (EINVAL
 * above).
 */
void vcodec_bitwriter_ue(struct vcodec_bitwriter *writer, uint32_t value);

/*
 * Writes se(v): value mapped to codeNum by H.264 Table 9-3 (k > 0 gives
 * 2k - 1, k <= 0 gives -2k), then as ue(v).  value is -(2^31 - 1) ..
 * 2^31 - 1 (EINVAL for -2^31).
 */
void vcodec_bitwriter_se(struct vcodec_bitwriter *writer, int32_t value);

/*
 * Writes te(v) with the largest value range (H.264 9.1): when range is
 * above 1, ue(v); when range is 1, the inverted bit (!value).  range 0,
 * or value above range, is EINVAL.
 */
void vcodec_bitwriter_te(struct vcodec_bitwriter *writer, uint32_t value, uint32_t range);

/* Writes bytes count raw bytes; the writer must be byte aligned (EINVAL otherwise). */
void vcodec_bitwriter_bytes(struct vcodec_bitwriter *writer, const uint8_t *bytes, size_t count);

/* Writes 0 bits up to the next byte boundary (pcm_alignment_zero_bit, alignment_zero_bit); nothing when aligned. */
void vcodec_bitwriter_align_zero(struct vcodec_bitwriter *writer);

/* Writes 1 bits up to the next byte boundary (cabac_alignment_one_bit); nothing when aligned. */
void vcodec_bitwriter_align_one(struct vcodec_bitwriter *writer);

/* Writes rbsp_trailing_bits(): a 1 bit, then 0 bits to the byte boundary (H.264 7.3.2.11). */
void vcodec_bitwriter_trailing(struct vcodec_bitwriter *writer);

/* Reports whether the next bit starts a byte. */
int vcodec_bitwriter_aligned(const struct vcodec_bitwriter *writer);

/* Returns the number of bits written so far (bytes * 8 + pending_bits). */
uint64_t vcodec_bitwriter_bits(const struct vcodec_bitwriter *writer);

/* Takes the writer's position into *mark. */
void vcodec_bitwriter_mark(const struct vcodec_bitwriter *writer, struct vcodec_bitmark *mark);

/*
 * Goes back to a position taken earlier from the same writer since its
 * last reset: what was written after it is dropped, and error becomes
 * the mark's (an ENOBUFS of a macroblock that is then written another
 * way does not stay).  A mark beyond the current
 * position is EINVAL.
 */
void vcodec_bitwriter_rewind(struct vcodec_bitwriter *writer, const struct vcodec_bitmark *mark);

/*
 * Gives the finished RBSP: *data and *size name the complete bytes.  The
 * writer must be byte aligned (EINVAL otherwise).  Returns error (0 on
 * success).  The bytes stay the writer's until its next write, reset or
 * free.
 */
int vcodec_bitwriter_finish(const struct vcodec_bitwriter *writer, const uint8_t **data, size_t *size);

/* The length in bits of ue(v) for value (0..2^32 - 2): 2 * Floor( Log2( value + 1 ) ) + 1. */
unsigned vcodec_ue_bits(uint32_t value);

/* The length in bits of se(v) for value (-(2^31 - 1)..2^31 - 1). */
unsigned vcodec_se_bits(int32_t value);

#endif
