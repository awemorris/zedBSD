/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant tables of the lossy format's key frames.
 */

#include "vp8-tables.h"

/* The luma mode tree of key frames (not filled yet). */
const int8_t webpc_vp8_kf_ymode_tree[8] = { 0 };

/* The chroma mode tree (not filled yet). */
const int8_t webpc_vp8_uv_mode_tree[6] = { 0 };

/* The subblock mode tree (not filled yet). */
const int8_t webpc_vp8_bmode_tree[18] = { 0 };

/* The segment id tree (not filled yet). */
const int8_t webpc_vp8_segment_tree[6] = { 0 };

/* The DCT token tree (not filled yet). */
const int8_t webpc_vp8_coeff_tree[22] = { 0 };

/* The fixed luma mode probabilities of key frames (not filled yet). */
const uint8_t webpc_vp8_kf_ymode_prob[4] = { 0 };

/* The fixed chroma mode probabilities of key frames (not filled yet). */
const uint8_t webpc_vp8_kf_uv_mode_prob[3] = { 0 };

/* The subblock mode probabilities of key frames, by the modes above and on the left (not filled yet). */
const uint8_t webpc_vp8_kf_bmode_prob[WEBPC_VP8_BMODES][WEBPC_VP8_BMODES][WEBPC_VP8_BMODES - 1] = { { { 0 } } };

/* The default token probabilities (not filled yet). */
const uint8_t webpc_vp8_coeff_default[WEBPC_VP8_COEFF_TYPES][WEBPC_VP8_COEFF_BANDS][WEBPC_VP8_COEFF_CONTEXTS]
	[WEBPC_VP8_COEFF_NODES] = { { { { 0 } } } };

/* The token probability update probabilities (not filled yet). */
const uint8_t webpc_vp8_coeff_update[WEBPC_VP8_COEFF_TYPES][WEBPC_VP8_COEFF_BANDS][WEBPC_VP8_COEFF_CONTEXTS]
	[WEBPC_VP8_COEFF_NODES] = { { { { 0 } } } };

/* The band of each coefficient position (not filled yet). */
const uint8_t webpc_vp8_coeff_bands[17] = { 0 };

/* The zig-zag scan (not filled yet). */
const uint8_t webpc_vp8_zigzag[16] = { 0 };

/* The extra-bit probabilities of the token categories (not filled yet). */
const uint8_t webpc_vp8_cat_probs[6][12] = { { 0 } };

/* The smallest magnitude of each token category (not filled yet). */
const uint16_t webpc_vp8_cat_base[6] = { 0 };

/* The DC quantizer steps (not filled yet). */
const uint16_t webpc_vp8_dc_q[WEBPC_VP8_QINDEX_RANGE] = { 0 };

/* The AC quantizer steps (not filled yet). */
const uint16_t webpc_vp8_ac_q[WEBPC_VP8_QINDEX_RANGE] = { 0 };
