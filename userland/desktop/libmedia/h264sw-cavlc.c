/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CAVLC entropy decoding of the software H.264 decoder (9.2, 9.1.2).
 */

#include "h264sw-cavlc.h"
#include "h264sw-context.h"

#include <errno.h>
#include <stddef.h>

/* The bits a code lookup looks at: the longest code of Tables 9-5, 9-7 to 9-10 is 16 bits. */
#define CAVLC_WINDOW_BITS 16U

/* The codes of each column of Table 9-5: 62 combinations of TotalCoeff and TrailingOnes, and 14 for the chroma DC of 4:2:0. */
#define CAVLC_TOKEN_COUNT 62U
#define CAVLC_TOKEN_CHROMA_DC_COUNT 14U

/* The most coefficients of one block (maxNumCoeff 16). */
#define CAVLC_COEFF_MAX 16U

/* The largest level_prefix accepted, which keeps levelSuffixSize (level_prefix - 3) at 22 bits. */
#define CAVLC_LEVEL_PREFIX_MAX 25U

/* The largest and smallest level a block may hold (the range of cij at 8 bits). */
#define CAVLC_LEVEL_MAX 32767
#define CAVLC_LEVEL_MIN (-32768)

/* The number of codeNum values of coded_block_pattern for ChromaArrayType 1 (Table 9-4 (a)). */
#define CAVLC_CBP_COUNT 48U

/*
 * One code of a variable-length table: the bits of the code (right
 * aligned), its length in bits, and the value it stands for.  For
 * coeff_token the value is ( TotalCoeff << 2 ) | TrailingOnes; for
 * total_zeros and run_before it is the syntax element itself.  Tables are
 * constant data, shared by every decoder.
 */
struct cavlc_code {
	uint16_t code;
	uint8_t length;
	uint8_t value;
};

/*
 * coeff_token, column 0 <= nC < 2, by increasing length: the code, its length in bits, and
 * ( TotalCoeff << 2 ) | TrailingOnes.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-5, the 0 <= nC < 2 column.
 */
static const struct cavlc_code coeff_token_0_2[62] = {
	{ 1U, 1U, 0U }, /* TrailingOnes 0, TotalCoeff 0: 1 */
	{ 1U, 2U, 5U }, /* TrailingOnes 1, TotalCoeff 1: 01 */
	{ 1U, 3U, 10U }, /* TrailingOnes 2, TotalCoeff 2: 001 */
	{ 3U, 5U, 15U }, /* TrailingOnes 3, TotalCoeff 3: 0001 1 */
	{ 5U, 6U, 4U }, /* TrailingOnes 0, TotalCoeff 1: 0001 01 */
	{ 4U, 6U, 9U }, /* TrailingOnes 1, TotalCoeff 2: 0001 00 */
	{ 3U, 6U, 19U }, /* TrailingOnes 3, TotalCoeff 4: 0000 11 */
	{ 5U, 7U, 14U }, /* TrailingOnes 2, TotalCoeff 3: 0000 101 */
	{ 4U, 7U, 23U }, /* TrailingOnes 3, TotalCoeff 5: 0000 100 */
	{ 7U, 8U, 8U }, /* TrailingOnes 0, TotalCoeff 2: 0000 0111 */
	{ 6U, 8U, 13U }, /* TrailingOnes 1, TotalCoeff 3: 0000 0110 */
	{ 5U, 8U, 18U }, /* TrailingOnes 2, TotalCoeff 4: 0000 0101 */
	{ 4U, 8U, 27U }, /* TrailingOnes 3, TotalCoeff 6: 0000 0100 */
	{ 7U, 9U, 12U }, /* TrailingOnes 0, TotalCoeff 3: 0000 0011 1 */
	{ 6U, 9U, 17U }, /* TrailingOnes 1, TotalCoeff 4: 0000 0011 0 */
	{ 5U, 9U, 22U }, /* TrailingOnes 2, TotalCoeff 5: 0000 0010 1 */
	{ 4U, 9U, 31U }, /* TrailingOnes 3, TotalCoeff 7: 0000 0010 0 */
	{ 7U, 10U, 16U }, /* TrailingOnes 0, TotalCoeff 4: 0000 0001 11 */
	{ 6U, 10U, 21U }, /* TrailingOnes 1, TotalCoeff 5: 0000 0001 10 */
	{ 5U, 10U, 26U }, /* TrailingOnes 2, TotalCoeff 6: 0000 0001 01 */
	{ 4U, 10U, 35U }, /* TrailingOnes 3, TotalCoeff 8: 0000 0001 00 */
	{ 7U, 11U, 20U }, /* TrailingOnes 0, TotalCoeff 5: 0000 0000 111 */
	{ 6U, 11U, 25U }, /* TrailingOnes 1, TotalCoeff 6: 0000 0000 110 */
	{ 5U, 11U, 30U }, /* TrailingOnes 2, TotalCoeff 7: 0000 0000 101 */
	{ 4U, 11U, 39U }, /* TrailingOnes 3, TotalCoeff 9: 0000 0000 100 */
	{ 15U, 13U, 24U }, /* TrailingOnes 0, TotalCoeff 6: 0000 0000 0111 1 */
	{ 11U, 13U, 28U }, /* TrailingOnes 0, TotalCoeff 7: 0000 0000 0101 1 */
	{ 14U, 13U, 29U }, /* TrailingOnes 1, TotalCoeff 7: 0000 0000 0111 0 */
	{ 8U, 13U, 32U }, /* TrailingOnes 0, TotalCoeff 8: 0000 0000 0100 0 */
	{ 10U, 13U, 33U }, /* TrailingOnes 1, TotalCoeff 8: 0000 0000 0101 0 */
	{ 13U, 13U, 34U }, /* TrailingOnes 2, TotalCoeff 8: 0000 0000 0110 1 */
	{ 9U, 13U, 38U }, /* TrailingOnes 2, TotalCoeff 9: 0000 0000 0100 1 */
	{ 12U, 13U, 43U }, /* TrailingOnes 3, TotalCoeff 10: 0000 0000 0110 0 */
	{ 15U, 14U, 36U }, /* TrailingOnes 0, TotalCoeff 9: 0000 0000 0011 11 */
	{ 14U, 14U, 37U }, /* TrailingOnes 1, TotalCoeff 9: 0000 0000 0011 10 */
	{ 11U, 14U, 40U }, /* TrailingOnes 0, TotalCoeff 10: 0000 0000 0010 11 */
	{ 10U, 14U, 41U }, /* TrailingOnes 1, TotalCoeff 10: 0000 0000 0010 10 */
	{ 13U, 14U, 42U }, /* TrailingOnes 2, TotalCoeff 10: 0000 0000 0011 01 */
	{ 9U, 14U, 46U }, /* TrailingOnes 2, TotalCoeff 11: 0000 0000 0010 01 */
	{ 12U, 14U, 47U }, /* TrailingOnes 3, TotalCoeff 11: 0000 0000 0011 00 */
	{ 8U, 14U, 51U }, /* TrailingOnes 3, TotalCoeff 12: 0000 0000 0010 00 */
	{ 15U, 15U, 44U }, /* TrailingOnes 0, TotalCoeff 11: 0000 0000 0001 111 */
	{ 14U, 15U, 45U }, /* TrailingOnes 1, TotalCoeff 11: 0000 0000 0001 110 */
	{ 11U, 15U, 48U }, /* TrailingOnes 0, TotalCoeff 12: 0000 0000 0001 011 */
	{ 10U, 15U, 49U }, /* TrailingOnes 1, TotalCoeff 12: 0000 0000 0001 010 */
	{ 13U, 15U, 50U }, /* TrailingOnes 2, TotalCoeff 12: 0000 0000 0001 101 */
	{ 1U, 15U, 53U }, /* TrailingOnes 1, TotalCoeff 13: 0000 0000 0000 001 */
	{ 9U, 15U, 54U }, /* TrailingOnes 2, TotalCoeff 13: 0000 0000 0001 001 */
	{ 12U, 15U, 55U }, /* TrailingOnes 3, TotalCoeff 13: 0000 0000 0001 100 */
	{ 8U, 15U, 59U }, /* TrailingOnes 3, TotalCoeff 14: 0000 0000 0001 000 */
	{ 15U, 16U, 52U }, /* TrailingOnes 0, TotalCoeff 13: 0000 0000 0000 1111 */
	{ 11U, 16U, 56U }, /* TrailingOnes 0, TotalCoeff 14: 0000 0000 0000 1011 */
	{ 14U, 16U, 57U }, /* TrailingOnes 1, TotalCoeff 14: 0000 0000 0000 1110 */
	{ 13U, 16U, 58U }, /* TrailingOnes 2, TotalCoeff 14: 0000 0000 0000 1101 */
	{ 7U, 16U, 60U }, /* TrailingOnes 0, TotalCoeff 15: 0000 0000 0000 0111 */
	{ 10U, 16U, 61U }, /* TrailingOnes 1, TotalCoeff 15: 0000 0000 0000 1010 */
	{ 9U, 16U, 62U }, /* TrailingOnes 2, TotalCoeff 15: 0000 0000 0000 1001 */
	{ 12U, 16U, 63U }, /* TrailingOnes 3, TotalCoeff 15: 0000 0000 0000 1100 */
	{ 4U, 16U, 64U }, /* TrailingOnes 0, TotalCoeff 16: 0000 0000 0000 0100 */
	{ 6U, 16U, 65U }, /* TrailingOnes 1, TotalCoeff 16: 0000 0000 0000 0110 */
	{ 5U, 16U, 66U }, /* TrailingOnes 2, TotalCoeff 16: 0000 0000 0000 0101 */
	{ 8U, 16U, 67U }, /* TrailingOnes 3, TotalCoeff 16: 0000 0000 0000 1000 */
};

