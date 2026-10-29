/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the CABAC initialization table (h264sw-cabac-init) against
 * values read by hand from Tables 9-12 to 9-24 of ITU-T H.264 (08/2021)
 * and against the structure the header promises.  With --dump it prints
 * every (m, n) pair, one "ctxIdx set m n" line each, for the independent
 * cross-check in cabac-init-extract.py.
 */

#include "check.h"

#include <stdint.h>
#include <string.h>

#include "userland/desktop/libmedia/h264sw-cabac-init.h"

/*
 * A pair read from the specification: the ctxIdx and the (m, n) of the
 * I set and of cabac_init_idc 0, 1 and 2.
 */
struct spot {
	unsigned ctx;
	int pair[H264SW_CABAC_INIT_SETS][2];
};

/*
 * The first and last ctxIdx of every table, read from the page images of
 * the specification.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Tables 9-12, 9-13, 9-14, 9-15, 9-16, 9-17, 9-18, 9-19, 9-20, 9-21,
 * 9-22, 9-23 and 9-24.
 */
static const struct spot spots[] = {
	/* Table 9-12: one column for every slice type. */
	{ 0U, { { 20, -15 }, { 20, -15 }, { 20, -15 }, { 20, -15 } } },
	{ 10U, { { 7, 51 }, { 7, 51 }, { 7, 51 }, { 7, 51 } } },

	/* Table 9-13: cabac_init_idc 0, 1 and 2 only. */
	{ 11U, { { 0, 0 }, { 23, 33 }, { 22, 25 }, { 29, 16 } } },
	{ 23U, { { 0, 0 }, { 17, 50 }, { 10, 54 }, { 14, 57 } } },

	/* Table 9-14: the m of some cells is written over two lines. */
	{ 24U, { { 0, 0 }, { 18, 64 }, { 26, 34 }, { 20, 40 } } },
	{ 30U, { { 0, 0 }, { -46, 127 }, { -45, 127 }, { -32, 127 } } },
	{ 31U, { { 0, 0 }, { -20, 104 }, { -15, 101 }, { -22, 117 } } },
	{ 39U, { { 0, 0 }, { 9, 45 }, { 8, 43 }, { 4, 55 } } },

	/* Table 9-15. */
	{ 40U, { { 0, 0 }, { -3, 69 }, { -2, 69 }, { -11, 89 } } },
	{ 53U, { { 0, 0 }, { 0, 88 }, { -5, 95 }, { -1, 101 } } },

	/* Table 9-16, ctxIdx 54 to 59 (na for I slices). */
	{ 54U, { { 0, 0 }, { -7, 67 }, { -1, 66 }, { 3, 55 } } },
	{ 59U, { { 0, 0 }, { 1, 58 }, { 0, 61 }, { 1, 60 } } },

	/* Table 9-17: one column for every slice type. */
	{ 60U, { { 0, 41 }, { 0, 41 }, { 0, 41 }, { 0, 41 } } },
	{ 69U, { { 3, 62 }, { 3, 62 }, { 3, 62 }, { 3, 62 } } },

	/* Table 9-18. */
	{ 70U, { { 0, 11 }, { 0, 45 }, { 13, 15 }, { 7, 34 } } },
	{ 104U, { { -22, 125 }, { -8, 88 }, { -9, 88 }, { -13, 98 } } },

	/* Table 9-19. */
	{ 105U, { { -7, 93 }, { -2, 85 }, { -13, 103 }, { -4, 86 } } },
	{ 165U, { { 12, 72 }, { 9, 69 }, { 0, 89 }, { -22, 127 } } },

	/* Table 9-20. */
	{ 166U, { { 24, 0 }, { 11, 28 }, { 4, 45 }, { 4, 39 } } },
	{ 226U, { { 2, 97 }, { -9, 108 }, { -10, 116 }, { -24, 127 } } },

	/* Table 9-21. */
	{ 227U, { { -3, 71 }, { -6, 76 }, { -23, 112 }, { -24, 115 } } },
	{ 275U, { { -14, 97 }, { -8, 85 }, { -4, 78 }, { -10, 87 } } },

	/* Table 9-22. */
	{ 277U, { { -6, 93 }, { -13, 106 }, { -21, 126 }, { -22, 127 } } },
	{ 337U, { { 12, 70 }, { 26, 43 }, { 18, 50 }, { 25, 42 } } },

	/* Table 9-23. */
	{ 338U, { { 15, 6 }, { 14, 11 }, { 19, -6 }, { 17, -13 } } },
	{ 398U, { { 19, 66 }, { 11, 86 }, { 11, 83 }, { 25, 61 } } },

	/* Table 9-16, ctxIdx 399 to 401 (I slices and cabac_init_idc 0, 1 and 2). */
	{ 399U, { { 31, 21 }, { 12, 40 }, { 25, 32 }, { 21, 33 } } },
	{ 401U, { { 25, 50 }, { 14, 59 }, { 21, 54 }, { 17, 61 } } },

	/* Table 9-24. */
	{ 402U, { { -17, 120 }, { -4, 79 }, { -5, 85 }, { -3, 78 } } },
	{ 459U, { { 14, 67 }, { 42, 62 }, { 47, 57 }, { 20, 64 } } }
};

