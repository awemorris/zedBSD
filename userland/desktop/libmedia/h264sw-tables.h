/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The constant tables of the software H.264 decoder that more than one
 * part reads: the scans, the default scaling lists, the chroma
 * quantization parameter and the normalization of the scaling functions.
 * The tables that only one part reads (the CAVLC codes, the CABAC
 * contexts, the deblocking thresholds) live in that part.
 *
 * Each table is data of the specification; its definition in
 * h264sw-tables.c carries, in the comment of its block, the table or the
 * equation it is taken from.
 */

#ifndef LIBMEDIA_H264SW_TABLES_H
#define LIBMEDIA_H264SW_TABLES_H

#include "h264sw.h"

/*
 * The 4x4 zig-zag scan (Table 8-13, the zig-zag column): for scan
 * position idx 0..15, the raster position 4 * i + j of coefficient cij,
 * where i is the row (vertical frequency) and j the column (horizontal
 * frequency), as 8.5.6 and 8.5.12 use them: idx 1 is c01, the coefficient
 * right of the DC one.  The field scan is not used (frames only).
 */
extern const uint8_t h264sw_zigzag4x4[16];

/*
 * The 8x8 zig-zag scan (Table 8-14, the 8x8 zig-zag column): for idx
 * 0..63, the raster position 8 * i + j of cij (i row, j column) as in
 * 8.5.7.
 */
extern const uint8_t h264sw_zigzag8x8[64];

/*
 * The default scaling lists, in the order they are transmitted (the scan
 * order idx, as Tables 7-3 and 7-4 list them): [0] the intra list
 * (Default_4x4_Intra, Default_8x8_Intra), [1] the inter list
 * (Default_4x4_Inter, Default_8x8_Inter).
 */
extern const uint8_t h264sw_default4x4[2][16];
extern const uint8_t h264sw_default8x8[2][64];

/*
 * QPC as a function of qPI (Table 8-15), for qPI 0..51.  The table's
 * qPI < 30 column is written out (QPC equal to qPI) so the index is qPI
 * itself.
 */
extern const uint8_t h264sw_chroma_qp[52];

/*
 * The normalization of the 4x4 scaling function: v of equation 8-315,
 * [m][k] for m = qP % 6 and k the column of v (k 0 for (i % 2, j % 2)
 * equal to (0, 0), 1 for (1, 1), 2 otherwise, as equation 8-314 selects).
 */
extern const uint8_t h264sw_norm4x4[6][3];

/*
 * The normalization of the 8x8 scaling function: v of equation 8-318,
 * [m][k] for m = qP % 6 and k 0..5 the column of v that equation 8-317
 * selects for (i, j).
 */
extern const uint8_t h264sw_norm8x8[6][6];

#endif
