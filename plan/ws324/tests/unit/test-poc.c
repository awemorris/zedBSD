/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tests of h265sw-poc: the picture order count (8.3.1, 8-1 and 8-2),
 * prevTid0Pic, and the order counts of the reference picture set (8.3.2,
 * 8-5).
 *
 * The expected values of the hand examples are worked by hand (comments
 * give the working).  The exhaustive and random comparisons use a
 * different formulation of the same rules: the order count nearest the
 * previous one among the three that share the lsb, ties going up, for
 * 8-1; the floor of a quotient by arithmetic on a remainder for the
 * bitwise AND of 8-5.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "userland/desktop/libmedia/h265sw-poc.h"

/* The state of the pseudo-random generator, fixed so a failure repeats. */
static uint32_t random_state = 0x6C078965U;

/* The range of int32_t as int64_t bounds. */
#define INT32_TOP ((int64_t)2147483647)
#define INT32_BOTTOM (-(int64_t)2147483647 - 1)

/* Static function declarations. */
static uint32_t random_next(void);
static uint32_t rnd(uint32_t count);
static void make_header(struct h265sw_slice_header *header, unsigned nal_type, unsigned temporal_id, uint32_t poc_lsb);
static int compute(int32_t prev_msb, uint32_t prev_lsb, int irap, unsigned nal_type, uint32_t lsb, unsigned log2, int32_t *poc);
static int64_t nearest_poc(int64_t prev_msb, int64_t prev_lsb, int64_t lsb, int64_t max_lsb);
static void check_init(void);
static void check_compute_hand(void);
static void check_compute_exhaustive(void);
static void check_compute_limits(void);
static void check_poc_finish(void);
static void check_sequence(void);
static void check_rps_hand(void);
static void check_rps_negative(void);
static void check_rps_limits(void);
static void check_rps_random(void);

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

/* Returns a pseudo-random number below count (count above 0). */
static uint32_t
rnd(
	uint32_t count)
{
	/* Succeeded: the number. */
	return random_next() % count;
}

/* Makes a header with a NAL type, a temporal id and an order count lsb, and nothing else. */
static void
make_header(
	struct h265sw_slice_header *header,
	unsigned nal_type,
	unsigned temporal_id,
	uint32_t poc_lsb)
{
	/* Every other field is 0. */
	memset(header, 0, sizeof(*header));
	header->nal_type = nal_type;
	header->temporal_id = temporal_id;
	header->poc_lsb = poc_lsb;
}

/* Derives an order count from a hand-made state. */
static int
compute(
	int32_t prev_msb,
	uint32_t prev_lsb,
	int irap,
	unsigned nal_type,
	uint32_t lsb,
	unsigned log2,
	int32_t *poc)
{
	static struct h265sw_slice_header header;
	struct h265sw_poc_state state;

	/* The state and the header. */
	state.prev_lsb = prev_lsb;
	state.prev_msb = prev_msb;
	make_header(&header, nal_type, 0U, lsb);

	/* Succeeded: the answer of the derivation. */
	return h265sw_poc_compute(&state, &header, irap, log2, poc);
}

/*
 * The order count 8-1 and 8-2 give, by another rule: among the three
 * order counts prev_msb + k * MaxPicOrderCntLsb + lsb (k = -1, 0, 1) the
 * one nearest the previous picture's, and the higher one when two are
 * equally near.
 */
static int64_t
nearest_poc(
	int64_t prev_msb,
	int64_t prev_lsb,
	int64_t lsb,
	int64_t max_lsb)
{
	int64_t previous;
	int64_t best;
	int64_t candidate;
	int64_t best_distance;
	int64_t distance;
	int step;

	/* Measures each candidate's distance; a tie goes to the higher candidate. */
	previous = prev_msb + prev_lsb;
	best = 0;
	best_distance = -1;
	for (step = -1; step <= 1; step++) {
		candidate = prev_msb + (int64_t)step * max_lsb + lsb;
		distance = candidate - previous;
		if (distance < 0)
			distance = -distance;
		if (best_distance < 0 || distance < best_distance || (distance == best_distance && candidate > best)) {
			best = candidate;
			best_distance = distance;
		}
	}

	/* Succeeded: the order count. */
	return best;
}

/* h265sw_poc_init and the arguments' checks. */
static void
check_init(void)
{
	struct h265sw_poc_state state;

	/* The state starts at 0 and 0. */
	state.prev_lsb = 0xFFFFU;
	state.prev_msb = 12345;
	h265sw_poc_init(&state);
	CHECK(state.prev_lsb == 0U);
	CHECK(state.prev_msb == 0);

	/* A missing state is not a fault. */
	h265sw_poc_init(NULL);
}

