/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the picture order count of the software H.264 decoder (8.2.1):
 * the three types with sequences worked out by hand from the equations
 * 8-3 to 8-13, the carried state, the reset after operation 5, and the
 * errors for values that leave the ranges of the specification.
 */

#include "check.h"

#include "userland/desktop/libmedia/h264sw-poc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The most the tests use of the int32_t range. */
#define TEST_INT32_MAX 2147483647
#define TEST_INT32_MIN (-2147483647 - 1)

/*
 * Fills an SPS with the fields of 8.2.1: the order count type, and a
 * log2_max_frame_num of 4 (MaxFrameNum 16) so frame_num wraps within a
 * few pictures.
 */
static void
make_sps(
	struct h264sw_sps *sps,
	unsigned poc_type,
	unsigned log2_lsb)
{
	/* Everything not named stays 0. */
	memset(sps, 0, sizeof(*sps));
	sps->log2_max_frame_num = 4U;
	sps->max_frame_num = 16U;
	sps->poc_type = poc_type;
	sps->log2_max_poc_lsb = log2_lsb;
}

/*
 * Sets the cycle of type 1 and ExpectedDeltaPerPicOrderCntCycle (7-12) from it.
 */
static void
set_cycle(
	struct h264sw_sps *sps,
	unsigned length,
	const int32_t *offsets,
	int32_t non_ref,
	int32_t top_to_bottom)
{
	unsigned index;
	int64_t total;

	/* Copies the offsets and sums them. */
	total = 0;
	for (index = 0U;
	     index < length;
	     index++) {
		sps->offset_for_ref_frame[index] = offsets[index];
		total += offsets[index];
	}
	sps->poc_cycle_length = length;
	sps->expected_delta_per_poc_cycle = total;
	sps->offset_for_non_ref_pic = non_ref;
	sps->offset_for_top_to_bottom_field = top_to_bottom;
}

/*
 * Fills a slice header with the fields 8.2.1 reads.
 */
static void
make_header(
	struct h264sw_slice_header *header,
	int idr,
	unsigned ref_idc,
	uint32_t frame_num,
	uint32_t lsb)
{
	/* Everything not named stays 0. */
	memset(header, 0, sizeof(*header));
	header->idr = idr;
	header->nal_type = 1U;
	if (idr)
		header->nal_type = 5U;
	header->nal_ref_idc = ref_idc;
	header->frame_num = frame_num;
	header->poc_lsb = lsb;
}

/*
 * Derives, checks and carries on one picture.
 */
static void
run_picture(
	struct h264sw_poc_state *state,
	const struct h264sw_sps *sps,
	const struct h264sw_slice_header *header,
	int32_t top,
	int32_t bottom)
{
	struct h264sw_poc poc;
	int error;
	int32_t expected_poc;

	/* Derives the counts. */
	memset(&poc, 0, sizeof(poc));
	error = h264sw_poc_compute(state, sps, header, &poc);
	CHECK(error == 0);
	if (error != 0)
		return;

	/* The counts are the expected ones and PicOrderCnt of the frame is the smaller (8-1). */
	expected_poc = top;
	if (bottom < expected_poc)
		expected_poc = bottom;
	CHECK(poc.top == top);
	CHECK(poc.bottom == bottom);
	CHECK(poc.poc == expected_poc);

	/* Carries the state past the picture. */
	h264sw_poc_finish(state, sps, header, &poc);
}

/*
 * Checks that init clears every count.
 */
static void
test_init(void)
{
	struct h264sw_poc_state state;

	/* Starts from bytes that are not zero. */
	memset(&state, 0x5A, sizeof(state));
	h264sw_poc_init(&state);
	CHECK(state.prev_msb == 0);
	CHECK(state.prev_lsb == 0);
	CHECK(state.prev_frame_num == 0U);
	CHECK(state.prev_frame_num_offset == 0);
	CHECK(state.prev_mmco5 == 0);
	CHECK(state.prev_ref_frame_num == 0U);
}

/*
 * Checks type 0 with MaxPicOrderCntLsb 16: the wraps of 8-3, the bottom count, and the previous reference picture.
 */
static void
test_type0_wrap(void)
{
	struct h264sw_poc_state state;
	struct h264sw_sps sps;
	struct h264sw_slice_header header;

	make_sps(&sps, 0U, 4U);
	h264sw_poc_init(&state);

	/* An IDR picture with lsb 0 is at 0. */
	make_header(&header, 1, 3U, 0U, 0U);
	run_picture(&state, &sps, &header, 0, 0);
	CHECK(state.prev_msb == 0);
	CHECK(state.prev_lsb == 0);

	/* The lsb climbs 0, 7, 14: each step is at most 8, so there is no wrap. */
	make_header(&header, 0, 2U, 1U, 7U);
	run_picture(&state, &sps, &header, 7, 7);
	make_header(&header, 0, 2U, 2U, 14U);
	run_picture(&state, &sps, &header, 14, 14);
	CHECK(state.prev_msb == 0);
	CHECK(state.prev_lsb == 14);

	/* 14 to 2: the lsb is below and 12 >= 8 below, so PicOrderCntMsb gains 16 and the count is 18 (8-3). */
	make_header(&header, 0, 2U, 3U, 2U);
	run_picture(&state, &sps, &header, 18, 18);
	CHECK(state.prev_msb == 16);
	CHECK(state.prev_lsb == 2);

	/* 2 to 14: the lsb is above by 12 > 8, so PicOrderCntMsb loses 16 and the count is 14 (8-3). */
	make_header(&header, 0, 2U, 4U, 14U);
	run_picture(&state, &sps, &header, 14, 14);
	CHECK(state.prev_msb == 0);
	CHECK(state.prev_lsb == 14);

	/* A third wrap upward: 14 to 4 gives 16 + 4 = 20. */
	make_header(&header, 0, 2U, 5U, 4U);
	run_picture(&state, &sps, &header, 20, 20);
	CHECK(state.prev_msb == 16);
}

