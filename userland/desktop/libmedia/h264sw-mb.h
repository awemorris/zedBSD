/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The macroblocks of the software H.264 decoder: what each macroblock of
 * the picture being decoded keeps for its neighbours and for the
 * deblocking filter, the meaning of mb_type and sub_mb_type (Tables 7-11,
 * 7-13, 7-14, 7-17, 7-18), the scans of blocks inside a macroblock
 * (6.4.3, 6.4.7, 6.4.13) and the neighbours of a macroblock, a block or a
 * sample location in a frame without MBAFF (6.4.8, 6.4.9, 6.4.11,
 * 6.4.12.1).
 *
 * Block indices are always those of the specification: luma4x4BlkIdx
 * 0..15 in the order of 6.4.3 (8x8 quadrants in raster order, 4x4
 * blocks in raster order inside each), luma8x8BlkIdx 0..3 in raster
 * order, chroma4x4BlkIdx 0..3 in raster order (6.4.7).
 */

#ifndef LIBMEDIA_H264SW_MB_H
#define LIBMEDIA_H264SW_MB_H

#include "h264sw.h"
#include "h264sw-frame.h"

/*
 * The kinds of macroblock, the decoder's own grouping of mb_type
 * values: what the parsing and the reconstruction branch on.
 * H264SW_MB_I_NXN is I_NxN before transform_size_8x8_flag is read; it
 * becomes H264SW_MB_I_4X4 or H264SW_MB_I_8X8.
 */
#define H264SW_MB_I_NXN 0U
#define H264SW_MB_I_4X4 1U
#define H264SW_MB_I_8X8 2U
#define H264SW_MB_I_16X16 3U
#define H264SW_MB_I_PCM 4U
#define H264SW_MB_P_SKIP 5U
#define H264SW_MB_P 6U
#define H264SW_MB_P_8X8 7U
#define H264SW_MB_P_8X8REF0 8U
#define H264SW_MB_B_SKIP 9U
#define H264SW_MB_B_DIRECT 10U
#define H264SW_MB_B 11U
#define H264SW_MB_B_8X8 12U

/* The prediction of a partition (MbPartPredMode, SubMbPredMode): none, list 0, list 1, both, direct. */
#define H264SW_PRED_NONE 0U
#define H264SW_PRED_L0 1U
#define H264SW_PRED_L1 2U
#define H264SW_PRED_BI 3U
#define H264SW_PRED_DIRECT 4U

/*
 * What one mb_type value means (Tables 7-11, 7-13 and 7-14).
 *
 * kind is H264SW_MB_*.  parts is NumMbPart (1, 2 or 4; 4 for the 8x8
 * kinds, whose partitions are sub-macroblocks; 0 for intra kinds), and
 * part_width and part_height are MbPartWidth and MbPartHeight in luma
 * samples.  pred[0] and pred[1] are MbPartPredMode of partitions 0 and 1
 * (H264SW_PRED_*; NONE for the intra kinds and the second partition of a
 * one-partition type; DIRECT for B_Direct_16x16 and B_Skip).  For
 * I_16x16: intra16x16_mode is Intra16x16PredMode 0..3, cbp_luma 0 or 15
 * and cbp_chroma 0..2 (CodedBlockPatternLuma and CodedBlockPatternChroma
 * of Table 7-11).
 */
struct h264sw_mb_type {
	unsigned kind;
	unsigned parts;
	unsigned part_width;
	unsigned part_height;
	unsigned pred[2];
	unsigned intra16x16_mode;
	unsigned cbp_luma;
	unsigned cbp_chroma;
};

/*
 * What one sub_mb_type value means (Tables 7-17 and 7-18): parts is
 * NumSubMbPart (1, 2 or 4; 4 for B_Direct_8x8, whose motion is derived
 * per 4x4 or per 8x8 as direct_8x8_inference_flag says), part_width and
 * part_height SubMbPartWidth and SubMbPartHeight, pred SubMbPredMode.
 */
struct h264sw_sub_mb_type {
	unsigned parts;
	unsigned part_width;
	unsigned part_height;
	unsigned pred;
};