/*
 * coeff_token, column 2 <= nC < 4, by increasing length: the code, its length in bits, and
 * ( TotalCoeff << 2 ) | TrailingOnes.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-5, the 2 <= nC < 4 column.
 */
static const struct cavlc_code coeff_token_2_4[62] = {
	{ 3U, 2U, 0U }, /* TrailingOnes 0, TotalCoeff 0: 11 */
	{ 2U, 2U, 5U }, /* TrailingOnes 1, TotalCoeff 1: 10 */
	{ 3U, 3U, 10U }, /* TrailingOnes 2, TotalCoeff 2: 011 */
	{ 5U, 4U, 15U }, /* TrailingOnes 3, TotalCoeff 3: 0101 */
	{ 4U, 4U, 19U }, /* TrailingOnes 3, TotalCoeff 4: 0100 */
	{ 7U, 5U, 9U }, /* TrailingOnes 1, TotalCoeff 2: 0011 1 */
	{ 6U, 5U, 23U }, /* TrailingOnes 3, TotalCoeff 5: 0011 0 */
	{ 11U, 6U, 4U }, /* TrailingOnes 0, TotalCoeff 1: 0010 11 */
	{ 7U, 6U, 8U }, /* TrailingOnes 0, TotalCoeff 2: 0001 11 */
	{ 10U, 6U, 13U }, /* TrailingOnes 1, TotalCoeff 3: 0010 10 */
	{ 9U, 6U, 14U }, /* TrailingOnes 2, TotalCoeff 3: 0010 01 */
	{ 6U, 6U, 17U }, /* TrailingOnes 1, TotalCoeff 4: 0001 10 */
	{ 5U, 6U, 18U }, /* TrailingOnes 2, TotalCoeff 4: 0001 01 */
	{ 8U, 6U, 27U }, /* TrailingOnes 3, TotalCoeff 6: 0010 00 */
	{ 4U, 6U, 31U }, /* TrailingOnes 3, TotalCoeff 7: 0001 00 */
	{ 7U, 7U, 12U }, /* TrailingOnes 0, TotalCoeff 3: 0000 111 */
	{ 6U, 7U, 21U }, /* TrailingOnes 1, TotalCoeff 5: 0000 110 */
	{ 5U, 7U, 22U }, /* TrailingOnes 2, TotalCoeff 5: 0000 101 */
	{ 4U, 7U, 35U }, /* TrailingOnes 3, TotalCoeff 8: 0000 100 */
	{ 7U, 8U, 16U }, /* TrailingOnes 0, TotalCoeff 4: 0000 0111 */
	{ 4U, 8U, 20U }, /* TrailingOnes 0, TotalCoeff 5: 0000 0100 */
	{ 6U, 8U, 25U }, /* TrailingOnes 1, TotalCoeff 6: 0000 0110 */
	{ 5U, 8U, 26U }, /* TrailingOnes 2, TotalCoeff 6: 0000 0101 */
	{ 7U, 9U, 24U }, /* TrailingOnes 0, TotalCoeff 6: 0000 0011 1 */
	{ 6U, 9U, 29U }, /* TrailingOnes 1, TotalCoeff 7: 0000 0011 0 */
	{ 5U, 9U, 30U }, /* TrailingOnes 2, TotalCoeff 7: 0000 0010 1 */
	{ 4U, 9U, 39U }, /* TrailingOnes 3, TotalCoeff 9: 0000 0010 0 */
	{ 15U, 11U, 28U }, /* TrailingOnes 0, TotalCoeff 7: 0000 0001 111 */
	{ 11U, 11U, 32U }, /* TrailingOnes 0, TotalCoeff 8: 0000 0001 011 */
	{ 14U, 11U, 33U }, /* TrailingOnes 1, TotalCoeff 8: 0000 0001 110 */
	{ 13U, 11U, 34U }, /* TrailingOnes 2, TotalCoeff 8: 0000 0001 101 */
	{ 10U, 11U, 37U }, /* TrailingOnes 1, TotalCoeff 9: 0000 0001 010 */
	{ 9U, 11U, 38U }, /* TrailingOnes 2, TotalCoeff 9: 0000 0001 001 */
	{ 12U, 11U, 43U }, /* TrailingOnes 3, TotalCoeff 10: 0000 0001 100 */
	{ 8U, 11U, 47U }, /* TrailingOnes 3, TotalCoeff 11: 0000 0001 000 */
	{ 15U, 12U, 36U }, /* TrailingOnes 0, TotalCoeff 9: 0000 0000 1111 */
	{ 11U, 12U, 40U }, /* TrailingOnes 0, TotalCoeff 10: 0000 0000 1011 */
	{ 14U, 12U, 41U }, /* TrailingOnes 1, TotalCoeff 10: 0000 0000 1110 */
	{ 13U, 12U, 42U }, /* TrailingOnes 2, TotalCoeff 10: 0000 0000 1101 */
	{ 8U, 12U, 44U }, /* TrailingOnes 0, TotalCoeff 11: 0000 0000 1000 */
	{ 10U, 12U, 45U }, /* TrailingOnes 1, TotalCoeff 11: 0000 0000 1010 */
	{ 9U, 12U, 46U }, /* TrailingOnes 2, TotalCoeff 11: 0000 0000 1001 */
	{ 12U, 12U, 51U }, /* TrailingOnes 3, TotalCoeff 12: 0000 0000 1100 */
	{ 15U, 13U, 48U }, /* TrailingOnes 0, TotalCoeff 12: 0000 0000 0111 1 */
	{ 14U, 13U, 49U }, /* TrailingOnes 1, TotalCoeff 12: 0000 0000 0111 0 */
	{ 13U, 13U, 50U }, /* TrailingOnes 2, TotalCoeff 12: 0000 0000 0110 1 */
	{ 11U, 13U, 52U }, /* TrailingOnes 0, TotalCoeff 13: 0000 0000 0101 1 */
	{ 10U, 13U, 53U }, /* TrailingOnes 1, TotalCoeff 13: 0000 0000 0101 0 */
	{ 9U, 13U, 54U }, /* TrailingOnes 2, TotalCoeff 13: 0000 0000 0100 1 */
	{ 12U, 13U, 55U }, /* TrailingOnes 3, TotalCoeff 13: 0000 0000 0110 0 */
	{ 7U, 13U, 56U }, /* TrailingOnes 0, TotalCoeff 14: 0000 0000 0011 1 */
	{ 6U, 13U, 58U }, /* TrailingOnes 2, TotalCoeff 14: 0000 0000 0011 0 */
	{ 8U, 13U, 59U }, /* TrailingOnes 3, TotalCoeff 14: 0000 0000 0100 0 */
	{ 1U, 13U, 63U }, /* TrailingOnes 3, TotalCoeff 15: 0000 0000 0000 1 */
	{ 11U, 14U, 57U }, /* TrailingOnes 1, TotalCoeff 14: 0000 0000 0010 11 */
	{ 9U, 14U, 60U }, /* TrailingOnes 0, TotalCoeff 15: 0000 0000 0010 01 */
	{ 8U, 14U, 61U }, /* TrailingOnes 1, TotalCoeff 15: 0000 0000 0010 00 */
	{ 10U, 14U, 62U }, /* TrailingOnes 2, TotalCoeff 15: 0000 0000 0010 10 */
	{ 7U, 14U, 64U }, /* TrailingOnes 0, TotalCoeff 16: 0000 0000 0001 11 */
	{ 6U, 14U, 65U }, /* TrailingOnes 1, TotalCoeff 16: 0000 0000 0001 10 */
	{ 5U, 14U, 66U }, /* TrailingOnes 2, TotalCoeff 16: 0000 0000 0001 01 */
	{ 4U, 14U, 67U }, /* TrailingOnes 3, TotalCoeff 16: 0000 0000 0001 00 */
};

/*
 * coeff_token, column 4 <= nC < 8, by increasing length: the code, its length in bits, and
 * ( TotalCoeff << 2 ) | TrailingOnes.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-5, the 4 <= nC < 8 column.
 */
