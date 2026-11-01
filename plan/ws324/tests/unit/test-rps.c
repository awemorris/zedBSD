/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tests of h265sw-rps: st_ref_pic_set() (7.3.7) and the derivation of the
 * set (7.4.8, 7-59 to 7-71).
 *
 * The expected sets of the hand examples are worked by hand from 7-61 and
 * 7-62 (comments give the working).  The random predicted sets are checked
 * against a second formulation of the same rule: the entries that survive
 * are collected and sorted (negative ones from the nearest, positive ones
 * from the nearest), which is what 7-61 and 7-62 produce by walking the
 * source in a fixed order.  Inputs are written by the syntax writer of
 * h265write.h.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "h265write.h"
#include "userland/desktop/libmedia/h265sw-rps.h"

/* The state of the pseudo-random generator, fixed so a failure repeats. */
static uint32_t random_state = 0x2545F491U;

/* The writer, and the bits and bytes of the last written structure. */
static struct bitwriter writer;
static size_t last_bits;
static size_t last_bytes;

/* The most sets a test builds as an SPS's sets, and the sets the read works on. */
#define TEST_SETS 65U
static struct h265sw_st_rps sets[TEST_SETS];

/* Static function declarations. */
static uint32_t random_next(void);
static void set_make(struct h265sw_st_rps *set, unsigned num_negative, const int32_t *negative, const uint8_t *negative_used, unsigned num_positive, const int32_t *positive, const uint8_t *positive_used);
static int read_set(const struct h265w_st_rps *syntax, unsigned index, unsigned num_sets, const struct h265sw_st_rps *source, unsigned max_pictures, size_t cut, struct h265sw_st_rps *out, struct h265sw_st_rps_syntax *result);
static void predicted_syntax(struct h265w_st_rps *syntax, unsigned delta_idx_minus1, int negative_delta, unsigned abs_minus1, unsigned ref_count, uint32_t used_mask, uint32_t use_delta_mask);
static void check_explicit_counts(void);
static void check_explicit_values(void);
static void check_explicit_deltas(void);
static void check_predicted_hand(void);
static void check_predicted_random(void);
static void check_indexes(void);
static void check_syntax_fields(void);
static void check_arguments(void);
static void check_truncated(void);
static void check_garbage(void);

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

/* Builds a derived set: the negative entries (decreasing), then the positive ones (increasing). */
static void
set_make(
	struct h265sw_st_rps *set,
	unsigned num_negative,
	const int32_t *negative,
	const uint8_t *negative_used,
	unsigned num_positive,
	const int32_t *positive,
	const uint8_t *positive_used)
{
	unsigned index;

	/* Everything outside the entries is 0. */
	memset(set, 0, sizeof(*set));
	set->num_negative = num_negative;
	set->num_positive = num_positive;
	for (index = 0U; index < num_negative; index++) {
		set->delta_poc[index] = negative[index];
		set->used[index] = negative_used[index];
	}
	for (index = 0U; index < num_positive; index++) {
		set->delta_poc[num_negative + index] = positive[index];
		set->used[num_negative + index] = positive_used[index];
	}
}

/*
 * Writes syntax as st_ref_pic_set( index ) of an SPS with num_sets sets,
 * then trailing bits, and reads it with the first cut bytes (all when cut
 * is larger).  Returns what the reader returns.
 */
static int
read_set(
	const struct h265w_st_rps *syntax,
	unsigned index,
	unsigned num_sets,
	const struct h265sw_st_rps *source,
	unsigned max_pictures,
	size_t cut,
	struct h265sw_st_rps *out,
	struct h265sw_st_rps_syntax *result)
{
	struct vcodec_bitreader bits;
	size_t bytes;

	/* Writes the syntax and its trailing bits. */
	bitwriter_init(&writer);
	h265w_st_rps(&writer, syntax, index, num_sets);
	last_bits = writer.bits;
	bitwriter_trailing(&writer);
	bytes = bitwriter_bytes(&writer);
	last_bytes = bytes;
	if (cut < bytes)
		bytes = cut;

	/* Reads it back into structures that hold values the reader must overwrite. */
	memset(out, 0xCC, sizeof(*out));
	if (result != NULL)
		memset(result, 0xCC, sizeof(*result));
	vcodec_bitreader_init(&bits, writer.data, bytes);

	/* Succeeded: the reader's answer. */
	return h265sw_rps_read(&bits, index, num_sets, source, max_pictures, out, result);
}

/* Fills a predicted-form syntax; the flag arrays take bit j of the masks, use_delta only where used is 0. */
static void
predicted_syntax(
	struct h265w_st_rps *syntax,
	unsigned delta_idx_minus1,
	int negative_delta,
	unsigned abs_minus1,
	unsigned ref_count,
	uint32_t used_mask,
	uint32_t use_delta_mask)
{
	unsigned entry;

	/* The reference and the delta. */
	memset(syntax, 0, sizeof(*syntax));
	syntax->inter_rps_pred = 1U;
	syntax->delta_idx_minus1 = delta_idx_minus1;
	syntax->delta_rps_sign = (negative_delta != 0) ? 1U : 0U;
	syntax->abs_delta_rps_minus1 = abs_minus1;
	syntax->ref_num_delta_pocs = ref_count;

	/* One flag pair for each entry of the source and one more. */
	for (entry = 0U; entry <= ref_count; entry++) {
		syntax->used_by_curr_pic_flag[entry] = (used_mask >> entry) & 1U;
		syntax->use_delta_flag[entry] = (use_delta_mask >> entry) & 1U;
	}
}

