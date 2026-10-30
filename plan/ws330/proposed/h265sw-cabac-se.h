/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CABAC syntax elements of the software H.265 decoder above the
 * residual: each function decodes one syntax element of 7.3.8.3 to
 * 7.3.8.10 (other than residual_coding(), h265sw-residual.h) by its
 * binarization (9.3.3, Tables 9-43 to 9-47) and the ctxInc of each bin
 * (Table 9-48, 9.3.4.2.2), from the engine (vcodec-cabac.h) and the
 * slice's context array (h265sw-cabac-init.h).
 *
 * The functions decode; they do not check the result against the
 * semantics (the caller does, before using it as an index).  Values that
 * depend on neighbours (the ctxInc of split_cu_flag and cu_skip_flag,
 * 9.3.4.2.2 and Table 9-49) are computed by the caller with
 * h265sw_se_ctxinc_left_above and passed in.  Every function returns
 * garbage but stays in bounds when the engine's error is set; the caller
 * checks the engine's error once per coding unit.
 */

#ifndef LIBMEDIA_H265SW_CABAC_SE_H
#define LIBMEDIA_H265SW_CABAC_SE_H

#include "h265sw.h"
#include "h265sw-cabac-init.h"
#include "vcodec-cabac.h"

/* The engine and context variables of one slice segment's arithmetic decoding. */
struct h265sw_cabac {
	struct vcodec_cabac engine;
	uint8_t contexts[H265SW_CABAC_CONTEXTS];
};

/* PartMode (Table 7-10). */
#define H265SW_PART_2Nx2N 0U
#define H265SW_PART_2NxN 1U
#define H265SW_PART_Nx2N 2U
#define H265SW_PART_NxN 3U
#define H265SW_PART_2NxnU 4U
#define H265SW_PART_2NxnD 5U
#define H265SW_PART_nLx2N 6U
#define H265SW_PART_nRx2N 7U

/* inter_pred_idc (Table 7-11). */
#define H265SW_PRED_L0 0U
#define H265SW_PRED_L1 1U
#define H265SW_PRED_BI 2U

/* Returns ( condL && availableL ) + ( condA && availableA ) (Table 9-49): the ctxInc of split_cu_flag and cu_skip_flag. */
unsigned h265sw_se_ctxinc_left_above(int available_left, int cond_left, int available_above, int cond_above);

/* sao_merge_left_flag or sao_merge_up_flag (one context bin). */
unsigned h265sw_se_sao_merge(struct h265sw_cabac *cabac);

/* sao_type_idx_luma or _chroma: TR with cMax 2 (bin 0 context, bin 1 bypass); 0..2. */
unsigned h265sw_se_sao_type(struct h265sw_cabac *cabac);

/* sao_offset_abs: TR with cMax ( 1 << ( Min( bitDepth, 10 ) - 5 ) ) - 1 = 7, all bypass; 0..7. */
unsigned h265sw_se_sao_offset_abs(struct h265sw_cabac *cabac);

/* sao_offset_sign, sao_band_position (FL 5 bits) and sao_eo_class (FL 2 bits): bypass bins as an unsigned number of count bits. */
unsigned h265sw_se_bypass_fl(struct h265sw_cabac *cabac, unsigned count);

/* split_cu_flag with ctxInc 0..2. */
unsigned h265sw_se_split_cu(struct h265sw_cabac *cabac, unsigned ctx_inc);

/* cu_transquant_bypass_flag. */
unsigned h265sw_se_transquant_bypass(struct h265sw_cabac *cabac);

/* cu_skip_flag with ctxInc 0..2. */
unsigned h265sw_se_skip(struct h265sw_cabac *cabac, unsigned ctx_inc);

/* pred_mode_flag (1 means MODE_INTRA). */
unsigned h265sw_se_pred_mode(struct h265sw_cabac *cabac);

/*
 * part_mode by Table 9-45, returned as PartMode (H265SW_PART_*): intra
 * selects between 2Nx2N and NxN (only called at log2_cb_size ==
 * min_cb_log2); inter uses the columns of log2_cb_size against
 * min_cb_log2, amp and log2_cb_size == 3.  Bin contexts by Table 9-48
 * (0, 1, then 2 at the minimum size or 3 with AMP; the AMP's last bin
 * bypass).
 */