/* 8-1 by hand, for MaxPicOrderCntLsb 16 and 65536: around the half-way points. */
static void
check_compute_hand(void)
{
	int32_t poc;
	int error;

	/* Max 16, previous (lsb 0, Msb 0): lsb 8 is exactly half way up, which does not wrap; lsb 9 does (-16 + 9 = -7). */
	error = compute(0, 0U, 0, H265SW_NAL_TRAIL_R, 8U, 4U, &poc);
	CHECK(error == 0);
	CHECK(poc == 8);
	error = compute(0, 0U, 0, H265SW_NAL_TRAIL_R, 9U, 4U, &poc);
	CHECK(error == 0);
	CHECK(poc == -7);
	error = compute(0, 0U, 0, H265SW_NAL_TRAIL_R, 7U, 4U, &poc);
	CHECK(error == 0);
	CHECK(poc == 7);

	/* Max 16, previous (lsb 15, Msb 0): lsb 7 is 8 below, which wraps up (16 + 7 = 23); lsb 8 is 7 below and does not. */
	error = compute(0, 15U, 0, H265SW_NAL_TRAIL_R, 7U, 4U, &poc);
	CHECK(error == 0);
	CHECK(poc == 23);
	error = compute(0, 15U, 0, H265SW_NAL_TRAIL_R, 8U, 4U, &poc);
	CHECK(error == 0);
	CHECK(poc == 8);

	/* Max 16, previous (lsb 10, Msb 80): lsb 2 is 8 below: 96 + 2 = 98; lsb 3 is 7 below: 80 + 3 = 83. */
	error = compute(80, 10U, 0, H265SW_NAL_TRAIL_R, 2U, 4U, &poc);
	CHECK(error == 0);
	CHECK(poc == 98);
	error = compute(80, 10U, 0, H265SW_NAL_TRAIL_R, 3U, 4U, &poc);
	CHECK(error == 0);
	CHECK(poc == 83);

	/* The same lsb stays in the same cycle. */
	error = compute(80, 10U, 0, H265SW_NAL_TRAIL_R, 10U, 4U, &poc);
	CHECK(error == 0);
	CHECK(poc == 90);

	/* A wrap down: previous (lsb 2, Msb 80), lsb 11 is 9 above: 64 + 11 = 75; lsb 10 is 8 above and stays: 90. */
	error = compute(80, 2U, 0, H265SW_NAL_TRAIL_R, 11U, 4U, &poc);
	CHECK(error == 0);
	CHECK(poc == 75);
	error = compute(80, 2U, 0, H265SW_NAL_TRAIL_R, 10U, 4U, &poc);
	CHECK(error == 0);
	CHECK(poc == 90);

	/* A negative previous Msb: previous (lsb 3, Msb -32), lsb 12 is 9 above: -48 + 12 = -36; lsb 11: -32 + 11 = -21. */
	error = compute(-32, 3U, 0, H265SW_NAL_TRAIL_R, 12U, 4U, &poc);
	CHECK(error == 0);
	CHECK(poc == -36);
	error = compute(-32, 3U, 0, H265SW_NAL_TRAIL_R, 11U, 4U, &poc);
	CHECK(error == 0);
	CHECK(poc == -21);

	/* Max 65536, previous (lsb 0, Msb 0): lsb 32768 does not wrap; 32769 wraps (-65536 + 32769 = -32767). */
	error = compute(0, 0U, 0, H265SW_NAL_TRAIL_R, 32768U, 16U, &poc);
	CHECK(error == 0);
	CHECK(poc == 32768);
	error = compute(0, 0U, 0, H265SW_NAL_TRAIL_R, 32769U, 16U, &poc);
	CHECK(error == 0);
	CHECK(poc == -32767);

	/* Max 65536, previous (lsb 65535, Msb 0): lsb 32767 is 32768 below, wraps up: 65536 + 32767 = 98303; lsb 32768 does not. */
	error = compute(0, 65535U, 0, H265SW_NAL_TRAIL_R, 32767U, 16U, &poc);
	CHECK(error == 0);
	CHECK(poc == 98303);
	error = compute(0, 65535U, 0, H265SW_NAL_TRAIL_R, 32768U, 16U, &poc);
	CHECK(error == 0);
	CHECK(poc == 32768);

	/* An IRAP picture with NoRaslOutputFlag 1 has Msb 0 whatever came before. */
	error = compute(160, 3U, 1, H265SW_NAL_CRA, 5U, 4U, &poc);
	CHECK(error == 0);
	CHECK(poc == 5);
	error = compute(-160, 14U, 1, H265SW_NAL_BLA_W_LP, 9U, 4U, &poc);
	CHECK(error == 0);
	CHECK(poc == 9);

	/* The same CRA without NoRaslOutputFlag continues the count (5 is 2 above 3: Msb 160). */
	error = compute(160, 3U, 0, H265SW_NAL_CRA, 5U, 4U, &poc);
	CHECK(error == 0);
	CHECK(poc == 165);

	/* An IDR picture is order count 0 (its lsb is 0), with any state and either flag. */
	error = compute(160, 3U, 0, H265SW_NAL_IDR_W_RADL, 0U, 4U, &poc);
	CHECK(error == 0);
	CHECK(poc == 0);
	error = compute(-96, 14U, 1, H265SW_NAL_IDR_N_LP, 0U, 16U, &poc);
	CHECK(error == 0);
	CHECK(poc == 0);

	/* Every order count length from 4 to 16: the half-way point wraps down by one more than the wrap up. */
	{
		unsigned log2;
		uint32_t max_lsb;

		for (log2 = 4U; log2 <= 16U; log2++) {
			max_lsb = (uint32_t)1U << log2;
			error = compute(0, 0U, 0, H265SW_NAL_TRAIL_R, max_lsb / 2U, log2, &poc);
			CHECK(error == 0);
			CHECK(poc == (int32_t)(max_lsb / 2U));
			error = compute(0, 0U, 0, H265SW_NAL_TRAIL_R, max_lsb / 2U + 1U, log2, &poc);
			CHECK(error == 0);
			CHECK(poc == (int32_t)(max_lsb / 2U + 1U) - (int32_t)max_lsb);
			error = compute(0, max_lsb - 1U, 0, H265SW_NAL_TRAIL_R, max_lsb / 2U - 1U, log2, &poc);
			CHECK(error == 0);
			CHECK(poc == (int32_t)(max_lsb / 2U - 1U) + (int32_t)max_lsb);
			error = compute(0, max_lsb - 1U, 0, H265SW_NAL_TRAIL_R, max_lsb / 2U, log2, &poc);
			CHECK(error == 0);
			CHECK(poc == (int32_t)(max_lsb / 2U));
		}
	}
}

