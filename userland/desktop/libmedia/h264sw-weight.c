/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The weighted sample prediction of the software H.264 decoder (8.4.2.3,
 * 8.4.3).
 */

#include "h264sw-weight.h"

#include <stddef.h>

/* The explicit weighting is read from a table whose denominators are at most 7 (7.4.3.2: luma_log2_weight_denom, chroma_log2_weight_denom). */
#define WEIGHT_MAX_LOG_WD 7U

/* The implicit weights have logWD 5 and the weights 32 and 32 when the order counts give no scale (8-277, 8-280, 8-281). */
#define WEIGHT_IMPLICIT_LOG_WD 5U
#define WEIGHT_IMPLICIT_EQUAL 32

/* The two implicit weights add up to 64, the scale of logWD 5 (8-282, 8-283). */
#define WEIGHT_IMPLICIT_TOTAL 64

/* The largest sample value of an 8-bit picture, the upper bound of Clip1 in 8-274 to 8-276. */
#define WEIGHT_SAMPLE_MAX 255

/* The component numbers of h264sw_weights_of. */
#define WEIGHT_COMPONENT_LUMA 0U
#define WEIGHT_COMPONENT_CR 2U

/* Forward declarations of the static functions. */
static int64_t weight_floor_shift(int64_t value, unsigned shift);
static int64_t weight_clip3(int64_t low, int64_t high, int64_t value);
static uint8_t weight_clip_sample(int64_t value);
static int weight_implicit_w0(int32_t poc, const struct h264sw_ref *pic0, const struct h264sw_ref *pic1);
static void weight_default_factors(struct h264sw_weight_factors *factors);
static void weight_copy(const uint8_t *source, size_t source_stride, unsigned width, unsigned height, uint8_t *out, size_t out_stride);
static void weight_average(const uint8_t *p0, const uint8_t *p1, size_t p_stride, unsigned width, unsigned height, uint8_t *out, size_t out_stride);
static void weight_single(const uint8_t *source, size_t source_stride, int weight, int offset, unsigned log_wd, unsigned width, unsigned height, uint8_t *out, size_t out_stride);
static void weight_bipred(const struct h264sw_weight_factors *factors, const uint8_t *p0, const uint8_t *p1, size_t p_stride, unsigned width, unsigned height, uint8_t *out, size_t out_stride);

/*
 * Prepares the weights of a slice (8.4.3).
 *
 * The mode follows the slice type and the PPS (8.4.2.3); explicit
 * weights need the slice header to carry its table, and a header
 * without one gives the default prediction.  Every pair of the implicit
 * table is set, to 32 when the pair is not used or the inputs are
 * missing.
 */
