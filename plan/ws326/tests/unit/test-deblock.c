/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the deblocking filter of the software H.264 decoder (8.7): the
 * threshold tables (Tables 8-16 and 8-17) through the filters they
 * control, the luma filters for bS 1 to 3 (8.7.2.3) and bS 4 (8.7.2.4),
 * the chroma filters, the clip of indexA and indexB (8-454, 8-455), the
 * boundary strength (8.7.2.1) and the filtering of whole pictures (the
 * edge order, disable_deblocking_filter_idc, the 8x8 transform, the
 * quantization parameters of 8.7.2).
 *
 * The expected values are the spec's equations worked out by hand for
 * small sample sets (each case says its samples and the arithmetic), the
 * values of the spec's tables written again here from the page images,
 * and for the tables sweeps over every index with a sample set chosen so
 * that the table entry shows in the result.
 */

#include <limits.h>
#include <string.h>

#include "check.h"
#include "userland/desktop/libmedia/h264sw-context.h"
#include "userland/desktop/libmedia/h264sw-deblock.h"

/* The picture area the tests use: at most 3 by 2 macroblocks, with rows longer than the picture so that a wrong stride shows. */
#define MAX_W 3U
#define MAX_H 2U
#define LUMA_STRIDE 64U
#define CHROMA_STRIDE 40U

/* The value of the samples outside the pictures the tests fill, so a write outside shows. */
#define SENTINEL 77U

/*
 * The planes, the frame, the macroblocks and the slices of the picture
 * under test.  They live for the whole run and begin_picture sets them up
 * again for each test.
 */
static uint8_t luma[LUMA_STRIDE * MAX_H * 16U];
static uint8_t cb[CHROMA_STRIDE * MAX_H * 8U];
static uint8_t cr[CHROMA_STRIDE * MAX_H * 8U];
static struct h264sw_frame frame;
static struct h264sw_mb mbs[MAX_W * MAX_H];
static struct h264sw_slice_params slices[4];
static struct h264sw_picture_ctx picture;

/* Copies of the planes, to see what a call changed. */
static uint8_t luma_before[sizeof(luma)];
static uint8_t cb_before[sizeof(cb)];
static uint8_t cr_before[sizeof(cr)];

/*
 * Table 8-16 and Table 8-17 as the spec's pages print them: indexA 0..25
 * and then 26..51 of each row, written again from the page images.
 */
static const uint8_t spec_alpha[52] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 5, 6, 7, 8, 9, 10, 12, 13,
	15, 17, 20, 22, 25, 28, 32, 36, 40, 45, 50, 56, 63, 71, 80, 90, 101, 113, 127, 144, 162, 182, 203, 226, 255, 255
};
static const uint8_t spec_beta[52] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4,
	6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18
};
static const uint8_t spec_tc0[3][52] = {
	{
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 6, 6, 7, 8, 9, 10, 11, 13
	},
	{
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 5, 5, 6, 7, 8, 8, 10, 11, 12, 13, 15, 17
	},
	{
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 2, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 6, 6, 7, 8, 9, 10, 11, 13, 14, 16, 18, 20, 23, 25
	}
};

/*
 * The spec's QPC for qPI 30..51 (Table 8-15); below 30 QPC is qPI.  The
 * chroma tests derive their expected indices from it.
 */
static const uint8_t spec_qpc_30[22] = {
	29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36, 36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39
};

/* Forward declarations of the static functions. */
static void begin_picture(unsigned width_mbs, unsigned height_mbs);
static void fill_luma(unsigned value);
static void fill_chroma(unsigned value);
static void set_luma(unsigned x, unsigned y, unsigned value);
static unsigned get_luma(unsigned x, unsigned y);
static void set_cb(unsigned x, unsigned y, unsigned value);
static unsigned get_cb(unsigned x, unsigned y);
static void set_cr(unsigned x, unsigned y, unsigned value);
static unsigned get_cr(unsigned x, unsigned y);
static void snapshot(void);
static int planes_unchanged(void);
static void make_intra(unsigned addr, unsigned qp);
static void make_inter(unsigned addr, unsigned qp, uint32_t ref_id);
static void set_slice(unsigned slice, unsigned idc, int offset_a, int offset_b);
static int same8(const uint8_t *first, const uint8_t *second);
static void run_luma(const uint8_t *samples, const uint8_t *bs, int qp_av, int offset_a, int offset_b, uint8_t out[16][8]);
static void run_luma_rows(const uint8_t *samples, const uint8_t *bs, int qp_av, int offset_a, int offset_b, uint8_t out[16][8]);
static void run_chroma(const uint8_t *samples, const uint8_t *bs, int qp_av, int offset_a, int offset_b, uint8_t out[8][8]);
static void run_chroma_rows(const uint8_t *samples, const uint8_t *bs, int qp_av, int offset_a, int offset_b, uint8_t out[8][8]);
static void luma_uniform(const uint8_t *samples, int strength, int qp_av, int offset_a, int offset_b, uint8_t *out);
static int all_lines_equal(uint8_t lines[16][8]);
static unsigned near_value(unsigned value);
static int alpha_filters(int qp_av, int offset_a, unsigned step);
static void bs_of(unsigned addr, int vertical, unsigned edge, uint8_t *bs);
static void begin_inter_picture(void);
static int bs_is(const uint8_t *bs, unsigned first, unsigned second, unsigned third, unsigned fourth);
static void check_tables(void);
static void check_luma_weak(void);
static void check_luma_strong(void);
static void check_chroma(void);
static void check_index_clip(void);
static void check_segments_and_orientation(void);
static void check_bs_intra(void);
static void check_bs_coefficients(void);
static void check_bs_motion(void);
static void check_bs_errors(void);
static void check_picture_idc(void);
static void check_picture_order(void);
static void check_picture_transform(void);
static void check_picture_qp(void);
static void check_picture_inter(void);
static void check_picture_errors(void);

int
main(void)
{
	/* Runs every group of checks. */
	check_tables();
	check_luma_weak();
	check_luma_strong();
	check_chroma();
	check_index_clip();
	check_segments_and_orientation();
	check_bs_intra();
	check_bs_coefficients();
	check_bs_motion();
	check_bs_errors();
	check_picture_idc();
	check_picture_order();
	check_picture_transform();
	check_picture_qp();
	check_picture_inter();
	check_picture_errors();

	/* Succeeded: reports the summary line. */
	return check_finish("deblock");
}

/*
 * Sets up a picture of the given size: flat planes of the sentinel, every
 * macroblock an intra macroblock of QPY 36 in slice 0, one slice with the
 * filter on and offsets 0, and chroma offsets 0.
 */
static void
begin_picture(
	unsigned width_mbs,
	unsigned height_mbs)
{
	unsigned i;

	/* Fills the planes with the sentinel. */
	memset(luma, (int)SENTINEL, sizeof(luma));
	memset(cb, (int)SENTINEL, sizeof(cb));
	memset(cr, (int)SENTINEL, sizeof(cr));

	/* Describes the frame the picture writes to. */
	memset(&frame, 0, sizeof(frame));
	frame.plane[0] = luma;
	frame.plane[1] = cb;
	frame.plane[2] = cr;
	frame.stride[0] = LUMA_STRIDE;
	frame.stride[1] = CHROMA_STRIDE;
	frame.stride[2] = CHROMA_STRIDE;
	frame.width = width_mbs * 16U;
	frame.height = height_mbs * 16U;

	/* Makes the macroblocks intra with QPY 36 in slice 0. */
	for (i = 0; i < MAX_W * MAX_H; i++) {
		h264sw_mb_reset(&mbs[i], 0);
		make_intra(i, 36U);
	}

	/* Makes one slice with the filter on and no offsets. */
	memset(slices, 0, sizeof(slices));

	/* Describes the picture. */
	memset(&picture, 0, sizeof(picture));
	picture.frame = &frame;
	picture.mbs = mbs;
	picture.width_mbs = width_mbs;
	picture.height_mbs = height_mbs;
	picture.size_mbs = width_mbs * height_mbs;
	picture.slices = slices;
	picture.slice_count = 1;
	picture.slice_capacity = 4;
}

/* Sets every luma sample of the buffer, the margin included. */
static void
fill_luma(
	unsigned value)
{
	memset(luma, (int)value, sizeof(luma));
}

/* Sets every Cb and Cr sample of the buffers, the margin included. */
static void
fill_chroma(
	unsigned value)
{
	memset(cb, (int)value, sizeof(cb));
	memset(cr, (int)value, sizeof(cr));
}

/* Sets one luma sample. */
static void
set_luma(
	unsigned x,
	unsigned y,
	unsigned value)
{
	luma[y * LUMA_STRIDE + x] = (uint8_t)value;
}

/* Reads one luma sample. */
static unsigned
get_luma(
	unsigned x,
	unsigned y)
{
	return luma[y * LUMA_STRIDE + x];
}

/* Sets one Cb sample. */
static void
set_cb(
	unsigned x,
	unsigned y,
	unsigned value)
{
	cb[y * CHROMA_STRIDE + x] = (uint8_t)value;
}

/* Reads one Cb sample. */
static unsigned
get_cb(
	unsigned x,
	unsigned y)
{
	return cb[y * CHROMA_STRIDE + x];
}

/* Sets one Cr sample. */
static void
set_cr(
	unsigned x,
	unsigned y,
	unsigned value)
{
	cr[y * CHROMA_STRIDE + x] = (uint8_t)value;
}

/* Reads one Cr sample. */
static unsigned
get_cr(
	unsigned x,
	unsigned y)
{
	return cr[y * CHROMA_STRIDE + x];
}

/* Remembers the planes as they are. */
static void
snapshot(void)
{
	memcpy(luma_before, luma, sizeof(luma));
	memcpy(cb_before, cb, sizeof(cb));
	memcpy(cr_before, cr, sizeof(cr));
}

/* Tells whether the planes are as the last snapshot left them. */
static int
planes_unchanged(void)
{
	if (memcmp(luma, luma_before, sizeof(luma)) != 0)
		return 0;
	if (memcmp(cb, cb_before, sizeof(cb)) != 0)
		return 0;
	if (memcmp(cr, cr_before, sizeof(cr)) != 0)
		return 0;

	return 1;
}

/* Makes a macroblock an Intra_16x16 one of the given QPY, in slice 0, with no coefficients. */
static void
make_intra(
	unsigned addr,
	unsigned qp)
{
	h264sw_mb_reset(&mbs[addr], 0);
	mbs[addr].kind = H264SW_MB_I_16X16;
	mbs[addr].qp = (uint8_t)qp;
	mbs[addr].motion.intra = 1;
}

/* Makes a macroblock a P one of the given QPY, in slice 0, whose four quadrants use list 0 with reference index 0 naming the frame ref_id and all vectors 0. */
static void
make_inter(
	unsigned addr,
	unsigned qp,
	uint32_t ref_id)
{
	unsigned quadrant;

	h264sw_mb_reset(&mbs[addr], 0);
	mbs[addr].kind = H264SW_MB_P;
	mbs[addr].qp = (uint8_t)qp;
	for (quadrant = 0; quadrant < 4U; quadrant++) {
		mbs[addr].motion.ref_idx[0][quadrant] = 0;
		mbs[addr].motion.ref_id[0][quadrant] = ref_id;
	}
}