static const struct cavlc_code coeff_token_4_8[62] = {
	{ 15U, 4U, 0U }, /* TrailingOnes 0, TotalCoeff 0: 1111 */
	{ 14U, 4U, 5U }, /* TrailingOnes 1, TotalCoeff 1: 1110 */
	{ 13U, 4U, 10U }, /* TrailingOnes 2, TotalCoeff 2: 1101 */
	{ 12U, 4U, 15U }, /* TrailingOnes 3, TotalCoeff 3: 1100 */
	{ 11U, 4U, 19U }, /* TrailingOnes 3, TotalCoeff 4: 1011 */
	{ 10U, 4U, 23U }, /* TrailingOnes 3, TotalCoeff 5: 1010 */
	{ 9U, 4U, 27U }, /* TrailingOnes 3, TotalCoeff 6: 1001 */
	{ 8U, 4U, 31U }, /* TrailingOnes 3, TotalCoeff 7: 1000 */
	{ 15U, 5U, 9U }, /* TrailingOnes 1, TotalCoeff 2: 0111 1 */
	{ 12U, 5U, 13U }, /* TrailingOnes 1, TotalCoeff 3: 0110 0 */
	{ 14U, 5U, 14U }, /* TrailingOnes 2, TotalCoeff 3: 0111 0 */
	{ 10U, 5U, 17U }, /* TrailingOnes 1, TotalCoeff 4: 0101 0 */
	{ 11U, 5U, 18U }, /* TrailingOnes 2, TotalCoeff 4: 0101 1 */
	{ 8U, 5U, 21U }, /* TrailingOnes 1, TotalCoeff 5: 0100 0 */
	{ 9U, 5U, 22U }, /* TrailingOnes 2, TotalCoeff 5: 0100 1 */
	{ 13U, 5U, 35U }, /* TrailingOnes 3, TotalCoeff 8: 0110 1 */
	{ 15U, 6U, 4U }, /* TrailingOnes 0, TotalCoeff 1: 0011 11 */
	{ 11U, 6U, 8U }, /* TrailingOnes 0, TotalCoeff 2: 0010 11 */
	{ 8U, 6U, 12U }, /* TrailingOnes 0, TotalCoeff 3: 0010 00 */
	{ 14U, 6U, 25U }, /* TrailingOnes 1, TotalCoeff 6: 0011 10 */
	{ 13U, 6U, 26U }, /* TrailingOnes 2, TotalCoeff 6: 0011 01 */
	{ 10U, 6U, 29U }, /* TrailingOnes 1, TotalCoeff 7: 0010 10 */
	{ 9U, 6U, 30U }, /* TrailingOnes 2, TotalCoeff 7: 0010 01 */
	{ 12U, 6U, 39U }, /* TrailingOnes 3, TotalCoeff 9: 0011 00 */
	{ 15U, 7U, 16U }, /* TrailingOnes 0, TotalCoeff 4: 0001 111 */
	{ 11U, 7U, 20U }, /* TrailingOnes 0, TotalCoeff 5: 0001 011 */
	{ 9U, 7U, 24U }, /* TrailingOnes 0, TotalCoeff 6: 0001 001 */
	{ 8U, 7U, 28U }, /* TrailingOnes 0, TotalCoeff 7: 0001 000 */
	{ 14U, 7U, 33U }, /* TrailingOnes 1, TotalCoeff 8: 0001 110 */
	{ 13U, 7U, 34U }, /* TrailingOnes 2, TotalCoeff 8: 0001 101 */
	{ 10U, 7U, 38U }, /* TrailingOnes 2, TotalCoeff 9: 0001 010 */
	{ 12U, 7U, 43U }, /* TrailingOnes 3, TotalCoeff 10: 0001 100 */
	{ 15U, 8U, 32U }, /* TrailingOnes 0, TotalCoeff 8: 0000 1111 */
	{ 11U, 8U, 36U }, /* TrailingOnes 0, TotalCoeff 9: 0000 1011 */
	{ 14U, 8U, 37U }, /* TrailingOnes 1, TotalCoeff 9: 0000 1110 */
	{ 10U, 8U, 41U }, /* TrailingOnes 1, TotalCoeff 10: 0000 1010 */
	{ 13U, 8U, 42U }, /* TrailingOnes 2, TotalCoeff 10: 0000 1101 */
	{ 9U, 8U, 46U }, /* TrailingOnes 2, TotalCoeff 11: 0000 1001 */
	{ 12U, 8U, 47U }, /* TrailingOnes 3, TotalCoeff 11: 0000 1100 */
	{ 8U, 8U, 51U }, /* TrailingOnes 3, TotalCoeff 12: 0000 1000 */
	{ 15U, 9U, 40U }, /* TrailingOnes 0, TotalCoeff 10: 0000 0111 1 */
	{ 11U, 9U, 44U }, /* TrailingOnes 0, TotalCoeff 11: 0000 0101 1 */
	{ 14U, 9U, 45U }, /* TrailingOnes 1, TotalCoeff 11: 0000 0111 0 */
	{ 8U, 9U, 48U }, /* TrailingOnes 0, TotalCoeff 12: 0000 0100 0 */
	{ 10U, 9U, 49U }, /* TrailingOnes 1, TotalCoeff 12: 0000 0101 0 */
	{ 13U, 9U, 50U }, /* TrailingOnes 2, TotalCoeff 12: 0000 0110 1 */
	{ 7U, 9U, 53U }, /* TrailingOnes 1, TotalCoeff 13: 0000 0011 1 */
	{ 9U, 9U, 54U }, /* TrailingOnes 2, TotalCoeff 13: 0000 0100 1 */
	{ 12U, 9U, 55U }, /* TrailingOnes 3, TotalCoeff 13: 0000 0110 0 */
	{ 13U, 10U, 52U }, /* TrailingOnes 0, TotalCoeff 13: 0000 0011 01 */
	{ 9U, 10U, 56U }, /* TrailingOnes 0, TotalCoeff 14: 0000 0010 01 */
	{ 12U, 10U, 57U }, /* TrailingOnes 1, TotalCoeff 14: 0000 0011 00 */
	{ 11U, 10U, 58U }, /* TrailingOnes 2, TotalCoeff 14: 0000 0010 11 */
	{ 10U, 10U, 59U }, /* TrailingOnes 3, TotalCoeff 14: 0000 0010 10 */
	{ 5U, 10U, 60U }, /* TrailingOnes 0, TotalCoeff 15: 0000 0001 01 */
	{ 8U, 10U, 61U }, /* TrailingOnes 1, TotalCoeff 15: 0000 0010 00 */
	{ 7U, 10U, 62U }, /* TrailingOnes 2, TotalCoeff 15: 0000 0001 11 */
	{ 6U, 10U, 63U }, /* TrailingOnes 3, TotalCoeff 15: 0000 0001 10 */
	{ 1U, 10U, 64U }, /* TrailingOnes 0, TotalCoeff 16: 0000 0000 01 */
	{ 4U, 10U, 65U }, /* TrailingOnes 1, TotalCoeff 16: 0000 0001 00 */
	{ 3U, 10U, 66U }, /* TrailingOnes 2, TotalCoeff 16: 0000 0000 11 */
	{ 2U, 10U, 67U }, /* TrailingOnes 3, TotalCoeff 16: 0000 0000 10 */
};

/*
 * coeff_token, column 8 <= nC, by increasing length: the code, its length in bits, and
 * ( TotalCoeff << 2 ) | TrailingOnes.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-5, the 8 <= nC column.
 */
