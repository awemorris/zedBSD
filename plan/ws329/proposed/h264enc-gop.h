/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The picture types and the coding order of the software H.264 encoder:
 * which picture is IDR, P or B, the order they are coded in, and the
 * values of their slice headers that follow from it (frame_num, the
 * order count, nal_ref_idc, idr_pic_id).  The encoder's own choice,
 * within 7.4.3 and 8.2.1:
 *
 * - The first picture, every picture key_interval pictures after the last
 *   IDR picture, and every picture given with H264ENC_FORCE_KEY are IDR
 *   pictures.
 * - With b_frames 0 every other picture is P, coded at once.
 * - With b_frames N: after an anchor (IDR or P), the next N pictures are
 *   B and the one after them P (the next anchor); the anchor is coded
 *   first, then its B pictures in presentation order.  A group cut short
 *   (an IDR picture due among them, or the drain) makes its last waiting
 *   picture P (coded first) and the others B; then the IDR picture.  So a
 *   B picture never refers across an IDR picture.
 * - nal_ref_idc: 3 for IDR, 2 for P, 0 for B (B pictures are not
 *   references).  frame_num: 0 for IDR; for each other picture
 *   ( PrevRefFrameNum + 1 ) % MaxFrameNum, where PrevRefFrameNum is the
 *   last reference picture's (B pictures do not change it, 7.4.3).
 *   pic_order_cnt_lsb: ( 2 * ( index - idr_index ) ) % MaxPicOrderCntLsb
 *   (the order count is twice the presentation distance from the IDR
 *   picture).  idr_pic_id: 0, 1, 0, 1, ... from IDR to IDR (consecutive
 *   IDR pictures differ, 7.4.3).
 * - decode_time_us of the n-th picture coded (n from 0): without B-frames
 *   its own time; with B-frames, for n 0 its time less one frame duration
 *   ( 1 000 000 * rate_den / rate_num, rounded down, in int64_t), and for
 *   n >= 1 the ( n - 1 )-th smallest of the times given so far (counting
 *   from 0).  With max_num_reorder_frames 1 this never exceeds the
 *   picture's own time and increases from picture to picture.
 *
 * The module keeps indices and decisions only; the pictures themselves
 * are the encoder's (h264enc-source.h), named by their slot.
 */

#ifndef LIBMEDIA_H264ENC_GOP_H
#define LIBMEDIA_H264ENC_GOP_H

#include "h264enc.h"

/* The most pictures waiting for coding at once: the B pictures of a group and their anchor. */
#define H264ENC_GOP_WAIT (H264ENC_MAX_B_FRAMES + 1U)

/*
 * One picture to code, in coding order.
 *
 * slot: the source slot the caller gave with the picture; index its
 * presentation number; type H264ENC_TYPE_*; idr; nal_ref_idc; frame_num;
 * poc_lsb; poc (the full order count 2 * ( index - idr_index ));
 * idr_pic_id; time_us; decode_time_us; refs_past the reference pictures
 * coded since the IDR picture that precede it in presentation order and
 * are still in the sliding window (at most max_refs): the encoder's list
 * 0 length.
 */
struct h264enc_gop_entry {
	unsigned slot;
	uint64_t index;
	unsigned type;
	int idr;
	unsigned nal_ref_idc;
	uint32_t frame_num;
	uint32_t poc_lsb;
	int32_t poc;
	unsigned idr_pic_id;
	int64_t time_us;
	int64_t decode_time_us;
	unsigned refs_past;
};

/*
 * The state: the settings (key_interval, b_frames, max_refs the sliding
 * window's references for P, log2_max_frame_num, log2_max_poc_lsb,
 * rate_num, rate_den), the waiting pictures in presentation order, the
 * pictures decided and not yet taken (in coding order), the counters, and
 * the times given not yet assigned to a decoding time.
 */
struct h264enc_gop {
	uint32_t key_interval;
	unsigned b_frames;
	unsigned max_refs;
	unsigned log2_max_frame_num;
	unsigned log2_max_poc_lsb;
	uint32_t rate_num;
	uint32_t rate_den;

	unsigned waiting_count;
	unsigned waiting_slot[H264ENC_GOP_WAIT];
	uint64_t waiting_index[H264ENC_GOP_WAIT];
	int64_t waiting_time[H264ENC_GOP_WAIT];
	unsigned waiting_flags[H264ENC_GOP_WAIT];

	unsigned ready_count;
	unsigned ready_head;
	struct h264enc_gop_entry ready[2U * H264ENC_GOP_WAIT];

	uint64_t next_index;
	uint64_t idr_index;
	int have_idr;
	uint32_t prev_ref_frame_num;
	unsigned next_idr_pic_id;
	unsigned refs_in_window;
	uint64_t coded;
	int64_t times[2U * H264ENC_GOP_WAIT];
	unsigned time_count;
	int draining;
};

/* Prepares the state; 0, or EINVAL for settings out of range (b_frames above H264ENC_MAX_B_FRAMES, key_interval 0 or above H264ENC_MAX_KEY_INTERVAL, a zero rate). */
int h264enc_gop_init(struct h264enc_gop *gop, uint32_t key_interval, unsigned b_frames, unsigned max_refs, unsigned log2_max_frame_num, unsigned log2_max_poc_lsb, uint32_t rate_num, uint32_t rate_den);

/*
 * Adds the next picture in presentation order (in the caller's slot, its
 * time and flags).  Decides every picture that can now be decided (an IDR
 * or P anchor and its B pictures).  Returns 0, EINVAL after the drain,
 * EBUSY when the decided pictures were not taken (the caller takes them
 * first), ENOBUFS when more than H264ENC_GOP_WAIT pictures would wait.
 */
int h264enc_gop_push(struct h264enc_gop *gop, unsigned slot, int64_t time_us, unsigned flags);

/* Starts the drain: decides the waiting pictures as a group cut short. */
void h264enc_gop_drain(struct h264enc_gop *gop);

/* Takes the next decided picture in coding order into *entry: 1, or 0 when none is decided. */
int h264enc_gop_next(struct h264enc_gop *gop, struct h264enc_gop_entry *entry);

/*
 * The counts of the group that starts at an IDR picture (for the rate
 * control): pictures, P and B pictures up to the next scheduled IDR
 * picture by key_interval and b_frames (forced key pictures not known
 * yet are not counted).
 */
void h264enc_gop_group(const struct h264enc_gop *gop, unsigned *n, unsigned *n_p, unsigned *n_b);

#endif