/* Sets the parameters of one slice. */
static void
set_slice(
	unsigned slice,
	unsigned idc,
	int offset_a,
	int offset_b)
{
	slices[slice].disable_deblocking = idc;
	slices[slice].filter_offset_a = offset_a;
	slices[slice].filter_offset_b = offset_b;
}

/* Tells whether two lines of 8 samples are equal. */
static int
same8(
	const uint8_t *first,
	const uint8_t *second)
{
	if (memcmp(first, second, 8) != 0)
		return 0;

	return 1;
}

/*
 * Runs the luma filter over one vertical edge.  Every line of the
 * 16-line block holds the same eight samples p3 p2 p1 p0 q0 q1 q2 q3, in
 * this order; the edge lies between the fourth and fifth.  out gets the
 * 16 lines after the filter.
 */
static void
run_luma(
	const uint8_t *samples,
	const uint8_t *bs,
	int qp_av,
	int offset_a,
	int offset_b,
	uint8_t out[16][8])
{
	unsigned line;

	for (line = 0; line < 16U; line++)
		memcpy(out[line], samples, 8);

	h264sw_deblock_luma_edge(&out[0][4], 1, 8, bs, qp_av, offset_a, offset_b);
}

/*
 * Runs the luma filter over one horizontal edge: the eight samples of
 * the same order go down a column, 16 columns side by side, so across is
 * the row length and along is 1.  out gets the 16 columns, each as eight
 * samples in the same order.
 */
static void
run_luma_rows(
	const uint8_t *samples,
	const uint8_t *bs,
	int qp_av,
	int offset_a,
	int offset_b,
	uint8_t out[16][8])
{
	uint8_t block[8][16];
	unsigned row;
	unsigned column;

	for (row = 0; row < 8U; row++) {
		for (column = 0; column < 16U; column++)
			block[row][column] = samples[row];
	}

	h264sw_deblock_luma_edge(&block[4][0], 16, 1, bs, qp_av, offset_a, offset_b);

	for (column = 0; column < 16U; column++) {
		for (row = 0; row < 8U; row++)
			out[column][row] = block[row][column];
	}
}

/* As run_luma for a chroma edge of 8 lines. */
static void
run_chroma(
	const uint8_t *samples,
	const uint8_t *bs,
	int qp_av,
	int offset_a,
	int offset_b,
	uint8_t out[8][8])
{
	unsigned line;

	for (line = 0; line < 8U; line++)
		memcpy(out[line], samples, 8);

	h264sw_deblock_chroma_edge(&out[0][4], 1, 8, bs, qp_av, offset_a, offset_b);
}

/* As run_luma_rows for a chroma edge of 8 columns. */
static void
run_chroma_rows(
	const uint8_t *samples,
	const uint8_t *bs,
	int qp_av,
	int offset_a,
	int offset_b,
	uint8_t out[8][8])
{
	uint8_t block[8][8];
	unsigned row;
	unsigned column;

	for (row = 0; row < 8U; row++) {
		for (column = 0; column < 8U; column++)
			block[row][column] = samples[row];
	}

	h264sw_deblock_chroma_edge(&block[4][0], 8, 1, bs, qp_av, offset_a, offset_b);

	for (column = 0; column < 8U; column++) {
		for (row = 0; row < 8U; row++)
			out[column][row] = block[row][column];
	}
}

/* Runs the luma filter over one vertical edge with the same strength in all four segments, and gives the first line; the 16 lines must all be alike. */
static void
luma_uniform(
	const uint8_t *samples,
	int strength,
	int qp_av,
	int offset_a,
	int offset_b,
	uint8_t *out)
{
	uint8_t bs[4];
	uint8_t lines[16][8];

	bs[0] = (uint8_t)strength;
	bs[1] = (uint8_t)strength;
	bs[2] = (uint8_t)strength;
	bs[3] = (uint8_t)strength;
	run_luma(samples, bs, qp_av, offset_a, offset_b, lines);
	CHECK(all_lines_equal(lines) != 0);
	memcpy(out, lines[0], 8);
}

/* Tells whether the 16 lines of a block are all equal. */
static int
all_lines_equal(
	uint8_t lines[16][8])
{
	unsigned line;

	for (line = 1; line < 16U; line++) {
		if (memcmp(lines[0], lines[line], 8) != 0)
			return 0;
	}

	return 1;
}

/* Gives the value next to a sample value that differs from it by 1 (one below, or 1 for 0). */
static unsigned
near_value(
	unsigned value)
{
	if (value == 0U)
		return 1U;

	return value - 1U;
}

/*
 * Tells whether the bS 4 filter changes a line whose p0 is 0 and q0 is
 * step (p1 is 1, q1 one off q0, the p2 and q2 far away so that only the
 * 3-tap filters apply) for the given qPav and filterOffsetA, with beta
 * at its largest.
 */
static int
alpha_filters(
	int qp_av,
	int offset_a,
	unsigned step)
{
	uint8_t samples[8];
	uint8_t out[8];

	samples[0] = 50;
	samples[1] = 50;
	samples[2] = 1;
	samples[3] = 0;
	samples[4] = (uint8_t)step;
	samples[5] = (uint8_t)near_value(step);
	if (step >= 128U) {
		samples[6] = (uint8_t)(step - 50U);
		samples[7] = (uint8_t)(step - 50U);
	} else {
		samples[6] = (uint8_t)(step + 50U);
		samples[7] = (uint8_t)(step + 50U);
	}
	luma_uniform(samples, 4, qp_av, offset_a, 51, out);

	if (out[3] != samples[3])
		return 1;

	return 0;
}

/*
 * Checks the tables of alpha', beta' and tC0' through the filters, for
 * every index 0..51.
 *
 * alpha: p0 = 0, q0 = alpha' - 1 (filtered) or alpha' (left alone) with
 * the bS 4 filter; the filtered p0 and q0 are (2 * p1 + p0 + q1 + 2) >> 2
 * and (2 * q1 + q0 + p1 + 2) >> 2 (8-480, 8-487).  beta: the same for
 * |p1 - p0| and |q1 - q0| at beta' - 1 and beta'.  tC0: with ap and aq
 * not below beta, tC is tC0, and with a step of alpha' - 1 the
 * unclipped delta of 8-467 is far above tC0, so p0 moves by exactly tC0.
 */
static void
check_tables(void)
{
	uint8_t samples[8];
	uint8_t out[8];
	unsigned index;
	unsigned strength;
	unsigned step;
	unsigned alpha;
	unsigned beta;
	unsigned q1;
	int delta;
	int unclipped;

	/* Sweeps alpha: a step of alpha' - 1 is filtered and a step of alpha' is not. */
	for (index = 0; index < 52U; index++) {
		alpha = spec_alpha[index];

		/* A step of alpha' - 1 (or 0 when alpha' is 0, which no step is below) is the largest filtered one. */
		step = alpha;
		if (alpha != 0U)
			step = alpha - 1U;

		samples[0] = 50;
		samples[1] = 50;
		samples[2] = 1;
		samples[3] = 0;
		samples[4] = (uint8_t)step;
		samples[5] = (uint8_t)near_value(step);
		samples[6] = (uint8_t)(step >= 128U ? step - 50U : step + 50U);
		samples[7] = samples[6];
		luma_uniform(samples, 4, (int)index, 0, 51, out);

		if (alpha != 0U) {
			q1 = near_value(step);
			CHECK(out[3] == ((2U * 1U + 0U + q1 + 2U) >> 2));
			CHECK(out[4] == ((2U * q1 + step + 1U + 2U) >> 2));
		} else {
			CHECK(same8(out, samples));
		}

		/* A step of alpha' is not filtered. */
		step = alpha;
		samples[4] = (uint8_t)step;
		samples[5] = (uint8_t)near_value(step);
		samples[6] = (uint8_t)(step >= 128U ? step - 50U : step + 50U);
		samples[7] = samples[6];
		luma_uniform(samples, 4, (int)index, 0, 51, out);
		CHECK(same8(out, samples));
	}

	/* Sweeps beta: a difference of beta' - 1 on either side is filtered and one of beta' is not. */
	for (index = 0; index < 52U; index++) {
		beta = spec_beta[index];

		/* The p side: p1 is p0 + t, q1 is q0, with p2, q2 far from p0, q0. */
		for (step = (beta == 0U ? 0U : beta - 1U); step <= beta; step++) {
			samples[0] = 150;
			samples[1] = 150;
			samples[2] = (uint8_t)(100U + step);
			samples[3] = 100;
			samples[4] = 100;
			samples[5] = 100;
			samples[6] = 150;
			samples[7] = 150;
			luma_uniform(samples, 4, (int)index, 51, 0, out);

			if (step < beta) {
				CHECK(out[3] == ((2U * (100U + step) + 100U + 100U + 2U) >> 2));
				CHECK(out[4] == ((2U * 100U + 100U + (100U + step) + 2U) >> 2));
			} else {
				CHECK(same8(out, samples));
			}
		}

		/* The q side: q1 is q0 + t, p1 is p0. */
		for (step = (beta == 0U ? 0U : beta - 1U); step <= beta; step++) {
			samples[0] = 150;
			samples[1] = 150;
			samples[2] = 100;
			samples[3] = 100;
			samples[4] = 100;
			samples[5] = (uint8_t)(100U + step);
			samples[6] = 150;
			samples[7] = 150;
			luma_uniform(samples, 4, (int)index, 51, 0, out);

			if (step < beta) {
				CHECK(out[3] == ((2U * 100U + 100U + (100U + step) + 2U) >> 2));
				CHECK(out[4] == ((2U * (100U + step) + 100U + 100U + 2U) >> 2));
			} else {
				CHECK(same8(out, samples));
			}
		}
	}

	/* Sweeps tC0 for bS 1, 2 and 3 at every indexA. */
	for (strength = 1; strength <= 3U; strength++) {
		for (index = 0; index < 52U; index++) {
			alpha = spec_alpha[index];

			/* A step just below alpha' (none when alpha' is 0, where nothing is filtered). */
			step = 0;
			if (alpha != 0U)
				step = alpha - 1U;

			/* p0 = 0 and q0 = step, flat otherwise, p2 and q2 far away. */
			samples[0] = 0;
			samples[1] = 40;
			samples[2] = 0;
			samples[3] = 0;
			samples[4] = (uint8_t)step;
			samples[5] = (uint8_t)step;
			samples[6] = (uint8_t)(step >= 128U ? step - 40U : step + 40U);
			samples[7] = samples[6];
			luma_uniform(samples, (int)strength, (int)index, 0, 51, out);

			/* The unclipped delta of 8-467 for this step; tC is tC0 so p0 moves by min( delta, tC0 ). */
			unclipped = (int)((3U * step + 4U) >> 3);
			delta = unclipped;
			if (delta > (int)spec_tc0[strength - 1U][index])
				delta = (int)spec_tc0[strength - 1U][index];
			if (alpha == 0U)
				delta = 0;

			CHECK((int)out[3] == delta);
			CHECK((int)out[4] == (int)step - delta);

			/* The sample sets are far enough from the unclipped delta that the clip, not the step, decides. */
			if (alpha != 0U)
				CHECK(unclipped >= (int)spec_tc0[strength - 1U][index]);
		}
	}
}

