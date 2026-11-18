/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The picture order count of H.265 and the order counts of a reference
 * picture set (8.3.1, 8.3.2).
 */

#include "h265sw-poc.h"

#include <errno.h>
#include <string.h>

/* The range of int32_t as int64_t bounds, for the checked sums of order counts. */
#define POC_MAX ((int64_t)2147483647)
#define POC_MIN (-(int64_t)2147483647 - 1)

/* The most entries of a short-term or long-term set the arrays of the structures hold. */
#define POC_MAX_ENTRIES (H265SW_MAX_RPS - 1U)

/* The range of log2_max_pic_order_cnt_lsb (7.4.3.2.1: 4..16). */
#define POC_MIN_LOG2_LSB 4U
#define POC_MAX_LOG2_LSB 16U

/* Static function declarations. */
static int64_t poc_floor_div(int64_t numerator, int64_t denominator);
static int poc_push(int32_t *list, unsigned *count, int64_t value);
static int poc_rps_short_term(const struct h265sw_slice_header *header, int32_t poc, struct h265sw_rps_pocs *out);
static int poc_rps_long_term(const struct h265sw_slice_header *header, int32_t poc, unsigned log2_max_poc_lsb, struct h265sw_rps_pocs *out);

/*
 * Resets the state (the start of a stream, or after a reset of the
 * decoding).
 *
 * Both values are 0, which is what the first picture's derivation expects
 * when it is not an IRAP picture (a stream should start with one).
 */
void
h265sw_poc_init(
	struct h265sw_poc_state *state)
{
	/* A missing state has nothing to reset. */
	if (state == NULL)
		return;

	/* Clears prevTid0Pic's two values. */
	state->prev_lsb = 0U;
	state->prev_msb = 0;
}

/*
 * Derives PicOrderCntVal of a picture (8.3.1, 8-1 and 8-2).
 *
 * An IDR picture has NoRaslOutputFlag 1 whatever the caller says, so its
 * PicOrderCntMsb is 0 and its order count is its lsb, which is 0.
 */
int
h265sw_poc_compute(
	const struct h265sw_poc_state *state,
	const struct h265sw_slice_header *header,
	int irap_no_rasl,
	unsigned log2_max_poc_lsb,
	int32_t *poc)
{
	int64_t max_lsb;
	int64_t lsb;
	int64_t prev_lsb;
	int64_t msb;
	int64_t value;
	int idr;

	/* Refuses arguments no valid caller passes. */
	if (state == NULL || header == NULL || poc == NULL)
		return EINVAL;
	if (log2_max_poc_lsb < POC_MIN_LOG2_LSB || log2_max_poc_lsb > POC_MAX_LOG2_LSB)
		return EINVAL;

	/* Takes the values the equations use as wide integers. */
	max_lsb = (int64_t)1 << log2_max_poc_lsb;
	lsb = (int64_t)header->poc_lsb;
	prev_lsb = (int64_t)state->prev_lsb;
	idr = h265sw_nal_is_idr(header->nal_type);

	/* Derives PicOrderCntMsb: 0 for an IRAP picture that starts the decoding, else the nearest wrap of prevTid0Pic's (8-1). */
	if (irap_no_rasl != 0 || idr != 0) {
		msb = 0;
	} else if (lsb < prev_lsb && prev_lsb - lsb >= max_lsb / 2) {
		msb = (int64_t)state->prev_msb + max_lsb;
	} else if (lsb > prev_lsb && lsb - prev_lsb > max_lsb / 2) {
		msb = (int64_t)state->prev_msb - max_lsb;
	} else {
		msb = (int64_t)state->prev_msb;
	}

	/* Adds the lsb (8-2); the order count must fit int32_t. */
	value = msb + lsb;
	if (value < POC_MIN || value > POC_MAX)
		return EINVAL;
	*poc = (int32_t)value;

	/* Succeeded: the picture order count. */
	return 0;
}

