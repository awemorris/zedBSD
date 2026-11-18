/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The limits and conventions shared by every part of libmedia's H.265
 * code: the parse of the bitstream (NAL units, parameter sets, slice
 * segment headers, picture order counts and reference picture sets),
 * which the Vulkan Video path and the software decoder both use, and the
 * software decoder built on it.
 *
 * The parse reads the base layer (nuh_layer_id 0).  It keeps every format
 * a valid parameter set may name, so one parse serves back ends with
 * different ranges; what each back end decodes is decided by that back
 * end (for the software decoder, h265sw_ps_sps_supported).  It refuses
 * with ENOTSUP only the syntax it does not read: the screen content, 3D
 * and multilayer extensions, the range extension tools of a PPS, and the
 * sizes above the limits below.  NAL units of other layers are skipped by
 * the caller.
 *
 * Every function that reads the bitstream treats it as untrusted: it never
 * reads outside its buffers, never indexes an array with a value it has
 * not checked against the array, and never loops without a bound that the
 * buffer sizes or these limits give.  A failure is returned, never asserted.
 *
 * The error values and the arithmetic rules are those of vcodec.h.  A
 * formula's "x >> n" on a value that can be negative is computed as a
 * floor division by explicit arithmetic, and "x & ( m - 1 )" on a value
 * that can be negative (m a power of two) as x - Floor( x / m ) * m in
 * int64_t, never as a bitwise operation on a negative value.
 *
 * No part keeps global state other than constant tables: everything lives
 * in the structures a caller owns, so two streams run on two threads
 * without locks.
 */

#ifndef LIBMEDIA_H265SW_H
#define LIBMEDIA_H265SW_H

#include "vcodec.h"

/*
 * The clause, table and equation numbers in the comments of these files
 * ("7.3.2.2.1", "Table 7-1", "8-5") are those of ITU-T H.265 (V11)
 * (01/2026), whose text is common with ISO/IEC 23008-2.
 *
 * The largest picture taken is that of level 6.2 (Table A.8: MaxLumaPs
 * 35 651 584 samples; A.4.1 b) and c): each side at most
 * Sqrt( MaxLumaPs * 8 ), 16 888 samples).  Higher levels are taken while
 * the picture stays within these.
 */
#define H265SW_MAX_LUMA_PS 35651584U
#define H265SW_MAX_SIDE 16888U

/* BitDepthY and BitDepthC (7-4, 7-6) of the software decoder: the only bit depth it decodes. */
#define H265SW_BIT_DEPTH 8

/* The coding tree block sizes of every profile of Annex A (CtbLog2SizeY 4..6) and the transform sizes (7.4.3.2.1: up to 32). */
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

/*
 * The most entries of one short-term reference picture set
 * (NumNegativePics + NumPositivePics at most
 * sps_max_dec_pic_buffering_minus1, at most 15, 7.4.8; kept with a margin
 * of one), and of the long-term entries of one slice header.
 */
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

/* The most slice segments of one picture taken (Table A.8, level 6.2: MaxSliceSegmentsPerPicture 600). */
#define H265SW_MAX_SLICE_SEGMENTS 600U

/* The most sub-layers (sps_max_sub_layers_minus1 and vps_max_sub_layers_minus1 0..6). */
#define H265SW_MAX_SUB_LAYERS 7U

/* The most CPB specifications of one sub-layer's HRD parameters (cpb_cnt_minus1 0..31, E.3.2). */
#define H265SW_MAX_CPB 32U

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
