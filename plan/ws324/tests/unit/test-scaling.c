/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tests of h265sw-scaling: the default lists (7.4.5, Tables 7-5 and 7-6) and
 * scaling_list_data() (7.3.4, 7.4.5).
 *
 * The expected default values are the 64-entry rows of Table 7-6, taken by
 * a script from the text of the page (pdftotext -layout) and checked against
 * an image of the page.  The other expected values are worked by hand from
 * 7.3.4 (nextCoef = ( nextCoef + scaling_list_delta_coef + 256 ) % 256) and
 * from 7-42 and 7-43; the inputs are written by the syntax writer of
 * h265write.h.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "h265write.h"
#include "userland/desktop/libmedia/h265sw-scaling.h"

/* The state of the pseudo-random generator, fixed so a failure repeats. */
static uint32_t random_state = 0x1B873593U;

/* The writer, the syntax values and the read result, too large for the stack of a check. */
static struct bitwriter writer;
static struct h265w_scaling syntax;
static struct h265sw_scaling_list result;
static struct h265sw_scaling_list expected;

/* The bytes of the last written scaling_list_data(), and the bits of the syntax before its trailing bits. */
static size_t last_bytes;
static size_t last_bits;

/*
 * The default lists of Table 7-6 for sizeId 1..3: the intra rows
 * (matrixId 0..2) and the inter rows (matrixId 3..5), 64 entries each.
 */
static const uint8_t spec_intra[64] = {
	16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 16, 17, 16, 17, 18,
	17, 18, 18, 17, 18, 21, 19, 20, 21, 20, 19, 21, 24, 22, 22, 24,
	24, 22, 22, 24, 25, 25, 27, 30, 27, 25, 25, 29, 31, 35, 35, 31,
	29, 36, 41, 44, 41, 36, 47, 54, 54, 47, 65, 70, 65, 88, 88, 115
};
static const uint8_t spec_inter[64] = {
	16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18,
	18, 18, 18, 18, 18, 20, 20, 20, 20, 20, 20, 20, 24, 24, 24, 24,
	24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 28, 28, 28, 28, 28,
	28, 33, 33, 33, 33, 33, 41, 41, 41, 41, 54, 54, 54, 71, 71, 91
};

/* Static function declarations. */
static uint32_t random_next(void);
static unsigned list_entries(unsigned size_id);
static unsigned matrix_step(unsigned size_id);
static void default_list(unsigned size_id, unsigned matrix_id, uint8_t *out);
static void expected_default_all(struct h265sw_scaling_list *list);
static void syntax_flat(struct h265w_scaling *scaling);
static int read_syntax(const struct h265w_scaling *scaling, size_t cut);
static void check_default(void);
static void check_round_trip_lists(void);
static void check_round_trip_mixed(void);
static void check_hand_values(void);
static void check_ranges(void);
static void check_truncated(void);

/* Returns the next pseudo-random number (xorshift32). */
static uint32_t
random_next(void)
{
	/* The three shifts of the generator. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;

	/* Succeeded: the next number. */
	return random_state;
}

/* Returns coefNum of 7.3.4: 16 for sizeId 0, else 64. */
static unsigned
list_entries(
	unsigned size_id)
{
	/* Min( 64, 1 << ( 4 + ( sizeId << 1 ) ) ). */
	if (size_id == 0U)
		return 16U;

	/* The larger sizes. */
	return 64U;
}

/* Returns the step of the matrixId loop of 7.3.4: 3 for sizeId 3, else 1. */
static unsigned
matrix_step(
	unsigned size_id)
{
	/* The 32x32 size codes matrixId 0 and 3 only. */
	if (size_id == 3U)
		return 3U;

	/* The other sizes code all six. */
	return 1U;
}

/* Writes the default list of a size and matrix (Tables 7-5 and 7-6) into out, 64 bytes. */
static void
default_list(
	unsigned size_id,
	unsigned matrix_id,
	uint8_t *out)
{
	unsigned index;

	/* Everything outside the list's length is 0. */
	memset(out, 0, 64U);

	/* Table 7-5: sizeId 0 is flat 16. */
	if (size_id == 0U) {
		for (index = 0U; index < 16U; index++)
			out[index] = 16U;
		return;
	}

	/* Table 7-6: the intra rows for matrixId 0..2, the inter rows for 3..5. */
	if (matrix_id < 3U) {
		memcpy(out, spec_intra, 64U);
	} else {
		memcpy(out, spec_inter, 64U);
	}
}

