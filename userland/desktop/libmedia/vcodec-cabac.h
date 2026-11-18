/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CABAC arithmetic decoding engine of libmedia's software video
 * decoders: the initialization of one context variable (H.264 9.3.1.1,
 * 9-5; H.265 9.3.2.2, 9-6) and of the engine (H.264 9.3.1.2; H.265
 * 9.3.2.6), and the three ways of decoding a bin (H.264 9.3.3.2:
 * DecodeDecision 9.3.3.2.1 with the renormalization of 9.3.3.2.2,
 * DecodeBypass 9.3.3.2.3 and DecodeTerminate 9.3.3.2.4; H.265 9.3.4.3.2
 * to 9.3.4.3.5 are the same), with rangeTabLPS (H.264 Table 9-44, H.265
 * Table 9-52) and the state transitions (H.264 Table 9-45, H.265 Table
 * 9-53).
 *
 * The engine knows nothing of a codec's contexts: a context variable is
 * one byte the caller keeps, pStateIdx in bits 0..5 and valMPS in bit 6,
 * and the caller passes the byte of the context it decodes with.  The
 * codec's numbering of its contexts, which of them a slice initializes
 * and the values of m and n are the codec's.
 *
 * The clause numbers below without a name are those of H.264.
 */

#ifndef LIBMEDIA_VCODEC_CABAC_H
#define LIBMEDIA_VCODEC_CABAC_H

#include <errno.h>

#include "vcodec.h"

/* The fewest bytes the engine starts from: they hold the 9 bits of codIOffset (9.3.1.2). */
#define VCODEC_CABAC_START_BYTES 2U

/*
 * A context variable's pStateIdx is bits 0..5 of its byte, and valMPS is
 * bit 6; bit 7 is 0 (the inline DecodeDecision reads the byte as if it
 * were).
 */
#define VCODEC_CABAC_STATE_MASK 63U
#define VCODEC_CABAC_MPS_SHIFT 6U
#define VCODEC_CABAC_CONTEXT_MASK 127U

/* The renormalization (9.3.3.2.2) doubles codIRange while it is below this. */
#define VCODEC_CABAC_RANGE_RENORM 256U

/*
 * A byte is read ahead when fewer than this many bits are left.  One
 * decoded bin consumes at most 6 bits (the smallest codIRangeLPS of a
 * context is 6), so a read-ahead of at least this many covers a bin.
 */
#define VCODEC_CABAC_AHEAD_MIN 8

/* The bits of one byte, which is the unit of the read-ahead. */
#define VCODEC_CABAC_BYTE_BITS 8

/*
 * rangeTabLPS: codIRangeLPS for each pStateIdx (rows) and qCodIRangeIdx
 * (columns), and the pStateIdx after a least and a most probable symbol
 * (transIdxLPS, transIdxMPS).  Also used by the encoding engines.
 */
extern const uint8_t vcodec_cabac_range_lps[64][4];
extern const uint8_t vcodec_cabac_trans_lps[64];
extern const uint8_t vcodec_cabac_trans_mps[64];

/*
 * Derived from those tables for the inline DecodeDecision below: the
 * number of doublings of the renormalization after a least probable
 * symbol, by pStateIdx and qCodIRangeIdx, and a context variable's byte
 * after a most and a least probable symbol, by its byte before.
 */
extern const uint8_t vcodec_cabac_lps_shift[64][4];
extern const uint8_t vcodec_cabac_next_state[2][128];

/*
 * One slice's arithmetic decoder.
 *
 * data and size are the bytes from the byte where the arithmetic
 * decoding starts to the end of the RBSP; position is the next byte to
 * read.  range and offset are codIRange and codIOffset (9 bits each in
 * the clause's terms; an implementation may keep offset scaled with more
 * bits read ahead, as long as the decoded bins are the same).  bits_left
 * belongs to such an implementation (the read-ahead bits not yet
 * consumed).
 *
 * When the 9-bit model of the clauses would read a bit beyond the end
 * of data, zero bits are supplied and error is set (a conforming slice
 * ends with its last terminate bin before that): the bins read then are
 * garbage, and the caller stops at its next check of error.  An engine
 * that reads ahead of the model pads with zeros without setting error
 * for bits the model has not consumed.
 */
