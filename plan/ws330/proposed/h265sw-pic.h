/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The state of the picture being decoded by the software H.265 decoder:
 * what the syntax and the reconstruction of each CTB, each block of 4x4
 * luma samples and each slice segment leave for later blocks (the
 * neighbours of context selection, intra mode prediction, motion vector
 * prediction), for the in-loop filters at the end of the picture, and for
 * the motion a later picture reads; and the availability processes that
 * read it (6.4.1, 6.4.2).
 *
 * The arrays are sized for the sequence (h265sw_pic_alloc) and reused
 * from picture to picture.  One writer at a time: the slice data of the
 * picture, then the filters.
 */

#ifndef LIBMEDIA_H265SW_PIC_H
#define LIBMEDIA_H265SW_PIC_H

#include "h265sw.h"
#include "h265sw-ps.h"
#include "h265sw-scan.h"

/* The flags of struct h265sw_blk. */
#define H265SW_BLK_DECODED 0x01U /* the block's coding unit has been parsed (a concealed block keeps it 0) */
#define H265SW_BLK_SKIP 0x02U /* cu_skip_flag of the block's coding unit */
#define H265SW_BLK_NOFILTER 0x04U /* samples the in-loop filters leave as they are: cu_transquant_bypass_flag, or pcm_flag with pcm_loop_filter_disabled_flag (8.7.2.5.7, 8.7.3) */
#define H265SW_BLK_PCM 0x08U /* pcm_flag of the block's coding unit (its intra mode counts as DC for neighbours, 8.4.2) */
#define H265SW_BLK_TU_LEFT 0x10U /* the block's left edge is a transform block edge (8.7.2.2) */
#define H265SW_BLK_TU_TOP 0x20U /* the block's top edge is a transform block edge */
#define H265SW_BLK_PU_LEFT 0x40U /* the block's left edge is a prediction block edge (8.7.2.3) */
#define H265SW_BLK_PU_TOP 0x80U /* the block's top edge is a prediction block edge */

/*
 * One block of 4x4 luma samples of the picture.
 *
 * flags: H265SW_BLK_*.  pred_mode: CuPredMode (H265SW_MODE_*; SKIP for a
 * skipped coding unit).  depth: CtDepth of its coding unit (the
 * context of split_cu_flag).  intra_mode: IntraPredModeY of the block
 * (0..34) for an intra coding unit, else 1 (INTRA_DC, what 8.4.2 uses
 * for a neighbour that is not intra).  qp_y: QpY of its coding unit
 * (0..51, the deblocking's QpQ and QpP).  coded: 1 when the luma
 * transform block covering it has a non-zero coefficient level (8.7.2.4).
 */
struct h265sw_blk {
	uint8_t flags;
	uint8_t pred_mode;
	uint8_t depth;
	uint8_t intra_mode;
	int8_t qp_y;
	uint8_t coded;
};

/*
 * The motion of one block of 4x4 luma samples (8.5.3.2): mv[X] is mvLX
 * in quarter luma samples ([0] horizontal), ref_idx[X] is refIdxLX, -1
 * when predFlagLX is 0.  An intra block has both -1.
 */
struct h265sw_mv_field {
	int16_t mv[2][2];
	int8_t ref_idx[2];
};

/*
 * The SAO parameters of one CTB and colour component (7.4.9.3): type
 * SaoTypeIdx (0 off, 1 band, 2 edge), band_or_class sao_band_position
 * (type 1) or SaoEoClass (type 2), offset[0..3] SaoOffsetVal[ cIdx ][ rx
 * ][ ry ][ i + 1 ] (7-72; for edge offset the signs of 7.4.9.3.2).
 */
struct h265sw_sao {
	uint8_t type;
	uint8_t band_or_class;
	int8_t offset[4];
};