/*
 * What one macroblock of the current picture leaves for the macroblocks
 * after it, the deblocking filter and the pictures after it.
 *
 * The decoder keeps an array of these, one per macroblock address, for
 * the picture being decoded; it is reset at the start of each picture
 * (slice -1 for every entry) and read until the picture is deblocked and
 * its motion copied into its frame.
 *
 * slice: the number, within the picture, of the slice the macroblock
 *   belongs to (0, 1, ... in decoding order), or -1 while the macroblock
 *   is not decoded.  Two macroblocks are in the same slice when their
 *   numbers are equal; that is the availability rule of 6.4.8.
 * kind: H264SW_MB_* (I_NXN resolved to I_4X4 or I_8X8).
 * cbp: CodedBlockPatternLuma in bits 0..3 (bit b for luma8x8BlkIdx b)
 *   and CodedBlockPatternChroma (0..2) in bits 4..5; I_PCM records 0x2f
 *   (all coded) for the CABAC context rules that ask about it.
 * transform_8x8: transform_size_8x8_flag.
 * qp: QPY of the macroblock (after mb_qp_delta; for a skipped macroblock
 *   the QPY carried from the macroblock before; 0 for I_PCM, as the
 *   deblocking filter needs, 8.7.2.2).
 * chroma_pred_mode: intra_chroma_pred_mode 0..3 (0 for other kinds).
 * intra4x4_mode: Intra4x4PredMode of each luma4x4BlkIdx for I_4X4, and
 *   Intra8x8PredMode of each 8x8 block written to its four 4x4 entries
 *   for I_8X8; 2 (DC) for every other kind, which is what 8.3.1.1 and
 *   8.3.2.1 use for a neighbour that is not I_4X4 or I_8X8 (the
 *   availability and constrained_intra_pred rules are applied by the
 *   derivation, not stored here).
 * total_coeff: TotalCoeff( coeff_token ) of each 4x4 block for the nC of
 *   CAVLC (9.2.1): [0..15] luma by luma4x4BlkIdx (for an Intra_16x16
 *   macroblock the AC blocks; for an 8x8 transform with CAVLC each 4x4
 *   part's own count), [16..19] Cb AC and [20..23] Cr AC by
 *   chroma4x4BlkIdx.  0 for skipped macroblocks; 16 for every block of
 *   I_PCM (9.2.1).
 * coded_flags: the coded_block_flag of each block for the CABAC context
 *   of 9.3.3.1.1.9, one bit each: bits 0..15 luma 4x4 blocks
 *   (luma4x4BlkIdx; for an 8x8 transform all four bits of an 8x8 block
 *   are its inferred flag, 1 when its CodedBlockPatternLuma bit is set),
 *   bit 16 the Intra_16x16 luma DC, bit 17 Cb DC, bit 18 Cr DC, bits
 *   19..22 Cb AC and 23..26 Cr AC (chroma4x4BlkIdx).  All set for I_PCM,
 *   none for skipped macroblocks.
 * nonzero: bit n is 1 when luma4x4BlkIdx n has a non-zero transform
 *   coefficient, for the boundary strength of 8.7.2.1 (an 8x8 transform
 *   block with any non-zero coefficient sets its four bits; the
 *   Intra_16x16 DC coefficients count for every block).
 * direct: bit n is 1 when 8x8 quadrant n was predicted in direct mode
 *   (B_Skip and B_Direct_16x16: all four; B_Direct_8x8 sub-macroblocks),
 *   for the CABAC contexts of ref_idx and mvd (9.3.3.1.1.6, 9.3.3.1.1.7).
 * mvd: the absolute mvd of each 4x4 block, [list][luma4x4BlkIdx][0
 *   horizontal, 1 vertical], capped at 255, for the CABAC context of mvd
 *   (9.3.3.1.1.7); 0 where no mvd was sent (skip, direct, intra).
 * motion: the motion vectors, reference indices, reference frames and
 *   intra flag (h264sw-frame.h), copied into the frame when the picture
 *   is done.
 */
struct h264sw_mb {
	int32_t slice;
	uint8_t kind;
	uint8_t cbp;
	uint8_t transform_8x8;
	uint8_t qp;
	uint8_t chroma_pred_mode;
	uint8_t direct;
	uint8_t intra4x4_mode[16];
	uint8_t total_coeff[24];
	uint32_t coded_flags;
	uint16_t nonzero;
	uint8_t mvd[2][16][2];
	struct h264sw_motion motion;
};

/*
 * The neighbouring macroblocks of the current one (6.4.9 for frames
 * without MBAFF): A left, B above, C above-right, D above-left, each
 * NULL when not available (outside the picture, not decoded yet, or in
 * another slice, 6.4.8).  current is the current macroblock's entry;
 * addr its address; x and y its position in macroblocks.
 */
