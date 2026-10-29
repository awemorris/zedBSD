/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the shared constant tables of the software H.264 decoder: the
 * zig-zag scans against Figures 8-8 and 8-9 and the shape of a zig-zag
 * walk, the default scaling lists in run-length form, the chroma
 * quantization parameter table, and the normalization matrices.
 *
 * Every expected value is written in a different form from the table in
 * the source (the inverse scan of the figures, runs of equal values,
 * properties), so one misreading of a table does not pass both.
 */

#include "check.h"

#include "userland/desktop/libmedia/h264sw-tables.h"

/* One run of equal values in a default list: the value and how many entries have it. */
struct run {
	unsigned value;
	unsigned count;
};

/*
 * Figure 8-8 (a): the zig-zag scan position idx at each raster position
 * 4 * row + column of a 4x4 block.
 */
static const unsigned figure_4x4[16] = {
	0, 1, 5, 6,
	2, 4, 7, 12,
	3, 8, 11, 13,
	9, 10, 14, 15
};

/*
 * Figure 8-9 (a): the zig-zag scan position idx at each raster position
 * 8 * row + column of an 8x8 block.
 */
static const unsigned figure_8x8[64] = {
	0, 1, 5, 6, 14, 15, 27, 28,
	2, 4, 7, 13, 16, 26, 29, 42,
	3, 8, 12, 17, 25, 30, 41, 43,
	9, 11, 18, 24, 31, 40, 44, 53,
	10, 19, 23, 32, 39, 45, 52, 54,
	20, 22, 33, 38, 46, 51, 55, 60,
	21, 34, 37, 47, 50, 56, 59, 61,
	35, 36, 48, 49, 57, 58, 62, 63
};

/* Table 7-3 as runs: Default_4x4_Intra. */
static const struct run runs_4x4_intra[] = {
	{ 6U, 1U }, { 13U, 2U }, { 20U, 3U }, { 28U, 4U }, { 32U, 3U }, { 37U, 2U }, { 42U, 1U }
};

/* Table 7-3 as runs: Default_4x4_Inter. */
static const struct run runs_4x4_inter[] = {
	{ 10U, 1U }, { 14U, 2U }, { 20U, 3U }, { 24U, 4U }, { 27U, 3U }, { 30U, 2U }, { 34U, 1U }
};

/* Table 7-4 as runs: Default_8x8_Intra. */
static const struct run runs_8x8_intra[] = {
	{ 6U, 1U }, { 10U, 2U }, { 13U, 1U }, { 11U, 1U }, { 13U, 1U }, { 16U, 4U }, { 18U, 5U },
	{ 23U, 6U }, { 25U, 7U }, { 27U, 8U }, { 29U, 7U }, { 31U, 6U }, { 33U, 5U }, { 36U, 4U },
	{ 38U, 3U }, { 40U, 2U }, { 42U, 1U }
};

/* Table 7-4 as runs: Default_8x8_Inter. */
static const struct run runs_8x8_inter[] = {
	{ 9U, 1U }, { 13U, 2U }, { 15U, 1U }, { 13U, 1U }, { 15U, 1U }, { 17U, 4U }, { 19U, 5U },
	{ 21U, 6U }, { 22U, 7U }, { 24U, 8U }, { 25U, 7U }, { 27U, 6U }, { 28U, 5U }, { 30U, 4U },
	{ 32U, 3U }, { 33U, 2U }, { 35U, 1U }
};

/* Equation 8-315 and 8-318, written as the values a reader of the matrices sees, row m 0..5. */
static const unsigned v4_expected[6][3] = {
	{ 10U, 16U, 13U }, { 11U, 18U, 14U }, { 13U, 20U, 16U },
	{ 14U, 23U, 18U }, { 16U, 25U, 20U }, { 18U, 29U, 23U }
};
static const unsigned v8_expected[6][6] = {
	{ 20U, 18U, 32U, 19U, 25U, 24U }, { 22U, 19U, 35U, 21U, 28U, 26U },
	{ 26U, 23U, 42U, 24U, 33U, 31U }, { 28U, 25U, 45U, 26U, 35U, 33U },
	{ 32U, 28U, 51U, 30U, 40U, 38U }, { 36U, 32U, 58U, 34U, 46U, 43U }
};

/* Expands runs into a list of the given size and compares it with a table; returns 1 when equal. */
static int
list_matches_runs(
	const uint8_t *list,
	unsigned size,
	const struct run *runs,
	unsigned run_count)
{
	unsigned run_index;
	unsigned entry;
	unsigned position;

	/* Walks the runs, comparing every entry the run covers. */
	position = 0U;
	for (run_index = 0U; run_index < run_count; run_index++) {
		for (entry = 0U; entry < runs[run_index].count; entry++) {
			if (position >= size)
				return 0;
			if (list[position] != runs[run_index].value)
				return 0;
			position++;
		}
	}

	/* The runs must cover the whole list and nothing more. */
	if (position != size)
		return 0;

	/* Succeeded: every entry matched. */
	return 1;
}