void
h264sw_weights_setup(
	struct h264sw_weights *weights,
	const struct h264sw_pps *pps,
	const struct h264sw_slice_header *header,
	const struct h264sw_reflist *lists,
	int32_t poc)
{
	unsigned count0;
	unsigned count1;
	unsigned i;
	unsigned j;
	int w0;

	/* Ignores a missing output. */
	if (weights == NULL)
		return;

	/* Starts from the default prediction with every implicit pair at the equal weights. */
	weights->mode = H264SW_WEIGHT_DEFAULT;
	weights->header = header;
	for (i = 0; i < H264SW_MAX_LIST; i++) {
		for (j = 0; j < H264SW_MAX_LIST; j++)
			weights->implicit_w0[i][j] = WEIGHT_IMPLICIT_EQUAL;
	}

	/* Keeps the default prediction when the PPS or the header is missing. */
	if (pps == NULL)
		return;
	if (header == NULL)
		return;

	/* Selects the mode: P and SP slices by weighted_pred_flag, B slices by weighted_bipred_idc (8.4.2.3); I slices predict nothing. */
	if (header->slice_type == H264SW_SLICE_P || header->slice_type == H264SW_SLICE_SP) {
		/* A P slice weights explicitly when weighted_pred_flag is set. */
		if (pps->weighted_pred != 0)
			weights->mode = H264SW_WEIGHT_EXPLICIT;
	} else if (header->slice_type == H264SW_SLICE_B) {
		/* A B slice weights explicitly for idc 1 and implicitly for idc 2. */
		if (pps->weighted_bipred_idc == 1U)
			weights->mode = H264SW_WEIGHT_EXPLICIT;
		else if (pps->weighted_bipred_idc == 2U)
			weights->mode = H264SW_WEIGHT_IMPLICIT;
	}

	/* Falls back to the default prediction when the explicit table was not read, as its entries would then hold nothing. */
	if (weights->mode == H264SW_WEIGHT_EXPLICIT && header->weighted == 0)
		weights->mode = H264SW_WEIGHT_DEFAULT;

	/* Derives the implicit weights of every pair of reference indices only when they will be used. */
	if (weights->mode != H264SW_WEIGHT_IMPLICIT)
		return;
	if (lists == NULL)
		return;

	/* Limits the pairs to the lists' lengths, which cannot exceed the arrays. */
	count0 = lists->count[0];
	count1 = lists->count[1];
	if (count0 > H264SW_MAX_LIST)
		count0 = H264SW_MAX_LIST;
	if (count1 > H264SW_MAX_LIST)
		count1 = H264SW_MAX_LIST;

	/* Derives w0 for each pair of a list 0 and a list 1 entry from the order counts (8-280 to 8-283). */
	for (i = 0; i < count0; i++) {
		for (j = 0; j < count1; j++) {
			w0 = weight_implicit_w0(poc, &lists->list[0][i], &lists->list[1][j]);
			weights->implicit_w0[i][j] = (int16_t)w0;
		}
	}
}

/*
 * Gives the factors for one partition's prediction (8.4.3).
 *
 * An input that names no prediction (a missing output, no list in use,
 * a reference index beyond the lists' arrays, a component above 2) gives
 * the default prediction (weighted 0).
 */
void
h264sw_weights_of(
	const struct h264sw_weights *weights,
	int ref_idx0,
	int ref_idx1,
	unsigned component,
	struct h264sw_weight_factors *factors)
{
	const struct h264sw_weight *entry0;
	const struct h264sw_weight *entry1;
	int use0;
	int use1;

	/* Ignores a missing output. */
	if (factors == NULL)
		return;

	/* Starts from the default prediction, which stands for every input that is refused. */
	weight_default_factors(factors);

	/* Refuses a missing weight set. */
	if (weights == NULL)
		return;

	/* Finds which lists the partition predicts from; a partition that uses neither has nothing to weight. */
	use0 = 0;
	use1 = 0;
	if (ref_idx0 >= 0)
		use0 = 1;
	if (ref_idx1 >= 0)
		use1 = 1;
	if (!use0 && !use1)
		return;

	/* Refuses a reference index beyond the tables. */
	if (use0 && (unsigned)ref_idx0 >= H264SW_MAX_LIST)
		return;
	if (use1 && (unsigned)ref_idx1 >= H264SW_MAX_LIST)
		return;

	/* Gives the implicit factors for a bi-predicted partition; a single list is predicted by default (8.4.3, implicitModeFlag). */
	if (weights->mode == H264SW_WEIGHT_IMPLICIT) {
		/* Leaves the default prediction for a partition of one list. */
		if (!use0 || !use1)
			return;

		/* Takes w0 from the table, w1 as its complement to 64, and no offsets (8-277 to 8-279, 8-283). */
		factors->weighted = 1;
		factors->log_wd = WEIGHT_IMPLICIT_LOG_WD;
		factors->w0 = weights->implicit_w0[ref_idx0][ref_idx1];
		factors->w1 = WEIGHT_IMPLICIT_TOTAL - factors->w0;
		factors->o0 = 0;
		factors->o1 = 0;

		/* The implicit factors are complete; the explicit table does not apply. */
		return;
	}

	/* Keeps the default prediction unless the explicit table applies. */
	if (weights->mode != H264SW_WEIGHT_EXPLICIT)
		return;
	if (weights->header == NULL)
		return;
	if (component > WEIGHT_COMPONENT_CR)
		return;