/*
 * Checks the edges of 8-3: a difference of exactly half wraps downward only.
 */
static void
test_type0_half(void)
{
	struct h264sw_poc_state state;
	struct h264sw_sps sps;
	struct h264sw_slice_header header;
	struct h264sw_poc poc;

	make_sps(&sps, 0U, 4U);

	/* The previous lsb is 8 and the new one 0: 8 >= MaxPicOrderCntLsb / 2 wraps, so the count is 16. */
	h264sw_poc_init(&state);
	state.prev_lsb = 8;
	make_header(&header, 0, 1U, 1U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == 16);

	/* The previous lsb is 0 and the new one 8: 8 > 8 is false, so there is no wrap and the count is 8. */
	h264sw_poc_init(&state);
	state.prev_lsb = 0;
	make_header(&header, 0, 1U, 1U, 8U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == 8);

	/* Differences of 7 do not wrap in either direction. */
	h264sw_poc_init(&state);
	state.prev_lsb = 7;
	make_header(&header, 0, 1U, 1U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == 0);
	state.prev_lsb = 0;
	make_header(&header, 0, 1U, 1U, 7U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == 7);

	/* A difference of 9 upward wraps downward: previous 0, new 9 gives -16 + 9 = -7. */
	state.prev_lsb = 0;
	make_header(&header, 0, 1U, 1U, 9U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == -7);
	CHECK(poc.bottom == -7);
	CHECK(poc.poc == -7);

	/* A previous msb is added to the result: msb 32, previous lsb 14, lsb 2 gives 48 + 2. */
	state.prev_msb = 32;
	state.prev_lsb = 14;
	make_header(&header, 0, 1U, 1U, 2U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == 50);
}

/*
 * Checks the bottom count of type 0 and the minimum.
 */
static void
test_type0_bottom(void)
{
	struct h264sw_poc_state state;
	struct h264sw_sps sps;
	struct h264sw_slice_header header;
	struct h264sw_poc poc;

	make_sps(&sps, 0U, 4U);
	h264sw_poc_init(&state);

	/* A positive delta puts the bottom after the top (8-5): 6 and 6 + 1; PicOrderCnt is 6. */
	make_header(&header, 0, 1U, 1U, 6U);
	header.delta_poc_bottom = 1;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == 6);
	CHECK(poc.bottom == 7);
	CHECK(poc.poc == 6);
	CHECK(poc.frame_num_offset == 0);

	/* A negative delta puts the bottom first: 6 and 6 - 3; PicOrderCnt is 3. */
	header.delta_poc_bottom = -3;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == 6);
	CHECK(poc.bottom == 3);
	CHECK(poc.poc == 3);

	/* The delta does not enter prevPicOrderCntLsb: the lsb is the next picture's reference. */
	h264sw_poc_finish(&state, &sps, &header, &poc);
	CHECK(state.prev_lsb == 6);
	CHECK(state.prev_msb == 0);
}

/*
 * Checks that a non-reference picture leaves the previous reference picture alone, and that an IDR picture starts from 0.
 */
static void
test_type0_non_ref_and_idr(void)
{
	struct h264sw_poc_state state;
	struct h264sw_sps sps;
	struct h264sw_slice_header header;

	make_sps(&sps, 0U, 4U);
	h264sw_poc_init(&state);

	/* A reference picture at lsb 10 (msb 0), then a non-reference picture at lsb 12. */
	make_header(&header, 0, 1U, 1U, 4U);
	run_picture(&state, &sps, &header, 4, 4);
	make_header(&header, 0, 1U, 2U, 10U);
	run_picture(&state, &sps, &header, 10, 10);
	make_header(&header, 0, 0U, 3U, 12U);
	run_picture(&state, &sps, &header, 12, 12);

	/* The non-reference picture changed neither the msb nor the lsb, but its frame_num is the previous one now. */
	CHECK(state.prev_msb == 0);
	CHECK(state.prev_lsb == 10);
	CHECK(state.prev_frame_num == 3U);
	CHECK(state.prev_ref_frame_num == 2U);

	/* A non-reference picture that would have wrapped (lsb 2 after 10) does not move the base either. */
	make_header(&header, 0, 0U, 3U, 2U);
	run_picture(&state, &sps, &header, 18, 18);
	CHECK(state.prev_msb == 0);
	CHECK(state.prev_lsb == 10);

	/* The next reference picture uses the lsb 10 of the last reference picture: lsb 14 is 14, not 14 after a wrap. */
	make_header(&header, 0, 1U, 3U, 14U);
	run_picture(&state, &sps, &header, 14, 14);
	CHECK(state.prev_lsb == 14);
	CHECK(state.prev_ref_frame_num == 3U);

	/* Climbs far enough that the msb is non-zero. */
	make_header(&header, 0, 1U, 4U, 5U);
	run_picture(&state, &sps, &header, 21, 21);
	CHECK(state.prev_msb == 16);

	/* An IDR picture takes the previous values as 0, whatever the state holds. */
	make_header(&header, 1, 3U, 0U, 6U);
	run_picture(&state, &sps, &header, 6, 6);
	CHECK(state.prev_msb == 0);
	CHECK(state.prev_lsb == 6);
	CHECK(state.prev_ref_frame_num == 0U);
	CHECK(state.prev_frame_num == 0U);
}

/*
 * Checks type 0 with the largest lsb range and the errors.
 */
