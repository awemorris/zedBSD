/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The short-term reference picture sets of H.265 (7.3.7, 7.4.8).
 */

#include "h265sw-rps.h"

#include <errno.h>
#include <string.h>

/* The largest abs_delta_rps_minus1, delta_poc_s0_minus1 and delta_poc_s1_minus1 (7.4.8: 2^15 - 1). */
#define RPS_DELTA_MINUS1_MAX 32767U

/* The most entries a set holds: the largest sps_max_dec_pic_buffering_minus1 (7.4.8). */
#define RPS_MAX_PICTURES (H265SW_MAX_RPS - 1U)

/* The range of the int32_t the deltas are kept in, as int64_t bounds for the checked sums. */
#define RPS_DELTA_MAX ((int64_t)2147483647)
#define RPS_DELTA_MIN (-(int64_t)2147483647 - 1)

/* Static function declarations. */
static int rps_push(int32_t *deltas, uint8_t *used, unsigned *count, unsigned limit, int64_t delta, unsigned used_flag);
static int rps_read_explicit(struct vcodec_bitreader *bits, unsigned max_pictures, struct h265sw_st_rps *out, struct h265sw_st_rps_syntax *syntax);
static int rps_read_predicted(struct vcodec_bitreader *bits, unsigned index, unsigned num_sets, const struct h265sw_st_rps *sets, unsigned max_pictures, struct h265sw_st_rps *out, struct h265sw_st_rps_syntax *syntax);
static int rps_derive_predicted(const struct h265sw_st_rps *reference, int32_t delta_rps, uint32_t used_flags, uint32_t use_delta_flags, unsigned max_pictures, struct h265sw_st_rps *out);

/*
 * Reads st_ref_pic_set( index ) (7.3.7) and derives the set (7.4.8).
 *
 * The syntax elements are kept in the structure the caller passes, or in a
 * local one when it passes none, so the two forms of the set share one
 * reading path.
 */
int
h265sw_rps_read(
	struct vcodec_bitreader *bits,
	unsigned index,
	unsigned num_sets,
	const struct h265sw_st_rps *sets,
	unsigned max_pictures,
	struct h265sw_st_rps *out,
	struct h265sw_st_rps_syntax *syntax)
{
	struct h265sw_st_rps_syntax local_syntax;
	struct h265sw_st_rps_syntax *syntax_out;
	size_t start_position;
	unsigned predicted;
	int error;

	/* Refuses arguments no valid caller passes. */
	if (bits == NULL || out == NULL)
		return EINVAL;
	if (num_sets > H265SW_MAX_ST_RPS - 1U || index > num_sets)
		return EINVAL;
	if (max_pictures > RPS_MAX_PICTURES)
		return EINVAL;
	if (index != 0U && sets == NULL)
		return EINVAL;

	/* Picks where the signalled form goes and clears both outputs. */
	syntax_out = syntax;
	if (syntax_out == NULL)
		syntax_out = &local_syntax;
	memset(syntax_out, 0, sizeof(*syntax_out));
	memset(out, 0, sizeof(*out));

	/* Remembers the first bit, for the size of the structure. */
	start_position = bits->position;

	/* Reads inter_ref_pic_set_prediction_flag, which the first set does not have (7.3.7). */
	predicted = 0U;
	if (index != 0U)
		predicted = vcodec_bitreader_u1(bits);
	if (bits->error != 0)
		return EINVAL;

	/* Reads the set in the form the flag says. */
	if (predicted != 0U) {
		syntax_out->inter_rps_pred = 1;
		error = rps_read_predicted(bits, index, num_sets, sets, max_pictures, out, syntax_out);
	} else {
		error = rps_read_explicit(bits, max_pictures, out, syntax_out);
	}
	if (error != 0)
		return error;

	/* A read that ran past the end gave zeros the derivation took for values. */
	if (bits->error != 0)
		return EINVAL;

	/* Records how many bits the structure took. */
	syntax_out->bits = (unsigned)(bits->position - start_position);

	/* Succeeded: the set and its syntax are complete. */
	return 0;
}

/*
 * Appends one entry to a list of a set under construction, refusing it
 * when the list is full or the value leaves int32_t.
 */
static int
rps_push(
	int32_t *deltas,
	uint8_t *used,
	unsigned *count,
	unsigned limit,
	int64_t delta,
	unsigned used_flag)
{
	/* No entry beyond the bound of 7.4.8 is stored, so no write leaves the arrays. */
	if (*count >= limit)
		return EINVAL;

	/* Deltas that do not fit in 32 bits cannot come from a valid chain of sets. */
	if (delta > RPS_DELTA_MAX || delta < RPS_DELTA_MIN)
		return EINVAL;

	/* Stores the entry. */
	deltas[*count] = (int32_t)delta;
	used[*count] = (uint8_t)used_flag;
	*count += 1U;

	/* Succeeded: the entry is stored. */
	return 0;
}

/*
 * Reads the explicit form of st_ref_pic_set() (7.3.7) and derives the set
 * with 7-63 to 7-71.
 */