/*
 * One CTB of the picture: slice_addr_rs is SliceAddrRs of the slice
 * segment that contains it, written by the slice data loop before the
 * CTU is parsed (UINT32_MAX until then: a CTB no slice reached belongs
 * to no slice); segment the index of that slice segment in the picture's
 * segments; tile the TileId of the CTB (from h265sw_pic_start); decoded
 * 1 once its coding tree unit was completely parsed and reconstructed
 * (the concealment's test); sao[cIdx] its SAO parameters.
 */
struct h265sw_ctb {
	uint32_t slice_addr_rs;
	uint16_t segment;
	uint16_t tile;
	uint8_t decoded;
	struct h265sw_sao sao[3];
};

/*
 * What later stages need of one slice segment (its own values for an
 * independent one; a dependent one copies its slice's): slice_addr_rs
 * SliceAddrRs; slice_type; deblocking_disabled
 * slice_deblocking_filter_disabled_flag with beta_offset_div2 and
 * tc_offset_div2; loop_filter_across_slices
 * slice_loop_filter_across_slices_enabled_flag; sao_luma and
 * sao_chroma; cb_qp_offset and cr_qp_offset (pps + slice offsets,
 * 8.6.1); num_ref[X] the active entries of RefPicListX; ref_id[X][i] the
 * vcodec_frame id of RefPicListX[ i ] (0 for "no reference picture"),
 * ref_poc[X][i] its PicOrderCntVal and ref_long[X][i] 1 when it was
 * marked "used for long-term reference" for this slice.
 */
struct h265sw_segment {
	uint32_t slice_addr_rs;
	unsigned slice_type;
	int deblocking_disabled;
	int beta_offset_div2;
	int tc_offset_div2;
	int loop_filter_across_slices;
	int sao_luma;
	int sao_chroma;
	int cb_qp_offset;
	int cr_qp_offset;
	unsigned num_ref[2];
	uint32_t ref_id[2][H265SW_MAX_REFS];
	int32_t ref_poc[2][H265SW_MAX_REFS];
	uint8_t ref_long[2][H265SW_MAX_REFS];
};

/*
 * The picture context.
 *
 * Sizes (from the active SPS): width and height in luma samples,
 * ctb_log2, min_tb_log2, width_ctbs and height_ctbs, blk_stride (blocks
 * per row: width_ctbs << ( ctb_log2 - 2 )) and blk_rows.  sps and pps
 * point to the active sets for the picture (the decoder's copies).
 *
 * Arrays (owned, sized by h265sw_pic_alloc): ctb[ctbAddrRs]; blk and mv
 * (blk_stride * blk_rows entries, index ( y >> 2 ) * blk_stride + ( x
 * >> 2 ), covering whole CTBs beyond the picture's edge); rs_to_ts,
 * ts_to_rs and tile_id of h265sw_scan_ctb_maps with tiles;
 * segments (segments_capacity entries, H265SW_MAX_SLICE_SEGMENTS) and
 * num_segments.
 */
struct h265sw_pic {
	unsigned width;
	unsigned height;
	unsigned ctb_log2;
	unsigned min_tb_log2;
	unsigned width_ctbs;
	unsigned height_ctbs;
	unsigned blk_stride;
	unsigned blk_rows;
	const struct h265sw_sps *sps;
	const struct h265sw_pps *pps;

	struct h265sw_ctb *ctb;
	struct h265sw_blk *blk;
	struct h265sw_mv_field *mv;
	uint32_t *rs_to_ts;
	uint32_t *ts_to_rs;
	uint16_t *tile_id;
	struct h265sw_tiles tiles;
	struct h265sw_segment *segments;
	unsigned segments_capacity;
	unsigned num_segments;
};

/*
 * Allocates the arrays for a sequence (width, height, ctb_log2 and
 * min_tb_log2 of the SPS) and records the sizes.  Frees what pic held
 * before.  Returns 0, EINVAL for sizes outside h265sw.h, ENOMEM (then
 * pic holds nothing).
 */
int h265sw_pic_alloc(struct h265sw_pic *pic, const struct h265sw_sps *sps);

/* Frees the arrays; pic holds nothing (a second call does nothing). */
void h265sw_pic_free(struct h265sw_pic *pic);