static void
test_type0_errors(void)
{
	struct h264sw_poc_state state;
	struct h264sw_sps sps;
	struct h264sw_slice_header header;
	struct h264sw_poc poc;

	h264sw_poc_init(&state);

	/* An lsb of 16 bits wraps at 65536: previous 65000, new 100 gives 65536 + 100. */
	make_sps(&sps, 0U, 16U);
	state.prev_lsb = 65000;
	make_header(&header, 0, 1U, 1U, 100U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == 65636);

	/* An lsb that does not fit its bits is refused. */
	make_header(&header, 0, 1U, 1U, 65536U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);
	make_sps(&sps, 0U, 4U);
	make_header(&header, 0, 1U, 1U, 16U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);
	make_header(&header, 0, 1U, 1U, 0xFFFFFFFFU);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);

	/* A log2_max_pic_order_cnt_lsb outside 4..16 is refused. */
	make_sps(&sps, 0U, 3U);
	make_header(&header, 0, 1U, 1U, 1U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);
	make_sps(&sps, 0U, 17U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);
	make_sps(&sps, 0U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);

	/* PicOrderCntMsb at the top of int32_t: 2^31 - 16 plus a wrap leaves the range. */
	make_sps(&sps, 0U, 4U);
	state.prev_msb = 2147483632;
	state.prev_lsb = 14;
	make_header(&header, 0, 1U, 1U, 2U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);

	/* The same without a wrap stays inside: 2^31 - 16 + 15 = 2^31 - 1. */
	state.prev_lsb = 15;
	make_header(&header, 0, 1U, 1U, 15U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == TEST_INT32_MAX);

	/* A bottom count that leaves int32_t is refused: 5 + (2^31 - 1). */
	state.prev_msb = 0;
	state.prev_lsb = 0;
	make_header(&header, 0, 1U, 1U, 5U);
	header.delta_poc_bottom = TEST_INT32_MAX;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);

	/* The largest and smallest deltas that stay inside are accepted. */
	make_header(&header, 0, 1U, 1U, 0U);
	header.delta_poc_bottom = TEST_INT32_MAX;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.bottom == TEST_INT32_MAX);
	CHECK(poc.poc == 0);
	header.delta_poc_bottom = TEST_INT32_MIN;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.bottom == TEST_INT32_MIN);
	CHECK(poc.poc == TEST_INT32_MIN);

	/* A bottom count below int32_t is refused: the top is -32 + 1 = -31 and -31 + (-2^31) is too small. */
	state.prev_lsb = 4;
	make_header(&header, 0, 1U, 1U, 1U);
	state.prev_msb = -32;
	header.delta_poc_bottom = TEST_INT32_MIN;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);
}

/*
 * Checks the unsupported or damaged arguments that every type shares.
 */
static void
test_common_errors(void)
{
	struct h264sw_poc_state state;
	struct h264sw_sps sps;
	struct h264sw_slice_header header;
	struct h264sw_poc poc;
	struct h264sw_poc untouched;
	unsigned type;

	h264sw_poc_init(&state);

	/* A type above 2 is refused, and poc is not written. */
	make_sps(&sps, 3U, 4U);
	make_header(&header, 0, 1U, 1U, 1U);
	memset(&poc, 0x11, sizeof(poc));
	memset(&untouched, 0x11, sizeof(untouched));
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);
	CHECK(memcmp(&poc, &untouched, sizeof(poc)) == 0);
	make_sps(&sps, 0xFFFFFFFFU, 4U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);

	/* A frame_num that MaxFrameNum cannot hold is refused by every type. */
	for (type = 0U;
	     type < 3U;
	     type++) {
		make_sps(&sps, type, 4U);
		make_header(&header, 0, 1U, 16U, 1U);
		CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);
		make_header(&header, 0, 1U, 0xFFFFFFFFU, 1U);
		CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);
		make_header(&header, 0, 1U, 15U, 1U);
		CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	}

	/* A missing argument is refused. */
	make_sps(&sps, 0U, 4U);
	make_header(&header, 0, 1U, 1U, 1U);
	CHECK(h264sw_poc_compute(NULL, &sps, &header, &poc) == EINVAL);
	CHECK(h264sw_poc_compute(&state, NULL, &header, &poc) == EINVAL);
	CHECK(h264sw_poc_compute(&state, &sps, NULL, &poc) == EINVAL);
	CHECK(h264sw_poc_compute(&state, &sps, &header, NULL) == EINVAL);
}

/*
 * Checks type 1 with a cycle of two reference frames, worked out by hand.
 *
 * offset_for_ref_frame is { 4, 2 } so ExpectedDeltaPerPicOrderCntCycle is
 * 6, offset_for_non_ref_pic is -1 and offset_for_top_to_bottom_field is 1.
 */
