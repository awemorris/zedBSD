/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reference picture lists of a slice of the software H.265 decoder
 * (8.3.4, 8-8 to 8-11) and the collocated picture and
 * NoBackwardPredFlag (8.3.5).
 */

#ifndef LIBMEDIA_H265SW_REFLIST_H
#define LIBMEDIA_H265SW_REFLIST_H

#include "h265sw.h"
#include "h265sw-dpb.h"
#include "h265sw-slice.h"

/*
 * The reference picture lists of one slice.
 *
 * num[X] entries of list X (num_ref_idx_lX_active_minus1 + 1; 0 for a
 * list the slice type does not use).  frame[X][i] is RefPicListX[ i ]
 * (borrowed from the DPB for the slice; never NULL after
 * h265sw_reflist_build: a missing reference was replaced by a generated
 * picture before), poc[X][i] its PicOrderCntVal, long_term[X][i] 1 when
 * it came from RefPicSetLtCurr.  col is ColPic (NULL when
 * slice_temporal_mvp_enabled_flag is 0) with col_list and col_idx;
 * no_backward_pred NoBackwardPredFlag.
 */
struct h265sw_reflists {
	unsigned num[2];
	struct vcodec_frame *frame[2][H265SW_MAX_REFS];
	int32_t poc[2][H265SW_MAX_REFS];
	uint8_t long_term[2][H265SW_MAX_REFS];
	struct vcodec_frame *col;
	unsigned col_list;
	unsigned col_idx;
	int no_backward_pred;
};

/*
 * Builds the lists of a P or B slice (8.3.4) and derives ColPic and
 * NoBackwardPredFlag (8.3.5); for an I slice, empty lists.
 *
 * dpb and sets give RefPicSetStCurrBefore, StCurrAfter and LtCurr;
 * header gives num_ref_idx, the modification and collocated fields and
 * num_pic_total_curr; poc is the current PicOrderCntVal.
 * RefPicListTempX cycles through the three sets up to
 * NumRpsCurrTempListX = Max( num_ref_idx, NumPicTotalCurr ) entries
 * (8-8, 8-10); list_entry_lX selects from it when modified (8-9, 8-11).
 *
 * Returns 0; EINVAL when NumPicTotalCurr is 0 for a P or B slice, when a
 * set entry is -1 (the caller generates stand-ins first), when a
 * list_entry is not below NumRpsCurrTempListX, or when collocated_ref_idx
 * is not below the list's length.
 */
int h265sw_reflist_build(const struct h265sw_dpb *dpb, const struct h265sw_ref_sets *sets, const struct h265sw_slice_header *header, int32_t poc, struct h265sw_reflists *lists);

#endif
