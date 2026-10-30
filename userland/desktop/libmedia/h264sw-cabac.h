/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CABAC of one slice of the software H.264 decoder: its context
 * variables, their initialization (9.3.1.1 with the values of m and n of
 * Tables 9-12 to 9-24) and the decoding of a bin with a context named by
 * its ctxIdx.  The arithmetic decoding engine itself (9.3.1.2, 9.3.3.2)
 * is the codec-independent one of vcodec-cabac.h: the bypass and
 * terminate bins and the I_PCM samples are decoded with its functions on
 * the engine member.
 *
 * The syntax elements built from these bins (binarizations and context
 * index increments) are in h264sw-cabac-se.h.
 */

#ifndef LIBMEDIA_H264SW_CABAC_H
#define LIBMEDIA_H264SW_CABAC_H

#include "h264sw.h"
#include "vcodec-bitreader.h"
#include "vcodec-cabac.h"

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
 * One slice's CABAC: the arithmetic decoding engine and the context
 * variables.
 *
 * state[ctxIdx] is pStateIdx (bits 0..5) and valMPS (bit 6) of each
 * context variable, the byte vcodec_cabac_decision takes.  The engine's
 * error is the slice's: the macroblock layer checks engine.error.
 */
struct h264sw_cabac {
	struct vcodec_cabac engine;
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
 * h264sw_cabac_init_mn (h264sw-cabac-init.h) by 9-5
 * (vcodec_cabac_context_init); the others are set to pStateIdx 0 and
 * valMPS 0 and are never read.  Then the engine starts on the bytes from
 * the boundary to the end of the RBSP (vcodec_cabac_start): codIRange is
 * 510 and codIOffset the next 9 bits (9.3.1.2); codIOffset 510 or 511
 * returns EINVAL.
 *
 * Before any check the whole of cabac is cleared and engine.error set to
 * EINVAL, so a failed start leaves an engine that reads nothing.  The
 * bytes are taken over; bits is not used again for the slice.
 */
int h264sw_cabac_start(struct h264sw_cabac *cabac, struct vcodec_bitreader *bits, unsigned slice_type, unsigned cabac_init_idc, int qp);

/*
 * Decodes one bin with context ctx_idx (DecodeDecision, 9.3.3.2.1).
 *
 * ctx_idx is below H264SW_CABAC_CONTEXTS and not 276 (a caller's
 * mistake otherwise; such a call returns 0 and sets engine.error to
 * EINVAL).  Decodes with vcodec_cabac_decision on state[ctx_idx], which
 * updates the context's state by Table 9-45 and renormalizes
 * (9.3.3.2.2).  Returns the bin, 0 or 1.
 */
unsigned h264sw_cabac_decision(struct h264sw_cabac *cabac, unsigned ctx_idx);

#endif
