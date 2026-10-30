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

#include "vcodec.h"

/* The fewest bytes the engine starts from: they hold the 9 bits of codIOffset (9.3.1.2). */
#define VCODEC_CABAC_START_BYTES 2U

/*
 * rangeTabLPS: codIRangeLPS for each pStateIdx (rows) and qCodIRangeIdx
 * (columns), and the pStateIdx after a least and a most probable symbol
 * (transIdxLPS, transIdxMPS).  Also used by the encoding engines.
 */
extern const uint8_t vcodec_cabac_range_lps[64][4];
extern const uint8_t vcodec_cabac_trans_lps[64];
extern const uint8_t vcodec_cabac_trans_mps[64];

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

#endif
