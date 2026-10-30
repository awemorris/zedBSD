/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The variable-length code tables of CAVLC (9.2 and 9.1.2), shared by the
 * software H.264 decoder (which reads codes with them) and the software
 * H.264 encoder (which writes codes with them).
 *
 * These are the tables h264sw-cavlc.c already holds, moved here without
 * a change of value, order or shape: the decoder's tests and the JVT
 * conformance streams have checked them.  Each definition in
 * h264sw-cavlc-tables.c keeps the "Values referenced:" comment of its
 * block.
 */

#ifndef LIBMEDIA_H264SW_CAVLC_TABLES_H
#define LIBMEDIA_H264SW_CAVLC_TABLES_H

#include "h264sw.h"

/*
 * One code of a variable-length table: the bits of the code, right
 * aligned (the first bit sent is bit length - 1), its length in bits, and
 * the value it stands for.  For coeff_token the value is
 * ( TotalCoeff << 2 ) | TrailingOnes; for total_zeros and run_before it
 * is the syntax element itself.
 */
struct h264sw_vlc_code {
	uint16_t code;
	uint8_t length;
	uint8_t value;
};

/* The columns of Table 9-5 for 0 <= nC < 2, 2 <= nC < 4, 4 <= nC < 8 and 8 <= nC (index 0..3). */
#define H264SW_COEFF_TOKEN_COLUMNS 4U

/* The codes of each of those columns, and of the nC = -1 column. */
#define H264SW_COEFF_TOKEN_CODES 62U
#define H264SW_COEFF_TOKEN_CHROMA_DC_CODES 14U

/* The number of codeNum values of coded_block_pattern for ChromaArrayType 1 (Table 9-4 (a)). */
#define H264SW_CBP_CODES 48U

/*
 * coeff_token, Table 9-5: [column][k] for the four columns above, each
 * sorted by increasing length (the order the decoder's search needs).
 * Every ( TotalCoeff, TrailingOnes ) pair that the column defines appears
 * exactly once.
 */
extern const struct h264sw_vlc_code h264sw_cavlc_coeff_token[H264SW_COEFF_TOKEN_COLUMNS][H264SW_COEFF_TOKEN_CODES];

/* coeff_token, Table 9-5, the nC = -1 column (the chroma DC of 4:2:0), sorted by increasing length. */
extern const struct h264sw_vlc_code h264sw_cavlc_coeff_token_chroma_dc[H264SW_COEFF_TOKEN_CHROMA_DC_CODES];

/*
 * total_zeros for blocks of 15 or 16 coefficients (Tables 9-7 and 9-8):
 * row tzVlcIndex - 1 (tzVlcIndex 1..15), entry total_zeros 0 up to 16 -
 * tzVlcIndex; entries past the row's last are { 0, 0, 0 }.
 */
extern const struct h264sw_vlc_code h264sw_cavlc_total_zeros[15][16];

/*
 * total_zeros for the chroma DC block of 4:2:0 (Table 9-9 (a)): row
 * tzVlcIndex - 1 (1..3), entry total_zeros 0 up to 4 - tzVlcIndex.
 */
extern const struct h264sw_vlc_code h264sw_cavlc_total_zeros_chroma_dc[3][4];

/*
 * run_before (Table 9-10): row Min( zerosLeft, 7 ) - 1, entry run_before 0
 * up to Min( zerosLeft, 14 ); entries past the row's last are { 0, 0, 0 }.
 */
extern const struct h264sw_vlc_code h264sw_cavlc_run_before[7][15];

/*
 * coded_block_pattern of each codeNum (Table 9-4 (a), ChromaArrayType 1
 * or 2): [0] the Intra_4x4 and Intra_8x8 column, [1] the Inter column;
 * CodedBlockPatternLuma in bits 0..3, CodedBlockPatternChroma in bits 4..5.
 */
extern const uint8_t h264sw_cavlc_cbp[2][H264SW_CBP_CODES];

#endif
