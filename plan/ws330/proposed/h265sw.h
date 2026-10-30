/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The limits and conventions shared by every part of libmedia's software
 * H.265 decoder.
 *
 * The decoder reads the base layer (nuh_layer_id 0) of streams whose
 * sequence stays within the tools of the Main profile: 8-bit 4:2:0
 * progressive pictures with every tool of A.3.2 (CTBs of 16 to 64, AMP,
 * SAO, PCM, transform skip of 4x4 blocks, sign data hiding, scaling lists,
 * weighted prediction, long-term pictures, temporal motion vector
 * prediction, dependent slice segments, tiles and wavefront parallel
 * processing, decoded one after another on one thread).  A stream that
 * names another profile is taken when its sequence uses nothing more (a
 * Main 10 or Main 4:4:4 stream of 8-bit 4:2:0 pictures with every range
 * extension tool off).  Refused with ENOTSUP: bit depths above 8, the
 * 4:0:0, 4:2:2 and 4:4:4 formats, field-coded sequences
 * (field_seq_flag 1), and the range, screen content and 3D extension
 * tools.  NAL units of other layers are skipped.
 *
 * Every function that reads the bitstream treats it as untrusted: it never
 * reads outside its buffers, never indexes an array with a value it has
 * not checked against the array, and never loops without a bound that the
 * buffer sizes or these limits give.  A failure is returned, never asserted.
 *
 * The error values and the arithmetic rules are those of vcodec.h.  The
 * formulas of the specification are written with BitDepthY and BitDepthC
 * kept as the constant H265SW_BIT_DEPTH, not folded into the numbers, so
 * the shifts read as the clauses write them.
 *
 * No part keeps global state other than constant tables: everything lives
 * in the structures a decoder owns, so two decoders run on two threads
 * without locks.
 */

#ifndef LIBMEDIA_H265SW_H
#define LIBMEDIA_H265SW_H

#include "vcodec.h"

/*
 * The clause, table and equation numbers in the comments of these files
 * ("8.5.3.2.2", "Table 9-4", "8-224") are those of ITU-T H.265 (V11)
 * (01/2026), whose text is common with ISO/IEC 23008-2.
 *
 * The largest picture taken is that of level 6.2 (Table A.8: MaxLumaPs
 * 35 651 584 samples; A.4.1 b) and c): each side at most
 * Sqrt( MaxLumaPs * 8 ), 16 888 samples).  Higher levels are taken while
 * the picture stays within these.
 */
#define H265SW_MAX_LUMA_PS 35651584U
#define H265SW_MAX_SIDE 16888U

/* BitDepthY and BitDepthC (7-4, 7-6): the only bit depth taken. */
#define H265SW_BIT_DEPTH 8

/* The coding tree block sizes of the Main profile (A.3.2: CtbLog2SizeY 4..6) and the transform sizes (7.4.3.2.1: up to 32). */
#define H265SW_MIN_CTB_LOG2 4U
#define H265SW_MAX_CTB_LOG2 6U
#define H265SW_MAX_CTB 64U
#define H265SW_MIN_CB_LOG2 3U
#define H265SW_MAX_TB_LOG2 5U

/*
 * The most pictures the decoded picture buffer holds (A.4.2: MaxDpbSize
 * at most 16; sps_max_dec_pic_buffering_minus1 + 1 at most that).
 */
#define H265SW_MAX_DPB 16U

/* The longest reference picture list (num_ref_idx_l0_active_minus1 0..14, 7.4.7.1). */
#define H265SW_MAX_REFS 15U

/* The most entries of one short-term reference picture set (NumDeltaPocs at most sps_max_dec_pic_buffering_minus1, at most 15; kept with a margin of one). */
#define H265SW_MAX_RPS 16U

/* num_short_term_ref_pic_sets 0..64 (7.4.3.2.1); the set a slice header carries is index 64. */
#define H265SW_MAX_ST_RPS 65U

/* num_long_term_ref_pics_sps 0..32 (7.4.3.2.1). */
#define H265SW_MAX_LT_SPS 32U

/* The parameter set id ranges (7.4.3.1, 7.4.3.2.1, 7.4.3.3.1). */
#define H265SW_VPS_IDS 16U
#define H265SW_SPS_IDS 16U
#define H265SW_PPS_IDS 64U

/* The most tile columns and rows taken (Table A.8, level 6.2: MaxTileCols 20, MaxTileRows 22). */
#define H265SW_MAX_TILE_COLS 20U
#define H265SW_MAX_TILE_ROWS 22U

/* The most slice segments of one picture taken (Table A.8, level 6.2: 600). */
#define H265SW_MAX_SLICE_SEGMENTS 600U

/* The most sub-layers (sps_max_sub_layers_minus1 0..6). */
#define H265SW_MAX_SUB_LAYERS 7U

/* The largest NAL unit taken, a bound on the RBSP buffer (a larger one is refused with EINVAL). */
#define H265SW_MAX_NAL_BYTES (64U * 1024U * 1024U)

/* The prediction modes of a coding unit (CuPredMode, 7.4.9.5); SKIP is MODE_SKIP. */
#define H265SW_MODE_INTER 0U
#define H265SW_MODE_INTRA 1U
#define H265SW_MODE_SKIP 2U

/* The slice types (Table 7-7). */
#define H265SW_SLICE_B 0U
#define H265SW_SLICE_P 1U
#define H265SW_SLICE_I 2U

#endif
