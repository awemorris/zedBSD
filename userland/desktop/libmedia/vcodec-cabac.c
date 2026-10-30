/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CABAC arithmetic decoding engine of libmedia's software video
 * decoders (H.264 9.3.1.2 and 9.3.3.2; H.265 9.3.2.6 and 9.3.4.3).
 *
 * The engine keeps codIOffset scaled: offset holds the 9-bit codIOffset
 * of the clauses followed by bits_left bits that were read ahead of it,
 * so offset = ( codIOffset << bits_left ) | the read-ahead bits.  A
 * comparison of codIOffset with a range is then a comparison of offset
 * with ( range << bits_left ), and a renormalization that shifts one bit
 * into codIOffset only lowers bits_left.  Bytes are read ahead one at a
 * time, when fewer than CABAC_AHEAD_MIN bits are left.
 *
 * A bitstream that breaks the rule that codIOffset stays below codIRange
 * decodes to garbage bins, but every operation stays defined (unsigned
 * arithmetic, bounded table indices).
 */

#include "vcodec-cabac.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/* codIRange after the initialization of the engine (9.3.1.2). */
#define CABAC_RANGE_INIT 510U

/* The renormalization (9.3.3.2.2) doubles codIRange while it is below this. */
#define CABAC_RANGE_RENORM 256U

/* The smallest codIOffset a bitstream shall not start with: 510 and 511 are forbidden (9.3.1.2). */
#define CABAC_OFFSET_FORBIDDEN_LOW 510U

/* The read-ahead bits that remain after the 9 bits of codIOffset in the start bytes: 16 - 9. */
#define CABAC_START_AHEAD 7

/*
 * A byte is read ahead when fewer than this many bits are left.  One
 * decoded bin consumes at most 6 bits (the smallest codIRangeLPS of a
 * context is 6), so a read-ahead of at least this many covers a bin.
 */
#define CABAC_AHEAD_MIN 8

/* The bits of one byte, which is the unit of the read-ahead. */
#define CABAC_BYTE_BITS 8

/* A context variable's pStateIdx is bits 0..5 of its byte, and valMPS is bit 6. */
#define CABAC_STATE_MASK 63U
#define CABAC_MPS_SHIFT 6U

/* The largest SliceQPY the initialization uses (9-5: Clip3( 0, 51, SliceQPY )). */
#define CABAC_QP_MAX 51

/* The bounds of preCtxState (9-5). */
#define CABAC_PRE_STATE_MIN 1
#define CABAC_PRE_STATE_MAX 126

/* The largest preCtxState that gives valMPS 0 (9-5). */
#define CABAC_PRE_STATE_MPS0_MAX 63

/*
 * An offset, a multiple of 16, that makes ( m * SliceQPY ) of 9-5
 * non-negative so that its ">> 4" can be computed as a division of a
 * non-negative number: m is at least -128 and the clipped SliceQPY at
 * most 51, so the product is at least -6528.
 */
#define CABAC_FLOOR_BIAS 8192

/* Static function declarations. */
static int cabac_engine_start(struct vcodec_cabac *cabac, size_t position);
static void cabac_ahead(struct vcodec_cabac *cabac);
static void cabac_renorm(struct vcodec_cabac *cabac);
static void cabac_overrun_check(struct vcodec_cabac *cabac);

/*
 * rangeTabLPS: codIRangeLPS for each pStateIdx (rows) and qCodIRangeIdx
 * (columns) (9-26).
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-44, pStateIdx 0 to 63, qCodIRangeIdx 0 to 3.
 * ITU-T H.265 (V11) (01/2026) Table 9-52 has the same values.
 */
