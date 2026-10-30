/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libmedia's software H.264 encoder: its limits, its conventions, and
 * the interface libmedia's encoder back end and the host tools use.
 *
 * The encoder writes progressive frames of 8-bit 4:2:0 video as an
 * Annex B byte stream conforming to the Constrained Baseline, Main or
 * High profile.  It uses I, P and (Main and High) non-reference B
 * slices, CAVLC or CABAC, the 4x4 and (High) 8x8 transforms with flat
 * scaling lists, the Intra_4x4, Intra_8x8 (High), Intra_16x16 and chroma
 * intra modes, inter partitions of 16x16, 16x8, 8x16 and 8x8 (never
 * smaller), P_Skip, B_Skip and the spatial direct mode, quarter-sample
 * motion vectors, several reference frames for P slices, the deblocking
 * filter, and I_PCM only when a macroblock would exceed the bit limit.
 * It never uses interlace, slice groups, redundant pictures, weighted
 * prediction, scaling matrices, long-term references or memory
 * management operations.
 *
 * The encoder mirrors the decoder: it keeps the software decoder's
 * picture state (struct h264sw_picture_ctx, struct h264sw_slice_ctx,
 * struct h264sw_mb entries, the DPB, the reference lists) for the
 * pictures it writes, derives every predicted value (motion vector
 * predictors, predicted intra modes, CAVLC nC, CABAC contexts) with the
 * decoder's functions, and makes its reference frames only with the
 * decoder's reconstruction (h264sw_recon_mb) and deblocking
 * (h264sw_deblock_picture).  So a conforming decoder reconstructs exactly
 * the frames the encoder predicts from.
 *
 * Conventions, as h264sw.h (whose error values and arithmetic rules apply
 * unchanged): 0 succeeded; EINVAL a caller's argument out of range (or an
 * internal inconsistency, which is a bug); ENOTSUP a configuration valid
 * in itself but outside what this encoder makes; ENOMEM; ENOBUFS a buffer
 * limit; EAGAIN the caller must take packets before giving more frames.
 * No global state.
 */

#ifndef LIBMEDIA_H264ENC_H
#define LIBMEDIA_H264ENC_H

#include <stddef.h>
#include <stdint.h>

/* The profiles the encoder writes (the numbers of MEDIA_PROFILE_H264_* of media.h). */
#define H264ENC_PROFILE_DEFAULT 0U
#define H264ENC_PROFILE_CONSTRAINED_BASELINE 1U
#define H264ENC_PROFILE_MAIN 2U
#define H264ENC_PROFILE_HIGH 3U

/* Rate controls (the numbers of MEDIA_RATE_*). */
#define H264ENC_RATE_DEFAULT 0U
#define H264ENC_RATE_CQP 1U
#define H264ENC_RATE_CBR 2U
#define H264ENC_RATE_VBR 3U

/* Entropy coding: chosen by the profile (CAVLC for Constrained Baseline, CABAC otherwise), or asked for. */
#define H264ENC_ENTROPY_AUTO 0U
#define H264ENC_ENTROPY_CAVLC 1U
#define H264ENC_ENTROPY_CABAC 2U

/*
 * The speed presets: how much each macroblock's decision searches.  The
 * parameters of each are fixed in h264enc-mode.h; DEFAULT is BALANCED.
 */
#define H264ENC_SPEED_DEFAULT 0U
#define H264ENC_SPEED_FAST 1U
#define H264ENC_SPEED_BALANCED 2U
#define H264ENC_SPEED_QUALITY 3U

/* The input layouts (MEDIA_FORMAT_NV12 and MEDIA_FORMAT_I420 of media.h). */
#define H264ENC_FORMAT_NV12 1U
#define H264ENC_FORMAT_I420 2U

/* Flags of a picture given (make it an IDR picture) and of a packet taken (it holds an IDR picture). */
#define H264ENC_FORCE_KEY 0x1U
#define H264ENC_PACKET_KEY 0x1U

/* The slice types of a packet (H264SW_SLICE_* values). */
#define H264ENC_TYPE_P 0U
#define H264ENC_TYPE_B 1U
#define H264ENC_TYPE_I 2U

/*
 * The limits of a configuration.
 *
 * Reference frames for P slices: 1..H264ENC_MAX_REFS (the DPB also holds
 * the next anchor when B-frames are used).  B-frames in a row: 0..3.
 * Slices per picture: 1..H264ENC_MAX_SLICES (each a run of whole
 * macroblock rows).  The picture: even width and height, 16..16880 luma
 * samples each (H264SW_MAX_SIDE_MBS macroblocks) and at most
 * H264SW_MAX_FRAME_MBS macroblocks.
 */
#define H264ENC_MAX_REFS 4U
#define H264ENC_MAX_B_FRAMES 3U
#define H264ENC_MAX_SLICES 16U
#define H264ENC_MIN_SIDE 16U

/*
 * The most bits of one macroblock_layer() for 8-bit 4:2:0: 128 + RawMbBits
 * with RawMbBits = 256 * 8 + 2 * 8 * 8 * 8 = 3072 (7-7; A.3.1 j), A.3.3).
 * A macroblock that would exceed it is written as I_PCM.
 */
#define H264ENC_MB_MAX_BITS 3200U

