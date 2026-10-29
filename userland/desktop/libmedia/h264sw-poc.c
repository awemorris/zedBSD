/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The picture order count of the software H.264 decoder (8.2.1).
 */

#include "h264sw-poc.h"

#include <errno.h>
#include <stddef.h>

/* The range an order count, PicOrderCntMsb and FrameNumOffset must stay in (8.2.1). */
#define POC_VALUE_MIN (-2147483647 - 1)
#define POC_VALUE_MAX 2147483647

/* The range log2_max_pic_order_cnt_lsb_minus4 + 4 may have (7.4.2.1.1). */
#define POC_LOG2_LSB_MIN 4U
#define POC_LOG2_LSB_MAX 16U

/* Static function declarations. */
static int poc_add(int64_t first, int64_t second, int64_t *sum);
static int poc_multiply(int64_t first, int64_t second, int64_t *product);
static int poc_fits(int64_t value);
static int32_t poc_clamp(int64_t value);
static int poc_frame_num_offset(const struct h264sw_poc_state *state, const struct h264sw_sps *sps, const struct h264sw_slice_header *header, int64_t *offset);
static int poc_type0(const struct h264sw_poc_state *state, const struct h264sw_sps *sps, const struct h264sw_slice_header *header, int64_t *top, int64_t *bottom);
static int poc_type1(const struct h264sw_poc_state *state, const struct h264sw_sps *sps, const struct h264sw_slice_header *header, int64_t *offset, int64_t *top, int64_t *bottom);
static int poc_type2(const struct h264sw_poc_state *state, const struct h264sw_sps *sps, const struct h264sw_slice_header *header, int64_t *offset, int64_t *top, int64_t *bottom);

/*
 * Clears the state as before the first picture of a stream.
 *
 * Every count starts at 0, as the clauses of 8.2.1 take them for an IDR
 * picture, and no previous picture had operation 5.
 */
void
h264sw_poc_init(
	struct h264sw_poc_state *state)
{
	/* Empties every count the next picture may read. */
	state->prev_msb = 0;
	state->prev_lsb = 0;
	state->prev_frame_num = 0U;
	state->prev_frame_num_offset = 0;
	state->prev_mmco5 = 0;
	state->prev_ref_frame_num = 0U;
}

/*
 * Derives the order counts of a frame from its first slice's header (8.2.1).
 *
 * Nothing is written to poc unless every value is in range.
 */
int
h264sw_poc_compute(
	const struct h264sw_poc_state *state,
	const struct h264sw_sps *sps,
	const struct h264sw_slice_header *header,
	struct h264sw_poc *poc)
{
	int64_t top;
	int64_t bottom;
	int64_t minimum;
	int64_t offset;
	int64_t reset_top;
	int64_t reset_bottom;
	int error;

	/* Refuses a missing argument. */
	if (state == NULL ||
	    sps == NULL ||
	    header == NULL ||
	    poc == NULL)
		return EINVAL;

	/* A frame_num that MaxFrameNum cannot hold is not a value of the syntax (7.4.3). */
	if (header->frame_num >= sps->max_frame_num)
		return EINVAL;

	/* Derives the counts with the type the sequence uses. */
	offset = 0;
	top = 0;
	bottom = 0;
	error = EINVAL;
	if (sps->poc_type == 0U) {
		error = poc_type0(state, sps, header, &top, &bottom);
	} else if (sps->poc_type == 1U) {
		error = poc_type1(state, sps, header, &offset, &top, &bottom);
	} else if (sps->poc_type == 2U) {
		error = poc_type2(state, sps, header, &offset, &top, &bottom);
	}

	/* Reports a stream whose values leave the range of 8.2.1. */
	if (error != 0)
		return error;

	/* Both counts of the frame must fit the range the decoding process allows (8.2.1). */
	if (!poc_fits(top))
		return EINVAL;
	if (!poc_fits(bottom))
		return EINVAL;

	/* PicOrderCnt of a frame is the smaller of its two counts (8-1). */
	minimum = top;
	if (bottom < minimum)
		minimum = bottom;

	/*
	 * A picture with operation 5 has the minimum taken off both counts
	 * after it is decoded; that result must fit as well, or the
	 * decoder could not keep it.
	 */
	if (header->has_mmco5) {
		reset_top = top - minimum;
		reset_bottom = bottom - minimum;
		if (reset_top > POC_VALUE_MAX)
			return EINVAL;
		if (reset_bottom > POC_VALUE_MAX)
			return EINVAL;
	}

	/* Hands the counts to the caller. */
	poc->top = (int32_t)top;
	poc->bottom = (int32_t)bottom;
	poc->poc = (int32_t)minimum;
	poc->frame_num_offset = offset;

	/* Succeeded: the order counts of the frame are derived. */
	return 0;
}