/* Sets list to what the defaults are for every list the syntax codes, with DC 16 for sizeId 2 and 3. */
static void
expected_default_all(
	struct h265sw_scaling_list *list)
{
	unsigned size_id;
	unsigned matrix_id;

	/* Starts with every entry 0. */
	memset(list, 0, sizeof(*list));

	/* Each coded list takes its default. */
	for (size_id = 0U; size_id < 4U; size_id++) {
		for (matrix_id = 0U; matrix_id < 6U; matrix_id += matrix_step(size_id)) {
			default_list(size_id, matrix_id, list->list[size_id][matrix_id]);
			if (size_id > 1U)
				list->dc[size_id - 2U][matrix_id] = 16U;
		}
	}
}

/* Sets scaling to every list signalled with all deltas 0 (all entries 8, or the DC 8 for the larger sizes). */
static void
syntax_flat(
	struct h265w_scaling *scaling)
{
	unsigned size_id;
	unsigned matrix_id;

	/* Every coded list has pred_mode 1 and no delta. */
	memset(scaling, 0, sizeof(*scaling));
	for (size_id = 0U; size_id < 4U; size_id++) {
		for (matrix_id = 0U; matrix_id < 6U; matrix_id += matrix_step(size_id))
			scaling->pred_mode[size_id][matrix_id] = 1U;
	}
}

/*
 * Writes scaling as scaling_list_data() and reads it into result, using
 * only the first cut bytes when cut is shorter than the data.  Returns what
 * the reader returns.
 */
static int
read_syntax(
	const struct h265w_scaling *scaling,
	size_t cut)
{
	struct vcodec_bitreader bits;
	size_t bytes;

	/* Writes the syntax and its trailing bits. */
	bitwriter_init(&writer);
	h265w_scaling(&writer, scaling);
	last_bits = writer.bits;
	bitwriter_trailing(&writer);
	bytes = bitwriter_bytes(&writer);
	last_bytes = bytes;
	if (cut < bytes)
		bytes = cut;

	/* Reads it back into a structure that holds a value the reader must overwrite. */
	memset(&result, 0xCC, sizeof(result));
	vcodec_bitreader_init(&bits, writer.data, bytes);

	/* Succeeded: the reader's answer. */
	return h265sw_scaling_read(&result, &bits);
}

/* Checks the defaults against Tables 7-5 and 7-6 and the entries outside the lists. */
static void
check_default(void)
{
	struct h265sw_scaling_list list;
	unsigned size_id;
	unsigned matrix_id;
	unsigned index;
	uint8_t want[64];

	/* The function overwrites a structure filled with another value. */
	memset(&list, 0xFF, sizeof(list));
	h265sw_scaling_default(&list);

	/* Every entry: the default for a coded list, 0 elsewhere. */
	for (size_id = 0U; size_id < 4U; size_id++) {
		for (matrix_id = 0U; matrix_id < 6U; matrix_id++) {
			if (size_id == 3U && (matrix_id % 3U) != 0U) {
				memset(want, 0, sizeof(want));
			} else {
				default_list(size_id, matrix_id, want);
			}
			for (index = 0U; index < 64U; index++)
				CHECK(list.list[size_id][matrix_id][index] == want[index]);
		}
	}

	/* The DC values: 16 for the coded lists of sizeId 2 and 3, 0 for the others. */
	for (matrix_id = 0U; matrix_id < 6U; matrix_id++) {
		CHECK(list.dc[0][matrix_id] == 16U);
		if ((matrix_id % 3U) == 0U) {
			CHECK(list.dc[1][matrix_id] == 16U);
		} else {
			CHECK(list.dc[1][matrix_id] == 0U);
		}
	}

	/* Spot checks of Table 7-6 by hand: the first, one in the middle and the last entries. */
	CHECK(list.list[1][0][0] == 16U);
	CHECK(list.list[1][0][10] == 17U);
	CHECK(list.list[1][0][15] == 18U);
	CHECK(list.list[2][1][63] == 115U);
	CHECK(list.list[3][3][63] == 91U);
	CHECK(list.list[3][3][12] == 17U);
	CHECK(list.list[1][5][47] == 28U);
	CHECK(list.list[0][4][15] == 16U);
	CHECK(list.list[0][4][16] == 0U);
}