static void
test_type1_cycle(void)
{
	static const int32_t offsets[2] = { 4, 2 };
	struct h264sw_poc_state state;
	struct h264sw_sps sps;
	struct h264sw_slice_header header;

	make_sps(&sps, 1U, 4U);
	set_cycle(&sps, 2U, offsets, -1, 1);
	h264sw_poc_init(&state);

	/* IDR, frame_num 0: absFrameNum 0, expected 0; top 0, bottom 0 + 1. */
	make_header(&header, 1, 3U, 0U, 0U);
	run_picture(&state, &sps, &header, 0, 1);

	/* Reference, frame_num 1: absFrameNum 1, cycle 0, position 0; expected 4. */
	make_header(&header, 0, 2U, 1U, 0U);
	run_picture(&state, &sps, &header, 4, 5);

	/* Reference, frame_num 2: absFrameNum 2, cycle 0, position 1; expected 4 + 2 = 6. */
	make_header(&header, 0, 2U, 2U, 0U);
	run_picture(&state, &sps, &header, 6, 7);

	/* Non-reference, frame_num 3: absFrameNum 3 - 1 = 2, position 1; expected 6 - 1 = 5. */
	make_header(&header, 0, 0U, 3U, 0U);
	run_picture(&state, &sps, &header, 5, 6);
	CHECK(state.prev_frame_num == 3U);
	CHECK(state.prev_ref_frame_num == 2U);
	CHECK(state.prev_frame_num_offset == 0);

	/* Reference, frame_num 3: absFrameNum 3, cycle 1, position 0; expected 6 + 4 = 10. */
	make_header(&header, 0, 2U, 3U, 0U);
	run_picture(&state, &sps, &header, 10, 11);

	/* Reference, frame_num 4: absFrameNum 4, cycle 1, position 1; expected 6 + 4 + 2 = 12. */
	make_header(&header, 0, 2U, 4U, 0U);
	run_picture(&state, &sps, &header, 12, 13);

	/* Non-reference, frame_num 5: absFrameNum 4, expected 12 - 1 = 11. */
	make_header(&header, 0, 0U, 5U, 0U);
	run_picture(&state, &sps, &header, 11, 12);

	/* Reference, frame_num 5 with deltas: absFrameNum 5, cycle 2, position 0, expected 12 + 4 = 16; top 16 + 3 = 19, bottom 19 + 1 + (-1) = 19. */
	make_header(&header, 0, 2U, 5U, 0U);
	header.delta_poc[0] = 3;
	header.delta_poc[1] = -1;
	run_picture(&state, &sps, &header, 19, 19);

	/* Reference, frame_num 6: absFrameNum 6, cycle 2, position 1, expected 12 + 4 + 2 = 18; a delta of -4 puts the bottom first: 18 + 1 - 4 = 15. */
	make_header(&header, 0, 2U, 6U, 0U);
	header.delta_poc[0] = 0;
	header.delta_poc[1] = -4;
	run_picture(&state, &sps, &header, 18, 15);
}

/*
 * Checks that frame_num wrapping adds MaxFrameNum to FrameNumOffset (8-6).
 */
static void
test_type1_wrap(void)
{
	static const int32_t offsets[2] = { 4, 2 };
	struct h264sw_poc_state state;
	struct h264sw_sps sps;
	struct h264sw_slice_header header;
	struct h264sw_poc poc;

	make_sps(&sps, 1U, 4U);
	set_cycle(&sps, 2U, offsets, -1, 1);
	h264sw_poc_init(&state);

	/* The previous picture had frame_num 15; frame_num 0 follows: FrameNumOffset 16, absFrameNum 16, cycle 7, position 1: 7 * 6 + 4 + 2 = 48. */
	state.prev_frame_num = 15U;
	make_header(&header, 0, 1U, 0U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.frame_num_offset == 16);
	CHECK(poc.top == 48);
	CHECK(poc.bottom == 49);
	h264sw_poc_finish(&state, &sps, &header, &poc);
	CHECK(state.prev_frame_num == 0U);
	CHECK(state.prev_frame_num_offset == 16);

	/* The next picture, frame_num 1, keeps the offset: absFrameNum 17, cycle 8, position 0: 8 * 6 + 4 = 52. */
	make_header(&header, 0, 1U, 1U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.frame_num_offset == 16);
	CHECK(poc.top == 52);
	h264sw_poc_finish(&state, &sps, &header, &poc);

	/* A second wrap adds MaxFrameNum again: FrameNumOffset 32, absFrameNum 32, cycle 15, position 1: 15 * 6 + 4 + 2 = 96. */
	state.prev_frame_num = 15U;
	make_header(&header, 0, 1U, 0U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.frame_num_offset == 32);
	CHECK(poc.top == 96);
	CHECK(poc.bottom == 97);

	/* An equal frame_num does not wrap (a second field or a non-reference picture repeating the number). */
	state.prev_frame_num = 7U;
	state.prev_frame_num_offset = 16;
	make_header(&header, 0, 0U, 7U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.frame_num_offset == 16);
}

/*
 * Checks type 1 with an empty cycle, a non-reference picture and an IDR picture.
 */
static void
test_type1_empty_cycle(void)
{
	struct h264sw_poc_state state;
	struct h264sw_sps sps;
	struct h264sw_slice_header header;
	struct h264sw_poc poc;

	make_sps(&sps, 1U, 4U);
	set_cycle(&sps, 0U, NULL, -1, 2);
	h264sw_poc_init(&state);

	/* With no cycle absFrameNum is 0 whatever frame_num is: a reference picture is at the delta alone. */
	make_header(&header, 0, 1U, 5U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == 0);
	CHECK(poc.bottom == 2);
	header.delta_poc[0] = 9;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == 9);
	CHECK(poc.bottom == 11);

	/* A non-reference picture adds offset_for_non_ref_pic and does not go below absFrameNum 0. */
	make_header(&header, 0, 0U, 5U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == -1);
	CHECK(poc.bottom == 1);

	/* An IDR picture has FrameNumOffset 0 even when the previous picture left a large one. */
	state.prev_frame_num = 9U;
	state.prev_frame_num_offset = 1600;
	make_header(&header, 1, 3U, 0U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.frame_num_offset == 0);
	CHECK(poc.top == 0);
	CHECK(poc.bottom == 2);
}

/*
 * Checks the cycle of one frame, with an IDR picture in a long cycle.
 */
static void
test_type1_one_frame_cycle(void)
{
	static const int32_t offsets[1] = { 10 };
	struct h264sw_poc_state state;
	struct h264sw_sps sps;
	struct h264sw_slice_header header;

	make_sps(&sps, 1U, 4U);
	set_cycle(&sps, 1U, offsets, -3, 0);
	h264sw_poc_init(&state);

	/* IDR at 0, then each reference frame is 10 further: absFrameNum n gives cycle n - 1, expected 10 * (n - 1) + 10. */
	make_header(&header, 1, 3U, 0U, 0U);
	run_picture(&state, &sps, &header, 0, 0);
	make_header(&header, 0, 1U, 1U, 0U);
	run_picture(&state, &sps, &header, 10, 10);
	make_header(&header, 0, 1U, 2U, 0U);
	run_picture(&state, &sps, &header, 20, 20);

	/* A non-reference picture after frame_num 2 has frame_num 3: absFrameNum 2, expected 20 - 3. */
	make_header(&header, 0, 0U, 3U, 0U);
	run_picture(&state, &sps, &header, 17, 17);
}