/*
 * The hand-worked lines of the luma filter for bS 1, 2 and 3 at qPav 36
 * (indexA 36: alpha 50, beta 11, tC0 2, 3 and 4).  Each case is the eight
 * samples p3 p2 p1 p0 q0 q1 q2 q3 and the result for bS 1, 2 and 3.
 *
 * Case 0, both sides flat (ap = aq = 4 < beta, so tC = tC0 + 2 = 4, 5,
 * 6): p3..p0 = 100 104 100 100, q0..q3 = 140 140 136 140.  The unclipped
 * delta is ( ( 40 << 2 ) + ( 100 - 140 ) + 4 ) >> 3 = 124 >> 3 = 15, so
 * delta is tC = 4, 5, 6.  p1 moves by Clip3( -tC0, tC0, ( 104 + ( ( 100 +
 * 140 + 1 ) >> 1 ) - 200 ) >> 1 ) = Clip3( -tC0, tC0, 12 ) = tC0; q1 by
 * Clip3( -tC0, tC0, ( 136 + 120 - 280 ) >> 1 = -12 ) = -tC0.
 *
 * Case 1, the mirror image (p0 above q0): the unclipped delta is ( ( -40
 * << 2 ) + ( 140 - 100 ) + 4 ) >> 3 = -116 >> 3 = -15, rounded toward
 * minus infinity, and every sample moves the other way.
 *
 * Case 2, the p side not flat (ap = |120 - 100| = 20 >= beta): tC = tC0
 * + 1 = 3, 4, 5 and p1 stays; the q side is flat.
 *
 * Case 3, the q side not flat (aq = |160 - 140| = 20 >= beta): tC = tC0
 * + 1 and q1 stays; p1 moves by tC0.
 *
 * Case 4, rounding toward minus infinity of a negative odd value: delta
 * = ( ( 1 << 2 ) + ( 101 - 101 ) + 4 ) >> 3 = 1, so p0 = 101 and q0 =
 * 100; p1 moves by Clip3( -tC0, tC0, ( 100 + ( ( 100 + 101 + 1 ) >> 1 ) -
 * 202 ) >> 1 ) = ( -1 ) >> 1 = -1, so p1 = 100; q1 by ( 101 + 101 - 202 )
 * >> 1 = 0.
 */
struct weak_case {
	uint8_t in[8];
	uint8_t out[3][8];
};

static const struct weak_case weak_cases[5] = {
	{
		{ 100, 104, 100, 100, 140, 140, 136, 140 },
		{
			{ 100, 104, 102, 104, 136, 138, 136, 140 },
			{ 100, 104, 103, 105, 135, 137, 136, 140 },
			{ 100, 104, 104, 106, 134, 136, 136, 140 }
		}
	},
	{
		{ 140, 136, 140, 140, 100, 100, 104, 100 },
		{
			{ 140, 136, 138, 136, 104, 102, 104, 100 },
			{ 140, 136, 137, 135, 105, 103, 104, 100 },
			{ 140, 136, 136, 134, 106, 104, 104, 100 }
		}
	},
	{
		{ 100, 120, 100, 100, 140, 140, 136, 140 },
		{
			{ 100, 120, 100, 103, 137, 138, 136, 140 },
			{ 100, 120, 100, 104, 136, 137, 136, 140 },
			{ 100, 120, 100, 105, 135, 136, 136, 140 }
		}
	},
	{
		{ 100, 104, 100, 100, 140, 140, 160, 140 },
		{
			{ 100, 104, 102, 103, 137, 140, 160, 140 },
			{ 100, 104, 103, 104, 136, 140, 160, 140 },
			{ 100, 104, 104, 105, 135, 140, 160, 140 }
		}
	},
	{
		{ 100, 100, 101, 100, 101, 101, 101, 101 },
		{
			{ 100, 100, 100, 101, 100, 101, 101, 101 },
			{ 100, 100, 100, 101, 100, 101, 101, 101 },
			{ 100, 100, 100, 101, 100, 101, 101, 101 }
		}
	}
};

/* A step across the edge of exactly alpha' (50 at indexA 36), which is left alone, and one just below it, which is filtered. */
static const uint8_t step_at_alpha[8] = { 100, 100, 100, 100, 150, 150, 150, 150 };
static const uint8_t step_below_alpha[8] = { 100, 100, 100, 100, 149, 149, 149, 149 };

/*
 * Checks the luma filter for bS 1, 2 and 3 on hand-worked lines (8.7.2.3).
 */
static void
check_luma_weak(void)
{
	uint8_t out[8];
	unsigned index;
	unsigned strength;

	/* Runs each worked case for each strength. */
	for (index = 0; index < 5U; index++) {
		for (strength = 1; strength <= 3U; strength++) {
			luma_uniform(weak_cases[index].in, (int)strength, 36, 0, 0, out);
			CHECK(same8(out, weak_cases[index].out[strength - 1U]));
		}
	}

	/* A step of alpha' (50 at indexA 36) is left alone and one of 49 is filtered. */
	luma_uniform(step_at_alpha, 3, 36, 0, 0, out);
	CHECK(same8(out, step_at_alpha));
	luma_uniform(step_below_alpha, 3, 36, 0, 0, out);
	CHECK(out[3] != 100U);
}

/*
 * The hand-worked lines of the luma filter for bS 4 at qPav 36 (alpha
 * 50, so the strong filter needs a step below ( 50 >> 2 ) + 2 = 14, and
 * beta 11).  Each case is the eight samples p3 p2 p1 p0 q0 q1 q2 q3 and
 * the result.
 *
 * Case 0, both sides flat, step 4: p3..p0 = 90 92 94 96, q0..q3 = 100 102
 * 104 106.  p0' = ( 92 + 188 + 192 + 200 + 102 + 4 ) >> 3 = 778 >> 3 =
 * 97, p1' = ( 92 + 94 + 96 + 100 + 2 ) >> 2 = 96, p2' = ( 180 + 276 + 94
 * + 96 + 100 + 4 ) >> 3 = 93; q0' = ( 94 + 192 + 200 + 204 + 104 + 4 ) >>
 * 3 = 99, q1' = ( 96 + 100 + 102 + 104 + 2 ) >> 2 = 101, q2' = ( 212 +
 * 312 + 102 + 100 + 96 + 4 ) >> 3 = 103.
 *
 * Case 1, the p side not flat (p2 = 80, ap = 16 >= beta): p0' = ( 188 +
 * 96 + 102 + 2 ) >> 2 = 97 and p1, p2 stay; the q side is as in case 0.
 *
 * Case 2, the q side not flat (q2 = 120, aq = 20 >= beta): q0' = ( 204 +
 * 100 + 94 + 2 ) >> 2 = 100 and q1, q2 stay; the p side is as in case 0.
 *
 * Case 3, a step of 14, not below ( alpha >> 2 ) + 2 = 14: both sides use
 * the weak form.  p3..p0 = 90 92 94 96, q0..q3 = 110 108 106 104: p0' =
 * ( 188 + 96 + 108 + 2 ) >> 2 = 98, q0' = ( 216 + 110 + 94 + 2 ) >> 2 =
 * 105.
 *
 * Case 4, a step of 13, below 14: both sides use the strong form.  q0..q3
 * = 109 108 106 104: p0' = ( 92 + 188 + 192 + 218 + 108 + 4 ) >> 3 = 802
 * >> 3 = 100, p1' = ( 92 + 94 + 96 + 109 + 2 ) >> 2 = 98, p2' = ( 180 +
 * 276 + 94 + 96 + 109 + 4 ) >> 3 = 94; q0' = ( 94 + 192 + 218 + 216 + 106
 * + 4 ) >> 3 = 103, q1' = ( 96 + 109 + 108 + 106 + 2 ) >> 2 = 105, q2' =
 * ( 208 + 318 + 108 + 109 + 96 + 4 ) >> 3 = 105.
 */
struct strong_case {
	uint8_t in[8];
	uint8_t out[8];
};

static const struct strong_case strong_cases[5] = {
	{ { 90, 92, 94, 96, 100, 102, 104, 106 }, { 90, 93, 96, 97, 99, 101, 103, 106 } },
	{ { 90, 80, 94, 96, 100, 102, 104, 106 }, { 90, 80, 94, 97, 99, 101, 103, 106 } },
	{ { 90, 92, 94, 96, 100, 102, 120, 106 }, { 90, 93, 96, 97, 100, 102, 120, 106 } },
	{ { 90, 92, 94, 96, 110, 108, 106, 104 }, { 90, 92, 94, 98, 105, 108, 106, 104 } },
	{ { 90, 92, 94, 96, 109, 108, 106, 104 }, { 90, 94, 98, 100, 103, 105, 105, 104 } }
};

/* Checks the luma filter for bS 4 on hand-worked lines (8.7.2.4). */
static void
check_luma_strong(void)
{
	uint8_t out[8];
	unsigned index;

	/* Runs each worked case. */
	for (index = 0; index < 5U; index++) {
		luma_uniform(strong_cases[index].in, 4, 36, 0, 0, out);
		CHECK(same8(out, strong_cases[index].out));
	}

	/* A strength above 4 is treated as 4. */
	luma_uniform(strong_cases[0].in, 200, 36, 0, 0, out);
	CHECK(same8(out, strong_cases[0].out));
}

/*
 * The chroma samples p3 p2 p1 p0 q0 q1 q2 q3 and results of the chroma
 * tests at qPav 36 (indexA 36: alpha 50, beta 11, tC0 2, 3 and 4).
 *
 * chroma_step: p3..p0 = 100 100 100 100, q0..q3 = 120 120 120 120.  The
 * unclipped delta is ( ( 20 << 2 ) + ( 100 - 120 ) + 4 ) >> 3 = 64 >> 3 =
 * 8, so delta is tC = tC0 + 1 = 3, 4, 5: p0 = 103, 104, 105 and q0 = 117,
 * 116, 115; for bS 4 p0' = ( 200 + 100 + 120 + 2 ) >> 2 = 105 and q0' =
 * ( 240 + 120 + 100 + 2 ) >> 2 = 115.
 *
 * chroma_ramp: p3..p0 = 90 92 94 96, q0..q3 = 100 102 104 106, bS 4
 * gives p0' = ( 188 + 96 + 102 + 2 ) >> 2 = 97 and q0' = ( 204 + 100 + 94
 * + 2 ) >> 2 = 100; p1, p2, q1, q2 stay (the luma filter would change
 * them).
 */
static const uint8_t chroma_step[8] = { 100, 100, 100, 100, 120, 120, 120, 120 };
static const uint8_t chroma_step_bs1[8] = { 100, 100, 100, 103, 117, 120, 120, 120 };
static const uint8_t chroma_step_bs2[8] = { 100, 100, 100, 104, 116, 120, 120, 120 };
static const uint8_t chroma_step_bs3[8] = { 100, 100, 100, 105, 115, 120, 120, 120 };
static const uint8_t chroma_step_bs4[8] = { 100, 100, 100, 105, 115, 120, 120, 120 };
static const uint8_t chroma_ramp[8] = { 90, 92, 94, 96, 100, 102, 104, 106 };
static const uint8_t chroma_ramp_bs4[8] = { 90, 92, 94, 97, 100, 102, 104, 106 };
static const uint8_t chroma_big_step[8] = { 100, 100, 100, 100, 150, 150, 150, 150 };