/* Explicit sets: every pair of counts against the bounds of 7.4.8, for several max_pictures. */
static void
check_explicit_counts(void)
{
	static const unsigned limits[] = {0U, 1U, 2U, 5U, 14U, 15U};
	struct h265w_st_rps syntax;
	struct h265sw_st_rps out;
	struct h265sw_st_rps_syntax result;
	unsigned limit_index;
	unsigned max_pictures;
	unsigned num_negative;
	unsigned num_positive;
	unsigned entry;
	int error;
	int expected;

	/* Every pair of counts up to 16 each: valid when each fits and the sum fits. */
	for (limit_index = 0U; limit_index < sizeof(limits) / sizeof(limits[0]); limit_index++) {
		max_pictures = limits[limit_index];
		for (num_negative = 0U; num_negative <= 16U; num_negative++) {
			for (num_positive = 0U; num_positive <= 16U; num_positive++) {
				memset(&syntax, 0, sizeof(syntax));
				syntax.num_negative_pics = num_negative;
				syntax.num_positive_pics = num_positive;
				for (entry = 0U; entry < 16U; entry++) {
					syntax.delta_poc_s0_minus1[entry] = entry;
					syntax.delta_poc_s1_minus1[entry] = entry + 1U;
					syntax.used_by_curr_pic_s0_flag[entry] = entry & 1U;
					syntax.used_by_curr_pic_s1_flag[entry] = (entry + 1U) & 1U;
				}

				error = read_set(&syntax, 0U, 0U, NULL, max_pictures, (size_t)-1, &out, &result);
				expected = EINVAL;
				if (num_negative <= max_pictures && num_positive <= max_pictures - num_negative)
					expected = 0;
				CHECK(error == expected);
				if (expected == 0) {
					CHECK(out.num_negative == num_negative);
					CHECK(out.num_positive == num_positive);
					CHECK(result.bits == last_bits);
				}
			}
		}
	}
}

/* Explicit sets: the derived deltas and the signalled fields against hand-worked values (7-63 to 7-71). */
static void
check_explicit_values(void)
{
	struct h265w_st_rps syntax;
	struct h265sw_st_rps out;
	struct h265sw_st_rps expect;
	struct h265sw_st_rps_syntax result;
	struct h265sw_st_rps_syntax with_null;
	struct h265sw_st_rps out_null;
	struct vcodec_bitreader bits;
	int error;

	/*
	 * Three negative entries (delta minus1 0, 2, 0 with flags 1, 0, 1) and
	 * two positive ones (delta minus1 1, 3 with flags 0, 1):
	 * DeltaPocS0 = -1, -4, -5; DeltaPocS1 = 2, 6.
	 */
	{
		static const int32_t negative[] = {-1, -4, -5};
		static const uint8_t negative_used[] = {1, 0, 1};
		static const int32_t positive[] = {2, 6};
		static const uint8_t positive_used[] = {0, 1};

		memset(&syntax, 0, sizeof(syntax));
		syntax.num_negative_pics = 3U;
		syntax.num_positive_pics = 2U;
		syntax.delta_poc_s0_minus1[0] = 0U;
		syntax.delta_poc_s0_minus1[1] = 2U;
		syntax.delta_poc_s0_minus1[2] = 0U;
		syntax.used_by_curr_pic_s0_flag[0] = 1U;
		syntax.used_by_curr_pic_s0_flag[2] = 1U;
		syntax.delta_poc_s1_minus1[0] = 1U;
		syntax.delta_poc_s1_minus1[1] = 3U;
		syntax.used_by_curr_pic_s1_flag[1] = 1U;

		error = read_set(&syntax, 0U, 0U, NULL, 15U, (size_t)-1, &out, &result);
		CHECK(error == 0);
		set_make(&expect, 3U, negative, negative_used, 2U, positive, positive_used);
		CHECK(memcmp(&out, &expect, sizeof(out)) == 0);

		/* The signalled form: counts, minus1 values and flag masks (bit i is entry i). */
		CHECK(result.inter_rps_pred == 0);
		CHECK(result.num_negative_pics == 3U);
		CHECK(result.num_positive_pics == 2U);
		CHECK(result.delta_poc_s0_minus1[0] == 0U);
		CHECK(result.delta_poc_s0_minus1[1] == 2U);
		CHECK(result.delta_poc_s0_minus1[2] == 0U);
		CHECK(result.delta_poc_s0_minus1[3] == 0U);
		CHECK(result.delta_poc_s1_minus1[0] == 1U);
		CHECK(result.delta_poc_s1_minus1[1] == 3U);
		CHECK(result.used_by_curr_pic_s0_flags == 5U);
		CHECK(result.used_by_curr_pic_s1_flags == 2U);
		CHECK(result.delta_idx_minus1 == 0U && result.delta_rps_sign == 0 && result.abs_delta_rps_minus1 == 0U);
		CHECK(result.ref_num_delta_pocs == 0U && result.used_by_curr_pic_flags == 0U && result.use_delta_flags == 0U);

		/* The size in bits: ue(3) 5 + ue(2) 3 + (ue(0) 1 + 1) + (ue(2) 3 + 1) + (1 + 1) + (ue(1) 3 + 1) + (ue(3) 5 + 1) = 26. */
		CHECK(result.bits == 26U);
		CHECK(result.bits == last_bits);

		/* A NULL syntax pointer gives the same set. */
		memset(&out_null, 0xCC, sizeof(out_null));
		vcodec_bitreader_init(&bits, writer.data, last_bytes);
		error = h265sw_rps_read(&bits, 0U, 0U, NULL, 15U, &out_null, NULL);
		CHECK(error == 0);
		CHECK(memcmp(&out_null, &out, sizeof(out)) == 0);
		CHECK(bits.position == last_bits);
	}

	/* Only positive entries; the second delta of 2^15 - 1 + 1 gives 32768 then 65536 (7-70). */
	{
		static const int32_t positive[] = {32768, 65536};
		static const uint8_t positive_used[] = {1, 0};

		memset(&syntax, 0, sizeof(syntax));
		syntax.num_positive_pics = 2U;
		syntax.delta_poc_s1_minus1[0] = 32767U;
		syntax.delta_poc_s1_minus1[1] = 32767U;
		syntax.used_by_curr_pic_s1_flag[0] = 1U;
		error = read_set(&syntax, 0U, 0U, NULL, 15U, (size_t)-1, &out, &with_null);
		CHECK(error == 0);
		set_make(&expect, 0U, NULL, NULL, 2U, positive, positive_used);
		CHECK(memcmp(&out, &expect, sizeof(out)) == 0);
	}

	/* Fifteen negative entries of the largest step reach -15 * 32768 = -491520 (7-69). */
	{
		memset(&syntax, 0, sizeof(syntax));
		syntax.num_negative_pics = 15U;
		memset(syntax.delta_poc_s0_minus1, 0, sizeof(syntax.delta_poc_s0_minus1));
		{
			unsigned entry;

			for (entry = 0U; entry < 15U; entry++) {
				syntax.delta_poc_s0_minus1[entry] = 32767U;
				syntax.used_by_curr_pic_s0_flag[entry] = 1U;
			}
		}
		error = read_set(&syntax, 0U, 0U, NULL, 15U, (size_t)-1, &out, &result);
		CHECK(error == 0);
		CHECK(out.num_negative == 15U);
		CHECK(out.delta_poc[0] == -32768);
		CHECK(out.delta_poc[14] == -491520);
		CHECK(out.delta_poc[15] == 0);
		CHECK(result.used_by_curr_pic_s0_flags == 0x7FFFU);
	}
}