/* Checks a scan of side x side against its figure, as a permutation and as a connected zig-zag walk. */
static void
check_scan(
	const uint8_t *scan,
	unsigned side,
	const unsigned *figure)
{
	unsigned count;
	unsigned index;
	unsigned raster;
	unsigned seen[64];
	int row_step;
	int column_step;
	int diagonal_step;
	unsigned previous_diagonal;
	unsigned diagonal;

	/* The figure gives, for each raster position, its scan index: the scan is the inverse of that map. */
	count = side * side;
	for (raster = 0U; raster < count; raster++)
		CHECK(scan[figure[raster]] == raster);

	/* A scan visits every raster position exactly once. */
	for (index = 0U; index < count; index++)
		seen[index] = 0U;
	for (index = 0U; index < count; index++) {
		CHECK(scan[index] < count);
		if (scan[index] < count)
			seen[scan[index]]++;
	}
	for (index = 0U; index < count; index++)
		CHECK(seen[index] == 1U);

	/* The walk starts at the DC coefficient and ends in the corner opposite to it. */
	CHECK(scan[0] == 0U);
	CHECK(scan[count - 1U] == count - 1U);

	/* Each step moves to a neighbouring position, and to the same anti-diagonal or the next one. */
	previous_diagonal = 0U;
	for (index = 1U; index < count; index++) {
		row_step = (int)(scan[index] / side) - (int)(scan[index - 1U] / side);
		column_step = (int)(scan[index] % side) - (int)(scan[index - 1U] % side);
		diagonal = (scan[index] / side) + (scan[index] % side);
		diagonal_step = (int)diagonal - (int)previous_diagonal;
		CHECK(row_step >= -1 && row_step <= 1);
		CHECK(column_step >= -1 && column_step <= 1);
		CHECK(diagonal_step == 0 || diagonal_step == 1);
		previous_diagonal = diagonal;
	}
}

/* Checks the two zig-zag scans, including a few entries worked out from Tables 8-13 and 8-14. */
static void
test_scans(
	void)
{
	/* The 4x4 scan against Figure 8-8 (a). */
	check_scan(h264sw_zigzag4x4, 4U, figure_4x4);

	/* The 8x8 scan against Figure 8-9 (a). */
	check_scan(h264sw_zigzag8x8, 8U, figure_8x8);

	/* Table 8-13: idx 1 is c01 (raster 1), idx 2 is c10 (4), idx 3 is c20 (8), idx 9 is c30 (12). */
	CHECK(h264sw_zigzag4x4[1] == 1U);
	CHECK(h264sw_zigzag4x4[2] == 4U);
	CHECK(h264sw_zigzag4x4[3] == 8U);
	CHECK(h264sw_zigzag4x4[9] == 12U);

	/* Table 8-14: idx 10 is c40 (raster 32), idx 14 is c04 (4), idx 20 is c50 (40), idx 35 is c70 (56). */
	CHECK(h264sw_zigzag8x8[10] == 32U);
	CHECK(h264sw_zigzag8x8[14] == 4U);
	CHECK(h264sw_zigzag8x8[20] == 40U);
	CHECK(h264sw_zigzag8x8[35] == 56U);
}

/* Checks the four default scaling lists value by value, through their run-length form. */
static void
test_defaults(
	void)
{
	/* Default_4x4_Intra and Default_4x4_Inter (Table 7-3). */
	CHECK(list_matches_runs(h264sw_default4x4[0], 16U, runs_4x4_intra, (unsigned)(sizeof(runs_4x4_intra) / sizeof(runs_4x4_intra[0]))));
	CHECK(list_matches_runs(h264sw_default4x4[1], 16U, runs_4x4_inter, (unsigned)(sizeof(runs_4x4_inter) / sizeof(runs_4x4_inter[0]))));

	/* Default_8x8_Intra and Default_8x8_Inter (Table 7-4). */
	CHECK(list_matches_runs(h264sw_default8x8[0], 64U, runs_8x8_intra, (unsigned)(sizeof(runs_8x8_intra) / sizeof(runs_8x8_intra[0]))));
	CHECK(list_matches_runs(h264sw_default8x8[1], 64U, runs_8x8_inter, (unsigned)(sizeof(runs_8x8_inter) / sizeof(runs_8x8_inter[0]))));
}