	/* Takes the denominator of the component (8-288, 8-293). */
	factors->weighted = 1;
	if (component == WEIGHT_COMPONENT_LUMA)
		factors->log_wd = weights->header->luma_log2_denom;
	else
		factors->log_wd = weights->header->chroma_log2_denom;

	/* Takes the weight and the offset of the list 0 reference (8-289, 8-291, 8-294, 8-296). */
	if (use0) {
		entry0 = &weights->header->weight[0][ref_idx0];

		/* Luma has one weight and offset; each chroma component has its own. */
		if (component == WEIGHT_COMPONENT_LUMA) {
			factors->w0 = entry0->luma_weight;
			factors->o0 = entry0->luma_offset;
		} else {
			factors->w0 = entry0->chroma_weight[component - 1U];
			factors->o0 = entry0->chroma_offset[component - 1U];
		}
	}

	/* Takes the weight and the offset of the list 1 reference (8-290, 8-292, 8-295, 8-297). */
	if (use1) {
		entry1 = &weights->header->weight[1][ref_idx1];

		/* Luma has one weight and offset; each chroma component has its own. */
		if (component == WEIGHT_COMPONENT_LUMA) {
			factors->w1 = entry1->luma_weight;
			factors->o1 = entry1->luma_offset;
		} else {
			factors->w1 = entry1->chroma_weight[component - 1U];
			factors->o1 = entry1->chroma_offset[component - 1U];
		}
	}
}

/*
 * Combines the predictions of a block into the final prediction (8.4.2.3).
 *
 * A call that cannot describe a block (a missing factors or output, no
 * prediction, an empty block) writes nothing.  A logWD above 7 is
 * taken as 7, the largest the syntax allows.  The products are
 * formed in 64 bits, so any weights and offsets give a defined result.
 */
void
h264sw_weights_apply(
	const struct h264sw_weight_factors *factors,
	const uint8_t *p0,
	const uint8_t *p1,
	size_t p_stride,
	unsigned width,
	unsigned height,
	uint8_t *out,
	size_t out_stride)
{
	unsigned log_wd;

	/* Refuses a call without factors, without a destination or without a block. */
	if (factors == NULL)
		return;
	if (out == NULL)
		return;
	if (width == 0U || height == 0U)
		return;

	/* Refuses a call with no prediction at all. */
	if (p0 == NULL && p1 == NULL)
		return;

	/* Keeps the shift in the range the syntax allows. */
	log_wd = factors->log_wd;
	if (log_wd > WEIGHT_MAX_LOG_WD)
		log_wd = WEIGHT_MAX_LOG_WD;

	/* Combines the prediction by the default process (8-271 to 8-273) when no weights apply. */
	if (factors->weighted == 0) {
		/* Two lists are averaged; one list is copied. */
		if (p0 != NULL && p1 != NULL)
			weight_average(p0, p1, p_stride, width, height, out, out_stride);
		else if (p0 != NULL)
			weight_copy(p0, p_stride, width, height, out, out_stride);
		else
			weight_copy(p1, p_stride, width, height, out, out_stride);

		/* The default prediction is complete. */
		return;
	}

	/* Combines the two predictions by the weighted process (8-276). */
	if (p0 != NULL && p1 != NULL) {
		weight_bipred(factors, p0, p1, p_stride, width, height, out, out_stride);

		/* The weighted bi-prediction is complete. */
		return;
	}

	/* Weights the list 0 prediction alone with w0 and o0 (8-274), or the list 1 prediction alone with w1 and o1 (8-275). */
	if (p0 != NULL)
		weight_single(p0, p_stride, factors->w0, factors->o0, log_wd, width, height, out, out_stride);
	else
		weight_single(p1, p_stride, factors->w1, factors->o1, log_wd, width, height, out, out_stride);
}

/*
 * Divides by a power of two, rounding toward minus infinity, as the shift of 8.4.2.3.2 does.
 *
 * A negative value is not shifted: its magnitude is rounded up and negated, which is the floor.
 */