unsigned h265sw_se_part_mode(struct h265sw_cabac *cabac, int intra, unsigned log2_cb_size, unsigned min_cb_log2, int amp);

/* prev_intra_luma_pred_flag. */
unsigned h265sw_se_prev_intra(struct h265sw_cabac *cabac);

/* mpm_idx: TR with cMax 2, bypass; 0..2. */
unsigned h265sw_se_mpm_idx(struct h265sw_cabac *cabac);

/* rem_intra_luma_pred_mode: FL 5 bits, bypass; 0..31. */
unsigned h265sw_se_rem_intra(struct h265sw_cabac *cabac);

/* intra_chroma_pred_mode by Table 9-46 (bin 0 context, then two bypass bins); 0..4. */
unsigned h265sw_se_chroma_pred(struct h265sw_cabac *cabac);

/* rqt_root_cbf. */
unsigned h265sw_se_rqt_root_cbf(struct h265sw_cabac *cabac);

/* merge_flag. */
unsigned h265sw_se_merge_flag(struct h265sw_cabac *cabac);

/* merge_idx: TR with cMax max_merge_cand - 1 (1..4; bin 0 context, the rest bypass); 0..cMax.  max_merge_cand 1 decodes nothing and returns 0. */
unsigned h265sw_se_merge_idx(struct h265sw_cabac *cabac, unsigned max_merge_cand);

/*
 * inter_pred_idc by Table 9-47 (H265SW_PRED_*): with nPbW + nPbH != 12,
 * bin 0 with ctxInc ct_depth (0..3) and, when it is 0, bin 1 with
 * ctxInc 4; with nPbW + nPbH == 12, one bin with ctxInc 4.
 */
unsigned h265sw_se_inter_pred_idc(struct h265sw_cabac *cabac, unsigned pb_width, unsigned pb_height, unsigned ct_depth);

/* ref_idx_lX: TR with cMax num_ref - 1 (num_ref 2..15; bins 0 and 1 context 0 and 1, the rest bypass). */
unsigned h265sw_se_ref_idx(struct h265sw_cabac *cabac, unsigned num_ref);

/* mvp_l0_flag or mvp_l1_flag. */
unsigned h265sw_se_mvp(struct h265sw_cabac *cabac);

/*
 * mvd_coding() (7.3.8.9, 7.4.9.9): abs_mvd_greater0_flag and
 * greater1_flag of both components, abs_mvd_minus2 (EG1, bypass,
 * 9.3.3.3) and mvd_sign_flag; writes mvd[0] and mvd[1] = lMvd
 * (7.4.9.9).  MvdLX lies in -2^15..2^15 - 1 (7.4.9.9): an EG1 prefix
 * longer than 16 bins, or a value outside that range, sets the engine's
 * error and gives 0.
 */
void h265sw_se_mvd(struct h265sw_cabac *cabac, int32_t mvd[2]);

/* split_transform_flag with ctxInc 5 - log2_trafo_size (log2_trafo_size 3..5). */
unsigned h265sw_se_split_transform(struct h265sw_cabac *cabac, unsigned log2_trafo_size);

/* cbf_luma with ctxInc trafoDepth == 0 ? 1 : 0. */
unsigned h265sw_se_cbf_luma(struct h265sw_cabac *cabac, unsigned trafo_depth);

/* cbf_cb or cbf_cr with ctxInc trafoDepth (0..4). */
unsigned h265sw_se_cbf_chroma(struct h265sw_cabac *cabac, unsigned trafo_depth);

/*
 * cu_qp_delta_abs and, when it is not 0, cu_qp_delta_sign_flag
 * (9.3.3.10: prefix TR cMax 5 with ctxInc 0 for bin 0 and 1 for bins
 * 1..4, suffix EG0 bypass); returns CuQpDeltaVal (7.4.9.14).  An EG0
 * prefix longer than 31 bins sets the engine's error; the caller checks
 * the range -26..25 (8 bits).
 */
int32_t h265sw_se_cu_qp_delta(struct h265sw_cabac *cabac);

/* end_of_slice_segment_flag, end_of_subset_one_bit or pcm_flag: DecodeTerminate (9.3.4.3.5). */
unsigned h265sw_se_terminate(struct h265sw_cabac *cabac);

#endif