/*
 * What an encoder is asked for.
 *
 * width, height: the visible picture in luma samples (even).  rate_num /
 * rate_den: frames per second (both above 0).  profile: H264ENC_PROFILE_*
 * (DEFAULT is High).  level_idc: 0 to choose the lowest level that fits,
 * or a level_idc of Table A-1 (not 9) the configuration must fit
 * (ENOTSUP otherwise).  rate_control and its numbers: CQP uses qp (the P
 * slices' QP, 0..51; I slices qp - 3, B slices qp + 2, clamped); CBR uses
 * bitrate; VBR bitrate on average and max_bitrate at most (0: bitrate *
 * 3 / 2); DEFAULT is CQP with qp (23 when qp is 0) when bitrate is 0,
 * else VBR.  key_interval: frames from one IDR picture to the next (0:
 * two seconds' worth, at least 1).  b_frames: 0..3 (above 0 refused for
 * Constrained Baseline).  refs: P reference frames (0: the preset's).
 * entropy, speed: as above (CABAC refused for Constrained Baseline).
 * slices: 0 or 1 for one slice per picture, up to H264ENC_MAX_SLICES and
 * at most the macroblock rows.  The colour signalled in the VUI (ITU-T
 * H.273 code points): full_range, primaries, transfer, matrix (2 =
 * unspecified for each: then no colour description is sent),
 * chroma_location (0..5; 0 is not sent).  sar_width and sar_height: the
 * sample aspect ratio (0 and 0: not sent).
 */
struct h264enc_config {
	unsigned width;
	unsigned height;
	uint32_t rate_num;
	uint32_t rate_den;
	unsigned profile;
	unsigned level_idc;
	unsigned rate_control;
	uint32_t bitrate;
	uint32_t max_bitrate;
	unsigned qp;
	uint32_t key_interval;
	unsigned b_frames;
	unsigned refs;
	unsigned entropy;
	unsigned speed;
	unsigned slices;
	int full_range;
	unsigned primaries;
	unsigned transfer;
	unsigned matrix;
	unsigned chroma_location;
	unsigned sar_width;
	unsigned sar_height;
};

/*
 * One picture given to the encoder: its planes in format (I420: Y, Cb,
 * Cr; NV12: Y and the interleaved CbCr plane in plane[1], plane[2]
 * unused), each stride bytes per row, of the configured size; its
 * presentation time; flags (H264ENC_FORCE_KEY).  The encoder copies the
 * samples before h264enc_send returns: the caller may reuse them.
 */
struct h264enc_picture {
	const uint8_t *plane[3];
	size_t stride[3];
	unsigned format;
	int64_t time_us;
	unsigned flags;
};

/*
 * One packet taken: one access unit in decoding order, Annex B (every NAL
 * unit after 0x00000001; the SPS and the PPS before each IDR picture).
 * data and size stay the encoder's until the next h264enc_receive or
 * h264enc_destroy.  time_us is the picture's presentation time,
 * decode_time_us its decoding time (the presentation times in increasing
 * order, moved earlier by one frame when B-frames are used, so that
 * decode_time_us never exceeds time_us).  flags H264ENC_PACKET_KEY; type
 * the slice type (H264ENC_TYPE_*) and qp the picture's QP, for logs and
 * tests.
 */
struct h264enc_packet {
	const uint8_t *data;
	size_t size;
	int64_t time_us;
	int64_t decode_time_us;
	unsigned flags;
	unsigned type;
	int qp;
};

struct h264enc;

/*
 * Makes an encoder.
 *
 * Checks config (EINVAL for a value out of its range, ENOTSUP for a
 * combination this encoder does not make or a level_idc the
 * configuration does not fit), resolves the defaults, chooses the level,
 * writes the parameter sets.  Returns 0 and *encoder; EINVAL; ENOTSUP;
 * ENOMEM.
 */
int h264enc_create(const struct h264enc_config *config, struct h264enc **encoder);

/*
 * Gives the next picture in presentation order, or NULL to drain (every
 * picture held is encoded; no picture may follow).
 *
 * Pictures wait while B-frames need a later anchor; encoded access
 * units wait to be taken.  Returns 0; EAGAIN when packets are waiting and
 * the caller must take them first (nothing was taken); EINVAL for a
 * wrong format, a time not after the last one, a picture after the
 * drain; ENOMEM; another errno for an internal failure, after which the
 * encoder is unusable.
 */
int h264enc_send(struct h264enc *encoder, const struct h264enc_picture *picture);

/*
 * Takes the next packet.  Returns 1 and *packet, 0 when none is ready
 * (more pictures are needed, or the drain is complete), or a negative
 * errno after an internal failure.
 */
int h264enc_receive(struct h264enc *encoder, struct h264enc_packet *packet);

/*
 * Gives the SPS and the PPS as NAL units in Annex B (each after
 * 0x00000001), for a container's codec data.  The bytes stay the
 * encoder's until h264enc_destroy.
 */
void h264enc_parameter_sets(const struct h264enc *encoder, const uint8_t **data, size_t *size);

/* The profile_idc, constraint flags (bit n constraint_setn_flag) and level_idc chosen. */
void h264enc_profile_level(const struct h264enc *encoder, unsigned *profile_idc, unsigned *constraint_flags, unsigned *level_idc);

/* Frees an encoder and everything it holds (NULL is ignored). */
void h264enc_destroy(struct h264enc *encoder);

#endif