/*
 * Checks the chroma filter (8.7.2.3 and 8.7.2.4 with chromaStyleFilteringFlag
 * 1): only p0 and q0 change, tC is tC0 + 1 for bS below 4 and bS 4 uses
 * the 3-tap averages whatever the flatness.  The segments 0..3 of bs
 * cover the line pairs 0-1, 2-3, 4-5 and 6-7.
 */
static void
check_chroma(void)
{
	uint8_t bs[4];
	uint8_t lines[8][8];
	uint8_t rows[8][8];
	unsigned line;

	/* Mixed strengths: bS 1, 0, 4 and 2 on the four line pairs of a vertical edge. */
	bs[0] = 1;
	bs[1] = 0;
	bs[2] = 4;
	bs[3] = 2;
	run_chroma(chroma_step, bs, 36, 0, 0, lines);
	CHECK(same8(lines[0], chroma_step_bs1));
	CHECK(same8(lines[1], chroma_step_bs1));
	CHECK(same8(lines[2], chroma_step));
	CHECK(same8(lines[3], chroma_step));
	CHECK(same8(lines[4], chroma_step_bs4));
	CHECK(same8(lines[5], chroma_step_bs4));
	CHECK(same8(lines[6], chroma_step_bs2));
	CHECK(same8(lines[7], chroma_step_bs2));

	/* The same on a horizontal edge: the across step is the row length. */
	run_chroma_rows(chroma_step, bs, 36, 0, 0, rows);
	CHECK(same8(rows[0], chroma_step_bs1));
	CHECK(same8(rows[1], chroma_step_bs1));
	CHECK(same8(rows[2], chroma_step));
	CHECK(same8(rows[3], chroma_step));
	CHECK(same8(rows[4], chroma_step_bs4));
	CHECK(same8(rows[5], chroma_step_bs4));
	CHECK(same8(rows[6], chroma_step_bs2));
	CHECK(same8(rows[7], chroma_step_bs2));

	/* bS 3 on every line. */
	bs[0] = 3;
	bs[1] = 3;
	bs[2] = 3;
	bs[3] = 3;
	run_chroma(chroma_step, bs, 36, 0, 0, lines);
	for (line = 0; line < 8U; line++)
		CHECK(same8(lines[line], chroma_step_bs3));

	/* bS 4 on flat sides with a small step is still the 3-tap form of p0 and q0 only. */
	bs[0] = 4;
	bs[1] = 4;
	bs[2] = 4;
	bs[3] = 4;
	run_chroma(chroma_ramp, bs, 36, 0, 0, lines);
	for (line = 0; line < 8U; line++)
		CHECK(same8(lines[line], chroma_ramp_bs4));

	/* The thresholds hold for chroma: a step of alpha' (50) is left alone. */
	bs[0] = 3;
	bs[1] = 3;
	bs[2] = 3;
	bs[3] = 3;
	run_chroma(chroma_big_step, bs, 36, 0, 0, lines);
	for (line = 0; line < 8U; line++)
		CHECK(same8(lines[line], chroma_big_step));
}

/*
 * Checks Clip3( 0, 51, qPav + filterOffsetA ) (8-454) and the same for
 * indexB (8-455), with the offsets of the spec's range of -12..12, and
 * that no integer overflows.
 */
static void
check_index_clip(void)
{
	uint8_t samples[8];
	uint8_t out[8];

	/* qPav 51 with +12 stays at index 51 (alpha' 255): a step of 254 is filtered. */
	CHECK(alpha_filters(51, 12, 254U) != 0);

	/* qPav 51 with -12 is index 39 (alpha' 71): a step of 70 is filtered and 71 is not. */
	CHECK(alpha_filters(51, -12, 70U) != 0);
	CHECK(alpha_filters(51, -12, 71U) == 0);

	/* qPav 20 with +12 is index 32 (alpha' 32): a step of 31 is filtered and 32 is not. */
	CHECK(alpha_filters(20, 12, 31U) != 0);
	CHECK(alpha_filters(20, 12, 32U) == 0);

	/* qPav 20 with -12 is index 8 (alpha' 0): nothing is filtered. */
	CHECK(alpha_filters(20, -12, 3U) == 0);

	/* qPav 5 with -12 is below 0 and clips to index 0: nothing is filtered. */
	CHECK(alpha_filters(5, -12, 3U) == 0);

	/* qPav 51 with +12 and qPav 60 with 0 are both clipped to 51. */
	CHECK(alpha_filters(60, 0, 254U) != 0);

	/* The sum is made without overflow: INT_MAX + INT_MAX clips to 51, and INT_MAX + INT_MIN (-1) and INT_MIN + INT_MIN clip to 0. */
	CHECK(alpha_filters(INT_MAX, INT_MAX, 254U) != 0);
	CHECK(alpha_filters(INT_MAX, INT_MIN, 3U) == 0);
	CHECK(alpha_filters(INT_MIN, INT_MIN, 3U) == 0);
	CHECK(alpha_filters(INT_MIN, INT_MAX, 3U) == 0);

	/*
	 * indexB clips alike: with qPav 40 and filterOffsetB -12 (index 28,
	 * beta' 7) a difference of 6 on the p side is filtered and one of 7 is
	 * not; with +12 (index 51, beta' 18) a difference of 17 is filtered
	 * and one of 18 is not.
	 */
	samples[0] = 150;
	samples[1] = 150;
	samples[2] = 106;
	samples[3] = 100;
	samples[4] = 100;
	samples[5] = 100;
	samples[6] = 150;
	samples[7] = 150;
	luma_uniform(samples, 4, 40, 51, -12, out);
	CHECK(out[3] != 100U);

	samples[2] = 107;
	luma_uniform(samples, 4, 40, 51, -12, out);
	CHECK(same8(out, samples));

	samples[2] = 117;
	luma_uniform(samples, 4, 40, 51, 12, out);
	CHECK(out[3] != 100U);

	samples[2] = 118;
	luma_uniform(samples, 4, 40, 51, 12, out);
	CHECK(same8(out, samples));
}

/*
 * Checks that each 4-line segment of a luma edge takes its own strength,
 * and that a horizontal edge (the across step being the row length) gives
 * what a vertical edge does.  The samples are the first weak case (bS 1,
 * 2 and 3) and the first strong case (bS 4) at qPav 36.
 */
static void
check_segments_and_orientation(void)
{
	uint8_t bs[4];
	uint8_t lines[16][8];
	unsigned line;

	/* Segment 0 bS 1, segment 1 bS 0, segment 2 bS 3, segment 3 bS 2. */
	bs[0] = 1;
	bs[1] = 0;
	bs[2] = 3;
	bs[3] = 2;
	run_luma(weak_cases[0].in, bs, 36, 0, 0, lines);
	for (line = 0; line < 4U; line++)
		CHECK(same8(lines[line], weak_cases[0].out[0]));
	for (line = 4; line < 8U; line++)
		CHECK(same8(lines[line], weak_cases[0].in));
	for (line = 8; line < 12U; line++)
		CHECK(same8(lines[line], weak_cases[0].out[2]));
	for (line = 12; line < 16U; line++)
		CHECK(same8(lines[line], weak_cases[0].out[1]));

	/* The same strengths on a horizontal edge give the same columns. */
	run_luma_rows(weak_cases[0].in, bs, 36, 0, 0, lines);
	for (line = 0; line < 4U; line++)
		CHECK(same8(lines[line], weak_cases[0].out[0]));
	for (line = 4; line < 8U; line++)
		CHECK(same8(lines[line], weak_cases[0].in));
	for (line = 8; line < 12U; line++)
		CHECK(same8(lines[line], weak_cases[0].out[2]));
	for (line = 12; line < 16U; line++)
		CHECK(same8(lines[line], weak_cases[0].out[1]));

	/* A bS 4 horizontal edge gives the strong filter's result. */
	bs[0] = 4;
	bs[1] = 4;
	bs[2] = 4;
	bs[3] = 4;
	run_luma_rows(strong_cases[0].in, bs, 36, 0, 0, lines);
	for (line = 0; line < 16U; line++)
		CHECK(same8(lines[line], strong_cases[0].out));
}

/* Gives the strengths of one edge of a macroblock of the picture under test. */
static void
bs_of(
	unsigned addr,
	int vertical,
	unsigned edge,
	uint8_t *bs)
{
	h264sw_deblock_bs(&picture, addr, vertical, edge, bs);
}

/* Makes the 2 by 2 picture of the boundary strength tests: all four macroblocks inter, QPY 36, reference frame 10, no coefficients. */
static void
begin_inter_picture(void)
{
	unsigned addr;

	begin_picture(2, 2);
	for (addr = 0; addr < 4U; addr++)
		make_inter(addr, 36U, 10U);
}

/* Tells whether the four strengths are those given. */
static int
bs_is(
	const uint8_t *bs,
	unsigned first,
	unsigned second,
	unsigned third,
	unsigned fourth)
{
	if (bs[0] != first)
		return 0;
	if (bs[1] != second)
		return 0;
	if (bs[2] != third)
		return 0;
	if (bs[3] != fourth)
		return 0;

	return 1;
}

/*
 * Checks the intra rules of 8.7.2.1: bS 4 on a macroblock edge when
 * either side is intra, 3 on an internal edge of an intra macroblock, and
 * nothing at the picture's left and upper borders.
 */
static void
check_bs_intra(void)
{
	uint8_t bs[4];
	unsigned edge;

	/* An intra current macroblock: its macroblock edges are 4 and its internal edges 3, in both directions. */
	begin_inter_picture();
	make_intra(3, 36U);
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 4, 4, 4, 4));
	bs_of(3, 0, 0, bs);
	CHECK(bs_is(bs, 4, 4, 4, 4));
	for (edge = 1; edge < 4U; edge++) {
		bs_of(3, 1, edge, bs);
		CHECK(bs_is(bs, 3, 3, 3, 3));
		bs_of(3, 0, edge, bs);
		CHECK(bs_is(bs, 3, 3, 3, 3));
	}

	/* An intra left neighbour makes the vertical macroblock edge 4 but not the horizontal one or the internal ones. */
	begin_inter_picture();
	make_intra(2, 36U);
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 4, 4, 4, 4));
	bs_of(3, 0, 0, bs);
	CHECK(bs_is(bs, 0, 0, 0, 0));
	for (edge = 1; edge < 4U; edge++) {
		bs_of(3, 1, edge, bs);
		CHECK(bs_is(bs, 0, 0, 0, 0));
		bs_of(3, 0, edge, bs);
		CHECK(bs_is(bs, 0, 0, 0, 0));
	}

	/* An intra upper neighbour makes the horizontal macroblock edge 4 but not the vertical one. */
	begin_inter_picture();
	make_intra(1, 36U);
	bs_of(3, 0, 0, bs);
	CHECK(bs_is(bs, 4, 4, 4, 4));
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 0, 0, 0, 0));

	/* The picture's left and upper borders have no macroblock edge to filter, intra or not. */
	begin_picture(2, 2);
	bs_of(0, 1, 0, bs);
	CHECK(bs_is(bs, 0, 0, 0, 0));
	bs_of(0, 0, 0, bs);
	CHECK(bs_is(bs, 0, 0, 0, 0));
	bs_of(2, 1, 0, bs);
	CHECK(bs_is(bs, 0, 0, 0, 0));
	bs_of(1, 0, 0, bs);
	CHECK(bs_is(bs, 0, 0, 0, 0));

	/* The edges inside the first macroblock exist. */
	bs_of(0, 1, 2, bs);
	CHECK(bs_is(bs, 3, 3, 3, 3));

	/* A macroblock the concealment filled in (kind only, no intra flag in the motion) counts as intra. */
	begin_inter_picture();
	mbs[2].kind = H264SW_MB_I_16X16;
	mbs[2].motion.intra = 0;
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 4, 4, 4, 4));

	/* A macroblock with the intra flag in the motion but another kind counts as intra too. */
	begin_inter_picture();
	mbs[2].motion.intra = 1;
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 4, 4, 4, 4));

	/* I_PCM is intra. */
	begin_inter_picture();
	mbs[2].kind = H264SW_MB_I_PCM;
	mbs[2].motion.intra = 0;
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 4, 4, 4, 4));
}