/* 8-1 against the nearest-order-count rule: every lsb pair for MaxPicOrderCntLsb 16 and 32, and a random sample for 65536. */
static void
check_compute_exhaustive(void)
{
	static const int32_t msbs[] = {0, 16, -16, 160, -160, 1024, -2048};
	uint32_t prev_lsb;
	uint32_t lsb;
	unsigned index;
	unsigned log2;
	unsigned iteration;
	int32_t prev_msb;
	int32_t poc;
	int64_t max_lsb;
	int error;

	/* Every pair of lsb values for two lengths, with several previous Msb values (multiples of the length). */
	for (log2 = 4U; log2 <= 5U; log2++) {
		max_lsb = (int64_t)1 << log2;
		for (index = 0U; index < sizeof(msbs) / sizeof(msbs[0]); index++) {
			prev_msb = msbs[index] * (int32_t)(max_lsb / 16);
			for (prev_lsb = 0U; prev_lsb < (uint32_t)max_lsb; prev_lsb++) {
				for (lsb = 0U; lsb < (uint32_t)max_lsb; lsb++) {
					error = compute(prev_msb, prev_lsb, 0, H265SW_NAL_TRAIL_R, lsb, log2, &poc);
					CHECK(error == 0);
					CHECK((int64_t)poc == nearest_poc(prev_msb, prev_lsb, lsb, max_lsb));
				}
			}
		}
	}

	/* A sample for the other lengths. */
	for (iteration = 0U; iteration < 100000U; iteration++) {
		log2 = 4U + rnd(13U);
		max_lsb = (int64_t)1 << log2;
		prev_msb = (int32_t)((int64_t)((int32_t)rnd(2000U) - 1000) * max_lsb);
		prev_lsb = rnd((uint32_t)max_lsb);
		if (rnd(4U) == 0U) {
			lsb = (prev_lsb + (uint32_t)(max_lsb / 2) + rnd(3U) + (uint32_t)max_lsb - 1U) % (uint32_t)max_lsb;
		} else {
			lsb = rnd((uint32_t)max_lsb);
		}
		error = compute(prev_msb, prev_lsb, 0, H265SW_NAL_TRAIL_R, lsb, log2, &poc);
		CHECK(error == 0);
		CHECK((int64_t)poc == nearest_poc(prev_msb, prev_lsb, lsb, max_lsb));
	}
}

/* The limits of the derivation: the range of int32_t and the arguments. */
static void
check_compute_limits(void)
{
	static struct h265sw_slice_header header;
	struct h265sw_poc_state state;
	int32_t poc;
	int error;

	/* The largest order count is allowed; one more is not. */
	error = compute(2147483632, 15U, 0, H265SW_NAL_TRAIL_R, 15U, 4U, &poc);
	CHECK(error == 0);
	CHECK(poc == 2147483647);
	error = compute(2147483632, 15U, 0, H265SW_NAL_TRAIL_R, 7U, 4U, &poc);
	CHECK(error == EINVAL);

	/* The smallest is allowed (Msb -2^31, lsb 0); wrapping down from it is not. */
	error = compute((int32_t)INT32_BOTTOM, 0U, 0, H265SW_NAL_TRAIL_R, 0U, 4U, &poc);
	CHECK(error == 0);
	CHECK(poc == (int32_t)INT32_BOTTOM);
	error = compute((int32_t)INT32_BOTTOM, 0U, 0, H265SW_NAL_TRAIL_R, 9U, 4U, &poc);
	CHECK(error == EINVAL);
	error = compute((int32_t)INT32_BOTTOM, 0U, 0, H265SW_NAL_TRAIL_R, 8U, 4U, &poc);
	CHECK(error == 0);
	CHECK(poc == (int32_t)INT32_BOTTOM + 8);

	/* The same with 65536: the Msb steps are large. */
	error = compute(2147418112, 65535U, 0, H265SW_NAL_TRAIL_R, 65535U, 16U, &poc);
	CHECK(error == 0);
	CHECK(poc == 2147483647);
	error = compute(2147418112, 65535U, 0, H265SW_NAL_TRAIL_R, 100U, 16U, &poc);
	CHECK(error == EINVAL);
	error = compute((int32_t)INT32_BOTTOM, 0U, 0, H265SW_NAL_TRAIL_R, 40000U, 16U, &poc);
	CHECK(error == EINVAL);

	/* The order count length is 4 to 16. */
	error = compute(0, 0U, 0, H265SW_NAL_TRAIL_R, 1U, 3U, &poc);
	CHECK(error == EINVAL);
	error = compute(0, 0U, 0, H265SW_NAL_TRAIL_R, 1U, 17U, &poc);
	CHECK(error == EINVAL);
	error = compute(0, 0U, 0, H265SW_NAL_TRAIL_R, 1U, 0U, &poc);
	CHECK(error == EINVAL);
	error = compute(0, 0U, 0, H265SW_NAL_TRAIL_R, 1U, 32U, &poc);
	CHECK(error == EINVAL);

	/* A missing argument; the state is not changed by a derivation. */
	state.prev_lsb = 4U;
	state.prev_msb = 32;
	make_header(&header, H265SW_NAL_TRAIL_R, 0U, 7U);
	CHECK(h265sw_poc_compute(NULL, &header, 0, 4U, &poc) == EINVAL);
	CHECK(h265sw_poc_compute(&state, NULL, 0, 4U, &poc) == EINVAL);
	CHECK(h265sw_poc_compute(&state, &header, 0, 4U, NULL) == EINVAL);
	CHECK(h265sw_poc_compute(&state, &header, 0, 4U, &poc) == 0);
	CHECK(poc == 39);
	CHECK(state.prev_lsb == 4U);
	CHECK(state.prev_msb == 32);
}