/* Explicit sets: delta_poc_sX_minus1 at the edges of 0..2^15 - 1 and one past. */
static void
check_explicit_deltas(void)
{
	static const uint32_t values[] = {0U, 1U, 32766U, 32767U, 32768U, 32769U, 65535U, 0x7FFFFFFFU};
	struct h265w_st_rps syntax;
	struct h265sw_st_rps out;
	struct h265sw_st_rps_syntax result;
	unsigned index;
	unsigned form;
	int error;
	int expected;

	/* One entry of each list, in the first and the second place. */
	for (index = 0U; index < sizeof(values) / sizeof(values[0]); index++) {
		expected = (values[index] <= 32767U) ? 0 : EINVAL;
		for (form = 0U; form < 4U; form++) {
			memset(&syntax, 0, sizeof(syntax));
			if (form == 0U) {
				syntax.num_negative_pics = 1U;
				syntax.delta_poc_s0_minus1[0] = values[index];
			} else if (form == 1U) {
				syntax.num_positive_pics = 1U;
				syntax.delta_poc_s1_minus1[0] = values[index];
			} else if (form == 2U) {
				syntax.num_negative_pics = 2U;
				syntax.delta_poc_s0_minus1[1] = values[index];
			} else {
				syntax.num_positive_pics = 2U;
				syntax.delta_poc_s1_minus1[1] = values[index];
			}
			error = read_set(&syntax, 0U, 0U, NULL, 15U, (size_t)-1, &out, &result);
			CHECK(error == expected);
		}
	}
}

/*
 * Predicted sets: six or more worked examples.  Each gives the source set,
 * deltaRps, the flags and the set that 7-61 and 7-62 build.
 */
