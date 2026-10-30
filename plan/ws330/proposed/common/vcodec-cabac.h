/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CABAC arithmetic decoding engine of libmedia's software video
 * decoders.  ITU-T H.264 9.3.1.2 and 9.3.3.2 and ITU-T H.265 9.3.2.6 and
 * 9.3.4.3 define the same engine with the same tables (rangeTabLPS: H.264
 * Table 9-44, H.265 Table 9-52; the state transitions: H.264 Table 9-45,
 * H.265 Table 9-53) and the same derivation of a context variable's
 * initial state from m, n and the slice QP (H.264 9-5, H.265 9-6).
 * Only the set of context variables, their (m, n) values and the
 * binarizations differ, and those stay with each codec: the engine takes
 * a pointer to the context variable it decodes with.
 *
 * A context variable is one byte: pStateIdx in bits 0..5 and valMps in
 * bit 6, as the encoder's engine (vcodec-cabac-enc.h) keeps them.
 */

#ifndef LIBMEDIA_VCODEC_CABAC_H
#define LIBMEDIA_VCODEC_CABAC_H

#include "vcodec.h"

/* The bit of a context variable's byte that holds valMps; pStateIdx is the byte & 63. */
#define VCODEC_CABAC_MPS 64U

/* The fewest bytes the engine's initialization needs (the 9 bits of the offset). */
#define VCODEC_CABAC_START_BYTES 2U

/*
 * rangeTabLPS[ pStateIdx ][ qRangeIdx ] and the transitions transIdxLps
 * and transIdxMps, the same in both codecs.
 *
 * Values referenced: ITU-T H.264 (08/2021) Tables 9-44 and 9-45; ITU-T
 * H.265 (01/2026) Tables 9-52 and 9-53 (equal).
 */
extern const uint8_t vcodec_cabac_range_lps[64][4];
extern const uint8_t vcodec_cabac_trans_lps[64];
extern const uint8_t vcodec_cabac_trans_mps[64];

/*
 * One arithmetic decoder over one run of bytes.
 *
 * data and size are the bytes from the byte where arithmetic decoding
 * starts to the end of the RBSP; position is the next byte to read.
 * range and offset are codIRange and codIOffset (H.264) or ivlCurrRange
 * and ivlOffset (H.265), 9 bits each in the clauses' model; an
 * implementation may keep offset scaled with more bits read ahead
 * (bits_left counts them), as long as every decoded bin and every
 * position vcodec_cabac_position reports are those of the 9-bit model.
 *
 * When the 9-bit model would read a bit beyond the end of data, zero bits
 * are supplied and error is set (a conforming stream ends before that);
 * the bins read then are garbage and the caller stops at its next check
 * of error.  Read-ahead past the end pads with zeros without setting
 * error for bits the model has not consumed.
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
 * Initializes the engine at data (H.264 9.3.1.2, H.265 9.3.2.6): range
 * 510, offset the first 9 bits.
 *
 * data is byte aligned by construction; data may be NULL only with size
 * 0.  Returns 0; EINVAL (and error EINVAL) when size is below
 * VCODEC_CABAC_START_BYTES or the first 9 bits are 510 or 511 (forbidden
 * by both specifications).  The context variables are not touched (they
 * are the caller's).
 */
int vcodec_cabac_start(struct vcodec_cabac *cabac, const uint8_t *data, size_t size);

/*
 * Decodes one bin with the context variable *context (DecodeDecision:
 * H.264 9.3.3.2.1, H.265 9.3.4.3.2), updating the context's state and
 * renormalizing (H.264 9.3.3.2.2, H.265 9.3.4.3.3).  Returns 0 or 1.
 */
unsigned vcodec_cabac_decision(struct vcodec_cabac *cabac, uint8_t *context);

/* Decodes one bin with equal probabilities (DecodeBypass: H.264 9.3.3.2.3, H.265 9.3.4.3.4); returns 0 or 1. */
unsigned vcodec_cabac_bypass(struct vcodec_cabac *cabac);

/*
 * Decodes count bypass bins (1..32) and returns them as an unsigned
 * number, the first bin most significant (the fixed-length and
 * Exp-Golomb suffixes of H.265 9.3.3).  The result is the same as count
 * calls of vcodec_cabac_bypass.
 */
uint32_t vcodec_cabac_bypass_bits(struct vcodec_cabac *cabac, unsigned count);

/*
 * Decodes a bin before termination (DecodeTerminate: H.264 9.3.3.2.4,
 * H.265 9.3.4.3.5): range minus 2; returns 1 without renormalizing when
 * offset >= range (the arithmetic decoding ends there), else 0 after
 * renormalizing.
 */
unsigned vcodec_cabac_terminate(struct vcodec_cabac *cabac);

/*
 * After vcodec_cabac_terminate returned 1, gives the offset into data of
 * the first byte boundary at or after the bit position the 9-bit model
 * has reached (just after the last bit read into offset; an engine that
 * reads ahead gives those bits back).  What follows there: the PCM
 * samples (H.264 I_PCM after pcm_alignment_zero_bit; H.265 pcm_sample()
 * after pcm_alignment_zero_bit), or the next substream (H.265
 * end_of_subset_one_bit then byte_alignment()), or nothing (the end of
 * the slice data).  The bits skipped to reach the boundary are not
 * checked.  Returns 0 and *byte_offset (at most size); EINVAL when the
 * last terminate did not return 1.
 */
int vcodec_cabac_position(const struct vcodec_cabac *cabac, size_t *byte_offset);

/*
 * Reads the samples of an H.264 I_PCM macroblock and restarts the engine
 * (H.264 7.3.5, 9.3.1.2): from the byte vcodec_cabac_position gives, count bytes into samples,
 * then vcodec_cabac_start on the bytes after them.  Returns 0, or EINVAL
 * when the data ends first.  H.265 PCM samples (PcmBitDepth bits each)
 * are read by the caller from vcodec_cabac_position with a bit reader,
 * then vcodec_cabac_start.
 */
int vcodec_cabac_pcm(struct vcodec_cabac *cabac, uint8_t *samples, size_t count);

/*
 * Returns the initial state of a context variable from m, n and the
 * slice's luma QP: preCtxState = Clip3( 1, 126, ( ( m * Clip3( 0, 51,
 * qp ) ) >> 4 ) + n ), valMps = preCtxState > 63, pStateIdx = valMps ?
 * preCtxState - 64 : 63 - preCtxState (H.264 9-5, H.265 9-6).  The
 * ">> 4" of a negative product is the floor (vcodec_asr32).
 */
uint8_t vcodec_cabac_context_init(int m, int n, int qp);

#endif
