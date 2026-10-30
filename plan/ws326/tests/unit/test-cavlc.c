/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the CAVLC parsing of the software H.264 decoder (9.2, 9.1.2).
 *
 * The expected values are codewords chosen by hand from the page images
 * of Tables 9-4, 9-5, 9-7, 9-8, 9-9 and 9-10 of ITU-T H.264 (08/2021),
 * blocks whose bit strings were worked out on paper by running 9.2.2 to
 * 9.2.4 backwards, and an encoder written here from the same clauses for
 * round trips.  The tables of the module are also checked for their
 * shape: every table is a prefix code, and the sum of 2^-length over its
 * codewords is the number the specification's table leaves.
 *
 * The module's source is included so that its tables and its static
 * helpers can be read; its public functions are renamed there, and the
 * public interface is tested through the module linked in alongside.
 */

#include "check.h"
#include "bitwriter.h"

#include <errno.h>
#include <string.h>

#include "userland/desktop/libmedia/h264sw-cavlc.h"
#include "userland/desktop/libmedia/h264sw-context.h"

#define h264sw_cavlc_residual_block copy_residual_block
#define h264sw_cavlc_nc copy_nc
#define h264sw_cavlc_cbp copy_cbp
#include "userland/desktop/libmedia/h264sw-cavlc.c"
#undef h264sw_cavlc_residual_block
#undef h264sw_cavlc_nc
#undef h264sw_cavlc_cbp

/* The units of the Kraft sums below: a code of n bits counts 2^(16 - n). */
#define KRAFT_ONE 65536UL

/* The largest bit string of a hand-written block or codeword. */
#define BITS_TEXT_MAX 256U

/* A codeword picked from a table: its bits as printed and the values it stands for. */
struct picked {
	const char *bits;
	unsigned first;
	unsigned second;
};

/* A block worked out by hand: its bits, the shape it is read with and the levels it gives. */
struct hand_block {
	const char *bits;
	int nc;
	unsigned start;
	unsigned end;
	unsigned max_coeff;
	unsigned total;
	int32_t levels[16];
};

/*
 * The code of one macroblock's blocks for the nC tests: a 3 by 3
 * macroblock picture and the slice context that holds its neighbours.
 */
static struct h264sw_mb nc_mbs[9];
static struct h264sw_slice_ctx nc_ctx;

/* The writer shared by the tests; every test empties it before it writes. */
static struct bitwriter writer;

/* The state of the pseudo-random generator of the round-trip and fuzz tests. */
static uint32_t random_state = 2463534242UL;

/* Forward declarations of the static functions. */
static uint32_t random_next(void);
static void put_bits(struct bitwriter *out, const char *bits);
static size_t bits_length(const char *bits);
static void put_table_code(struct bitwriter *out, const struct cavlc_code *codes, unsigned count, unsigned value);
static void put_token(struct bitwriter *out, int nc, unsigned total, unsigned trailing);
static void put_level(struct bitwriter *out, int32_t level_code, unsigned suffix_length);
static void put_block(struct bitwriter *out, int nc, unsigned start, unsigned end, unsigned max_coeff, const int32_t *levels);
static int read_block(const struct bitwriter *in, size_t bytes, int nc, unsigned start, unsigned end, unsigned max_coeff, int32_t *levels, unsigned *total, size_t *position);
static unsigned long kraft_sum(const struct cavlc_code *codes, unsigned count);
static int prefix_free(const struct cavlc_code *codes, unsigned count);
static void check_picked_tokens(int nc, const struct picked *picks, unsigned count);
static void check_token_tables(void);
static void check_token_columns(void);
static void check_token_errors(void);
static void check_table_shapes(void);
static void check_total_zeros(void);
static void check_run_before(void);
static void check_hand_blocks(void);
static void check_level_edges(void);
static void check_round_trips(void);
static void check_truncated(void);
static void check_block_errors(void);
static void check_fuzz(void);
static void setup_nc(unsigned addr, int32_t a_slice, int32_t b_slice);
static void check_nc(void);
static void check_cbp(void);

/*
 * The coeff_token codewords chosen from Table 9-5, column by column:
 * the bits as printed, TrailingOnes and TotalCoeff.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-5.
 */
static const struct picked token_col0[] = {
	{ "1", 0, 0 }, { "01", 1, 1 }, { "001", 2, 2 }, { "00011", 3, 3 },
	{ "000101", 0, 1 }, { "000100", 1, 2 }, { "000011", 3, 4 },
	{ "0000 0011 1", 0, 3 }, { "0000 0010 0", 3, 7 },
	{ "0000 0000 0100 0", 0, 8 }, { "0000 0000 0011 01", 2, 10 },
	{ "0000 0000 0000 001", 1, 13 }, { "0000 0000 0000 0100", 0, 16 },
	{ "0000 0000 0000 0110", 1, 16 }, { "0000 0000 0000 1000", 3, 16 }
};
static const struct picked token_col1[] = {
	{ "11", 0, 0 }, { "10", 1, 1 }, { "011", 2, 2 }, { "0101", 3, 3 },
	{ "0010 11", 0, 1 }, { "0100", 3, 4 }, { "0000 0100", 0, 5 },
	{ "0000 0110", 1, 6 }, { "0000 100", 3, 8 }, { "0000 0000 1111", 0, 9 },
	{ "0000 0000 0110 1", 2, 12 }, { "0000 0000 0000 1", 3, 15 },
	{ "0000 0000 0001 11", 0, 16 }, { "0000 0000 0001 00", 3, 16 }
};
static const struct picked token_col2[] = {
	{ "1111", 0, 0 }, { "1110", 1, 1 }, { "0011 11", 0, 1 }, { "1101", 2, 2 },
	{ "1100", 3, 3 }, { "1011", 3, 4 }, { "1001", 3, 6 }, { "1000", 3, 7 },
	{ "0001 011", 0, 5 }, { "0000 0111 1", 0, 10 }, { "0000 0011 01", 0, 13 },
	{ "0000 0000 01", 0, 16 }, { "0000 0000 11", 2, 16 }, { "0000 0000 10", 3, 16 }
};
static const struct picked token_col3[] = {
	{ "0000 11", 0, 0 }, { "0000 00", 0, 1 }, { "0000 01", 1, 1 },
	{ "0001 00", 0, 2 }, { "0001 10", 2, 2 }, { "0010 11", 3, 3 },
	{ "0011 11", 3, 4 }, { "0111 00", 0, 8 }, { "1000 10", 2, 9 },
	{ "1111 00", 0, 16 }, { "1111 01", 1, 16 }, { "1111 10", 2, 16 },
	{ "1111 11", 3, 16 }
};
static const struct picked token_chroma_dc[] = {
	{ "01", 0, 0 }, { "1", 1, 1 }, { "0001 11", 0, 1 }, { "0001 00", 0, 2 },
	{ "0001 10", 1, 2 }, { "001", 2, 2 }, { "0000 11", 0, 3 },
	{ "0000 011", 1, 3 }, { "0000 010", 2, 3 }, { "0001 01", 3, 3 },
	{ "0000 10", 0, 4 }, { "0000 0011", 1, 4 }, { "0000 0010", 2, 4 },
	{ "0000 000", 3, 4 }
};

/*
 * total_zeros codewords chosen from Tables 9-7, 9-8 and 9-9 (a): the bits as
 * printed, tzVlcIndex and total_zeros.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Tables 9-7, 9-8 and 9-9 (a).
 */
static const struct picked zeros_4x4[] = {
	{ "1", 1, 0 }, { "0000 10", 1, 8 }, { "0000 0000 1", 1, 15 },
	{ "111", 2, 0 }, { "0000 00", 2, 14 }, { "0000 00", 3, 13 },
	{ "0001 1", 3, 9 }, { "0001 1", 4, 0 }, { "0001 0", 4, 10 },
	{ "0001", 5, 10 }, { "0000 01", 6, 0 }, { "001", 6, 9 },
	{ "11", 7, 5 }, { "0000 00", 7, 9 }, { "0000 00", 8, 8 },
	{ "0000 01", 8, 0 }, { "0000 00", 9, 1 }, { "0000 1", 9, 7 },
	{ "0000 1", 10, 0 }, { "0001", 10, 6 }, { "0000", 11, 0 },
	{ "1", 11, 4 }, { "001", 12, 4 }, { "000", 13, 0 }, { "01", 13, 3 },
	{ "00", 14, 0 }, { "1", 14, 2 }, { "0", 15, 0 }, { "1", 15, 1 }
};
static const struct picked zeros_chroma_dc[] = {
	{ "1", 1, 0 }, { "01", 1, 1 }, { "001", 1, 2 }, { "000", 1, 3 },
	{ "1", 2, 0 }, { "01", 2, 1 }, { "00", 2, 2 }, { "1", 3, 0 }, { "0", 3, 1 }
};

