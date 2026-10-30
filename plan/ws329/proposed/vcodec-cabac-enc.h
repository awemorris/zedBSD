/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The arithmetic encoding engine of CABAC for libmedia's software video
 * encoders, written from the informative encoder of ITU-T H.264 (08/2021)
 * 9.3.4: InitEncoder (9.3.4.1), EncodeDecision (9.3.4.2, Figure 9-7),
 * RenormE and PutBit (9.3.4.3, Figures 9-8 and 9-9), EncodeBypass
 * (9.3.4.4, Figure 9-10), EncodeTerminate and EncodeFlush (9.3.4.5,
 * Figures 9-11 and 9-12).  H.265 9.3.4.3 uses the same engine, tables
 * and state transitions, so the engine knows nothing of a codec's
 * contexts: the caller passes the byte of the context variable it codes
 * with.
 *
 * A context variable is one byte: pStateIdx in bits 0..5 and valMPS in
 * bit 6, as the decoders keep them.  rangeTabLPS (H.264 Table 9-44) and
 * the transitions (Table 9-45) are the decoder's tables
 * (h264sw_cabac_range_lps, h264sw_cabac_trans_lps,
 * h264sw_cabac_trans_mps), not a copy.
 *
 * Bits go to a struct vcodec_bitwriter; its errors are the engine's.
 */

#ifndef LIBMEDIA_VCODEC_CABAC_ENC_H
#define LIBMEDIA_VCODEC_CABAC_ENC_H

#include <stddef.h>
#include <stdint.h>

#include "vcodec-bitwriter.h"

/*
 * The scale of the bit costs: vcodec_cabac_cost gives costs in 1/256 bit.
 */
#define VCODEC_CABAC_COST_SHIFT 8U

/*
 * One slice's arithmetic encoder.
 *
 * out is the writer the bits go to (borrowed).  low and range are
 * codILow (10 bits, more while outstanding bits wait) and codIRange (9
 * bits) of 9.3.4.1; first_bit is firstBitFlag and outstanding
 * bitsOutstanding of PutBit.  bins is BinCountsInNALunits as 9.3.4.1
 * keeps it: the caller sets it to 0 for the first slice of a picture and
 * carries it from slice to slice of the picture (the engine only adds
 * to it).
 */
struct vcodec_cabac_enc {
	struct vcodec_bitwriter *out;
	uint32_t low;
	uint32_t range;
	int first_bit;
	uint32_t outstanding;
	uint64_t bins;
};

/*
 * Starts the engine (InitEncoder, 9.3.4.1): low 0, range 510, first_bit 1,
 * outstanding 0; bins is left as it is.  out must be byte aligned (after
 * cabac_alignment_one_bit, or after the samples of an I_PCM macroblock);
 * an unaligned writer gets error EINVAL.
 */
void vcodec_cabac_enc_start(struct vcodec_cabac_enc *engine, struct vcodec_bitwriter *out);

/*
 * Encodes one bin with a context variable (EncodeDecision, 9.3.4.2).
 *
 * context points to the context variable's byte; bin is 0 or 1 (any
 * non-zero value is 1).  qCodIRangeIdx = ( range >> 6 ) & 3 (9-25),
 * codIRangeLPS from rangeTabLPS; an LPS adds range - codIRangeLPS to
 * low and makes range codIRangeLPS; the state moves by Table 9-45 (and
 * valMPS flips on an LPS in state 0); RenormE; bins + 1.
 */
void vcodec_cabac_enc_decision(struct vcodec_cabac_enc *engine, uint8_t *context, unsigned bin);

/* Encodes one bin with equal probabilities (EncodeBypass, 9.3.4.4); bins + 1. */
void vcodec_cabac_enc_bypass(struct vcodec_cabac_enc *engine, unsigned bin);

/*
 * Encodes the bin of ctxIdx 276 (end_of_slice_flag, the I_PCM bin of
 * mb_type) (EncodeTerminate, 9.3.4.5); bins + 1.
 *
 * range -= 2; a bin of 1 adds range to low and then flushes (EncodeFlush:
 * range 2, RenormE, PutBit( ( low >> 9 ) & 1 ), the two bits ( ( low >>
 * 7 ) & 3 ) | 1): the last bit written is 1, which is the
 * rbsp_stop_one_bit when the bin was end_of_slice_flag, so the caller
 * then writes only alignment zero bits.  A bin of 0 renormalizes.
 * After a flush the engine is stopped until vcodec_cabac_enc_start.
 */
void vcodec_cabac_enc_terminate(struct vcodec_cabac_enc *engine, unsigned bin);

/*
 * Returns the bits the engine has produced so far, including the
 * outstanding bits not yet written and the 10 bits low still holds (an
 * upper bound of what a decoder reads for the bins encoded), for the
 * per-macroblock bit limit of the codecs.
 */
uint64_t vcodec_cabac_enc_bits(const struct vcodec_cabac_enc *engine);

/*
 * The estimated cost of coding a bin with a context in state
 * pStateIdx s: [s][0] for the most probable symbol, [s][1] for the least,
 * in 1/256 bit.  Not the specification's data: the value for state s is
 * Round( -256 * Log2( 1 - p ) ) and Round( -256 * Log2( p ) ) with
 * p = 0.5 * alpha^s, alpha = ( 0.01875 / 0.5 )^( 1 / 63 ) (the
 * probability model the 64 states were designed from, as D. Marpe, H.
 * Schwarz and T. Wiegand describe it in "Context-based adaptive binary
 * arithmetic coding in the H.264/AVC video compression standard", IEEE
 * Trans. Circuits Syst. Video Technol. 13(7), 2003).  The table is
 * written as data computed from that formula; used only for decisions,
 * never for the bitstream.
 */
extern const uint16_t vcodec_cabac_cost_table[64][2];

/* Returns the estimated cost in 1/256 bit of coding bin with the context byte (not changing it). */
uint32_t vcodec_cabac_cost(uint8_t context, unsigned bin);

#endif