/* h265sw_poc_finish: which pictures become prevTid0Pic. */
static void
check_poc_finish(void)
{
	static struct h265sw_slice_header header;
	struct h265sw_poc_state state;
	unsigned type;
	unsigned temporal_id;
	int record;
	int slnr;
	int leading;

	/* Every NAL type 0..31 and TemporalId 0 and 1: recorded when TemporalId is 0 and the type is not RASL, RADL or an even type up to 14. */
	for (type = 0U; type < 32U; type++) {
		for (temporal_id = 0U; temporal_id < 2U; temporal_id++) {
			state.prev_lsb = 3U;
			state.prev_msb = 16;
			make_header(&header, type, temporal_id, 11U);
			h265sw_poc_finish(&state, &header, 59);

			slnr = (type <= 14U && (type & 1U) == 0U);
			leading = (type >= 6U && type <= 9U);
			record = (temporal_id == 0U && slnr == 0 && leading == 0);
			if (record != 0) {
				CHECK(state.prev_lsb == 11U);
				CHECK(state.prev_msb == 48);
			} else {
				CHECK(state.prev_lsb == 3U);
				CHECK(state.prev_msb == 16);
			}
		}
	}

	/* A negative order count: Msb = poc - lsb (poc -5, lsb 11 gives -16). */
	state.prev_lsb = 3U;
	state.prev_msb = 16;
	make_header(&header, H265SW_NAL_TRAIL_R, 0U, 11U);
	h265sw_poc_finish(&state, &header, -5);
	CHECK(state.prev_lsb == 11U);
	CHECK(state.prev_msb == -16);

	/* The extremes: the smallest order count with lsb 0, and the largest. */
	make_header(&header, H265SW_NAL_TRAIL_R, 0U, 0U);
	h265sw_poc_finish(&state, &header, (int32_t)INT32_BOTTOM);
	CHECK(state.prev_lsb == 0U);
	CHECK(state.prev_msb == (int32_t)INT32_BOTTOM);
	make_header(&header, H265SW_NAL_CRA, 0U, 15U);
	h265sw_poc_finish(&state, &header, 2147483647);
	CHECK(state.prev_lsb == 15U);
	CHECK(state.prev_msb == 2147483632);

	/* A hand-made order count whose Msb would leave int32_t is held at the end of the range, not wrapped. */
	make_header(&header, H265SW_NAL_TRAIL_R, 0U, 100U);
	h265sw_poc_finish(&state, &header, (int32_t)INT32_BOTTOM + 5);
	CHECK(state.prev_lsb == 100U);
	CHECK(state.prev_msb == (int32_t)INT32_BOTTOM);

	/* A missing argument is not a fault. */
	h265sw_poc_finish(NULL, &header, 0);
	h265sw_poc_finish(&state, NULL, 0);
}

/* A decoding order with leading, non-reference and higher sub-layer pictures: each picture's order count with the state kept by finish. */
static void
check_sequence(void)
{
	static const struct {
		unsigned nal_type;
		unsigned temporal_id;
		uint32_t lsb;
		int irap_no_rasl;
		int32_t expected;
	} pictures[] = {
		/* IDR, then lsb 8, 0, 8, 0: each is half a cycle from the one before and wraps up at 0 (16, 24, 32). */
		{19U, 0U, 0U, 1, 0},
		{1U, 0U, 8U, 0, 8},
		{1U, 0U, 0U, 0, 16},
		{1U, 0U, 8U, 0, 24},
		{1U, 0U, 0U, 0, 32},
		/*
		 * IDR again; then, eight times: a recorded picture at lsb 2, a
		 * picture that is not recorded at lsb 9 (a higher sub-layer, a
		 * sub-layer non-reference, RADL or RASL), and a recorded one at
		 * lsb 1.  Against the recorded lsb 2 the last is 1 below and
		 * stays (1); had the lsb 9 picture been recorded it would be 8
		 * below and wrap up (17).
		 */
		{19U, 0U, 0U, 1, 0},
		{1U, 0U, 2U, 0, 2},
		{1U, 1U, 9U, 0, 9},
		{1U, 0U, 1U, 0, 1},
		{1U, 0U, 2U, 0, 2},
		{0U, 0U, 9U, 0, 9},
		{1U, 0U, 1U, 0, 1},
		{1U, 0U, 2U, 0, 2},
		{2U, 2U, 9U, 0, 9},
		{1U, 0U, 1U, 0, 1},
		{1U, 0U, 2U, 0, 2},
		{4U, 1U, 9U, 0, 9},
		{1U, 0U, 1U, 0, 1},
		{1U, 0U, 2U, 0, 2},
		{6U, 0U, 9U, 0, 9},
		{1U, 0U, 1U, 0, 1},
		{1U, 0U, 2U, 0, 2},
		{7U, 0U, 9U, 0, 9},
		{1U, 0U, 1U, 0, 1},
		{1U, 0U, 2U, 0, 2},
		{8U, 0U, 9U, 0, 9},
		{1U, 0U, 1U, 0, 1},
		{1U, 0U, 2U, 0, 2},
		{9U, 0U, 9U, 0, 9},
		{1U, 0U, 1U, 0, 1},
		/*
		 * A BLA picture with NoRaslOutputFlag 1 at lsb 12 is recorded:
		 * lsb 4 is then 8 below it and wraps up (20); against the state
		 * before it (1) it would be 4 and 3 above and give 4.
		 */
		{16U, 0U, 12U, 1, 12},
		{1U, 0U, 4U, 0, 20},
		/*
		 * A CRA without NoRaslOutputFlag at lsb 6 continues the count
		 * (22) and is recorded: lsb 14 is 8 above it and stays (30); had
		 * the CRA not been recorded, lsb 14 would be 10 above 4 and
		 * wrap down (14).
		 */
		{21U, 0U, 6U, 0, 22},
		{1U, 0U, 14U, 0, 30}
	};
	static struct h265sw_slice_header header;
	struct h265sw_poc_state state;
	unsigned index;
	int32_t poc;
	int error;

	/* Walks the pictures, deriving each order count and recording the picture as the decoding process would. */
	h265sw_poc_init(&state);
	for (index = 0U; index < sizeof(pictures) / sizeof(pictures[0]); index++) {
		make_header(&header, pictures[index].nal_type, pictures[index].temporal_id, pictures[index].lsb);
		error = h265sw_poc_compute(&state, &header, pictures[index].irap_no_rasl, 4U, &poc);
		CHECK(error == 0);
		CHECK(poc == pictures[index].expected);
		h265sw_poc_finish(&state, &header, poc);
	}
}

