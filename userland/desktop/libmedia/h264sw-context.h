/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoding state of the software H.264 decoder that the parts of
 * the macroblock layer share: the picture being decoded, the slice being
 * decoded, and the syntax of the macroblock being decoded.
 *
 * Ownership: the decoder (h264sw-decoder.c) owns one picture context and
 * one slice context and fills them; the parsing (h264sw-macroblock.c,
 * h264sw-cavlc.c, h264sw-cabac-se.c), the reconstruction
 * (h264sw-recon.c with the intra, inter, transform and direct parts) and
 * the deblocking (h264sw-deblock.c) read them and write only the fields
 * their comments name.  Nothing here allocates; the decoder sizes the
 * arrays.
 */

#ifndef LIBMEDIA_H264SW_CONTEXT_H
#define LIBMEDIA_H264SW_CONTEXT_H

#include "h264sw.h"
#include "h264sw-bits.h"
#include "h264sw-cabac.h"
#include "h264sw-direct.h"
#include "h264sw-frame.h"
#include "h264sw-mb.h"
#include "h264sw-ps.h"
#include "h264sw-reflist.h"
#include "h264sw-slice.h"
#include "h264sw-transform.h"
#include "h264sw-weight.h"

/*
 * What the deblocking filter needs of each slice of a picture: its
 * disable_deblocking_filter_idc and filter offsets (8.7), copied from the
 * slice header when the slice starts.  slice_type is kept for the
 * boundary strength's SP and SI rule, which never applies here.
 */
struct h264sw_slice_params {
	unsigned disable_deblocking;
	int filter_offset_a;
	int filter_offset_b;
	unsigned slice_type;
};

/*
 * The picture being decoded.
 *
 * frame: the frame the samples go to (one reference held by the decoder
 *   while the picture is decoded).
 * sps, pps: the active parameter sets (pointers into the decoder's store,
 *   valid for the picture: a parameter set NAL unit that would replace
 *   an active set within a picture is refused by the decoder).
 * levelscale: the scaling functions of the picture's scaling lists.
 * mbs: size_mbs entries, one per macroblock address; width_mbs and
 *   height_mbs the picture's size in macroblocks.
 * slices: the parameters of each slice, by slice number; slice_count
 *   slices so far, at most slice_capacity (size_mbs: a slice has at
 *   least one macroblock).
 * decoded_mbs: the macroblocks decoded so far (the decoder conceals the
 *   rest when the picture ends with fewer than size_mbs).
 * chroma_qp_offset[0] and [1]: chroma_qp_index_offset and
 *   second_chroma_qp_index_offset (Cb and Cr), for the reconstruction and
 *   the deblocking.
 */
struct h264sw_picture_ctx {
	struct h264sw_frame *frame;
	const struct h264sw_sps *sps;
	const struct h264sw_pps *pps;
	struct h264sw_levelscale levelscale;
	struct h264sw_mb *mbs;
	unsigned width_mbs;
	unsigned height_mbs;
	unsigned size_mbs;
	struct h264sw_slice_params *slices;
	unsigned slice_count;
	unsigned slice_capacity;
	unsigned decoded_mbs;
	int chroma_qp_offset[2];
};

