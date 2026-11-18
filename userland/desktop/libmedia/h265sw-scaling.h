/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scaling lists of H.265: scaling_list_data() (7.3.4, 7.4.5) and the
 * default lists (Tables 7-5 and 7-6), kept as ScalingList[ sizeId ][
 * matrixId ][ i ] in the order the syntax codes them (the up-right
 * diagonal order of 6.5.3), which is also the order of the Vulkan
 * StdVideoH265ScalingLists arrays.
 *
 * The quantization matrices derived from the lists (ScalingFactor, 7-44
 * to 7-51) are the software decoder's part and are not declared here.
 */

#ifndef LIBMEDIA_H265SW_SCALING_H
#define LIBMEDIA_H265SW_SCALING_H

#include "h265sw.h"
#include "vcodec-bitreader.h"

/*
 * The scaling lists of one SPS or PPS as signalled or inferred.
 *
 * list[sizeId][matrixId][i] is ScalingList[ sizeId ][ matrixId ][ i ]
 * (i < 16 for sizeId 0, i < 64 otherwise; entries past the list's length
 * are 0).  For sizeId 3 the syntax codes matrixId 0 and 3 only (7.3.4:
 * the loop steps by 3); list[3][1], [3][2], [3][4] and [3][5] are 0.
 * dc[sizeId - 2][matrixId] is the DC value of the 16x16 and 32x32
 * matrices (scaling_list_dc_coef_minus8 + 8, or 16 for a default list,
 * or the referenced list's DC for a predicted list, 7.4.5); dc[1][1],
 * [1][2], [1][4] and [1][5] are 0.
 */
struct h265sw_scaling_list {
	uint8_t list[4][6][64];
	uint8_t dc[2][6];
};

/*
 * Sets list to the default lists (Table 7-5 for sizeId 0, Table 7-6 for
 * sizeId 1..3 with matrixId 0..2 intra and 3..5 inter; DC 16): what a
 * sequence with scaling_list_enabled_flag 1 and no list data uses
 * (7.4.3.2.1), and what a predicted list with
 * scaling_list_pred_matrix_id_delta 0 copies (7.4.5).  The entries
 * outside the lists (see struct h265sw_scaling_list) are 0.
 */
void h265sw_scaling_default(struct h265sw_scaling_list *list);

/*
 * Reads scaling_list_data() (7.3.4) into list.
 *
 * bits is at the first bit of scaling_list_data().  list is written in
 * full: the caller need not clear it first.  Applies the semantics of
 * 7.4.5 as it reads: for sizeId 0..3 and matrixId 0..5 (step 3 for
 * sizeId 3), scaling_list_pred_mode_flag 0 with
 * scaling_list_pred_matrix_id_delta 0 copies the default list (and DC
 * 16); with delta above 0 copies the list and DC of refMatrixId =
 * matrixId - delta * ( sizeId == 3 ? 3 : 1 ) (7-42, 7-43); flag 1 reads
 * the DC (scaling_list_dc_coef_minus8, sizeId above 1) and the deltas,
 * building each entry as nextCoef = ( nextCoef +
 * scaling_list_delta_coef + 256 ) % 256 starting from 8, or from the DC
 * value for sizeId above 1 (7.3.4).
 *
 * Checks scaling_list_pred_matrix_id_delta (0..matrixId, or
 * 0..matrixId / 3 for sizeId 3), scaling_list_dc_coef_minus8 (-7..247)
 * and scaling_list_delta_coef (-128..127).  A list entry that comes out 0
 * breaks 7.4.5 (ScalingFactor is greater than 0) and is EINVAL.
 *
 * Returns 0; EINVAL for a value out of range or a read error.  On error
 * list is left partly written (the caller discards it).
 */
int h265sw_scaling_read(struct h265sw_scaling_list *list, struct vcodec_bitreader *bits);

#endif