/*
 * Checks bS 2: a non-zero coefficient in the 4x4 (or 8x8) block holding
 * p0 or q0, segment by segment.  The block of a location is luma4x4BlkIdx
 * = 8 * ( y / 8 ) + 4 * ( x / 8 ) + 2 * ( ( y % 8 ) / 4 ) + ( ( x % 8 ) / 4 ) (6-38).
 */
static void
check_bs_coefficients(void)
{
	uint8_t bs[4];

	/* The q block at ( 0, 0 ) is block 0: segment 0 of the left macroblock edge. */
	begin_inter_picture();
	mbs[3].nonzero = 1U << 0;
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 2, 0, 0, 0));

	/* The q blocks at ( 0, 4 ), ( 0, 8 ) and ( 0, 12 ) are blocks 2, 8 and 10. */
	begin_inter_picture();
	mbs[3].nonzero = (1U << 2) | (1U << 10);
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 0, 2, 0, 2));
	begin_inter_picture();
	mbs[3].nonzero = 1U << 8;
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 0, 0, 2, 0));

	/* The p blocks of the left neighbour at x = 12 are blocks 5, 7, 13 and 15. */
	begin_inter_picture();
	mbs[2].nonzero = 1U << 5;
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 2, 0, 0, 0));
	begin_inter_picture();
	mbs[2].nonzero = (1U << 7) | (1U << 13);
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 0, 2, 2, 0));
	begin_inter_picture();
	mbs[2].nonzero = 1U << 15;
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 0, 0, 0, 2));

	/* A coefficient in a block that is not on the edge does not count: block 0 is not at x = 12. */
	begin_inter_picture();
	mbs[2].nonzero = 1U << 0;
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 0, 0, 0, 0));

	/* The p blocks of the upper neighbour at y = 12 are blocks 10, 11, 14 and 15; the q blocks at y = 0 are 0, 1, 4 and 5. */
	begin_inter_picture();
	mbs[1].nonzero = 1U << 14;
	bs_of(3, 0, 0, bs);
	CHECK(bs_is(bs, 0, 0, 2, 0));
	begin_inter_picture();
	mbs[1].nonzero = 1U << 10;
	bs_of(3, 0, 0, bs);
	CHECK(bs_is(bs, 2, 0, 0, 0));
	begin_inter_picture();
	mbs[3].nonzero = (1U << 1) | (1U << 5);
	bs_of(3, 0, 0, bs);
	CHECK(bs_is(bs, 0, 2, 0, 2));

	/* Inside a macroblock the edge at x = 4 lies between blocks 0|1, 2|3, 8|9 and 10|11. */
	begin_inter_picture();
	mbs[3].nonzero = (1U << 3) | (1U << 8);
	bs_of(3, 1, 1, bs);
	CHECK(bs_is(bs, 0, 2, 2, 0));

	/* The edge at x = 8 lies between blocks 1|4, 3|6, 9|12 and 11|14. */
	begin_inter_picture();
	mbs[3].nonzero = (1U << 1) | (1U << 14);
	bs_of(3, 1, 2, bs);
	CHECK(bs_is(bs, 2, 0, 0, 2));

	/* The edge at y = 12 lies between blocks 10|... no: 8|10, 9|11, 12|14 and 13|15 across the 8x8 quadrants' rows; with block 8 set, segment 0 is 2. */
	begin_inter_picture();
	mbs[3].nonzero = 1U << 10;
	bs_of(3, 0, 3, bs);
	CHECK(bs_is(bs, 2, 0, 0, 0));

	/* The coefficients of an 8x8 transform block set the four bits of the block: quadrant 0 at x = 8 touches the segments of y 0..7. */
	begin_inter_picture();
	mbs[3].transform_8x8 = 1;
	mbs[3].nonzero = 0xfU;
	bs_of(3, 1, 2, bs);
	CHECK(bs_is(bs, 2, 2, 0, 0));
}

/*
 * Checks bS 1 and 0 from the motion (8.7.2.1): different reference
 * frames, a different number of vectors, vector differences of 3 and 4
 * quarter samples, bi-prediction with the lists in the other order, and the
 * same frame used twice.  The macroblock edge between macroblock 2 (left)
 * and 3 is used, the q blocks 0, 2, 8, 10 (x = 0) against the p blocks 5,
 * 7, 13, 15 (x = 15): q quadrants 0, 0, 2, 2 and p quadrants 1, 1, 3, 3.
 */
static void
check_bs_motion(void)
{
	uint8_t bs[4];

	/* The same frame and the same vectors give 0. */
	begin_inter_picture();
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 0, 0, 0, 0));

	/* A different reference frame in quadrant 0 of q gives 1 in the segments of its blocks 0 and 2. */
	begin_inter_picture();
	mbs[3].motion.ref_id[0][0] = 11;
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 1, 1, 0, 0));

	/* A different reference index for the same frame gives 0 (the frames are compared, not the indices). */
	begin_inter_picture();
	mbs[3].motion.ref_idx[0][0] = 2;
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 0, 0, 0, 0));

	/* The same frame through list 1 instead of list 0 gives 0 (the list does not matter). */
	begin_inter_picture();
	mbs[3].motion.ref_idx[0][2] = -1;
	mbs[3].motion.ref_id[0][2] = 0;
	mbs[3].motion.ref_idx[1][2] = 0;
	mbs[3].motion.ref_id[1][2] = 10;
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 0, 0, 0, 0));

	/* A vector difference of 3 gives 0 and of 4 gives 1, horizontally and vertically, in either sign; q block 0 against p block 5. */
	begin_inter_picture();
	mbs[3].motion.mv[0][0][0] = 3;
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 0, 0, 0, 0));
	mbs[3].motion.mv[0][0][0] = 4;
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 1, 0, 0, 0));
	mbs[3].motion.mv[0][0][0] = -4;
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 1, 0, 0, 0));
	mbs[3].motion.mv[0][0][0] = -3;
	mbs[3].motion.mv[0][0][1] = 3;
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 0, 0, 0, 0));
	mbs[3].motion.mv[0][0][1] = -4;
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 1, 0, 0, 0));
	mbs[3].motion.mv[0][0][1] = 4;
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 1, 0, 0, 0));

	/* The vector that counts is the one of the block holding the sample: block 5 of the left macroblock is segment 0, block 7 segment 1. */
	begin_inter_picture();
	mbs[2].motion.mv[0][7][1] = 9;
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 0, 1, 0, 0));

	/* A different number of vectors gives 1: p uses lists 0 and 1 in quadrant 1, q only list 0 in quadrant 0 (segments 0 and 1). */
	begin_inter_picture();
	mbs[2].motion.ref_idx[1][1] = 0;
	mbs[2].motion.ref_id[1][1] = 10;
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 1, 1, 0, 0));

	/*
	 * Two vectors each to two different frames, the lists in the other
	 * order: p (quadrant 1) list 0 frame 10 vector ( 0, 0 ), list 1
	 * frame 20 vector ( 10, 0 ); q (quadrant 0) list 0 frame 20 vector ( 10, 0 ),
	 * list 1 frame 10 vector ( 0, 0 ).  Each vector is compared with the one
	 * of the same frame: bS 0.  Changing q's vector of frame 10 to ( 4, 0 )
	 * gives 1.  The block pair is q block 0 and p block 5.
	 */
	begin_inter_picture();
	mbs[2].motion.ref_idx[1][1] = 0;
	mbs[2].motion.ref_id[1][1] = 20;
	mbs[2].motion.mv[1][5][0] = 10;
	mbs[3].motion.ref_idx[0][0] = 0;
	mbs[3].motion.ref_id[0][0] = 20;
	mbs[3].motion.mv[0][0][0] = 10;
	mbs[3].motion.ref_idx[1][0] = 0;
	mbs[3].motion.ref_id[1][0] = 10;
	bs_of(3, 1, 0, bs);
	CHECK(bs[0] == 0);
	mbs[3].motion.mv[1][0][0] = 4;
	bs_of(3, 1, 0, bs);
	CHECK(bs[0] == 1);

	/* Two vectors each, but to different frames (q uses frame 30 instead of 10): 1. */
	begin_inter_picture();
	mbs[2].motion.ref_idx[1][1] = 0;
	mbs[2].motion.ref_id[1][1] = 20;
	mbs[3].motion.ref_idx[0][0] = 0;
	mbs[3].motion.ref_id[0][0] = 30;
	mbs[3].motion.ref_idx[1][0] = 0;
	mbs[3].motion.ref_id[1][0] = 20;
	bs_of(3, 1, 0, bs);
	CHECK(bs[0] == 1);

	/*
	 * Two vectors each to the same frame 10 twice: p ( 0, 0 ) and ( 8, 0 );
	 * q ( 8, 0 ) and ( 0, 0 ) matches p crosswise: 0.  q ( 0, 0 ) and ( 8, 0 )
	 * matches straight: 0.  q ( 0, 8 ) and ( 8, 8 ) matches neither
	 * way: 1.  q ( 0, 0 ) and ( 0, 8 ): straight fails on the second pair, crosswise
	 * on the first: 1.
	 */
	begin_inter_picture();
	mbs[2].motion.ref_idx[1][1] = 0;
	mbs[2].motion.ref_id[1][1] = 10;
	mbs[2].motion.mv[1][5][0] = 8;
	mbs[3].motion.ref_idx[1][0] = 0;
	mbs[3].motion.ref_id[1][0] = 10;
	mbs[3].motion.mv[0][0][0] = 8;
	mbs[3].motion.mv[1][0][0] = 0;
	bs_of(3, 1, 0, bs);
	CHECK(bs[0] == 0);
	mbs[3].motion.mv[0][0][0] = 0;
	mbs[3].motion.mv[1][0][0] = 8;
	bs_of(3, 1, 0, bs);
	CHECK(bs[0] == 0);
	mbs[3].motion.mv[0][0][0] = 0;
	mbs[3].motion.mv[0][0][1] = 8;
	mbs[3].motion.mv[1][0][0] = 8;
	mbs[3].motion.mv[1][0][1] = 8;
	bs_of(3, 1, 0, bs);
	CHECK(bs[0] == 1);
	mbs[3].motion.mv[0][0][0] = 0;
	mbs[3].motion.mv[0][0][1] = 0;
	mbs[3].motion.mv[1][0][0] = 0;
	mbs[3].motion.mv[1][0][1] = 8;
	bs_of(3, 1, 0, bs);
	CHECK(bs[0] == 1);

	/* Inside a macroblock: the blocks 0 and 1 (edge at x = 4) share an 8x8 quadrant but may have different vectors. */
	begin_inter_picture();
	mbs[3].motion.mv[0][1][0] = 4;
	bs_of(3, 1, 1, bs);
	CHECK(bs_is(bs, 1, 0, 0, 0));
	bs_of(3, 1, 2, bs);
	CHECK(bs_is(bs, 1, 0, 0, 0));

	/* Across quadrants (edge at x = 8, blocks 1 and 4) a different frame gives 1 in the segments of both. */
	begin_inter_picture();
	mbs[3].motion.ref_id[0][1] = 11;
	bs_of(3, 1, 2, bs);
	CHECK(bs_is(bs, 1, 1, 0, 0));

	/* The horizontal macroblock edge: the upper macroblock's quadrant 2 (blocks 10 and 11) has another frame, segments 0 and 1. */
	begin_inter_picture();
	mbs[1].motion.ref_id[0][2] = 11;
	bs_of(3, 0, 0, bs);
	CHECK(bs_is(bs, 1, 1, 0, 0));

	/* A coefficient decides before the motion: bS 2 even with a different frame. */
	begin_inter_picture();
	mbs[3].motion.ref_id[0][0] = 11;
	mbs[3].nonzero = 1U << 0;
	bs_of(3, 1, 0, bs);
	CHECK(bs_is(bs, 2, 1, 0, 0));
}