/*
 * Records a decoded picture as prevTid0Pic when it qualifies (TemporalId
 * 0 and not RASL, RADL or SLNR, by its nal_type): prev_lsb its
 * slice_pic_order_cnt_lsb, prev_msb poc - lsb.
 */
void
h265sw_poc_finish(
	struct h265sw_poc_state *state,
	const struct h265sw_slice_header *header,
	int32_t poc)
{
	int64_t msb;
	int rasl;
	int radl;
	int slnr;

	/* A missing argument has nothing to record. */
	if (state == NULL || header == NULL)
		return;

	/* A picture of a higher sub-layer is never prevTid0Pic. */
	if (header->temporal_id != 0U)
		return;

	/* Leading pictures and sub-layer non-reference pictures are never prevTid0Pic. */
	rasl = h265sw_nal_is_rasl(header->nal_type);
	radl = h265sw_nal_is_radl(header->nal_type);
	slnr = h265sw_nal_is_slnr(header->nal_type);
	if (rasl != 0 || radl != 0 || slnr != 0)
		return;

	/*
	 * PicOrderCntMsb of the picture, clamped to what the state holds.  An
	 * order count that h265sw_poc_compute made has a Msb that fits, being a
	 * multiple of MaxPicOrderCntLsb inside the range of the count; only a
	 * hand-made count can leave it.
	 */
	msb = (int64_t)poc - (int64_t)header->poc_lsb;
	if (msb < POC_MIN)
		msb = POC_MIN;
	if (msb > POC_MAX)
		msb = POC_MAX;

	/* Records the picture's lsb and Msb. */
	state->prev_lsb = header->poc_lsb;
	state->prev_msb = (int32_t)msb;
}

/*
 * Derives the order counts of the reference picture set (8.3.2, 8-5).
 *
 * The short-term entries are split by their used flag in the order the
 * equations visit them, then the long-term entries.  The arrays hold at
 * most 15 entries each, so a header with more entries than the slice
 * parse allows is refused.
 */
int
h265sw_poc_rps(
	const struct h265sw_slice_header *header,
	int32_t poc,
	unsigned log2_max_poc_lsb,
	struct h265sw_rps_pocs *out)
{
	int idr;
	int error;

	/* Refuses arguments no valid caller passes. */
	if (header == NULL || out == NULL)
		return EINVAL;
	if (log2_max_poc_lsb < POC_MIN_LOG2_LSB || log2_max_poc_lsb > POC_MAX_LOG2_LSB)
		return EINVAL;

	/* Starts with every list empty; an IDR picture has no reference picture set. */
	memset(out, 0, sizeof(*out));
	idr = h265sw_nal_is_idr(header->nal_type);
	if (idr != 0)
		return 0;

	/* Splits the short-term entries into the lists the picture uses and those it only keeps. */
	error = poc_rps_short_term(header, poc, out);
	if (error != 0)
		return error;

	/* Derives the long-term entries' order counts. */
	error = poc_rps_long_term(header, poc, log2_max_poc_lsb, out);
	if (error != 0)
		return error;

	/* Succeeded: the five lists are derived. */
	return 0;
}

/* Divides rounding toward negative infinity, for a positive denominator (the Floor( x / m ) of h265sw.h). */
static int64_t
poc_floor_div(
	int64_t numerator,
	int64_t denominator)
{
	int64_t quotient;

	/* C division rounds toward zero, which is one too high for a negative inexact quotient. */
	quotient = numerator / denominator;
	if (numerator % denominator != 0 && numerator < 0)
		quotient -= 1;

	/* Succeeded: the floor. */
	return quotient;
}

/* Appends an order count to a list, refusing a full list or a count outside int32_t. */
static int
poc_push(
	int32_t *list,
	unsigned *count,
	int64_t value)
{
	/* The lists hold at most POC_MAX_ENTRIES entries, so no store leaves the arrays. */
	if (*count >= POC_MAX_ENTRIES)
		return EINVAL;

	/* An order count outside int32_t cannot be a picture's. */
	if (value < POC_MIN || value > POC_MAX)
		return EINVAL;
	list[*count] = (int32_t)value;
	*count += 1U;

	/* Succeeded: the order count is stored. */
	return 0;
}

