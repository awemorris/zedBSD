/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The whole of the software H.264 encoder (the structure behind
 * struct h264enc of h264enc.h) and the flow of one picture.
 *
 * Creating (h264enc_create): the configuration resolved (profile: DEFAULT
 * is High; entropy: CAVLC for Constrained Baseline, else CABAC unless
 * asked; speed; refs from the preset, at most H264ENC_MAX_REFS;
 * key_interval; rate control); max_num_ref_frames = refs + ( b_frames >
 * 0 ); the level (h264enc_level_choose with dpb_frames =
 * max_num_ref_frames, or h264enc_level_check); the sequence
 * (h264enc-ps.h; log2_max_frame_num 8, log2_max_poc_lsb 8; pic_init_qp
 * 26; num_ref_idx_default { refs, 1 }; max_num_reorder_frames 1 with
 * B-frames else 0; max_dec_frame_buffering max_num_ref_frames); the SPS
 * and PPS written, kept as NAL units, and read back into ps with the
 * decoder's reader (a failure there is EINVAL: a bug); the frame pool
 * (h264sw_frame_pool_create, limit 2 * max_num_ref_frames + 4, the
 * decoder's rule); the DPB
 * (h264sw_dpb_init with the read-back SPS) and its output queue; the
 * macroblock entries and slice parameters for the picture context; the
 * source slots (b_frames + 2); the reference planes (max_num_ref_frames +
 * 1); the tables (quant, lambda, cavlc); the rate control; the group
 * state; one slice writer and macroblock context per slice.
 *
 * One picture (from h264enc_gop_next):
 *  1. The slice header of each slice: nal_type 5 or 1, nal_ref_idc,
 *     first_mb (slices are runs of whole macroblock rows, as even as the
 *     row count allows), slice_type, frame_num, idr_pic_id, poc_lsb,
 *     direct_spatial 1, num_ref_idx_active (P: Min( refs, refs_past ); B:
 *     { Min( refs, refs_past ), 1 }), no modification, the marking (IDR:
 *     no_output_of_prior_pics 0, long_term_reference 0; others: sliding
 *     window, adaptive 0), cabac_init_idc 0, qp from the rate control,
 *     disable_deblocking 0 with offsets 0.
 *  2. A frame from the pool with the SPS's display fields; the picture
 *     context as the decoder's step 4 sets it (entries reset with slice
 *     -1, the levelscale of flat lists by h264sw_levelscale_init); the
 *     order counts and frame_num on the frame.
 *  3. Each slice: its slice parameters; the lists by h264sw_reflist_build
 *     from the DPB (a missing entry is a bug for this encoder: EINVAL);
 *     h264sw_weights_setup and h264sw_direct_setup as the decoder; the
 *     reference planes of each list entry (built when the frame became a
 *     reference); the slice context; the header written; CABAC started
 *     (bins 0 for the first slice); then for each macroblock: neighbours,
 *     the decision (h264enc_mode_i, _p or _b), a writing mark,
 *     h264enc_mb_code, h264enc_mbwrite; ERANGE from the writer clamps the
 *     levels to H264ENC_CAVLC_LEVEL_LIMIT and codes again; a macroblock
 *     that passed H264ENC_MB_MAX_BITS (h264enc_mbwrite_bits) is rewound
 *     and coded as I_PCM; picture->decoded_mbs counted; finish; the NAL
 *     unit appended (the last slice of a CABAC picture with
 *     h264enc_cabac_zero_words).
 *  4. The picture finished as the decoder's step 6 (no concealment):
 *     h264sw_deblock_picture, the motion copied into the frame, the
 *     order counts, h264sw_dpb_remove and h264sw_dpb_store (output frames
 *     taken and dropped at once: the encoder outputs packets, not
 *     frames), the reference planes built for a reference picture into
 *     the slot of a frame no longer in the DPB.
 *  5. The rate control updated; filler data for CBR; the access unit
 *     (the SPS and PPS first for IDR) queued as a packet with its times.
 *
 * Ownership: the encoder owns every allocation; frames are counted
 * references (the DPB's and the picture's); h264enc_destroy drops the
 * DPB and the queue, destroys the pool and frees the rest.  One thread
 * at a time.
 */

#ifndef LIBMEDIA_H264ENC_ENCODER_H
#define LIBMEDIA_H264ENC_ENCODER_H

#include "h264enc.h"
#include "h264enc-cavlc.h"
#include "h264enc-cost.h"
#include "h264enc-gop.h"
#include "h264enc-mbctx.h"
#include "h264enc-mbwrite.h"
#include "h264enc-ps.h"
#include "h264enc-quant.h"
#include "h264enc-ratecontrol.h"
#include "h264enc-refplanes.h"
#include "h264enc-source.h"
#include "h264sw.h"
#include "h264sw-context.h"
#include "h264sw-dpb.h"
#include "h264sw-frame.h"
#include "h264sw-ps.h"
#include "vcodec-nalwriter.h"

/* The most packets waiting to be taken (a drain of a full group, plus one). */
#define H264ENC_PACKETS_MAX (H264ENC_GOP_WAIT + 2U)

/* The most source pictures held: a full group waiting, and the one being given. */
#define H264ENC_SOURCES (H264ENC_MAX_B_FRAMES + 2U)

/* The most reference planes kept: every reference frame of the DPB and one being built. */
#define H264ENC_PLANES (H264ENC_MAX_REFS + 2U)

/*
 * One packet waiting: its access unit (owned) and what struct
 * h264enc_packet reports.
 */
struct h264enc_queued {
	struct vcodec_au au;
	int64_t time_us;
	int64_t decode_time_us;
	unsigned flags;
	unsigned type;
	int qp;
};

/* One slice's state: its header, the decoder's slice context, its writer and its macroblock context. */
struct h264enc_slice_state {
	struct h264sw_slice_header header;
	struct h264sw_slice_ctx ctx;
	struct h264enc_slice_writer writer;
	struct h264enc_mbctx mb;
};

/*
 * The encoder.
 *
 * config: as resolved.  sequence: its choices; parameter_sets: the SPS
 * and PPS NAL units; ps: the read-back store (the SPS and PPS of id 0);
 * pool, dpb, output: the frames; mbs, slice_params: the picture
 * context's arrays (size_mbs each); picture: the picture context;
 * slices and slice_count; sources and the group state; planes; tables;
 * rc; packets waiting (a ring); state: 0 running, 1 drained, an errno
 * once failed.
 */
struct h264enc {
	struct h264enc_config config;
	struct h264enc_sequence sequence;
	struct vcodec_au parameter_sets;
	struct h264sw_ps_store ps;

	struct h264sw_frame_pool *pool;
	struct h264sw_dpb dpb;
	struct h264sw_output_queue output;
	struct h264sw_mb *mbs;
	struct h264sw_slice_params *slice_params;
	struct h264sw_picture_ctx picture;
	unsigned slice_count;
	struct h264enc_slice_state *slices;

	struct h264enc_source sources[H264ENC_SOURCES];
	int source_busy[H264ENC_SOURCES];
	struct h264enc_gop gop;
	struct h264enc_refplanes planes[H264ENC_PLANES];

	struct h264enc_quant quant;
	struct h264enc_lambda lambda;
	struct h264enc_cavlc cavlc;
	struct h264enc_preset preset;
	struct h264enc_rc rc;

	struct h264enc_queued packets[H264ENC_PACKETS_MAX];
	unsigned packet_head;
	unsigned packet_count;
	int state;
};

#endif