struct vcodec_cabac {
	const uint8_t *data;
	size_t size;
	size_t position;
	uint32_t range;
	uint32_t offset;
	int bits_left;
	int error;
};

/*
 * Returns the byte of a context variable initialized from m, n and
 * SliceQPY (9-5): preCtxState = Clip3( 1, 126, ( ( m * Clip3( 0, 51,
 * SliceQPY ) ) >> 4 ) + n ), then valMPS and pStateIdx from it.
 *
 * m and n are -128..127 (the values of the H.264 tables of 9.3.1.1 and
 * those H.265 derives by its 9-5 lie in that range); qp may be any value
 * (it is clipped).
 */
uint8_t vcodec_cabac_context_init(int m, int n, int qp);

/*
 * Starts the engine on data (9.3.1.2): codIRange 510 and codIOffset the
 * first 9 bits.
 *
 * Clears every field of engine and sets error to EINVAL first, so a
 * failed start leaves an engine that reads nothing.  data is NULL only
 * when size is 0.  Returns 0; EINVAL when size is below
 * VCODEC_CABAC_START_BYTES or codIOffset is 510 or 511 (which a
 * bitstream shall not hold).
 */
int vcodec_cabac_start(struct vcodec_cabac *engine, const uint8_t *data, size_t size);

/*
 * Decodes one bin with the context variable *context (DecodeDecision, 9.3.3.2.1).
 *
 * Updates *context by Table 9-45 and renormalizes (9.3.3.2.2).  Returns
 * the bin, 0 or 1.
 */
unsigned vcodec_cabac_decision(struct vcodec_cabac *engine, uint8_t *context);

/* Decodes one bin with equal probabilities (DecodeBypass, 9.3.3.2.3); returns 0 or 1. */
unsigned vcodec_cabac_bypass(struct vcodec_cabac *engine);

/*
 * Decodes a bin that may end the arithmetic decoding: H.264's
 * end_of_slice_flag and mb_type I_PCM (DecodeTerminate, 9.3.3.2.4).
 *
 * Returns 1 when the bin is 1: the arithmetic decoding ends there, and
 * the last bit read into codIOffset is the rbsp_stop_one_bit when the
 * bin ended the slice (9.3.3.2.4), so nothing more is read; for PCM
 * samples, vcodec_cabac_pcm goes on from that position.  Returns 0
 * otherwise, after renormalizing (9.3.3.2.2).
 */
unsigned vcodec_cabac_terminate(struct vcodec_cabac *engine);

/*
 * Reads 8-bit PCM samples and restarts the engine (H.264 7.3.5, 9.3.1.2).
 *
 * Called after vcodec_cabac_terminate returned 1 for the bin that
 * introduces the samples.  Finds the bit position the arithmetic
 * decoding has reached: in the 9-bit model of the clauses, just after
 * the last bit read into codIOffset (9.3.3.2.4; an engine that reads
 * ahead gives those bits back).  The alignment zero bits from there to
 * the byte boundary are skipped (not checked), then count bytes are
 * read into samples, and the engine is initialized again (9.3.1.2) from
 * the following bytes; the context variables, which the caller keeps,
 * are not touched.  Returns 0, or EINVAL when the data ends first, when
 * the engine had already failed, or when samples is NULL and count is
 * not 0.
 */
int vcodec_cabac_pcm(struct vcodec_cabac *engine, uint8_t *samples, size_t count);

/*
 * The inline forms of DecodeDecision and DecodeBypass follow.  A
 * decoder's hot loops call them so that a bin costs no call; the
 * functions above give the same bins and the same engine state, and are
 * built from them.
 *
 * The engine keeps codIOffset scaled: offset holds the 9-bit codIOffset
 * of the clauses followed by bits_left bits that were read ahead of it,
 * so offset = ( codIOffset << bits_left ) | the read-ahead bits.  A
 * comparison of codIOffset with a range is then a comparison of offset
 * with ( range << bits_left ), and a renormalization that shifts one bit
 * into codIOffset only lowers bits_left.
 */