static const struct cavlc_code coeff_token_8_up[62] = {
	{ 3U, 6U, 0U }, /* TrailingOnes 0, TotalCoeff 0: 0000 11 */
	{ 0U, 6U, 4U }, /* TrailingOnes 0, TotalCoeff 1: 0000 00 */
	{ 1U, 6U, 5U }, /* TrailingOnes 1, TotalCoeff 1: 0000 01 */
	{ 4U, 6U, 8U }, /* TrailingOnes 0, TotalCoeff 2: 0001 00 */
	{ 5U, 6U, 9U }, /* TrailingOnes 1, TotalCoeff 2: 0001 01 */
	{ 6U, 6U, 10U }, /* TrailingOnes 2, TotalCoeff 2: 0001 10 */
	{ 8U, 6U, 12U }, /* TrailingOnes 0, TotalCoeff 3: 0010 00 */
	{ 9U, 6U, 13U }, /* TrailingOnes 1, TotalCoeff 3: 0010 01 */
	{ 10U, 6U, 14U }, /* TrailingOnes 2, TotalCoeff 3: 0010 10 */
	{ 11U, 6U, 15U }, /* TrailingOnes 3, TotalCoeff 3: 0010 11 */
	{ 12U, 6U, 16U }, /* TrailingOnes 0, TotalCoeff 4: 0011 00 */
	{ 13U, 6U, 17U }, /* TrailingOnes 1, TotalCoeff 4: 0011 01 */
	{ 14U, 6U, 18U }, /* TrailingOnes 2, TotalCoeff 4: 0011 10 */
	{ 15U, 6U, 19U }, /* TrailingOnes 3, TotalCoeff 4: 0011 11 */
	{ 16U, 6U, 20U }, /* TrailingOnes 0, TotalCoeff 5: 0100 00 */
	{ 17U, 6U, 21U }, /* TrailingOnes 1, TotalCoeff 5: 0100 01 */
	{ 18U, 6U, 22U }, /* TrailingOnes 2, TotalCoeff 5: 0100 10 */
	{ 19U, 6U, 23U }, /* TrailingOnes 3, TotalCoeff 5: 0100 11 */
	{ 20U, 6U, 24U }, /* TrailingOnes 0, TotalCoeff 6: 0101 00 */
	{ 21U, 6U, 25U }, /* TrailingOnes 1, TotalCoeff 6: 0101 01 */
	{ 22U, 6U, 26U }, /* TrailingOnes 2, TotalCoeff 6: 0101 10 */
	{ 23U, 6U, 27U }, /* TrailingOnes 3, TotalCoeff 6: 0101 11 */
	{ 24U, 6U, 28U }, /* TrailingOnes 0, TotalCoeff 7: 0110 00 */
	{ 25U, 6U, 29U }, /* TrailingOnes 1, TotalCoeff 7: 0110 01 */
	{ 26U, 6U, 30U }, /* TrailingOnes 2, TotalCoeff 7: 0110 10 */
	{ 27U, 6U, 31U }, /* TrailingOnes 3, TotalCoeff 7: 0110 11 */
	{ 28U, 6U, 32U }, /* TrailingOnes 0, TotalCoeff 8: 0111 00 */
	{ 29U, 6U, 33U }, /* TrailingOnes 1, TotalCoeff 8: 0111 01 */
	{ 30U, 6U, 34U }, /* TrailingOnes 2, TotalCoeff 8: 0111 10 */
	{ 31U, 6U, 35U }, /* TrailingOnes 3, TotalCoeff 8: 0111 11 */
	{ 32U, 6U, 36U }, /* TrailingOnes 0, TotalCoeff 9: 1000 00 */
	{ 33U, 6U, 37U }, /* TrailingOnes 1, TotalCoeff 9: 1000 01 */
	{ 34U, 6U, 38U }, /* TrailingOnes 2, TotalCoeff 9: 1000 10 */
	{ 35U, 6U, 39U }, /* TrailingOnes 3, TotalCoeff 9: 1000 11 */
	{ 36U, 6U, 40U }, /* TrailingOnes 0, TotalCoeff 10: 1001 00 */
	{ 37U, 6U, 41U }, /* TrailingOnes 1, TotalCoeff 10: 1001 01 */
	{ 38U, 6U, 42U }, /* TrailingOnes 2, TotalCoeff 10: 1001 10 */
	{ 39U, 6U, 43U }, /* TrailingOnes 3, TotalCoeff 10: 1001 11 */
	{ 40U, 6U, 44U }, /* TrailingOnes 0, TotalCoeff 11: 1010 00 */
	{ 41U, 6U, 45U }, /* TrailingOnes 1, TotalCoeff 11: 1010 01 */
	{ 42U, 6U, 46U }, /* TrailingOnes 2, TotalCoeff 11: 1010 10 */
	{ 43U, 6U, 47U }, /* TrailingOnes 3, TotalCoeff 11: 1010 11 */
	{ 44U, 6U, 48U }, /* TrailingOnes 0, TotalCoeff 12: 1011 00 */
	{ 45U, 6U, 49U }, /* TrailingOnes 1, TotalCoeff 12: 1011 01 */
	{ 46U, 6U, 50U }, /* TrailingOnes 2, TotalCoeff 12: 1011 10 */
	{ 47U, 6U, 51U }, /* TrailingOnes 3, TotalCoeff 12: 1011 11 */
	{ 48U, 6U, 52U }, /* TrailingOnes 0, TotalCoeff 13: 1100 00 */
	{ 49U, 6U, 53U }, /* TrailingOnes 1, TotalCoeff 13: 1100 01 */
	{ 50U, 6U, 54U }, /* TrailingOnes 2, TotalCoeff 13: 1100 10 */
	{ 51U, 6U, 55U }, /* TrailingOnes 3, TotalCoeff 13: 1100 11 */
	{ 52U, 6U, 56U }, /* TrailingOnes 0, TotalCoeff 14: 1101 00 */
	{ 53U, 6U, 57U }, /* TrailingOnes 1, TotalCoeff 14: 1101 01 */
	{ 54U, 6U, 58U }, /* TrailingOnes 2, TotalCoeff 14: 1101 10 */
	{ 55U, 6U, 59U }, /* TrailingOnes 3, TotalCoeff 14: 1101 11 */
	{ 56U, 6U, 60U }, /* TrailingOnes 0, TotalCoeff 15: 1110 00 */
	{ 57U, 6U, 61U }, /* TrailingOnes 1, TotalCoeff 15: 1110 01 */
	{ 58U, 6U, 62U }, /* TrailingOnes 2, TotalCoeff 15: 1110 10 */
	{ 59U, 6U, 63U }, /* TrailingOnes 3, TotalCoeff 15: 1110 11 */
	{ 60U, 6U, 64U }, /* TrailingOnes 0, TotalCoeff 16: 1111 00 */
	{ 61U, 6U, 65U }, /* TrailingOnes 1, TotalCoeff 16: 1111 01 */
	{ 62U, 6U, 66U }, /* TrailingOnes 2, TotalCoeff 16: 1111 10 */
	{ 63U, 6U, 67U }, /* TrailingOnes 3, TotalCoeff 16: 1111 11 */
};

/*
 * coeff_token, column nC = -1, by increasing length: the code, its length in bits, and
 * ( TotalCoeff << 2 ) | TrailingOnes.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-5, the nC = -1 column.
 */
static const struct cavlc_code coeff_token_chroma_dc[14] = {
	{ 1U, 1U, 5U }, /* TrailingOnes 1, TotalCoeff 1: 1 */
	{ 1U, 2U, 0U }, /* TrailingOnes 0, TotalCoeff 0: 01 */
	{ 1U, 3U, 10U }, /* TrailingOnes 2, TotalCoeff 2: 001 */
	{ 7U, 6U, 4U }, /* TrailingOnes 0, TotalCoeff 1: 0001 11 */
	{ 4U, 6U, 8U }, /* TrailingOnes 0, TotalCoeff 2: 0001 00 */
	{ 6U, 6U, 9U }, /* TrailingOnes 1, TotalCoeff 2: 0001 10 */
	{ 3U, 6U, 12U }, /* TrailingOnes 0, TotalCoeff 3: 0000 11 */
	{ 5U, 6U, 15U }, /* TrailingOnes 3, TotalCoeff 3: 0001 01 */
	{ 2U, 6U, 16U }, /* TrailingOnes 0, TotalCoeff 4: 0000 10 */
	{ 3U, 7U, 13U }, /* TrailingOnes 1, TotalCoeff 3: 0000 011 */
	{ 2U, 7U, 14U }, /* TrailingOnes 2, TotalCoeff 3: 0000 010 */
	{ 0U, 7U, 19U }, /* TrailingOnes 3, TotalCoeff 4: 0000 000 */
	{ 3U, 8U, 17U }, /* TrailingOnes 1, TotalCoeff 4: 0000 0011 */
	{ 2U, 8U, 18U }, /* TrailingOnes 2, TotalCoeff 4: 0000 0010 */
};

/*
 * total_zeros for blocks of 15 or 16 coefficients: row tzVlcIndex - 1, entry total_zeros (0 up to 16 -
 * tzVlcIndex); the code, its length in bits and the value.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-7 (tzVlcIndex 1 to 7) and Table 9-8 (tzVlcIndex 8 to 15).
 */