const uint8_t vcodec_cabac_range_lps[64][4] = {
	/* 0 */ { 128, 176, 208, 240 },
	/* 1 */ { 128, 167, 197, 227 },
	/* 2 */ { 128, 158, 187, 216 },
	/* 3 */ { 123, 150, 178, 205 },
	/* 4 */ { 116, 142, 169, 195 },
	/* 5 */ { 111, 135, 160, 185 },
	/* 6 */ { 105, 128, 152, 175 },
	/* 7 */ { 100, 122, 144, 166 },
	/* 8 */ { 95, 116, 137, 158 },
	/* 9 */ { 90, 110, 130, 150 },
	/* 10 */ { 85, 104, 123, 142 },
	/* 11 */ { 81, 99, 117, 135 },
	/* 12 */ { 77, 94, 111, 128 },
	/* 13 */ { 73, 89, 105, 122 },
	/* 14 */ { 69, 85, 100, 116 },
	/* 15 */ { 66, 80, 95, 110 },
	/* 16 */ { 62, 76, 90, 104 },
	/* 17 */ { 59, 72, 86, 99 },
	/* 18 */ { 56, 69, 81, 94 },
	/* 19 */ { 53, 65, 77, 89 },
	/* 20 */ { 51, 62, 73, 85 },
	/* 21 */ { 48, 59, 69, 80 },
	/* 22 */ { 46, 56, 66, 76 },
	/* 23 */ { 43, 53, 63, 72 },
	/* 24 */ { 41, 50, 59, 69 },
	/* 25 */ { 39, 48, 56, 65 },
	/* 26 */ { 37, 45, 54, 62 },
	/* 27 */ { 35, 43, 51, 59 },
	/* 28 */ { 33, 41, 48, 56 },
	/* 29 */ { 32, 39, 46, 53 },
	/* 30 */ { 30, 37, 43, 50 },
	/* 31 */ { 29, 35, 41, 48 },
	/* 32 */ { 27, 33, 39, 45 },
	/* 33 */ { 26, 31, 37, 43 },
	/* 34 */ { 24, 30, 35, 41 },
	/* 35 */ { 23, 28, 33, 39 },
	/* 36 */ { 22, 27, 32, 37 },
	/* 37 */ { 21, 26, 30, 35 },
	/* 38 */ { 20, 24, 29, 33 },
	/* 39 */ { 19, 23, 27, 31 },
	/* 40 */ { 18, 22, 26, 30 },
	/* 41 */ { 17, 21, 25, 28 },
	/* 42 */ { 16, 20, 23, 27 },
	/* 43 */ { 15, 19, 22, 25 },
	/* 44 */ { 14, 18, 21, 24 },
	/* 45 */ { 14, 17, 20, 23 },
	/* 46 */ { 13, 16, 19, 22 },
	/* 47 */ { 12, 15, 18, 21 },
	/* 48 */ { 12, 14, 17, 20 },
	/* 49 */ { 11, 14, 16, 19 },
	/* 50 */ { 11, 13, 15, 18 },
	/* 51 */ { 10, 12, 15, 17 },
	/* 52 */ { 10, 12, 14, 16 },
	/* 53 */ { 9, 11, 13, 15 },
	/* 54 */ { 9, 11, 12, 14 },
	/* 55 */ { 8, 10, 12, 14 },
	/* 56 */ { 8, 9, 11, 13 },
	/* 57 */ { 7, 9, 11, 12 },
	/* 58 */ { 7, 9, 10, 12 },
	/* 59 */ { 7, 8, 10, 11 },
	/* 60 */ { 6, 8, 9, 11 },
	/* 61 */ { 6, 7, 9, 10 },
	/* 62 */ { 6, 7, 8, 9 },
	/* 63 */ { 2, 2, 2, 2 }
};

/*
 * transIdxLPS: the pStateIdx after a least probable symbol (9-27).
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-45, row transIdxLPS, pStateIdx 0 to 63.
 * ITU-T H.265 (V11) (01/2026) Table 9-53 has the same values.
 */
const uint8_t vcodec_cabac_trans_lps[64] = {
	/* 0..15 */ 0, 0, 1, 2, 2, 4, 4, 5, 6, 7, 8, 9, 9, 11, 11, 12,
	/* 16..31 */ 13, 13, 15, 15, 16, 16, 18, 18, 19, 19, 21, 21, 22, 22, 23, 24,
	/* 32..47 */ 24, 25, 26, 26, 27, 27, 28, 29, 29, 30, 30, 30, 31, 32, 32, 33,
	/* 48..63 */ 33, 33, 34, 34, 35, 35, 35, 36, 36, 36, 37, 37, 37, 38, 38, 63
};

/*
 * transIdxMPS: the pStateIdx after a most probable symbol (9-27).
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-45, row transIdxMPS, pStateIdx 0 to 63.
 * ITU-T H.265 (V11) (01/2026) Table 9-53 has the same values.
 */
