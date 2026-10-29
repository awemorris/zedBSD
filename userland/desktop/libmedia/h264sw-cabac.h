/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CABAC arithmetic decoding engine of the software H.264 decoder: the
 * initialization of the context variables (9.3.1.1 with the values of m
 * and n of Tables 9-12 to 9-24) and of the engine (9.3.1.2), and the
 * three ways of decoding a bin (9.3.3.2: DecodeDecision 9.3.3.2.1 with
 * the renormalization of 9.3.3.2.2, DecodeBypass 9.3.3.2.3 and
 * DecodeTerminate 9.3.3.2.4, with rangeTabLPS of Table 9-44 and the
 * state transitions of Table 9-45).
 *
 * The syntax elements built from these bins (binarizations and context
 * index increments) are in h264sw-cabac-se.h.
 */

#ifndef LIBMEDIA_H264SW_CABAC_H
#define LIBMEDIA_H264SW_CABAC_H

#include "h264sw.h"
#include "h264sw-bits.h"

/*
 * The context variables kept: ctxIdx 0..459.  These cover every syntax
 * element of a frame of a 4:2:0 stream (Table 9-11 and Table 9-34 for
 * ChromaArrayType other than 3; the frame-coded and the field-coded
 * contexts of the significance maps both lie in this range, and only the
 * frame-coded ones are used).  ctxIdx 460..1023 serve the Cb and Cr of
 * 4:4:4 and the coded_block_flag of 8x8 blocks of 4:4:4, which this
 * decoder does not read.
 */
#define H264SW_CABAC_CONTEXTS 460U

/* ctxIdx 276, the end_of_slice_flag and the I_PCM bin: decoded with DecodeTerminate, never initialized from m and n. */
#define H264SW_CABAC_TERMINATE 276U

/*
 * One slice's arithmetic decoder.
 *
 * data and size are the bytes of the slice data from the byte where
 * CABAC starts (after cabac_alignment_one_bit) to the end of the RBSP;
 * position is the next byte to read.  range and offset are codIRange and
 * codIOffset (9 bits each in the clause's terms; an implementation may
 * keep offset scaled with more bits read ahead, as long as the decoded
 * bins are the same).  bits_left belongs to such an implementation (the
 * read-ahead bits not yet consumed).  state[ctxIdx] is pStateIdx (bits
 * 0..5) and valMPS (bit 6) of each context variable.
 *
 * When the 9-bit model of the clauses would read a bit beyond the end
 * of data, zero bits are supplied and error is set (a conforming slice
 * ends with end_of_slice_flag before that): the bins read then are
 * garbage, and the macroblock layer stops at its next check of error.
 * An engine that reads ahead of the model pads with zeros without
 * setting error for bits the model has not consumed.
 */
struct h264sw_cabac {
	const uint8_t *data;
	size_t size;
	size_t position;
	uint32_t range;
	uint32_t offset;
	int bits_left;
	int error;
	uint8_t state[H264SW_CABAC_CONTEXTS];
};

/*
 * Starts CABAC for a slice: the context variables and then the engine (9.3.1).
 *
 * bits is at the start of slice_data(); the cabac_alignment_one_bit
 * bits up to the byte boundary are read and checked to be 1 (EINVAL
 * otherwise).  slice_type is H264SW_SLICE_I, _P or _B; cabac_init_idc
 * 0..2 (ignored for I); qp is SliceQPY.  Each ctxIdx 0..459 except 276
 * that Table 9-11 lists for the slice type is initialized from (m, n) of
 * h264sw_cabac_init_mn (h264sw-cabac-init.h) by 9-5 (with Clip3( 0, 51,
 * SliceQPY )); the others are set to pStateIdx 0 and valMPS 0 and are
 * never read.  Then codIRange is 510 and codIOffset
 * the next 9 bits (9.3.1.2); codIOffset 510 or 511 returns EINVAL.
 *
 * The bytes from the boundary to the end of the RBSP are taken over;
 * bits is not used again for the slice (except by h264sw_cabac_pcm).
 */
int h264sw_cabac_start(struct h264sw_cabac *cabac, struct h264sw_bits *bits, unsigned slice_type, unsigned cabac_init_idc, int qp);

/*
 * Decodes one bin with context ctx_idx (DecodeDecision, 9.3.3.2.1).
 *
 * ctx_idx is below H264SW_CABAC_CONTEXTS and not 276 (a caller's
 * mistake otherwise; such a call returns 0 and sets error).  Updates the
 * context's state by Table 9-45 and renormalizes (9.3.3.2.2).  Returns
 * the bin, 0 or 1.
 */
unsigned h264sw_cabac_decision(struct h264sw_cabac *cabac, unsigned ctx_idx);

/* Decodes one bin with equal probabilities (DecodeBypass, 9.3.3.2.3); returns 0 or 1. */
unsigned h264sw_cabac_bypass(struct h264sw_cabac *cabac);

/*
 * Decodes the bin of end_of_slice_flag or of mb_type I_PCM (DecodeTerminate, 9.3.3.2.4).
 *
 * Returns 1 when the bin is 1: the arithmetic decoding ends there, and
 * the last bit read into codIOffset is the rbsp_stop_one_bit when the
 * bin was end_of_slice_flag (9.3.3.2.4), so nothing more is read; for
 * I_PCM, h264sw_cabac_pcm goes on from that position.  Returns 0
 * otherwise, after renormalizing (9.3.3.2.2).
 */
unsigned h264sw_cabac_terminate(struct h264sw_cabac *cabac);

/*
 * Reads the samples of an I_PCM macroblock and restarts the engine (7.3.5, 9.3.1.2).
 *
 * Called after h264sw_cabac_terminate returned 1 for mb_type I_PCM.
 * Finds the bit position the arithmetic decoding has reached: in the
 * 9-bit model of the clauses, just after the last bit read into
 * codIOffset (9.3.3.2.4; an engine that reads ahead gives those bits
 * back).  The pcm_alignment_zero_bit bits from there to the byte
 * boundary are skipped (not checked), then count bytes (384 for
 * 4:2:0, pcm_sample_luma then pcm_sample_chroma) are read into
 * samples, and the engine is initialized again (9.3.1.2) from the
 * following bytes; the context variables are kept.  Returns 0, or EINVAL when the data ends first.
 */
int h264sw_cabac_pcm(struct h264sw_cabac *cabac, uint8_t *samples, size_t count);

#endif
