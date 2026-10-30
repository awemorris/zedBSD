/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libmedia's software H.265 encoder: its limits, its conventions, and
 * the interface libmedia's encoder back end and the host tools use.
 *
 * The encoder writes progressive frames of 8-bit 4:2:0 video as an
 * Annex B byte stream conforming to the Main profile.  It uses I, P and
 * non-reference B slices, CTBs of 32 or 64 (by the speed preset), the
 * coding quadtree down to 8x8, the 2Nx2N, 2NxN and Nx2N inter partitions
 * and the 2Nx2N and NxN (8x8) intra partitions, the 35 intra modes, the
 * transform tree with transforms of 4 to 32 and the 4x4 DST, merge and
 * skip, AMVP with quarter-sample vectors, temporal motion vector
 * prediction, several reference pictures for P slices, the deblocking
 * filter and SAO, and wavefront parallel processing when asked for.  It
 * never uses AMP, transform skip, scaling lists, PCM, weighted
 * prediction, long-term references, tiles, dependent slice segments or
 * the transquant bypass.
 *
 * The encoder mirrors the decoder (as the H.264 encoder does): it keeps
 * the software decoder's picture state (struct h265sw_pic, the DPB, the
 * reference lists), derives every predicted value (merge candidates,
 * motion vector predictors, most probable intra modes, CABAC contexts)
 * with the decoder's functions, and makes its reference pictures only
 * with the decoder's reconstruction (h265sw_recon_ctu), deblocking and
 * SAO.  So a conforming decoder reconstructs exactly the pictures the
 * encoder predicts from.
 *
 * The module headers of the encoder (h265enc-*) are designed after the
 * decoder's headers are final; this header fixes the interface the
 * libmedia back end sees, which mirrors the H.264 encoder's (the same
 * structures, returns and rules, field for field, except the codec's
 * own: wavefront for slices, no entropy choice).
 *
 * Conventions, as vcodec.h: 0 succeeded; EINVAL a caller's argument out
 * of range (or an internal inconsistency, which is a bug); ENOTSUP a
 * configuration valid in itself but outside what this encoder makes;
 * ENOMEM; ENOBUFS a buffer limit; EAGAIN the caller must take packets
 * before giving more frames.  No global state.
 */

#ifndef LIBMEDIA_H265ENC_H
#define LIBMEDIA_H265ENC_H

#include <stddef.h>
#include <stdint.h>

/* The profile written (MEDIA_PROFILE_H265_MAIN of media.h; DEFAULT is Main). */
#define H265ENC_PROFILE_DEFAULT 0U
#define H265ENC_PROFILE_MAIN 1U

/* Rate controls (the numbers of MEDIA_RATE_*). */
#define H265ENC_RATE_DEFAULT 0U
#define H265ENC_RATE_CQP 1U
#define H265ENC_RATE_CBR 2U
#define H265ENC_RATE_VBR 3U

/* The speed presets (as the H.264 encoder's; their parameters are fixed in the encoder's mode module; DEFAULT is BALANCED). */
#define H265ENC_SPEED_DEFAULT 0U
#define H265ENC_SPEED_FAST 1U
#define H265ENC_SPEED_BALANCED 2U
#define H265ENC_SPEED_QUALITY 3U

/* The input layouts (MEDIA_FORMAT_NV12 and MEDIA_FORMAT_I420 of media.h). */
#define H265ENC_FORMAT_NV12 1U
#define H265ENC_FORMAT_I420 2U

/* Flags of a picture given (make it an IDR picture) and of a packet taken (it holds an IRAP picture). */
#define H265ENC_FORCE_KEY 0x1U
#define H265ENC_PACKET_KEY 0x1U

/*
 * The limits of a configuration: reference pictures for P slices
 * 1..H265ENC_MAX_REFS; B-frames in a row 0..H265ENC_MAX_B_FRAMES; the
 * picture even in each side, at least H265ENC_MIN_SIDE and at most
 * H265SW_MAX_SIDE, at most H265SW_MAX_LUMA_PS samples (coded as a
 * multiple of 8 with the conformance window); the key frame interval at
 * most H265ENC_MAX_KEY_INTERVAL.
 */
#define H265ENC_MAX_REFS 4U
#define H265ENC_MAX_B_FRAMES 3U
#define H265ENC_MIN_SIDE 16U
#define H265ENC_MAX_KEY_INTERVAL 1073741824U

