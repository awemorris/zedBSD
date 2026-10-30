/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CABAC context variables of the software H.265 decoder: their
 * layout in one array, the initValue tables of the Main profile's syntax
 * elements (Tables 9-5 to 9-31 through Table 9-4) and the initialization
 * of 9.3.2.2 (9-4 to 9-7).
 *
 * One array of H265SW_CABAC_CONTEXTS bytes holds every context variable
 * of a slice; H265SW_CTX_<element> is the index of the element's ctxInc
 * 0 (ctxIdx of Table 9-4 within its initType, from 0).  Each byte is a
 * context variable as vcodec-cabac.h keeps it.  end_of_slice_segment_flag,
 * end_of_subset_one_bit and pcm_flag use DecodeTerminate and have no
 * entry.  The storage and synchronization of 9.3.2.4 and 9.3.2.5 (for
 * wavefronts and dependent slice segments) are copies of the array
 * (StatCoeff and the palette predictor are not used by the Main profile).
 */

#ifndef LIBMEDIA_H265SW_CABAC_INIT_H
#define LIBMEDIA_H265SW_CABAC_INIT_H

#include "h265sw.h"

/*
 * The index of each syntax element's first context variable, and how
 * many it has (Table 9-4, one initType): sao_merge_left_flag and
 * sao_merge_up_flag share one, as do sao_type_idx_luma and
 * sao_type_idx_chroma, ref_idx_l0 and _l1, mvp_l0_flag and _l1,
 * cbf_cb and cbf_cr.  transform_skip_flag has one for luma (+0) and one
 * for chroma (+1).  sig_coeff_flag has 44 (ctxInc 0..43, of which 42 and
 * 43 serve transform_skip_context_enabled_flag of the range extension
 * and are never used here but are kept so the tables stay whole).
 */
#define H265SW_CTX_SAO_MERGE 0U
#define H265SW_CTX_SAO_TYPE 1U
#define H265SW_CTX_SPLIT_CU 2U
#define H265SW_CTX_TRANSQUANT_BYPASS 5U
#define H265SW_CTX_SKIP 6U
#define H265SW_CTX_PRED_MODE 9U
#define H265SW_CTX_PART_MODE 10U
#define H265SW_CTX_PREV_INTRA 14U
#define H265SW_CTX_CHROMA_PRED 15U
#define H265SW_CTX_RQT_ROOT_CBF 16U
#define H265SW_CTX_MERGE_FLAG 17U
#define H265SW_CTX_MERGE_IDX 18U
#define H265SW_CTX_INTER_PRED_IDC 19U
#define H265SW_CTX_REF_IDX 24U
#define H265SW_CTX_MVP 26U
#define H265SW_CTX_SPLIT_TRANSFORM 27U
#define H265SW_CTX_CBF_LUMA 30U
#define H265SW_CTX_CBF_CHROMA 32U
#define H265SW_CTX_MVD_GT0 37U
#define H265SW_CTX_MVD_GT1 38U
#define H265SW_CTX_CU_QP_DELTA 39U
#define H265SW_CTX_TRANSFORM_SKIP 41U
#define H265SW_CTX_LAST_X 43U
#define H265SW_CTX_LAST_Y 61U
#define H265SW_CTX_CODED_SUB_BLOCK 79U
#define H265SW_CTX_SIG 83U
#define H265SW_CTX_GT1 127U
#define H265SW_CTX_GT2 151U
#define H265SW_CABAC_CONTEXTS 157U

/*
 * initValue of every context variable for each initType (0, 1, 2),
 * indexed as above.  A context variable an initType does not list (for
 * initType 0: cu_skip_flag, pred_mode_flag, part_mode ctxInc 1..3,
 * rqt_root_cbf, merge_flag, merge_idx, inter_pred_idc, ref_idx, mvp,
 * abs_mvd_greater0/1_flag) holds 154 and is never read.
 *
 * Placement by Table 9-4, where the ctxIdx of an element are not one run
 * per initType: cbf_cb and cbf_cr have ctxIdx 0..3 and 12 for initType
 * 0, 4..7 and 13 for 1, 8..11 and 14 for 2 (ctxInc 4 is the last of each);
 * abs_mvd_greater0_flag and abs_mvd_greater1_flag share Table 9-23
 * (greater0 ctxIdx 0 and 2, greater1 1 and 3 for initType 1 and 2);
 * sig_coeff_flag has ctxIdx 0..41, 42..83, 84..125 and then 126..127,
 * 128..129, 130..131 for ctxInc 42 and 43.
 *
 * Values referenced: ITU-T H.265 (01/2026) Tables 9-5 to 9-31, placed by
 * Table 9-4.
 */
extern const uint8_t h265sw_cabac_init_value[3][H265SW_CABAC_CONTEXTS];

/*
 * Returns initType (9-7): 0 for an I slice; for P, 2 when cabac_init_flag
 * is 1 else 1; for B, 1 when cabac_init_flag is 1 else 2.  slice_type is
 * H265SW_SLICE_*.
 */
unsigned h265sw_cabac_init_type(unsigned slice_type, int cabac_init_flag);

/*
 * Initializes every context variable of a slice (9.3.2.2): for each
 * index, slopeIdx and offsetIdx of initValue (9-4), m and n (9-5), and
 * the state by vcodec_cabac_context_init( m, n, slice_qp ) (9-6).
 * slice_qp is SliceQpY (any value; it is clipped to 0..51 there).
 */
void h265sw_cabac_init_contexts(uint8_t *contexts, unsigned init_type, int slice_qp);

#endif
