/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The merge mode of the software H.265 decoder (8.5.3.2.2 to
 * 8.5.3.2.5): the spatial candidates A1, B1, B0, A0, B2 with their
 * pruning and the parallel merge level, the temporal candidate with
 * refIdx 0, the combined bi-predictive candidates of B slices (Table
 * 8-7), the zero candidates, the choice by merge_idx, and the 8x4 and
 * 4x8 rule that turns a bi-predictive result into list 0 only.
 */

#ifndef LIBMEDIA_H265SW_MERGE_H
#define LIBMEDIA_H265SW_MERGE_H

#include "h265sw.h"
#include "h265sw-mvctx.h"

/*
 * Derives the motion of a prediction block in merge mode (8.5.3.2.2).
 *
 * pu is the prediction block (its coding block, partIdx and PartMode);
 * with Log2ParMrgLevel above 2 and nCbS 8 every prediction unit of the
 * coding unit uses the 2Nx2N list (8-110 to 8-113).  merge_idx is
 * 0..MaxNumMergeCand - 1 (checked by the caller).
 *
 * Spatial candidates (8.5.3.2.3): the five locations relative to the
 * prediction block, each available by h265sw_pic_pb_available (6.4.2),
 * not inside the same merge estimation region ( ( xPb >> Log2ParMrgLevel
 * ) == ( xNb >> Log2ParMrgLevel ) and likewise for y), A1 not for the
 * second partition of Nx2N, nLx2N, nRx2N, B1 not for the second
 * partition of 2NxN, 2NxnU, 2NxnD, with the pairwise comparisons of the
 * clause and B2 only when fewer than four came before.  Temporal
 * (8.5.3.2.8 through h265sw_tmvp_candidate with refIdx 0 of each list,
 * L1 only for B slices).  Combined bi-predictive (8.5.3.2.4: l0CandIdx and
 * l1CandIdx of Table 8-7, kept when DiffPicOrderCnt of the two references
 * is not 0 or the vectors differ) up to MaxNumMergeCand, B slices only.
 * Zero (8.5.3.2.5: numRefIdx the list 0 count for P, the smaller count
 * for B; refIdx zeroIdx while below numRefIdx, else 0).
 *
 * The result (8.5.3.2.2 step 9 and 10): out is the candidate at
 * merge_idx; when it uses both lists and nOrigPbW + nOrigPbH is 12, list
 * 1 is dropped (refIdxL1 -1).  The vectors are not changed (no mvd in
 * merge mode).  Never fails: the zero candidates fill the list.
 */
void h265sw_merge_derive(const struct h265sw_mvctx *ctx, const struct h265sw_pu *pu, unsigned merge_idx, struct h265sw_mv_field *out);

/*
 * Builds the whole merge candidate list of a prediction block, as
 * h265sw_merge_derive does before it picks one: list[0 ..
 * MaxNumMergeCand) in mergeCandList order (8-119, then the combined and
 * zero candidates), without the 8x4 and 4x8 rule (h265sw_merge_derive
 * applies it to the one it picks; a caller choosing among the list, as
 * the encoder does, applies it with h265sw_merge_restrict).  Returns
 * MaxNumMergeCand.
 */
unsigned h265sw_merge_candidates(const struct h265sw_mvctx *ctx, const struct h265sw_pu *pu, struct h265sw_mv_field list[5]);

/*
 * Applies the rule of 8.5.3.2.2 step 10 to a chosen candidate: when it
 * uses both lists and orig_w + orig_h (the prediction block's own size)
 * is 12, list 1 is dropped (refIdxL1 -1).
 */
void h265sw_merge_restrict(struct h265sw_mv_field *motion, unsigned orig_w, unsigned orig_h);

#endif