/* 8-5 by hand: the five lists for an IDR picture and for pictures with short-term and long-term entries. */
static void
check_rps_hand(void)
{
	static struct h265sw_slice_header header;
	static struct h265sw_rps_pocs out;
	unsigned index;
	int error;

	/* An IDR picture has every list empty, whatever the header holds. */
	make_header(&header, H265SW_NAL_IDR_W_RADL, 0U, 0U);
	header.st_rps.num_negative = 2U;
	header.st_rps.num_positive = 1U;
	header.st_rps.delta_poc[0] = -1;
	header.st_rps.delta_poc[1] = -2;
	header.st_rps.delta_poc[2] = 3;
	header.st_rps.used[0] = 1U;
	header.num_long_term_sps = 1U;
	header.lt_used[0] = 1U;
	memset(&out, 0xCC, sizeof(out));
	error = h265sw_poc_rps(&header, 0, 4U, &out);
	CHECK(error == 0);
	CHECK(out.num_st_before == 0U);
	CHECK(out.num_st_after == 0U);
	CHECK(out.num_st_foll == 0U);
	CHECK(out.num_lt_curr == 0U);
	CHECK(out.num_lt_foll == 0U);
	for (index = 0U; index < H265SW_MAX_RPS; index++) {
		CHECK(out.st_before[index] == 0);
		CHECK(out.st_after[index] == 0);
		CHECK(out.st_foll[index] == 0);
		CHECK(out.lt_curr[index] == 0);
		CHECK(out.lt_foll[index] == 0);
		CHECK(out.lt_curr_msb[index] == 0U);
		CHECK(out.lt_foll_msb[index] == 0U);
	}
	make_header(&header, H265SW_NAL_IDR_N_LP, 0U, 0U);
	error = h265sw_poc_rps(&header, 0, 16U, &out);
	CHECK(error == 0);
	CHECK(out.num_st_before == 0U);

	/*
	 * Short-term entries at poc 100: negative deltas -1 (used), -3 (not
	 * used), -4 (used); positive +2 (not used), +5 (used).  Visiting the
	 * negative ones in order: Before gets 99 and 96, Foll gets 97.  Then
	 * the positive ones: Foll gets 102, After gets 105.
	 */
	make_header(&header, H265SW_NAL_TRAIL_R, 0U, 4U);
	header.st_rps.num_negative = 3U;
	header.st_rps.num_positive = 2U;
	header.st_rps.delta_poc[0] = -1;
	header.st_rps.delta_poc[1] = -3;
	header.st_rps.delta_poc[2] = -4;
	header.st_rps.delta_poc[3] = 2;
	header.st_rps.delta_poc[4] = 5;
	header.st_rps.used[0] = 1U;
	header.st_rps.used[1] = 0U;
	header.st_rps.used[2] = 1U;
	header.st_rps.used[3] = 0U;
	header.st_rps.used[4] = 1U;
	memset(&out, 0xCC, sizeof(out));
	error = h265sw_poc_rps(&header, 100, 4U, &out);
	CHECK(error == 0);
	CHECK(out.num_st_before == 2U);
	CHECK(out.st_before[0] == 99);
	CHECK(out.st_before[1] == 96);
	CHECK(out.num_st_after == 1U);
	CHECK(out.st_after[0] == 105);
	CHECK(out.num_st_foll == 2U);
	CHECK(out.st_foll[0] == 97);
	CHECK(out.st_foll[1] == 102);
	CHECK(out.num_lt_curr == 0U);
	CHECK(out.num_lt_foll == 0U);
	CHECK(out.st_before[2] == 0);
	CHECK(out.st_after[1] == 0);
	CHECK(out.st_foll[2] == 0);
	CHECK(out.lt_curr[0] == 0);

	/*
	 * Long-term entries at poc 100 with MaxPicOrderCntLsb 16 (100 & 15 =
	 * 4): entry 0 lsb 5, used, with msb cycle 2: 5 + 100 - 2 * 16 - 4 =
	 * 69.  Entry 1 lsb 9, not used, no msb: 9.  Entry 2 lsb 3, used, with
	 * cycle 0: 3 + 100 - 0 - 4 = 99.  Entry 3 lsb 12, not used, with
	 * cycle 1: 12 + 100 - 16 - 4 = 92.
	 */
	header.st_rps.num_negative = 0U;
	header.st_rps.num_positive = 0U;
	header.num_long_term_sps = 1U;
	header.num_long_term_pics = 3U;
	header.num_long_term = 4U;
	header.lt_poc_lsb[0] = 5U;
	header.lt_used[0] = 1U;
	header.lt_msb_present[0] = 1U;
	header.lt_msb_cycle[0] = 2U;
	header.lt_poc_lsb[1] = 9U;
	header.lt_used[1] = 0U;
	header.lt_msb_present[1] = 0U;
	header.lt_msb_cycle[1] = 2U;
	header.lt_poc_lsb[2] = 3U;
	header.lt_used[2] = 1U;
	header.lt_msb_present[2] = 1U;
	header.lt_msb_cycle[2] = 0U;
	header.lt_poc_lsb[3] = 12U;
	header.lt_used[3] = 0U;
	header.lt_msb_present[3] = 1U;
	header.lt_msb_cycle[3] = 1U;
	memset(&out, 0xCC, sizeof(out));
	error = h265sw_poc_rps(&header, 100, 4U, &out);
	CHECK(error == 0);
	CHECK(out.num_st_before == 0U);
	CHECK(out.num_st_after == 0U);
	CHECK(out.num_st_foll == 0U);
	CHECK(out.num_lt_curr == 2U);
	CHECK(out.lt_curr[0] == 69);
	CHECK(out.lt_curr[1] == 99);
	CHECK(out.lt_curr_msb[0] == 1U);
	CHECK(out.lt_curr_msb[1] == 1U);
	CHECK(out.num_lt_foll == 2U);
	CHECK(out.lt_foll[0] == 9);
	CHECK(out.lt_foll[1] == 92);
	CHECK(out.lt_foll_msb[0] == 0U);
	CHECK(out.lt_foll_msb[1] == 1U);
	CHECK(out.lt_curr_msb[2] == 0U);

	/* A used entry without msb: its order count is its lsb, and the flag is 0. */
	header.num_long_term_sps = 0U;
	header.num_long_term_pics = 1U;
	header.num_long_term = 1U;
	header.lt_poc_lsb[0] = 13U;
	header.lt_used[0] = 1U;
	header.lt_msb_present[0] = 0U;
	header.lt_msb_cycle[0] = 7U;
	error = h265sw_poc_rps(&header, 100, 4U, &out);
	CHECK(error == 0);
	CHECK(out.num_lt_curr == 1U);
	CHECK(out.lt_curr[0] == 13);
	CHECK(out.lt_curr_msb[0] == 0U);
	CHECK(out.num_lt_foll == 0U);

	/* A cycle 7 with the flag set: 13 + 100 - 7 * 16 - 4 = -3, negative order counts are fine. */
	header.lt_msb_present[0] = 1U;
	error = h265sw_poc_rps(&header, 100, 4U, &out);
	CHECK(error == 0);
	CHECK(out.lt_curr[0] == -3);
	CHECK(out.lt_curr_msb[0] == 1U);

	/* The count of long-term entries is the sum of the two counts, not num_long_term. */
	header.num_long_term_sps = 1U;
	header.num_long_term_pics = 1U;
	header.num_long_term = 0U;
	header.lt_poc_lsb[1] = 14U;
	header.lt_used[1] = 1U;
	header.lt_msb_present[1] = 0U;
	error = h265sw_poc_rps(&header, 100, 4U, &out);
	CHECK(error == 0);
	CHECK(out.num_lt_curr == 2U);
	CHECK(out.lt_curr[1] == 14);
}

