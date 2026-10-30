/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The rate control of the software H.264 encoder: one QP per picture.
 * Not the specification's; the encoder's own, fixed here:
 *
 * CQP: I pictures qp - 3, P qp, B qp + 2, each clamped to 0..51.
 *
 * CBR and VBR: a target bit allocation per picture type with the
 * complexities X_I, X_P, X_B and K_P = 1.0, K_B = 1.4 (see
 * h264enc_rc_qp), mapped to H.264's quantizer step Qstep( QP ) = 0.625 *
 * 2^( QP / 6 ), with a buffer check:
 *
 *  - b = bitrate * rate_den / rate_num, the bits of one frame on average.
 *  - At each IDR picture, R += b * N, with N, N_P and N_B the pictures,
 *    P pictures and B pictures of the group from this IDR picture to the
 *    next (the encoder's group structure gives them; the remaining counts
 *    go down as pictures are coded).
 *  - Targets: T_I = R / ( 1 + N_P X_P / ( X_I K_P ) + N_B X_B / (
 *    X_I K_B ) ); T_P = R / ( N_P + N_B K_P X_B / ( K_B X_P ) ); T_B = R / (
 *    N_B + N_P K_B X_P / ( K_P X_B ) ); each at least b / 8.
 *  - The model: a picture of type t coded at QP takes X_t / Qstep( QP )
 *    bits, so QP = Round( 6 * Log2( X_t / ( T_t * 0.625 ) ) ), clamped to
 *    qp_min..qp_max and to the last QP of the same type +-4 (I: the last
 *    I QP; the first picture of each type: no step limit).
 *  - Before a type has been coded, its QP is qp0 + offset (I -3, P 0, B
 *    +2) with qp0 = Clip3( 10, 45, Round( 26 - 6 * Log2( bpp / 0.1 ) ) )
 *    and bpp = b / ( width * height ) bits per sample of the visible
 *    picture.
 *  - After each picture: X_t = bits * Qstep( QP ) with bits the access
 *    unit without its filler data (the SPS and PPS of an IDR picture
 *    included), R -= bits + the filler's bits.
 *  - The buffer: size B = rate * 1 s with rate the max_bitrate (VBR) or the
 *    bitrate (CBR); fullness F starts at 0.9 B; before a picture, while
 *    F - X_t / Qstep( QP ) < 0.1 B and QP < qp_max, QP + 1; after it, F =
 *    F - bits + rate * rate_den / rate_num.  CBR: when F exceeds B, the
 *    excess bytes are written as filler data in the same access unit and F
 *    becomes B (the filler counts in bits).  VBR: F is capped at B.  F below
 *    0 is an underflow: counted, F set to 0.
 *
 * All arithmetic is in double; the state is one structure (no global
 * state).  Tests drive it with a synthetic picture model.  Because the
 * QP comes from double arithmetic and <math.h>, CBR and VBR streams may
 * differ between machines; the tests compare the encoder with decoders
 * on the same machine, never streams across machines.
 */

#ifndef LIBMEDIA_H264ENC_RATECONTROL_H
#define LIBMEDIA_H264ENC_RATECONTROL_H

#include "h264enc.h"

/* The quantizer bounds of CBR and VBR. */
#define H264ENC_RC_QP_MIN 10
#define H264ENC_RC_QP_MAX 51

/*
 * The state.
 *
 * mode H264ENC_RATE_CQP, _CBR or _VBR (DEFAULT resolved before); qp the
 * CQP value; bitrate, max_bitrate (bits/s), rate_num, rate_den; width and
 * height the visible size.  The rest as the comment above: frame_bits b,
 * remaining R, the counts left in the group, complexity[3] by
 * H264ENC_TYPE_* (0 until that type is coded), last_qp[3] (-1 until
 * coded), buffer_size, fullness, underflows.
 */
struct h264enc_rc {
	unsigned mode;
	int qp;
	uint32_t bitrate;
	uint32_t max_bitrate;
	uint32_t rate_num;
	uint32_t rate_den;
	unsigned width;
	unsigned height;
	double frame_bits;
	double remaining;
	unsigned left_p;
	unsigned left_b;
	double complexity[3];
	int last_qp[3];
	double buffer_size;
	double fullness;
	unsigned underflows;
};

/* Prepares the state.  Returns 0, or EINVAL (a mode out of range, a zero rate, a zero bitrate for CBR or VBR, max_bitrate below bitrate for VBR). */
int h264enc_rc_init(struct h264enc_rc *rc, unsigned mode, int qp, uint32_t bitrate, uint32_t max_bitrate, uint32_t rate_num, uint32_t rate_den, unsigned width, unsigned height);

/* Starts a group at an IDR picture: n pictures in all, n_p P and n_b B pictures in it (the I picture is the rest). */
void h264enc_rc_group(struct h264enc_rc *rc, unsigned n, unsigned n_p, unsigned n_b);

/*
 * Gives the QP of the next picture of type (H264ENC_TYPE_*), in this
 * order: the model's QP (or the first-picture QP), clamped to the last QP
 * of the type +-4, then raised while the buffer check fails, then clamped
 * to H264ENC_RC_QP_MIN..H264ENC_RC_QP_MAX.  CQP: the fixed QP of the type.
 *
 * Procedure referenced: ISO/IEC JTC1/SC29/WG11 MPEG93/457, MPEG-2 Test
 * Model 5, rate control step 1 (the target bit allocation T_I, T_P, T_B
 * from the remaining bits and the complexities, with K_P and K_B).
 */
int h264enc_rc_qp(struct h264enc_rc *rc, unsigned type);

/*
 * Accounts a coded picture of type at qp that took bits.  Returns the
 * bytes of filler data the access unit must carry (CBR only; 0
 * otherwise); the caller adds them (h264enc_filler_write takes at least
 * 6, so a smaller excess is carried to the next picture).
 */
uint32_t h264enc_rc_update(struct h264enc_rc *rc, unsigned type, int qp, uint64_t bits);

#endif