static int
rps_read_explicit(
	struct vcodec_bitreader *bits,
	unsigned max_pictures,
	struct h265sw_st_rps *out,
	struct h265sw_st_rps_syntax *syntax)
{
	uint32_t num_negative;
	uint32_t num_positive;
	uint32_t delta_minus1;
	uint32_t used_flag;
	int32_t delta;
	unsigned entry;

	/* Reads the two counts. */
	num_negative = vcodec_bitreader_ue(bits);
	num_positive = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;

	/* Bounds the counts by the largest DPB size, the second one by what the first leaves (7.4.8). */
	if (num_negative > max_pictures)
		return EINVAL;
	if (num_positive > max_pictures - num_negative)
		return EINVAL;
	syntax->num_negative_pics = (unsigned)num_negative;
	syntax->num_positive_pics = (unsigned)num_positive;
	out->num_negative = (unsigned)num_negative;
	out->num_positive = (unsigned)num_positive;

	/* Reads the negative entries, each a step further back than the one before (7-67, 7-69). */
	delta = 0;
	for (entry = 0U; entry < num_negative; entry++) {
		delta_minus1 = vcodec_bitreader_ue(bits);
		used_flag = vcodec_bitreader_u1(bits);
		if (bits->error != 0)
			return EINVAL;
		if (delta_minus1 > RPS_DELTA_MINUS1_MAX)
			return EINVAL;

		delta -= (int32_t)delta_minus1 + 1;
		syntax->delta_poc_s0_minus1[entry] = (uint16_t)delta_minus1;
		syntax->used_by_curr_pic_s0_flags |= used_flag << entry;
		out->delta_poc[entry] = delta;
		out->used[entry] = (uint8_t)used_flag;
	}

	/* Reads the positive entries, each a step further ahead (7-68, 7-70). */
	delta = 0;
	for (entry = 0U; entry < num_positive; entry++) {
		delta_minus1 = vcodec_bitreader_ue(bits);
		used_flag = vcodec_bitreader_u1(bits);
		if (bits->error != 0)
			return EINVAL;
		if (delta_minus1 > RPS_DELTA_MINUS1_MAX)
			return EINVAL;

		delta += (int32_t)delta_minus1 + 1;
		syntax->delta_poc_s1_minus1[entry] = (uint16_t)delta_minus1;
		syntax->used_by_curr_pic_s1_flags |= used_flag << entry;
		out->delta_poc[num_negative + entry] = delta;
		out->used[num_negative + entry] = (uint8_t)used_flag;
	}

	/* Succeeded: the set is read. */
	return 0;
}

/*
 * Reads the predicted form of st_ref_pic_set() (7.3.7), after its
 * prediction flag, and derives the set with 7-59 to 7-62.
 */
static int
rps_read_predicted(
	struct vcodec_bitreader *bits,
	unsigned index,
	unsigned num_sets,
	const struct h265sw_st_rps *sets,
	unsigned max_pictures,
	struct h265sw_st_rps *out,
	struct h265sw_st_rps_syntax *syntax)
{
	const struct h265sw_st_rps *reference;
	uint32_t delta_idx_minus1;
	uint32_t delta_rps_sign;
	uint32_t abs_delta_rps_minus1;
	uint32_t used_flag;
	uint32_t use_delta_flag;
	uint32_t used_flags;
	uint32_t use_delta_flags;
	int32_t delta_rps;
	unsigned reference_index;
	unsigned reference_count;
	unsigned entry;
	int error;

	/* The slice header's set names its source; an SPS set predicts from the one before it (7-59). */
	delta_idx_minus1 = 0U;
	if (index == num_sets) {
		delta_idx_minus1 = vcodec_bitreader_ue(bits);
		if (bits->error != 0)
			return EINVAL;
		if (delta_idx_minus1 >= index)
			return EINVAL;
	}
	reference_index = index - ((unsigned)delta_idx_minus1 + 1U);
	reference = &sets[reference_index];
	syntax->delta_idx_minus1 = (unsigned)delta_idx_minus1;

	/* A source set the caller built out of bounds is a caller's mistake. */
	if (reference->num_negative > RPS_MAX_PICTURES || reference->num_positive > RPS_MAX_PICTURES)
		return EINVAL;
	reference_count = reference->num_negative + reference->num_positive;
	if (reference_count > RPS_MAX_PICTURES)
		return EINVAL;
	syntax->ref_num_delta_pocs = reference_count;

	/* Reads the sign and size of deltaRps (7-60). */
	delta_rps_sign = vcodec_bitreader_u1(bits);
	abs_delta_rps_minus1 = vcodec_bitreader_ue(bits);
	if (bits->error != 0)
		return EINVAL;
	if (abs_delta_rps_minus1 > RPS_DELTA_MINUS1_MAX)
		return EINVAL;
	syntax->delta_rps_sign = (int)delta_rps_sign;
	syntax->abs_delta_rps_minus1 = (unsigned)abs_delta_rps_minus1;
	delta_rps = (int32_t)abs_delta_rps_minus1 + 1;
	if (delta_rps_sign != 0U)
		delta_rps = -delta_rps;