/*
 * Reads one more byte ahead when fewer than VCODEC_CABAC_AHEAD_MIN bits are left.
 *
 * Past the end of the data the bytes are zeros.  position counts those
 * padding bytes too, so that the bits the model has consumed can still be
 * told from the bits only read ahead.  The next byte is taken whether it
 * is wanted or not and applied through a mask, so that the read-ahead
 * needs no branch on the bits left, which a decoder could not predict.
 */
static __inline void
vcodec_cabac_ahead(
	struct vcodec_cabac *engine)
{
	uint32_t next_byte;
	uint32_t wanted;
	uint32_t wanted_mask;

	/* Takes the next byte, or a zero byte past the end of the data. */
	next_byte = 0U;
	if (engine->position < engine->size)
		next_byte = engine->data[engine->position];

	/* The byte is wanted when fewer than VCODEC_CABAC_AHEAD_MIN bits are left; wanted_mask is then all ones. */
	wanted = 0U;
	if (engine->bits_left < VCODEC_CABAC_AHEAD_MIN)
		wanted = 1U;
	wanted_mask = 0U - wanted;

	/* Appends a wanted byte below the bits already read ahead; an unwanted one changes nothing. */
	engine->offset = (engine->offset << (VCODEC_CABAC_BYTE_BITS & wanted_mask)) | (next_byte & wanted_mask);
	engine->position += wanted;
	engine->bits_left += (int)(VCODEC_CABAC_BYTE_BITS & wanted_mask);
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
static __inline void
vcodec_cabac_overrun_check(
	struct vcodec_cabac *engine)
{
	size_t padding;

	/* No padding byte has been read, so no bit past the end has been consumed. */
	if (engine->position <= engine->size)
		return;