const uint8_t vcodec_cabac_trans_mps[64] = {
	/* 0..15 */ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
	/* 16..31 */ 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
	/* 32..47 */ 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
	/* 48..63 */ 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 62, 63
};

/*
 * Derives pStateIdx and valMPS from (m, n) and SliceQPY (9-5) and packs
 * them as a context variable's byte.
 */
uint8_t
vcodec_cabac_context_init(
	int m,
	int n,
	int qp)
{
	int clipped_qp;
	int product;
	int shifted;
	int pre_state;
	int state_idx;
	int value_mps;

	/* Clip3( 0, 51, SliceQPY ): a QP below the range counts as 0 and one above it as 51. */
	clipped_qp = qp;
	if (clipped_qp < 0)
		clipped_qp = 0;
	if (clipped_qp > CABAC_QP_MAX)
		clipped_qp = CABAC_QP_MAX;

	/*
	 * ( m * Clip3( 0, 51, SliceQPY ) ) >> 4, with the shift rounding
	 * toward minus infinity: the product is made non-negative first so
	 * that no right shift of a negative number is needed.
	 */
	product = m * clipped_qp;
	shifted = (product + CABAC_FLOOR_BIAS) / 16 - CABAC_FLOOR_BIAS / 16;

	/* preCtxState = Clip3( 1, 126, shifted + n ): the two ends keep a context from starting in a state that never adapts. */
	pre_state = shifted + n;
	if (pre_state < CABAC_PRE_STATE_MIN)
		pre_state = CABAC_PRE_STATE_MIN;
	if (pre_state > CABAC_PRE_STATE_MAX)
		pre_state = CABAC_PRE_STATE_MAX;

	/* Splits preCtxState into pStateIdx and valMPS. */
	if (pre_state <= CABAC_PRE_STATE_MPS0_MAX) {
		state_idx = 63 - pre_state;
		value_mps = 0;
	} else {
		state_idx = pre_state - 64;
		value_mps = 1;
	}

	/* Reports the packed state. */
	return (uint8_t)((value_mps << CABAC_MPS_SHIFT) | state_idx);
}

/*
 * Starts the engine on the bytes of data (9.3.1.2).
 *
 * Every failure leaves error set and the engine unusable, so a caller
 * that does not look at the return value still stops at its next check
 * of error.
 */
int
vcodec_cabac_start(
	struct vcodec_cabac *cabac,
	const uint8_t *data,
	size_t size)
{
	int status;

	/* An engine that was never started reads nothing and reports an error. */
	memset(cabac, 0, sizeof(*cabac));
	cabac->error = EINVAL;

	/* Refuses bytes that are named but missing. */
	if (data == NULL && size != 0U)
		return EINVAL;

	/* Takes over the bytes; the engine reads nothing beyond them. */
	cabac->data = data;
	cabac->size = size;

	/* Initializes the engine from the first bits (9.3.1.2). */
	status = cabac_engine_start(cabac, 0U);
	if (status != 0)
		return status;

	/* Succeeded: the engine is ready to decode the first bin. */
	return 0;
}

/*
 * Decodes one bin with a context variable (DecodeDecision, 9.3.3.2.1).
 *
 * pStateIdx and valMPS are bits of one byte, *context, and the new
 * values are stored back in one write.
 */