/*
 * What an encoder is asked for: the same fields as the H.264 encoder's
 * configuration, so the libmedia back ends map media_encoder_config the
 * same way.  width, height, rate_num / rate_den; profile; level_idc (0:
 * the lowest level of Table A.8 that fits, else general_level_idc, 30
 * times the level, which the configuration must fit: ENOTSUP);
 * rate_control with bitrate, max_bitrate, qp (CQP: the P slices' QP,
 * I slices qp - 3, B slices qp + 2, clamped to 0..51; DEFAULT as the
 * H.264 encoder's); key_interval; b_frames; refs (0: the preset's);
 * speed; wavefront (1: entropy_coding_sync_enabled_flag, which lets the
 * encoder spread CTU rows over threads, at a small cost in bits); the
 * VUI colour (full_range, primaries, transfer, matrix: 2 = unspecified
 * and not sent; chroma_location) and sar_width and sar_height.
 */
struct h265enc_config {
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
	unsigned speed;
	int wavefront;
	int full_range;
	unsigned primaries;
	unsigned transfer;
	unsigned matrix;
	unsigned chroma_location;
	unsigned sar_width;
	unsigned sar_height;
};

/* The slice types of a packet (H265SW_SLICE_* values, Table 7-7). */
#define H265ENC_TYPE_B 0U
#define H265ENC_TYPE_P 1U
#define H265ENC_TYPE_I 2U

/*
 * One picture given to the encoder, as struct h264enc_picture: planes in
 * format (I420: Y, Cb, Cr; NV12: Y and the interleaved CbCr in plane[1],
 * plane[2] unused), stride bytes per row, of the configured size;
 * time_us; flags (H265ENC_FORCE_KEY).  The encoder copies the samples
 * before h265enc_send returns: the caller may reuse them.
 */
struct h265enc_picture {
	const uint8_t *plane[3];
	size_t stride[3];
	unsigned format;
	int64_t time_us;
	unsigned flags;
};

/*
 * One packet taken, as struct h264enc_packet: one access unit in decoding
 * order, Annex B (every NAL unit after 0x00000001; the VPS, SPS and PPS
 * before each IRAP picture).  data and size stay the encoder's until the
 * next h265enc_receive or h265enc_destroy.  time_us is the picture's
 * presentation time, decode_time_us its decoding time by the H.264
 * encoder's rule (without B-frames the presentation time; with B-frames,
 * for the first packet its presentation time less one frame duration,
 * for the n-th packet (n >= 1) the ( n - 1 )-th smallest of the
 * presentation times given), so decoding times increase and never exceed
 * presentation times.  flags H265ENC_PACKET_KEY; type the slice type
 * (H265ENC_TYPE_*) and qp the picture's SliceQpY, for logs and tests.
 */
struct h265enc_packet {
	const uint8_t *data;
	size_t size;
	int64_t time_us;
	int64_t decode_time_us;
	unsigned flags;
	unsigned type;
	int qp;
};

struct h265enc;

/*
 * Makes an encoder.  Checks config (EINVAL for a value out of its range,
 * ENOTSUP for a combination this encoder does not make or a level_idc
 * the configuration does not fit), resolves the defaults, chooses the
 * level, writes the parameter sets.  Returns 0 and *encoder; EINVAL;
 * ENOTSUP; ENOMEM.
 */
int h265enc_create(const struct h265enc_config *config, struct h265enc **encoder);

/*
 * Gives the next picture in presentation order, or NULL to drain (every
 * picture held is encoded; no picture may follow).  Returns 0; EAGAIN
 * when packets are waiting and the caller must take them first (nothing
 * was taken); EINVAL for a wrong format, a time not after the last one,
 * a picture after the drain; ENOMEM; another errno for an internal
 * failure, after which the encoder is unusable.
 */
int h265enc_send(struct h265enc *encoder, const struct h265enc_picture *picture);

/*
 * Takes the next packet.  Returns 1 and *packet, 0 when none is ready
 * (more pictures are needed, or the drain is complete), or a negative
 * errno after an internal failure.
 */
int h265enc_receive(struct h265enc *encoder, struct h265enc_packet *packet);

/*
 * Gives the VPS, SPS and PPS as NAL units in Annex B (each after
 * 0x00000001), for a container's codec data (hvcC is assembled by
 * libmedia's packet format code).  The bytes stay the encoder's until
 * h265enc_destroy.
 */
void h265enc_parameter_sets(const struct h265enc *encoder, const uint8_t **data, size_t *size);

/* The general_profile_idc, general_tier_flag and general_level_idc chosen. */
void h265enc_profile_level(const struct h265enc *encoder, unsigned *profile_idc, unsigned *tier, unsigned *level_idc);

/* Frees an encoder and everything it holds (NULL is ignored). */
void h265enc_destroy(struct h265enc *encoder);

#endif
