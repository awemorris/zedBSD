/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant tables of the software H.264 decoder that several parts read.
 */

#include "h264sw-tables.h"

#include <errno.h>
#include <stddef.h>

/*
 * The 4x4 zig-zag scan (Table 8-13, the zig-zag column): for scan position idx 0..15, the raster position 4 * i + j of coefficient cij, where i is the row (vertical frequency) and j the column (horizontal frequency), as 8.5.6 and 8.5.12 use them: idx 1 is c01, the coefficient right of the DC one.
 *
 * Values referenced: ITU-T H.264 (08/2021) Table 8-13
 * (the zig-zag row: c00 c01 c10 c20 c11 c02 c03 c12 c21 c30 c31 c22 c13 c23 c32 c33,
 * written here as 4 * i + j).
 * It is read-only and lives as long as the program.
 */
const uint8_t h264sw_zigzag4x4[16] = {
	0, 1, 4, 8,
	5, 2, 3, 6,
	9, 12, 13, 10,
	7, 11, 14, 15
};

/*
 * The 8x8 zig-zag scan (Table 8-14, the 8x8 zig-zag column): for idx 0..63, the raster position 8 * i + j of cij (i row, j column) as in 8.5.7.
 *
 * Values referenced: ITU-T H.264 (08/2021) Table 8-14
 * (the 8x8 zig-zag row, c00 c01 c10 c20 c11 c02 ... c77, written here as 8 * i + j).
 * It is read-only and lives as long as the program.
 */
const uint8_t h264sw_zigzag8x8[64] = {
	0, 1, 8, 16, 9, 2, 3, 10,
	17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46,
	53, 60, 61, 54, 47, 55, 62, 63
};

/*
 * The default 4x4 scaling lists, in the order they are transmitted (the scan order idx, as Table 7-3 lists them): [0] Default_4x4_Intra, [1] Default_4x4_Inter.
 *
 * Values referenced: ITU-T H.264 (08/2021) Table 7-3
 * (Default_4x4_Intra[ idx ] and Default_4x4_Inter[ idx ], idx 0..15).
 * It is read-only and lives as long as the program.
 */
const uint8_t h264sw_default4x4[2][16] = {
	{
		6, 13, 13, 20, 20, 20, 28, 28,
		28, 28, 32, 32, 32, 37, 37, 42
	},
	{
		10, 14, 14, 20, 20, 20, 24, 24,
		24, 24, 27, 27, 27, 30, 30, 34
	}
};

/*
 * The default 8x8 scaling lists, in the order they are transmitted (the scan order idx, as Table 7-4 lists them): [0] Default_8x8_Intra, [1] Default_8x8_Inter.
 *
 * Values referenced: ITU-T H.264 (08/2021) Table 7-4
 * (Default_8x8_Intra[ idx ] and Default_8x8_Inter[ idx ], idx 0..63).
 * It is read-only and lives as long as the program.
 */
const uint8_t h264sw_default8x8[2][64] = {
	{
		6, 10, 10, 13, 11, 13, 16, 16,
		16, 16, 18, 18, 18, 18, 18, 23,
		23, 23, 23, 23, 23, 25, 25, 25,
		25, 25, 25, 25, 27, 27, 27, 27,
		27, 27, 27, 27, 29, 29, 29, 29,
		29, 29, 29, 31, 31, 31, 31, 31,
		31, 33, 33, 33, 33, 33, 36, 36,
		36, 36, 38, 38, 38, 40, 40, 42
	},
	{
		9, 13, 13, 15, 13, 15, 17, 17,
		17, 17, 19, 19, 19, 19, 19, 21,
		21, 21, 21, 21, 21, 22, 22, 22,
		22, 22, 22, 22, 24, 24, 24, 24,
		24, 24, 24, 24, 25, 25, 25, 25,
		25, 25, 25, 27, 27, 27, 27, 27,
		27, 28, 28, 28, 28, 28, 30, 30,
		30, 30, 32, 32, 32, 33, 33, 35
	}
};

/*
 * QPC as a function of qPI (Table 8-15), for qPI 0..51.  The table's qPI < 30 column is written out (QPC equal to qPI) so the index is qPI itself.
 *
 * Values referenced: ITU-T H.264 (08/2021) Table 8-15
 * (qPI below 30 gives QPC equal to qPI; qPI 30..51 gives
 * 29 30 31 32 32 33 34 34 35 35 36 36 37 37 37 38 38 38 39 39 39 39).
 * It is read-only and lives as long as the program.
 */
const uint8_t h264sw_chroma_qp[52] = {
	0, 1, 2, 3, 4, 5, 6, 7,
	8, 9, 10, 11, 12, 13, 14, 15,
	16, 17, 18, 19, 20, 21, 22, 23,
	24, 25, 26, 27, 28, 29, 29, 30,
	31, 32, 32, 33, 34, 34, 35, 35,
	36, 36, 37, 37, 37, 38, 38, 38,
	39, 39, 39, 39
};

/*
 * The normalization of the 4x4 scaling function: v of equation 8-315, [m][k] for m = qP % 6 and k the column of v (k 0 for (i % 2, j % 2) equal to (0, 0), 1 for (1, 1), 2 otherwise, as equation 8-314 selects).
 *
 * Values referenced: ITU-T H.264 (08/2021) equation 8-315
 * (the 6x3 matrix v, rows m 0..5).
 * It is read-only and lives as long as the program.
 */
const uint8_t h264sw_norm4x4[6][3] = {
	{ 10, 16, 13 },
	{ 11, 18, 14 },
	{ 13, 20, 16 },
	{ 14, 23, 18 },
	{ 16, 25, 20 },
	{ 18, 29, 23 }
};

/*
 * The normalization of the 8x8 scaling function: v of equation 8-318, [m][k] for m = qP % 6 and k 0..5 the column of v that equation 8-317 selects for (i, j).
 *
 * Values referenced: ITU-T H.264 (08/2021) equation 8-318
 * (the 6x6 matrix v, rows m 0..5).
 * It is read-only and lives as long as the program.
 */
const uint8_t h264sw_norm8x8[6][6] = {
	{ 20, 18, 32, 19, 25, 24 },
	{ 22, 19, 35, 21, 28, 26 },
	{ 26, 23, 42, 24, 33, 31 },
	{ 28, 25, 45, 26, 35, 33 },
	{ 32, 28, 51, 30, 40, 38 },
	{ 36, 32, 58, 34, 46, 43 }
};