unsigned
vcodec_cabac_decision(
	struct vcodec_cabac *cabac,
	uint8_t *context)
{
	unsigned state_idx;
	unsigned value_mps;
	unsigned quantized;
	unsigned range_lps;
	unsigned bin;
	uint32_t range_scaled;

	/* Reads the context variable. */
	state_idx = (unsigned)*context & CABAC_STATE_MASK;
	value_mps = (unsigned)*context >> CABAC_MPS_SHIFT;

	/* Makes sure the bits this bin may consume are already read ahead. */
	cabac_ahead(cabac);

	/* Looks up codIRangeLPS by (9-25) and (9-26), and takes it off codIRange. */
	quantized = (cabac->range >> 6) & 3U;
	range_lps = vcodec_cabac_range_lps[state_idx][quantized];
	cabac->range -= range_lps;
	range_scaled = cabac->range << cabac->bits_left;

	/*
	 * Decides between the most and the least probable symbol, with the
	 * state transition of 9-27.  The LPS takes the upper part of the
	 * interval, so it also lowers codIOffset and shrinks codIRange.
	 */
	if (cabac->offset >= range_scaled) {
		bin = 1U - value_mps;
		cabac->offset -= range_scaled;
		cabac->range = range_lps;

		/* The least probable symbol of the equiprobable state becomes the most probable one. */
		if (state_idx == 0U)
			value_mps = 1U - value_mps;

		state_idx = vcodec_cabac_trans_lps[state_idx];
	} else {
		bin = value_mps;
		state_idx = vcodec_cabac_trans_mps[state_idx];
	}

	/* Stores the context variable's new state. */
	*context = (uint8_t)((value_mps << CABAC_MPS_SHIFT) | state_idx);

	/* Renormalizes (9.3.3.2.2) and notes a read beyond the end of the data. */
	cabac_renorm(cabac);
	cabac_overrun_check(cabac);

	/* Reports the decoded bin. */
	return bin;
}

/* Decodes one bin with equal probabilities (DecodeBypass, 9.3.3.2.3); returns 0 or 1. */
unsigned
vcodec_cabac_bypass(
	struct vcodec_cabac *cabac)
{
	unsigned bin;
	uint32_t range_scaled;

	/* Makes sure the bit this bin consumes is already read ahead. */
	cabac_ahead(cabac);

	/* Shifts one bit into codIOffset: that only lowers the read-ahead count. */
	cabac->bits_left--;
	range_scaled = cabac->range << cabac->bits_left;

	/* Decides the bin by comparing codIOffset with codIRange. */
	bin = 0U;
	if (cabac->offset >= range_scaled) {
		bin = 1U;
		cabac->offset -= range_scaled;
	}

	/* Notes a read beyond the end of the data. */
	cabac_overrun_check(cabac);

	/* Reports the decoded bin. */
	return bin;
}

/*
 * Decodes the bin of end_of_slice_flag or of mb_type I_PCM (DecodeTerminate, 9.3.3.2.4).
 *
 * A bin of 1 consumes nothing more, so vcodec_cabac_pcm finds the
 * position of the last bit read into codIOffset from the counts alone.
 */
unsigned
vcodec_cabac_terminate(
	struct vcodec_cabac *cabac)
{
	uint32_t range_scaled;

	/* Makes sure the bit a renormalization may consume is already read ahead. */
	cabac_ahead(cabac);

	/* Takes 2 off codIRange and compares it with codIOffset. */
	cabac->range -= 2U;
	range_scaled = cabac->range << cabac->bits_left;

	/* A bin of 1 ends the arithmetic decoding: nothing is renormalized or read. */
	if (cabac->offset >= range_scaled)
		return 1U;

	/* A bin of 0 renormalizes (9.3.3.2.2) and notes a read beyond the end of the data. */
	cabac_renorm(cabac);
	cabac_overrun_check(cabac);

	/* Reports a bin of 0: the arithmetic decoding goes on. */
	return 0U;
}

/*
 * Reads the samples of an I_PCM macroblock and restarts the engine (7.3.5, 9.3.1.2).
 *
 * The bits the 9-bit model of the clauses has read are the bits the
 * engine has taken from the data (8 * position, counting zero bytes
 * supplied past the end) less the bits_left that are not yet part of
 * codIOffset.
 */
int
vcodec_cabac_pcm(
	struct vcodec_cabac *cabac,
	uint8_t *samples,
	size_t count)
{
	size_t model_bits;
	size_t first;
	int status;

	/* Refuses an engine that has read beyond the data or never started. */
	if (cabac->error != 0)
		return EINVAL;

	/* Finds the bit just after the last bit read into codIOffset (9.3.3.2.4). */
	model_bits = cabac->position * 8U - (size_t)cabac->bits_left;

	/* The pcm_alignment_zero_bit bits take it to the byte boundary where the samples start. */
	first = (model_bits + 7U) / 8U;

	/* Refuses samples that do not all lie inside the data. */
	if (first > cabac->size || count > cabac->size - first) {
		cabac->error = EINVAL;
		return EINVAL;
	}

	/* Refuses a missing destination for samples that are wanted. */
	if (count != 0U && samples == NULL) {
		cabac->error = EINVAL;
		return EINVAL;
	}

	/* Copies the pcm_sample_luma and pcm_sample_chroma bytes out. */
	if (count != 0U)
		memcpy(samples, cabac->data + first, count);

	/* Initializes the engine again from the bytes after the samples (9.3.1.2). */
	status = cabac_engine_start(cabac, first + count);
	if (status != 0)
		return status;

	/* Succeeded: the engine decodes the macroblock that follows. */
	return 0;
}

