/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * An arithmetic encoder for the host unit tests of the software H.264
 * decoder's CABAC parts: the informative encoding engine of 9.3.4
 * (InitEncoder, EncodeDecision, EncodeBypass, EncodeTerminate with the
 * flush, and PutBit with its outstanding bits), written from the flow
 * charts of the specification, with its own copy of Tables 9-44 and 9-45
 * so that an error in the decoder's tables shows up as a stream the
 * decoder cannot read back.  It writes into a struct bitwriter
 * (bitwriter.h).
 *
 * The tests give the encoder the bins they want coded, with the contexts,
 * and decode the bytes with the decoder under test.
 */

#ifndef H264SW_TEST_CABACWRITER_H
#define H264SW_TEST_CABACWRITER_H

#include <stddef.h>
#include <stdint.h>

#include "bitwriter.h"

/* The context variables the encoder keeps, as many as the decoder's (ctxIdx 0..459). */
#define CABACWRITER_CONTEXTS 460U

/*
 * The state of one slice's encoder: codILow and codIRange of 9.3.4.1,
 * firstBitFlag and bitsOutstanding of PutBit (9.3.4.3), the context
 * variables (pStateIdx in bits 0..5 and valMPS in bit 6 of each byte)
 * and the writer the bits go to.
 */
struct cabacwriter {
	struct bitwriter *out;
	uint32_t low;
	uint32_t range;
	int first_bit;
	unsigned outstanding;
	uint8_t state[CABACWRITER_CONTEXTS];
};

/*
 * rangeTabLPS, the encoder's own copy.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-44, pStateIdx 0 to 63 in rows, qCodIRangeIdx 0 to 3 in columns.
 */
