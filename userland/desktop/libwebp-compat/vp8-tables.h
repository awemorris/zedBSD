/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant tables of the lossy format (key frames only), and the
 * numbering of its prediction modes and tokens.  The tables are data
 * taken from RFC 6386; each block of vp8-tables.c names its section.
 */

#ifndef LIBWEBP_COMPAT_VP8_TABLES_H
#define LIBWEBP_COMPAT_VP8_TABLES_H

#include "common.h"

/* The 16x16 luma and the chroma prediction modes (RFC 6386 8.1, 11.2: intra_mbmode). */
#define WEBPC_VP8_DC_PRED		0U
#define WEBPC_VP8_V_PRED		1U
#define WEBPC_VP8_H_PRED		2U
#define WEBPC_VP8_TM_PRED		3U
#define WEBPC_VP8_B_PRED		4U

/* The 4x4 luma subblock modes (RFC 6386 11.2: intra_bmode), in the order the tables index them. */
#define WEBPC_VP8_B_DC_PRED		0U
#define WEBPC_VP8_B_TM_PRED		1U
#define WEBPC_VP8_B_VE_PRED		2U
#define WEBPC_VP8_B_HE_PRED		3U
#define WEBPC_VP8_B_LD_PRED		4U
#define WEBPC_VP8_B_RD_PRED		5U
#define WEBPC_VP8_B_VR_PRED		6U
#define WEBPC_VP8_B_VL_PRED		7U
#define WEBPC_VP8_B_HD_PRED		8U
#define WEBPC_VP8_B_HU_PRED		9U
#define WEBPC_VP8_BMODES		10U

/* The DCT tokens (RFC 6386 13.2: dct_token), in coeff_tree's leaf values. */
#define WEBPC_VP8_DCT_0			0U
#define WEBPC_VP8_DCT_1			1U
#define WEBPC_VP8_DCT_2			2U
#define WEBPC_VP8_DCT_3			3U
#define WEBPC_VP8_DCT_4			4U
#define WEBPC_VP8_DCT_CAT1		5U
#define WEBPC_VP8_DCT_CAT2		6U
#define WEBPC_VP8_DCT_CAT3		7U
#define WEBPC_VP8_DCT_CAT4		8U
#define WEBPC_VP8_DCT_CAT5		9U
#define WEBPC_VP8_DCT_CAT6		10U
#define WEBPC_VP8_DCT_EOB		11U
#define WEBPC_VP8_TOKENS		12U

/*
 * The block types, the outer index of the token probabilities (RFC 6386
 * 13.3, the list after coeff_probs; the plane numbers of 13.3's
 * pseudocode differ from this list and are wrong).
 */
#define WEBPC_VP8_TYPE_Y_AFTER_Y2	0U
#define WEBPC_VP8_TYPE_Y2		1U
#define WEBPC_VP8_TYPE_CHROMA		2U
#define WEBPC_VP8_TYPE_Y_WITH_DC	3U

/* The dimensions of the token probabilities: types, bands, contexts, tree nodes (RFC 6386 13.3). */
#define WEBPC_VP8_COEFF_TYPES		4U
#define WEBPC_VP8_COEFF_BANDS		8U
#define WEBPC_VP8_COEFF_CONTEXTS	3U
#define WEBPC_VP8_COEFF_NODES		11U

/* The quantizer indices (RFC 6386 14.1: 0..127). */
#define WEBPC_VP8_QINDEX_RANGE		128U

/*
 * The trees (RFC 6386 8.1's tree_index arrays: a positive entry is the
 * index of the next pair, an entry of 0 or less is the leaf -entry).
 *
 *   kf_ymode_tree (11.2), 8 entries, leaves WEBPC_VP8_*_PRED;
 *   uv_mode_tree (11.2), 6 entries;
 *   bmode_tree (11.2), 18 entries, leaves WEBPC_VP8_B_*;
 *   mb_segment_tree (9.3, 10), 6 entries, leaves 0..3;
 *   coeff_tree (13.2), 22 entries, leaves WEBPC_VP8_DCT_*; read from node
 *     2 it is the tree without the EOB branch (after a DCT_0).
 */
extern const int8_t webpc_vp8_kf_ymode_tree[8];
extern const int8_t webpc_vp8_uv_mode_tree[6];
extern const int8_t webpc_vp8_bmode_tree[18];
extern const int8_t webpc_vp8_segment_tree[6];
extern const int8_t webpc_vp8_coeff_tree[22];

/* The fixed key-frame mode probabilities (RFC 6386 11.2 kf_ymode_prob, 11.4 kf_uv_mode_prob, 11.5 kf_bmode_prob [above][left]). */
extern const uint8_t webpc_vp8_kf_ymode_prob[4];
extern const uint8_t webpc_vp8_kf_uv_mode_prob[3];
extern const uint8_t webpc_vp8_kf_bmode_prob[WEBPC_VP8_BMODES][WEBPC_VP8_BMODES][WEBPC_VP8_BMODES - 1];

/*
 * The token probabilities: the defaults a key frame starts from (13.5
 * default_coeff_probs) and the probabilities of updating each (13.4
 * coeff_update_probs).
 */
extern const uint8_t webpc_vp8_coeff_default[WEBPC_VP8_COEFF_TYPES][WEBPC_VP8_COEFF_BANDS][WEBPC_VP8_COEFF_CONTEXTS]
	[WEBPC_VP8_COEFF_NODES];
extern const uint8_t webpc_vp8_coeff_update[WEBPC_VP8_COEFF_TYPES][WEBPC_VP8_COEFF_BANDS][WEBPC_VP8_COEFF_CONTEXTS]
	[WEBPC_VP8_COEFF_NODES];

/*
 * The band of each coefficient position, with a 17th entry (0) for the
 * position after the last, so a lookup one past is defined (13.3
 * coeff_bands).
 */
extern const uint8_t webpc_vp8_coeff_bands[17];

/* The zig-zag scan: the raster position of the n-th coefficient read (20.16 zigzag; 13 names the order). */
extern const uint8_t webpc_vp8_zigzag[16];

/*
 * The extra bits of the categories DCT_CAT1..DCT_CAT6 (13.2 Pcat1..Pcat6):
 * row k (for CAT(k + 1)) lists the probabilities of its extra bits, most
 * significant first, ended by 0; webpc_vp8_cat_base[k] is the category's
 * smallest magnitude (5, 7, 11, 19, 35, 67).
 */
extern const uint8_t webpc_vp8_cat_probs[6][12];
extern const uint16_t webpc_vp8_cat_base[6];

/* The quantizer step of each index for the DC and the AC coefficients (14.1 dc_qlookup, ac_qlookup). */
extern const uint16_t webpc_vp8_dc_q[WEBPC_VP8_QINDEX_RANGE];
extern const uint16_t webpc_vp8_ac_q[WEBPC_VP8_QINDEX_RANGE];

#endif