/*
 * Checks the boundary strength function's refusals: a missing output or
 * picture, an address or edge out of range.  The strengths are 0 where an
 * output exists.
 */
static void
check_bs_errors(void)
{
	uint8_t bs[4];

	begin_inter_picture();
	make_intra(3, 36U);

	/* A macroblock address outside the picture gives 0. */
	memset(bs, 9, sizeof(bs));
	h264sw_deblock_bs(&picture, 4, 1, 0, bs);
	CHECK(bs_is(bs, 0, 0, 0, 0));
	memset(bs, 9, sizeof(bs));
	h264sw_deblock_bs(&picture, 0xffffffffU, 1, 0, bs);
	CHECK(bs_is(bs, 0, 0, 0, 0));

	/* An edge index above 3 gives 0. */
	memset(bs, 9, sizeof(bs));
	h264sw_deblock_bs(&picture, 3, 1, 4, bs);
	CHECK(bs_is(bs, 0, 0, 0, 0));
	memset(bs, 9, sizeof(bs));
	h264sw_deblock_bs(&picture, 3, 0, 0xffffffffU, bs);
	CHECK(bs_is(bs, 0, 0, 0, 0));

	/* A missing picture, a missing macroblock array and a zero width give 0. */
	memset(bs, 9, sizeof(bs));
	h264sw_deblock_bs(NULL, 3, 1, 0, bs);
	CHECK(bs_is(bs, 0, 0, 0, 0));
	picture.mbs = NULL;
	memset(bs, 9, sizeof(bs));
	h264sw_deblock_bs(&picture, 3, 1, 0, bs);
	CHECK(bs_is(bs, 0, 0, 0, 0));
	picture.mbs = mbs;
	picture.width_mbs = 0;
	memset(bs, 9, sizeof(bs));
	h264sw_deblock_bs(&picture, 3, 1, 0, bs);
	CHECK(bs_is(bs, 0, 0, 0, 0));

	/* A missing output does not crash. */
	picture.width_mbs = 2;
	h264sw_deblock_bs(&picture, 3, 1, 0, NULL);

	/* Garbage in the motion (a reference index far outside) does not crash. */
	begin_inter_picture();
	mbs[3].motion.ref_idx[0][0] = 127;
	mbs[3].motion.ref_idx[1][0] = -128;
	mbs[3].motion.mv[0][0][0] = 32767;
	mbs[2].motion.mv[0][5][0] = -32768;
	h264sw_deblock_bs(&picture, 3, 1, 0, bs);
	CHECK(bs[0] <= 4U);
}

/*
 * Checks disable_deblocking_filter_idc 1 (no filtering) and 2 (not across
 * a slice boundary), the filter offsets of the slice holding q0, and
 * filtering of a macroblock edge with the neighbour's samples, on a 2 by 1
 * picture with a step of 100 to 106 at the macroblock edge x = 16 (all
 * intra, QPY 36: alpha 50, beta 11).
 *
 * With filtering on, the bS 4 strong filter gives p3..p0 = 100 100 100
 * 100, q0..q3 = 106 106 106 106: p0' = ( 100 + 200 + 200 + 212 + 106 + 4 )
 * >> 3 = 822 >> 3 = 102, p1' = ( 100 + 100 + 100 + 106 + 2 ) >> 2 = 102,
 * p2' = ( 200 + 300 + 100 + 100 + 106 + 4 ) >> 3 = 101; q0' = ( 100 + 200 +
 * 212 + 212 + 106 + 4 ) >> 3 = 834 >> 3 = 104, q1' = ( 100 + 106 + 106 +
 * 106 + 2 ) >> 2 = 105, q2' = ( 212 + 318 + 106 + 106 + 100 + 4 ) >> 3 =
 * 105.  The edges inside the macroblocks lie on flat or already smoothed
 * samples and change nothing (checked by hand for the edge at x = 20).
 */
static void
check_picture_idc(void)
{
	unsigned x;
	unsigned y;
	unsigned expected;
	static const unsigned filtered[8] = { 100, 101, 102, 102, 104, 105, 105, 106 };

	/* Builds the step image. */
	begin_picture(2, 1);

	/* Idc 1 leaves the picture as it is. */
	set_slice(0, 1, 0, 0);
	fill_luma(100);
	fill_chroma(100);
	for (y = 0; y < 16U; y++) {
		for (x = 16; x < 32U; x++)
			set_luma(x, y, 106);
	}
	for (y = 0; y < 8U; y++) {
		for (x = 8; x < 16U; x++) {
			set_cb(x, y, 106);
			set_cr(x, y, 106);
		}
	}
	snapshot();
	h264sw_deblock_picture(&picture);
	CHECK(planes_unchanged() != 0);

	/* Idc 0 smooths the macroblock edge on every row; samples further than three from the edge stay. */
	set_slice(0, 0, 0, 0);
	h264sw_deblock_picture(&picture);
	for (y = 0; y < 16U; y++) {
		CHECK(get_luma(11, y) == 100U);
		for (x = 12; x < 20U; x++)
			CHECK(get_luma(x, y) == filtered[x - 12U]);
		CHECK(get_luma(20, y) == 106U);
	}

	/*
	 * The chroma edge at x = 8 is a bS 4 edge of QPC 34 ( 36 maps to 34,
	 * alpha 40, beta 10): p0' = ( 200 + 100 + 106 + 2 ) >> 2 = 102, q0' =
	 * ( 212 + 106 + 100 + 2 ) >> 2 = 105; no other chroma sample moves.
	 */
	for (y = 0; y < 8U; y++) {
		for (x = 0; x < 16U; x++) {
			expected = (x < 8U) ? 100U : 106U;
			if (x == 7U)
				expected = 102U;
			if (x == 8U)
				expected = 105U;
			CHECK(get_cb(x, y) == expected);
			CHECK(get_cr(x, y) == expected);
		}
	}

	/* The margin outside the picture is untouched. */
	CHECK(get_luma(32, 0) == 100U);
	CHECK(get_luma(0, 16) == 100U);

	/* Idc 2 with both macroblocks in one slice filters like idc 0. */
	begin_picture(2, 1);
	set_slice(0, 2, 0, 0);
	fill_luma(100);
	fill_chroma(100);
	for (y = 0; y < 16U; y++) {
		for (x = 16; x < 32U; x++)
			set_luma(x, y, 106);
	}
	h264sw_deblock_picture(&picture);
	CHECK(get_luma(15, 3) == 102U);
	CHECK(get_luma(16, 3) == 104U);

	/* Idc 2 with the macroblocks in different slices leaves the edge between them alone. */
	begin_picture(2, 1);
	mbs[1].slice = 1;
	picture.slice_count = 2;
	set_slice(0, 0, 0, 0);
	set_slice(1, 2, 0, 0);
	fill_luma(100);
	fill_chroma(100);
	for (y = 0; y < 16U; y++) {
		for (x = 16; x < 32U; x++)
			set_luma(x, y, 106);
	}
	for (y = 0; y < 8U; y++) {
		for (x = 8; x < 16U; x++)
			set_cb(x, y, 106);
	}
	snapshot();
	h264sw_deblock_picture(&picture);
	CHECK(planes_unchanged() != 0);

	/* Idc 0 in the second slice filters the edge between slices; the first slice's idc 2 does not matter (the current macroblock's slice decides). */
	set_slice(0, 2, 0, 0);
	set_slice(1, 0, 0, 0);
	h264sw_deblock_picture(&picture);
	CHECK(get_luma(15, 0) == 102U);
	CHECK(get_luma(16, 0) == 104U);
	CHECK(get_cb(7, 0) == 102U);
	CHECK(get_cb(8, 0) == 105U);

	/*
	 * The offsets of the slice holding q0 are used, not those of p0's slice:
	 * slice 1 (macroblock 1, q0) has offsets -12, so indexA is 24 (alpha 12,
	 * beta 4) and the step of 6 is below the strong limit ( 12 >> 2 ) + 2 =
	 * 5 only if... it is not, so the weak 3-tap form gives p0' = ( 200 + 100 +
	 * 106 + 2 ) >> 2 = 102 and q0' = ( 212 + 106 + 100 + 2 ) >> 2 = 105, with
	 * p1 and q1 unchanged; slice 0 has offsets +12 (index 48) that would
	 * give the strong form.
	 */
	begin_picture(2, 1);
	mbs[1].slice = 1;
	picture.slice_count = 2;
	set_slice(0, 0, 12, 12);
	set_slice(1, 0, -12, -12);
	fill_luma(100);
	for (y = 0; y < 16U; y++) {
		for (x = 16; x < 32U; x++)
			set_luma(x, y, 106);
	}
	h264sw_deblock_picture(&picture);
	CHECK(get_luma(13, 5) == 100U);
	CHECK(get_luma(14, 5) == 100U);
	CHECK(get_luma(15, 5) == 102U);
	CHECK(get_luma(16, 5) == 105U);
	CHECK(get_luma(17, 5) == 106U);
	CHECK(get_luma(18, 5) == 106U);

	/*
	 * Idc 2 and the upper edge: a 1 by 2 picture whose macroblocks are in
	 * different slices is not filtered across, in the same slice it is.
	 */
	begin_picture(1, 2);
	mbs[1].slice = 1;
	picture.slice_count = 2;
	set_slice(0, 0, 0, 0);
	set_slice(1, 2, 0, 0);
	fill_luma(100);
	for (y = 16; y < 32U; y++) {
		for (x = 0; x < 16U; x++)
			set_luma(x, y, 106);
	}
	snapshot();
	h264sw_deblock_picture(&picture);
	CHECK(planes_unchanged() != 0);
	set_slice(1, 0, 0, 0);
	h264sw_deblock_picture(&picture);
	CHECK(get_luma(3, 15) == 102U);
	CHECK(get_luma(3, 16) == 104U);
	CHECK(get_luma(3, 12) == 100U);
	CHECK(get_luma(3, 13) == 101U);
	CHECK(get_luma(3, 14) == 102U);
	CHECK(get_luma(3, 17) == 105U);
	CHECK(get_luma(3, 18) == 105U);
	CHECK(get_luma(3, 19) == 106U);
}