	/* A padding bit is consumed when the padding is more than the bits still read ahead. */
	padding = engine->position - engine->size;
	if (padding * 8U > (size_t)engine->bits_left)
		engine->error = EINVAL;
}

/*
 * Decodes one bin with the context variable *context (DecodeDecision,
 * 9.3.3.2.1, with the renormalization of 9.3.3.2.2) without noting a
 * read beyond the end of the data.
 *
 * The bits the 9-bit model has consumed only grow from bin to bin, so a
 * caller that decodes a run of bins with this function and then calls
 * vcodec_cabac_overrun_check sets error exactly as a check after each
 * bin would; it must do so before anything reads error.
 *
 * The context's new byte comes from vcodec_cabac_next_state and the
 * doublings after a least probable symbol from vcodec_cabac_lps_shift,
 * which give what the clauses' steps give.  codIRange, codIOffset and
 * the read-ahead count are worked on in locals, and the context's byte
 * is stored before the engine's fields: a later bin then reads the
 * fields the compiler still holds, which a store through the byte
 * pointer (which may alias anything) would otherwise make it read again
 * from memory.  Returns the bin, 0 or 1.
 */
static __inline unsigned
vcodec_cabac_decision_nocheck(
	struct vcodec_cabac *engine,
	uint8_t *context)
{
	unsigned packed;
	unsigned state_idx;
	unsigned quantized;
	unsigned lps;
	unsigned lps_mask;
	unsigned shift_mps;
	unsigned shift_lps;
	unsigned shift;
	unsigned bin;
	uint32_t range_lps;
	uint32_t range_mps;
	uint32_t range;
	uint32_t range_scaled;
	uint32_t offset;
	int bits_left;

	/* Reads the context variable: pStateIdx in bits 0..5, valMPS in bit 6. */
	packed = (unsigned)*context & VCODEC_CABAC_CONTEXT_MASK;
	state_idx = packed & VCODEC_CABAC_STATE_MASK;

	/* Makes sure the bits this bin may consume are already read ahead. */
	vcodec_cabac_ahead(engine);

	/* Looks up codIRangeLPS by (9-25) and (9-26); codIRange less it is the most probable symbol's part. */
	range = engine->range;
	offset = engine->offset;
	bits_left = engine->bits_left;
	quantized = (range >> 6) & 3U;
	range_lps = vcodec_cabac_range_lps[state_idx][quantized];
	range_mps = range - range_lps;
	range_scaled = range_mps << bits_left;

	/* The least probable symbol takes the upper part of the interval: codIOffset at or above the most probable part. */
	lps = 0U;
	if (offset >= range_scaled)
		lps = 1U;

	/*
	 * Applies the symbol without a branch on it, which a decoder could
	 * not predict: lps_mask is all ones for the least probable symbol.
	 * That symbol lowers codIOffset by the most probable part and leaves
	 * codIRange at codIRangeLPS; the most probable one leaves codIRange
	 * at its part.
	 */
	lps_mask = 0U - lps;
	offset -= range_scaled & lps_mask;
	range = range_mps ^ ((range_mps ^ range_lps) & lps_mask);
	bin = (packed >> VCODEC_CABAC_MPS_SHIFT) ^ lps;
	*context = vcodec_cabac_next_state[lps][packed];

	/*
	 * Counts the doublings of the renormalization (9.3.3.2.2) after a
	 * most probable symbol: it leaves codIRange at 128 or more when that
	 * was 256 to 510 before the bin, as in a running engine, so it needs
	 * at most one here.
	 */
	shift_mps = 0U;
	if (range_mps < VCODEC_CABAC_RANGE_RENORM)
		shift_mps = 1U;

	/*
	 * Renormalizes (9.3.3.2.2) by the symbol's count, the least probable
	 * symbol's from the table: each doubling of codIRange shifts one bit
	 * into codIOffset, which only lowers bits_left.
	 */
	shift_lps = vcodec_cabac_lps_shift[state_idx][quantized];
	shift = shift_mps ^ ((shift_mps ^ shift_lps) & lps_mask);
	range <<= shift;
	bits_left -= (int)shift;

	/* An engine whose codIRange was below 256 before the bin (one that has not started) doubles on until the clauses' loop would stop. */
	while (range < VCODEC_CABAC_RANGE_RENORM) {
		range <<= 1;
		bits_left--;
	}

	/* Stores the engine's new state. */
	engine->range = range;
	engine->offset = offset;
	engine->bits_left = bits_left;

	/* Reports the decoded bin. */
	return bin;
}

/*
 * Decodes one bin with the context variable *context (DecodeDecision,
 * 9.3.3.2.1, with the renormalization of 9.3.3.2.2), as
 * vcodec_cabac_decision does: vcodec_cabac_decision_nocheck, then the
 * check for a read beyond the end of the data.  Returns the bin, 0 or 1.
 */
static __inline unsigned
vcodec_cabac_decision_inline(
	struct vcodec_cabac *engine,
	uint8_t *context)
{
	unsigned bin;

	/* Decodes the bin. */
	bin = vcodec_cabac_decision_nocheck(engine, context);

	/* Notes a read beyond the end of the data. */
	vcodec_cabac_overrun_check(engine);

	/* Reports the decoded bin. */
	return bin;
}

/*
 * Decodes one bin with equal probabilities (DecodeBypass, 9.3.3.2.3)
 * without noting a read beyond the end of the data (the caller checks
 * as for vcodec_cabac_decision_nocheck); returns 0 or 1.
 */
static __inline unsigned
vcodec_cabac_bypass_nocheck(
	struct vcodec_cabac *engine)
{
	unsigned bin;
	uint32_t range_scaled;

	/* Makes sure the bit this bin consumes is already read ahead. */
	vcodec_cabac_ahead(engine);

	/* Shifts one bit into codIOffset: that only lowers the read-ahead count. */
	engine->bits_left--;
	range_scaled = engine->range << engine->bits_left;

	/* Decides the bin by comparing codIOffset with codIRange. */
	bin = 0U;
	if (engine->offset >= range_scaled) {
		bin = 1U;
		engine->offset -= range_scaled;
	}

	/* Reports the decoded bin. */
	return bin;
}

/*
 * Decodes one bin with equal probabilities (DecodeBypass, 9.3.3.2.3), as
 * vcodec_cabac_bypass does: vcodec_cabac_bypass_nocheck, then the check
 * for a read beyond the end of the data; returns 0 or 1.
 */
static __inline unsigned
vcodec_cabac_bypass_inline(
	struct vcodec_cabac *engine)
{
	unsigned bin;

	/* Decodes the bin. */
	bin = vcodec_cabac_bypass_nocheck(engine);

	/* Notes a read beyond the end of the data. */
	vcodec_cabac_overrun_check(engine);

	/* Reports the decoded bin. */
	return bin;
}

#endif