/*
 * Checks the errors of type 1: an overflow of the products and sums, and results outside int32_t.
 */
static void
test_type1_errors(void)
{
	static const int32_t offsets[1] = { 1 };
	struct h264sw_poc_state state;
	struct h264sw_sps sps;
	struct h264sw_slice_header header;
	struct h264sw_poc poc;
	struct h264sw_poc untouched;

	make_sps(&sps, 1U, 4U);
	set_cycle(&sps, 1U, offsets, 0, 0);
	h264sw_poc_init(&state);
	memset(&poc, 0x22, sizeof(poc));
	memset(&untouched, 0x22, sizeof(untouched));

	/* picOrderCntCycleCnt * ExpectedDeltaPerPicOrderCntCycle overflows int64_t: 3 * 2^62. */
	sps.expected_delta_per_poc_cycle = (int64_t)1 << 62;
	make_header(&header, 0, 1U, 4U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);
	CHECK(memcmp(&poc, &untouched, sizeof(poc)) == 0);

	/* The same with a negative delta overflows below int64_t. */
	sps.expected_delta_per_poc_cycle = -((int64_t)1 << 62);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);

	/* A product inside int64_t but outside int32_t is refused: 2 * 2^40. */
	sps.expected_delta_per_poc_cycle = (int64_t)1 << 40;
	make_header(&header, 0, 1U, 3U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);

	/* The largest count that fits is accepted: 2 * 1073741823 + 1 = 2^31 - 1. */
	sps.expected_delta_per_poc_cycle = 1073741823;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == 2147483647);

	/* One more is outside. */
	sps.expected_delta_per_poc_cycle = 1073741824;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);

	/* The sum of the offsets of the last cycle overflows int64_t: the product is INT64_MAX, then + 1. */
	sps.expected_delta_per_poc_cycle = INT64_MAX;
	make_header(&header, 0, 1U, 2U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);

	/* A deltas sum that leaves int32_t is refused: 0 + 2^31 - 1 + 1. */
	sps.expected_delta_per_poc_cycle = 0;
	make_header(&header, 0, 1U, 1U, 0U);
	header.delta_poc[0] = TEST_INT32_MAX;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);

	/* A bottom count that leaves int32_t because of offset_for_top_to_bottom_field and the delta is refused. */
	sps.offset_for_top_to_bottom_field = TEST_INT32_MAX;
	header.delta_poc[0] = 0;
	header.delta_poc[1] = 1;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);
	sps.offset_for_top_to_bottom_field = TEST_INT32_MIN;
	header.delta_poc[1] = -2;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);
	sps.offset_for_top_to_bottom_field = 0;
	header.delta_poc[1] = 0;

	/* FrameNumOffset beyond int32_t from a wrap is refused: previous 2^31 - 10 plus 16. */
	state.prev_frame_num = 15U;
	state.prev_frame_num_offset = 2147483637;
	make_header(&header, 0, 1U, 0U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);

	/* FrameNumOffset that overflows int64_t is refused without wrapping. */
	state.prev_frame_num_offset = INT64_MAX;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);

	/* A previous FrameNumOffset already outside the range is refused, even with no wrap. */
	state.prev_frame_num = 0U;
	state.prev_frame_num_offset = (int64_t)1 << 40;
	make_header(&header, 0, 1U, 3U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);
	state.prev_frame_num_offset = -1;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);

	/* A cycle longer than the SPS can hold is refused. */
	state.prev_frame_num_offset = 0;
	sps.poc_cycle_length = H264SW_MAX_POC_CYCLE + 1U;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);
	sps.poc_cycle_length = 0xFFFFFFFFU;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);
}

/*
 * Checks a full-length cycle, where the loop of 8-9 reads the last offset.
 */
static void
test_type1_long_cycle(void)
{
	int32_t offsets[H264SW_MAX_POC_CYCLE];
	struct h264sw_poc_state state;
	struct h264sw_sps sps;
	struct h264sw_slice_header header;
	struct h264sw_poc poc;
	unsigned index;

	/* 255 offsets of 1, so the total is 255 and absFrameNum n is at n for n up to 255. */
	for (index = 0U;
	     index < H264SW_MAX_POC_CYCLE;
	     index++)
		offsets[index] = 1;
	make_sps(&sps, 1U, 4U);
	sps.log2_max_frame_num = 16U;
	sps.max_frame_num = 65536U;
	set_cycle(&sps, H264SW_MAX_POC_CYCLE, offsets, 0, 0);
	h264sw_poc_init(&state);

	/* frame_num 255 is the last position of the first cycle: 255 offsets of 1. */
	make_header(&header, 0, 1U, 255U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == 255);

	/* frame_num 256 starts the second cycle: 255 + 1. */
	make_header(&header, 0, 1U, 256U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == 256);
}

/*
 * Checks type 2: twice the frame count, one less for a non-reference picture, and the wrap.
 */