/*
 * Checks the order of the edges (8.7): the luma vertical edges from left
 * to right, then the horizontal edges from top to bottom, each seeing the
 * samples the earlier ones left.
 *
 * One intra macroblock of QPY 36 (alpha 50, beta 11, tC0 4 for bS 3).
 * First a row of steps 100 | 104 | 108 | 112 (each four wide): the edge at
 * x = 4 has p0 = 100, q0 = 104, delta = ( 16 - 4 + 4 ) >> 3 = 2 (below tC =
 * 4 + 2), so 102 | 102, p1' = 100 + ( ( 100 + 102 - 200 ) >> 1 = 1 ) = 101,
 * q1' = 104 + ( ( 104 + 102 - 208 ) >> 1 = -1 ) = 103.  The edge at x = 8
 * then sees 102 103 104 104 | 108 108 108 108 and moves p0, q0 to 106 and
 * p1' = 104 + ( ( 103 + 106 - 208 ) >> 1 = 0 ) = 104, q1' = 108 + ( ( 108 +
 * 106 - 216 ) >> 1 = -1 ) = 107.  The edge at x = 12 sees 106 107 108 108
 * | 112 112 112 112 and gives 110 | 110, p1' = 108 + ( ( 107 + 110 - 216 ) >> 1
 * = 0 ) = 108, q1' = 112 + ( ( 112 + 110 - 224 ) >> 1 = -1 ) = 111.
 * Result for x = 0..15: 100 100 101 102 102 103 104 106 106 107 108 110
 * 110 111 112 112.  The vertical ramp gives the same down the rows.
 */
static void
check_picture_order(void)
{
	static const unsigned ramp[16] = { 100, 100, 101, 102, 102, 103, 104, 106, 106, 107, 108, 110, 110, 111, 112, 112 };
	static const unsigned column3[16] = { 100, 100, 101, 101, 102, 102, 102, 103, 103, 103, 103, 103, 103, 103, 103, 103 };
	unsigned x;
	unsigned y;

	/* The horizontal ramp, all rows alike. */
	begin_picture(1, 1);
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++)
			set_luma(x, y, 100U + 4U * (x / 4U));
	}
	h264sw_deblock_picture(&picture);
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++)
			CHECK(get_luma(x, y) == ramp[x]);
	}

	/* The vertical ramp, all columns alike: the horizontal edges in order top to bottom. */
	begin_picture(1, 1);
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++)
			set_luma(x, y, 100U + 4U * (y / 4U));
	}
	h264sw_deblock_picture(&picture);
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++)
			CHECK(get_luma(x, y) == ramp[y]);
	}

	/*
	 * Vertical edges before horizontal ones: the image is 100 except 108 for
	 * x >= 4 and y >= 4.  The edge at x = 4 on rows 4 and below (bS 3,
	 * tC = 6): delta = ( 32 - 8 + 4 ) >> 3 = 3, so x = 3 becomes 103 and
	 * x = 4 105; p1' = 100 + ( ( 100 + 104 - 200 ) >> 1 = 2 ) = 102 at x = 2;
	 * q1' = 108 + ( ( 108 + 104 - 216 ) >> 1 = -2 ) = 106 at x = 5.  Rows 0..3 do
	 * not change.  Column x = 3 is then 100 above row 4 and 103 from row
	 * 4 down.  The edge at y = 4 on that column: delta = ( 12 - 3 + 4 ) >> 3
	 * = 1, so y = 3 is 101 and y = 4 is 102; p1' = 100 + ( ( 100 + 102 - 200 )
	 * >> 1 = 1 ) = 101 at y = 2, q1' = 103 + ( ( 103 + 102 - 206 ) >> 1 = -1 ) =
	 * 102 at y = 5 (-1 rounds toward minus infinity).  The edge at y = 8
	 * then has p2 = 102 (y = 5), p1 = p0 = 103 and q = 103: p1' = 103 + ( ( 102 +
	 * 103 - 206 ) >> 1 = -1 ) = 102 at y = 6.  The edge at y = 12 changes
	 * nothing.  Column x = 3 from row 0: 100 100 101 101 102 102 102 103
	 * and then 103.
	 */
	begin_picture(1, 1);
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			if (x >= 4U && y >= 4U)
				set_luma(x, y, 108);
			else
				set_luma(x, y, 100);
		}
	}
	h264sw_deblock_picture(&picture);
	for (y = 0; y < 16U; y++)
		CHECK(get_luma(3, y) == column3[y]);

	/* Rows 0..3 on the vertical edge at x = 4 did not move. */
	CHECK(get_luma(2, 0) == 100U);
	CHECK(get_luma(4, 1) == 100U);
}

/*
 * Checks the 8x8 transform (8.7): the luma edges at 4 and 12 samples are
 * skipped, the one at 8 stays, and the chroma edge at 4 chroma samples is
 * filtered whatever transform_size_8x8_flag is.
 *
 * A row of steps 100 | 104 | 108 | 112 of one intra macroblock of QPY 36:
 * only the edge at x = 8 is filtered.  p0 = 104, q0 = 108: delta = ( 16 -
 * 4 + 4 ) >> 3 = 2, p0 = q0 = 106, p1' = 104 + ( ( 104 + 106 - 208 ) >> 1 =
 * 1 ) = 105, q1' = 108 + ( ( 108 + 106 - 216 ) >> 1 = -1 ) = 107.  Result
 * for x = 0..15: 100 100 100 100 104 104 105 106 106 107 108 108 112 112
 * 112 112.
 *
 * The chroma planes have a step of 100 | 108 at x = 4, QPY 36 with
 * chroma offsets 3 (Cb) and -12 (Cr): Cb's qPI is 39 giving QPC 35
 * (alpha 45, beta 10, tC0 4 for bS 3, tC 5): delta = ( 32 - 8 + 4 ) >> 3 =
 * 3, so 103 | 105.  Cr's qPI is 24 giving QPC 24 (alpha 12, beta 4, tC0 1
 * for bS 3, tC 2): delta = 2, so 102 | 106.
 */
static void
check_picture_transform(void)
{
	static const unsigned row8[16] = { 100, 100, 100, 100, 104, 104, 105, 106, 106, 107, 108, 108, 112, 112, 112, 112 };
	unsigned x;
	unsigned y;
	unsigned expected_cb;
	unsigned expected_cr;

	/* Checks the table of QPC the expectations rely on: qPI 39 gives 35, and below 30 QPC is qPI. */
	CHECK(spec_qpc_30[39 - 30] == 35U);

	/* The horizontal ramp with the 8x8 transform: only the edge at x = 8. */
	begin_picture(1, 1);
	mbs[0].transform_8x8 = 1;
	picture.chroma_qp_offset[0] = 3;
	picture.chroma_qp_offset[1] = -12;
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++)
			set_luma(x, y, 100U + 4U * (x / 4U));
	}
	for (y = 0; y < 8U; y++) {
		for (x = 0; x < 8U; x++) {
			set_cb(x, y, (x < 4U) ? 100U : 108U);
			set_cr(x, y, (x < 4U) ? 100U : 108U);
		}
	}
	h264sw_deblock_picture(&picture);
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++)
			CHECK(get_luma(x, y) == row8[x]);
	}

	/* The chroma edge at 4 is filtered although the transform is 8x8, with each component's own offset. */
	for (y = 0; y < 8U; y++) {
		for (x = 0; x < 8U; x++) {
			expected_cb = (x < 4U) ? 100U : 108U;
			expected_cr = expected_cb;
			if (x == 3U) {
				expected_cb = 103U;
				expected_cr = 102U;
			}
			if (x == 4U) {
				expected_cb = 105U;
				expected_cr = 106U;
			}
			CHECK(get_cb(x, y) == expected_cb);
			CHECK(get_cr(x, y) == expected_cr);
		}
	}

	/* The same for the horizontal edges: the vertical ramp with the 8x8 transform filters y = 8 only. */
	begin_picture(1, 1);
	mbs[0].transform_8x8 = 1;
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++)
			set_luma(x, y, 100U + 4U * (y / 4U));
	}
	for (y = 0; y < 8U; y++) {
		for (x = 0; x < 8U; x++) {
			set_cb(x, y, (y < 4U) ? 100U : 108U);
			set_cr(x, y, (y < 4U) ? 100U : 108U);
		}
	}
	h264sw_deblock_picture(&picture);
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++)
			CHECK(get_luma(x, y) == row8[y]);
	}

	/* The chroma edge at row 4 is filtered too (QPC 34: tC0 4 for bS 3 gives the same 103 | 105). */
	for (y = 0; y < 8U; y++) {
		for (x = 0; x < 8U; x++) {
			expected_cb = (y < 4U) ? 100U : 108U;
			if (y == 3U)
				expected_cb = 103U;
			if (y == 4U)
				expected_cb = 105U;
			CHECK(get_cb(x, y) == expected_cb);
		}
	}
}

/*
 * Checks the quantization parameters of the edge (8.7.2): the average
 * ( qPp + qPq + 1 ) >> 1 rounds up, and an I_PCM macroblock counts as 0
 * for luma and as the QPC of 0 for chroma.
 *
 * Two intra macroblocks with QPY 20 and 33 side by side and a step of
 * 100 | 116 at x = 16: qPav = ( 20 + 33 + 1 ) >> 1 = 27, alpha' 17 > 16, so
 * the edge is filtered; without the +1 the average is 26 (alpha' 15) and the
 * step would be left alone.  Step 16 is not below ( 17 >> 2 ) + 2 = 6, so the
 * weak 3-tap form gives p0' = ( 200 + 100 + 116 + 2 ) >> 2 = 104 and q0' =
 * ( 232 + 116 + 100 + 2 ) >> 2 = 112.
 *
 * Then macroblock 0 with QPY 40 and macroblock 1 an I_PCM one that
 * records QPY 40: qPav is ( 40 + 0 + 1 ) >> 1 = 20 (alpha' 7), so a step of
 * 30 is left alone; if the PCM macroblock's 40 were used it would be
 * filtered (alpha' 80).  For chroma the average is ( QPC( 40 ) + QPC( 0 ) +
 * 1 ) >> 1 = ( 36 + 0 + 1 ) >> 1 = 18 (alpha' 5), so a chroma step of 30 is
 * left alone as well.
 */