/*
 * Carries the state past a decoded picture, and resets its counts when it had operation 5 (8.2.1).
 *
 * The counts the next picture reads are written here, once the picture
 * is done, so a picture that fails to decode leaves the state alone.
 */
void
h264sw_poc_finish(
	struct h264sw_poc_state *state,
	const struct h264sw_sps *sps,
	const struct h264sw_slice_header *header,
	struct h264sw_poc *poc)
{
	int64_t temp_poc;
	int64_t msb;
	int64_t lsb;

	/* A picture with operation 5 restarts the counts from itself (8.2.1). */
	if (header->has_mmco5) {
		/* tempPicOrderCnt is PicOrderCnt of the picture; both counts lose it. */
		temp_poc = (int64_t)poc->poc;
		poc->top = poc_clamp((int64_t)poc->top - temp_poc);
		poc->bottom = poc_clamp((int64_t)poc->bottom - temp_poc);
		poc->poc = poc->top;
		if (poc->bottom < poc->poc)
			poc->poc = poc->bottom;

		/* The picture counts as frame_num 0 and FrameNumOffset 0 for the next one (7.4.3, 8.2.1.2). */
		state->prev_mmco5 = 1;
		state->prev_frame_num = 0U;
		state->prev_frame_num_offset = 0;
		state->prev_ref_frame_num = 0U;

		/* Type 0 continues from the reset TopFieldOrderCnt with no msb (8.2.1.1). */
		if (sps->poc_type == 0U) {
			state->prev_msb = 0;
			state->prev_lsb = poc->top;
		}

		return;
	}

	/* The next picture of types 1 and 2 reads this picture's frame_num and FrameNumOffset. */
	state->prev_mmco5 = 0;
	state->prev_frame_num = header->frame_num;
	state->prev_frame_num_offset = poc->frame_num_offset;

	/* Only a reference picture moves the previous reference picture. */
	if (header->nal_ref_idc == 0U)
		return;

	/* PrevRefFrameNum is the frame_num of the last reference picture (7.4.3). */
	state->prev_ref_frame_num = header->frame_num;

	/* Type 0 keeps PicOrderCntMsb (the top count less pic_order_cnt_lsb) and the lsb (8.2.1.1). */
	if (sps->poc_type == 0U) {
		lsb = (int64_t)header->poc_lsb;
		msb = (int64_t)poc->top - lsb;
		state->prev_msb = poc_clamp(msb);
		state->prev_lsb = poc_clamp(lsb);
	}
}

/*
 * Adds two values, refusing a sum that int64_t cannot hold.
 *
 * Returns 0 and *sum, or EINVAL.
 */
static int
poc_add(
	int64_t first,
	int64_t second,
	int64_t *sum)
{
	/* A positive second value overflows when first is above what is left below the maximum. */
	if (second > 0 &&
	    first > INT64_MAX - second)
		return EINVAL;

	/* A negative second value overflows when first is below what is left above the minimum. */
	if (second < 0 &&
	    first < INT64_MIN - second)
		return EINVAL;

	/* The sum cannot overflow now. */
	*sum = first + second;

	/* Succeeded: the sum fits. */
	return 0;
}

/*
 * Multiplies two values, refusing a product that int64_t cannot hold.
 *
 * Returns 0 and *product, or EINVAL.
 */
static int
poc_multiply(
	int64_t first,
	int64_t second,
	int64_t *product)
{
	/* A zero factor gives zero and cannot overflow. */
	if (first == 0 ||
	    second == 0) {
		*product = 0;
		return 0;
	}

	/* Two positive factors overflow when first is above the maximum divided by second. */
	if (first > 0 &&
	    second > 0 &&
	    first > INT64_MAX / second)
		return EINVAL;

	/* A positive and a negative factor overflow when the negative one is below the minimum divided by the positive one. */
	if (first > 0 &&
	    second < 0 &&
	    second < INT64_MIN / first)
		return EINVAL;
	if (first < 0 &&
	    second > 0 &&
	    first < INT64_MIN / second)
		return EINVAL;

	/* Two negative factors overflow when first is below the maximum divided by second. */
	if (first < 0 &&
	    second < 0 &&
	    first < INT64_MAX / second)
		return EINVAL;

	/* The product cannot overflow now. */
	*product = first * second;

	/* Succeeded: the product fits. */
	return 0;
}

/*
 * Tells whether a value is inside the range of int32_t, the range 8.2.1 allows the counts.
 *
 * Returns 1 when it is, 0 when it is not.
 */
static int
poc_fits(
	int64_t value)
{
	/* Below the range. */
	if (value < (int64_t)POC_VALUE_MIN)
		return 0;

	/* Above the range. */
	if (value > (int64_t)POC_VALUE_MAX)
		return 0;

	/* The value is inside the range. */
	return 1;
}