static const struct cavlc_code total_zeros_4x4[15][16] = {
	/* tzVlcIndex 1 */
	{
		{ 1U, 1U, 0U }, /* total_zeros 0: 1 */
		{ 3U, 3U, 1U }, /* total_zeros 1: 011 */
		{ 2U, 3U, 2U }, /* total_zeros 2: 010 */
		{ 3U, 4U, 3U }, /* total_zeros 3: 0011 */
		{ 2U, 4U, 4U }, /* total_zeros 4: 0010 */
		{ 3U, 5U, 5U }, /* total_zeros 5: 0001 1 */
		{ 2U, 5U, 6U }, /* total_zeros 6: 0001 0 */
		{ 3U, 6U, 7U }, /* total_zeros 7: 0000 11 */
		{ 2U, 6U, 8U }, /* total_zeros 8: 0000 10 */
		{ 3U, 7U, 9U }, /* total_zeros 9: 0000 011 */
		{ 2U, 7U, 10U }, /* total_zeros 10: 0000 010 */
		{ 3U, 8U, 11U }, /* total_zeros 11: 0000 0011 */
		{ 2U, 8U, 12U }, /* total_zeros 12: 0000 0010 */
		{ 3U, 9U, 13U }, /* total_zeros 13: 0000 0001 1 */
		{ 2U, 9U, 14U }, /* total_zeros 14: 0000 0001 0 */
		{ 1U, 9U, 15U }, /* total_zeros 15: 0000 0000 1 */
	},
	/* tzVlcIndex 2 */
	{
		{ 7U, 3U, 0U }, /* total_zeros 0: 111 */
		{ 6U, 3U, 1U }, /* total_zeros 1: 110 */
		{ 5U, 3U, 2U }, /* total_zeros 2: 101 */
		{ 4U, 3U, 3U }, /* total_zeros 3: 100 */
		{ 3U, 3U, 4U }, /* total_zeros 4: 011 */
		{ 5U, 4U, 5U }, /* total_zeros 5: 0101 */
		{ 4U, 4U, 6U }, /* total_zeros 6: 0100 */
		{ 3U, 4U, 7U }, /* total_zeros 7: 0011 */
		{ 2U, 4U, 8U }, /* total_zeros 8: 0010 */
		{ 3U, 5U, 9U }, /* total_zeros 9: 0001 1 */
		{ 2U, 5U, 10U }, /* total_zeros 10: 0001 0 */
		{ 3U, 6U, 11U }, /* total_zeros 11: 0000 11 */
		{ 2U, 6U, 12U }, /* total_zeros 12: 0000 10 */
		{ 1U, 6U, 13U }, /* total_zeros 13: 0000 01 */
		{ 0U, 6U, 14U }, /* total_zeros 14: 0000 00 */
	},
	/* tzVlcIndex 3 */
	{
		{ 5U, 4U, 0U }, /* total_zeros 0: 0101 */
		{ 7U, 3U, 1U }, /* total_zeros 1: 111 */
		{ 6U, 3U, 2U }, /* total_zeros 2: 110 */
		{ 5U, 3U, 3U }, /* total_zeros 3: 101 */
		{ 4U, 4U, 4U }, /* total_zeros 4: 0100 */
		{ 3U, 4U, 5U }, /* total_zeros 5: 0011 */
		{ 4U, 3U, 6U }, /* total_zeros 6: 100 */
		{ 3U, 3U, 7U }, /* total_zeros 7: 011 */
		{ 2U, 4U, 8U }, /* total_zeros 8: 0010 */
		{ 3U, 5U, 9U }, /* total_zeros 9: 0001 1 */
		{ 2U, 5U, 10U }, /* total_zeros 10: 0001 0 */
		{ 1U, 6U, 11U }, /* total_zeros 11: 0000 01 */
		{ 1U, 5U, 12U }, /* total_zeros 12: 0000 1 */
		{ 0U, 6U, 13U }, /* total_zeros 13: 0000 00 */
	},
	/* tzVlcIndex 4 */
	{
		{ 3U, 5U, 0U }, /* total_zeros 0: 0001 1 */
		{ 7U, 3U, 1U }, /* total_zeros 1: 111 */
		{ 5U, 4U, 2U }, /* total_zeros 2: 0101 */
		{ 4U, 4U, 3U }, /* total_zeros 3: 0100 */
		{ 6U, 3U, 4U }, /* total_zeros 4: 110 */
		{ 5U, 3U, 5U }, /* total_zeros 5: 101 */
		{ 4U, 3U, 6U }, /* total_zeros 6: 100 */
		{ 3U, 4U, 7U }, /* total_zeros 7: 0011 */
		{ 3U, 3U, 8U }, /* total_zeros 8: 011 */
		{ 2U, 4U, 9U }, /* total_zeros 9: 0010 */
		{ 2U, 5U, 10U }, /* total_zeros 10: 0001 0 */
		{ 1U, 5U, 11U }, /* total_zeros 11: 0000 1 */
		{ 0U, 5U, 12U }, /* total_zeros 12: 0000 0 */
	},
	/* tzVlcIndex 5 */
	{
		{ 5U, 4U, 0U }, /* total_zeros 0: 0101 */
		{ 4U, 4U, 1U }, /* total_zeros 1: 0100 */
		{ 3U, 4U, 2U }, /* total_zeros 2: 0011 */
		{ 7U, 3U, 3U }, /* total_zeros 3: 111 */
		{ 6U, 3U, 4U }, /* total_zeros 4: 110 */
		{ 5U, 3U, 5U }, /* total_zeros 5: 101 */
		{ 4U, 3U, 6U }, /* total_zeros 6: 100 */
		{ 3U, 3U, 7U }, /* total_zeros 7: 011 */
		{ 2U, 4U, 8U }, /* total_zeros 8: 0010 */
		{ 1U, 5U, 9U }, /* total_zeros 9: 0000 1 */
		{ 1U, 4U, 10U }, /* total_zeros 10: 0001 */
		{ 0U, 5U, 11U }, /* total_zeros 11: 0000 0 */
	},
	/* tzVlcIndex 6 */
	{
		{ 1U, 6U, 0U }, /* total_zeros 0: 0000 01 */
		{ 1U, 5U, 1U }, /* total_zeros 1: 0000 1 */
		{ 7U, 3U, 2U }, /* total_zeros 2: 111 */
		{ 6U, 3U, 3U }, /* total_zeros 3: 110 */
		{ 5U, 3U, 4U }, /* total_zeros 4: 101 */
		{ 4U, 3U, 5U }, /* total_zeros 5: 100 */
		{ 3U, 3U, 6U }, /* total_zeros 6: 011 */
		{ 2U, 3U, 7U }, /* total_zeros 7: 010 */
		{ 1U, 4U, 8U }, /* total_zeros 8: 0001 */
		{ 1U, 3U, 9U }, /* total_zeros 9: 001 */
		{ 0U, 6U, 10U }, /* total_zeros 10: 0000 00 */
	},
	/* tzVlcIndex 7 */
	{
		{ 1U, 6U, 0U }, /* total_zeros 0: 0000 01 */
		{ 1U, 5U, 1U }, /* total_zeros 1: 0000 1 */
		{ 5U, 3U, 2U }, /* total_zeros 2: 101 */
		{ 4U, 3U, 3U }, /* total_zeros 3: 100 */
		{ 3U, 3U, 4U }, /* total_zeros 4: 011 */
		{ 3U, 2U, 5U }, /* total_zeros 5: 11 */
		{ 2U, 3U, 6U }, /* total_zeros 6: 010 */
		{ 1U, 4U, 7U }, /* total_zeros 7: 0001 */
		{ 1U, 3U, 8U }, /* total_zeros 8: 001 */
		{ 0U, 6U, 9U }, /* total_zeros 9: 0000 00 */
	},
	/* tzVlcIndex 8 */
	{
		{ 1U, 6U, 0U }, /* total_zeros 0: 0000 01 */
		{ 1U, 4U, 1U }, /* total_zeros 1: 0001 */
		{ 1U, 5U, 2U }, /* total_zeros 2: 0000 1 */
		{ 3U, 3U, 3U }, /* total_zeros 3: 011 */
		{ 3U, 2U, 4U }, /* total_zeros 4: 11 */
		{ 2U, 2U, 5U }, /* total_zeros 5: 10 */
		{ 2U, 3U, 6U }, /* total_zeros 6: 010 */
		{ 1U, 3U, 7U }, /* total_zeros 7: 001 */
		{ 0U, 6U, 8U }, /* total_zeros 8: 0000 00 */
	},
	/* tzVlcIndex 9 */
	{
		{ 1U, 6U, 0U }, /* total_zeros 0: 0000 01 */
		{ 0U, 6U, 1U }, /* total_zeros 1: 0000 00 */
		{ 1U, 4U, 2U }, /* total_zeros 2: 0001 */
		{ 3U, 2U, 3U }, /* total_zeros 3: 11 */
		{ 2U, 2U, 4U }, /* total_zeros 4: 10 */
		{ 1U, 3U, 5U }, /* total_zeros 5: 001 */
		{ 1U, 2U, 6U }, /* total_zeros 6: 01 */
		{ 1U, 5U, 7U }, /* total_zeros 7: 0000 1 */
	},
	/* tzVlcIndex 10 */
	{
		{ 1U, 5U, 0U }, /* total_zeros 0: 0000 1 */
		{ 0U, 5U, 1U }, /* total_zeros 1: 0000 0 */
		{ 1U, 3U, 2U }, /* total_zeros 2: 001 */
		{ 3U, 2U, 3U }, /* total_zeros 3: 11 */
		{ 2U, 2U, 4U }, /* total_zeros 4: 10 */
		{ 1U, 2U, 5U }, /* total_zeros 5: 01 */
		{ 1U, 4U, 6U }, /* total_zeros 6: 0001 */
	},
	/* tzVlcIndex 11 */
	{
		{ 0U, 4U, 0U }, /* total_zeros 0: 0000 */
		{ 1U, 4U, 1U }, /* total_zeros 1: 0001 */
		{ 1U, 3U, 2U }, /* total_zeros 2: 001 */
		{ 2U, 3U, 3U }, /* total_zeros 3: 010 */
		{ 1U, 1U, 4U }, /* total_zeros 4: 1 */
		{ 3U, 3U, 5U }, /* total_zeros 5: 011 */
	},
	/* tzVlcIndex 12 */
	{
		{ 0U, 4U, 0U }, /* total_zeros 0: 0000 */
		{ 1U, 4U, 1U }, /* total_zeros 1: 0001 */
		{ 1U, 2U, 2U }, /* total_zeros 2: 01 */
		{ 1U, 1U, 3U }, /* total_zeros 3: 1 */
		{ 1U, 3U, 4U }, /* total_zeros 4: 001 */
	},
	/* tzVlcIndex 13 */
	{
		{ 0U, 3U, 0U }, /* total_zeros 0: 000 */
		{ 1U, 3U, 1U }, /* total_zeros 1: 001 */
		{ 1U, 1U, 2U }, /* total_zeros 2: 1 */
		{ 1U, 2U, 3U }, /* total_zeros 3: 01 */
	},
	/* tzVlcIndex 14 */
	{
		{ 0U, 2U, 0U }, /* total_zeros 0: 00 */
		{ 1U, 2U, 1U }, /* total_zeros 1: 01 */
		{ 1U, 1U, 2U }, /* total_zeros 2: 1 */
	},
	/* tzVlcIndex 15 */
	{
		{ 0U, 1U, 0U }, /* total_zeros 0: 0 */
		{ 1U, 1U, 1U }, /* total_zeros 1: 1 */
	},
};