static void
test_type2(void)
{
	struct h264sw_poc_state state;
	struct h264sw_sps sps;
	struct h264sw_slice_header header;
	struct h264sw_poc poc;

	make_sps(&sps, 2U, 4U);
	h264sw_poc_init(&state);

	/* IDR is 0; the references are 2 * frame_num (8-12). */
	make_header(&header, 1, 3U, 0U, 0U);
	run_picture(&state, &sps, &header, 0, 0);
	make_header(&header, 0, 2U, 1U, 0U);
	run_picture(&state, &sps, &header, 2, 2);
	make_header(&header, 0, 2U, 2U, 0U);
	run_picture(&state, &sps, &header, 4, 4);

	/* A non-reference picture is 2 * frame_num - 1: frame_num 3 gives 5. */
	make_header(&header, 0, 0U, 3U, 0U);
	run_picture(&state, &sps, &header, 5, 5);

	/* The next reference picture has the same frame_num and 2 * 3 = 6. */
	make_header(&header, 0, 2U, 3U, 0U);
	run_picture(&state, &sps, &header, 6, 6);

	/* The fields of type 0 and 1 in the header are not read. */
	make_header(&header, 0, 2U, 4U, 9U);
	header.delta_poc[0] = 100;
	header.delta_poc[1] = 100;
	header.delta_poc_bottom = 100;
	run_picture(&state, &sps, &header, 8, 8);

	/* Climbs to frame_num 15, then wraps: frame_num 0 has FrameNumOffset 16 and 2 * 16 = 32. */
	state.prev_frame_num = 15U;
	state.prev_frame_num_offset = 0;
	make_header(&header, 0, 2U, 0U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.frame_num_offset == 16);
	CHECK(poc.top == 32);
	CHECK(poc.bottom == 32);
	CHECK(poc.poc == 32);
	h264sw_poc_finish(&state, &sps, &header, &poc);

	/* A non-reference picture after the wrap: frame_num 1 gives 2 * 17 - 1 = 33. */
	make_header(&header, 0, 0U, 1U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.frame_num_offset == 16);
	CHECK(poc.top == 33);
	CHECK(poc.bottom == 33);

	/* A non-reference picture with frame_num 0 and no IDR picture before it is at 2 * 0 - 1 = -1. */
	h264sw_poc_init(&state);
	make_header(&header, 0, 0U, 0U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == -1);

	/* An IDR picture is 0 even after a large offset. */
	state.prev_frame_num = 9U;
	state.prev_frame_num_offset = 320;
	make_header(&header, 1, 3U, 0U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.frame_num_offset == 0);
	CHECK(poc.top == 0);
	CHECK(poc.bottom == 0);

	/* An offset beyond int32_t from a wrap is refused. */
	state.prev_frame_num = 15U;
	state.prev_frame_num_offset = 2147483637;
	make_header(&header, 0, 1U, 0U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);

	/* A count whose double leaves int32_t is refused: FrameNumOffset 2^30 and frame_num 0 gives 2^31. */
	state.prev_frame_num = 0U;
	state.prev_frame_num_offset = 1073741824;
	make_header(&header, 0, 1U, 0U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);

	/* 2^30 - 1 is the largest reference count: 2^31 - 2. */
	state.prev_frame_num_offset = 1073741823;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == 2147483646);

	/* An offset that overflows int64_t is refused. */
	state.prev_frame_num = 15U;
	state.prev_frame_num_offset = INT64_MAX;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);
}

/*
 * Checks the reset after a picture with operation 5 for type 0.
 */
static void
test_mmco5_type0(void)
{
	struct h264sw_poc_state state;
	struct h264sw_sps sps;
	struct h264sw_slice_header header;
	struct h264sw_poc poc;

	make_sps(&sps, 0U, 4U);
	h264sw_poc_init(&state);

	/* A picture at 16 + 3 = 19 (bottom 19 + 2), before the reset. */
	state.prev_msb = 16;
	state.prev_lsb = 1;
	make_header(&header, 0, 1U, 4U, 3U);
	header.delta_poc_bottom = 2;
	header.has_mmco5 = 1;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == 19);
	CHECK(poc.bottom == 21);
	CHECK(poc.poc == 19);

	/* After the picture tempPicOrderCnt is 19: top 0, bottom 2, PicOrderCnt 0 (8.2.1). */
	h264sw_poc_finish(&state, &sps, &header, &poc);
	CHECK(poc.top == 0);
	CHECK(poc.bottom == 2);
	CHECK(poc.poc == 0);

	/* The next picture's base is msb 0 and lsb the new top, 0; the frame_num counts restart. */
	CHECK(state.prev_msb == 0);
	CHECK(state.prev_lsb == 0);
	CHECK(state.prev_mmco5 == 1);
	CHECK(state.prev_frame_num == 0U);
	CHECK(state.prev_frame_num_offset == 0);
	CHECK(state.prev_ref_frame_num == 0U);

	/* The next picture, lsb 5, is at 5 rather than 16 + 5. */
	make_header(&header, 0, 1U, 1U, 5U);
	run_picture(&state, &sps, &header, 5, 5);
	CHECK(state.prev_mmco5 == 0);
	CHECK(state.prev_lsb == 5);
	CHECK(state.prev_ref_frame_num == 1U);

	/* When the bottom is the smaller, it is the one that reaches 0: top 10, bottom 10 - 4 = 6. */
	make_header(&header, 0, 1U, 2U, 10U);
	header.delta_poc_bottom = -4;
	header.has_mmco5 = 1;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == 10);
	CHECK(poc.bottom == 6);
	CHECK(poc.poc == 6);
	h264sw_poc_finish(&state, &sps, &header, &poc);
	CHECK(poc.top == 4);
	CHECK(poc.bottom == 0);
	CHECK(poc.poc == 0);

	/* The lsb the next picture compares with is the new TopFieldOrderCnt, 4. */
	CHECK(state.prev_msb == 0);
	CHECK(state.prev_lsb == 4);

	/* A picture with lsb 6 after it is at 6. */
	make_header(&header, 0, 1U, 1U, 6U);
	run_picture(&state, &sps, &header, 6, 6);
}

/*
 * Checks the reset after a picture with operation 5 for types 1 and 2.
 */