/*
 * The syntax of one macroblock as parsed (7.3.5, 7.4.5), before it is
 * reconstructed.  The parser fills it; the reconstruction reads it.
 *
 * mb_type, type: the value read and its meaning (for a skipped
 *   macroblock, mb_type is unused and type is the skip type).
 * sub_mb_type, sub: the four sub-macroblock types and their meanings
 *   (for the 8x8 kinds).
 * prev_intra_flag, rem_intra_mode: prev_intra4x4_pred_mode_flag and
 *   rem_intra4x4_pred_mode of the 16 blocks (I_4X4), or the
 *   prev_intra8x8 and rem_intra8x8 values in entries 0..3 (I_8X8).
 * ref_idx[list][part]: ref_idx_l0 and ref_idx_l1 of each macroblock
 *   partition (1, 2 or 4 parts), 0 when not sent but used (one
 *   reference, or P_8x8ref0), -1 when the list is not used.
 * mvd[list][part][sub][c]: mvd_l0 and mvd_l1 of each partition and
 *   sub-macroblock partition, c 0 horizontal and 1 vertical.
 * cbp_luma, cbp_chroma: CodedBlockPatternLuma and CodedBlockPatternChroma.
 * qp_delta: mb_qp_delta (0 when absent).
 * luma_dc: Intra16x16DCLevel in scan order (16 levels).
 * luma[blk]: for each luma4x4BlkIdx, the 16 levels of the block in scan
 *   order; for an Intra_16x16 macroblock, entry 0 is left 0 and the AC
 *   levels are at 1..15 (i16x16AClevel shifted by one, as 8.5.2 builds
 *   lumaList).
 * luma8x8[blk]: for an 8x8 transform, the 64 levels of each luma8x8BlkIdx
 *   in 8x8 scan order (with CAVLC, the four interleaved 4x4 lists of
 *   7.3.5.3.2 already put together: luma8x8[b][4 * i + k] is the level
 *   i of the 4x4 list k).
 * chroma_dc[c]: the four ChromaDCLevel values of Cb (c 0) and Cr (c 1).
 * chroma_ac[c][blk]: the levels of each chroma4x4BlkIdx with entry 0
 *   left 0 and the AC levels at 1..15.
 * pcm: the 384 samples of I_PCM (256 luma, 64 Cb, 64 Cr, raster order).
 *
 * Levels are int32_t; the parser keeps every level within -2^15 ..
 * 2^15 - 1 (the range of cij that 8.5.12.1 and 8.5.13.1 give for 8
 * bits) and refuses a block that leaves it.
 */
struct h264sw_mb_syntax {
	unsigned mb_type;
	struct h264sw_mb_type type;
	unsigned sub_mb_type[4];
	struct h264sw_sub_mb_type sub[4];
	uint8_t prev_intra_flag[16];
	uint8_t rem_intra_mode[16];
	int8_t ref_idx[2][4];
	int16_t mvd[2][4][4][2];
	unsigned cbp_luma;
	unsigned cbp_chroma;
	int qp_delta;
	int32_t luma_dc[16];
	int32_t luma[16][16];
	int32_t luma8x8[4][64];
	int32_t chroma_dc[2][4];
	int32_t chroma_ac[2][4][16];
	uint8_t pcm[384];
};

/*
 * The slice being decoded.
 *
 * picture: the picture it belongs to.  header: its header (owned by the
 * decoder).  slice_num: its number in the picture (index of
 * picture->slices).  lists: its reference picture lists.  weights, direct:
 * what its prediction prepares once.
 *
 * bits: the reader positioned in slice_data() (CAVLC reads from it; with
 * CABAC it is used only up to the alignment).  cabac_mode:
 * entropy_coding_mode_flag; cabac: the arithmetic decoder.
 *
 * mb_addr: CurrMbAddr.  n: the neighbours of the current macroblock
 * (h264sw_mb_neighbours), n.current is the entry being written.
 * decoded_4x4: the bits (luma4x4BlkIdx) of the current macroblock's 4x4
 * blocks whose motion is already derived, for h264sw_mvpred.
 * qp: QPY of the last macroblock (QPY,PREV of 7.4.5), first
 * SliceQPY.  prev_qp_delta_nonzero: whether the previous macroblock in
 * decoding order had a non-zero mb_qp_delta (the CABAC context of
 * 9.3.3.1.1.5; 0 at the start of a slice).  prev_mb_addr: the previous
 * macroblock in decoding order in the slice (-1 at the start), whose
 * kind and cbp that context also reads.
 * skip_run: the mb_skip_run left (CAVLC), -1 when none is being counted.
 * prev_skipped: whether the previous macroblock was skipped (the
 * mb_field_decoding_flag inference does not apply to frames; kept for
 * the CAVLC loop of 7.3.4).
 * concealed: the macroblocks of this slice decoded with errors.
 * syntax: the current macroblock's syntax.
 */
struct h264sw_slice_ctx {
	struct h264sw_picture_ctx *picture;
	const struct h264sw_slice_header *header;
	int32_t slice_num;
	struct h264sw_reflist lists;
	struct h264sw_weights weights;
	struct h264sw_direct direct;

	struct h264sw_bits bits;
	int cabac_mode;
	struct h264sw_cabac cabac;

	unsigned mb_addr;
	struct h264sw_neighbours n;
	uint32_t decoded_4x4;
	int qp;
	int prev_qp_delta_nonzero;
	int32_t prev_mb_addr;
	int32_t skip_run;
	int prev_skipped;
	unsigned concealed;

	struct h264sw_mb_syntax syntax;
};

#endif