/*
 * total_zeros for the chroma DC block of 4:2:0: row tzVlcIndex - 1, entry total_zeros (0 up to 4 -
 * tzVlcIndex); the code, its length in bits and the value.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-9 (a).
 */
static const struct cavlc_code total_zeros_chroma_dc[3][4] = {
	/* tzVlcIndex 1 */
	{
		{ 1U, 1U, 0U }, /* total_zeros 0: 1 */
		{ 1U, 2U, 1U }, /* total_zeros 1: 01 */
		{ 1U, 3U, 2U }, /* total_zeros 2: 001 */
		{ 0U, 3U, 3U }, /* total_zeros 3: 000 */
	},
	/* tzVlcIndex 2 */
	{
		{ 1U, 1U, 0U }, /* total_zeros 0: 1 */
		{ 1U, 2U, 1U }, /* total_zeros 1: 01 */
		{ 0U, 2U, 2U }, /* total_zeros 2: 00 */
	},
	/* tzVlcIndex 3 */
	{
		{ 1U, 1U, 0U }, /* total_zeros 0: 1 */
		{ 0U, 1U, 1U }, /* total_zeros 1: 0 */
	},
};

/*
 * run_before: row Min( zerosLeft, 7 ) - 1, entry run_before (0 up to Min( zerosLeft, 14 )); the
 * code, its length in bits and the value.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-10 (zerosLeft 1 to 6 and greater than 6).
 */
static const struct cavlc_code run_before[7][15] = {
	/* zerosLeft 1 */
	{
		{ 1U, 1U, 0U }, /* run_before 0: 1 */
		{ 0U, 1U, 1U }, /* run_before 1: 0 */
	},
	/* zerosLeft 2 */
	{
		{ 1U, 1U, 0U }, /* run_before 0: 1 */
		{ 1U, 2U, 1U }, /* run_before 1: 01 */
		{ 0U, 2U, 2U }, /* run_before 2: 00 */
	},
	/* zerosLeft 3 */
	{
		{ 3U, 2U, 0U }, /* run_before 0: 11 */
		{ 2U, 2U, 1U }, /* run_before 1: 10 */
		{ 1U, 2U, 2U }, /* run_before 2: 01 */
		{ 0U, 2U, 3U }, /* run_before 3: 00 */
	},
	/* zerosLeft 4 */
	{
		{ 3U, 2U, 0U }, /* run_before 0: 11 */
		{ 2U, 2U, 1U }, /* run_before 1: 10 */
		{ 1U, 2U, 2U }, /* run_before 2: 01 */
		{ 1U, 3U, 3U }, /* run_before 3: 001 */
		{ 0U, 3U, 4U }, /* run_before 4: 000 */
	},
	/* zerosLeft 5 */
	{
		{ 3U, 2U, 0U }, /* run_before 0: 11 */
		{ 2U, 2U, 1U }, /* run_before 1: 10 */
		{ 3U, 3U, 2U }, /* run_before 2: 011 */
		{ 2U, 3U, 3U }, /* run_before 3: 010 */
		{ 1U, 3U, 4U }, /* run_before 4: 001 */
		{ 0U, 3U, 5U }, /* run_before 5: 000 */
	},
	/* zerosLeft 6 */
	{
		{ 3U, 2U, 0U }, /* run_before 0: 11 */
		{ 0U, 3U, 1U }, /* run_before 1: 000 */
		{ 1U, 3U, 2U }, /* run_before 2: 001 */
		{ 3U, 3U, 3U }, /* run_before 3: 011 */
		{ 2U, 3U, 4U }, /* run_before 4: 010 */
		{ 5U, 3U, 5U }, /* run_before 5: 101 */
		{ 4U, 3U, 6U }, /* run_before 6: 100 */
	},
	/* zerosLeft above 6 */
	{
		{ 7U, 3U, 0U }, /* run_before 0: 111 */
		{ 6U, 3U, 1U }, /* run_before 1: 110 */
		{ 5U, 3U, 2U }, /* run_before 2: 101 */
		{ 4U, 3U, 3U }, /* run_before 3: 100 */
		{ 3U, 3U, 4U }, /* run_before 4: 011 */
		{ 2U, 3U, 5U }, /* run_before 5: 010 */
		{ 1U, 3U, 6U }, /* run_before 6: 001 */
		{ 1U, 4U, 7U }, /* run_before 7: 0001 */
		{ 1U, 5U, 8U }, /* run_before 8: 0000 1 */
		{ 1U, 6U, 9U }, /* run_before 9: 0000 01 */
		{ 1U, 7U, 10U }, /* run_before 10: 0000 001 */
		{ 1U, 8U, 11U }, /* run_before 11: 0000 0001 */
		{ 1U, 9U, 12U }, /* run_before 12: 0000 0000 1 */
		{ 1U, 10U, 13U }, /* run_before 13: 0000 0000 01 */
		{ 1U, 11U, 14U }, /* run_before 14: 0000 0000 001 */
	},
};

/*
 * The coded_block_pattern of each codeNum for the Intra_4x4 and Intra_8x8
 * prediction modes (the macroblock prediction mode columns of the table for
 * ChromaArrayType 1 or 2): CodedBlockPatternLuma in bits 0 to 3 and
 * CodedBlockPatternChroma in bits 4 and 5.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-4 (a), the Intra_4x4, Intra_8x8 column.
 */
static const uint8_t cbp_intra[CAVLC_CBP_COUNT] = {
	47, 31, 15, 0, 23, 27, 29, 30,
	7, 11, 13, 14, 39, 43, 45, 46,
	16, 3, 5, 10, 12, 19, 21, 26,
	28, 35, 37, 42, 44, 1, 2, 4,
	8, 17, 18, 20, 24, 6, 9, 22,
	25, 32, 33, 34, 36, 40, 38, 41
};

/*
 * The coded_block_pattern of each codeNum for the Inter prediction mode
 * (the same layout as the intra table).
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-4 (a), the Inter column.
 */
static const uint8_t cbp_inter[CAVLC_CBP_COUNT] = {
	0, 16, 1, 2, 4, 8, 32, 3,
	5, 10, 12, 15, 47, 7, 11, 13,
	14, 6, 9, 31, 35, 37, 42, 44,
	33, 34, 36, 40, 39, 43, 45, 46,
	17, 18, 20, 24, 19, 21, 26, 28,
	23, 27, 29, 30, 22, 25, 38, 41
};

/* Forward declarations of the static functions. */
static int cavlc_find(struct h264sw_bits *bits, const struct cavlc_code *codes, unsigned count, unsigned *value);
static int cavlc_coeff_token(struct h264sw_bits *bits, int nc, unsigned *total, unsigned *trailing);
static int cavlc_level_prefix(struct h264sw_bits *bits, unsigned *prefix);
static int cavlc_levels(struct h264sw_bits *bits, unsigned total, unsigned trailing, int32_t *level_values);
static int cavlc_total_zeros(struct h264sw_bits *bits, unsigned total, unsigned max_coeff, unsigned *zeros);
static int cavlc_run_before(struct h264sw_bits *bits, unsigned zeros_left, unsigned *run);

