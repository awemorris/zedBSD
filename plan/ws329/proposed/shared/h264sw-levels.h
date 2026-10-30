/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The level limits of H.264 (Table A-1 and the parts of A.3 the software
 * decoder and encoder use), shared by both: the decoder derives the size
 * of its decoded picture buffer from them (A.3.1 h)), the encoder chooses
 * the lowest level a configuration fits and keeps its motion vectors and
 * picture sizes inside it.
 *
 * The decoder's private table in h264sw-ps.c (MaxDpbMbs only) is
 * replaced by this one, whose definition in h264sw-levels.c carries the
 * "Values referenced: ITU-T H.264 (08/2021) Table A-1" comment.
 */

#ifndef LIBMEDIA_H264SW_LEVELS_H
#define LIBMEDIA_H264SW_LEVELS_H

#include "h264sw.h"

/* The level_idc of level 1b when it is not sent as 11 with constraint_set3_flag (A.3.1). */
#define H264SW_LEVEL_1B 9U

/* The rows of Table A-1, levels 1, 1b, 1.1 ... 6.2. */
#define H264SW_LEVELS 20U

/*
 * One row of Table A-1.
 *
 * level_idc is ten times the level number (9 for level 1b, the row's own
 * key; level 1b sent as level_idc 11 with constraint_set3_flag 1 is found
 * by h264sw_level_find).  max_mbps MaxMBPS (macroblocks per second),
 * max_fs MaxFS (macroblocks), max_dpb_mbs MaxDpbMbs, max_br MaxBR and
 * max_cpb MaxCPB in units of 1000 bits/s and 1000 bits (multiplied by
 * cpbBrVclFactor or cpbBrNalFactor of Table A-2 for the profile: 1000 and
 * 1200 for Baseline, Constrained Baseline, Main and Extended, 1250 and
 * 1500 for High), max_vmv_r MaxVmvR (the vertical motion vector component
 * range: -MaxVmvR .. MaxVmvR - 0.25 luma samples), min_cr MinCR, and
 * max_mvs_per_2mb MaxMvsPer2Mb (0 where the table has "-").
 */
struct h264sw_level {
	unsigned level_idc;
	uint32_t max_mbps;
	uint32_t max_fs;
	uint32_t max_dpb_mbs;
	uint32_t max_br;
	uint32_t max_cpb;
	unsigned max_vmv_r;
	unsigned min_cr;
	unsigned max_mvs_per_2mb;
};

/* Table A-1, in increasing order of level (1, 1b, 1.1, 1.2, 1.3, 2, ... 6.2). */
extern const struct h264sw_level h264sw_levels[H264SW_LEVELS];

/*
 * Finds the row of a level.
 *
 * level_idc as sent; constraint_set3 the SPS's constraint_set3_flag;
 * profile_idc the SPS's.  Level 1b is level_idc 9, or level_idc 11 with
 * constraint_set3_flag 1 in the Baseline, Constrained Baseline, Main and
 * Extended profiles (66, 77, 88; A.3.1, A.3.2).  Returns the row, or NULL
 * for a level_idc not in the table.
 */
const struct h264sw_level *h264sw_level_find(unsigned profile_idc, unsigned level_idc, int constraint_set3);

#endif