static void
test_mmco5_type12(void)
{
	static const int32_t offsets[2] = { 4, 2 };
	struct h264sw_poc_state state;
	struct h264sw_sps sps;
	struct h264sw_slice_header header;
	struct h264sw_poc poc;

	/* Type 1: the previous picture had frame_num 6 and FrameNumOffset 16, so frame_num 7 has absFrameNum 23, cycle 11, position 0: 66 + 4 = 70. */
	make_sps(&sps, 1U, 4U);
	set_cycle(&sps, 2U, offsets, -1, 1);
	h264sw_poc_init(&state);
	state.prev_frame_num = 6U;
	state.prev_frame_num_offset = 16;
	make_header(&header, 0, 1U, 7U, 0U);
	header.has_mmco5 = 1;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.frame_num_offset == 16);
	CHECK(poc.top == 70);
	CHECK(poc.bottom == 71);
	h264sw_poc_finish(&state, &sps, &header, &poc);
	CHECK(poc.top == 0);
	CHECK(poc.bottom == 1);
	CHECK(poc.poc == 0);
	CHECK(state.prev_mmco5 == 1);
	CHECK(state.prev_frame_num == 0U);
	CHECK(state.prev_frame_num_offset == 0);
	CHECK(state.prev_ref_frame_num == 0U);

	/* The next reference picture, frame_num 1, counts from FrameNumOffset 0: absFrameNum 1, expected 4. */
	make_header(&header, 0, 1U, 1U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.frame_num_offset == 0);
	CHECK(poc.top == 4);
	CHECK(poc.bottom == 5);

	/* The flag alone makes prevFrameNumOffset 0, whatever the state holds. */
	state.prev_frame_num = 0U;
	state.prev_frame_num_offset = 99;
	state.prev_mmco5 = 1;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.frame_num_offset == 0);
	CHECK(poc.top == 4);

	/* Type 2: frame_num 9 after 8 with offset 16 is at 2 * 25 = 50; after the reset the next frame_num 1 is at 2. */
	make_sps(&sps, 2U, 4U);
	h264sw_poc_init(&state);
	state.prev_frame_num = 8U;
	state.prev_frame_num_offset = 16;
	make_header(&header, 0, 1U, 9U, 0U);
	header.has_mmco5 = 1;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == 50);
	h264sw_poc_finish(&state, &sps, &header, &poc);
	CHECK(poc.top == 0);
	CHECK(poc.bottom == 0);
	CHECK(poc.poc == 0);
	make_header(&header, 0, 1U, 1U, 0U);
	run_picture(&state, &sps, &header, 2, 2);

	/* The reset does not touch the type 0 pair of a type 2 stream. */
	h264sw_poc_init(&state);
	state.prev_msb = 3;
	state.prev_lsb = 4;
	make_header(&header, 0, 1U, 2U, 0U);
	header.has_mmco5 = 1;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	h264sw_poc_finish(&state, &sps, &header, &poc);
	CHECK(state.prev_msb == 3);
	CHECK(state.prev_lsb == 4);
}

/*
 * Checks the range of the counts after operation 5, and finish with values that would overflow.
 */
static void
test_mmco5_range(void)
{
	struct h264sw_poc_state state;
	struct h264sw_sps sps;
	struct h264sw_slice_header header;
	struct h264sw_poc poc;

	make_sps(&sps, 0U, 16U);
	h264sw_poc_init(&state);

	/* The top 100 and the bottom 100 - 2^31: both fit, and without operation 5 that is all that is asked. */
	make_header(&header, 0, 1U, 1U, 100U);
	header.delta_poc_bottom = TEST_INT32_MIN;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == 100);
	CHECK(poc.bottom == 100 - TEST_INT32_MAX - 1);

	/* With operation 5 the top would become 2^31, which int32_t cannot hold. */
	header.has_mmco5 = 1;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == EINVAL);

	/* One less fits: the top becomes 2^31 - 1. */
	header.delta_poc_bottom = TEST_INT32_MIN + 1;
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	h264sw_poc_finish(&state, &sps, &header, &poc);
	CHECK(poc.top == TEST_INT32_MAX);
	CHECK(poc.bottom == 0);
	CHECK(poc.poc == 0);
	CHECK(state.prev_lsb == TEST_INT32_MAX);

	/* Finish given counts that would overflow the subtraction clamps instead of wrapping. */
	memset(&poc, 0, sizeof(poc));
	poc.top = TEST_INT32_MAX;
	poc.bottom = TEST_INT32_MIN;
	poc.poc = TEST_INT32_MIN;
	h264sw_poc_finish(&state, &sps, &header, &poc);
	CHECK(poc.top == TEST_INT32_MAX);
	CHECK(poc.bottom == 0);
	CHECK(poc.poc == 0);
}

/*
 * Checks the state a picture leaves for type 0 after an IDR picture and a reference picture, and for the others.
 */