/*
 * Reads one residual block (residual_block_cavlc, 7.3.5.3.2, and 9.2).
 *
 * The levels are zeroed first, so an early success (no coefficient) and the
 * levels between the coefficients are already right; then coeff_token, the
 * levels, total_zeros and the runs are read in the order of the syntax and
 * combined as 9.2.4 says.
 */
int
h264sw_cavlc_residual_block(
	struct h264sw_bits *bits,
	int nc,
	unsigned start,
	unsigned end,
	unsigned max_coeff,
	int32_t *levels,
	unsigned *total_coeff)
{
	int32_t level_values[CAVLC_COEFF_MAX];
	unsigned run_values[CAVLC_COEFF_MAX];
	unsigned total;
	unsigned trailing;
	unsigned zeros_left;
	unsigned run;
	unsigned position;
	unsigned index;
	int error;

	/* Refuses missing outputs and a block shape that no syntax produces. */
	if (bits == NULL || levels == NULL || total_coeff == NULL)
		return EINVAL;
	if (max_coeff != 4U && max_coeff != 15U && max_coeff != 16U)
		return EINVAL;
	if (start > end || end >= max_coeff)
		return EINVAL;

	/* Starts with no coefficient: every level 0 and TotalCoeff 0. */
	for (index = 0; index < max_coeff; index++)
		levels[index] = 0;
	*total_coeff = 0U;

	/* Reads coeff_token, which gives TotalCoeff and TrailingOnes. */
	error = cavlc_coeff_token(bits, nc, &total, &trailing);
	if (error != 0)
		return error;

	/* Refuses a block with more coefficients than positions (and TotalCoeff 16 of a 15-coefficient block). */
	if (total > end - start + 1U)
		return EINVAL;

	/* Reports TotalCoeff, which the caller keeps for the nC of later blocks. */
	*total_coeff = total;

	/* Ends the block when no coefficient was sent. */
	if (total == 0U)
		return 0;

	/* Reads the trailing ones and the other levels, the first one read being the highest frequency. */
	error = cavlc_levels(bits, total, trailing, level_values);
	if (error != 0)
		return error;

	/* Takes the zeros before the last coefficient from total_zeros, or none when every position holds a coefficient. */
	zeros_left = 0U;
	if (total < end - start + 1U) {
		error = cavlc_total_zeros(bits, total, max_coeff, &zeros_left);
		if (error != 0)
			return error;
	}

	/* Refuses zeros that would push a coefficient past endIdx. */
	if (zeros_left > end - start + 1U - total)
		return EINVAL;

	/* Reads run_before for every coefficient but the last one read; zeros not given to a run precede the lowest frequency coefficient. */
	for (index = 0; index + 1U < total; index++) {
		run = 0U;
		if (zeros_left > 0U) {
			error = cavlc_run_before(bits, zeros_left, &run);
			if (error != 0)
				return error;
		}

		/* The run is taken from the zeros left, so a run longer than they are is a broken stream. */
		if (run > zeros_left)
			return EINVAL;

		run_values[index] = run;
		zeros_left = zeros_left - run;
	}
	run_values[total - 1U] = zeros_left;

	/* Places the levels from the lowest frequency, each after its run of zeros (9.2.4). */
	position = start;
	for (index = total; index > 0U; index--) {
		position = position + run_values[index - 1U];

		/* A level that would land past endIdx breaks the block's size. */
		if (position > end)
			return EINVAL;

		levels[position] = level_values[index - 1U];
		position++;
	}

	/* Succeeded: levels holds coeffLevel and total_coeff TotalCoeff. */
	return 0;
}

/*
 * Derives nC for a block of the current macroblock (9.2.1).
 *
 * The neighbouring blocks A and B are found by the block scan of the kind of
 * block (luma, Cb or Cr), and each one's count is read from the macroblock
 * that holds it.
 */
int
h264sw_cavlc_nc(
	const struct h264sw_slice_ctx *ctx,
	unsigned block)
{
	struct h264sw_mb *mb_a;
	struct h264sw_mb *mb_b;
	unsigned base;
	unsigned local;
	unsigned blk_a;
	unsigned blk_b;
	int count_a;
	int count_b;
	int nc;

	/* A missing context or a block index no macroblock has gives no neighbours. */
	if (ctx == NULL)
		return 0;
	if (block >= 24U)
		return 0;

	/* Finds the neighbouring blocks A (left) and B (above) in the scan of the block's kind. */
	if (block < 16U) {
		/* A luma block, or the Intra16x16 DC block which is read as block 0 (6.4.11.4). */
		base = 0U;
		local = block;
		mb_a = h264sw_mb_neighbour_4x4(&ctx->n, local, 0, &blk_a);
		mb_b = h264sw_mb_neighbour_4x4(&ctx->n, local, 1, &blk_b);
	} else if (block < 20U) {
		/* A Cb AC block, whose counts are kept after the luma blocks (6.4.11.5). */
		base = 16U;
		local = block - 16U;
		mb_a = h264sw_mb_neighbour_chroma(&ctx->n, local, 0, &blk_a);
		mb_b = h264sw_mb_neighbour_chroma(&ctx->n, local, 1, &blk_b);
	} else {
		/* A Cr AC block, whose counts follow those of Cb (6.4.11.5). */
		base = 20U;
		local = block - 20U;
		mb_a = h264sw_mb_neighbour_chroma(&ctx->n, local, 0, &blk_a);
		mb_b = h264sw_mb_neighbour_chroma(&ctx->n, local, 1, &blk_b);
	}

	/* Reads the counts of the neighbours that are available; an unavailable neighbour has no count. */
	count_a = 0;
	count_b = 0;
	if (mb_a != NULL)
		count_a = (int)mb_a->total_coeff[base + blk_a];
	if (mb_b != NULL)
		count_b = (int)mb_b->total_coeff[base + blk_b];

	/* Combines the counts by the rule of availability (9.2.1, step 7). */
	if (mb_a != NULL && mb_b != NULL) {
		/* Both neighbours are available: their rounded mean. */
		nc = (count_a + count_b + 1) >> 1;
	} else if (mb_a != NULL) {
		/* Only the left neighbour is available. */
		nc = count_a;
	} else if (mb_b != NULL) {
		/* Only the upper neighbour is available. */
		nc = count_b;
	} else {
		/* No neighbour is available. */
		nc = 0;
	}

	/* Reports the context nC. */
	return nc;
}

/*
 * Maps the codeNum of coded_block_pattern me(v) to its value (9.1.2, Table 9-4).
 *
 * The table is the one for ChromaArrayType 1, the only one this decoder reads.
 */
int
h264sw_cavlc_cbp(
	uint32_t code_num,
	int intra,
	unsigned *cbp)
{
	/* Refuses a missing output and a codeNum above the table. */
	if (cbp == NULL)
		return EINVAL;
	if (code_num >= CAVLC_CBP_COUNT)
		return EINVAL;

	/* Looks the pattern up in the column of the macroblock prediction mode. */
	if (intra != 0) {
		*cbp = (unsigned)cbp_intra[code_num];
	} else {
		*cbp = (unsigned)cbp_inter[code_num];
	}

	/* Succeeded: *cbp holds CodedBlockPatternLuma and CodedBlockPatternChroma. */
	return 0;
}

/*
 * Finds the code of a table that starts the next bits, and moves past it.
 *
 * The next 16 bits are compared with the first count codes; the codes of a
 * table form a prefix code, so at most one of them matches.  Returns EINVAL
 * when none does or when the code runs past the end of the RBSP.
 */
static int
cavlc_find(
	struct h264sw_bits *bits,
	const struct cavlc_code *codes,
	unsigned count,
	unsigned *value)
{
	uint32_t window;
	unsigned index;
	unsigned shift;

	/* Looks at the next bits without moving; bits past the end read as 0 and are caught when the code is skipped. */
	window = h264sw_bits_peek(bits, CAVLC_WINDOW_BITS);
	if (bits->error != 0)
		return EINVAL;

	/* Tries each code of the table against the start of the window. */
	for (index = 0; index < count; index++) {
		shift = CAVLC_WINDOW_BITS - (unsigned)codes[index].length;

		/* A match gives the value and moves past the code. */
		if ((window >> shift) == (uint32_t)codes[index].code) {
			*value = (unsigned)codes[index].value;

			/* Moves past the code; this fails when the code was made of bits beyond the end. */
			h264sw_bits_skip(bits, (size_t)codes[index].length);
			if (bits->error != 0)
				return EINVAL;

			/* Succeeded: the value of the code. */
			return 0;
		}
	}

	/* Reports a bit pattern that is in no code of the table. */
	return EINVAL;
}

/*
 * Reads coeff_token and splits it into TotalCoeff and TrailingOnes (9.2.1, Table 9-5).
 *
 * nc selects the column of Table 9-5.  The nC of -2 (4:2:2 chroma DC) and
 * below is refused: the decoder does not read that format.
 */