/* 8-5 for negative order counts: the bitwise AND is a floor, and large cycle counts. */
static void
check_rps_negative(void)
{
	static struct h265sw_slice_header header;
	static struct h265sw_rps_pocs out;
	int error;

	/* Order count -1 with MaxPicOrderCntLsb 16: -1 & 15 = 15, so lsb 15 with cycle 0 is the picture itself (-1), cycle 1 is -17. */
	make_header(&header, H265SW_NAL_TRAIL_R, 0U, 15U);
	header.num_long_term_pics = 2U;
	header.num_long_term = 2U;
	header.lt_poc_lsb[0] = 15U;
	header.lt_used[0] = 1U;
	header.lt_msb_present[0] = 1U;
	header.lt_msb_cycle[0] = 0U;
	header.lt_poc_lsb[1] = 15U;
	header.lt_used[1] = 1U;
	header.lt_msb_present[1] = 1U;
	header.lt_msb_cycle[1] = 1U;
	error = h265sw_poc_rps(&header, -1, 4U, &out);
	CHECK(error == 0);
	CHECK(out.num_lt_curr == 2U);
	CHECK(out.lt_curr[0] == -1);
	CHECK(out.lt_curr[1] == -17);

	/* Order count -17: -17 & 15 = 15 (-17 + 32); lsb 15 cycle 0 is -17. */
	error = h265sw_poc_rps(&header, -17, 4U, &out);
	CHECK(error == 0);
	CHECK(out.lt_curr[0] == -17);
	CHECK(out.lt_curr[1] == -33);

	/* Order count -16: -16 & 15 = 0; lsb 3 cycle 0 is -13 (3 + -16 - 0 - 0). */
	header.lt_poc_lsb[0] = 3U;
	header.lt_poc_lsb[1] = 3U;
	error = h265sw_poc_rps(&header, -16, 4U, &out);
	CHECK(error == 0);
	CHECK(out.lt_curr[0] == -13);
	CHECK(out.lt_curr[1] == -29);

	/* With 65536: order count -65537 & 65535 = 65535; lsb 65535 cycle 0 is the picture. */
	header.lt_poc_lsb[0] = 65535U;
	header.lt_poc_lsb[1] = 65535U;
	error = h265sw_poc_rps(&header, -65537, 16U, &out);
	CHECK(error == 0);
	CHECK(out.lt_curr[0] == -65537);
	CHECK(out.lt_curr[1] == -65537 - 65536);

	/* Order count 5 with 65536 and cycle 32768 * ... : lsb 0, cycle 32768 gives 0 + 5 - 2^31 - 5 = -2^31, the smallest. */
	header.num_long_term_pics = 1U;
	header.num_long_term = 1U;
	header.lt_poc_lsb[0] = 0U;
	header.lt_msb_cycle[0] = 32768U;
	error = h265sw_poc_rps(&header, 5, 16U, &out);
	CHECK(error == 0);
	CHECK(out.lt_curr[0] == (int32_t)INT32_BOTTOM);

	/* One cycle more leaves int32_t; so does the largest cycle count. */
	header.lt_msb_cycle[0] = 32769U;
	CHECK(h265sw_poc_rps(&header, 5, 16U, &out) == EINVAL);
	header.lt_msb_cycle[0] = 0xFFFFFFFFU;
	CHECK(h265sw_poc_rps(&header, 5, 16U, &out) == EINVAL);
	header.lt_msb_cycle[0] = 0xFFFFFFFFU;
	CHECK(h265sw_poc_rps(&header, 5, 4U, &out) == EINVAL);

	/* A large cycle with a large order count: 2^31 - 1 down by 32767 cycles of 65536 and lsb 65535. */
	header.lt_poc_lsb[0] = 65535U;
	header.lt_msb_cycle[0] = 1U;
	error = h265sw_poc_rps(&header, 2147483647, 16U, &out);
	CHECK(error == 0);
	CHECK(out.lt_curr[0] == 2147483647 - 65536);
}

