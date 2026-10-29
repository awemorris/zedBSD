/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The macroblock types and the neighbours of the software H.264 decoder
 * (Tables 7-11 to 7-18, 6.4).
 */

#include "h264sw-mb.h"

#include <errno.h>
#include <stddef.h>

/*
 * The position of each luma4x4BlkIdx in its macroblock, in luma samples (6.4.3): x and y.
 *
 * The values are not entered yet; the block will name its table.
 */
const uint8_t h264sw_blk4x4_x[16] = { 0 };

/*
 * The position of each luma4x4BlkIdx in its macroblock, in luma samples (6.4.3): x and y.
 *
 * The values are not entered yet; the block will name its table.
 */
const uint8_t h264sw_blk4x4_y[16] = { 0 };

/*
 * Gives the meaning of an mb_type value of a slice type.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-mb.h.
 */
int
h264sw_mb_type_of(
	unsigned slice_type,
	unsigned mb_type,
	struct h264sw_mb_type *type)
{
	UNUSED_PARAMETER(slice_type);
	UNUSED_PARAMETER(mb_type);
	UNUSED_PARAMETER(type);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Gives the meaning of P_Skip (slice_type P) or B_Skip (slice_type B), as inferred by 7.4.4.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-mb.h.
 */
void
h264sw_mb_skip_type(
	unsigned slice_type,
	struct h264sw_mb_type *type)
{
	UNUSED_PARAMETER(slice_type);
	UNUSED_PARAMETER(type);

	/* Does nothing yet. */
	return;
}

/*
 * Gives the meaning of a sub_mb_type value (Table 7-17 for P: 0..3, Table 7-18 for B: 0..12).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-mb.h.
 */
int
h264sw_sub_mb_type_of(
	unsigned slice_type,
	unsigned sub_mb_type,
	struct h264sw_sub_mb_type *type)
{
	UNUSED_PARAMETER(slice_type);
	UNUSED_PARAMETER(sub_mb_type);
	UNUSED_PARAMETER(type);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Finds the neighbouring macroblocks of a macroblock (6.4.8, 6.4.9).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-mb.h.
 */
void
h264sw_mb_neighbours(
	struct h264sw_mb *mbs,
	unsigned width_mbs,
	unsigned addr,
	struct h264sw_neighbours *n)
{
	UNUSED_PARAMETER(mbs);
	UNUSED_PARAMETER(width_mbs);
	UNUSED_PARAMETER(addr);
	UNUSED_PARAMETER(n);

	/* Does nothing yet. */
	return;
}

/*
 * Finds the macroblock that holds a location near the current one (6.4.12.1, Table 6-3).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-mb.h.
 */
struct h264sw_mb *
h264sw_mb_locate(
	const struct h264sw_neighbours *n,
	int xn,
	int yn,
	unsigned max_w,
	unsigned max_h,
	unsigned *xw,
	unsigned *yw)
{
	UNUSED_PARAMETER(n);
	UNUSED_PARAMETER(xn);
	UNUSED_PARAMETER(yn);
	UNUSED_PARAMETER(max_w);
	UNUSED_PARAMETER(max_h);
	UNUSED_PARAMETER(xw);
	UNUSED_PARAMETER(yw);

	/* Reports that nothing is made yet. */
	return NULL;
}

/*
 * Gives the luma4x4BlkIdx of the 4x4 block holding luma location (x, y) of a macroblock, 0..15 each (6.4.13.1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-mb.h.
 */
unsigned
h264sw_blk4x4_at(
	unsigned x,
	unsigned y)
{
	UNUSED_PARAMETER(x);
	UNUSED_PARAMETER(y);

	/* Reports nothing yet. */
	return 0;
}

/*
 * Finds the left (A) or upper (B) neighbour of a luma 4x4 block (6.4.11.4).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-mb.h.
 */
struct h264sw_mb *
h264sw_mb_neighbour_4x4(
	const struct h264sw_neighbours *n,
	unsigned blk,
	int upper,
	unsigned *blk_n)
{
	UNUSED_PARAMETER(n);
	UNUSED_PARAMETER(blk);
	UNUSED_PARAMETER(upper);
	UNUSED_PARAMETER(blk_n);

	/* Reports that nothing is made yet. */
	return NULL;
}

/*
 * As h264sw_mb_neighbour_4x4 for a luma 8x8 block (6.4.11.2): blk is luma8x8BlkIdx, *blk_n the neighbour's.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-mb.h.
 */
struct h264sw_mb *
h264sw_mb_neighbour_8x8(
	const struct h264sw_neighbours *n,
	unsigned blk,
	int upper,
	unsigned *blk_n)
{
	UNUSED_PARAMETER(n);
	UNUSED_PARAMETER(blk);
	UNUSED_PARAMETER(upper);
	UNUSED_PARAMETER(blk_n);

	/* Reports that nothing is made yet. */
	return NULL;
}

/*
 * As h264sw_mb_neighbour_4x4 for a chroma 4x4 block of 4:2:0 (6.4.11.5): blk is chroma4x4BlkIdx 0..3.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-mb.h.
 */
struct h264sw_mb *
h264sw_mb_neighbour_chroma(
	const struct h264sw_neighbours *n,
	unsigned blk,
	int upper,
	unsigned *blk_n)
{
	UNUSED_PARAMETER(n);
	UNUSED_PARAMETER(blk);
	UNUSED_PARAMETER(upper);
	UNUSED_PARAMETER(blk_n);

	/* Reports that nothing is made yet. */
	return NULL;
}