static int
cavlc_coeff_token(
	struct h264sw_bits *bits,
	int nc,
	unsigned *total,
	unsigned *trailing)
{
	const struct cavlc_code *codes;
	unsigned count;
	unsigned value;
	int error;

	/* Picks the column of Table 9-5 that nC selects. */
	count = CAVLC_TOKEN_COUNT;
	if (nc < H264SW_NC_CHROMA_DC) {
		/* No column exists for these values of nC. */
		return EINVAL;
	} else if (nc == H264SW_NC_CHROMA_DC) {
		/* The chroma DC block of 4:2:0. */
		codes = coeff_token_chroma_dc;
		count = CAVLC_TOKEN_CHROMA_DC_COUNT;
	} else if (nc < 2) {
		/* 0 <= nC < 2. */
		codes = coeff_token_0_2;
	} else if (nc < 4) {
		/* 2 <= nC < 4. */
		codes = coeff_token_2_4;
	} else if (nc < 8) {
		/* 4 <= nC < 8. */
		codes = coeff_token_4_8;
	} else {
		/* 8 <= nC, the 6-bit fixed-length code. */
		codes = coeff_token_8_up;
	}

	/* Reads the code and finds its value. */
	error = cavlc_find(bits, codes, count, &value);
	if (error != 0)
		return error;

	/* Splits the value into the two counts. */
	*total = value >> 2;
	*trailing = value & 3U;

	/* Succeeded: TotalCoeff and TrailingOnes. */
	return 0;
}

/*
 * Reads level_prefix: the count of zero bits before the first 1 bit (9.2.2.1).
 *
 * A prefix above 25 is refused, which also ends a run of zeros that never
 * meets a 1 bit; the largest level a block may hold needs a prefix well
 * below that.
 */
static int
cavlc_level_prefix(
	struct h264sw_bits *bits,
	unsigned *prefix)
{
	uint32_t window;
	unsigned zeros;

	/* Looks at the next 32 bits for the first 1. */
	window = h264sw_bits_peek(bits, 32U);
	if (bits->error != 0)
		return EINVAL;

	/* A window with no 1 bit holds a prefix of 32 or more, which is out of range. */
	if (window == 0U)
		return EINVAL;

	/* Counts the zero bits before the first 1. */
	zeros = 0U;
	while ((window & 0x80000000U) == 0U) {
		window = window << 1;
		zeros++;
	}

	/* Refuses a prefix that no level of 8-bit video needs. */
	if (zeros > CAVLC_LEVEL_PREFIX_MAX)
		return EINVAL;

	/* Moves past the zeros and the 1; this fails when they run past the end of the RBSP. */
	h264sw_bits_skip(bits, (size_t)zeros + 1U);
	if (bits->error != 0)
		return EINVAL;

	/* Succeeded: level_prefix. */
	*prefix = zeros;
	return 0;
}

/*
 * Reads the levels of one block in the order they are sent: the trailing ones
 * by their sign, then the other levels by level_prefix and level_suffix (9.2.2).
 *
 * level_values[0] is the level of the highest frequency.
 */
static int
cavlc_levels(
	struct h264sw_bits *bits,
	unsigned total,
	unsigned trailing,
	int32_t *level_values)
{
	uint32_t sign;
	uint32_t suffix;
	int32_t level_code;
	int32_t level;
	int32_t magnitude;
	int32_t limit;
	unsigned index;
	unsigned suffix_length;
	unsigned suffix_size;
	unsigned prefix;
	unsigned capped;
	int error;

	/* Reads the sign of each trailing one: a 0 bit is +1 and a 1 bit is -1. */
	for (index = 0; index < trailing; index++) {
		sign = h264sw_bits_u1(bits);
		if (sign == 0U) {
			level_values[index] = 1;
		} else {
			level_values[index] = -1;
		}
	}
	if (bits->error != 0)
		return EINVAL;

	/* Starts with suffixLength 1 when the block is crowded and has few trailing ones, else 0. */
	suffix_length = 0U;
	if (total > 10U && trailing < 3U)
		suffix_length = 1U;

	/* Reads the remaining levels, each by its prefix and suffix. */
	for (index = trailing; index < total; index++) {
		error = cavlc_level_prefix(bits, &prefix);
		if (error != 0)
			return error;

		/* Picks levelSuffixSize: 4 for the escape of prefix 14 at suffixLength 0, prefix - 3 from prefix 15 on, else suffixLength. */
		suffix_size = suffix_length;
		if (prefix == 14U && suffix_length == 0U) {
			suffix_size = 4U;
		} else if (prefix >= 15U) {
			suffix_size = prefix - 3U;
		}

		/* Reads level_suffix, which is 0 when its size is 0. */
		suffix = h264sw_bits_u(bits, suffix_size);
		if (bits->error != 0)
			return EINVAL;

		/* Forms levelCode: the prefix capped at 15 shifted by suffixLength, plus the suffix. */
		capped = prefix;
		if (capped > 15U)
			capped = 15U;
		level_code = (int32_t)((capped << suffix_length) + suffix);

		/* The escape of prefix 15 at suffixLength 0 starts after the 15 values the shorter prefixes cover. */
		if (prefix >= 15U && suffix_length == 0U)
			level_code = level_code + 15;

		/* A prefix of 16 or more adds the values that the shorter prefixes of its size left. */
		if (prefix >= 16U)
			level_code = level_code + ((int32_t)(1U << (prefix - 3U)) - 4096);

		/* The first level after fewer than 3 trailing ones is at least 2 in magnitude, so its code is shifted by 2. */
		if (index == trailing && trailing < 3U)
			level_code = level_code + 2;

		/* An even code is a positive level and an odd code a negative one. */
		if ((level_code % 2) == 0) {
			level = (level_code + 2) / 2;
		} else {
			level = 0 - ((level_code + 1) / 2);
		}

		/* Refuses a level outside the range of 8-bit video. */
		if (level > CAVLC_LEVEL_MAX || level < CAVLC_LEVEL_MIN)
			return EINVAL;

		level_values[index] = level;

		/* Adapts suffixLength: it starts at 1 after the first level, and grows when a level is large. */
		if (suffix_length == 0U)
			suffix_length = 1U;
		magnitude = level;
		if (magnitude < 0)
			magnitude = 0 - magnitude;
		limit = 3 * (int32_t)(1U << (suffix_length - 1U));
		if (magnitude > limit && suffix_length < 6U)
			suffix_length++;
	}

	/* Succeeded: every level of the block is read. */
	return 0;
}

/*
 * Reads total_zeros, the zeros before the last coefficient (9.2.3, Tables 9-7 to 9-9).
 *
 * tzVlcIndex is TotalCoeff; the chroma DC block (max_coeff 4) has its own
 * table, and the blocks of 15 or 16 coefficients share one.
 */
static int
cavlc_total_zeros(
	struct h264sw_bits *bits,
	unsigned total,
	unsigned max_coeff,
	unsigned *zeros)
{
	const struct cavlc_code *codes;
	unsigned count;
	int error;

	/* Picks the row of the table that tzVlcIndex selects; the row holds the values 0 up to the size of the block less TotalCoeff. */
	if (max_coeff == 4U) {
		/* The chroma DC block of 4:2:0 has TotalCoeff 1 to 3 when total_zeros is sent. */
		if (total < 1U || total > 3U)
			return EINVAL;
		codes = total_zeros_chroma_dc[total - 1U];
		count = 5U - total;
	} else {
		/* A block of 15 or 16 coefficients has TotalCoeff 1 to 15 when total_zeros is sent. */
		if (total < 1U || total > 15U)
			return EINVAL;
		codes = total_zeros_4x4[total - 1U];
		count = 17U - total;
	}

	/* Reads the code and finds total_zeros. */
	error = cavlc_find(bits, codes, count, zeros);
	if (error != 0)
		return error;

	/* Succeeded: total_zeros. */
	return 0;
}

/*
 * Reads run_before, the zeros just before a coefficient (9.2.3, Table 9-10).
 *
 * The table row is chosen by zerosLeft, with one row for every zerosLeft
 * above 6.
 */
static int
cavlc_run_before(
	struct h264sw_bits *bits,
	unsigned zeros_left,
	unsigned *run)
{
	unsigned row;
	unsigned count;
	int error;

	/* Refuses a request with no zeros left, which has no code. */
	if (zeros_left == 0U)
		return EINVAL;

	/* Picks the row of Table 9-10 and the number of runs it holds (0 up to zerosLeft, or up to 14 above 6). */
	row = zeros_left;
	count = zeros_left + 1U;
	if (zeros_left > 6U) {
		row = 7U;
		count = 15U;
	}

	/* Reads the code and finds run_before. */
	error = cavlc_find(bits, run_before[row - 1U], count, run);
	if (error != 0)
		return error;

	/* Succeeded: run_before. */
	return 0;
}
