/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The intra prediction of the software H.264 decoder (8.3): the
 * prediction modes of Intra_4x4 and Intra_8x8 blocks (8.3.1.1,
 * 8.3.2.1), the neighbouring samples a block is predicted from and their
 * availability, and the predictions themselves (Intra_4x4 8.3.1.2,
 * Intra_8x8 8.3.2.2 with its reference sample filtering 8.3.2.2.1,
 * Intra_16x16 8.3.3, chroma 8.3.4).
 *
 * The predictors are pure functions of an edge (the neighbouring samples
 * and which of them are available) so each can be tested alone; the
 * edge gathering reads the current picture through the slice context, or
 * (for the reconstruction from a macroblock's recorded work) through the
 * frame and the parts of the edge known to be available.
 */

#ifndef LIBMEDIA_H264SW_INTRA_H
#define LIBMEDIA_H264SW_INTRA_H

#include "h264sw.h"

struct h264sw_frame;
struct h264sw_slice_ctx;

/* The parts of an edge that are available for Intra prediction (bits of struct h264sw_intra_edge's available). */
#define H264SW_EDGE_LEFT 1U
#define H264SW_EDGE_TOP 2U
#define H264SW_EDGE_TOP_RIGHT 4U
#define H264SW_EDGE_TOP_LEFT 8U

/* The Intra_4x4 and Intra_8x8 modes (Tables 8-2 and 8-3), the Intra_16x16 modes (Table 8-4), the chroma modes (Table 8-5). */
#define H264SW_I4_VERTICAL 0U
#define H264SW_I4_HORIZONTAL 1U
#define H264SW_I4_DC 2U
#define H264SW_I4_DIAGONAL_DOWN_LEFT 3U
#define H264SW_I4_DIAGONAL_DOWN_RIGHT 4U
#define H264SW_I4_VERTICAL_RIGHT 5U
#define H264SW_I4_HORIZONTAL_DOWN 6U
#define H264SW_I4_VERTICAL_LEFT 7U
#define H264SW_I4_HORIZONTAL_UP 8U
#define H264SW_I16_VERTICAL 0U
#define H264SW_I16_HORIZONTAL 1U
#define H264SW_I16_DC 2U
#define H264SW_I16_PLANE 3U
#define H264SW_ICHROMA_DC 0U
#define H264SW_ICHROMA_HORIZONTAL 1U
#define H264SW_ICHROMA_VERTICAL 2U
#define H264SW_ICHROMA_PLANE 3U

/*
 * The neighbouring samples of a block, as the clauses name them p[ x, y ].
 *
 * top_left is p[ -1, -1 ]; top[0..2N-1] are p[ 0..2N-1, -1 ] (the top
 * row and, for 4x4 and 8x8 blocks, the top-right row; 16 entries are
 * used for a 16x16 or an 8x8 chroma block); left[0..N-1] are p[ -1,
 * 0..N-1 ].  available says which parts hold samples (H264SW_EDGE_*);
 * the entries of an unavailable part are unspecified.  For Intra_4x4
 * (8.3.1.2) and Intra_8x8 (8.3.2.2) the substitution of p[ 4..7, -1 ] or
 * p[ 8..15, -1 ] by p[ 3, -1 ] or p[ 7, -1 ] when the top-right is not
 * available but the top is, is done by the predictor, not by the
 * gatherer.
 */
struct h264sw_intra_edge {
	uint8_t top_left;
	uint8_t top[16];
	uint8_t left[16];
	unsigned available;
};

/*
 * Derives Intra4x4PredMode of a block (8.3.1.1).
 *
 * blk is luma4x4BlkIdx of the current macroblock (ctx->n); prev_flag and
 * rem are prev_intra4x4_pred_mode_flag and rem_intra4x4_pred_mode.  The
 * neighbours A and B are those of h264sw_mb_neighbour_4x4.
 * dcPredModePredictedFlag is 1 when A or B is not available or is an
 * inter macroblock with constrained_intra_pred_flag 1; a neighbour not
 * coded in Intra_4x4 or Intra_8x8 gives mode 2 (the intra4x4_mode entry
 * of struct h264sw_mb holds 2 for such macroblocks, and the Intra_8x8
 * mode of an 8x8 neighbour in each of its 4x4 entries).
 * predIntra4x4PredMode = Min( A, B ); the result is it when prev_flag,
 * else rem when rem is below it, else rem + 1.  Returns 0..8.
 */
unsigned h264sw_intra4x4_mode(const struct h264sw_slice_ctx *ctx, unsigned blk, unsigned prev_flag, unsigned rem);

/*
 * Derives Intra8x8PredMode of a block (8.3.2.1).
 *
 * blk is luma8x8BlkIdx; as h264sw_intra4x4_mode with the neighbours of
 * h264sw_mb_neighbour_8x8, except that an Intra_4x4 neighbour gives
 * Intra4x4PredMode[ luma8x8BlkIdxN * 4 + n ] with n 1 for A and 2 for B
 * (8-72, frames).  Returns 0..8.
 */
unsigned h264sw_intra8x8_mode(const struct h264sw_slice_ctx *ctx, unsigned blk, unsigned prev_flag, unsigned rem);

/*
 * Gathers the neighbouring luma samples of a block of the current macroblock.
 *
 * size is 4, 8 or 16; blk is luma4x4BlkIdx (size 4), luma8x8BlkIdx (size
 * 8) or 0 (size 16).  The samples are read from the current frame
 * (already reconstructed, not yet deblocked) at the locations of
 * 8.3.1.2, 8.3.2.2 or 8.3.3, each part marked available as these clauses
 * say: not available when the location's macroblock is not available
 * (h264sw_mb_locate), or is an inter macroblock and
 * constrained_intra_pred_flag is 1, or (top-right of a 4x4 or 8x8 block)
 * the block holding it is inside the current macroblock and later in
 * decoding order (h264sw_blk4x4_at or the 8x8 index above blk), or is to
 * the right of the macroblock below row -1 (Table 6-3).
 */
void h264sw_intra_edge_luma(const struct h264sw_slice_ctx *ctx, unsigned size, unsigned blk, struct h264sw_intra_edge *edge);

/* Gathers the neighbouring samples of the current macroblock's 8x8 block of chroma component plane (1 Cb, 2 Cr), for 8.3.4, with the same rules. */
void h264sw_intra_edge_chroma(const struct h264sw_slice_ctx *ctx, unsigned plane, struct h264sw_intra_edge *edge);

/*
 * Tells which neighbouring macroblocks of the current macroblock (ctx->n)
 * an intra prediction may read: the H264SW_JOB_* bits (h264sw-context.h)
 * of A, B, C and D that are available and not barred (an inter
 * macroblock while constrained_intra_pred_flag is 1).  C is left out
 * when the frame does not reach past the macroblock above-right.  A
 * missing context gives 0.
 */
unsigned h264sw_intra_neighbours(const struct h264sw_slice_ctx *ctx);

/*
 * Tells which parts of a block's edge are available (H264SW_EDGE_*), from the usable neighbouring macroblocks.
 *
 * neighbours are the bits of h264sw_intra_neighbours; size is 4 (blk a
 * luma4x4BlkIdx), 8 (blk a luma8x8BlkIdx) or 16 (the whole macroblock,
 * luma or chroma, which has no top-right).  The parts inside the current
 * macroblock follow 6.4.12 and Table 6-3: a part in the current
 * macroblock is available when it is decoded before the block (a lower
 * index of the same size), a part right of the macroblock below its top
 * row is not.  Another size gives 0.
 */
unsigned h264sw_intra_parts(unsigned neighbours, unsigned size, unsigned blk);

/* The tables of modes h264sw_intra_usable checks a mode against: Intra_4x4 and Intra_8x8, Intra_16x16, chroma. */
#define H264SW_INTRA_NXN 0U
#define H264SW_INTRA_16X16 1U
#define H264SW_INTRA_CHROMA 2U

/*
 * Tells whether a mode can be predicted from the parts of an edge.
 *
 * table is H264SW_INTRA_*, mode the mode, parts the H264SW_EDGE_* bits
 * available.  Returns 1 when the predictor of the table gives the mode
 * without an error, 0 when it gives EINVAL (a mode the table does not
 * have, or one that needs a part that is not available).
 */
int h264sw_intra_usable(unsigned table, unsigned mode, unsigned parts);

/*
 * Gathers the neighbouring samples of a block from a frame.
 *
 * plane is 0 (luma), 1 (Cb) or 2 (Cr); (x, y) is the block's upper-left
 * sample in the plane and size its width and height; parts are the
 * H264SW_EDGE_* parts to read (h264sw_intra_parts), each of which lies
 * inside the plane.  The top-right part has size samples.  Parts not
 * read are 0 and not marked available.
 */
void h264sw_intra_edge_from(const struct h264sw_frame *frame, unsigned plane, unsigned x, unsigned y, unsigned size, unsigned parts, struct h264sw_intra_edge *edge);

/*
 * Predicts a 4x4 luma block (8.3.1.2.1 to 8.3.1.2.9).
 *
 * mode is Intra4x4PredMode 0..8; pred receives 16 samples in raster
 * order.  Returns 0, or EINVAL when the mode needs a sample that is not
 * available (a broken stream): pred is then filled with 128 and the
 * caller counts the block as concealed.
 */
int h264sw_intra_pred4x4(unsigned mode, const struct h264sw_intra_edge *edge, uint8_t *pred);

/*
 * Predicts an 8x8 luma block (8.3.2.2): first the reference samples are
 * filtered (8.3.2.2.1, which also substitutes the unavailable top-right
 * samples), then the mode's equations (8.3.2.2.2 to 8.3.2.2.10).  pred
 * receives 64 samples.  Errors as h264sw_intra_pred4x4.
 */
int h264sw_intra_pred8x8(unsigned mode, const struct h264sw_intra_edge *edge, uint8_t *pred);

/* Predicts a 16x16 luma block (8.3.3.1 to 8.3.3.4); pred receives 256 samples.  Errors as h264sw_intra_pred4x4. */
int h264sw_intra_pred16x16(unsigned mode, const struct h264sw_intra_edge *edge, uint8_t *pred);

/*
 * Predicts an 8x8 chroma block of 4:2:0 (8.3.4.1 to 8.3.4.4).
 *
 * mode is intra_chroma_pred_mode.  The DC mode works on each 4x4 chroma
 * block with the rules of 8.3.4.1 (which neighbours each block's DC
 * prefers depends on its position: the top-right block the top samples,
 * the bottom-left block the left samples).  pred receives 64 samples.
 * Errors as h264sw_intra_pred4x4.
 */
int h264sw_intra_pred_chroma(unsigned mode, const struct h264sw_intra_edge *edge, uint8_t *pred);

#endif
