/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scaling lists of the software H.265 decoder: scaling_list_data()
 * (7.3.4, 7.4.5), the default lists (Tables 7-5 and 7-6) and the
 * quantization matrices ScalingFactor derived from the lists (7-44 to
 * 7-49) that the scaling process uses as m[ x ][ y ] (8-308).
 *
 * Only the matrices of 4:2:0 are kept: sizeId 0..2 with matrixId 0..5,
 * sizeId 3 with matrixId 0 and 3 (the luma 32x32 matrices; 7-50 and
 * 7-51, the chroma 32x32 of 4:4:4, are not used).
 */

#ifndef LIBMEDIA_H265SW_SCALING_H
#define LIBMEDIA_H265SW_SCALING_H

#include "h265sw.h"
#include "vcodec-bitreader.h"

/*
 * The scaling lists of one SPS or PPS as signalled or inferred.
 *
 * list[sizeId][matrixId][i] is ScalingList (i < 16 for sizeId 0, < 64
 * otherwise; for sizeId 3 only matrixId 0 and 3 are meaningful) and
 * dc[sizeId - 2][matrixId] the DC value of the 16x16 and 32x32 matrices
 * (scaling_list_dc_coef_minus8 + 8, or 16 for a default list).
 */
struct h265sw_scaling_list {
	uint8_t list[4][6][64];
	uint8_t dc[2][6];
};

/*
 * The quantization matrices of one picture, ScalingFactor of 7.4.5 laid
 * out as [ y ][ x ] rows: m4[matrixId][y * 4 + x], m8 likewise with 8,
 * m16 with 16, m32[0] for matrixId 0 and m32[1] for matrixId 3 with 32.
 * About 4 KiB; derived once per picture from the active list.
 */
struct h265sw_scaling_factors {
	uint8_t m4[6][16];
	uint8_t m8[6][64];
	uint8_t m16[6][256];
	uint8_t m32[2][1024];
};

/*
 * Sets list to the default lists (Table 7-5 for sizeId 0, Table 7-6 for
 * sizeId 1..3; DC 16): what a sequence with scaling_list_enabled_flag 1
 * and no list data uses (7.4.3.2.1).
 */
void h265sw_scaling_default(struct h265sw_scaling_list *list);

/*
 * Reads scaling_list_data() (7.3.4) into list.
 *
 * Applies the semantics of 7.4.5 as it reads: a predicted list
 * (scaling_list_pred_mode_flag 0) copies the default list when the delta
 * is 0, else the list and DC of refMatrixId (7-42, 7-43); a signalled
 * list is built with nextCoef modulo 256.  Checks
 * scaling_list_pred_matrix_id_delta (0..matrixId, or 0..matrixId / 3 for
 * sizeId 3), scaling_list_dc_coef_minus8 (-7..247) and
 * scaling_list_delta_coef (-128..127).
 *
 * Returns 0; EINVAL for a value out of range or a read error.  On error
 * list is left partly written (the caller discards it).
 */
int h265sw_scaling_read(struct h265sw_scaling_list *list, struct vcodec_bitreader *bits);

/*
 * Derives the quantization matrices of a list (7-44 to 7-49): sizeId 0
 * and 1 place the list along the up-right diagonal scan of a 4x4 and an
 * 8x8 block (6.5.3, h265sw_scan_order with H265SW_SCAN_DIAG); sizeId 2 and 3 repeat each
 * entry of the 8x8 placement over 2x2 and 4x4 samples and put the DC at
 * [0][0].
 */
void h265sw_scaling_derive(const struct h265sw_scaling_list *list, struct h265sw_scaling_factors *factors);

/*
 * Returns the matrix m of a transform block, or NULL when m is flat 16:
 * matrixId from Table 7-4 (cIdx, plus 3 for an inter coding unit) and
 * sizeId from Table 7-3 (log2_size 2..5).  factors NULL means
 * scaling_list_enabled_flag 0.  The caller handles the other flat case
 * of 8.6.3 (transform skip with nTbS above 4).  The result points into
 * factors, rows of 1 << log2_size entries.
 */
const uint8_t *h265sw_scaling_matrix(const struct h265sw_scaling_factors *factors, unsigned log2_size, unsigned c_idx, int inter);

#endif