/*
 * Narrows a value to int32_t by clamping it to the range of int32_t.
 */
static int32_t
poc_clamp(
	int64_t value)
{
	/* Below the range gives the smallest value. */
	if (value < (int64_t)POC_VALUE_MIN)
		return POC_VALUE_MIN;

	/* Above the range gives the largest value. */
	if (value > (int64_t)POC_VALUE_MAX)
		return POC_VALUE_MAX;

	/* In range: the value is kept. */
	return (int32_t)value;
}

/*
 * Derives FrameNumOffset of types 1 and 2 (8-6, 8-11).
 *
 * Returns 0 and *offset, or EINVAL when the offset leaves 0..2^31 - 1.
 */
static int
poc_frame_num_offset(
	const struct h264sw_poc_state *state,
	const struct h264sw_sps *sps,
	const struct h264sw_slice_header *header,
	int64_t *offset)
{
	int64_t previous;
	int64_t result;
	int error;

	/* An IDR picture starts the offset again. */
	if (header->idr) {
		*offset = 0;
		return 0;
	}

	/* prevFrameNumOffset is 0 after a picture with operation 5. */
	previous = state->prev_frame_num_offset;
	if (state->prev_mmco5)
		previous = 0;

	/* A frame_num below the previous one means frame_num wrapped past MaxFrameNum. */
	result = previous;
	if (state->prev_frame_num > header->frame_num) {
		error = poc_add(previous, (int64_t)sps->max_frame_num, &result);
		if (error != 0)
			return error;
	}

	/* The offset is a count of wraps and stays in 0..2^31 - 1 (8.2.1). */
	if (result < 0)
		return EINVAL;
	if (result > (int64_t)POC_VALUE_MAX)
		return EINVAL;

	/* Succeeded: the offset is derived. */
	*offset = result;
	return 0;
}

/*
 * Derives the counts of type 0 (8.2.1.1, 8-3, 8-4, 8-5).
 *
 * Returns 0 with *top and *bottom, or EINVAL.
 */
static int
poc_type0(
	const struct h264sw_poc_state *state,
	const struct h264sw_sps *sps,
	const struct h264sw_slice_header *header,
	int64_t *top,
	int64_t *bottom)
{
	int64_t max_lsb;
	int64_t lsb;
	int64_t prev_msb;
	int64_t prev_lsb;
	int64_t msb;
	int64_t sum;
	int error;

	/* MaxPicOrderCntLsb is 2 to the power log2_max_pic_order_cnt_lsb (7-11). */
	if (sps->log2_max_poc_lsb < POC_LOG2_LSB_MIN)
		return EINVAL;
	if (sps->log2_max_poc_lsb > POC_LOG2_LSB_MAX)
		return EINVAL;
	max_lsb = (int64_t)1 << sps->log2_max_poc_lsb;

	/* pic_order_cnt_lsb is a u(v) of that many bits, so it is below MaxPicOrderCntLsb. */
	if ((int64_t)header->poc_lsb >= max_lsb)
		return EINVAL;
	lsb = (int64_t)header->poc_lsb;

	/* An IDR picture takes the previous values as 0; the others take those of the previous reference picture. */
	prev_msb = 0;
	prev_lsb = 0;
	if (!header->idr) {
		prev_msb = (int64_t)state->prev_msb;
		prev_lsb = (int64_t)state->prev_lsb;
	}

	/* Finds PicOrderCntMsb by the nearest wrap of the lsb (8-3). */
	msb = prev_msb;
	if (lsb < prev_lsb &&
	    (prev_lsb - lsb) >= (max_lsb / 2)) {
		/* The lsb wrapped upward past the maximum. */
		msb = prev_msb + max_lsb;
	} else if (lsb > prev_lsb &&
		   (lsb - prev_lsb) > (max_lsb / 2)) {
		/* The lsb stepped back across the wrap. */
		msb = prev_msb - max_lsb;
	}

	/* PicOrderCntMsb is a count the range of 8.2.1 limits. */
	if (!poc_fits(msb))
		return EINVAL;

	/* TopFieldOrderCnt is the msb plus the lsb (8-4). */
	*top = msb + lsb;

	/* BottomFieldOrderCnt follows by delta_pic_order_cnt_bottom (8-5). */
	error = poc_add(*top, (int64_t)header->delta_poc_bottom, &sum);
	if (error != 0)
		return error;
	*bottom = sum;

	/* Succeeded: the counts of type 0 are derived. */
	return 0;
}

/*
 * Derives the counts of type 1 (8.2.1.2, 8-6 to 8-10).
 *
 * Returns 0 with *offset (FrameNumOffset), *top and *bottom, or EINVAL.
 */