static void
check_picture_qp(void)
{
	unsigned x;
	unsigned y;

	/* The averaged parameter rounds up. */
	begin_picture(2, 1);
	make_intra(0, 20U);
	make_intra(1, 33U);
	fill_luma(100);
	for (y = 0; y < 16U; y++) {
		for (x = 16; x < 32U; x++)
			set_luma(x, y, 116);
	}
	h264sw_deblock_picture(&picture);
	CHECK(get_luma(14, 2) == 100U);
	CHECK(get_luma(15, 2) == 104U);
	CHECK(get_luma(16, 2) == 112U);
	CHECK(get_luma(17, 2) == 116U);

	/*
	 * The macroblock across a horizontal edge is the upper one, not the left
	 * one: in a 2 by 2 picture macroblock 3 has macroblock 1 above (QPY 20)
	 * and macroblock 2 on its left (QPY 51).  Luma has a step of 100 | 116 at
	 * y = 16, filtered at qPav 27 as above (p0' = 104, q0' = 112; with the
	 * left macroblock's 51 the average would be 42 and the strong filter would
	 * give other values).  Chroma has the same step of 16 at row 8: QPC( 20 ) =
	 * 20 and QPC( 33 ) = 32 average to 26 (alpha' 15), so it is left alone;
	 * with the left macroblock's QPC( 51 ) = 39 the average 36 would filter it.
	 */
	begin_picture(2, 2);
	make_intra(0, 20U);
	make_intra(1, 20U);
	make_intra(2, 51U);
	make_intra(3, 33U);
	fill_luma(100);
	fill_chroma(100);
	for (y = 16; y < 32U; y++) {
		for (x = 0; x < 32U; x++)
			set_luma(x, y, 116);
	}
	for (y = 8; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			set_cb(x, y, 116);
			set_cr(x, y, 116);
		}
	}
	h264sw_deblock_picture(&picture);
	CHECK(get_luma(20, 14) == 100U);
	CHECK(get_luma(20, 15) == 104U);
	CHECK(get_luma(20, 16) == 112U);
	CHECK(get_luma(20, 17) == 116U);
	CHECK(get_cb(10, 7) == 100U);
	CHECK(get_cb(10, 8) == 116U);
	CHECK(get_cr(10, 7) == 100U);
	CHECK(get_cr(10, 8) == 116U);

	/* An I_PCM macroblock counts as QPY 0 whatever it records. */
	begin_picture(2, 1);
	make_intra(0, 40U);
	make_intra(1, 40U);
	mbs[1].kind = H264SW_MB_I_PCM;
	fill_luma(100);
	fill_chroma(100);
	for (y = 0; y < 16U; y++) {
		for (x = 16; x < 32U; x++)
			set_luma(x, y, 130);
	}
	for (y = 0; y < 8U; y++) {
		for (x = 8; x < 16U; x++) {
			set_cb(x, y, 130);
			set_cr(x, y, 130);
		}
	}
	snapshot();
	h264sw_deblock_picture(&picture);
	CHECK(planes_unchanged() != 0);

	/* Control: the same picture with an ordinary macroblock of QPY 40 on the right is filtered, so the test above can fail. */
	mbs[1].kind = H264SW_MB_I_16X16;
	h264sw_deblock_picture(&picture);
	CHECK(get_luma(15, 0) != 100U);
	CHECK(get_cb(7, 0) != 100U);
}

/*
 * Checks that the boundary strengths of inter macroblocks reach the
 * filters, segment by segment.
 *
 * One P macroblock of QPY 36 whose block 1 (at x = 4, y = 0) has a
 * coefficient: the vertical edge at x = 4 has bS 2 in its first segment
 * (rows 0..3) only (block 0 | 1), the one at x = 8 there too (block 1 |
 * 4), and the horizontal edge at y = 4 has bS 2 on columns 4..7 (block 1
 * above block 3); nothing else is filtered.  A step of 100 | 108 at x = 4
 * on all rows:
 * with tC0 3 for bS 2 and indexA 36, tC = 5, delta = ( 32 - 8 + 4 ) >> 3 = 3,
 * so x = 3 is 103 and x = 4 is 105; p1' = 100 + Clip3( -3, 3, ( 100 + 104
 * - 200 ) >> 1 = 2 ) = 102 at x = 2; q1' = 108 + Clip3( -3, 3, ( 108 + 104 -
 * 216 ) >> 1 = -2 ) = 106 at x = 5.  Rows 4..15 are untouched at x = 2..5.
 */
static void
check_picture_inter(void)
{
	unsigned x;
	unsigned y;

	begin_picture(1, 1);
	make_inter(0, 36U, 10U);
	mbs[0].nonzero = 1U << 1;
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++)
			set_luma(x, y, (x < 4U) ? 100U : 108U);
	}
	h264sw_deblock_picture(&picture);
	/* Rows 0..3 on the left of the edge at x = 4 are smoothed; the other rows are not. */
	for (y = 0; y < 4U; y++) {
		CHECK(get_luma(1, y) == 100U);
		CHECK(get_luma(2, y) == 102U);
		CHECK(get_luma(3, y) == 103U);
	}
	for (y = 4; y < 16U; y++) {
		CHECK(get_luma(1, y) == 100U);
		CHECK(get_luma(2, y) == 100U);
		CHECK(get_luma(3, y) == 100U);
	}

	/* On the right of the edge the first rows are smoothed (the horizontal edge at y = 4 reaches rows 2..5 of columns 4 and 5, so rows 0 and 1 and rows 8 and below show the edge at x = 4 alone). */
	for (y = 0; y < 2U; y++) {
		CHECK(get_luma(4, y) == 105U);
		CHECK(get_luma(5, y) == 106U);
	}
	for (y = 8; y < 16U; y++) {
		CHECK(get_luma(4, y) == 108U);
		CHECK(get_luma(5, y) == 108U);
	}

	/*
	 * The chroma edge at 4 chroma samples takes the strengths of the luma
	 * edge at 8 samples, not of the one at 4: with a coefficient in block 4
	 * (at x = 8, y = 0) only the luma edge at x = 8 has bS 2, in its first
	 * segment, which is the first two lines of the chroma edge.  A chroma step
	 * of 100 | 108 at x = 4 at QPC 34 (indexA 34: tC0 2 for bS 2, tC 3, delta
	 * = ( 32 - 8 + 4 ) >> 3 = 3) gives 103 | 105 on rows 0 and 1 of Cb and Cr
	 * and nothing on the other rows.
	 */
	begin_picture(1, 1);
	make_inter(0, 36U, 10U);
	mbs[0].nonzero = 1U << 4;
	fill_luma(100);
	for (y = 0; y < 8U; y++) {
		for (x = 0; x < 8U; x++) {
			set_cb(x, y, (x < 4U) ? 100U : 108U);
			set_cr(x, y, (x < 4U) ? 100U : 108U);
		}
	}
	h264sw_deblock_picture(&picture);
	for (y = 0; y < 8U; y++) {
		CHECK(get_cb(2, y) == 100U);
		CHECK(get_cb(5, y) == 108U);
		CHECK(get_cr(2, y) == 100U);
		CHECK(get_cr(5, y) == 108U);
		if (y < 2U) {
			CHECK(get_cb(3, y) == 103U);
			CHECK(get_cb(4, y) == 105U);
			CHECK(get_cr(3, y) == 103U);
			CHECK(get_cr(4, y) == 105U);
		} else {
			CHECK(get_cb(3, y) == 100U);
			CHECK(get_cb(4, y) == 108U);
			CHECK(get_cr(3, y) == 100U);
			CHECK(get_cr(4, y) == 108U);
		}
	}
}

/*
 * Checks that a picture that cannot be filtered, or macroblocks of an
 * unknown slice, leave the planes alone and do not crash.
 */
static void
check_picture_errors(void)
{
	unsigned x;
	unsigned y;
	uint8_t bs[4];
	uint8_t samples[8];
	uint8_t out[8];

	/* A missing picture does not crash. */
	h264sw_deblock_picture(NULL);

	/* A picture without a frame, macroblocks, slices or planes is left alone. */
	begin_picture(2, 1);
	fill_luma(100);
	for (y = 0; y < 16U; y++) {
		for (x = 16; x < 32U; x++)
			set_luma(x, y, 106);
	}
	snapshot();
	picture.frame = NULL;
	h264sw_deblock_picture(&picture);
	picture.frame = &frame;
	picture.mbs = NULL;
	h264sw_deblock_picture(&picture);
	picture.mbs = mbs;
	picture.slices = NULL;
	h264sw_deblock_picture(&picture);
	picture.slices = slices;
	frame.plane[1] = NULL;
	h264sw_deblock_picture(&picture);
	frame.plane[1] = cb;
	CHECK(planes_unchanged() != 0);

	/* A size that is not the grid, a grid of zero, a frame smaller than the grid and more slices than the array holds are left alone. */
	picture.size_mbs = 3;
	h264sw_deblock_picture(&picture);
	picture.size_mbs = 2;
	picture.width_mbs = 0;
	h264sw_deblock_picture(&picture);
	picture.width_mbs = 2;
	frame.width = 16;
	h264sw_deblock_picture(&picture);
	frame.width = 32;
	frame.stride[0] = 16;
	h264sw_deblock_picture(&picture);
	frame.stride[0] = LUMA_STRIDE;
	frame.stride[1] = 8;
	h264sw_deblock_picture(&picture);
	frame.stride[1] = CHROMA_STRIDE;
	picture.slice_count = 5;
	h264sw_deblock_picture(&picture);
	picture.slice_count = 1;
	picture.width_mbs = H264SW_MAX_SIDE_MBS + 1U;
	h264sw_deblock_picture(&picture);
	picture.width_mbs = 2;
	CHECK(planes_unchanged() != 0);

	/* Control: the unmodified picture is filtered, so the checks above can fail. */
	h264sw_deblock_picture(&picture);
	CHECK(get_luma(15, 0) == 102U);

	/* A macroblock whose slice number is -1 or beyond the slices recorded is not filtered, nor is the edge it shares. */
	begin_picture(2, 1);
	mbs[1].slice = -1;
	fill_luma(100);
	for (y = 0; y < 16U; y++) {
		for (x = 16; x < 32U; x++)
			set_luma(x, y, 106);
	}
	snapshot();
	h264sw_deblock_picture(&picture);
	CHECK(planes_unchanged() != 0);
	mbs[1].slice = 7;
	h264sw_deblock_picture(&picture);
	CHECK(planes_unchanged() != 0);
	mbs[1].slice = 0;
	mbs[0].slice = -1;
	h264sw_deblock_picture(&picture);
	CHECK(planes_unchanged() != 0);
	mbs[0].slice = 7;
	h264sw_deblock_picture(&picture);
	CHECK(planes_unchanged() != 0);

	/* The edge filters take a missing sample pointer or strength array without crashing. */
	memset(bs, 4, sizeof(bs));
	h264sw_deblock_luma_edge(NULL, 1, 8, bs, 36, 0, 0);
	h264sw_deblock_chroma_edge(NULL, 1, 8, bs, 36, 0, 0);
	memset(samples, 100, sizeof(samples));
	h264sw_deblock_luma_edge(samples + 4, 1, 8, NULL, 36, 0, 0);
	h264sw_deblock_chroma_edge(samples + 4, 1, 8, NULL, 36, 0, 0);
	memset(out, 100, sizeof(out));
	CHECK(same8(samples, out));
}