/* The limits of 8-5: sums outside int32_t, the sizes of the lists, and the arguments. */
static void
check_rps_limits(void)
{
	static struct h265sw_slice_header header;
	static struct h265sw_rps_pocs out;
	unsigned index;
	int error;

	/* Short-term: the largest order count with a positive delta of 5 and back to the end of the range. */
	make_header(&header, H265SW_NAL_TRAIL_R, 0U, 0U);
	header.st_rps.num_positive = 1U;
	header.st_rps.delta_poc[0] = 5;
	header.st_rps.used[0] = 1U;
	error = h265sw_poc_rps(&header, 2147483642, 4U, &out);
	CHECK(error == 0);
	CHECK(out.st_after[0] == 2147483647);
	error = h265sw_poc_rps(&header, 2147483643, 4U, &out);
	CHECK(error == EINVAL);
	header.st_rps.used[0] = 0U;
	error = h265sw_poc_rps(&header, 2147483643, 4U, &out);
	CHECK(error == EINVAL);

	/* The smallest end. */
	make_header(&header, H265SW_NAL_TRAIL_R, 0U, 0U);
	header.st_rps.num_negative = 1U;
	header.st_rps.delta_poc[0] = -7;
	header.st_rps.used[0] = 1U;
	error = h265sw_poc_rps(&header, (int32_t)INT32_BOTTOM + 7, 4U, &out);
	CHECK(error == 0);
	CHECK(out.st_before[0] == (int32_t)INT32_BOTTOM);
	error = h265sw_poc_rps(&header, (int32_t)INT32_BOTTOM + 6, 4U, &out);
	CHECK(error == EINVAL);

	/* A delta of 2^31 - 1 magnitude (the largest a set can hold) added to a negative order count stays inside. */
	header.st_rps.delta_poc[0] = 2147483647;
	header.st_rps.num_negative = 0U;
	header.st_rps.num_positive = 1U;
	error = h265sw_poc_rps(&header, -1, 4U, &out);
	CHECK(error == 0);
	CHECK(out.st_after[0] == 2147483646);
	error = h265sw_poc_rps(&header, 1, 4U, &out);
	CHECK(error == EINVAL);

	/* A long-term entry without msb whose lsb is above int32_t (a hand-made header) leaves the range. */
	make_header(&header, H265SW_NAL_TRAIL_R, 0U, 0U);
	header.num_long_term_pics = 1U;
	header.num_long_term = 1U;
	header.lt_poc_lsb[0] = 0x80000000U;
	header.lt_used[0] = 1U;
	error = h265sw_poc_rps(&header, 0, 4U, &out);
	CHECK(error == EINVAL);
	header.lt_poc_lsb[0] = 0x7FFFFFFFU;
	error = h265sw_poc_rps(&header, 0, 4U, &out);
	CHECK(error == 0);
	CHECK(out.lt_curr[0] == 2147483647);

	/* The lists hold 15 entries: 15 short-term entries are taken, 16 are not. */
	make_header(&header, H265SW_NAL_TRAIL_R, 0U, 0U);
	header.st_rps.num_negative = 15U;
	for (index = 0U; index < 15U; index++) {
		header.st_rps.delta_poc[index] = -(int32_t)index - 1;
		header.st_rps.used[index] = 1U;
	}
	error = h265sw_poc_rps(&header, 100, 4U, &out);
	CHECK(error == 0);
	CHECK(out.num_st_before == 15U);
	CHECK(out.st_before[14] == 85);
	header.st_rps.num_negative = 16U;
	CHECK(h265sw_poc_rps(&header, 100, 4U, &out) == EINVAL);
	header.st_rps.num_negative = 8U;
	header.st_rps.num_positive = 8U;
	CHECK(h265sw_poc_rps(&header, 100, 4U, &out) == EINVAL);
	header.st_rps.num_negative = 0U;
	header.st_rps.num_positive = 16U;
	CHECK(h265sw_poc_rps(&header, 100, 4U, &out) == EINVAL);
	header.st_rps.num_negative = 0xFFFFFFFFU;
	header.st_rps.num_positive = 2U;
	CHECK(h265sw_poc_rps(&header, 100, 4U, &out) == EINVAL);

	/* 15 long-term entries are taken, 16 are not, and two huge counts do not wrap their sum. */
	make_header(&header, H265SW_NAL_TRAIL_R, 0U, 0U);
	header.num_long_term_sps = 5U;
	header.num_long_term_pics = 10U;
	for (index = 0U; index < 16U; index++) {
		header.lt_poc_lsb[index] = index;
		header.lt_used[index] = index & 1U;
	}
	error = h265sw_poc_rps(&header, 100, 4U, &out);
	CHECK(error == 0);
	CHECK(out.num_lt_curr == 7U);
	CHECK(out.num_lt_foll == 8U);
	header.num_long_term_pics = 11U;
	CHECK(h265sw_poc_rps(&header, 100, 4U, &out) == EINVAL);
	header.num_long_term_sps = 0xFFFFFFFFU;
	header.num_long_term_pics = 2U;
	CHECK(h265sw_poc_rps(&header, 100, 4U, &out) == EINVAL);

	/* The order count length is 4 to 16, and every pointer is needed. */
	make_header(&header, H265SW_NAL_TRAIL_R, 0U, 0U);
	CHECK(h265sw_poc_rps(&header, 0, 3U, &out) == EINVAL);
	CHECK(h265sw_poc_rps(&header, 0, 17U, &out) == EINVAL);
	CHECK(h265sw_poc_rps(&header, 0, 4U, &out) == 0);
	CHECK(h265sw_poc_rps(&header, 0, 16U, &out) == 0);
	CHECK(h265sw_poc_rps(NULL, 0, 4U, &out) == EINVAL);
	CHECK(h265sw_poc_rps(&header, 0, 4U, NULL) == EINVAL);

	/* The output is cleared before use, even for entries left from an earlier call. */
	memset(&out, 0x55, sizeof(out));
	CHECK(h265sw_poc_rps(&header, 0, 4U, &out) == 0);
	CHECK(out.num_st_before == 0U);
	CHECK(out.st_before[3] == 0);
	CHECK(out.lt_foll_msb[5] == 0U);
}