static int
poc_type1(
	const struct h264sw_poc_state *state,
	const struct h264sw_sps *sps,
	const struct h264sw_slice_header *header,
	int64_t *offset,
	int64_t *top,
	int64_t *bottom)
{
	int64_t frame_offset;
	int64_t abs_frame_num;
	int64_t cycle_count;
	int64_t frame_in_cycle;
	int64_t expected;
	int64_t cycle_total;
	int64_t sum;
	unsigned cycle_length;
	unsigned index;
	int error;

	/* The cycle holds at most as many offsets as the SPS keeps. */
	cycle_length = sps->poc_cycle_length;
	if (cycle_length > H264SW_MAX_POC_CYCLE)
		return EINVAL;

	/* Derives FrameNumOffset (8-6). */
	error = poc_frame_num_offset(state, sps, header, &frame_offset);
	if (error != 0)
		return error;

	/* absFrameNum counts the frames of the cycle, and is 0 when the cycle is empty (8-7). */
	abs_frame_num = 0;
	if (cycle_length != 0U) {
		error = poc_add(frame_offset, (int64_t)header->frame_num, &abs_frame_num);
		if (error != 0)
			return error;
	}

	/* A non-reference picture sits one frame before the next reference frame (8-7). */
	if (header->nal_ref_idc == 0U &&
	    abs_frame_num > 0)
		abs_frame_num = abs_frame_num - 1;

	/* expectedPicOrderCnt is 0 before the first frame of the cycle (8-9). */
	expected = 0;
	if (abs_frame_num > 0) {
		/* Splits the frames into whole cycles and a position in the last one (8-8). */
		cycle_count = (abs_frame_num - 1) / (int64_t)cycle_length;
		frame_in_cycle = (abs_frame_num - 1) % (int64_t)cycle_length;

		/* The whole cycles add ExpectedDeltaPerPicOrderCntCycle each. */
		cycle_total = sps->expected_delta_per_poc_cycle;
		error = poc_multiply(cycle_count, cycle_total, &expected);
		if (error != 0)
			return error;

		/* The frames of the last cycle add their offsets. */
		for (index = 0U;
		     (int64_t)index <= frame_in_cycle;
		     index++) {
			error = poc_add(expected, (int64_t)sps->offset_for_ref_frame[index], &sum);
			if (error != 0)
				return error;
			expected = sum;
		}
	}

	/* A non-reference picture adds offset_for_non_ref_pic (8-9). */
	if (header->nal_ref_idc == 0U) {
		error = poc_add(expected, (int64_t)sps->offset_for_non_ref_pic, &sum);
		if (error != 0)
			return error;
		expected = sum;
	}

	/* TopFieldOrderCnt adds delta_pic_order_cnt[ 0 ] (8-10). */
	error = poc_add(expected, (int64_t)header->delta_poc[0], &sum);
	if (error != 0)
		return error;
	*top = sum;

	/* BottomFieldOrderCnt adds offset_for_top_to_bottom_field and delta_pic_order_cnt[ 1 ] (8-10). */
	error = poc_add(*top, (int64_t)sps->offset_for_top_to_bottom_field, &sum);
	if (error != 0)
		return error;
	error = poc_add(sum, (int64_t)header->delta_poc[1], &sum);
	if (error != 0)
		return error;
	*bottom = sum;

	/* Succeeded: the counts of type 1 are derived. */
	*offset = frame_offset;
	return 0;
}

/*
 * Derives the counts of type 2 (8.2.1.3, 8-11 to 8-13).
 *
 * Returns 0 with *offset (FrameNumOffset), *top and *bottom, or EINVAL.
 */
static int
poc_type2(
	const struct h264sw_poc_state *state,
	const struct h264sw_sps *sps,
	const struct h264sw_slice_header *header,
	int64_t *offset,
	int64_t *top,
	int64_t *bottom)
{
	int64_t frame_offset;
	int64_t temp_poc;
	int64_t sum;
	int error;

	/* Derives FrameNumOffset (8-11). */
	error = poc_frame_num_offset(state, sps, header, &frame_offset);
	if (error != 0)
		return error;

	/* tempPicOrderCnt is 0 for an IDR picture, else twice the frame count, less 1 for a non-reference picture (8-12). */
	temp_poc = 0;
	if (!header->idr) {
		error = poc_add(frame_offset, (int64_t)header->frame_num, &sum);
		if (error != 0)
			return error;
		temp_poc = sum * 2;
		if (header->nal_ref_idc == 0U)
			temp_poc = temp_poc - 1;
	}

	/* Both counts of a frame are tempPicOrderCnt (8-13). */
	*top = temp_poc;
	*bottom = temp_poc;

	/* Succeeded: the counts of type 2 are derived. */
	*offset = frame_offset;
	return 0;
}
