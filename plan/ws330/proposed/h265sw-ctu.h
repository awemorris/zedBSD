/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The syntax of one coding tree unit in the software H.265 decoder:
 * coding_tree_unit() (7.3.8.2) with sao() (7.3.8.3), coding_quadtree()
 * (7.3.8.4), coding_unit() (7.3.8.5) with pcm_sample() (7.3.8.7),
 * prediction_unit() (7.3.8.6), transform_tree() (7.3.8.8),
 * mvd_coding() (7.3.8.9), transform_unit() (7.3.8.10) with delta_qp()
 * (7.3.8.14) and residual_coding() (7.3.8.11, h265sw-residual.h), and
 * their semantics (7.4.9).
 *
 * The parse of a whole CTU comes first and fills struct h265sw_ctu (the
 * coding units in decoding order, their transform units and coefficient
 * levels); the reconstruction (h265sw-recon.h) follows.  Nothing in the
 * parse depends on sample values, so the split is exact.  What later
 * syntax of the same picture reads of its neighbours (CtDepth,
 * cu_skip_flag, CuPredMode, IntraPredModeY, pcm_flag; SAO parameters) is
 * written to the picture context as soon as it is known, as is the luma
 * intra mode derivation (8.4.2), which the scan order of the residual
 * needs (7.4.9.11).
 */

#ifndef LIBMEDIA_H265SW_CTU_H
#define LIBMEDIA_H265SW_CTU_H

#include "h265sw.h"
#include "h265sw-cabac-se.h"
#include "h265sw-pic.h"
#include "h265sw-ps.h"
#include "h265sw-slice.h"

/* The most coding units of one CTU (64x64 in 8x8 units), transform units (64x64 in 4x4 units), and coefficient and PCM sample storage (one 64x64 luma and two 32x32 chroma blocks). */
#define H265SW_CTU_MAX_CUS 64U
#define H265SW_CTU_MAX_TUS 256U
#define H265SW_CTU_MAX_SAMPLES (64U * 64U + 2U * 32U * 32U)

/* An offset of struct h265sw_tu or struct h265sw_cu that names no storage. */
#define H265SW_CTU_NONE 0xFFFFU

/*
 * One transform unit: the luma block ( x, y ) of 1 << log2_size samples
 * (picture coordinates), cbf[cIdx] the coded flags of its luma block and
 * of the chroma blocks it carries, transform_skip[cIdx], coeff[cIdx] the
 * offset of the levels of each coded block in the CTU's levels (NONE
 * when not coded; ( 1 << log2 )^2 values, [ y * n + x ]).  The chroma
 * blocks: with log2_size above 2 at ( x, y ) of 1 << ( log2_size - 1 )
 * chroma samples; with log2_size 2, only the fourth unit (blkIdx 3) of a
 * split carries them, at the parent's ( xBase, yBase ) with 4 chroma
 * samples (7.3.8.10).  chroma_x, chroma_y (luma coordinates of the
 * chroma blocks' origin) and chroma_log2 (2..4, chroma samples) say
 * where, has_chroma whether this unit carries them.  pu_part the
 * prediction block (0..3) whose intra mode applies (intra NxN).
 */
struct h265sw_tu {
	uint16_t x;
	uint16_t y;
	uint8_t log2_size;
	uint8_t has_chroma;
	uint16_t chroma_x;
	uint16_t chroma_y;
	uint8_t chroma_log2;
	uint8_t pu_part;
	uint8_t cbf[3];
	uint8_t transform_skip[3];
	uint16_t coeff[3];
};

/*
 * One prediction unit of an inter coding unit: ( x, y, w, h ),
 * merge_flag (1 for a skipped CU), merge_idx, inter_pred_idc
 * (H265SW_PRED_*), ref_idx[X], mvd[X] (MvdLX; 0 when the list is not
 * used, and MvdL1 0 with mvd_l1_zero_flag and PRED_BI), mvp_flag[X].
 */
struct h265sw_pu_syntax {
	uint16_t x;
	uint16_t y;
	uint8_t w;
	uint8_t h;
	uint8_t merge_flag;
	uint8_t merge_idx;
	uint8_t inter_pred_idc;
	uint8_t ref_idx[2];
	uint8_t mvp_flag[2];
	int32_t mvd[2][2];
};

/*
 * One coding unit in decoding order: ( x, y ) and log2_size; pred_mode
 * (H265SW_MODE_*, SKIP for cu_skip_flag 1), part_mode, transquant_bypass,
 * pcm with pcm_samples (the offset of its PCM samples in the CTU's
 * samples: luma ( 1 << log2_size )^2 then Cb then Cr, each sample of
 * PcmBitDepth bits as read); intra_mode[0..3] IntraPredModeY of each
 * prediction block (one for 2Nx2N), intra_chroma IntraPredModeC (8.4.3);
 * pu[0..num_pu) for inter; cu_qp_delta CuQpDeltaVal as it stands when
 * the CU's parse ends (0 when not coded yet in its quantization group);
 * qg_start 1 when the CU starts a quantization group (( x, y ) aligned to
 * Log2MinCuQpDeltaSize, or the first CU after the reset of
 * coding_quadtree()); first_tu and num_tu its transform units in the
 * CTU's array (none for skip, PCM, or rqt_root_cbf 0).
 */