static int64_t
weight_floor_shift(
	int64_t value,
	unsigned shift)
{
	int64_t magnitude;
	int64_t rounding;

	/* Shifts a value that is not negative directly. */
	if (value >= 0)
		return value >> shift;

	/* Rounds the magnitude of a negative value up, then negates it. */
	magnitude = -value;
	rounding = ((int64_t)1 << shift) - 1;
	return -((magnitude + rounding) >> shift);
}

/* Clamps a value into low..high (Clip3). */
static int64_t
weight_clip3(
	int64_t low,
	int64_t high,
	int64_t value)
{
	/* Clamps a value below the range to its lower bound. */
	if (value < low)
		return low;

	/* Clamps a value above the range to its upper bound. */
	if (value > high)
		return high;

	/* Keeps a value inside the range. */
	return value;
}

/* Clips a value to a sample of an 8-bit picture (Clip1). */
static uint8_t
weight_clip_sample(
	int64_t value)
{
	int64_t clipped;

	/* Clamps to 0..255. */
	clipped = weight_clip3(0, WEIGHT_SAMPLE_MAX, value);

	/* Reports the sample. */
	return (uint8_t)clipped;
}

/*
 * Derives the implicit weight w0 of one pair of references (8-280 to 8-283).
 *
 * The weight is 32 when the two references have the same order count or
 * one is long-term, or when the scaled distance falls outside -64..128;
 * otherwise it is 64 - ( DistScaleFactor >> 2 ).  The order count
 * differences are formed in 64 bits and clamped as 8-201 and 8-202 say,
 * and the division of 8-197 is done on magnitudes, so no operation
 * depends on the rounding of a negative quotient or shift.
 */
static int
weight_implicit_w0(
	int32_t poc,
	const struct h264sw_ref *pic0,
	const struct h264sw_ref *pic1)
{
	int64_t pic_diff;
	int64_t tb;
	int64_t td;
	int64_t td_magnitude;
	int64_t tx_magnitude;
	int64_t tx;
	int64_t product;
	int64_t scale;
	int64_t scaled;

	/* Takes DiffPicOrderCnt( pic1, pic0 ), whose zero gives equal weights (8-280, 8-281). */
	pic_diff = (int64_t)pic1->poc - (int64_t)pic0->poc;
	if (pic_diff == 0)
		return WEIGHT_IMPLICIT_EQUAL;

	/* Gives equal weights when a reference is marked long-term (8-280, 8-281). */
	if (pic0->long_term != 0)
		return WEIGHT_IMPLICIT_EQUAL;
	if (pic1->long_term != 0)
		return WEIGHT_IMPLICIT_EQUAL;

	/* Derives tb and td, the clamped distances of the current picture and of pic1 from pic0 (8-201, 8-202). */
	tb = weight_clip3(-128, 127, (int64_t)poc - (int64_t)pic0->poc);
	td = weight_clip3(-128, 127, pic_diff);

	/* Derives tx = ( 16384 + Abs( td / 2 ) ) / td (8-197); the division is of magnitudes, so the quotient truncates toward zero. */
	td_magnitude = td;
	if (td_magnitude < 0)
		td_magnitude = -td_magnitude;
	tx_magnitude = (16384 + td_magnitude / 2) / td_magnitude;
	tx = tx_magnitude;
	if (td < 0)
		tx = -tx_magnitude;

	/* Derives DistScaleFactor = Clip3( -1024, 1023, ( tb * tx + 32 ) >> 6 ) (8-198). */
	product = tb * tx + 32;
	scale = weight_clip3(-1024, 1023, weight_floor_shift(product, 6));

	/* Gives equal weights when the scaled distance is outside -64..128 (8-280, 8-281). */
	scaled = weight_floor_shift(scale, 2);
	if (scaled < -64)
		return WEIGHT_IMPLICIT_EQUAL;
	if (scaled > 128)
		return WEIGHT_IMPLICIT_EQUAL;

	/* Reports w0 = 64 - ( DistScaleFactor >> 2 ) (8-282). */
	return (int)(WEIGHT_IMPLICIT_TOTAL - scaled);
}