/*
 * Starts a picture: sets sps and pps, derives the tiles and the CTB maps
 * (h265sw_scan_tiles, h265sw_scan_ctb_maps; EINVAL when they do not fit
 * the SPS), clears every ctb entry (slice_addr_rs UINT32_MAX, decoded 0,
 * SAO off, tile from the maps), every blk entry
 * (flags 0, pred_mode INTRA, intra_mode 1, depth 0, qp_y 0, coded 0) and
 * every mv entry (ref_idx -1), and num_segments.  Returns 0 or EINVAL.
 */
int h265sw_pic_start(struct h265sw_pic *pic, const struct h265sw_sps *sps, const struct h265sw_pps *pps);

/*
 * Returns MinTbAddrZs of the minimum transform block covering luma
 * location ( x, y ) inside the picture (6-10, computed rather than
 * stored).
 */
uint32_t h265sw_pic_zscan(const struct h265sw_pic *pic, unsigned x, unsigned y);

/*
 * Derives availableN of the z-scan order block availability (6.4.1) of
 * the neighbouring location ( x_nb, y_nb ) for the current block at (
 * x_cur, y_cur ): 0 when the neighbour is outside the picture, later in
 * z-scan order (6-1, 6-2), or in a CTB whose slice_addr_rs differs from
 * that of the current block's CTB (another slice, or a CTB no slice has
 * reached: its UINT32_MAX never equals a real address) or whose tile
 * differs; else 1.  The current CTB is "reached" while it is parsed and
 * reconstructed (its slice_addr_rs is written first), so blocks inside it
 * see each other.  x_nb and y_nb may be negative.
 */
int h265sw_pic_available(const struct h265sw_pic *pic, int x_cur, int y_cur, int x_nb, int y_nb);

/*
 * Derives availableN of the prediction block availability (6.4.2): the
 * coding block ( x_cb, y_cb, n_cbs ), the prediction block ( x_pb,
 * y_pb, w, h ) with part_idx, the neighbouring location ( x_nb, y_nb ).
 * sameCb (6.4.1 when the neighbour is in another coding block), the
 * rule of the second prediction block of an NxN partition (nPbW << 1 and
 * nPbH << 1 equal to nCbS, partIdx 1, the neighbour in the lower-left
 * one), and 0 for a neighbour whose CuPredMode is MODE_INTRA.
 */
int h265sw_pic_pb_available(const struct h265sw_pic *pic, int x_cb, int y_cb, unsigned n_cbs, int x_pb, int y_pb, unsigned w, unsigned h, unsigned part_idx, int x_nb, int y_nb);

/* Returns the block entry covering luma location ( x, y ) (inside the CTB-rounded picture). */
struct h265sw_blk *h265sw_pic_blk(const struct h265sw_pic *pic, unsigned x, unsigned y);

/* Returns the motion entry covering luma location ( x, y ). */
struct h265sw_mv_field *h265sw_pic_mv(const struct h265sw_pic *pic, unsigned x, unsigned y);

/*
 * Writes a rectangle of block entries: x, y, w, h in luma samples
 * (multiples of 4); every entry gets *blk except that the edge flags
 * H265SW_BLK_TU_* and _PU_* are kept as they were.
 */
void h265sw_pic_fill_blk(struct h265sw_pic *pic, unsigned x, unsigned y, unsigned w, unsigned h, const struct h265sw_blk *blk);

/* Writes a rectangle of motion entries (x, y, w, h multiples of 4). */
void h265sw_pic_fill_mv(struct h265sw_pic *pic, unsigned x, unsigned y, unsigned w, unsigned h, const struct h265sw_mv_field *mv);

/*
 * Marks the edges of a block rectangle: with tu set, H265SW_BLK_TU_LEFT
 * on the blocks of its left column and _TU_TOP on those of its top row;
 * with tu clear, the _PU_ flags.
 */
void h265sw_pic_mark_edges(struct h265sw_pic *pic, unsigned x, unsigned y, unsigned w, unsigned h, int tu);

#endif