/* Splits the short-term set into PocStCurrBefore, PocStCurrAfter and PocStFoll (8-5). */
static int
poc_rps_short_term(
	const struct h265sw_slice_header *header,
	int32_t poc,
	struct h265sw_rps_pocs *out)
{
	const struct h265sw_st_rps *set;
	int64_t value;
	unsigned index;
	int error;

	/* A set that does not fit the arrays did not come from the slice parse. */
	set = &header->st_rps;
	if (set->num_negative > POC_MAX_ENTRIES || set->num_positive > POC_MAX_ENTRIES)
		return EINVAL;
	if (set->num_negative + set->num_positive > POC_MAX_ENTRIES)
		return EINVAL;

	/* Visits the pictures before the current one, the nearest first: used ones go to Before, the others to Foll. */
	for (index = 0U; index < set->num_negative; index++) {
		value = (int64_t)poc + (int64_t)set->delta_poc[index];
		if (set->used[index] != 0U) {
			error = poc_push(out->st_before, &out->num_st_before, value);
		} else {
			error = poc_push(out->st_foll, &out->num_st_foll, value);
		}
		if (error != 0)
			return error;
	}

	/* Visits the pictures after the current one: used ones go to After, the others to Foll. */
	for (index = set->num_negative; index < set->num_negative + set->num_positive; index++) {
		value = (int64_t)poc + (int64_t)set->delta_poc[index];
		if (set->used[index] != 0U) {
			error = poc_push(out->st_after, &out->num_st_after, value);
		} else {
			error = poc_push(out->st_foll, &out->num_st_foll, value);
		}
		if (error != 0)
			return error;
	}

	/* Succeeded: the short-term lists are derived. */
	return 0;
}

/* Derives PocLtCurr and PocLtFoll with their msb-present flags (8-5). */
static int
poc_rps_long_term(
	const struct h265sw_slice_header *header,
	int32_t poc,
	unsigned log2_max_poc_lsb,
	struct h265sw_rps_pocs *out)
{
	uint64_t total;
	int64_t max_lsb;
	int64_t poc_lsb_part;
	int64_t value;
	unsigned index;
	uint8_t msb_flag;
	int error;

	/* The entries fit the arrays; the sum is made wide so no pair of counts wraps it. */
	total = (uint64_t)header->num_long_term_sps + (uint64_t)header->num_long_term_pics;
	if (total > (uint64_t)POC_MAX_ENTRIES)
		return EINVAL;

	/* PicOrderCntVal & ( MaxPicOrderCntLsb - 1 ), computed as x - Floor( x / m ) * m so a negative order count has no bitwise operation. */
	max_lsb = (int64_t)1 << log2_max_poc_lsb;
	poc_lsb_part = (int64_t)poc - poc_floor_div((int64_t)poc, max_lsb) * max_lsb;

	/* Derives each entry's order count: its lsb, moved to the full count when its most significant part is given. */
	for (index = 0U; index < (unsigned)total; index++) {
		value = (int64_t)header->lt_poc_lsb[index];
		msb_flag = 0U;
		if (header->lt_msb_present[index] != 0U) {
			msb_flag = 1U;
			value += (int64_t)poc - (int64_t)header->lt_msb_cycle[index] * max_lsb - poc_lsb_part;
		}

		/* The entry the picture uses goes to Curr, the one it only keeps to Foll. */
		if (header->lt_used[index] != 0U) {
			error = poc_push(out->lt_curr, &out->num_lt_curr, value);
			if (error != 0)
				return error;
			out->lt_curr_msb[out->num_lt_curr - 1U] = msb_flag;
		} else {
			error = poc_push(out->lt_foll, &out->num_lt_foll, value);
			if (error != 0)
				return error;
			out->lt_foll_msb[out->num_lt_foll - 1U] = msb_flag;
		}
	}

	/* Succeeded: the long-term lists are derived. */
	return 0;
}