/*
 * run_before codewords chosen from Table 9-10: the bits, zerosLeft (7 for
 * "above 6" with the value 9 for a zerosLeft of 9) and run_before.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-10.
 */
static const struct picked runs[] = {
	{ "1", 1, 0 }, { "0", 1, 1 }, { "1", 2, 0 }, { "01", 2, 1 }, { "00", 2, 2 },
	{ "10", 3, 1 }, { "00", 3, 3 }, { "000", 4, 4 }, { "01", 4, 2 },
	{ "011", 5, 2 }, { "000", 5, 5 }, { "010", 5, 3 }, { "11", 6, 0 },
	{ "000", 6, 1 }, { "101", 6, 5 }, { "100", 6, 6 }, { "111", 7, 0 },
	{ "011", 7, 4 }, { "0001", 7, 7 }, { "0000 0001", 9, 11 },
	{ "0000 0000 001", 14, 14 }, { "0000 001", 10, 10 }, { "00001", 8, 8 }
};

/*
 * Blocks worked out by hand with 9.2.2 to 9.2.4 run backwards.  Each bit
 * string is cut where one syntax element ends: coeff_token, the signs,
 * the levels, total_zeros and the runs.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Tables 9-5, 9-7, 9-8, 9-9 (a) and 9-10.
 */