	/* Reads one flag pair for each entry of the source set and one more (7.3.7). */
	used_flags = 0U;
	use_delta_flags = 0U;
	for (entry = 0U; entry <= reference_count; entry++) {
		used_flag = vcodec_bitreader_u1(bits);
		use_delta_flag = 1U;
		if (used_flag == 0U)
			use_delta_flag = vcodec_bitreader_u1(bits);
		if (bits->error != 0)
			return EINVAL;

		used_flags |= used_flag << entry;
		use_delta_flags |= use_delta_flag << entry;
	}
	syntax->used_by_curr_pic_flags = used_flags;
	syntax->use_delta_flags = use_delta_flags;

	/* Derives the new set from the source set and the flags (7-61, 7-62). */
	error = rps_derive_predicted(reference, delta_rps, used_flags, use_delta_flags, max_pictures, out);
	if (error != 0)
		return error;

	/* Succeeded: the set is read and derived. */
	return 0;
}

/*
 * Builds a set from a source set, deltaRps and the flags with 7-61 and
 * 7-62, checking each count against max_pictures before an entry is stored.
 */
static int
rps_derive_predicted(
	const struct h265sw_st_rps *reference,
	int32_t delta_rps,
	uint32_t used_flags,
	uint32_t use_delta_flags,
	unsigned max_pictures,
	struct h265sw_st_rps *out)
{
	int32_t positive_delta[H265SW_MAX_RPS];
	uint8_t positive_used[H265SW_MAX_RPS];
	unsigned positive_count;
	unsigned negative_count;
	unsigned reference_count;
	unsigned flag_index;
	unsigned entry;
	int64_t delta;
	int error;

	reference_count = reference->num_negative + reference->num_positive;

	/* The negative entries: the source's positive entries from the last, deltaRps itself, then the source's negative entries (7-61). */
	negative_count = 0U;
	for (entry = reference->num_positive; entry > 0U; entry--) {
		flag_index = reference->num_negative + (entry - 1U);
		delta = (int64_t)reference->delta_poc[flag_index] + delta_rps;
		if (delta >= 0 || ((use_delta_flags >> flag_index) & 1U) == 0U)
			continue;

		error = rps_push(out->delta_poc, out->used, &negative_count, max_pictures, delta, (used_flags >> flag_index) & 1U);
		if (error != 0)
			return error;
	}
	if (delta_rps < 0 && ((use_delta_flags >> reference_count) & 1U) != 0U) {
		error = rps_push(out->delta_poc, out->used, &negative_count, max_pictures, delta_rps, (used_flags >> reference_count) & 1U);
		if (error != 0)
			return error;
	}
	for (entry = 0U; entry < reference->num_negative; entry++) {
		delta = (int64_t)reference->delta_poc[entry] + delta_rps;
		if (delta >= 0 || ((use_delta_flags >> entry) & 1U) == 0U)
			continue;

		error = rps_push(out->delta_poc, out->used, &negative_count, max_pictures, delta, (used_flags >> entry) & 1U);
		if (error != 0)
			return error;
	}
	out->num_negative = negative_count;

	/* The positive entries: the source's negative entries from the last, deltaRps itself, then the source's positive entries (7-62). */
	positive_count = 0U;
	for (entry = reference->num_negative; entry > 0U; entry--) {
		flag_index = entry - 1U;
		delta = (int64_t)reference->delta_poc[flag_index] + delta_rps;
		if (delta <= 0 || ((use_delta_flags >> flag_index) & 1U) == 0U)
			continue;

		error = rps_push(positive_delta, positive_used, &positive_count, max_pictures - negative_count, delta, (used_flags >> flag_index) & 1U);
		if (error != 0)
			return error;
	}
	if (delta_rps > 0 && ((use_delta_flags >> reference_count) & 1U) != 0U) {
		error = rps_push(positive_delta, positive_used, &positive_count, max_pictures - negative_count, delta_rps, (used_flags >> reference_count) & 1U);
		if (error != 0)
			return error;
	}
	for (entry = 0U; entry < reference->num_positive; entry++) {
		flag_index = reference->num_negative + entry;
		delta = (int64_t)reference->delta_poc[flag_index] + delta_rps;
		if (delta <= 0 || ((use_delta_flags >> flag_index) & 1U) == 0U)
			continue;

		error = rps_push(positive_delta, positive_used, &positive_count, max_pictures - negative_count, delta, (used_flags >> flag_index) & 1U);
		if (error != 0)
			return error;
	}
	out->num_positive = positive_count;

	/* Places the positive entries after the negative ones. */
	for (entry = 0U; entry < positive_count; entry++) {
		out->delta_poc[negative_count + entry] = positive_delta[entry];
		out->used[negative_count + entry] = positive_used[entry];
	}

	/* Succeeded: both lists are built. */
	return 0;
}