/* Random headers against a second statement of 8-5: collect, then classify, with the remainder by arithmetic. */
static void
check_rps_random(void)
{
	static struct h265sw_slice_header header;
	static struct h265sw_rps_pocs out;
	int64_t want_before[H265SW_MAX_RPS];
	int64_t want_after[H265SW_MAX_RPS];
	int64_t want_foll[H265SW_MAX_RPS];
	int64_t want_lt_curr[H265SW_MAX_RPS];
	int64_t want_lt_foll[H265SW_MAX_RPS];
	unsigned want_lt_curr_msb[H265SW_MAX_RPS];
	unsigned want_lt_foll_msb[H265SW_MAX_RPS];
	unsigned before;
	unsigned after;
	unsigned foll;
	unsigned lt_curr;
	unsigned lt_foll;
	unsigned iteration;
	unsigned index;
	unsigned log2;
	unsigned negative;
	unsigned positive;
	unsigned long_term;
	int64_t poc;
	int64_t max_lsb;
	int64_t low;
	int64_t value;
	int error;
	int expect_ok;

	/* Random sets, order counts around 0 and near the ends of int32_t, and every order count length. */
	for (iteration = 0U; iteration < 100000U; iteration++) {
		log2 = 4U + rnd(13U);
		max_lsb = (int64_t)1 << log2;
		negative = rnd(8U);
		positive = rnd(8U);
		long_term = rnd(15U - negative - positive + 1U);
		switch (rnd(4U)) {
		case 0U:
			poc = (int64_t)rnd(400U) - 200;
			break;
		case 1U:
			poc = INT32_TOP - (int64_t)rnd(200U);
			break;
		case 2U:
			poc = INT32_BOTTOM + (int64_t)rnd(200U);
			break;
		default:
			poc = (int64_t)(int32_t)random_next();
			break;
		}

		make_header(&header, H265SW_NAL_TRAIL_R, 0U, 0U);
		header.st_rps.num_negative = negative;
		header.st_rps.num_positive = positive;
		for (index = 0U; index < negative; index++) {
			header.st_rps.delta_poc[index] = -(int32_t)(index * 7U + 1U + rnd(5U));
			header.st_rps.used[index] = (uint8_t)rnd(2U);
		}
		for (index = 0U; index < positive; index++) {
			header.st_rps.delta_poc[negative + index] = (int32_t)(index * 7U + 1U + rnd(5U));
			header.st_rps.used[negative + index] = (uint8_t)rnd(2U);
		}
		header.num_long_term_sps = long_term / 2U;
		header.num_long_term_pics = long_term - long_term / 2U;
		header.num_long_term = long_term;
		for (index = 0U; index < long_term; index++) {
			header.lt_poc_lsb[index] = rnd((uint32_t)max_lsb);
			header.lt_used[index] = (uint8_t)rnd(2U);
			header.lt_msb_present[index] = (uint8_t)rnd(2U);
			header.lt_msb_cycle[index] = rnd(100000U) % (rnd(3U) == 0U ? 100000U : 40U);
		}

		/* The expected lists: every sum made in int64_t, the validity being that every entry fits int32_t. */
		before = 0U;
		after = 0U;
		foll = 0U;
		lt_curr = 0U;
		lt_foll = 0U;
		expect_ok = 1;
		for (index = 0U; index < negative + positive; index++) {
			value = poc + header.st_rps.delta_poc[index];
			if (value < INT32_BOTTOM || value > INT32_TOP)
				expect_ok = 0;
			if (header.st_rps.used[index] != 0U && index < negative)
				want_before[before++] = value;
			else if (header.st_rps.used[index] != 0U)
				want_after[after++] = value;
			else
				want_foll[foll++] = value;
		}

		/* A remainder in 0..max_lsb - 1 for any sign: ( ( x % m ) + m ) % m. */
		low = ((poc % max_lsb) + max_lsb) % max_lsb;
		for (index = 0U; index < long_term; index++) {
			value = header.lt_poc_lsb[index];
			if (header.lt_msb_present[index] != 0U)
				value = value + poc - (int64_t)header.lt_msb_cycle[index] * max_lsb - low;
			if (value < INT32_BOTTOM || value > INT32_TOP)
				expect_ok = 0;
			if (header.lt_used[index] != 0U) {
				want_lt_curr[lt_curr] = value;
				want_lt_curr_msb[lt_curr++] = header.lt_msb_present[index];
			} else {
				want_lt_foll[lt_foll] = value;
				want_lt_foll_msb[lt_foll++] = header.lt_msb_present[index];
			}
		}

		/* The derivation agrees on validity, and on every list when valid. */
		error = h265sw_poc_rps(&header, (int32_t)poc, log2, &out);
		if (expect_ok == 0) {
			CHECK(error == EINVAL);
			continue;
		}
		CHECK(error == 0);
		CHECK(out.num_st_before == before);
		CHECK(out.num_st_after == after);
		CHECK(out.num_st_foll == foll);
		CHECK(out.num_lt_curr == lt_curr);
		CHECK(out.num_lt_foll == lt_foll);
		for (index = 0U; index < before; index++)
			CHECK((int64_t)out.st_before[index] == want_before[index]);
		for (index = 0U; index < after; index++)
			CHECK((int64_t)out.st_after[index] == want_after[index]);
		for (index = 0U; index < foll; index++)
			CHECK((int64_t)out.st_foll[index] == want_foll[index]);
		for (index = 0U; index < lt_curr; index++) {
			CHECK((int64_t)out.lt_curr[index] == want_lt_curr[index]);
			CHECK((unsigned)out.lt_curr_msb[index] == want_lt_curr_msb[index]);
		}
		for (index = 0U; index < lt_foll; index++) {
			CHECK((int64_t)out.lt_foll[index] == want_lt_foll[index]);
			CHECK((unsigned)out.lt_foll_msb[index] == want_lt_foll_msb[index]);
		}
	}
}

int
main(void)
{
	check_init();
	check_compute_hand();
	check_compute_exhaustive();
	check_compute_limits();
	check_poc_finish();
	check_sequence();
	check_rps_hand();
	check_rps_negative();
	check_rps_limits();
	check_rps_random();

	return check_finish("poc");
}