/* Checks signalled lists of random values, including the extremes, through the syntax. */
static void
check_round_trip_lists(void)
{
	uint8_t list[4][6][64];
	uint8_t dc[2][6];
	unsigned round;
	unsigned size_id;
	unsigned matrix_id;
	unsigned index;
	unsigned mode;
	int error;

	/* Several patterns of target values, each written as deltas and read back. */
	for (round = 0U; round < 300U; round++) {
		mode = round % 6U;
		memset(list, 0, sizeof(list));
		memset(dc, 0, sizeof(dc));
		for (size_id = 0U; size_id < 4U; size_id++) {
			for (matrix_id = 0U; matrix_id < 6U; matrix_id += matrix_step(size_id)) {
				for (index = 0U; index < list_entries(size_id); index++) {
					if (mode == 0U) {
						list[size_id][matrix_id][index] = (uint8_t)(1U + random_next() % 255U);
					} else if (mode == 1U) {
						list[size_id][matrix_id][index] = (index % 2U == 0U) ? 1U : 255U;
					} else if (mode == 2U) {
						list[size_id][matrix_id][index] = (index % 2U == 0U) ? 8U : 136U;
					} else if (mode == 3U) {
						list[size_id][matrix_id][index] = 255U;
					} else if (mode == 4U) {
						list[size_id][matrix_id][index] = 1U;
					} else {
						list[size_id][matrix_id][index] = (uint8_t)(1U + (index * 7U + round) % 255U);
					}
				}
				if (size_id > 1U) {
					if (mode == 1U) {
						dc[size_id - 2U][matrix_id] = 255U;
					} else if (mode == 4U) {
						dc[size_id - 2U][matrix_id] = 1U;
					} else {
						dc[size_id - 2U][matrix_id] = (uint8_t)(1U + random_next() % 255U);
					}
				}
			}
		}

		/* The expected structure is the target values, with 0 outside the coded lists. */
		h265w_scaling_from_lists(&syntax, (const uint8_t (*)[6][64])list, (const uint8_t (*)[6])dc);
		memset(&expected, 0, sizeof(expected));
		memcpy(expected.list, list, sizeof(list));
		memcpy(expected.dc, dc, sizeof(dc));
		error = read_syntax(&syntax, (size_t)-1);
		CHECK(error == 0);
		CHECK(memcmp(&result, &expected, sizeof(result)) == 0);
	}
}

/* Checks a mix of default, referenced and signalled lists against a worked-out expectation. */
static void
check_round_trip_mixed(void)
{
	uint8_t list[4][6][64];
	uint8_t dc[2][6];
	unsigned round;
	unsigned size_id;
	unsigned matrix_id;
	unsigned index;
	unsigned step;
	unsigned choice;
	unsigned max_delta;
	unsigned delta;
	unsigned reference;
	uint8_t reference_list[64];
	int error;

	/* Each round gives every coded list one of the three kinds. */
	for (round = 0U; round < 400U; round++) {
		/* The target values of the signalled lists. */
		memset(list, 0, sizeof(list));
		memset(dc, 0, sizeof(dc));
		for (size_id = 0U; size_id < 4U; size_id++) {
			for (matrix_id = 0U; matrix_id < 6U; matrix_id += matrix_step(size_id)) {
				for (index = 0U; index < list_entries(size_id); index++)
					list[size_id][matrix_id][index] = (uint8_t)(1U + random_next() % 255U);
				if (size_id > 1U)
					dc[size_id - 2U][matrix_id] = (uint8_t)(1U + random_next() % 255U);
			}
		}
		h265w_scaling_from_lists(&syntax, (const uint8_t (*)[6][64])list, (const uint8_t (*)[6])dc);

		/* Chooses the kind of each list in the order the syntax codes them, so a reference is final. */
		memset(&expected, 0, sizeof(expected));
		for (size_id = 0U; size_id < 4U; size_id++) {
			step = matrix_step(size_id);
			for (matrix_id = 0U; matrix_id < 6U; matrix_id += step) {
				choice = random_next() % 3U;
				max_delta = (size_id == 3U) ? matrix_id / 3U : matrix_id;
				if (choice == 1U && max_delta == 0U)
					choice = 0U;

				if (choice == 2U) {
					/* Signalled: the list as written. */
					memcpy(expected.list[size_id][matrix_id], list[size_id][matrix_id], 64U);
					if (size_id > 1U)
						expected.dc[size_id - 2U][matrix_id] = dc[size_id - 2U][matrix_id];
				} else if (choice == 0U) {
					/* Predicted with delta 0: the default list, DC 16 (7.4.5). */
					syntax.pred_mode[size_id][matrix_id] = 0U;
					syntax.pred_matrix_id_delta[size_id][matrix_id] = 0U;
					default_list(size_id, matrix_id, expected.list[size_id][matrix_id]);
					if (size_id > 1U)
						expected.dc[size_id - 2U][matrix_id] = 16U;
				} else {
					/* Predicted from an earlier list: refMatrixId of 7-42, the list and DC of 7-43 and 7.4.5. */
					delta = 1U + random_next() % max_delta;
					syntax.pred_mode[size_id][matrix_id] = 0U;
					syntax.pred_matrix_id_delta[size_id][matrix_id] = delta;
					reference = matrix_id - delta * step;
					memcpy(reference_list, expected.list[size_id][reference], 64U);
					memcpy(expected.list[size_id][matrix_id], reference_list, 64U);
					if (size_id > 1U)
						expected.dc[size_id - 2U][matrix_id] = expected.dc[size_id - 2U][reference];
				}
			}
		}

		error = read_syntax(&syntax, (size_t)-1);
		CHECK(error == 0);
		CHECK(memcmp(&result, &expected, sizeof(result)) == 0);
	}
}