struct h264sw_neighbours {
	struct h264sw_mb *current;
	struct h264sw_mb *a;
	struct h264sw_mb *b;
	struct h264sw_mb *c;
	struct h264sw_mb *d;
	unsigned addr;
	unsigned x;
	unsigned y;
};

/* The position of each luma4x4BlkIdx in its macroblock, in luma samples (6.4.3): x and y. */
extern const uint8_t h264sw_blk4x4_x[16];
extern const uint8_t h264sw_blk4x4_y[16];

/*
 * Gives the meaning of an mb_type value of a slice type.
 *
 * slice_type is H264SW_SLICE_I, _P or _B; mb_type the value read (I:
 * 0..25; P: 0..4 and 5..30 for the intra types of Table 7-11 at mb_type
 * - 5; B: 0..22 and 23..48 at mb_type - 23).  Returns 0 and *type, or
 * EINVAL for a value outside its slice type's range.  The skipped types
 * have no mb_type: h264sw_mb_skip_type gives them.
 */
int h264sw_mb_type_of(unsigned slice_type, unsigned mb_type, struct h264sw_mb_type *type);

/* Gives the meaning of P_Skip (slice_type P) or B_Skip (slice_type B), as inferred by 7.4.4. */
void h264sw_mb_skip_type(unsigned slice_type, struct h264sw_mb_type *type);

/*
 * Gives the meaning of a sub_mb_type value (Table 7-17 for P: 0..3,
 * Table 7-18 for B: 0..12).  Returns 0 and *type, or EINVAL for a value
 * outside the range.
 */
int h264sw_sub_mb_type_of(unsigned slice_type, unsigned sub_mb_type, struct h264sw_sub_mb_type *type);

/*
 * Finds the neighbouring macroblocks of a macroblock (6.4.8, 6.4.9).
 *
 * mbs is the picture's array of width_mbs * height_mbs entries; addr is
 * the current macroblock address (below the array's size); the current
 * entry's slice is already set.  A neighbour is available when its
 * address is inside the picture, in the same column range (A and D need
 * x above 0, C needs x below width_mbs - 1, B, C and D need y above 0)
 * and its slice equals the current one.  Fills *n.
 */
void h264sw_mb_neighbours(struct h264sw_mb *mbs, unsigned width_mbs, unsigned addr, struct h264sw_neighbours *n);

/*
 * Finds the macroblock that holds a location near the current one (6.4.12.1, Table 6-3).
 *
 * (xn, yn) is a location relative to the upper-left sample of the
 * current macroblock, in samples of a component whose macroblock is
 * max_w by max_h (16 by 16 for luma, 8 by 8 for chroma).  Returns the
 * entry of mbAddrN (n->current, a, b, c or d, or NULL when not
 * available or when Table 6-3 says "not available") and sets *xw and
 * *yw to the location inside it (6-34, 6-35).
 */
struct h264sw_mb *h264sw_mb_locate(const struct h264sw_neighbours *n, int xn, int yn, unsigned max_w, unsigned max_h, unsigned *xw, unsigned *yw);

/* Gives the luma4x4BlkIdx of the 4x4 block holding luma location (x, y) of a macroblock, 0..15 each (6.4.13.1). */
unsigned h264sw_blk4x4_at(unsigned x, unsigned y);

/*
 * Finds the left (A) or upper (B) neighbour of a luma 4x4 block (6.4.11.4).
 *
 * blk is luma4x4BlkIdx; upper is 0 for A (location x - 1, y) and 1 for B
 * (x, y - 1).  Returns the macroblock holding the neighbour (NULL when
 * not available) and its luma4x4BlkIdx in *blk_n.
 */
struct h264sw_mb *h264sw_mb_neighbour_4x4(const struct h264sw_neighbours *n, unsigned blk, int upper, unsigned *blk_n);

/* As h264sw_mb_neighbour_4x4 for a luma 8x8 block (6.4.11.2): blk is luma8x8BlkIdx, *blk_n the neighbour's. */
struct h264sw_mb *h264sw_mb_neighbour_8x8(const struct h264sw_neighbours *n, unsigned blk, int upper, unsigned *blk_n);

/* As h264sw_mb_neighbour_4x4 for a chroma 4x4 block of 4:2:0 (6.4.11.5): blk is chroma4x4BlkIdx 0..3. */
struct h264sw_mb *h264sw_mb_neighbour_chroma(const struct h264sw_neighbours *n, unsigned blk, int upper, unsigned *blk_n);

#endif
