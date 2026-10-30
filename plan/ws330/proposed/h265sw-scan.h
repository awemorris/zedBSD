/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scanning processes of the software H.265 decoder (6.5): the tile
 * layout and the conversions between the CTB raster scan and the tile
 * scan of a picture (6.5.1), the z-scan order of minimum transform
 * blocks (6.5.2), and the coefficient scans of a block: up-right
 * diagonal (6.5.3), horizontal (6.5.4) and vertical (6.5.5).
 *
 * The coefficient scans are constant tables of this module (the arrays
 * ScanOrder[ log2BlockSize ][ scanIdx ][ sPos ][ sComp ] for the sizes
 * the decoder uses); the unit test derives them again from the clauses'
 * loops and compares.
 */

#ifndef LIBMEDIA_H265SW_SCAN_H
#define LIBMEDIA_H265SW_SCAN_H

#include "h265sw.h"

/* scanIdx (7.4.9.11): up-right diagonal, horizontal, vertical. */
#define H265SW_SCAN_DIAG 0U
#define H265SW_SCAN_HOR 1U
#define H265SW_SCAN_VER 2U

/*
 * The tile layout of a picture (6.5.1): columns and rows tiles, col_bd[i]
 * and row_bd[j] the boundaries in CTBs (colBd and rowBd, 6-5 and 6-6;
 * col_bd[columns] is PicWidthInCtbsY).  One tile covering the picture
 * when tiles_enabled_flag is 0.
 */
struct h265sw_tiles {
	unsigned columns;
	unsigned rows;
	unsigned col_bd[H265SW_MAX_TILE_COLS + 1U];
	unsigned row_bd[H265SW_MAX_TILE_ROWS + 1U];
};

/*
 * Derives a picture's tile layout (6-3 to 6-6).
 *
 * width_ctbs and height_ctbs are PicWidthInCtbsY and PicHeightInCtbsY
 * (1..1056); tiles_enabled, columns and rows (1..H265SW_MAX_TILE_COLS and
 * ..._ROWS), uniform, and column_width[0..columns - 1) and
 * row_height[0..rows - 1) in CTBs come from the PPS.  Returns 0 and
 * *out; EINVAL when columns exceeds width_ctbs, rows exceeds
 * height_ctbs, or the explicit widths or heights leave nothing for the
 * last column or row (the PPS and SPS do not fit together).
 */
int h265sw_scan_tiles(unsigned width_ctbs, unsigned height_ctbs, int tiles_enabled, unsigned columns, unsigned rows, int uniform, const unsigned *column_width, const unsigned *row_height, struct h265sw_tiles *out);

/*
 * Fills the CTB address conversions of a picture (6-7, 6-8, 6-9):
 * rs_to_ts[ctbAddrRs] (CtbAddrRsToTs), ts_to_rs[ctbAddrTs]
 * (CtbAddrTsToRs) and tile_id[ctbAddrTs] (TileId), each of width_ctbs *
 * height_ctbs entries, owned by the caller.
 */
void h265sw_scan_ctb_maps(const struct h265sw_tiles *tiles, unsigned width_ctbs, unsigned height_ctbs, uint32_t *rs_to_ts, uint32_t *ts_to_rs, uint16_t *tile_id);

/*
 * Returns the scan order of a block of 1 << log2_size samples on each
 * side (log2_size 0..3; 0 is the single position): the array of
 * ( 1 << log2_size )^2 positions, each { x, y } (sComp 0 and 1), in scan
 * position order.  scan_idx is H265SW_SCAN_*; all three scans exist for
 * every log2_size 0..3 (7.4.3.2.1 builds them all): residual coding
 * uses log2_size 2 for the positions inside a 4x4 sub-block and
 * log2TrafoSize - 2 (0..3) for the order of the sub-blocks, so an 8x8
 * transform block with scanIdx 1 or 2 orders its four sub-blocks with
 * the 2x2 horizontal or vertical scan (the 2x2 horizontal differs from
 * the diagonal).
 *
 * Values referenced: ITU-T H.265 (01/2026) 6.5.3 to 6.5.5 (the arrays
 * the loops of those clauses produce).
 */
const uint8_t (*h265sw_scan_order(unsigned log2_size, unsigned scan_idx))[2];

#endif