/* Checks values worked out by hand: the wrap of nextCoef and the start from the DC. */
static void
check_hand_values(void)
{
	int error;
	unsigned index;

	/* All lists flat; then lists with big steps. */
	syntax_flat(&syntax);

	/* From 8 with +127: 135, then 262 % 256 = 6, 133, 4. */
	syntax.delta_coef[0][0][0] = 127;
	syntax.delta_coef[0][0][1] = 127;
	syntax.delta_coef[0][0][2] = 127;
	syntax.delta_coef[0][0][3] = 127;

	/* From 8 with -127: 8 - 127 + 256 = 137, 137 - 127 = 10, 10 - 127 + 256 = 139, 12. */
	syntax.delta_coef[0][1][0] = -127;
	syntax.delta_coef[0][1][1] = -127;
	syntax.delta_coef[0][1][2] = -127;
	syntax.delta_coef[0][1][3] = -127;

	/* sizeId 2: DC 255 (dc_coef_minus8 247), then +2 gives 257 % 256 = 1, then 0 keeps 1. */
	syntax.dc_coef_minus8[0][0] = 247;
	syntax.delta_coef[2][0][0] = 2;

	/* sizeId 2: DC 1 (dc_coef_minus8 -7), then -128 gives 1 - 128 + 256 = 129, then +127 gives 256 % 256 = 0 ... avoided: +126 gives 255. */
	syntax.dc_coef_minus8[0][1] = -7;
	syntax.delta_coef[2][1][0] = -128;
	syntax.delta_coef[2][1][1] = 126;

	/* sizeId 3 matrixId 3: DC 100 (dc_coef_minus8 92), first entry -1 gives 99. */
	syntax.dc_coef_minus8[1][3] = 92;
	syntax.delta_coef[3][3][0] = -1;

	error = read_syntax(&syntax, (size_t)-1);
	CHECK(error == 0);
	CHECK(result.list[0][0][0] == 135U);
	CHECK(result.list[0][0][1] == 6U);
	CHECK(result.list[0][0][2] == 133U);
	CHECK(result.list[0][0][3] == 4U);
	CHECK(result.list[0][0][4] == 4U);
	CHECK(result.list[0][1][0] == 137U);
	CHECK(result.list[0][1][1] == 10U);
	CHECK(result.list[0][1][2] == 139U);
	CHECK(result.list[0][1][3] == 12U);
	CHECK(result.dc[0][0] == 255U);
	CHECK(result.list[2][0][0] == 1U);
	CHECK(result.list[2][0][1] == 1U);
	CHECK(result.dc[0][1] == 1U);
	CHECK(result.list[2][1][0] == 129U);
	CHECK(result.list[2][1][1] == 255U);
	CHECK(result.dc[1][3] == 100U);
	CHECK(result.list[3][3][0] == 99U);
	CHECK(result.list[3][3][1] == 99U);

	/* The unsignalled sizeId 0 lists keep their 16 entries and the rest of the arrays is 0. */
	for (index = 16U; index < 64U; index++)
		CHECK(result.list[0][0][index] == 0U);
	CHECK(result.list[3][1][0] == 0U);
	CHECK(result.list[3][2][5] == 0U);
	CHECK(result.list[3][4][63] == 0U);
	CHECK(result.dc[1][1] == 0U);
	CHECK(result.dc[1][5] == 0U);

	/* The defaults a predicted list takes also match a read of an all-default stream. */
	memset(&syntax, 0, sizeof(syntax));
	error = read_syntax(&syntax, (size_t)-1);
	CHECK(error == 0);
	expected_default_all(&expected);
	CHECK(memcmp(&result, &expected, sizeof(result)) == 0);
}