static const uint8_t cabacwriter_range_lps[64][4] = {
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
 * transIdxLPS and transIdxMPS, the encoder's own copy.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-45, pStateIdx 0 to 63.
 */
static const uint8_t cabacwriter_trans_lps[64] = {
	/* 0..15 */ 0, 0, 1, 2, 2, 4, 4, 5, 6, 7, 8, 9, 9, 11, 11, 12,
	/* 16..31 */ 13, 13, 15, 15, 16, 16, 18, 18, 19, 19, 21, 21, 22, 22, 23, 24,
	/* 32..47 */ 24, 25, 26, 26, 27, 27, 28, 29, 29, 30, 30, 30, 31, 32, 32, 33,
	/* 48..63 */ 33, 33, 34, 34, 35, 35, 35, 36, 36, 36, 37, 37, 37, 38, 38, 63
};

static const uint8_t cabacwriter_trans_mps[64] = {
	/* 0..15 */ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
	/* 16..31 */ 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
	/* 32..47 */ 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
	/* 48..63 */ 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 62, 63
};

/*
 * Sets one context variable from (m, n) and SliceQPY as 9-5 says:
 * preCtxState = Clip3( 1, 126, ( ( m * Clip3( 0, 51, SliceQPY ) ) >> 4 ) + n ).
 *
 * The shift is written as a division that rounds toward minus infinity
 * so that it does not depend on how the host shifts negative numbers.
 */
static __inline void
cabacwriter_context_set(
	struct cabacwriter *writer,
	unsigned ctx_idx,
	int m,
	int n,
	int qp)
{
	int product;
	int shifted;
	int pre_state;

	/* SliceQPY is clipped to 0..51 first. */
	if (qp < 0)
		qp = 0;
	if (qp > 51)
		qp = 51;

	/* The product shifted right by 4, rounded down. */
	product = m * qp;
	if (product >= 0) {
		shifted = product / 16;
	} else {
		shifted = -((-product + 15) / 16);
	}

	/* preCtxState, clipped to 1..126. */
	pre_state = shifted + n;
	if (pre_state < 1)
		pre_state = 1;
	if (pre_state > 126)
		pre_state = 126;

	/* A state of at most 63 has valMPS 0, a higher one valMPS 1. */
	if (pre_state <= 63) {
		writer->state[ctx_idx] = (uint8_t)(63 - pre_state);
	} else {
		writer->state[ctx_idx] = (uint8_t)(64U | (unsigned)(pre_state - 64));
	}
}

/* Writes one bit through PutBit (9.3.4.3): the first bit is dropped, and outstanding bits follow it inverted. */
static __inline void
cabacwriter_put_bit(
	struct cabacwriter *writer,
	unsigned bit)
{
	/* The first bit of the slice is not written. */
	if (writer->first_bit != 0) {
		writer->first_bit = 0;
	} else {
		bitwriter_u(writer->out, bit, 1U);
	}

	/* The bits held back by carry propagation come out inverted. */
	while (writer->outstanding > 0U) {
		bitwriter_u(writer->out, 1U - bit, 1U);
		writer->outstanding--;
	}
}

/* Renormalizes the encoder (RenormE, 9.3.4.3). */
static __inline void
cabacwriter_renorm(
	struct cabacwriter *writer)
{
	/* Doubles the range until it is at least 256, putting out the settled top bit of low each time. */
	while (writer->range < 256U) {
		if (writer->low < 256U) {
			cabacwriter_put_bit(writer, 0U);
		} else if (writer->low >= 512U) {
			writer->low -= 512U;
			cabacwriter_put_bit(writer, 1U);
		} else {
			writer->low -= 256U;
			writer->outstanding++;
		}

		writer->range <<= 1;
		writer->low <<= 1;
	}
}

/*
 * Starts the encoder on an output (InitEncoder, 9.3.4.1), keeping the
 * context variables: it is also the restart after I_PCM samples.
 */
static __inline void
cabacwriter_engine_init(
	struct cabacwriter *writer,
	struct bitwriter *out)
{
	/* codILow 0, codIRange 510, firstBitFlag 1, bitsOutstanding 0. */
	writer->out = out;
	writer->low = 0U;
	writer->range = 510U;
	writer->first_bit = 1;
	writer->outstanding = 0U;
}

/* Encodes one decision with the context ctx_idx (EncodeDecision, 9.3.4.2). */
static __inline void
cabacwriter_decision(
	struct cabacwriter *writer,
	unsigned ctx_idx,
	unsigned bin)
{
	unsigned state_idx;
	unsigned value_mps;
	unsigned range_lps;

	/* The context's state, and codIRangeLPS from the quantized range (9-25, 9-26). */
	state_idx = (unsigned)writer->state[ctx_idx] & 63U;
	value_mps = (unsigned)writer->state[ctx_idx] >> 6;
	range_lps = cabacwriter_range_lps[state_idx][(writer->range >> 6) & 3U];
	writer->range -= range_lps;

	/* A symbol other than the most probable one takes the upper sub-interval. */
	if (bin != value_mps) {
		writer->low += writer->range;
		writer->range = range_lps;
		if (state_idx == 0U)
			value_mps = 1U - value_mps;
		state_idx = cabacwriter_trans_lps[state_idx];
	} else {
		state_idx = cabacwriter_trans_mps[state_idx];
	}

	/* Stores the new state and renormalizes. */
	writer->state[ctx_idx] = (uint8_t)((value_mps << 6) | state_idx);
	cabacwriter_renorm(writer);
}

/* Encodes one bin with equal probabilities (EncodeBypass, 9.3.4.4). */
static __inline void
cabacwriter_bypass(
	struct cabacwriter *writer,
	unsigned bin)
{
	/* Doubles low and adds the range for a bin of 1. */
	writer->low <<= 1;
	if (bin != 0U)
		writer->low += writer->range;

	/* Settles the top bit of low as the flow chart of Figure 9-10 says. */
	if (writer->low >= 1024U) {
		cabacwriter_put_bit(writer, 1U);
		writer->low -= 1024U;
	} else if (writer->low < 512U) {
		cabacwriter_put_bit(writer, 0U);
	} else {
		writer->low -= 512U;
		writer->outstanding++;
	}
}

/*
 * Encodes the bin of end_of_slice_flag or of the I_PCM mb_type
 * (EncodeTerminate, 9.3.4.5); a bin of 1 flushes the encoder (Figure 9-12),
 * whose last bit written is a 1.
 */
static __inline void
cabacwriter_terminate(
	struct cabacwriter *writer,
	unsigned bin)
{
	/* Takes 2 off the range. */
	writer->range -= 2U;

	/* A bin of 0 only renormalizes. */
	if (bin == 0U) {
		cabacwriter_renorm(writer);
		return;
	}

	/* A bin of 1 moves low up and flushes: range 2, renormalize, then 10 bits of low are put out. */
	writer->low += writer->range;
	writer->range = 2U;
	cabacwriter_renorm(writer);
	cabacwriter_put_bit(writer, (writer->low >> 9) & 1U);
	bitwriter_u(writer->out, ((writer->low >> 7) & 3U) | 1U, 2U);
}

/*
 * Writes the I_PCM part that follows a terminate bin of 1: the
 * pcm_alignment_zero_bit bits, the samples, and a restarted encoder (9.3.4.1).
 */
static __inline void
cabacwriter_pcm(
	struct cabacwriter *writer,
	const uint8_t *samples,
	size_t count)
{
	size_t index;

	/* Zero bits up to the byte boundary. */
	while (writer->out->bits % 8U != 0U)
		bitwriter_u(writer->out, 0U, 1U);

	/* The samples, a byte each. */
	for (index = 0U; index < count; index++)
		bitwriter_u(writer->out, samples[index], 8U);

	/* The encoder starts again on the same output. */
	cabacwriter_engine_init(writer, writer->out);
}

#endif