struct h265sw_cu {
	uint16_t x;
	uint16_t y;
	uint8_t log2_size;
	uint8_t pred_mode;
	uint8_t part_mode;
	uint8_t transquant_bypass;
	uint8_t pcm;
	uint16_t pcm_samples;
	uint8_t intra_mode[4];
	uint8_t intra_chroma;
	uint8_t num_pu;
	struct h265sw_pu_syntax pu[4];
	int8_t cu_qp_delta;
	uint8_t qg_start;
	uint16_t first_tu;
	uint16_t num_tu;
};

/*
 * One parsed CTU: ctb_addr_rs, ( x, y ) of its top-left luma sample,
 * cu[0..num_cu), tu[0..num_tu), levels (num_levels used) and samples
 * (num_samples used, PCM).  end_of_slice_segment is the flag read after
 * it by the caller (not by h265sw_ctu_parse).
 */
struct h265sw_ctu {
	uint32_t ctb_addr_rs;
	unsigned x;
	unsigned y;
	unsigned num_cu;
	struct h265sw_cu cu[H265SW_CTU_MAX_CUS];
	unsigned num_tu;
	struct h265sw_tu tu[H265SW_CTU_MAX_TUS];
	unsigned num_levels;
	int16_t levels[H265SW_CTU_MAX_SAMPLES];
	unsigned num_samples;
	uint8_t samples[H265SW_CTU_MAX_SAMPLES];
};

/*
 * What the parse of a CTU reads: pic (written as described above), sps,
 * pps and header of the slice segment, segment its index in
 * pic->segments, cabac the slice segment's engine and contexts.
 */
struct h265sw_ctu_ctx {
	struct h265sw_pic *pic;
	const struct h265sw_sps *sps;
	const struct h265sw_pps *pps;
	const struct h265sw_slice_header *header;
	unsigned segment;
	struct h265sw_cabac *cabac;
};

/*
 * Parses coding_tree_unit() of the CTB at ctb_addr_rs into *ctu.
 *
 * sao(): merges from the left or upper CTB only when it is in the same
 * slice and tile (leftCtbInSliceSeg uses SliceAddrRs), SaoTypeIdx,
 * offsets with SaoOffsetVal (7-72) and the edge offsets' signs, band
 * position or class; written to pic->ctb[ctb_addr_rs].sao (merged values
 * copied).  coding_quadtree(): split_cu_flag when the block lies inside
 * the picture and is above the minimum size, else inferred (split when
 * it crosses the picture's edge); the ctxInc from the left and upper
 * CtDepth (h265sw_pic_available, h265sw_se_ctxinc_left_above);
 * IsCuQpDeltaCoded and CuQpDeltaVal reset at Log2MinCuQpDeltaSize.
 * coding_unit(): every syntax element with the conditions of 7.3.8.5
 * (pcm only for 2Nx2N intra within Log2MinIpcmCbSizeY..Log2MaxIpcmCbSizeY;
 * PCM samples read raw at the byte after the terminate bin through
 * vcodec_cabac_position, then the engine restarted after them),
 * IntraPredModeY by h265sw_intra_luma_mode with the candidates of 8.4.2
 * step 2, IntraPredModeC; prediction_unit(); transform_tree() with
 * MaxTrafoDepth, IntraSplitFlag, interSplitFlag (7.4.9.8: the inferred
 * split of an inter CU of a non-2Nx2N partition at depth 0 when
 * max_transform_hierarchy_depth_inter is 0) and the inferred splits
 * above MaxTbLog2SizeY; transform_unit() with delta_qp() and the scanIdx
 * of 7.4.9.11 for residual_coding().
 *
 * The picture context gets, for each CU as soon as it is parsed: the
 * H265SW_BLK_DECODED, _SKIP, _PCM and _NOFILTER flags, pred_mode, depth
 * and intra_mode of its blocks (h265sw_pic_fill_blk).  Motion, QpY,
 * "coded" and the edges are the reconstruction's.
 *
 * Returns 0; EINVAL when the engine's error is set, a value breaks its
 * semantics (cu_qp_delta outside -26..25, an NxN intra partition above
 * the minimum size, a PCM block of the wrong size, a TransCoeffLevel out
 * of range), or a count would exceed the CTU's arrays (which a conforming
 * CTU never does).  On EINVAL the CTU is incomplete; the caller ends the
 * slice segment.
 */
int h265sw_ctu_parse(const struct h265sw_ctu_ctx *ctx, uint32_t ctb_addr_rs, struct h265sw_ctu *ctu);

#endif