/* Checks QPC of Table 8-15: the ends, the steps of the second column and the monotone shape. */
static void
test_chroma_qp(
	void)
{
	unsigned qpi;

	/* Below 30 QPC equals qPI. */
	for (qpi = 0U; qpi < 30U; qpi++)
		CHECK(h264sw_chroma_qp[qpi] == qpi);

	/* The table's own anchors: 29 to 29, 30 to 29, 51 to 39. */
	CHECK(h264sw_chroma_qp[29] == 29U);
	CHECK(h264sw_chroma_qp[30] == 29U);
	CHECK(h264sw_chroma_qp[51] == 39U);

	/* Entries read off the table by column: 31 to 30, 32 to 31, 33 to 32, 34 to 32, 35 to 33, 36 to 34, 37 to 34. */
	CHECK(h264sw_chroma_qp[31] == 30U);
	CHECK(h264sw_chroma_qp[32] == 31U);
	CHECK(h264sw_chroma_qp[33] == 32U);
	CHECK(h264sw_chroma_qp[34] == 32U);
	CHECK(h264sw_chroma_qp[35] == 33U);
	CHECK(h264sw_chroma_qp[36] == 34U);
	CHECK(h264sw_chroma_qp[37] == 34U);

	/* 38 and 39 to 35, 40 and 41 to 36, 42 to 37, 43 to 37, 44 to 37, 45 to 38, 46 to 38, 47 to 38, 48 to 39, 49 to 39, 50 to 39. */
	CHECK(h264sw_chroma_qp[38] == 35U);
	CHECK(h264sw_chroma_qp[39] == 35U);
	CHECK(h264sw_chroma_qp[40] == 36U);
	CHECK(h264sw_chroma_qp[41] == 36U);
	CHECK(h264sw_chroma_qp[42] == 37U);
	CHECK(h264sw_chroma_qp[43] == 37U);
	CHECK(h264sw_chroma_qp[44] == 37U);
	CHECK(h264sw_chroma_qp[45] == 38U);
	CHECK(h264sw_chroma_qp[46] == 38U);
	CHECK(h264sw_chroma_qp[47] == 38U);
	CHECK(h264sw_chroma_qp[48] == 39U);
	CHECK(h264sw_chroma_qp[49] == 39U);
	CHECK(h264sw_chroma_qp[50] == 39U);

	/* The shape: never decreasing, never more than one step, never above 39. */
	for (qpi = 1U; qpi < 52U; qpi++) {
		CHECK(h264sw_chroma_qp[qpi] >= h264sw_chroma_qp[qpi - 1U]);
		CHECK(h264sw_chroma_qp[qpi] <= h264sw_chroma_qp[qpi - 1U] + 1U);
		CHECK(h264sw_chroma_qp[qpi] <= 39U);
	}
}

/* Checks the normalization matrices of equations 8-315 and 8-318 and their structure. */
static void
test_norm(
	void)
{
	unsigned m;
	unsigned k;

	/* Every entry of the 4x4 matrix v (8-315). */
	for (m = 0U; m < 6U; m++) {
		for (k = 0U; k < 3U; k++)
			CHECK(h264sw_norm4x4[m][k] == v4_expected[m][k]);
	}

	/* Every entry of the 8x8 matrix v (8-318). */
	for (m = 0U; m < 6U; m++) {
		for (k = 0U; k < 6U; k++)
			CHECK(h264sw_norm8x8[m][k] == v8_expected[m][k]);
	}

	/* The step from qP to qP + 1 scales by about 2^(1/6) = 1.122 (rounded values, so 1.05 to 1.25), whatever the column. */
	for (m = 1U; m < 6U; m++) {
		for (k = 0U; k < 3U; k++) {
			CHECK(h264sw_norm4x4[m][k] * 100U > h264sw_norm4x4[m - 1U][k] * 105U);
			CHECK(h264sw_norm4x4[m][k] * 100U < h264sw_norm4x4[m - 1U][k] * 125U);
		}
		for (k = 0U; k < 6U; k++) {
			CHECK(h264sw_norm8x8[m][k] * 100U > h264sw_norm8x8[m - 1U][k] * 105U);
			CHECK(h264sw_norm8x8[m][k] * 100U < h264sw_norm8x8[m - 1U][k] * 125U);
		}
	}

	/* The first column of the 8x8 matrix is exactly twice the first column of the 4x4 one. */
	for (m = 0U; m < 6U; m++)
		CHECK(h264sw_norm8x8[m][0] == 2U * h264sw_norm4x4[m][0]);
}

int
main(
	void)
{
	/* Runs every group of checks and prints the summary line. */
	test_scans();
	test_defaults();
	test_chroma_qp();
	test_norm();

	/* Succeeded when every check passed. */
	return check_finish("tables");
}