/* Prints every pair, one "ctxIdx set m n" line each. */
static void dump_pairs(void);

/* Checks the pairs read from the specification by hand. */
static void check_spots(void);

/* Checks the structure the header describes: shared columns, unused pairs and the value range. */
static void check_shape(void);

/* Prints every pair, one "ctxIdx set m n" line each. */
static void
dump_pairs(
	void)
{
	unsigned ctx;
	unsigned set;

	/* Every context variable, then every set of it. */
	for (ctx = 0U;
	     ctx < H264SW_CABAC_CONTEXTS;
	     ctx++) {
		for (set = 0U;
		     set < H264SW_CABAC_INIT_SETS;
		     set++) {
			/* One pair per line for the cross-check. */
			printf("%u %u %d %d\n", ctx, set,
			    (int)h264sw_cabac_init_mn[ctx][set][0],
			    (int)h264sw_cabac_init_mn[ctx][set][1]);
		}
	}
}

/* Checks the pairs read from the specification by hand. */
static void
check_spots(
	void)
{
	size_t index;
	unsigned set;
	unsigned ctx;

	/* Every hand-read ctxIdx, in all four sets. */
	for (index = 0U;
	     index < sizeof(spots) / sizeof(spots[0]);
	     index++) {
		ctx = spots[index].ctx;
		for (set = 0U;
		     set < H264SW_CABAC_INIT_SETS;
		     set++) {
			CHECK((int)h264sw_cabac_init_mn[ctx][set][0] == spots[index].pair[set][0]);
			CHECK((int)h264sw_cabac_init_mn[ctx][set][1] == spots[index].pair[set][1]);
		}
	}
}

/* Checks the structure the header describes: shared columns, unused pairs and the value range. */
static void
check_shape(
	void)
{
	unsigned ctx;
	unsigned set;
	int max_n;
	int min_n;
	int value;

	/* The table has one entry per context variable and four sets of two values. */
	CHECK(sizeof(h264sw_cabac_init_mn) == (size_t)H264SW_CABAC_CONTEXTS * H264SW_CABAC_INIT_SETS * 2U);
	CHECK(H264SW_CABAC_CONTEXTS == 460U);
	CHECK(H264SW_CABAC_INIT_SETS == 4U);
	CHECK(H264SW_CABAC_INIT_I == 0U);

	/* Tables 9-12 and 9-17 give one pair that is the same in all four sets. */
	for (ctx = 0U;
	     ctx < 70U;
	     ctx++) {
		if (ctx > 10U && ctx < 60U)
			continue;
		for (set = 1U;
		     set < H264SW_CABAC_INIT_SETS;
		     set++) {
			CHECK(h264sw_cabac_init_mn[ctx][set][0] == h264sw_cabac_init_mn[ctx][0][0]);
			CHECK(h264sw_cabac_init_mn[ctx][set][1] == h264sw_cabac_init_mn[ctx][0][1]);
		}
	}

	/* The I set of ctxIdx 11..59 is not listed for I slices and is (0, 0). */
	for (ctx = 11U;
	     ctx < 60U;
	     ctx++) {
		CHECK(h264sw_cabac_init_mn[ctx][H264SW_CABAC_INIT_I][0] == 0);
		CHECK(h264sw_cabac_init_mn[ctx][H264SW_CABAC_INIT_I][1] == 0);
	}

	/* ctxIdx 276 is (0, 0) in every set. */
	for (set = 0U;
	     set < H264SW_CABAC_INIT_SETS;
	     set++) {
		CHECK(h264sw_cabac_init_mn[276][set][0] == 0);
		CHECK(h264sw_cabac_init_mn[276][set][1] == 0);
	}

	/* Every value is within -128..127 and n reaches 127 and -94, as the header says. */
	max_n = -128;
	min_n = 127;
	for (ctx = 0U;
	     ctx < H264SW_CABAC_CONTEXTS;
	     ctx++) {
		for (set = 0U;
		     set < H264SW_CABAC_INIT_SETS;
		     set++) {
			value = h264sw_cabac_init_mn[ctx][set][0];
			CHECK(value >= -128 && value <= 127);
			value = h264sw_cabac_init_mn[ctx][set][1];
			CHECK(value >= -128 && value <= 127);
			if (value > max_n)
				max_n = value;
			if (value < min_n)
				min_n = value;
		}
	}
	CHECK(max_n == 127);
	CHECK(min_n == -94);

	/* A table that is not all zero: ctxIdx 11..59 of cabac_init_idc 0 is listed in Tables 9-13 to 9-16. */
	CHECK(h264sw_cabac_init_mn[11][1][1] != 0);
}

/* Runs the checks, or prints the table with --dump. */
int
main(
	int argc,
	char **argv)
{
	/* The cross-check reads the table this way and makes no checks of its own. */
	if (argc == 2 && strcmp(argv[1], "--dump") == 0) {
		dump_pairs();
		return 0;
	}

	/* The pairs read by hand, then the structure. */
	check_spots();
	check_shape();

	/* Reports the result. */
	return check_finish("cabac_init");
}
