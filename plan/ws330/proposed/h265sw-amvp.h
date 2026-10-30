/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The luma motion vector prediction of the software H.265 decoder
 * (8.5.3.2.6, 8.5.3.2.7): the spatial candidates A (A0, A1) and B (B0,
 * B1, B2) with the scaling of a candidate that refers to another picture
 * and the isScaledFlag rule, the temporal candidate, the pruning to two
 * candidates and the zero fill; and the final vector from the predictor
 * and the difference (8.5.3.2.1, 8-94 to 8-97).
 */

#ifndef LIBMEDIA_H265SW_AMVP_H
#define LIBMEDIA_H265SW_AMVP_H

#include "h265sw.h"
#include "h265sw-mvctx.h"

/*
 * Derives mvpLX of a prediction block for list X with reference index
 * ref_idx (0..num_ref[X] - 1) and mvp_lX_flag mvp_flag (8.5.3.2.6).
 *
 * The candidates (8.5.3.2.7): A from A0 then A1 (each available by
 * h265sw_pic_pb_available), first one whose list X or list Y reference
 * has the same order count as RefPicListX[ refIdxLX ] (the same picture
 * by order count and no long-term mismatch), else with scaling
 * (h265sw_tmvp_scale) of the first available one whose reference has the
 * same long-term marking; isScaledFlagLX = availableA0 || availableA1;
 * B from B0, B1, B2 likewise without scaling, then, when isScaledFlagLX
 * is 0, mvLXA takes mvLXB, and B is derived again with scaling; the
 * temporal candidate only when A and B are not both available and
 * different (h265sw_tmvp_candidate); the list mvpListLX of 8-170 with
 * duplicates of A and B removed and zero vectors appended to two.
 *
 * Writes mvp[0..1].  Never fails.
 */
void h265sw_amvp_predict(const struct h265sw_mvctx *ctx, const struct h265sw_pu *pu, unsigned list, unsigned ref_idx, unsigned mvp_flag, int16_t mvp[2]);

/*
 * Adds a motion vector difference to a predictor (8-94 to 8-97: uLX = (
 * mvpLX + mvdLX + 2^16 ) % 2^16, mvLX = uLX >= 2^15 ? uLX - 2^16 : uLX),
 * the wrap to 16 bits the clause specifies, computed without overflow.
 */
int16_t h265sw_amvp_add(int16_t mvp, int32_t mvd);

#endif
