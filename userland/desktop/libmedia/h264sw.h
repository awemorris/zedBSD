/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The limits and conventions shared by every part of libmedia's software
 * H.264 decoder.
 *
 * The decoder reads progressive frames of 8-bit 4:2:0 streams of the
 * Constrained Baseline, Baseline (without slice groups and redundant
 * pictures), Main and High profiles.  Field pictures, MBAFF frames, the
 * 4:2:2 and 4:4:4 formats, bit depths above 8, slice groups, data
 * partitioning and the SP and SI slices are refused.
 *
 * The specification every part follows is ITU-T H.264 (08/2021),
 * "Advanced video coding for generic audiovisual services", whose text is
 * common with ISO/IEC 14496-10.  A clause such as "8.4.2.2.1" or a table
 * such as "Table 9-5" named in a comment of these files is that edition's.
 *
 * Every function that reads the bitstream treats it as untrusted: it never
 * reads outside its buffers, never indexes an array with a value it has
 * not checked against the array, and never loops without a bound that the
 * buffer sizes or these limits give.  A failure is returned, never asserted.
 *
 * The error values, used alike by every part:
 *
 *   0        Succeeded.
 *   EINVAL   The bitstream breaks the syntax or the semantics of the
 *            specification (a value out of its range, a read past the end
 *            of a NAL unit, a reference to an absent parameter set).
 *   ENOTSUP  The bitstream is valid but uses a tool or a format outside
 *            the range above.
 *   ENOMEM   An allocation failed.
 *   ENOBUFS  A buffer or queue the caller sized is too small (a caller's
 *            mistake, or an output queue the caller has not emptied).
 *   EBUSY    The decoder holds output the caller must take first.
 *
 * No part keeps global state: everything lives in the structures a
 * decoder owns, so two decoders run on two threads without locks.
 */

#ifndef LIBMEDIA_H264SW_H
#define LIBMEDIA_H264SW_H

#include <stddef.h>
#include <stdint.h>

/*
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

/*
 * Marks a parameter a function does not use, so the compiler does not
 * warn about it.
 */
#ifndef UNUSED_PARAMETER
#define UNUSED_PARAMETER(name) ((void)(name))
#endif

/* The samples of one macroblock: 16x16 luma and two 8x8 chroma blocks (4:2:0). */
#define H264SW_MB_LUMA 256U
#define H264SW_MB_CHROMA 64U

#endif
