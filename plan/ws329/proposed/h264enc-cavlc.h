/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CAVLC writing of the software H.264 encoder: residual_block_cavlc
 * (7.3.5.3.2) by the inverse of 9.2 (coeff_token from Table 9-5,
 * trailing ones' signs, levels with level_prefix and level_suffix and the
 * suffixLength adaptation of 9.2.2, total_zeros from Tables 9-7 to 9-9,
 * run_before from Table 9-10), its bit count without writing (for
 * decisions), and the codeNum of coded_block_pattern (inverse of Table
 * 9-4).
 *
 * The codes come from the decoder's tables (h264sw-cavlc-tables.h);
 * h264enc_cavlc_init indexes them by value once so that writing does not
 * search.  The nC of a block is the decoder's (h264sw_cavlc_nc).
 */

#ifndef LIBMEDIA_H264ENC_CAVLC_H
#define LIBMEDIA_H264ENC_CAVLC_H

#include "h264sw.h"
#include "vcodec-bitwriter.h"

/* One code: its bits right aligned and its length (0 where the table has no such value). */
struct h264enc_code {
	uint16_t code;
	uint8_t length;
};

/*
 * The CAVLC codes indexed by value, built from the shared tables.
 *
 * coeff_token[column][TotalCoeff][TrailingOnes] for the columns 0..3 of
 * h264sw_cavlc_coeff_token and column 4 the nC = -1 one (TotalCoeff 0..4
 * there); total_zeros[tzVlcIndex - 1][total_zeros];
 * total_zeros_chroma_dc[tzVlcIndex - 1][total_zeros];
 * run_before[Min( zerosLeft, 7 ) - 1][run_before]; cbp_code[column][cbp]
 * the codeNum of each coded_block_pattern value 0..47, with the columns
 * of h264sw_cavlc_cbp: [0] Intra_4x4 and Intra_8x8, [1] Inter.  Owned by the
 * encoder (embedded); constant after init.
 */
struct h264enc_cavlc {
	struct h264enc_code coeff_token[5][17][4];
	struct h264enc_code total_zeros[15][16];
	struct h264enc_code total_zeros_chroma_dc[3][4];
	struct h264enc_code run_before[7][15];
	uint8_t cbp_code[2][48];
};

/*
 * Builds the index from h264sw_cavlc_coeff_token, _coeff_token_chroma_dc,
 * _total_zeros, _total_zeros_chroma_dc, _run_before and _cbp.  Cannot
 * fail; every entry the tables define is set, the others have length 0.
 */
void h264enc_cavlc_init(struct h264enc_cavlc *cavlc);

/*
 * Writes one residual block (residual_block_cavlc, 7.3.5.3.2).
 *
 * nc is nC (0 and above, or -1 for the chroma DC of 4:2:0); levels holds
 * coeffLevel[ 0..max_coeff - 1 ] in scan order; start and end are
 * startIdx and endIdx and max_coeff maxNumCoeff, as for
 * h264sw_cavlc_residual_block (levels outside start..end must be 0).
 * max_prefix is the largest level_prefix allowed: 15 for the profiles
 * other than High (A.2.1 to A.2.3), 25 otherwise.  *total_coeff receives
 * TotalCoeff( coeff_token ) (the caller records it for later nC).
 *
 * The coding follows 9.2 in reverse: the coefficients are taken in
 * reverse scan order from end down to start; TrailingOnes counts the
 * leading +-1 levels (at most 3); suffixLength starts at 1 when
 * TotalCoeff > 10 and TrailingOnes < 3, else 0; each level gives
 * levelCode = 2 * |level| - 2 for a positive level and 2 * |level| - 1
 * for a negative one, less 2 for the first level after the trailing ones
 * when TrailingOnes < 3 (the inverse of the increment by 2 of 9.2.2,
 * which also makes |level| 1 impossible there); level_prefix and level_suffix are the
 * shortest that 9.2.2.1 decodes to levelCode for the current
 * suffixLength (the escapes of level_prefix 14 with suffixLength 0 and
 * of 15 and above); suffixLength then grows as 9.2.2 says; total_zeros
 * when TotalCoeff < end - start + 1; run_before for each coefficient
 * but the last while zerosLeft > 0.
 *
 * Returns 0; ERANGE when a level needs a level_prefix above max_prefix
 * (nothing is written: the caller clamps the levels and tries again);
 * EINVAL for bad arguments (a level beyond -2^15..2^15 - 1, a non-zero
 * level outside start..end, nc below -1); or the writer's error.
 */
int h264enc_cavlc_block(struct vcodec_bitwriter *writer, const struct h264enc_cavlc *cavlc, int nc, const int32_t *levels, unsigned start, unsigned end, unsigned max_coeff, unsigned max_prefix, unsigned *total_coeff);

/*
 * Returns the bits h264enc_cavlc_block would write for the same
 * arguments (0 when it would fail), without writing; *total_coeff as
 * there (may be NULL).
 */
unsigned h264enc_cavlc_block_bits(const struct h264enc_cavlc *cavlc, int nc, const int32_t *levels, unsigned start, unsigned end, unsigned max_coeff, unsigned max_prefix, unsigned *total_coeff);

/*
 * Gives the codeNum of coded_block_pattern me(v) (inverse of Table 9-4 (a)):
 * cbp holds CodedBlockPatternLuma in bits 0..3 and
 * CodedBlockPatternChroma in bits 4..5 (0..47); intra non-zero for the
 * Intra_4x4 and Intra_8x8 prediction modes (column 0), 0 for Inter
 * (column 1).  Returns 0 and *code_num, or EINVAL for
 * a cbp above 47 or a chroma part of 3.
 */
int h264enc_cavlc_cbp(const struct h264enc_cavlc *cavlc, unsigned cbp, int intra, uint32_t *code_num);

#endif