/* Checks the range of each value at its edge and one past it. */
static void
check_ranges(void)
{
	unsigned size_id;
	unsigned matrix_id;
	unsigned step;
	unsigned max_delta;
	unsigned entries;
	unsigned positions[3];
	unsigned which;
	int error;

	/* pred_matrix_id_delta: matrixId (matrixId / 3 for sizeId 3) is valid, one more is not. */
	for (size_id = 0U; size_id < 4U; size_id++) {
		step = matrix_step(size_id);
		for (matrix_id = 0U; matrix_id < 6U; matrix_id += step) {
			max_delta = (size_id == 3U) ? matrix_id / 3U : matrix_id;
			memset(&syntax, 0, sizeof(syntax));
			syntax.pred_matrix_id_delta[size_id][matrix_id] = max_delta;
			error = read_syntax(&syntax, (size_t)-1);
			CHECK(error == 0);
			syntax.pred_matrix_id_delta[size_id][matrix_id] = max_delta + 1U;
			error = read_syntax(&syntax, (size_t)-1);
			CHECK(error == EINVAL);
			syntax.pred_matrix_id_delta[size_id][matrix_id] = 0xFFFFFFFEU;
			error = read_syntax(&syntax, (size_t)-1);
			CHECK(error == EINVAL);
		}
	}

	/* scaling_list_dc_coef_minus8: -7 and 247 are valid, -8 and 248 are not. */
	for (size_id = 2U; size_id < 4U; size_id++) {
		for (matrix_id = 0U; matrix_id < 6U; matrix_id += matrix_step(size_id)) {
			syntax_flat(&syntax);
			syntax.dc_coef_minus8[size_id - 2U][matrix_id] = -7;
			error = read_syntax(&syntax, (size_t)-1);
			CHECK(error == 0);
			CHECK(result.dc[size_id - 2U][matrix_id] == 1U);
			CHECK(result.list[size_id][matrix_id][0] == 1U);
			syntax.dc_coef_minus8[size_id - 2U][matrix_id] = 247;
			error = read_syntax(&syntax, (size_t)-1);
			CHECK(error == 0);
			CHECK(result.dc[size_id - 2U][matrix_id] == 255U);
			CHECK(result.list[size_id][matrix_id][63] == 255U);
			syntax.dc_coef_minus8[size_id - 2U][matrix_id] = -8;
			error = read_syntax(&syntax, (size_t)-1);
			CHECK(error == EINVAL);
			syntax.dc_coef_minus8[size_id - 2U][matrix_id] = 248;
			error = read_syntax(&syntax, (size_t)-1);
			CHECK(error == EINVAL);
			syntax.dc_coef_minus8[size_id - 2U][matrix_id] = 2147483647;
			error = read_syntax(&syntax, (size_t)-1);
			CHECK(error == EINVAL);
			syntax.dc_coef_minus8[size_id - 2U][matrix_id] = -2147483647;
			error = read_syntax(&syntax, (size_t)-1);
			CHECK(error == EINVAL);
		}
	}

	/* scaling_list_delta_coef: -128 and 127 are valid, -129 and 128 are not, at the first, a middle and the last entry. */
	for (size_id = 0U; size_id < 4U; size_id++) {
		entries = list_entries(size_id);
		positions[0] = 0U;
		positions[1] = entries / 2U;
		positions[2] = entries - 1U;
		for (matrix_id = 0U; matrix_id < 6U; matrix_id += matrix_step(size_id)) {
			for (which = 0U; which < 3U; which++) {
				syntax_flat(&syntax);
				syntax.delta_coef[size_id][matrix_id][positions[which]] = -128;
				error = read_syntax(&syntax, (size_t)-1);
				CHECK(error == 0);
				CHECK(result.list[size_id][matrix_id][positions[which]] == 136U);
				syntax.delta_coef[size_id][matrix_id][positions[which]] = 127;
				error = read_syntax(&syntax, (size_t)-1);
				CHECK(error == 0);
				CHECK(result.list[size_id][matrix_id][positions[which]] == 135U);
				syntax.delta_coef[size_id][matrix_id][positions[which]] = -129;
				error = read_syntax(&syntax, (size_t)-1);
				CHECK(error == EINVAL);
				syntax.delta_coef[size_id][matrix_id][positions[which]] = 128;
				error = read_syntax(&syntax, (size_t)-1);
				CHECK(error == EINVAL);

				/* A step of -8 from the flat 8 makes the entry 0, which 7.4.5 forbids; -7 makes it 1. */
				syntax_flat(&syntax);
				syntax.delta_coef[size_id][matrix_id][positions[which]] = -8;
				error = read_syntax(&syntax, (size_t)-1);
				CHECK(error == EINVAL);
				syntax.delta_coef[size_id][matrix_id][positions[which]] = -7;
				error = read_syntax(&syntax, (size_t)-1);
				CHECK(error == 0);
				CHECK(result.list[size_id][matrix_id][positions[which]] == 1U);
			}
		}
	}

	/* An entry that wraps to 0 from the DC: DC 255 plus 1 is 256 % 256 = 0, refused; plus 2 is 1. */
	syntax_flat(&syntax);
	syntax.dc_coef_minus8[0][2] = 247;
	syntax.delta_coef[2][2][0] = 1;
	error = read_syntax(&syntax, (size_t)-1);
	CHECK(error == EINVAL);
	syntax.delta_coef[2][2][0] = 2;
	error = read_syntax(&syntax, (size_t)-1);
	CHECK(error == 0);
	CHECK(result.list[2][2][0] == 1U);

	/* DC 1 minus 1 is 0, refused. */
	syntax_flat(&syntax);
	syntax.dc_coef_minus8[1][0] = -7;
	syntax.delta_coef[3][0][0] = -1;
	error = read_syntax(&syntax, (size_t)-1);
	CHECK(error == EINVAL);
}