/*
 * Initializes the arithmetic decoding engine from the bytes at position
 * (9.3.1.2): codIRange 510 and codIOffset the next 9 bits.
 *
 * Two bytes are taken: the 9 bits of codIOffset and 7 bits read ahead.
 */
static int
cabac_engine_start(
	struct vcodec_cabac *cabac,
	size_t position)
{
	uint32_t first_byte;
	uint32_t second_byte;

	/* Refuses data that ends before the 9 bits of codIOffset do, or a position beyond it. */
	if (position > cabac->size || cabac->size - position < VCODEC_CABAC_START_BYTES) {
		cabac->error = EINVAL;
		return EINVAL;
	}

	/* Reads the two bytes: codIOffset is their first 9 bits. */
	first_byte = cabac->data[position];
	second_byte = cabac->data[position + 1U];
	cabac->offset = (first_byte << CABAC_BYTE_BITS) | second_byte;
	cabac->bits_left = CABAC_START_AHEAD;
	cabac->position = position + VCODEC_CABAC_START_BYTES;
	cabac->range = CABAC_RANGE_INIT;
	cabac->error = 0;

	/* The bitstream shall not start with a codIOffset of 510 or 511. */
	if ((cabac->offset >> CABAC_START_AHEAD) >= CABAC_OFFSET_FORBIDDEN_LOW) {
		cabac->error = EINVAL;
		return EINVAL;
	}

	/* Succeeded: codIOffset is below codIRange. */
	return 0;
}

/*
 * Reads one more byte ahead when fewer than CABAC_AHEAD_MIN bits are left.
 *
 * Past the end of the data the bytes are zeros.  position counts those
 * padding bytes too, so that the bits the model has consumed can still be
 * told from the bits only read ahead.
 */
static void
cabac_ahead(
	struct vcodec_cabac *cabac)
{
	uint32_t next_byte;

	/* Enough bits are left; nothing is read. */
	if (cabac->bits_left >= CABAC_AHEAD_MIN)
		return;

	/* Takes the next byte, or a zero byte past the end of the data. */
	next_byte = 0U;
	if (cabac->position < cabac->size)
		next_byte = cabac->data[cabac->position];
	cabac->position++;

	/* Appends it below the bits already read ahead. */
	cabac->offset = (cabac->offset << CABAC_BYTE_BITS) | next_byte;
	cabac->bits_left += CABAC_BYTE_BITS;
}

/*
 * Renormalizes (9.3.3.2.2): doubles codIRange while it is below 256, each
 * doubling shifting one bit into codIOffset, which only lowers bits_left.
 */
static void
cabac_renorm(
	struct vcodec_cabac *cabac)
{
	/* Doubles codIRange until it is at least 256. */
	while (cabac->range < CABAC_RANGE_RENORM) {
		cabac->range <<= 1;
		cabac->bits_left--;
	}
}

/*
 * Sets error when the 9-bit model of the clauses has consumed a bit past
 * the end of the data.
 *
 * The model has consumed 8 * position - bits_left bits and the data has
 * 8 * size, so a bit past the end is consumed when 8 * ( position - size )
 * is more than bits_left.  Before any padding byte is read nothing can be
 * past the end.
 */
static void
cabac_overrun_check(
	struct vcodec_cabac *cabac)
{
	size_t padding;

	/* No padding byte has been read, so no bit past the end has been consumed. */
	if (cabac->position <= cabac->size)
		return;

	/* A padding bit is consumed when the padding is more than the bits still read ahead. */
	padding = cabac->position - cabac->size;
	if (padding * 8U > (size_t)cabac->bits_left)
		cabac->error = EINVAL;
}