static void
check_predicted_hand(void)
{
	/* The source A: DeltaPocS0 = -1, -3; DeltaPocS1 = 2, 5 (NumDeltaPocs 4, flag index j: 0 is -1, 1 is -3, 2 is 2, 3 is 5, 4 is deltaRps itself). */
	static const int32_t a_negative[] = {-1, -3};
	static const uint8_t a_used[] = {1, 1};
	static const int32_t a_positive[] = {2, 5};
	struct h265w_st_rps syntax;
	struct h265sw_st_rps source;
	struct h265sw_st_rps out;
	struct h265sw_st_rps expect;
	struct h265sw_st_rps_syntax result;
	int error;

	set_make(&source, 2U, a_negative, a_used, 2U, a_positive, a_used);

	/*
	 * Example 1: deltaRps -1, every entry used, so every use_delta is 1.
	 * 7-61: S1 entries from the last: 5 - 1 = 4, 2 - 1 = 1 are positive, not taken;
	 * deltaRps -1 is negative: taken (flag j = 4); then -1 - 1 = -2, -3 - 1 = -4.
	 * DeltaPocS0 = -1, -2, -4.  7-62: -2, -4 are negative, not taken; deltaRps is not
	 * positive; then 2 - 1 = 1, 5 - 1 = 4.  DeltaPocS1 = 1, 4.
	 */
	{
		static const int32_t want_negative[] = {-1, -2, -4};
		static const uint8_t want_negative_used[] = {1, 1, 1};
		static const int32_t want_positive[] = {1, 4};
		static const uint8_t want_positive_used[] = {1, 1};

		predicted_syntax(&syntax, 0U, 1, 0U, 4U, 0x1FU, 0U);
		error = read_set(&syntax, 1U, 4U, &source, 15U, (size_t)-1, &out, &result);
		CHECK(error == 0);
		set_make(&expect, 3U, want_negative, want_negative_used, 2U, want_positive, want_positive_used);
		CHECK(memcmp(&out, &expect, sizeof(out)) == 0);

		/* The count is exactly five: that fits max_pictures 5 and not 4 (the S1 list is then one too long). */
		error = read_set(&syntax, 1U, 4U, &source, 5U, (size_t)-1, &out, &result);
		CHECK(error == 0);
		error = read_set(&syntax, 1U, 4U, &source, 4U, (size_t)-1, &out, &result);
		CHECK(error == EINVAL);

		/* S0 alone is three: max_pictures 2 fails in the S0 list. */
		error = read_set(&syntax, 1U, 4U, &source, 2U, (size_t)-1, &out, &result);
		CHECK(error == EINVAL);
	}

	/*
	 * Example 2: deltaRps +3.  Flags: j = 1 (-3) is dropped (used 0, use_delta 0);
	 * j = 3 (5) is kept but unused (used 0, use_delta 1); the others used.
	 * Entries: -1 + 3 = 2 (used), -3 dropped, 2 + 3 = 5 (used), 5 + 3 = 8 (not used),
	 * deltaRps 3 (flag j = 4, used).  No negative entry.
	 * DeltaPocS1 = 2, 3, 5, 8 with used 1, 1, 1, 0.
	 */
	{
		static const int32_t want_positive[] = {2, 3, 5, 8};
		static const uint8_t want_positive_used[] = {1, 1, 1, 0};

		predicted_syntax(&syntax, 0U, 0, 2U, 4U, 0x15U, 0x08U);
		error = read_set(&syntax, 1U, 4U, &source, 15U, (size_t)-1, &out, &result);
		CHECK(error == 0);
		set_make(&expect, 0U, NULL, NULL, 4U, want_positive, want_positive_used);
		CHECK(memcmp(&out, &expect, sizeof(out)) == 0);
		CHECK(result.used_by_curr_pic_flags == 0x15U);
		CHECK(result.use_delta_flags == (0x15U | 0x08U));
	}

	/*
	 * Example 3: an empty source (NumDeltaPocs 0, one flag pair).  deltaRps -4
	 * with the entry used gives S0 = -4; with the pair 0, 0 the set is empty;
	 * deltaRps +4 used gives S1 = 4.
	 */
	{
		static const int32_t one_negative[] = {-4};
		static const int32_t one_positive[] = {4};
		static const uint8_t one_used[] = {1};
		struct h265sw_st_rps empty;

		memset(&empty, 0, sizeof(empty));
		predicted_syntax(&syntax, 0U, 1, 3U, 0U, 0x1U, 0U);
		error = read_set(&syntax, 1U, 2U, &empty, 15U, (size_t)-1, &out, &result);
		CHECK(error == 0);
		set_make(&expect, 1U, one_negative, one_used, 0U, NULL, NULL);
		CHECK(memcmp(&out, &expect, sizeof(out)) == 0);

		predicted_syntax(&syntax, 0U, 1, 3U, 0U, 0x0U, 0x0U);
		error = read_set(&syntax, 1U, 2U, &empty, 15U, (size_t)-1, &out, &result);
		CHECK(error == 0);
		CHECK(out.num_negative == 0U && out.num_positive == 0U);

		predicted_syntax(&syntax, 0U, 0, 3U, 0U, 0x1U, 0U);
		error = read_set(&syntax, 1U, 2U, &empty, 15U, (size_t)-1, &out, &result);
		CHECK(error == 0);
		set_make(&expect, 0U, NULL, NULL, 1U, one_positive, one_used);
		CHECK(memcmp(&out, &expect, sizeof(out)) == 0);

		/* With max_pictures 0 the one new entry is refused before it is stored. */
		predicted_syntax(&syntax, 0U, 0, 3U, 0U, 0x1U, 0U);
		error = read_set(&syntax, 1U, 2U, &empty, 0U, (size_t)-1, &out, &result);
		CHECK(error == EINVAL);
	}

	/*
	 * Example 4: a negative-only source -2, -5 (flag j 0, 1; deltaRps is j 2),
	 * deltaRps +2, all used.  -2 + 2 = 0 is dropped (neither list), -5 + 2 = -3
	 * is negative; deltaRps 2 is positive.  S0 = -3, S1 = 2.
	 */
	{
		static const int32_t source_negative[] = {-2, -5};
		static const uint8_t source_used[] = {1, 1};
		static const int32_t want_negative[] = {-3};
		static const int32_t want_positive[] = {2};
		static const uint8_t want_used[] = {1};
		struct h265sw_st_rps negative_only;

		set_make(&negative_only, 2U, source_negative, source_used, 0U, NULL, NULL);
		predicted_syntax(&syntax, 0U, 0, 1U, 2U, 0x7U, 0U);
		error = read_set(&syntax, 1U, 2U, &negative_only, 15U, (size_t)-1, &out, &result);
		CHECK(error == 0);
		set_make(&expect, 1U, want_negative, want_used, 1U, want_positive, want_used);
		CHECK(memcmp(&out, &expect, sizeof(out)) == 0);
	}

	/*
	 * Example 5: a positive-only source 1, 4 (flag j 0, 1; deltaRps is j 2),
	 * deltaRps -1 (the entry 1 - 1 = 0 is dropped).  Flags: j 0 used, j 1
	 * not used but kept, j 2 used.  4 - 1 = 3 is positive (not used);
	 * deltaRps -1 is negative (used).  S0 = -1 (1), S1 = 3 (0).
	 */
	{
		static const int32_t source_positive[] = {1, 4};
		static const uint8_t source_used[] = {1, 1};
		static const int32_t want_negative[] = {-1};
		static const uint8_t want_negative_used[] = {1};
		static const int32_t want_positive[] = {3};
		static const uint8_t want_positive_used[] = {0};
		struct h265sw_st_rps positive_only;

		set_make(&positive_only, 0U, NULL, NULL, 2U, source_positive, source_used);
		predicted_syntax(&syntax, 0U, 1, 0U, 2U, 0x5U, 0x2U);
		error = read_set(&syntax, 1U, 2U, &positive_only, 15U, (size_t)-1, &out, &result);
		CHECK(error == 0);
		set_make(&expect, 1U, want_negative, want_negative_used, 1U, want_positive, want_positive_used);
		CHECK(memcmp(&out, &expect, sizeof(out)) == 0);
	}

	/*
	 * Example 6: the source of 15 entries (S0 = -1..-8, S1 = 1..7), deltaRps -1,
	 * all used.  S1 entries 7..1 give 6..0: 6..1 are positive (not taken), 0 is
	 * dropped; deltaRps -1 is taken; S0 entries give -2..-9.  S0 = -1..-9 would be
	 * nine entries, S1 six: 15 in all, which fits max_pictures 15.
	 */
	{
		static const int32_t full_negative[] = {-1, -2, -3, -4, -5, -6, -7, -8};
		static const int32_t full_positive[] = {1, 2, 3, 4, 5, 6, 7};
		static const uint8_t full_used[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
		static const int32_t want_negative[] = {-1, -2, -3, -4, -5, -6, -7, -8, -9};
		static const int32_t want_positive[] = {1, 2, 3, 4, 5, 6};
		struct h265sw_st_rps full;

		set_make(&full, 8U, full_negative, full_used, 7U, full_positive, full_used);
		predicted_syntax(&syntax, 0U, 1, 0U, 15U, 0xFFFFU, 0U);
		error = read_set(&syntax, 1U, 2U, &full, 15U, (size_t)-1, &out, &result);
		CHECK(error == 0);
		set_make(&expect, 9U, want_negative, full_used, 6U, want_positive, full_used);
		CHECK(memcmp(&out, &expect, sizeof(out)) == 0);

		/* One more entry than max_pictures allows is refused. */
		error = read_set(&syntax, 1U, 2U, &full, 14U, (size_t)-1, &out, &result);
		CHECK(error == EINVAL);

		/* deltaRps -100 turns the seven positive entries negative: 7 + 1 + 8 = 16 entries, over any bound. */
		predicted_syntax(&syntax, 0U, 1, 99U, 15U, 0xFFFFU, 0U);
		error = read_set(&syntax, 1U, 2U, &full, 15U, (size_t)-1, &out, &result);
		CHECK(error == EINVAL);

		/* Dropping the deltaRps entry (j = 15: used 0, use_delta 0) leaves 15, which fits. */
		predicted_syntax(&syntax, 0U, 1, 99U, 15U, 0x7FFFU, 0U);
		error = read_set(&syntax, 1U, 2U, &full, 15U, (size_t)-1, &out, &result);
		CHECK(error == 0);
		CHECK(out.num_negative == 15U && out.num_positive == 0U);
		CHECK(out.delta_poc[0] == -93 && out.delta_poc[6] == -99 && out.delta_poc[7] == -101 && out.delta_poc[14] == -108);
	}

	/* deltaRps at its extremes: abs_delta_rps_minus1 2^15 - 1 is taken, 2^15 is not. */
	{
		static const int32_t one_negative[] = {-32768};
		static const uint8_t one_used[] = {1};
		struct h265sw_st_rps empty;

		memset(&empty, 0, sizeof(empty));
		predicted_syntax(&syntax, 0U, 1, 32767U, 0U, 0x1U, 0U);
		error = read_set(&syntax, 1U, 2U, &empty, 15U, (size_t)-1, &out, &result);
		CHECK(error == 0);
		set_make(&expect, 1U, one_negative, one_used, 0U, NULL, NULL);
		CHECK(memcmp(&out, &expect, sizeof(out)) == 0);
		CHECK(result.abs_delta_rps_minus1 == 32767U && result.delta_rps_sign == 1);

		predicted_syntax(&syntax, 0U, 1, 32768U, 0U, 0x1U, 0U);
		error = read_set(&syntax, 1U, 2U, &empty, 15U, (size_t)-1, &out, &result);
		CHECK(error == EINVAL);
		predicted_syntax(&syntax, 0U, 0, 32768U, 0U, 0x1U, 0U);
		error = read_set(&syntax, 1U, 2U, &empty, 15U, (size_t)-1, &out, &result);
		CHECK(error == EINVAL);
	}
}

/* One entry of the model of a predicted set. */
struct model_entry {
	int32_t delta;
	uint8_t used;
};

/* Sorts entries by delta, increasing or decreasing (a small insertion sort). */
static void model_sort(struct model_entry *entries, unsigned count, int decreasing);

/* Sorts entries by delta, increasing or decreasing. */
static void
model_sort(
	struct model_entry *entries,
	unsigned count,
	int decreasing)
{
	struct model_entry held;
	unsigned outer;
	unsigned inner;
	int before;

	/* Moves each entry back past the ones that belong after it. */
	for (outer = 1U; outer < count; outer++) {
		held = entries[outer];
		inner = outer;
		while (inner > 0U) {
			before = (entries[inner - 1U].delta > held.delta);
			if (decreasing != 0)
				before = (entries[inner - 1U].delta < held.delta);
			if (before == 0)
				break;
			entries[inner] = entries[inner - 1U];
			inner--;
		}
		entries[inner] = held;
	}
}

/* Predicted sets from random sources, checked against the collect-and-sort model, in SPS and slice header form. */
static void
check_predicted_random(void)
{
	struct h265w_st_rps syntax;
	struct h265sw_st_rps out;
	struct h265sw_st_rps_syntax result;
	struct model_entry negatives[H265SW_MAX_RPS * 2U];
	struct model_entry positives[H265SW_MAX_RPS * 2U];
	struct h265sw_st_rps *source;
	unsigned round;
	unsigned num_sets;
	unsigned index;
	unsigned delta_idx_minus1;
	unsigned max_pictures;
	unsigned reference_count;
	unsigned entry;
	unsigned negative_count;
	unsigned positive_count;
	uint32_t used_mask;
	uint32_t use_delta_mask;
	uint32_t abs_minus1;
	int negative_delta;
	int32_t delta;
	int32_t last;
	int slice_form;
	int error;
	int expected_error;
	int included;
	int used;

	for (round = 0U; round < 20000U; round++) {
		/* Builds one to eight source sets, each with random distinct deltas. */
		num_sets = 1U + random_next() % 8U;
		for (index = 0U; index < num_sets; index++) {
			memset(&sets[index], 0, sizeof(sets[index]));
			sets[index].num_negative = random_next() % 8U;
			sets[index].num_positive = random_next() % (8U - sets[index].num_negative + 1U);
			last = 0;
			for (entry = 0U; entry < sets[index].num_negative; entry++) {
				last -= (int32_t)(1U + random_next() % 6U);
				sets[index].delta_poc[entry] = last;
				sets[index].used[entry] = (uint8_t)(random_next() & 1U);
			}
			last = 0;
			for (entry = 0U; entry < sets[index].num_positive; entry++) {
				last += (int32_t)(1U + random_next() % 6U);
				sets[index].delta_poc[sets[index].num_negative + entry] = last;
				sets[index].used[sets[index].num_negative + entry] = (uint8_t)(random_next() & 1U);
			}
		}

		/* The set being read: a slice header's (index == num_sets, any source) or an SPS's (the one before it). */
		slice_form = (int)(random_next() & 1U);
		if (slice_form != 0) {
			index = num_sets;
			delta_idx_minus1 = random_next() % num_sets;
			source = &sets[index - (delta_idx_minus1 + 1U)];
		} else {
			index = 1U + random_next() % num_sets;
			num_sets = index + random_next() % 3U;
			if (index >= num_sets) {
				num_sets = index + 1U;
			}
			if (num_sets > 8U) {
				/* Keeps the set array filled: sets past 8 are not built, so use the form that stays inside. */
				num_sets = 8U;
				index = 1U + random_next() % (num_sets - 1U);
			}
			delta_idx_minus1 = 0U;
			source = &sets[index - 1U];
		}
		reference_count = source->num_negative + source->num_positive;

		/* Random delta and flags (bit j of the masks). */
		abs_minus1 = random_next() % 12U;
		negative_delta = (int)(random_next() & 1U);
		used_mask = random_next() & ((1U << (reference_count + 1U)) - 1U);
		use_delta_mask = random_next() & ((1U << (reference_count + 1U)) - 1U);
		max_pictures = 4U + random_next() % 12U;
		predicted_syntax(&syntax, delta_idx_minus1, negative_delta, abs_minus1, reference_count, used_mask, use_delta_mask);

		/* The model: collect the entries the flags keep, with the deltaRps entry last, then sort. */
		delta = (int32_t)abs_minus1 + 1;
		if (negative_delta != 0)
			delta = -delta;
		negative_count = 0U;
		positive_count = 0U;
		for (entry = 0U; entry <= reference_count; entry++) {
			used = (int)((used_mask >> entry) & 1U);
			included = (used != 0) || (((use_delta_mask >> entry) & 1U) != 0U);
			if (included == 0)
				continue;
			last = delta;
			if (entry < reference_count)
				last = source->delta_poc[entry] + delta;
			if (last < 0) {
				negatives[negative_count].delta = last;
				negatives[negative_count].used = (uint8_t)used;
				negative_count++;
			} else if (last > 0) {
				positives[positive_count].delta = last;
				positives[positive_count].used = (uint8_t)used;
				positive_count++;
			}
		}
		model_sort(negatives, negative_count, 1);
		model_sort(positives, positive_count, 0);

		/* The bounds of 7.4.8 decide whether the set is accepted. */
		expected_error = 0;
		if (negative_count > max_pictures || negative_count + positive_count > max_pictures)
			expected_error = EINVAL;

		error = read_set(&syntax, index, num_sets, sets, max_pictures, (size_t)-1, &out, &result);
		CHECK(error == expected_error);
		if (error == 0 && expected_error == 0) {
			CHECK(out.num_negative == negative_count);
			CHECK(out.num_positive == positive_count);
			for (entry = 0U; entry < negative_count; entry++) {
				CHECK(out.delta_poc[entry] == negatives[entry].delta);
				CHECK(out.used[entry] == negatives[entry].used);
			}
			for (entry = 0U; entry < positive_count; entry++) {
				CHECK(out.delta_poc[negative_count + entry] == positives[entry].delta);
				CHECK(out.used[negative_count + entry] == positives[entry].used);
			}
			for (entry = negative_count + positive_count; entry < H265SW_MAX_RPS; entry++) {
				CHECK(out.delta_poc[entry] == 0);
				CHECK(out.used[entry] == 0U);
			}
			CHECK(result.inter_rps_pred == 1);
			CHECK(result.delta_idx_minus1 == delta_idx_minus1);
			CHECK(result.ref_num_delta_pocs == reference_count);
			CHECK(result.used_by_curr_pic_flags == used_mask);
			CHECK(result.bits == last_bits);
		}
	}
}

/* The index rules: delta_idx_minus1 only for the slice header's set, the prediction flag not for index 0. */
static void
check_indexes(void)
{
	static const int32_t source_negative[] = {-1};
	static const uint8_t source_used[] = {1};
	struct h265w_st_rps syntax;
	struct h265sw_st_rps out;
	struct h265sw_st_rps expect;
	struct h265sw_st_rps_syntax result;
	unsigned delta_idx_minus1;
	unsigned entry;
	int error;
	int expected;

	/* Three sets: set 0 is -1, set 1 is -1, -2 (not used), set 2 is empty. */
	memset(sets, 0, sizeof(sets));
	set_make(&sets[0], 1U, source_negative, source_used, 0U, NULL, NULL);
	sets[1].num_negative = 2U;
	sets[1].delta_poc[0] = -1;
	sets[1].delta_poc[1] = -2;
	sets[1].used[0] = 1U;

	/* The slice header's set (index 3 of 3): delta_idx_minus1 0..2 name sets 2..0, 3 is out of range. */
	for (delta_idx_minus1 = 0U; delta_idx_minus1 <= 4U; delta_idx_minus1++) {
		predicted_syntax(&syntax, delta_idx_minus1, 0, 0U, 0U, 0x1U, 0U);
		syntax.ref_num_delta_pocs = 0U;
		if (delta_idx_minus1 == 0U) {
			/* Set 2 is empty: one flag pair. */
		} else if (delta_idx_minus1 == 1U) {
			syntax.ref_num_delta_pocs = 2U;
		} else if (delta_idx_minus1 == 2U) {
			syntax.ref_num_delta_pocs = 1U;
		}
		for (entry = 0U; entry < 3U; entry++) {
			syntax.used_by_curr_pic_flag[entry] = 1U;
			syntax.use_delta_flag[entry] = 1U;
		}
		error = read_set(&syntax, 3U, 3U, sets, 15U, (size_t)-1, &out, &result);
		expected = (delta_idx_minus1 <= 2U) ? 0 : EINVAL;
		CHECK(error == expected);
		if (expected == 0) {
			CHECK(result.delta_idx_minus1 == delta_idx_minus1);
			CHECK(result.bits == last_bits);
		}
	}

	/* deltaRps +1 on the source of set 0 (-1): -1 + 1 = 0 is dropped, deltaRps 1 is positive; checks the source is the one named. */
	{
		static const int32_t want_positive[] = {1};
		static const uint8_t want_used[] = {1};

		predicted_syntax(&syntax, 2U, 0, 0U, 1U, 0x3U, 0U);
		error = read_set(&syntax, 3U, 3U, sets, 15U, (size_t)-1, &out, &result);
		CHECK(error == 0);
		set_make(&expect, 0U, NULL, NULL, 1U, want_positive, want_used);
		CHECK(memcmp(&out, &expect, sizeof(out)) == 0);
	}

	/* An SPS set (index 1 of 3) reads no delta_idx_minus1 and predicts from set 0: the same flag pairs give the same set and the size is one ue shorter. */
	{
		static const int32_t want_positive[] = {1};
		static const uint8_t want_used[] = {1};

		predicted_syntax(&syntax, 5U, 0, 0U, 1U, 0x3U, 0U);
		error = read_set(&syntax, 1U, 3U, sets, 15U, (size_t)-1, &out, &result);
		CHECK(error == 0);
		set_make(&expect, 0U, NULL, NULL, 1U, want_positive, want_used);
		CHECK(memcmp(&out, &expect, sizeof(out)) == 0);
		CHECK(result.delta_idx_minus1 == 0U);
		CHECK(result.bits == last_bits);
		CHECK(result.bits == 1U + 1U + 1U + 2U);
	}

	/* Index 0 reads no prediction flag: the explicit form starts at once, and a prediction flag written there would be a count. */
	{
		memset(&syntax, 0, sizeof(syntax));
		syntax.inter_rps_pred = 1U;
		syntax.num_negative_pics = 1U;
		syntax.delta_poc_s0_minus1[0] = 4U;
		error = read_set(&syntax, 0U, 5U, NULL, 15U, (size_t)-1, &out, &result);
		CHECK(error == 0);
		CHECK(result.inter_rps_pred == 0);
		CHECK(out.num_negative == 1U && out.delta_poc[0] == -5);
		CHECK(result.bits == 3U + 1U + 5U + 1U);
		CHECK(result.bits == last_bits);

		/* The slice header's set with no SPS sets is index 0 too. */
		error = read_set(&syntax, 0U, 0U, NULL, 15U, (size_t)-1, &out, &result);
		CHECK(error == 0);
		CHECK(result.bits == last_bits);
	}
}

/* The signalled fields of a predicted set, and of an explicit set after index 0 (the flag is read and is 0). */
static void
check_syntax_fields(void)
{
	static const int32_t source_negative[] = {-2};
	static const uint8_t source_used[] = {1};
	struct h265w_st_rps syntax;
	struct h265sw_st_rps out;
	struct h265sw_st_rps_syntax result;
	int error;

	memset(sets, 0, sizeof(sets));
	set_make(&sets[0], 1U, source_negative, source_used, 0U, NULL, NULL);

	/* An explicit set at index 1: the prediction flag is read (0), the rest as at index 0. */
	memset(&syntax, 0, sizeof(syntax));
	syntax.num_negative_pics = 1U;
	syntax.num_positive_pics = 1U;
	syntax.delta_poc_s0_minus1[0] = 1U;
	syntax.used_by_curr_pic_s0_flag[0] = 1U;
	syntax.delta_poc_s1_minus1[0] = 0U;
	error = read_set(&syntax, 1U, 2U, sets, 15U, (size_t)-1, &out, &result);
	CHECK(error == 0);
	CHECK(result.inter_rps_pred == 0);
	CHECK(result.bits == last_bits);
	CHECK(result.used_by_curr_pic_s0_flags == 1U && result.used_by_curr_pic_s1_flags == 0U);
	CHECK(out.delta_poc[0] == -2 && out.delta_poc[1] == 1);

	/* A predicted set: every field of the prediction is kept and the explicit fields are 0. */
	predicted_syntax(&syntax, 0U, 1, 6U, 1U, 0x2U, 0x1U);
	error = read_set(&syntax, 1U, 2U, sets, 15U, (size_t)-1, &out, &result);
	CHECK(error == 0);
	CHECK(result.inter_rps_pred == 1);
	CHECK(result.delta_idx_minus1 == 0U);
	CHECK(result.delta_rps_sign == 1);
	CHECK(result.abs_delta_rps_minus1 == 6U);
	CHECK(result.ref_num_delta_pocs == 1U);
	CHECK(result.used_by_curr_pic_flags == 0x2U);
	CHECK(result.use_delta_flags == 0x3U);
	CHECK(result.num_negative_pics == 0U && result.num_positive_pics == 0U);
	CHECK(result.used_by_curr_pic_s0_flags == 0U && result.used_by_curr_pic_s1_flags == 0U);
	CHECK(result.delta_poc_s0_minus1[0] == 0U && result.delta_poc_s1_minus1[0] == 0U);
	CHECK(result.bits == last_bits);

	/* The same prediction with a NULL syntax pointer gives the same set. */
	{
		struct h265sw_st_rps out_null;
		struct vcodec_bitreader bits;

		memset(&out_null, 0xCC, sizeof(out_null));
		vcodec_bitreader_init(&bits, writer.data, last_bytes);
		error = h265sw_rps_read(&bits, 1U, 2U, sets, 15U, &out_null, NULL);
		CHECK(error == 0);
		CHECK(memcmp(&out_null, &out, sizeof(out)) == 0);
	}
}

/* Arguments no valid caller passes. */
static void
check_arguments(void)
{
	struct h265w_st_rps syntax;
	struct h265sw_st_rps out;
	struct h265sw_st_rps_syntax result;
	struct vcodec_bitreader bits;
	int error;

	/* A valid explicit set in a writer to read from. */
	memset(&syntax, 0, sizeof(syntax));
	bitwriter_init(&writer);
	h265w_st_rps(&writer, &syntax, 0U, 0U);
	bitwriter_trailing(&writer);

	/* Missing reader or output. */
	vcodec_bitreader_init(&bits, writer.data, 4U);
	CHECK(h265sw_rps_read(NULL, 0U, 0U, NULL, 15U, &out, &result) == EINVAL);
	CHECK(h265sw_rps_read(&bits, 0U, 0U, NULL, 15U, NULL, &result) == EINVAL);

	/* An index above num_sets, num_sets above 64, max_pictures above 15, no sets for index above 0. */
	vcodec_bitreader_init(&bits, writer.data, 4U);
	error = h265sw_rps_read(&bits, 2U, 1U, sets, 15U, &out, &result);
	CHECK(error == EINVAL);
	vcodec_bitreader_init(&bits, writer.data, 4U);
	error = h265sw_rps_read(&bits, 0U, 65U, sets, 15U, &out, &result);
	CHECK(error == EINVAL);
	vcodec_bitreader_init(&bits, writer.data, 4U);
	error = h265sw_rps_read(&bits, 0U, 0U, NULL, 16U, &out, &result);
	CHECK(error == EINVAL);
	vcodec_bitreader_init(&bits, writer.data, 4U);
	error = h265sw_rps_read(&bits, 1U, 1U, NULL, 15U, &out, &result);
	CHECK(error == EINVAL);

	/* The valid call succeeds, and index 64 of 64 is allowed. */
	vcodec_bitreader_init(&bits, writer.data, 4U);
	error = h265sw_rps_read(&bits, 0U, 0U, NULL, 15U, &out, &result);
	CHECK(error == 0);
	CHECK(out.num_negative == 0U && out.num_positive == 0U && result.bits == 2U);

	/* A source set the caller built out of bounds (more entries than a set can hold) is refused, not read past. */
	{
		struct h265sw_st_rps bad;
		unsigned form;

		for (form = 0U; form < 5U; form++) {
			memset(&bad, 0, sizeof(bad));
			if (form == 0U) {
				bad.num_negative = 10U;
				bad.num_positive = 10U;
			} else if (form == 1U) {
				bad.num_negative = 16U;
			} else if (form == 2U) {
				bad.num_positive = 16U;
			} else if (form == 3U) {
				bad.num_negative = 0xFFFFFFFFU;
				bad.num_positive = 2U;
			} else {
				bad.num_negative = 8U;
				bad.num_positive = 8U;
			}
			predicted_syntax(&syntax, 0U, 0, 0U, 0U, 0x1U, 0U);
			error = read_set(&syntax, 1U, 2U, &bad, 15U, (size_t)-1, &out, &result);
			CHECK(error == EINVAL);
		}

		/* A source of 16 entries whose every entry is dropped would derive an empty set; it is refused for its size all the same. */
		memset(&bad, 0, sizeof(bad));
		bad.num_negative = 8U;
		bad.num_positive = 8U;
		predicted_syntax(&syntax, 0U, 0, 0U, 16U, 0U, 0U);
		error = read_set(&syntax, 1U, 2U, &bad, 15U, (size_t)-1, &out, &result);
		CHECK(error == EINVAL);

		/* The largest source that fits (15 entries) is read. */
		memset(&bad, 0, sizeof(bad));
		bad.num_negative = 8U;
		bad.num_positive = 7U;
		predicted_syntax(&syntax, 0U, 0, 0U, 15U, 0xFFFFU, 0U);
		error = read_set(&syntax, 1U, 2U, &bad, 15U, (size_t)-1, &out, &result);
		CHECK(error != EINVAL || out.num_negative + out.num_positive <= 15U);
	}
}

/* Every cut of every form is refused unless it still holds all the syntax bits. */
static void
check_truncated(void)
{
	static const int32_t source_negative[] = {-1, -2};
	static const uint8_t source_used[] = {1, 1};
	struct h265w_st_rps syntax;
	struct h265sw_st_rps out;
	struct h265sw_st_rps_syntax result;
	size_t cut;
	size_t bytes;
	size_t bits;
	unsigned form;
	int error;

	memset(sets, 0, sizeof(sets));
	set_make(&sets[0], 2U, source_negative, source_used, 0U, NULL, NULL);

	/* An explicit set, an SPS predicted set and a slice header predicted set, with large values so the codes are long. */
	for (form = 0U; form < 3U; form++) {
		if (form == 0U) {
			memset(&syntax, 0, sizeof(syntax));
			syntax.num_negative_pics = 3U;
			syntax.num_positive_pics = 2U;
			syntax.delta_poc_s0_minus1[0] = 32767U;
			syntax.delta_poc_s0_minus1[1] = 1000U;
			syntax.delta_poc_s0_minus1[2] = 5U;
			syntax.delta_poc_s1_minus1[0] = 20000U;
			syntax.delta_poc_s1_minus1[1] = 7U;
			error = read_set(&syntax, 0U, 0U, NULL, 15U, (size_t)-1, &out, &result);
		} else if (form == 1U) {
			predicted_syntax(&syntax, 0U, 1, 30000U, 2U, 0x5U, 0x2U);
			error = read_set(&syntax, 1U, 3U, sets, 15U, (size_t)-1, &out, &result);
		} else {
			predicted_syntax(&syntax, 0U, 0, 2000U, 2U, 0x7U, 0U);
			error = read_set(&syntax, 1U, 1U, sets, 15U, (size_t)-1, &out, &result);
		}
		CHECK(error == 0);
		bytes = last_bytes;
		bits = last_bits;

		for (cut = 0U; cut < bytes; cut++) {
			if (form == 0U)
				error = read_set(&syntax, 0U, 0U, NULL, 15U, cut, &out, &result);
			else if (form == 1U)
				error = read_set(&syntax, 1U, 3U, sets, 15U, cut, &out, &result);
			else
				error = read_set(&syntax, 1U, 1U, sets, 15U, cut, &out, &result);
			if (cut * 8U < bits) {
				CHECK(error == EINVAL);
			} else {
				CHECK(error == 0);
			}
		}
	}
}

/* Random bytes with random arguments: no crash, and a success keeps every bound. */
static void
check_garbage(void)
{
	uint8_t *data;
	struct h265sw_st_rps out;
	struct h265sw_st_rps_syntax result;
	struct vcodec_bitreader bits;
	size_t size;
	size_t index;
	unsigned round;
	unsigned num_sets;
	unsigned set_index;
	unsigned max_pictures;
	unsigned entry;
	unsigned count;
	int error;

	/* A set array of valid derived sets for the predicted forms to read. */
	memset(sets, 0, sizeof(sets));
	for (entry = 0U; entry < TEST_SETS; entry++) {
		sets[entry].num_negative = entry % 4U;
		sets[entry].num_positive = (entry / 4U) % 4U;
		for (index = 0U; index < sets[entry].num_negative; index++)
			sets[entry].delta_poc[index] = -(int32_t)(index + 1U);
		for (index = 0U; index < sets[entry].num_positive; index++)
			sets[entry].delta_poc[sets[entry].num_negative + index] = (int32_t)(index + 1U);
	}

	for (round = 0U; round < 100000U; round++) {
		size = random_next() % 24U;
		data = malloc(size + 1U);
		CHECK(data != NULL);
		if (data == NULL)
			return;
		for (index = 0U; index < size; index++) {
			data[index] = (uint8_t)random_next();
			if ((random_next() & 3U) == 0U)
				data[index] = 0U;
		}

		num_sets = random_next() % 65U;
		set_index = random_next() % (num_sets + 1U);
		max_pictures = random_next() % 16U;
		vcodec_bitreader_init(&bits, data, size);
		error = h265sw_rps_read(&bits, set_index, num_sets, sets, max_pictures, &out, &result);
		CHECK(error == 0 || error == EINVAL);
		if (error == 0) {
			CHECK(out.num_negative <= max_pictures);
			CHECK(out.num_negative + out.num_positive <= max_pictures);
			count = out.num_negative + out.num_positive;
			for (entry = count; entry < H265SW_MAX_RPS; entry++)
				CHECK(out.delta_poc[entry] == 0 && out.used[entry] == 0U);
			for (entry = 1U; entry < out.num_negative; entry++)
				CHECK(out.delta_poc[entry] < out.delta_poc[entry - 1U]);
			for (entry = 1U; entry < out.num_positive; entry++)
				CHECK(out.delta_poc[out.num_negative + entry] > out.delta_poc[out.num_negative + entry - 1U]);
			CHECK(result.bits <= size * 8U);
			CHECK(bits.position - 0U == result.bits);
		}
		free(data);
	}
}

int
main(void)
{
	/* Runs every group of checks. */
	check_explicit_counts();
	check_explicit_values();
	check_explicit_deltas();
	check_predicted_hand();
	check_predicted_random();
	check_indexes();
	check_syntax_fields();
	check_arguments();
	check_truncated();
	check_garbage();

	/* Reports the summary line. */
	return check_finish("rps");
}