/* Sets the factors of the default prediction, which uses no weights (8.4.2.3.1). */
static void
weight_default_factors(
	struct h264sw_weight_factors *factors)
{
	/* Clears every factor and marks the prediction as unweighted. */
	factors->weighted = 0;
	factors->log_wd = 0;
	factors->w0 = 0;
	factors->w1 = 0;
	factors->o0 = 0;
	factors->o1 = 0;
}

/* Copies one list's prediction as the final prediction (8-271, 8-272). */
static void
weight_copy(
	const uint8_t *source,
	size_t source_stride,
	unsigned width,
	unsigned height,
	uint8_t *out,
	size_t out_stride)
{
	unsigned row;
	unsigned col;

	/* Copies each sample of each row; the destination may be the source. */
	for (row = 0; row < height; row++) {
		for (col = 0; col < width; col++)
			out[(size_t)row * out_stride + col] = source[(size_t)row * source_stride + col];
	}
}

/* Averages the two lists' predictions with upward rounding (8-273). */
static void
weight_average(
	const uint8_t *p0,
	const uint8_t *p1,
	size_t p_stride,
	unsigned width,
	unsigned height,
	uint8_t *out,
	size_t out_stride)
{
	unsigned row;
	unsigned col;
	unsigned sum;

	/* Averages each sample of each row; the destination may be a source. */
	for (row = 0; row < height; row++) {
		for (col = 0; col < width; col++) {
			sum = (unsigned)p0[(size_t)row * p_stride + col] + (unsigned)p1[(size_t)row * p_stride + col] + 1U;
			out[(size_t)row * out_stride + col] = (uint8_t)(sum >> 1);
		}
	}
}

/*
 * Weights one list's prediction (8-274, 8-275).
 *
 * With logWD of 1 or more the product is rounded by 2^( logWD - 1 )
 * before the shift; with 0 it is not shifted at all.
 */
static void
weight_single(
	const uint8_t *source,
	size_t source_stride,
	int weight,
	int offset,
	unsigned log_wd,
	unsigned width,
	unsigned height,
	uint8_t *out,
	size_t out_stride)
{
	unsigned row;
	unsigned col;
	int64_t product;
	int64_t value;

	/* Weights each sample of each row; the destination may be the source. */
	for (row = 0; row < height; row++) {
		for (col = 0; col < width; col++) {
			product = (int64_t)source[(size_t)row * source_stride + col] * (int64_t)weight;

			/* Rounds and shifts the product when logWD is 1 or more, and leaves it when logWD is 0. */
			if (log_wd >= 1U)
				value = weight_floor_shift(product + ((int64_t)1 << (log_wd - 1U)), log_wd) + (int64_t)offset;
			else
				value = product + (int64_t)offset;

			out[(size_t)row * out_stride + col] = weight_clip_sample(value);
		}
	}
}

/* Weights and combines the two lists' predictions (8-276). */
static void
weight_bipred(
	const struct h264sw_weight_factors *factors,
	const uint8_t *p0,
	const uint8_t *p1,
	size_t p_stride,
	unsigned width,
	unsigned height,
	uint8_t *out,
	size_t out_stride)
{
	unsigned row;
	unsigned col;
	unsigned log_wd;
	int64_t sum;
	int64_t offset;
	int64_t value;

	/* Keeps the shift in the range the syntax allows. */
	log_wd = factors->log_wd;
	if (log_wd > WEIGHT_MAX_LOG_WD)
		log_wd = WEIGHT_MAX_LOG_WD;

	/* Derives the rounded mean of the two offsets ( ( o0 + o1 + 1 ) >> 1 ). */
	offset = weight_floor_shift((int64_t)factors->o0 + (int64_t)factors->o1 + 1, 1);

	/* Combines each sample of each row; the destination may be a source. */
	for (row = 0; row < height; row++) {
		for (col = 0; col < width; col++) {
			sum = (int64_t)p0[(size_t)row * p_stride + col] * (int64_t)factors->w0;
			sum += (int64_t)p1[(size_t)row * p_stride + col] * (int64_t)factors->w1;
			sum += (int64_t)1 << log_wd;
			value = weight_floor_shift(sum, log_wd + 1U) + offset;
			out[(size_t)row * out_stride + col] = weight_clip_sample(value);
		}
	}
}