static const struct hand_block hand_blocks[] = {
	/*
	 * Levels 0 3 -1 0 0 -1 1 0 1: TotalCoeff 5, TrailingOnes 3.
	 * token 0000 100, signs 0 0 1, levels -1 (prefix 1) and 3 (prefix 2,
	 * suffix 0 with suffixLength 1), total_zeros 4, runs 1 0 2 0.
	 */
	{ "0000 100 001 01 0010 110 10 11 01 1", 0, 0, 15, 16, 5,
	  { 0, 3, -1, 0, 0, -1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0 } },

	/*
	 * Levels 20 -5 1: TotalCoeff 3, TrailingOnes 1.  The level -5 is
	 * sent 2 lower (the +2 rule), as prefix 7; suffixLength then
	 * becomes 2, so 20 is prefix 9 with suffix 2; total_zeros 0.
	 */
	{ "0000 0110 0 0000 0001 0000 0000 01 10 0101", 0, 0, 15, 16, 3,
	  { 20, -5, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },

	/*
	 * Levels -300 10: prefix 14 with a 4-bit suffix at suffixLength 0,
	 * then prefix 15 with a 12-bit suffix at suffixLength 2.
	 */
	{ "0000 0111 0000 0000 0000 001 0010 0000 0000 0000 0001 0010 0001 1011 111", 0, 0, 15, 16, 2,
	  { -300, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },

	/* Level 2115 at index 0: prefix 16, a 13-bit suffix of 100. */
	{ "0001 01 0000 0000 0000 0000 1 0000 0011 0010 0 1", 0, 0, 15, 16, 1,
	  { 2115, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },

	/* Level -16 at index 2: the largest code of prefix 14 at suffixLength 0 (suffix 15), total_zeros 2. */
	{ "0001 01 0000 0000 0000 001 1111 010", 0, 0, 15, 16, 1,
	  { 0, 0, -16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },

	/* Level -17 at index 0: the first code of prefix 15 at suffixLength 0 (suffix 1). */
	{ "0001 01 0000 0000 0000 0001 0000 0000 0001 1", 0, 0, 15, 16, 1,
	  { -17, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },

	/*
	 * Eleven levels with TrailingOnes 0: suffixLength starts at 1, so
	 * each level 1 is prefix 0 and a 1-bit suffix; the first, 2, is
	 * sent 2 lower (the +2 rule).  total_zeros 0.
	 */
	{ "0000 0000 0001 111 10 10 10 10 10 10 10 10 10 10 10 0000", 0, 0, 15, 16, 11,
	  { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 0, 0, 0, 0, 0 } },

	/*
	 * Levels 2 2 0 0 0 1: total_zeros 3 and the first run takes all
	 * three, so no run_before is read for the second coefficient.
	 */
	{ "0000 0110 0 1 010 101 00", 0, 0, 15, 16, 3,
	  { 2, 2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },

	/* The chroma DC block of 4:2:0, levels 3 0 -1 1. */
	{ "0000 010 0 1 001 0 1 0", -1, 0, 3, 4, 3,
	  { 3, 0, -1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },

	/* An AC block of 15 coefficients with one level at its last index 14. */
	{ "01 0 0000 0001 0", 0, 0, 14, 15, 1,
	  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0 } },

	/* A block with no coefficient: coeff_token of TotalCoeff 0 only. */
	{ "1", 0, 0, 15, 16, 0,
	  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },

	/* The same with the chroma DC code (01) and the 8 <= nC code (0000 11). */
	{ "01", -1, 0, 3, 4, 0,
	  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
	{ "0000 11", 9, 0, 15, 16, 0,
	  { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } }
};

/*
 * coded_block_pattern of each codeNum 0 to 47 of Table 9-4 (a), read by hand
 * from the page image: the Intra_4x4 and Intra_8x8 column.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-4 (a).
 */
static const unsigned cbp_intra_expected[48] = {
	47, 31, 15, 0, 23, 27, 29, 30, 7, 11, 13, 14, 39, 43, 45, 46,
	16, 3, 5, 10, 12, 19, 21, 26, 28, 35, 37, 42, 44, 1, 2, 4,
	8, 17, 18, 20, 24, 6, 9, 22, 25, 32, 33, 34, 36, 40, 38, 41
};

/* The Inter column of Table 9-4 (a), read the same way. */
static const unsigned cbp_inter_expected[48] = {
	0, 16, 1, 2, 4, 8, 32, 3, 5, 10, 12, 15, 47, 7, 11, 13,
	14, 6, 9, 31, 35, 37, 42, 44, 33, 34, 36, 40, 39, 43, 45, 46,
	17, 18, 20, 24, 19, 21, 26, 28, 23, 27, 29, 30, 22, 25, 38, 41
};

/* Runs every check and prints the summary. */
int
main(void)
{
	/* The tables of the module first: their shape and the codewords picked from the specification. */
	check_table_shapes();
	check_token_tables();
	check_token_columns();
	check_token_errors();
	check_total_zeros();
	check_run_before();

	/* Whole blocks: the hand-made blocks, the level escapes, round trips and errors. */
	check_hand_blocks();
	check_level_edges();
	check_round_trips();
	check_truncated();
	check_block_errors();
	check_fuzz();

	/* The context nC and the coded block pattern. */
	check_nc();
	check_cbp();

	/* Succeeded: reports the totals. */
	return check_finish("cavlc");
}

/* Gives the next number of a xorshift generator, so a failure repeats. */
static uint32_t
random_next(void)
{
	/* One xorshift step. */
	random_state = random_state ^ (random_state << 13);
	random_state = random_state ^ (random_state >> 17);
	random_state = random_state ^ (random_state << 5);

	/* The new state is the number. */
	return random_state;
}

/* Writes a string of 0 and 1 characters as bits; spaces are ignored. */
static void
put_bits(
	struct bitwriter *out,
	const char *bits)
{
	size_t index;

	/* Each 0 or 1 character is one bit. */
	for (index = 0; bits[index] != '\0'; index++) {
		if (bits[index] == '0')
			bitwriter_u(out, 0U, 1U);
		if (bits[index] == '1')
			bitwriter_u(out, 1U, 1U);
	}
}

/* Counts the bits of a string of 0 and 1 characters, ignoring spaces. */
static size_t
bits_length(
	const char *bits)
{
	size_t index;
	size_t count;

	/* Each 0 or 1 character is one bit. */
	count = 0U;
	for (index = 0; bits[index] != '\0'; index++) {
		if (bits[index] == '0' || bits[index] == '1')
			count++;
	}

	/* The number of bits. */
	return count;
}

/* Writes the codeword of a table that stands for a value. */
static void
put_table_code(
	struct bitwriter *out,
	const struct cavlc_code *codes,
	unsigned count,
	unsigned value)
{
	unsigned index;

	/* Finds the entry of the value among the first count codes. */
	for (index = 0; index < count; index++) {
		if ((unsigned)codes[index].value == value) {
			bitwriter_u(out, (uint32_t)codes[index].code, (unsigned)codes[index].length);
			return;
		}
	}

	/* A value the table has no code for is a mistake in the test. */
	CHECK(0);
}

/* Writes coeff_token of a TotalCoeff and TrailingOnes in the column nC selects. */
static void
put_token(
	struct bitwriter *out,
	int nc,
	unsigned total,
	unsigned trailing)
{
	unsigned value;

	/* The value of the code is TotalCoeff * 4 + TrailingOnes. */
	value = total * 4U + trailing;

	/* The column nC selects. */
	if (nc == -1) {
		put_table_code(out, coeff_token_chroma_dc, CAVLC_TOKEN_CHROMA_DC_COUNT, value);
	} else if (nc < 2) {
		put_table_code(out, coeff_token_0_2, CAVLC_TOKEN_COUNT, value);
	} else if (nc < 4) {
		put_table_code(out, coeff_token_2_4, CAVLC_TOKEN_COUNT, value);
	} else if (nc < 8) {
		put_table_code(out, coeff_token_4_8, CAVLC_TOKEN_COUNT, value);
	} else {
		put_table_code(out, coeff_token_8_up, CAVLC_TOKEN_COUNT, value);
	}
}

/*
 * Writes level_prefix and level_suffix for a levelCode (after the +2 rule
 * is taken off) at a suffixLength, by inverting equations of 9.2.2.
 */
static void
put_level(
	struct bitwriter *out,
	int32_t level_code,
	unsigned suffix_length)
{
	unsigned prefix;
	unsigned suffix_size;
	int32_t suffix;
	int32_t rest;
	int32_t base;

	/* Chooses the prefix and the suffix that give the code. */
	prefix = 0U;
	suffix_size = suffix_length;
	suffix = 0;
	if (suffix_length == 0U) {
		if (level_code < 14) {
			/* A prefix below 14 is the code itself. */
			prefix = (unsigned)level_code;
			suffix_size = 0U;
		} else if (level_code < 30) {
			/* Prefix 14 carries a 4-bit suffix at suffixLength 0. */
			prefix = 14U;
			suffix_size = 4U;
			suffix = level_code - 14;
		} else {
			/* Prefix 15 starts at 30 (15 + 15) with a 12-bit suffix. */
			prefix = 15U;
			suffix_size = 12U;
			suffix = level_code - 30;
		}
	} else if (level_code < (int32_t)(15U << suffix_length)) {
		/* The prefix is the code above suffixLength, the suffix its low bits. */
		prefix = (unsigned)level_code >> suffix_length;
		suffix = level_code - (int32_t)(prefix << suffix_length);
	} else {
		/* Prefix 15 carries a 12-bit suffix from 15 << suffixLength. */
		prefix = 15U;
		suffix_size = 12U;
		suffix = level_code - (int32_t)(15U << suffix_length);
	}

	/* A suffix too big for 12 bits moves to the first larger prefix that holds it. */
	if (prefix == 15U && suffix >= 4096) {
		rest = suffix;
		prefix = 16U;
		for (;;) {
			base = (int32_t)(1U << (prefix - 3U)) - 4096;
			if (rest - base < (int32_t)(1U << (prefix - 3U)))
				break;
			prefix++;
		}
		suffix_size = prefix - 3U;
		suffix = rest - base;
	}

	/* Writes the prefix zeros, the 1, then the suffix. */
	bitwriter_u(out, 0U, prefix);
	bitwriter_u(out, 1U, 1U);
	bitwriter_u(out, (uint32_t)suffix, suffix_size);
}

/*
 * Writes a whole residual block for levels[0..max_coeff - 1] as the syntax
 * of 7.3.5.3.2 and the processes of 9.2 would read it, by inverting them.
 */
static void
put_block(
	struct bitwriter *out,
	int nc,
	unsigned start,
	unsigned end,
	unsigned max_coeff,
	const int32_t *levels)
{
	unsigned positions[16];
	int32_t read_order[16];
	unsigned count;
	unsigned index;
	unsigned trailing;
	unsigned suffix_length;
	unsigned zeros;
	unsigned zeros_left;
	unsigned run;
	int32_t level;
	int32_t level_code;
	int32_t magnitude;

	/* Finds the positions of the non-zero levels, lowest first (relative to startIdx). */
	count = 0U;
	for (index = start; index <= end; index++) {
		if (levels[index] != 0) {
			positions[count] = index - start;
			count++;
		}
	}

	/* The levels in the order they are sent: the highest frequency first. */
	for (index = 0; index < count; index++)
		read_order[index] = levels[positions[count - 1U - index] + start];

	/* TrailingOnes: up to 3 levels of magnitude 1 at the start of the read order. */
	trailing = 0U;
	while (trailing < 3U && trailing < count && (read_order[trailing] == 1 || read_order[trailing] == -1))
		trailing++;

	/* coeff_token and, with no level, the end of the block. */
	put_token(out, nc, count, trailing);
	if (count == 0U)
		return;

	/* The signs of the trailing ones. */
	for (index = 0; index < trailing; index++) {
		if (read_order[index] < 0) {
			bitwriter_u(out, 1U, 1U);
		} else {
			bitwriter_u(out, 0U, 1U);
		}
	}

	/* The other levels with the suffixLength of 9.2.2. */
	suffix_length = 0U;
	if (count > 10U && trailing < 3U)
		suffix_length = 1U;
	for (index = trailing; index < count; index++) {
		level = read_order[index];
		if (level > 0) {
			level_code = 2 * level - 2;
		} else {
			level_code = -2 * level - 1;
		}
		if (index == trailing && trailing < 3U)
			level_code = level_code - 2;
		put_level(out, level_code, suffix_length);

		/* The update of suffixLength after each level. */
		if (suffix_length == 0U)
			suffix_length = 1U;
		magnitude = level;
		if (magnitude < 0)
			magnitude = -magnitude;
		if (magnitude > 3 * (int32_t)(1U << (suffix_length - 1U)) && suffix_length < 6U)
			suffix_length++;
	}

	/* total_zeros: the zeros below the last coefficient, sent when the block is not full. */
	zeros = positions[count - 1U] + 1U - count;
	if (count < end - start + 1U) {
		if (max_coeff == 4U) {
			put_table_code(out, total_zeros_chroma_dc[count - 1U], 5U - count, zeros);
		} else {
			put_table_code(out, total_zeros_4x4[count - 1U], 17U - count, zeros);
		}
	}

	/* run_before for each coefficient but the lowest, while zeros are left. */
	zeros_left = zeros;
	for (index = 0; index + 1U < count && zeros_left > 0U; index++) {
		run = positions[count - 1U - index] - positions[count - 2U - index] - 1U;
		if (zeros_left > 6U) {
			put_table_code(out, run_before[6], 15U, run);
		} else {
			put_table_code(out, run_before[zeros_left - 1U], zeros_left + 1U, run);
		}
		zeros_left = zeros_left - run;
	}
}

/* Reads one block from the first bytes of a writer; reports the error, TotalCoeff and the bit position. */
static int
read_block(
	const struct bitwriter *in,
	size_t bytes,
	int nc,
	unsigned start,
	unsigned end,
	unsigned max_coeff,
	int32_t *levels,
	unsigned *total,
	size_t *position)
{
	struct vcodec_bitreader bits;
	int error;

	/* Reads the block with the module under test. */
	vcodec_bitreader_init(&bits, in->data, bytes);
	error = h264sw_cavlc_residual_block(&bits, nc, start, end, max_coeff, levels, total);

	/* Reports where the reader stopped. */
	*position = bits.position;

	/* The module's answer. */
	return error;
}

/*
 * Sums 2^(16 - length) over the first count codes, or gives ULONG_MAX
 * when one code is a prefix of another (or of itself twice).
 */
static unsigned long
kraft_sum(
	const struct cavlc_code *codes,
	unsigned count)
{
	unsigned long sum;
	unsigned first;
	unsigned second;
	unsigned shorter;

	/* Adds the weight of each code. */
	sum = 0UL;
	for (first = 0; first < count; first++)
		sum = sum + (1UL << (16U - (unsigned)codes[first].length));

	/* A code that starts another breaks the prefix property. */
	for (first = 0; first < count; first++) {
		for (second = 0; second < count; second++) {
			if (first == second)
				continue;
			shorter = (unsigned)codes[first].length;
			if (shorter > (unsigned)codes[second].length)
				continue;
			if ((unsigned)codes[second].code >> ((unsigned)codes[second].length - shorter) == (unsigned)codes[first].code)
				return (unsigned long)-1;
		}
	}

	/* The sum of a prefix code. */
	return sum;
}

/* Reports whether no code of a table is a prefix of another. */
static int
prefix_free(
	const struct cavlc_code *codes,
	unsigned count)
{
	unsigned long sum;

	/* The sum is the sentinel when the property fails. */
	sum = kraft_sum(codes, count);
	if (sum == (unsigned long)-1)
		return 0;

	/* Succeeded: a prefix code. */
	return 1;
}

/* Checks that picked codewords of a coeff_token column decode to their values and use their length. */
static void
check_picked_tokens(
	int nc,
	const struct picked *picks,
	unsigned count)
{
	unsigned index;
	unsigned tail;
	unsigned total;
	unsigned trailing;
	struct vcodec_bitreader bits;
	int error;

	/* Each codeword followed by all zeros and by all ones must decode the same, using only its own bits. */
	for (index = 0; index < count; index++) {
		for (tail = 0; tail < 2U; tail++) {
			bitwriter_init(&writer);
			put_bits(&writer, picks[index].bits);
			bitwriter_u(&writer, tail == 0U ? 0U : 0xffffU, 16U);
			vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
			total = 99U;
			trailing = 99U;
			error = cavlc_coeff_token(&bits, nc, &total, &trailing);
			CHECK(error == 0);
			CHECK(trailing == picks[index].first);
			CHECK(total == picks[index].second);
			CHECK(bits.position == bits_length(picks[index].bits));
		}
	}
}

/* Checks coeff_token against the codewords picked from Table 9-5, in all five columns. */
static void
check_token_tables(void)
{
	/* Each column in turn, with an nC from the middle of its range. */
	check_picked_tokens(0, token_col0, (unsigned)(sizeof(token_col0) / sizeof(token_col0[0])));
	check_picked_tokens(1, token_col0, (unsigned)(sizeof(token_col0) / sizeof(token_col0[0])));
	check_picked_tokens(2, token_col1, (unsigned)(sizeof(token_col1) / sizeof(token_col1[0])));
	check_picked_tokens(3, token_col1, (unsigned)(sizeof(token_col1) / sizeof(token_col1[0])));
	check_picked_tokens(4, token_col2, (unsigned)(sizeof(token_col2) / sizeof(token_col2[0])));
	check_picked_tokens(7, token_col2, (unsigned)(sizeof(token_col2) / sizeof(token_col2[0])));
	check_picked_tokens(8, token_col3, (unsigned)(sizeof(token_col3) / sizeof(token_col3[0])));
	check_picked_tokens(16, token_col3, (unsigned)(sizeof(token_col3) / sizeof(token_col3[0])));
	check_picked_tokens(-1, token_chroma_dc, (unsigned)(sizeof(token_chroma_dc) / sizeof(token_chroma_dc[0])));
}

/*
 * Checks that nC picks the column at each of its boundaries: the bits 1111
 * followed by zeros are TotalCoeff 0 in three columns (after 1, 2 and 4
 * bits), TotalCoeff 16 with no trailing ones in the 8 <= nC column (after 6
 * bits) and TrailingOnes 1 of TotalCoeff 1 in the nC -1 column (after 1 bit).
 */
static void
check_token_columns(void)
{
	static const int nc_values[] = { 0, 1, 2, 3, 4, 7, 8, 20, -1 };
	static const unsigned expect_length[] = { 1U, 1U, 2U, 2U, 4U, 4U, 6U, 6U, 1U };
	static const unsigned expect_total[] = { 0U, 0U, 0U, 0U, 0U, 0U, 16U, 16U, 1U };
	static const unsigned expect_trailing[] = { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U };
	struct vcodec_bitreader bits;
	unsigned total;
	unsigned trailing;
	unsigned index;

	/* Reads the same bits with each nC. */
	for (index = 0; index < (unsigned)(sizeof(nc_values) / sizeof(nc_values[0])); index++) {
		bitwriter_init(&writer);
		put_bits(&writer, "1111");
		bitwriter_u(&writer, 0U, 16U);
		vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
		CHECK(cavlc_coeff_token(&bits, nc_values[index], &total, &trailing) == 0);
		CHECK(total == expect_total[index]);
		CHECK(trailing == expect_trailing[index]);
		CHECK(bits.position == expect_length[index]);
	}

	/* The nC of -2 (4:2:2 chroma DC) and below has no column. */
	bitwriter_init(&writer);
	put_bits(&writer, "1");
	bitwriter_u(&writer, 0U, 16U);
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	CHECK(cavlc_coeff_token(&bits, -2, &total, &trailing) == EINVAL);
	CHECK(cavlc_coeff_token(&bits, -100, &total, &trailing) == EINVAL);
}

/* Checks the bit patterns that are in no column: unused codes and a run of zeros. */
static void
check_token_errors(void)
{
	struct vcodec_bitreader bits;
	unsigned total;
	unsigned trailing;

	/* 0000 10 is unused in the 8 <= nC column (it would be TrailingOnes 2 of TotalCoeff 1). */
	bitwriter_init(&writer);
	put_bits(&writer, "0000 10");
	bitwriter_u(&writer, 0xffffU, 16U);
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	CHECK(cavlc_coeff_token(&bits, 8, &total, &trailing) == EINVAL);

	/* 0001 11 is unused in the 8 <= nC column too (TrailingOnes 3 of TotalCoeff 2). */
	bitwriter_init(&writer);
	put_bits(&writer, "0001 11");
	bitwriter_u(&writer, 0xffffU, 16U);
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	CHECK(cavlc_coeff_token(&bits, 8, &total, &trailing) == EINVAL);

	/* A run of 16 zeros is in no column of nC 0 and above, whatever follows it. */
	bitwriter_init(&writer);
	bitwriter_u(&writer, 0U, 16U);
	bitwriter_u(&writer, 0xffffU, 16U);
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	CHECK(cavlc_coeff_token(&bits, 0, &total, &trailing) == EINVAL);
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	CHECK(cavlc_coeff_token(&bits, 2, &total, &trailing) == EINVAL);
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	CHECK(cavlc_coeff_token(&bits, 5, &total, &trailing) == EINVAL);

	/* A code cut by the end of the RBSP is an error: 0000 0011 1 needs 9 bits, only 8 are there. */
	bitwriter_init(&writer);
	put_bits(&writer, "0000 0011");
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	CHECK(cavlc_coeff_token(&bits, 0, &total, &trailing) == EINVAL);

	/* A reader that already failed reads nothing. */
	bitwriter_init(&writer);
	put_bits(&writer, "1");
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	bits.error = EINVAL;
	CHECK(cavlc_coeff_token(&bits, 0, &total, &trailing) == EINVAL);
}

/*
 * Checks the shape of every table: a prefix code whose Kraft sum is what
 * the specification's table leaves, and the right number of codewords.  The
 * tables of coeff_token and the first of total_zeros and run_before leave
 * the all-zero pattern unused (it would make a run of zero bits that no
 * start code may contain), and the 8 <= nC column leaves two of its 64
 * six-bit patterns; every other table fills the code space.
 */
static void
check_table_shapes(void)
{
	unsigned index;
	unsigned pair;
	unsigned seen[17][4];
	unsigned total;
	unsigned trailing;
	unsigned long expect;
	const struct cavlc_code *column;

	/* Every coeff_token column is a prefix code with the codes the table has. */
	CHECK(prefix_free(coeff_token_0_2, CAVLC_TOKEN_COUNT));
	CHECK(prefix_free(coeff_token_2_4, CAVLC_TOKEN_COUNT));
	CHECK(prefix_free(coeff_token_4_8, CAVLC_TOKEN_COUNT));
	CHECK(prefix_free(coeff_token_8_up, CAVLC_TOKEN_COUNT));
	CHECK(prefix_free(coeff_token_chroma_dc, CAVLC_TOKEN_CHROMA_DC_COUNT));

	/* The 8 <= nC column is 64 six-bit codes less two unused; 0 <= nC < 2 leaves 15 zeros, 2 <= nC < 4 leaves 13, 4 <= nC < 8 leaves 10; nC -1 is full. */
	CHECK(kraft_sum(coeff_token_0_2, CAVLC_TOKEN_COUNT) == KRAFT_ONE - (KRAFT_ONE >> 15));
	CHECK(kraft_sum(coeff_token_2_4, CAVLC_TOKEN_COUNT) == KRAFT_ONE - (KRAFT_ONE >> 13));
	CHECK(kraft_sum(coeff_token_4_8, CAVLC_TOKEN_COUNT) == KRAFT_ONE - (KRAFT_ONE >> 10));
	CHECK(kraft_sum(coeff_token_8_up, CAVLC_TOKEN_COUNT) == KRAFT_ONE - 2UL * (KRAFT_ONE >> 6));
	CHECK(kraft_sum(coeff_token_chroma_dc, CAVLC_TOKEN_CHROMA_DC_COUNT) == KRAFT_ONE);

	/* The fixed-length column has only 6-bit codes, as the table prints it. */
	for (index = 0; index < CAVLC_TOKEN_COUNT; index++)
		CHECK(coeff_token_8_up[index].length == 6U);

	/* Each (TrailingOnes, TotalCoeff) of a column is present once: 62 pairs, TrailingOnes at most 3 and at most TotalCoeff. */
	for (pair = 0; pair < 4U; pair++) {
		if (pair == 0U) {
			column = coeff_token_0_2;
		} else if (pair == 1U) {
			column = coeff_token_2_4;
		} else if (pair == 2U) {
			column = coeff_token_4_8;
		} else {
			column = coeff_token_8_up;
		}

		memset(seen, 0, sizeof(seen));
		for (index = 0; index < CAVLC_TOKEN_COUNT; index++) {
			total = (unsigned)column[index].value >> 2;
			trailing = (unsigned)column[index].value & 3U;
			CHECK(total <= 16U);
			CHECK(trailing <= 3U);
			CHECK(trailing <= total);
			CHECK(column[index].length >= 1U && column[index].length <= 16U);
			CHECK(((unsigned)column[index].code >> column[index].length) == 0U);
			if (total <= 16U && trailing <= 3U)
				seen[total][trailing]++;
		}
		for (total = 0; total <= 16U; total++) {
			for (trailing = 0; trailing <= 3U; trailing++) {
				if (trailing <= total)
					CHECK(seen[total][trailing] == 1U);
				else
					CHECK(seen[total][trailing] == 0U);
			}
		}
	}

	/* The chroma DC column has the 14 pairs of TotalCoeff up to 4. */
	memset(seen, 0, sizeof(seen));
	for (index = 0; index < CAVLC_TOKEN_CHROMA_DC_COUNT; index++) {
		total = (unsigned)coeff_token_chroma_dc[index].value >> 2;
		trailing = (unsigned)coeff_token_chroma_dc[index].value & 3U;
		CHECK(total <= 4U);
		CHECK(trailing <= 3U);
		if (total <= 16U && trailing <= 3U)
			seen[total][trailing]++;
	}
	for (total = 0; total <= 4U; total++) {
		for (trailing = 0; trailing <= 3U; trailing++) {
			if (trailing <= total)
				CHECK(seen[total][trailing] == 1U);
			else
				CHECK(seen[total][trailing] == 0U);
		}
	}

	/* total_zeros of 4x4 blocks: tzVlcIndex 1 leaves the all-zero pattern (9 bits), the others are full. */
	for (index = 1; index <= 15U; index++) {
		expect = KRAFT_ONE;
		if (index == 1U)
			expect = KRAFT_ONE - (KRAFT_ONE >> 9);
		CHECK(kraft_sum(total_zeros_4x4[index - 1U], 17U - index) == expect);
	}

	/* total_zeros of the chroma DC block of 4:2:0 are full. */
	for (index = 1; index <= 3U; index++)
		CHECK(kraft_sum(total_zeros_chroma_dc[index - 1U], 5U - index) == KRAFT_ONE);

	/* run_before: zerosLeft 1 to 6 are full; above 6 leaves the 11-bit all-zero pattern. */
	for (index = 1; index <= 6U; index++)
		CHECK(kraft_sum(run_before[index - 1U], index + 1U) == KRAFT_ONE);
	CHECK(kraft_sum(run_before[6], 15U) == KRAFT_ONE - (KRAFT_ONE >> 11));

	/* The entries of total_zeros and run_before are numbered from 0 in order. */
	for (index = 1; index <= 15U; index++) {
		for (pair = 0; pair < 17U - index; pair++)
			CHECK((unsigned)total_zeros_4x4[index - 1U][pair].value == pair);
	}
	for (index = 1; index <= 3U; index++) {
		for (pair = 0; pair < 5U - index; pair++)
			CHECK((unsigned)total_zeros_chroma_dc[index - 1U][pair].value == pair);
	}
	for (pair = 0; pair < 15U; pair++)
		CHECK((unsigned)run_before[6][pair].value == pair);
	for (index = 1; index <= 6U; index++) {
		for (pair = 0; pair <= index; pair++)
			CHECK((unsigned)run_before[index - 1U][pair].value == pair);
	}
}

/* Checks total_zeros against the codewords picked from Tables 9-7, 9-8 and 9-9 (a). */
static void
check_total_zeros(void)
{
	unsigned index;
	unsigned zeros;
	unsigned tail;
	size_t count;
	struct vcodec_bitreader bits;

	/* The tables of 4x4 blocks: each picked code with zeros and ones after it reads the same value. */
	count = sizeof(zeros_4x4) / sizeof(zeros_4x4[0]);
	for (index = 0; index < count; index++) {
		for (tail = 0; tail < 2U; tail++) {
			bitwriter_init(&writer);
			put_bits(&writer, zeros_4x4[index].bits);
			bitwriter_u(&writer, tail == 0U ? 0U : 0xffffU, 16U);
			vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
			zeros = 99U;
			CHECK(cavlc_total_zeros(&bits, zeros_4x4[index].first, 16U, &zeros) == 0);
			CHECK(zeros == zeros_4x4[index].second);
			CHECK(bits.position == bits_length(zeros_4x4[index].bits));
		}
	}

	/* The same tables are used for blocks of 15 coefficients. */
	bitwriter_init(&writer);
	put_bits(&writer, "0000 10");
	bitwriter_u(&writer, 0U, 16U);
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	CHECK(cavlc_total_zeros(&bits, 1U, 15U, &zeros) == 0);
	CHECK(zeros == 8U);

	/* The chroma DC table is chosen by max_coeff 4. */
	count = sizeof(zeros_chroma_dc) / sizeof(zeros_chroma_dc[0]);
	for (index = 0; index < count; index++) {
		for (tail = 0; tail < 2U; tail++) {
			bitwriter_init(&writer);
			put_bits(&writer, zeros_chroma_dc[index].bits);
			bitwriter_u(&writer, tail == 0U ? 0U : 0xffffU, 16U);
			vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
			zeros = 99U;
			CHECK(cavlc_total_zeros(&bits, zeros_chroma_dc[index].first, 4U, &zeros) == 0);
			CHECK(zeros == zeros_chroma_dc[index].second);
			CHECK(bits.position == bits_length(zeros_chroma_dc[index].bits));
		}
	}

	/* A TotalCoeff with no table row (0, 16 and above; 4 and above for chroma DC) is refused. */
	bitwriter_init(&writer);
	bitwriter_u(&writer, 0xffffU, 16U);
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	CHECK(cavlc_total_zeros(&bits, 0U, 16U, &zeros) == EINVAL);
	CHECK(cavlc_total_zeros(&bits, 16U, 16U, &zeros) == EINVAL);
	CHECK(cavlc_total_zeros(&bits, 0U, 4U, &zeros) == EINVAL);
	CHECK(cavlc_total_zeros(&bits, 4U, 4U, &zeros) == EINVAL);

	/* The all-zero pattern is no code of tzVlcIndex 1 (nine zeros), and is the last code, 11, of tzVlcIndex 5 (0000 0). */
	bitwriter_init(&writer);
	bitwriter_u(&writer, 0U, 16U);
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	CHECK(cavlc_total_zeros(&bits, 1U, 16U, &zeros) == EINVAL);
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	CHECK(cavlc_total_zeros(&bits, 5U, 16U, &zeros) == 0);
	CHECK(zeros == 11U);
}

/* Checks run_before against the codewords picked from Table 9-10. */
static void
check_run_before(void)
{
	unsigned index;
	unsigned run;
	unsigned tail;
	size_t count;
	struct vcodec_bitreader bits;

	/* Each picked code with zeros and ones after it reads the same value, for the zerosLeft of its row. */
	count = sizeof(runs) / sizeof(runs[0]);
	for (index = 0; index < count; index++) {
		for (tail = 0; tail < 2U; tail++) {
			bitwriter_init(&writer);
			put_bits(&writer, runs[index].bits);
			bitwriter_u(&writer, tail == 0U ? 0U : 0xffffU, 16U);
			vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
			run = 99U;
			CHECK(cavlc_run_before(&bits, runs[index].first, &run) == 0);
			CHECK(run == runs[index].second);
			CHECK(bits.position == bits_length(runs[index].bits));
		}
	}

	/* A zerosLeft of 0 has no code. */
	bitwriter_init(&writer);
	bitwriter_u(&writer, 0xffffU, 16U);
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	CHECK(cavlc_run_before(&bits, 0U, &run) == EINVAL);

	/* The all-zero pattern of 11 bits is no code, in the row above 6 and in the rows below. */
	bitwriter_init(&writer);
	bitwriter_u(&writer, 0U, 16U);
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	CHECK(cavlc_run_before(&bits, 9U, &run) == EINVAL);
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	CHECK(cavlc_run_before(&bits, 1U, &run) == 0);
	CHECK(run == 1U);
}

/* Checks the blocks worked out by hand, through the public function. */
static void
check_hand_blocks(void)
{
	unsigned index;
	unsigned count;
	unsigned position;
	int32_t levels[16];
	unsigned total;
	size_t end_position;
	int error;

	/* Each block is read and must give its levels, TotalCoeff and end where its bits end. */
	count = (unsigned)(sizeof(hand_blocks) / sizeof(hand_blocks[0]));
	for (index = 0; index < count; index++) {
		bitwriter_init(&writer);
		put_bits(&writer, hand_blocks[index].bits);
		bitwriter_trailing(&writer);

		memset(levels, 0x55, sizeof(levels));
		total = 99U;
		error = read_block(&writer, bitwriter_bytes(&writer), hand_blocks[index].nc, hand_blocks[index].start, hand_blocks[index].end, hand_blocks[index].max_coeff, levels, &total, &end_position);
		CHECK(error == 0);
		CHECK(total == hand_blocks[index].total);
		CHECK(end_position == bits_length(hand_blocks[index].bits));
		for (position = 0; position < hand_blocks[index].max_coeff; position++)
			CHECK(levels[position] == hand_blocks[index].levels[position]);

		/* The entries past max_coeff are not touched. */
		for (position = hand_blocks[index].max_coeff; position < 16U; position++)
			CHECK(levels[position] == 0x55555555L);
	}
}

/* Checks the largest and smallest levels and the prefixes of the escape codes by writing them with the encoder. */
static void
check_level_edges(void)
{
	static const int32_t values[] = { 2, -2, 3, -3, 4, 7, 8, -8, 15, 16, -16, 17, -17, 29, 30, 31, -31, 100, 2047, -2048, 2049, 4095, 4096, 4097, -4097, 12000, 12400, 28700, 28800, 32767, -32767, -32768 };
	unsigned index;
	unsigned shape;
	int32_t levels[16];
	int32_t expected[16];
	unsigned total;
	size_t end_position;
	size_t block_bits;
	unsigned position;
	int error;

	/* Every value alone at index 0, in the blocks of 16, 15 and (the first four) 4 coefficients. */
	for (index = 0; index < (unsigned)(sizeof(values) / sizeof(values[0])); index++) {
		for (shape = 0; shape < 3U; shape++) {
			memset(expected, 0, sizeof(expected));
			expected[0] = values[index];
			bitwriter_init(&writer);
			if (shape == 0U) {
				put_block(&writer, 0, 0U, 15U, 16U, expected);
			} else if (shape == 1U) {
				put_block(&writer, 9, 0U, 14U, 15U, expected);
			} else {
				put_block(&writer, -1, 0U, 3U, 4U, expected);
			}
			block_bits = writer.bits;
			bitwriter_trailing(&writer);

			memset(levels, 0x55, sizeof(levels));
			error = 0;
			if (shape == 0U) {
				error = read_block(&writer, bitwriter_bytes(&writer), 0, 0U, 15U, 16U, levels, &total, &end_position);
			} else if (shape == 1U) {
				error = read_block(&writer, bitwriter_bytes(&writer), 9, 0U, 14U, 15U, levels, &total, &end_position);
			} else {
				error = read_block(&writer, bitwriter_bytes(&writer), -1, 0U, 3U, 4U, levels, &total, &end_position);
			}
			CHECK(error == 0);
			CHECK(total == 1U);
			CHECK(end_position == block_bits);
			for (position = 0; position < 4U; position++)
				CHECK(levels[position] == expected[position]);
		}
	}
}

/* Round-trips random blocks: written by the encoder above, read by the module. */
static void
check_round_trips(void)
{
	static const int nc_choices[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16 };
	static const unsigned shapes[][3] = {
		{ 0U, 15U, 16U }, { 0U, 14U, 15U }, { 0U, 3U, 4U },
		{ 3U, 12U, 16U }, { 0U, 5U, 16U }, { 2U, 14U, 15U }, { 1U, 3U, 4U },
		{ 15U, 15U, 16U }, { 0U, 0U, 15U }
	};
	unsigned round;
	unsigned shape;
	unsigned start;
	unsigned end;
	unsigned max_coeff;
	unsigned room;
	unsigned count;
	unsigned index;
	unsigned slot;
	unsigned swap;
	unsigned pool[16];
	unsigned picked;
	unsigned kind;
	int nc;
	int32_t expected[16];
	int32_t levels[16];
	int32_t value;
	unsigned total;
	size_t end_position;
	size_t block_bits;
	int error;
	int same;

	/* Many random blocks of each shape. */
	for (round = 0; round < 6000U; round++) {
		shape = round % (unsigned)(sizeof(shapes) / sizeof(shapes[0]));
		start = shapes[shape][0];
		end = shapes[shape][1];
		max_coeff = shapes[shape][2];
		room = end - start + 1U;

		/* The context nC: chroma DC for 4 coefficients, else one of the columns. */
		nc = -1;
		if (max_coeff != 4U)
			nc = nc_choices[random_next() % (unsigned)(sizeof(nc_choices) / sizeof(nc_choices[0]))];

		/* A random number of non-zero levels, often few, sometimes all. */
		kind = random_next() % 8U;
		if (kind == 0U) {
			count = room;
		} else if (kind == 1U) {
			count = 0U;
		} else {
			count = random_next() % (room + 1U);
		}

		/* Random distinct positions in startIdx..endIdx. */
		for (index = 0; index < room; index++)
			pool[index] = start + index;
		for (index = 0; index < room; index++) {
			swap = index + random_next() % (room - index);
			picked = pool[swap];
			pool[swap] = pool[index];
			pool[index] = picked;
		}

		/* Random levels: mostly +-1, some small, some large, and the extremes now and then. */
		memset(expected, 0, sizeof(expected));
		for (slot = 0; slot < count; slot++) {
			kind = random_next() % 20U;
			if (kind < 10U) {
				value = 1;
			} else if (kind < 15U) {
				value = (int32_t)(random_next() % 4U) + 1;
			} else if (kind < 18U) {
				value = (int32_t)(random_next() % 300U) + 1;
			} else if (kind < 19U) {
				value = (int32_t)(random_next() % 32767U) + 1;
			} else {
				value = 32767;
			}
			if ((random_next() & 1U) != 0U)
				value = -value;
			if (kind == 19U && (random_next() & 1U) != 0U)
				value = -32768;
			expected[pool[slot]] = value;
		}

		/* Writes the block, then a stop bit, and reads it back. */
		bitwriter_init(&writer);
		put_block(&writer, nc, start, end, max_coeff, expected);
		block_bits = writer.bits;
		bitwriter_trailing(&writer);
		memset(levels, 0x55, sizeof(levels));
		total = 99U;
		error = read_block(&writer, bitwriter_bytes(&writer), nc, start, end, max_coeff, levels, &total, &end_position);

		/* The block reads as written and the reader stops at the last bit of the block. */
		CHECK(error == 0);
		CHECK(total == count);
		same = 1;
		for (index = 0; index < max_coeff; index++) {
			if (levels[index] != expected[index])
				same = 0;
		}
		CHECK(same);
		CHECK(end_position == block_bits);
	}
}

/* Checks that a block cut short, at every byte boundary before its end, is an error. */
static void
check_truncated(void)
{
	unsigned index;
	unsigned count;
	size_t cut;
	size_t bytes;
	int32_t levels[16];
	unsigned total;
	size_t end_position;

	/* Every hand-made block with some bits, cut at each shorter length in bytes. */
	count = (unsigned)(sizeof(hand_blocks) / sizeof(hand_blocks[0]));
	for (index = 0; index < count; index++) {
		bitwriter_init(&writer);
		put_bits(&writer, hand_blocks[index].bits);
		bytes = (bits_length(hand_blocks[index].bits) + 7U) / 8U;
		for (cut = 0; cut < bytes; cut++) {
			/* A cut inside the last byte still holds all the bits of a block of 8 bits or fewer; only cuts that remove bits count. */
			if (cut * 8U >= bits_length(hand_blocks[index].bits))
				continue;
			CHECK(read_block(&writer, cut, hand_blocks[index].nc, hand_blocks[index].start, hand_blocks[index].end, hand_blocks[index].max_coeff, levels, &total, &end_position) == EINVAL);
		}
	}
}

/* Checks the errors of a whole block: bad shape, too many coefficients, bad zeros, bad levels. */
static void
check_block_errors(void)
{
	int32_t levels[16];
	unsigned total;
	size_t end_position;
	struct vcodec_bitreader bits;
	unsigned prefix;

	/* A missing reader, levels or count, and a block shape no syntax produces. */
	bitwriter_init(&writer);
	put_bits(&writer, "1");
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	CHECK(h264sw_cavlc_residual_block(NULL, 0, 0U, 15U, 16U, levels, &total) == EINVAL);
	CHECK(h264sw_cavlc_residual_block(&bits, 0, 0U, 15U, 16U, NULL, &total) == EINVAL);
	CHECK(h264sw_cavlc_residual_block(&bits, 0, 0U, 15U, 16U, levels, NULL) == EINVAL);
	CHECK(h264sw_cavlc_residual_block(&bits, 0, 0U, 15U, 8U, levels, &total) == EINVAL);
	CHECK(h264sw_cavlc_residual_block(&bits, 0, 0U, 15U, 64U, levels, &total) == EINVAL);
	CHECK(h264sw_cavlc_residual_block(&bits, 0, 4U, 3U, 16U, levels, &total) == EINVAL);
	CHECK(h264sw_cavlc_residual_block(&bits, 0, 0U, 16U, 16U, levels, &total) == EINVAL);
	CHECK(h264sw_cavlc_residual_block(&bits, -2, 0U, 15U, 16U, levels, &total) == EINVAL);

	/* TotalCoeff 16 in a block of 15 coefficients (end 14) is not allowed. */
	bitwriter_init(&writer);
	put_bits(&writer, "0000 0000 0000 1000");
	bitwriter_u(&writer, 0U, 32U);
	CHECK(read_block(&writer, bitwriter_bytes(&writer), 0, 0U, 14U, 15U, levels, &total, &end_position) == EINVAL);

	/* TotalCoeff 5 with endIdx - startIdx + 1 = 4, and TotalCoeff 5 for the 4 coefficients of chroma DC (nC 0 code). */
	bitwriter_init(&writer);
	put_bits(&writer, "0000 100");
	bitwriter_u(&writer, 0U, 32U);
	CHECK(read_block(&writer, bitwriter_bytes(&writer), 0, 0U, 3U, 16U, levels, &total, &end_position) == EINVAL);
	CHECK(read_block(&writer, bitwriter_bytes(&writer), 0, 0U, 3U, 4U, levels, &total, &end_position) == EINVAL);

	/* total_zeros that pushes a coefficient past endIdx: two trailing ones (001, signs 00), total_zeros 14 (0000 00): fine in 16 coefficients, one too many in 15. */
	bitwriter_init(&writer);
	put_bits(&writer, "001 00 0000 00");
	bitwriter_u(&writer, 0xffffU, 16U);
	CHECK(read_block(&writer, bitwriter_bytes(&writer), 0, 0U, 14U, 15U, levels, &total, &end_position) == EINVAL);
	CHECK(read_block(&writer, bitwriter_bytes(&writer), 0, 0U, 15U, 16U, levels, &total, &end_position) == 0);
	CHECK(total == 2U);
	CHECK(levels[15] == 1 && levels[14] == 1 && levels[0] == 0);

	/* A run longer than the zeros left: three trailing ones (0001 1, signs 000), total_zeros 8 (0010), run_before 14. */
	bitwriter_init(&writer);
	put_bits(&writer, "0001 1 000 0010 0000 0000 001");
	bitwriter_u(&writer, 0xffffU, 16U);
	CHECK(read_block(&writer, bitwriter_bytes(&writer), 0, 0U, 15U, 16U, levels, &total, &end_position) == EINVAL);

	/* level_prefix of 26 is out of range; 25 is read but its level is out of range. */
	for (prefix = 25U; prefix <= 27U; prefix++) {
		bitwriter_init(&writer);
		put_bits(&writer, "0001 01");
		bitwriter_u(&writer, 0U, prefix);
		bitwriter_u(&writer, 1U, 1U);
		bitwriter_u(&writer, 0x3fffffU, 22U);
		bitwriter_u(&writer, 0xffffU, 16U);
		CHECK(read_block(&writer, bitwriter_bytes(&writer), 0, 0U, 15U, 16U, levels, &total, &end_position) == EINVAL);
	}

	/* A level_prefix with no 1 bit in the rest of the RBSP. */
	bitwriter_init(&writer);
	put_bits(&writer, "0001 01");
	bitwriter_u(&writer, 0U, 32U);
	bitwriter_u(&writer, 0U, 8U);
	CHECK(read_block(&writer, bitwriter_bytes(&writer), 0, 0U, 15U, 16U, levels, &total, &end_position) == EINVAL);

	/* The level 32768, one above the range: prefix 19 with a 16-bit suffix of 4062 (levelCode 65532 + 2 = 65534). */
	bitwriter_init(&writer);
	put_bits(&writer, "0001 01");
	bitwriter_u(&writer, 0U, 19U);
	bitwriter_u(&writer, 1U, 1U);
	bitwriter_u(&writer, 4062U, 16U);
	put_bits(&writer, "1");
	CHECK(read_block(&writer, bitwriter_bytes(&writer), 0, 0U, 15U, 16U, levels, &total, &end_position) == EINVAL);

	/* The level 32767 is the largest one accepted: levelCode 65532 gives 32767, prefix 19 with suffix 4060. */
	bitwriter_init(&writer);
	put_bits(&writer, "0001 01");
	bitwriter_u(&writer, 0U, 19U);
	bitwriter_u(&writer, 1U, 1U);
	bitwriter_u(&writer, 4060U, 16U);
	put_bits(&writer, "1");
	CHECK(read_block(&writer, bitwriter_bytes(&writer), 0, 0U, 15U, 16U, levels, &total, &end_position) == 0);
	CHECK(levels[0] == 32767);

	/* A reader that already failed gives an error and leaves zero levels. */
	bitwriter_init(&writer);
	put_bits(&writer, "1");
	vcodec_bitreader_init(&bits, writer.data, bitwriter_bytes(&writer));
	bits.error = EINVAL;
	memset(levels, 0x55, sizeof(levels));
	CHECK(h264sw_cavlc_residual_block(&bits, 0, 0U, 15U, 16U, levels, &total) == EINVAL);
}

/* Reads random bytes with random shapes: only success or EINVAL, a bounded count and levels in range. */
static void
check_fuzz(void)
{
	unsigned round;
	unsigned length;
	unsigned index;
	unsigned start;
	unsigned end;
	unsigned max_coeff;
	unsigned total;
	size_t end_position;
	int nc;
	int error;
	int32_t levels[16];
	int bad;
	static const unsigned maxes[3] = { 4U, 15U, 16U };

	/* Many random inputs; the sanitizers watch for reads outside the buffer and bad arithmetic. */
	for (round = 0; round < 40000U; round++) {
		bitwriter_init(&writer);
		length = random_next() % 24U;
		for (index = 0; index < length; index++)
			bitwriter_u(&writer, random_next() & 0xffU, 8U);

		/* Often a few set bits and mostly zeros, which reach the long codes. */
		if ((random_next() & 3U) == 0U) {
			bitwriter_init(&writer);
			for (index = 0; index < length; index++) {
				if ((random_next() & 7U) == 0U) {
					bitwriter_u(&writer, random_next() & 0xffU, 8U);
				} else {
					bitwriter_u(&writer, 0U, 8U);
				}
			}
		}

		max_coeff = maxes[random_next() % 3U];
		end = random_next() % max_coeff;
		start = random_next() % (end + 1U);
		nc = (int)(random_next() % 20U) - 3;
		total = 99U;
		error = read_block(&writer, length, nc, start, end, max_coeff, levels, &total, &end_position);

		/* Only the two answers, and a sane result when it worked. */
		CHECK(error == 0 || error == EINVAL);
		if (error == 0) {
			bad = 0;
			if (total > end - start + 1U)
				bad = 1;
			for (index = 0; index < max_coeff; index++) {
				if (levels[index] > 32767 || levels[index] < -32768)
					bad = 1;
				if (index < start || index > end) {
					if (levels[index] != 0)
						bad = 1;
				}
			}
			CHECK(!bad);
			CHECK(end_position <= (size_t)length * 8U);
		}
	}
}

/*
 * Prepares the 3 by 3 picture for an nC test: the current macroblock addr
 * is in slice 0, its left and upper macroblocks (where the picture has
 * them) in the given slices, -1 for not yet decoded.
 */
static void
setup_nc(
	unsigned addr,
	int32_t a_slice,
	int32_t b_slice)
{
	unsigned index;

	/* Every macroblock starts undecoded; the current one and its neighbours get their slices. */
	for (index = 0; index < 9U; index++)
		h264sw_mb_reset(&nc_mbs[index], -1);
	h264sw_mb_reset(&nc_mbs[addr], 0);
	if (addr % 3U != 0U)
		h264sw_mb_reset(&nc_mbs[addr - 1U], a_slice);
	if (addr >= 3U)
		h264sw_mb_reset(&nc_mbs[addr - 3U], b_slice);

	/* Fills the neighbour description the function reads. */
	memset(&nc_ctx, 0, sizeof(nc_ctx));
	h264sw_mb_neighbours(nc_mbs, 3U, addr, &nc_ctx.n);
}

/* Checks the context nC: availability, the rounded mean, skipped and I_PCM macroblocks and every kind of block. */
static void
check_nc(void)
{
	unsigned index;

	/* The centre macroblock (address 4) has the left macroblock 3 and the upper macroblock 1. */
	setup_nc(4U, 0, 0);

	/* Block 0: A is block 5 of the left macroblock and B block 10 of the upper one; (3 + 6 + 1) >> 1 is 5. */
	nc_mbs[3].total_coeff[5] = 3U;
	nc_mbs[1].total_coeff[10] = 6U;
	CHECK(h264sw_cavlc_nc(&nc_ctx, 0U) == 5);

	/* The mean rounds up: (2 + 3 + 1) >> 1 is 3. */
	nc_mbs[3].total_coeff[5] = 2U;
	nc_mbs[1].total_coeff[10] = 3U;
	CHECK(h264sw_cavlc_nc(&nc_ctx, 0U) == 3);

	/* A skipped macroblock counts 0: (0 + 8 + 1) >> 1 is 4 (the parser keeps 0 for it). */
	nc_mbs[3].total_coeff[5] = 0U;
	nc_mbs[1].total_coeff[10] = 8U;
	CHECK(h264sw_cavlc_nc(&nc_ctx, 0U) == 4);

	/* I_PCM counts 16 (the parser keeps 16 for it): 16 and 16 give 16, 16 and 0 give 8. */
	nc_mbs[3].total_coeff[5] = 16U;
	nc_mbs[1].total_coeff[10] = 16U;
	CHECK(h264sw_cavlc_nc(&nc_ctx, 0U) == 16);
	nc_mbs[1].total_coeff[10] = 0U;
	CHECK(h264sw_cavlc_nc(&nc_ctx, 0U) == 8);

	/* Block 3 has both neighbours in the current macroblock: A is block 2 and B block 1; (5 + 9 + 1) >> 1 is 7. */
	nc_mbs[4].total_coeff[2] = 5U;
	nc_mbs[4].total_coeff[1] = 9U;
	CHECK(h264sw_cavlc_nc(&nc_ctx, 3U) == 7);

	/* Block 5 has A in the current macroblock (block 4) and B in the upper one (block 15). */
	nc_mbs[4].total_coeff[4] = 4U;
	nc_mbs[1].total_coeff[15] = 7U;
	CHECK(h264sw_cavlc_nc(&nc_ctx, 5U) == 6);

	/* Block 2 has A in the left macroblock (block 7) and B in the current macroblock (block 0). */
	nc_mbs[3].total_coeff[7] = 10U;
	nc_mbs[4].total_coeff[0] = 3U;
	CHECK(h264sw_cavlc_nc(&nc_ctx, 2U) == 7);

	/* The Cb AC block 0 (block index 16) has A in chroma block 1 and B in chroma block 2 of the neighbours (counts kept at 16 + n). */
	nc_mbs[3].total_coeff[17] = 2U;
	nc_mbs[1].total_coeff[18] = 5U;
	nc_mbs[3].total_coeff[21] = 9U;
	nc_mbs[1].total_coeff[22] = 12U;
	CHECK(h264sw_cavlc_nc(&nc_ctx, 16U) == 4);

	/* The Cr AC block 0 (index 20) reads the counts kept at 20 + n: (9 + 12 + 1) >> 1 is 11. */
	CHECK(h264sw_cavlc_nc(&nc_ctx, 20U) == 11);

	/* The chroma block 3 (index 19) has A and B in the current macroblock: chroma blocks 2 and 1. */
	nc_mbs[4].total_coeff[18] = 6U;
	nc_mbs[4].total_coeff[17] = 1U;
	CHECK(h264sw_cavlc_nc(&nc_ctx, 19U) == 4);
	nc_mbs[4].total_coeff[22] = 2U;
	nc_mbs[4].total_coeff[21] = 7U;
	CHECK(h264sw_cavlc_nc(&nc_ctx, 23U) == 5);

	/* A block index no macroblock has gives no neighbours. */
	CHECK(h264sw_cavlc_nc(&nc_ctx, 24U) == 0);
	CHECK(h264sw_cavlc_nc(&nc_ctx, 1000U) == 0);
	CHECK(h264sw_cavlc_nc(NULL, 0U) == 0);

	/* Only the left neighbour is available (address 1 is in the top row: A is macroblock 0, B is outside). Block 2 also has B in the current macroblock (block 0, count 0): (4 + 0 + 1) >> 1 is 2. */
	setup_nc(1U, 0, 0);
	nc_mbs[0].total_coeff[5] = 9U;
	CHECK(h264sw_cavlc_nc(&nc_ctx, 0U) == 9);
	nc_mbs[0].total_coeff[7] = 4U;
	CHECK(h264sw_cavlc_nc(&nc_ctx, 2U) == 2);

	/* Only the upper neighbour is available (address 3 has no left neighbour). */
	setup_nc(3U, 0, 0);
	nc_mbs[0].total_coeff[10] = 7U;
	CHECK(h264sw_cavlc_nc(&nc_ctx, 0U) == 7);

	/* Neighbours of another slice are not available: the left one is decoded in slice 1 here, the upper one in slice 0. */
	setup_nc(4U, 1, 0);
	nc_mbs[3].total_coeff[5] = 12U;
	nc_mbs[1].total_coeff[10] = 3U;
	CHECK(h264sw_cavlc_nc(&nc_ctx, 0U) == 3);

	/* No neighbour at all: the first macroblock, block 0, and the first blocks of both chroma kinds. */
	setup_nc(0U, 0, 0);
	CHECK(h264sw_cavlc_nc(&nc_ctx, 0U) == 0);
	CHECK(h264sw_cavlc_nc(&nc_ctx, 16U) == 0);
	CHECK(h264sw_cavlc_nc(&nc_ctx, 20U) == 0);

	/* The neighbours inside the current macroblock count even with no neighbouring macroblock: block 1 has A in block 0. */
	nc_mbs[0].total_coeff[0] = 5U;
	CHECK(h264sw_cavlc_nc(&nc_ctx, 1U) == 5);

	/* Every luma block of a lone macroblock reads only itself or nothing. */
	for (index = 0; index < 16U; index++)
		nc_mbs[0].total_coeff[index] = 0U;
	for (index = 0; index < 24U; index++)
		CHECK(h264sw_cavlc_nc(&nc_ctx, index) == 0);
}

/* Checks the coded_block_pattern mapping of Table 9-4 (a) in full, and its errors. */
static void
check_cbp(void)
{
	unsigned code;
	unsigned cbp;
	unsigned seen_intra[48];
	unsigned seen_inter[48];

	/* Every codeNum of both columns against the table as read from the page. */
	memset(seen_intra, 0, sizeof(seen_intra));
	memset(seen_inter, 0, sizeof(seen_inter));
	for (code = 0; code < 48U; code++) {
		cbp = 99U;
		CHECK(h264sw_cavlc_cbp(code, 1, &cbp) == 0);
		CHECK(cbp == cbp_intra_expected[code]);
		if (cbp < 48U)
			seen_intra[cbp]++;

		cbp = 99U;
		CHECK(h264sw_cavlc_cbp(code, 0, &cbp) == 0);
		CHECK(cbp == cbp_inter_expected[code]);
		if (cbp < 48U)
			seen_inter[cbp]++;
	}

	/* Each column is a permutation of the 48 values of luma 0 to 15 and chroma 0 to 2. */
	for (code = 0; code < 48U; code++) {
		CHECK(seen_intra[code] == 1U);
		CHECK(seen_inter[code] == 1U);
	}

	/* A nonzero intra flag other than 1 is still intra, and codeNum 48 and above is an error that leaves the output. */
	cbp = 99U;
	CHECK(h264sw_cavlc_cbp(0U, 7, &cbp) == 0);
	CHECK(cbp == 47U);
	CHECK(h264sw_cavlc_cbp(48U, 1, &cbp) == EINVAL);
	CHECK(h264sw_cavlc_cbp(49U, 0, &cbp) == EINVAL);
	CHECK(h264sw_cavlc_cbp(0xffffffffUL, 0, &cbp) == EINVAL);
	CHECK(h264sw_cavlc_cbp(0U, 1, NULL) == EINVAL);
}
