/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the level limits the software H.264 decoder shares with the
 * encoder (h264sw_levels and h264sw_level_find of h264sw-ps.h): every row
 * and column of Table A-1 against a copy typed here from the image of the
 * table's page, the order of the rows, and how level 1b is found
 * (level_idc 9, or 11 with constraint_set3_flag in profiles 66, 77 and 88).
 */

#include "check.h"

#include <stddef.h>

#include "userland/desktop/libmedia/h264sw-ps.h"

/*
 * Table A-1, each row: level_idc, MaxMBPS, MaxFS, MaxDpbMbs, MaxBR,
 * MaxCPB, MaxVmvR, MinCR, MaxMvsPer2Mb (0 for "-").
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table A-1 (read from the image of the page).
 */
static const unsigned long table_a1[20][9] = {
	{ 10, 1485, 99, 396, 64, 175, 64, 2, 0 },
	{ 9, 1485, 99, 396, 128, 350, 64, 2, 0 },
	{ 11, 3000, 396, 900, 192, 500, 128, 2, 0 },
	{ 12, 6000, 396, 2376, 384, 1000, 128, 2, 0 },
	{ 13, 11880, 396, 2376, 768, 2000, 128, 2, 0 },
	{ 20, 11880, 396, 2376, 2000, 2000, 128, 2, 0 },
	{ 21, 19800, 792, 4752, 4000, 4000, 256, 2, 0 },
	{ 22, 20250, 1620, 8100, 4000, 4000, 256, 2, 0 },
	{ 30, 40500, 1620, 8100, 10000, 10000, 256, 2, 32 },
	{ 31, 108000, 3600, 18000, 14000, 14000, 512, 4, 16 },
	{ 32, 216000, 5120, 20480, 20000, 20000, 512, 4, 16 },
	{ 40, 245760, 8192, 32768, 20000, 25000, 512, 4, 16 },
	{ 41, 245760, 8192, 32768, 50000, 62500, 512, 2, 16 },
	{ 42, 522240, 8704, 34816, 50000, 62500, 512, 2, 16 },
	{ 50, 589824, 22080, 110400, 135000, 135000, 512, 2, 16 },
	{ 51, 983040, 36864, 184320, 240000, 240000, 512, 2, 16 },
	{ 52, 2073600, 36864, 184320, 240000, 240000, 512, 2, 16 },
	{ 60, 4177920, 139264, 696320, 240000, 240000, 8192, 2, 16 },
	{ 61, 8355840, 139264, 696320, 480000, 480000, 8192, 2, 16 },
	{ 62, 16711680, 139264, 696320, 800000, 800000, 8192, 2, 16 }
};

/* Forward declarations of the static functions. */
static void check_rows(void);
static void check_find(void);

int
main(void)
{
	check_rows();
	check_find();
	return check_finish("levels");
}

/* Checks every column of every row, and that MaxMBPS and MaxFS never fall from one level to the next. */
static void
check_rows(void)
{
	const struct h264sw_level *row;
	unsigned index;

	CHECK(H264SW_LEVELS == 20U);
	for (index = 0; index < H264SW_LEVELS; index++) {
		row = &h264sw_levels[index];
		CHECK(row->level_idc == table_a1[index][0]);
		CHECK(row->max_mbps == table_a1[index][1]);
		CHECK(row->max_fs == table_a1[index][2]);
		CHECK(row->max_dpb_mbs == table_a1[index][3]);
		CHECK(row->max_br == table_a1[index][4]);
		CHECK(row->max_cpb == table_a1[index][5]);
		CHECK(row->max_vmv_r == table_a1[index][6]);
		CHECK(row->min_cr == table_a1[index][7]);
		CHECK(row->max_mvs_per_2mb == table_a1[index][8]);

		/* The rows go up in level: no limit of picture size or rate falls. */
		if (index > 0U) {
			CHECK(row->max_mbps >= h264sw_levels[index - 1U].max_mbps);
			CHECK(row->max_fs >= h264sw_levels[index - 1U].max_fs);
			CHECK(row->max_dpb_mbs >= h264sw_levels[index - 1U].max_dpb_mbs);
		}
	}
}

/* Checks h264sw_level_find: every row by its own level_idc, level 1b both ways, and level_idc values not in the table. */
static void
check_find(void)
{
	const struct h264sw_level *row;
	unsigned index;

	/* Every row is found by its own key in a High profile stream (where 11 is always level 1.1). */
	for (index = 0; index < H264SW_LEVELS; index++) {
		row = h264sw_level_find(H264SW_PROFILE_HIGH, h264sw_levels[index].level_idc, 0);
		CHECK(row == &h264sw_levels[index]);
	}

	/* Level 1b sent as 9 in any profile. */
	CHECK(h264sw_level_find(H264SW_PROFILE_BASELINE, 9U, 0) == &h264sw_levels[1]);
	CHECK(h264sw_level_find(H264SW_PROFILE_HIGH, 9U, 1) == &h264sw_levels[1]);

	/* Level 1b sent as 11 with constraint_set3_flag in profiles 66, 77 and 88. */
	CHECK(h264sw_level_find(H264SW_PROFILE_BASELINE, 11U, 1) == &h264sw_levels[1]);
	CHECK(h264sw_level_find(H264SW_PROFILE_MAIN, 11U, 1) == &h264sw_levels[1]);
	CHECK(h264sw_level_find(H264SW_PROFILE_EXTENDED, 11U, 1) == &h264sw_levels[1]);

	/* Level 1.1 without the flag, and with it in the High profiles (where the flag means another thing). */
	CHECK(h264sw_level_find(H264SW_PROFILE_BASELINE, 11U, 0) == &h264sw_levels[2]);
	CHECK(h264sw_level_find(H264SW_PROFILE_HIGH, 11U, 1) == &h264sw_levels[2]);
	CHECK(h264sw_level_find(H264SW_PROFILE_HIGH10, 11U, 1) == &h264sw_levels[2]);

	/* The flag only matters for 11. */
	CHECK(h264sw_level_find(H264SW_PROFILE_MAIN, 30U, 1) == &h264sw_levels[8]);

	/* level_idc values that are no level. */
	CHECK(h264sw_level_find(H264SW_PROFILE_HIGH, 0U, 0) == NULL);
	CHECK(h264sw_level_find(H264SW_PROFILE_HIGH, 14U, 0) == NULL);
	CHECK(h264sw_level_find(H264SW_PROFILE_HIGH, 63U, 0) == NULL);
	CHECK(h264sw_level_find(H264SW_PROFILE_HIGH, 255U, 0) == NULL);
}