static void
test_finish_state(void)
{
	struct h264sw_poc_state state;
	struct h264sw_sps sps;
	struct h264sw_slice_header header;
	struct h264sw_poc poc;

	/* A reference picture of type 0 keeps PicOrderCntMsb and the lsb, and its frame_num. */
	make_sps(&sps, 0U, 4U);
	h264sw_poc_init(&state);
	state.prev_msb = 16;
	state.prev_lsb = 1;
	make_header(&header, 0, 2U, 6U, 3U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	h264sw_poc_finish(&state, &sps, &header, &poc);
	CHECK(state.prev_msb == 16);
	CHECK(state.prev_lsb == 3);
	CHECK(state.prev_frame_num == 6U);
	CHECK(state.prev_ref_frame_num == 6U);
	CHECK(state.prev_mmco5 == 0);
	CHECK(state.prev_frame_num_offset == 0);

	/* A negative msb is kept as it is. */
	state.prev_msb = 0;
	state.prev_lsb = 0;
	make_header(&header, 0, 2U, 7U, 9U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.top == -7);
	h264sw_poc_finish(&state, &sps, &header, &poc);
	CHECK(state.prev_msb == -16);
	CHECK(state.prev_lsb == 9);

	/* A non-reference picture of type 1 records its frame_num and FrameNumOffset only. */
	make_sps(&sps, 1U, 4U);
	h264sw_poc_init(&state);
	state.prev_msb = 5;
	state.prev_lsb = 6;
	state.prev_ref_frame_num = 2U;
	state.prev_frame_num = 15U;
	make_header(&header, 0, 0U, 1U, 0U);
	CHECK(h264sw_poc_compute(&state, &sps, &header, &poc) == 0);
	CHECK(poc.frame_num_offset == 16);
	h264sw_poc_finish(&state, &sps, &header, &poc);
	CHECK(state.prev_frame_num == 1U);
	CHECK(state.prev_frame_num_offset == 16);
	CHECK(state.prev_ref_frame_num == 2U);
	CHECK(state.prev_msb == 5);
	CHECK(state.prev_lsb == 6);
}

/*
 * Checks that the functions do not fail on arbitrary headers and SPS values.
 */
static void
test_random(void)
{
	struct h264sw_poc_state state;
	struct h264sw_sps sps;
	struct h264sw_slice_header header;
	struct h264sw_poc poc;
	unsigned round;
	unsigned index;
	unsigned long seed;
	uint32_t word;
	int error;
	int errors;

	/* A generator of values is enough; the checks are that nothing faults and that only 0 or EINVAL is returned. */
	seed = 12345UL;
	errors = 0;
	for (round = 0U;
	     round < 20000U;
	     round++) {
		memset(&sps, 0, sizeof(sps));
		memset(&header, 0, sizeof(header));

		/* Draws each field from a few interesting values and a few wild ones. */
		seed = (seed * 1103515245UL + 12345UL) & 0xFFFFFFFFUL;
		word = (uint32_t)(seed >> 3);
		sps.poc_type = word % 4U;
		sps.log2_max_poc_lsb = 3U + (word >> 4) % 15U;
		sps.log2_max_frame_num = 4U + (word >> 8) % 13U;
		sps.max_frame_num = (uint32_t)1 << sps.log2_max_frame_num;
		sps.poc_cycle_length = (word >> 12) % 4U;
		if ((word & 0x80000000UL) != 0UL)
			sps.poc_cycle_length = 255U + (word >> 12) % 3U;
		for (index = 0U;
		     index < H264SW_MAX_POC_CYCLE;
		     index++) {
			seed = (seed * 1103515245UL + 12345UL) & 0xFFFFFFFFUL;
			sps.offset_for_ref_frame[index] = (int32_t)(uint32_t)seed;
			if ((seed & 3UL) != 0UL)
				sps.offset_for_ref_frame[index] = (int32_t)(seed & 0xFFUL) - 100;
		}
		seed = (seed * 1103515245UL + 12345UL) & 0xFFFFFFFFUL;
		sps.expected_delta_per_poc_cycle = (int64_t)(int32_t)(uint32_t)seed * (int64_t)(int32_t)(uint32_t)(seed >> 7);
		sps.offset_for_non_ref_pic = (int32_t)(uint32_t)seed;
		sps.offset_for_top_to_bottom_field = (int32_t)(uint32_t)(seed >> 5);

		seed = (seed * 1103515245UL + 12345UL) & 0xFFFFFFFFUL;
		header.idr = (int)((seed >> 4) & 1UL);
		header.nal_ref_idc = (unsigned)((seed >> 6) & 3UL);
		header.frame_num = (uint32_t)(seed >> 9) % (sps.max_frame_num + 2U);
		header.poc_lsb = (uint32_t)(seed >> 3) % ((uint32_t)1 << (sps.log2_max_poc_lsb + 1U));
		header.has_mmco5 = (int)((seed >> 5) & 1UL);
		seed = (seed * 1103515245UL + 12345UL) & 0xFFFFFFFFUL;
		header.delta_poc_bottom = (int32_t)(uint32_t)seed;
		header.delta_poc[0] = (int32_t)(uint32_t)(seed >> 3);
		header.delta_poc[1] = (int32_t)(uint32_t)(seed >> 6);

		seed = (seed * 1103515245UL + 12345UL) & 0xFFFFFFFFUL;
		state.prev_msb = (int32_t)(uint32_t)seed;
		state.prev_lsb = (int32_t)(uint32_t)(seed >> 8);
		state.prev_frame_num = (uint32_t)seed;
		state.prev_frame_num_offset = (int64_t)(int32_t)(uint32_t)seed * (int64_t)(seed & 0xFFFFUL);
		state.prev_mmco5 = (int)((seed >> 2) & 1UL);
		state.prev_ref_frame_num = (uint32_t)(seed >> 11);

		/* A derivation either succeeds or says EINVAL; a success can be carried on. */
		memset(&poc, 0, sizeof(poc));
		error = h264sw_poc_compute(&state, &sps, &header, &poc);
		if (error != 0) {
			if (error != EINVAL)
				errors++;
			continue;
		}
		if (poc.poc > poc.top ||
		    poc.poc > poc.bottom)
			errors++;
		h264sw_poc_finish(&state, &sps, &header, &poc);
	}
	CHECK(errors == 0);
}

/*
 * Runs every check and prints the summary line.
 */
int
main(void)
{
	test_init();
	test_type0_wrap();
	test_type0_half();
	test_type0_bottom();
	test_type0_non_ref_and_idr();
	test_type0_errors();
	test_common_errors();
	test_type1_cycle();
	test_type1_wrap();
	test_type1_empty_cycle();
	test_type1_one_frame_cycle();
	test_type1_errors();
	test_type1_long_cycle();
	test_type2();
	test_mmco5_type0();
	test_mmco5_type12();
	test_mmco5_range();
	test_finish_state();
	test_random();

	/* Reports the checks. */
	return check_finish("poc");
}
