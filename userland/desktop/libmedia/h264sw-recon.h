/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reconstruction of one macroblock in the software H.264 decoder:
 * from its parsed syntax (ctx->syntax) to its samples in the current
 * frame (before deblocking) and its motion and modes in its struct
 * h264sw_mb entry.  It brings together the intra prediction
 * (h264sw-intra.h), the motion vectors (h264sw-mvpred.h,
 * h264sw-direct.h), the inter prediction (8.4.2: h264sw-interp.h and
 * h264sw-weight.h) and the residual (8.5: h264sw-transform.h).
 *
 * The work is split in two steps.  The preparation runs with the slice
 * context: it derives the motion and the modes into the struct h264sw_mb
 * entry, checks everything the samples depend on, counts the
 * concealment, and records the macroblock's work (struct h264sw_mb_job,
 * h264sw-context.h).  The reconstruction of the samples reads only that
 * record, the entry and the picture, and cannot fail.
 *
 * A picture with a ring of work (struct h264sw_picture_ctx's jobs) goes
 * through three stages: the parsing prepares each macroblock into the
 * ring (h264sw_recon_queue), the samples are reconstructed from the ring
 * in decoding order, and each macroblock row is deblocked
 * (h264sw_deblock_rows) once it and the row below it are in place and
 * the rows above it are deblocked.  At the end of the picture the
 * decoder reconstructs what is left (h264sw_recon_drain), conceals the
 * macroblocks no slice decoded, then deblocks the rest
 * (h264sw_recon_deblock_rest).
 *
 * With threads (struct h264sw_picture_ctx's threads, made by
 * h264sw_recon_threads_start), the thread that parses (the caller) only
 * prepares the work and hands it over in groups; the other threads
 * reconstruct the samples and deblock the rows.  A macroblock's samples
 * wait only for those of the neighbours its intra prediction reads (the
 * H264SW_JOB_* bits of its work), so macroblocks of several rows are
 * reconstructed at once.  The rows are deblocked one at a time, in order,
 * under the same rule as above (rows r and r + 1 in place, row r - 1
 * deblocked).  The caller takes the same work while it waits: when the
 * ring is full, and at the end of the picture until every macroblock in
 * the ring is in place and no row is being deblocked.  The output is the
 * same whatever the number of threads.  Every count the threads share is
 * read and written under one mutex.
 */

#ifndef LIBMEDIA_H264SW_RECON_H
#define LIBMEDIA_H264SW_RECON_H

#include "h264sw.h"

struct h264sw_mb_job;
struct h264sw_picture_ctx;
struct h264sw_recon_threads;
struct h264sw_slice_ctx;
struct media_threads;

/* The most threads a picture's stages use, the caller's included. */
#define H264SW_RECON_THREADS_MAX 4U

/*
 * Reconstructs the current macroblock (ctx->mb_addr, ctx->n, ctx->syntax).
 *
 * The macroblock is at luma location ( 16 * n.x, 16 * n.y ) of
 * ctx->picture->frame.  By kind:
 *
 * I_PCM (8.3.5): the samples are copied; motion intra (ref_idx -1 from
 * h264sw_mb_reset), nonzero 0xffff.
 *
 * Intra (I_4X4, I_8X8, I_16X16): for I_4X4 each block in luma4x4BlkIdx
 * order: its mode (h264sw_intra4x4_mode, stored in intra4x4_mode), its
 * edge, its prediction, then its residual added (8.5.1) before the next
 * block reads it as a neighbour; I_8X8 likewise per 8x8 block (8.5.3);
 * I_16X16 predicts the whole block, then the DC (8.5.10) and the AC
 * blocks (8.5.2).  The chroma (8.3.4, 8.5.4) is predicted with
 * intra_chroma_pred_mode and its DC and AC added.  motion.intra is 1,
 * ref_idx -1, mv 0.
 *
 * Inter: the motion first, partition by partition in mbPartIdx and
 * subMbPartIdx order, each setting its 4x4 blocks' bits in
 * ctx->decoded_4x4 so later partitions see it: P_Skip by
 * h264sw_mvpred_pskip, with ref_idx[0][0..3] 0 and ref_id[0][0..3] the
 * id of RefPicList0[ 0 ]'s frame (list 1 stays unused, -1); B_Skip and B_Direct_16x16 by
 * h264sw_direct_quadrant for quadrants 0..3; a B_Direct_8x8
 * sub-macroblock by h264sw_direct_quadrant; others mvLX = mvpLX + mvdLX
 * (8-172, 8-173, with mvp from h264sw_mvpred) for each list the
 * partition uses; the sum is computed in int and wrapped to -32768..32767
 * by explicit arithmetic before it is stored in int16_t (a conforming
 * stream stays in the range of 8.4.1; a broken one must not reach an
 * implementation-defined conversion).  ref_id is the id of RefPicListX[ refIdxLX ]'s frame.
 * Then the prediction of each partition (or each 4x4 block of a direct
 * quadrant without direct_8x8_inference_flag, whose vectors may
 * differ): for each list used, h264sw_interp_luma and, with the chroma
 * vector of 8.4.1.4 (equal to the luma vector for frames, read in
 * eighth chroma samples), h264sw_interp_chroma for Cb and Cr; then
 * h264sw_weights_of and h264sw_weights_apply write the prediction into
 * the frame.  Then the residual of each coded 4x4 or 8x8 luma block and
 * of the chroma blocks is added.
 *
 * Residual: QP'Y = QPY; QP'C from qPI = Clip3( -QpBdOffsetC, 51, QPY +
 * chroma offset ) by Table 8-15 (8.5.8), with chroma_qp_index_offset for
 * Cb and second_chroma_qp_index_offset for Cr.  The scaling list is
 * chosen by the macroblock's intra or inter mode and the component
 * (8.5.9).  nonzero gets the bit of each luma 4x4 block with a non-zero
 * level, and all four bits of an 8x8 transform block with any non-zero
 * level (struct h264sw_mb's rule, 8.7.2.1).  intra4x4_mode keeps the 2 of
 * h264sw_mb_reset for every kind but I_4X4 and I_8X8.
 *
 * Errors: an intra mode that needs unavailable samples, or a reference
 * marked missing in the lists, still produces samples (128, or the
 * stand-in frame's) and adds 1 to ctx->concealed; it is not returned.
 * Returns 0, or EINVAL when the macroblock cannot be built at all (a
 * direct prediction without a co-located frame).
 */
int h264sw_recon_mb(struct h264sw_slice_ctx *ctx);

/*
 * Prepares the reconstruction of the current macroblock: the first step of h264sw_recon_mb.
 *
 * Everything h264sw_recon_mb does but writing the samples: the motion
 * (and ctx->decoded_4x4), intra4x4_mode and nonzero of the entry, the
 * checks of the frame and of the references, and the concealment count
 * (at most 1 added to ctx->concealed for the macroblock).  job receives
 * the macroblock's work: its levels and the bits of the
 * blocks that have them, its I_PCM samples, its intra modes and the
 * neighbours an intra prediction may read (h264sw_intra_neighbours), and
 * for each 8x8 quadrant of an inter macroblock the frame of each list it
 * uses and the weight factors (h264sw_weights_of).  Returns 0, or the
 * error of h264sw_recon_mb (then job is not complete, and the samples
 * are not to be reconstructed).
 */
int h264sw_recon_prepare(struct h264sw_slice_ctx *ctx, struct h264sw_mb_job *job);

/*
 * Reconstructs the samples of a prepared macroblock: the second step of h264sw_recon_mb.
 *
 * Writes the macroblock's luma and chroma samples into picture->frame
 * at its place, from job, from its struct h264sw_mb entry (the motion
 * vectors of an inter macroblock) and from the picture (the frame, the
 * scaling functions and the chroma offsets); reads the samples of the
 * neighbours an intra macroblock predicts from.  Nothing fails: job was
 * checked when it was prepared.
 */
void h264sw_recon_samples(const struct h264sw_picture_ctx *picture, const struct h264sw_mb_job *job);

/*
 * Empties a picture's stages for a new picture (or a dropped one).
 *
 * No work is waiting, no row has a macroblock in place and none is
 * deblocked.  The filter field is left to the caller.  With threads, the
 * work not yet taken is given up and the work taken is waited for, so no
 * thread touches the picture, its frame or its arrays afterwards; the
 * threads then wait for the picture's work.
 */
void h264sw_recon_reset(struct h264sw_picture_ctx *picture);

/*
 * Puts the current macroblock through the stages of its picture.
 *
 * The macroblock is prepared (h264sw_recon_prepare) into the next
 * entry of the ring of ctx->picture; on an error nothing is added and
 * the error is returned as h264sw_recon_mb returns it.  Then the waiting
 * work is reconstructed, each row whose macroblocks are all in place
 * counted, and the rows that can be deblocked are (when the picture's
 * filter is 1), in order.  The last row is left for
 * h264sw_recon_deblock_rest.
 */
int h264sw_recon_queue(struct h264sw_slice_ctx *ctx);

/*
 * Reconstructs the samples of every macroblock still waiting in the ring of a picture.
 *
 * With threads, the caller hands over the work it holds and takes work
 * itself until every macroblock in the ring is in place and no row is
 * being deblocked; then the threads let the picture go (until the next
 * h264sw_recon_reset), so the caller alone conceals and deblocks the
 * rest.
 */
void h264sw_recon_drain(struct h264sw_picture_ctx *picture);

/*
 * Deblocks the rows of a picture not yet deblocked, in order.
 *
 * Called when every macroblock is in place (after h264sw_recon_drain and
 * the concealment); does nothing for a picture whose filter is 0.
 */
void h264sw_recon_deblock_rest(struct h264sw_picture_ctx *picture);

/*
 * Starts the threads of a decoder's stages.
 *
 * count: the threads in all, the caller's included (0 or 1: none is
 *   started; above H264SW_RECON_THREADS_MAX: that many).  threads: how
 *   the threads are made, with the caller's start and join and the names
 *   "h264-decode-1" to "h264-decode-3"; NULL: the system's threads
 *   (media_os_thread_start).  *made receives the threads, or NULL when
 *   none is started: a thread that cannot be started leaves the ones
 *   started before it to do the work, and none started leaves the
 *   decoder with its own thread alone.  Returns 0, or ENOMEM.
 */
int h264sw_recon_threads_start(unsigned count, const struct media_threads *threads, struct h264sw_recon_threads **made);

/*
 * Stops the threads of a decoder's stages and frees them.
 *
 * No picture may be in their hands (h264sw_recon_reset or
 * h264sw_recon_drain came last).  NULL is ignored.
 */
void h264sw_recon_threads_stop(struct h264sw_recon_threads *threads);

/* Tells how many threads do a picture's work, the caller's included: 1 for NULL. */
unsigned h264sw_recon_threads_count(const struct h264sw_recon_threads *threads);

/*
 * Sizes the threads' records for a ring of job_capacity entries and a picture of size_mbs macroblocks.
 *
 * Called between pictures.  Returns 0 (also for NULL), or ENOMEM (the
 * records keep their old size).
 */
int h264sw_recon_threads_size(struct h264sw_recon_threads *threads, unsigned job_capacity, unsigned size_mbs);

#endif
