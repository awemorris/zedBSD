/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * residual_coding() of the software H.265 decoder (7.3.8.11, 7.4.9.11):
 * transform_skip_flag, the last significant position, the sub-blocks of
 * 4x4 coefficients with coded_sub_block_flag and sig_coeff_flag, the
 * greater1 and greater2 flags, the signs with sign data hiding, and
 * coeff_abs_level_remaining with its Rice parameter (9.3.3.11), with the
 * ctxInc derivations of 9.3.4.2.3 to 9.3.4.2.7.  The result is the array
 * TransCoeffLevel of one transform block.
 *
 * The range extension's syntax (explicit_rdpcm_flag, the persistent Rice
 * adaptation, cabac_bypass_alignment, the extended precision) is never
 * present in the decoder's range and is not read.
 */

#ifndef LIBMEDIA_H265SW_RESIDUAL_H
#define LIBMEDIA_H265SW_RESIDUAL_H

#include "h265sw.h"
#include "h265sw-cabac-se.h"

/*
 * What residual_coding() of one transform block needs besides the
 * engine: log2_size log2TrafoSize (2..5); c_idx; scan_idx scanIdx
 * (H265SW_SCAN_*, derived by the caller as 7.4.9.11 says from the intra
 * prediction mode for 4x4 and 8x8 luma and 4x4 chroma blocks of intra
 * coding units, else diagonal); transform_skip_allowed
 * transform_skip_enabled_flag && !cu_transquant_bypass_flag &&
 * log2_size == 2 (Log2MaxTransformSkipSize is 2 in the range);
 * sign_hiding sign_data_hiding_enabled_flag && !cu_transquant_bypass_flag.
 */
struct h265sw_residual_params {
	unsigned log2_size;
	unsigned c_idx;
	unsigned scan_idx;
	int transform_skip_allowed;
	int sign_hiding;
};

/*
 * Decodes residual_coding() of one transform block.
 *
 * levels receives TransCoeffLevel[ x ][ y ] as levels[ y * ( 1 <<
 * log2_size ) + x] for the whole block (positions not coded are 0).
 * *transform_skip receives transform_skip_flag (0 when not present).
 *
 * Steps (7.3.8.11): last_sig_coeff_x_prefix and _y_prefix (TR with cMax (
 * log2_size << 1 ) - 1, ctxInc by 9.3.4.2.3 with ctxOffset and ctxShift
 * of luma or chroma), the suffixes (FL bypass), LastSignificantCoeffX
 * and Y (7-74 to 7-77, swapped by 7-78 for scan_idx 2); the scan back from the
 * last position; per sub-block coded_sub_block_flag (9.3.4.2.4),
 * sig_coeff_flag (9.3.4.2.5 with ctxIdxMap of Table 9-50 for 4x4 blocks,
 * sigCtx from the right and lower sub-blocks' flags otherwise, and the
 * chroma offset 27), coeff_abs_level_greater1_flag (9.3.4.2.6, ctxSet
 * and greater1Ctx from the last invocation for a previous sub-block of
 * the same block, 9.3.4.2.6),
 * coeff_abs_level_greater2_flag (9.3.4.2.7), coeff_sign_flag (bypass,
 * the hidden sign of firstSigScanPos when signHidden), and
 * coeff_abs_level_remaining (9.3.3.11 with persistent rice adaptation
 * off: cRiceParam from 0 in each sub-block, 9-24, cMax 4 <<
 * cRiceParam, prefix TR, suffix EG( cRiceParam + 1 ) bypass).
 *
 * Untrusted input: LastSignificantCoeffX and Y not below 1 << log2_size,
 * a coeff_abs_level_remaining whose prefix has more than 4 + 16 ones
 * (with cRiceParam at most 4 that already exceeds 2^15 + 3, beyond any
 * level the constraint allows; the suffix is read with at most 32 bits
 * at a time and summed in uint32_t), or a TransCoeffLevel outside
 * -32768..32767 (CoeffMinY and CoeffMaxY, 7.4.9.11: the stream breaks the
 * constraint) set the engine's error and end the block (levels then
 * hold what was decoded, all within int16_t).  Every loop is bounded by
 * the sub-block and position counts.
 *
 * Returns the number of non-zero levels (0..1024; the caller records
 * "coded" for deblocking from it), or 0 with the engine's error set.
 */
unsigned h265sw_residual_decode(struct h265sw_cabac *cabac, const struct h265sw_residual_params *params, int16_t *levels, int *transform_skip);

#endif
