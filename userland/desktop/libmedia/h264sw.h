/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The range and the limits of libmedia's software H.264 decoder, shared
 * by every part of it.
 *
 * The decoder reads progressive frames of 8-bit 4:2:0 streams: the
 * Constrained Baseline, Baseline, Main and High profiles, and streams of
 * other profiles whose sequence stays within what those use.  Field
 * pictures, MBAFF frames, the 4:0:0, 4:2:2 and 4:4:4 formats, bit depths
 * above 8, lossless coding, slice groups, data partitioning and the SP
 * and SI slices are refused.  Redundant pictures are skipped.
 *
 * The conventions of every part (untrusted input, arithmetic without
 * undefined behaviour, the error values, no global state) are those of
 * vcodec.h.
 */

#ifndef LIBMEDIA_H264SW_H
#define LIBMEDIA_H264SW_H

#include <stddef.h>
#include <stdint.h>

#include "vcodec.h"

/*
 * The clause, table and equation numbers in the comments of these files
 * ("8.4.2.2.1", "Table 9-5", "8-223") are those of ITU-T H.264 (08/2021),
 * whose text is common with ISO/IEC 14496-10.
 *
 * The largest picture taken, from the highest level of Table A-1 (level
 * 6.2: MaxFS 139264 macroblocks) and A.3.1 f) and g) (each side at most
 * Sqrt( MaxFS * 8 ) macroblocks).
 */
#define H264SW_MAX_FRAME_MBS 139264U
#define H264SW_MAX_SIDE_MBS 1055U

/* The most reference frames a sequence may keep (max_num_ref_frames, 7.4.2.1.1). */
#define H264SW_MAX_REF_FRAMES 16U

/* The most frames the decoded picture buffer holds (A.3.1 h): Min( MaxDpbMbs / PicSizeInMbs, 16 )). */
#define H264SW_MAX_DPB_FRAMES 16U

/* The longest reference picture list of a frame slice (num_ref_idx_l0_active_minus1 at most 31, 7.4.3). */
#define H264SW_MAX_LIST 32U

/* The parameter set id ranges (7.4.2.1.1 seq_parameter_set_id, 7.4.2.2 pic_parameter_set_id). */
#define H264SW_SPS_IDS 32U
#define H264SW_PPS_IDS 256U

/* The samples of one macroblock: 16x16 luma and two 8x8 chroma blocks (4:2:0). */
#define H264SW_MB_LUMA 256U
#define H264SW_MB_CHROMA 64U

#endif