/* Checks that every shorter RBSP is refused. */
static void
check_truncated(void)
{
	uint8_t list[4][6][64];
	uint8_t dc[2][6];
	size_t cut;
	size_t bytes;
	size_t bits;
	unsigned size_id;
	unsigned matrix_id;
	unsigned index;
	unsigned kind;
	int error;

	/* Three streams: all flat, all random signalled, all predicted. */
	for (kind = 0U; kind < 3U; kind++) {
		if (kind == 0U) {
			syntax_flat(&syntax);
		} else if (kind == 1U) {
			memset(list, 0, sizeof(list));
			memset(dc, 0, sizeof(dc));
			for (size_id = 0U; size_id < 4U; size_id++) {
				for (matrix_id = 0U; matrix_id < 6U; matrix_id += matrix_step(size_id)) {
					for (index = 0U; index < list_entries(size_id); index++)
						list[size_id][matrix_id][index] = (uint8_t)(1U + random_next() % 255U);
					dc[0][matrix_id] = (uint8_t)(1U + random_next() % 255U);
					dc[1][matrix_id] = (uint8_t)(1U + random_next() % 255U);
				}
			}
			h265w_scaling_from_lists(&syntax, (const uint8_t (*)[6][64])list, (const uint8_t (*)[6])dc);
		} else {
			memset(&syntax, 0, sizeof(syntax));
		}

		/* The complete stream reads; a shorter one is refused unless it still holds all the syntax bits. */
		error = read_syntax(&syntax, (size_t)-1);
		CHECK(error == 0);
		bytes = last_bytes;
		bits = last_bits;
		for (cut = 0U; cut < bytes; cut++) {
			error = read_syntax(&syntax, cut);
			if (cut * 8U < bits) {
				CHECK(error == EINVAL);
			} else {
				CHECK(error == 0);
			}
		}
	}
}

int
main(void)
{
	/* Runs every group of checks. */
	check_default();
	check_round_trip_lists();
	check_round_trip_mixed();
	check_hand_values();
	check_ranges();
	check_truncated();

	/* Reports the summary line. */
	return check_finish("scaling");
}
