/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the reconstruction of a macroblock of the software H.264
 * decoder (8.3 to 8.5) and, with the macroblock layer linked in, the
 * slice data loop that drives it.
 *
 * Expected values are worked out on paper from the equations of the
 * specification and are not taken from another decoder: a flat
 * Intra_16x16 DC macroblock is 128 plus the residual of its DC level
 * (8-321, 8-322, 8-354); QP'C comes from Table 8-15 (copied below from
 * the page); the inverse DC transform of a single level gives the
 * same value in every position (8-320, 8-324); integer vectors move the
 * reference by whole samples with the clamp of 8-239 and 8-240; the
 * fractional chroma samples are 8-270 formed again here from the
 * reference; temporal direct and implicit weights follow 8-191 to
 * 8-202 and 8-276 to 8-283 with the numbers worked out in the comments.
 * The streams of the loop cases are written with the bit writer.
 */

#include "check.h"
#include "bitwriter.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "userland/desktop/libmedia/vcodec-bitreader.h"
#include "userland/desktop/libmedia/h264sw-context.h"
#include "userland/desktop/libmedia/h264sw-intra.h"
#include "userland/desktop/libmedia/h264sw-macroblock.h"
#include "userland/desktop/libmedia/h264sw-recon.h"

/* The storage of every frame of the tests: 3 by 3 macroblocks; a frame may use less of it. */
#define PIC_W 3U
#define PIC_H 3U
#define PIC_MBS 9U
#define LUMA_STRIDE 48U
#define LUMA_ROWS 48U
#define CHROMA_STRIDE 24U
#define CHROMA_ROWS 24U

/* The macroblock most cases reconstruct: the middle one of the 3 by 3 picture. */
#define ADDR 4U

/* The ids of the frames. */
#define ID_CURRENT 1U
#define ID_A 11U
#define ID_B 12U
#define ID_C 13U

/* The frames: 0 the picture being decoded, 1 and 2 references, 3 the co-located frame. */
#define FRAME_CURRENT 0U
#define FRAME_A 1U
#define FRAME_B 2U
#define FRAME_C 3U
#define FRAME_COUNT 4U

/*
 * A frame with the storage of its samples and of its motion.
 */
struct test_frame {
	struct h264sw_frame frame;
	uint8_t luma[LUMA_STRIDE * LUMA_ROWS];
	uint8_t cb[CHROMA_STRIDE * CHROMA_ROWS];
	uint8_t cr[CHROMA_STRIDE * CHROMA_ROWS];
	struct h264sw_motion motion[PIC_MBS];
};

/* The frames, the slice context and the parts it points to; they are big, so they live for the whole run and env_make() rebuilds them. */
static struct test_frame frames[FRAME_COUNT];
static struct h264sw_slice_ctx ctx;
static struct h264sw_slice_header header;
static struct h264sw_picture_ctx picture;
static struct h264sw_sps sps;
static struct h264sw_pps pps;
static struct h264sw_mb mbs[PIC_MBS];

/* The macroblock address the case works on and the picture size in macroblocks. */
static unsigned cur_addr;
static unsigned pic_w;
static unsigned pic_h;

/* The stream of the slice data cases. */
static struct bitwriter writer;

/* The generator of the random cases. */
static uint32_t random_state = 20261031U;

/* The reference frame a slice case keeps from one slice to the next: the samples of a decoded picture. */
static uint8_t saved_luma[LUMA_STRIDE * LUMA_ROWS];
static uint8_t saved_cb[CHROMA_STRIDE * CHROMA_ROWS];
static uint8_t saved_cr[CHROMA_STRIDE * CHROMA_ROWS];

/*
 * QPC as a function of qPI for qPI 30..51; below 30 it equals qPI.
 *
 * Values referenced: ITU-T H.264 (08/2021) Table 8-15
 */
static const int qpc_table[22] = {
	29, 30, 31, 32, 32, 33, 34, 34, 35, 35, 36, 36,
	37, 37, 37, 38, 38, 38, 39, 39, 39, 39
};

/*
 * The first column of v: LevelScale4x4( m, 0, 0 ) with a flat weight of 16 is 16 times it.
 *
 * Values referenced: ITU-T H.264 (08/2021) 8-315
 */
static const int norm4x4_dc[6] = { 10, 11, 13, 14, 16, 18 };

/* Forward declarations of the static functions. */
static uint32_t random_next(void);
static int clamp255(int value);
static int floor_div8(int value);
static void frame_init(struct test_frame *test_frame, unsigned id, unsigned width_mbs, unsigned height_mbs);
static void frame_pattern(struct test_frame *test_frame, unsigned seed);
static void levelscale_flat(void);
static void env_make(unsigned slice_type, unsigned width_mbs, unsigned height_mbs, unsigned addr, int qp);
static void neighbours_update(void);
static void set_ref(unsigned list, unsigned index, unsigned frame_index, int poc);
static void set_type(unsigned slice_type, unsigned mb_type);
static void set_sub(unsigned slice_type, unsigned quadrant, unsigned sub_type);
static void clear_levels(void);
static void neighbour_inter(unsigned addr, int ref_idx, int x, int y);
static int luma_at(unsigned x, unsigned y);
static int cb_at(unsigned x, unsigned y);
static int cr_at(unsigned x, unsigned y);
static int mb_luma(unsigned column, unsigned row);
static int mb_cb(unsigned column, unsigned row);
static int mb_cr(unsigned column, unsigned row);
static int luma_is(unsigned column, unsigned row, unsigned width, unsigned height, int value);
static int chroma_is(unsigned plane, unsigned column, unsigned row, unsigned width, unsigned height, int value);
static int ref_luma(const struct test_frame *source, int x, int y);
static int ref_chroma(const struct test_frame *source, unsigned plane, int x, int y);
static int expect_chroma(const struct test_frame *source, unsigned plane, int x, int y, int mv_x, int mv_y);
static int mv_is(unsigned list, unsigned blk, int x, int y);
static void run_mb(int expected_status);
static void check_pcm(void);
static void check_i16_dc(void);
static void check_i16_ac(void);
static void check_chroma_qp(void);
static void chroma_qp_case(int qp, int offset_cb, int offset_cr, int level);
static void check_i4x4_order(void);
static void check_i8x8(void);
static void check_intra_unavailable(void);
static void check_scaling_lists(void);
static void check_p16x16(void);
static void check_p_fraction(void);
static void check_p_weighted(void);
static void check_pskip(void);
static void check_p_parts(void);
static void check_p_sub(void);
static void check_p_refs(void);
static void check_b_bi(void);
static void check_b_implicit(void);
static void check_b_mixed(void);
static void check_b_direct(void);
static void check_b_8x8(void);
static void check_vector_wrap(void);
static void check_errors(void);
static void fuzz_syntax(void);
static void slice_env(unsigned slice_type);
static void stream_start(void);
static void stream_run(unsigned expected_status);
static void put_pcm_mb(unsigned mb);
static void save_picture(void);
static void restore_reference(void);
static void check_slice_intra(void);
static void check_slice_inter(void);
static void check_slice_random(void);

int
main(
	void)
{
	int status;

	/* The intra and the transform paths of one macroblock. */
	check_pcm();
	check_i16_dc();
	check_i16_ac();
	check_chroma_qp();
	check_i4x4_order();
	check_i8x8();
	check_intra_unavailable();
	check_scaling_lists();

	/* The P macroblocks. */
	check_p16x16();
	check_p_fraction();
	check_p_weighted();
	check_pskip();
	check_p_parts();
	check_p_sub();
	check_p_refs();

	/* The B macroblocks. */
	check_b_bi();
	check_b_implicit();
	check_b_mixed();
	check_b_direct();
	check_b_8x8();
	check_vector_wrap();

	/* Errors and hostile syntax. */
	check_errors();
	fuzz_syntax();

	/* The slice data loop with the reconstruction. */
	check_slice_intra();
	check_slice_inter();
	check_slice_random();

	/* Reports the result. */
	status = check_finish("recon");
	return status;
}

/* Gives the next pseudo-random number (xorshift), for test data. */
static uint32_t
random_next(
	void)
{
	/* Mixes the state with three shifts. */
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;

	/* Succeeded: the new state is the number. */
	return random_state;
}

/* Clips a value to the range of a sample, 0..255 (Clip1 for 8 bits). */
static int
clamp255(
	int value)
{
	/* Below the range. */
	if (value < 0)
		return 0;

	/* Above the range. */
	if (value > 255)
		return 255;

	/* Inside the range. */
	return value;
}

/* Divides by 8 rounding toward minus infinity, as a right shift by 3 does in the clauses. */
static int
floor_div8(
	int value)
{
	int remainder;

	/* The remainder brought into 0..7, so that what is left divides exactly. */
	remainder = ((value % 8) + 8) % 8;

	/* The exact quotient. */
	return (value - remainder) / 8;
}

/* Makes a frame of the given size in macroblocks, with its planes and motion in its own storage, all samples 0. */
static void
frame_init(
	struct test_frame *test_frame,
	unsigned id,
	unsigned width_mbs,
	unsigned height_mbs)
{
	unsigned index;

	/* Everything starts at zero, the motion of every macroblock is intra with no reference. */
	memset(test_frame, 0, sizeof(*test_frame));
	for (index = 0; index < PIC_MBS; index++) {
		memset(test_frame->motion[index].ref_idx, 0xff, sizeof(test_frame->motion[index].ref_idx));
		test_frame->motion[index].intra = 1;
	}

	/* The planes, the strides and the size. */
	test_frame->frame.plane[0] = test_frame->luma;
	test_frame->frame.plane[1] = test_frame->cb;
	test_frame->frame.plane[2] = test_frame->cr;
	test_frame->frame.stride[0] = LUMA_STRIDE;
	test_frame->frame.stride[1] = CHROMA_STRIDE;
	test_frame->frame.stride[2] = CHROMA_STRIDE;
	test_frame->frame.width = 16U * width_mbs;
	test_frame->frame.height = 16U * height_mbs;
	test_frame->frame.motion = test_frame->motion;
	test_frame->frame.size_mbs = width_mbs * height_mbs;
	test_frame->frame.id = id;
	test_frame->frame.refs = 1;
}

/* Fills the samples of a frame with a pattern that differs from place to place and from seed to seed. */
static void
frame_pattern(
	struct test_frame *test_frame,
	unsigned seed)
{
	unsigned x;
	unsigned y;

	/* Luma changes by 3 along a row and by 5 down a column, so every shift of the picture is seen. */
	for (y = 0; y < LUMA_ROWS; y++) {
		for (x = 0; x < LUMA_STRIDE; x++)
			test_frame->luma[y * LUMA_STRIDE + x] = (uint8_t)((x * 3U + y * 5U + seed * 41U) & 255U);
	}

	/* Cb and Cr have their own steps. */
	for (y = 0; y < CHROMA_ROWS; y++) {
		for (x = 0; x < CHROMA_STRIDE; x++) {
			test_frame->cb[y * CHROMA_STRIDE + x] = (uint8_t)((x * 7U + y * 2U + seed * 29U) & 255U);
			test_frame->cr[y * CHROMA_STRIDE + x] = (uint8_t)((x * 2U + y * 9U + seed * 17U) & 255U);
		}
	}
}

/* Gives the picture flat scaling lists: every weight 16. */
static void
levelscale_flat(void)
{
	struct h264sw_scaling scaling;

	/* Flat_4x4_16 and Flat_8x8_16 in every list. */
	memset(&scaling, 16, sizeof(scaling));
	h264sw_levelscale_init(&picture.levelscale, &scaling);
}

/*
 * Rebuilds the whole context for a picture of the given size in
 * macroblocks: the current frame filled with 77 (a value the cases never
 * expect, so a sample left unwritten shows), reference frames A, B and
 * the co-located frame C patterned and with no motion, empty lists, a
 * flat scaling, no neighbours and the macroblock at addr in slice 0.
 */
static void
env_make(
	unsigned slice_type,
	unsigned width_mbs,
	unsigned height_mbs,
	unsigned addr,
	int qp)
{
	unsigned index;

	/* Starts from clean structures. */
	memset(&ctx, 0, sizeof(ctx));
	memset(&header, 0, sizeof(header));
	memset(&picture, 0, sizeof(picture));
	memset(&sps, 0, sizeof(sps));
	memset(&pps, 0, sizeof(pps));
	pic_w = width_mbs;
	pic_h = height_mbs;
	cur_addr = addr;

	/* The frames: the current one is 77 everywhere, the others are patterns. */
	frame_init(&frames[FRAME_CURRENT], ID_CURRENT, width_mbs, height_mbs);
	memset(frames[FRAME_CURRENT].luma, 77, sizeof(frames[FRAME_CURRENT].luma));
	memset(frames[FRAME_CURRENT].cb, 77, sizeof(frames[FRAME_CURRENT].cb));
	memset(frames[FRAME_CURRENT].cr, 77, sizeof(frames[FRAME_CURRENT].cr));
	frame_init(&frames[FRAME_A], ID_A, width_mbs, height_mbs);
	frame_pattern(&frames[FRAME_A], 1U);
	frame_init(&frames[FRAME_B], ID_B, width_mbs, height_mbs);
	frame_pattern(&frames[FRAME_B], 2U);
	frame_init(&frames[FRAME_C], ID_C, width_mbs, height_mbs);
	frame_pattern(&frames[FRAME_C], 3U);

	/* The header and the parameter sets. */
	header.slice_type = slice_type;
	header.num_ref_idx_active[0] = 1;
	header.num_ref_idx_active[1] = 1;
	header.qp = qp;
	sps.direct_8x8_inference = 1;

	/* The picture. */
	picture.frame = &frames[FRAME_CURRENT].frame;
	picture.sps = &sps;
	picture.pps = &pps;
	picture.mbs = mbs;
	picture.width_mbs = width_mbs;
	picture.height_mbs = height_mbs;
	picture.size_mbs = width_mbs * height_mbs;
	levelscale_flat();

	/* Every macroblock is not decoded yet; the current one is in slice 0. */
	for (index = 0; index < PIC_MBS; index++)
		h264sw_mb_reset(&mbs[index], -1);
	mbs[addr].slice = 0;
	mbs[addr].qp = (uint8_t)qp;

	/* The slice context. */
	ctx.picture = &picture;
	ctx.header = &header;
	ctx.slice_num = 0;
	ctx.qp = qp;
	ctx.mb_addr = addr;
	ctx.prev_mb_addr = -1;
	h264sw_weights_setup(&ctx.weights, &pps, &header, &ctx.lists, 0);
	neighbours_update();

	/* The syntax starts with every list unused and every level 0. */
	clear_levels();
	for (index = 0; index < 4U; index++) {
		ctx.syntax.ref_idx[0][index] = -1;
		ctx.syntax.ref_idx[1][index] = -1;
	}
}

/* Finds the neighbours of the current macroblock again, after a case has changed which macroblocks are available. */
static void
neighbours_update(void)
{
	/* The same derivation the decoder makes before each macroblock. */
	h264sw_mb_neighbours(mbs, pic_w, cur_addr, &ctx.n);
}

/* Puts frame frame_index in entry index of a reference list, with its PicOrderCnt, and counts the entry. */
static void
set_ref(
	unsigned list,
	unsigned index,
	unsigned frame_index,
	int poc)
{
	/* The entry names the frame and its order count. */
	ctx.lists.list[list][index].frame = &frames[frame_index].frame;
	ctx.lists.list[list][index].poc = poc;
	ctx.lists.list[list][index].missing = 0;
	ctx.lists.list[list][index].long_term = 0;

	/* The list holds at least up to this entry. */
	if (ctx.lists.count[list] < index + 1U)
		ctx.lists.count[list] = index + 1U;
}

/* Gives the macroblock the meaning of an mb_type of a slice type, in the syntax and in its entry. */
static void
set_type(
	unsigned slice_type,
	unsigned mb_type)
{
	int status;

	/* The table of the macroblock module gives the kind, the partitions and the prediction modes. */
	status = h264sw_mb_type_of(slice_type, mb_type, &ctx.syntax.type);
	CHECK(status == 0);
	ctx.syntax.mb_type = mb_type;
	mbs[cur_addr].kind = (uint8_t)ctx.syntax.type.kind;
}

/* Gives a sub-macroblock of a P_8x8 or B_8x8 macroblock the meaning of a sub_mb_type. */
static void
set_sub(
	unsigned slice_type,
	unsigned quadrant,
	unsigned sub_type)
{
	int status;

	/* Tables 7-17 and 7-18. */
	status = h264sw_sub_mb_type_of(slice_type, sub_type, &ctx.syntax.sub[quadrant]);
	CHECK(status == 0);
	ctx.syntax.sub_mb_type[quadrant] = sub_type;
}

/* Makes every level of the syntax 0, as the parser does before each macroblock. */
static void
clear_levels(void)
{
	/* No coefficient in any block. */
	memset(ctx.syntax.luma_dc, 0, sizeof(ctx.syntax.luma_dc));
	memset(ctx.syntax.luma, 0, sizeof(ctx.syntax.luma));
	memset(ctx.syntax.luma8x8, 0, sizeof(ctx.syntax.luma8x8));
	memset(ctx.syntax.chroma_dc, 0, sizeof(ctx.syntax.chroma_dc));
	memset(ctx.syntax.chroma_ac, 0, sizeof(ctx.syntax.chroma_ac));
}

/* Makes macroblock addr an available inter neighbour that moves all its blocks by (x, y) from reference index ref_idx of list 0. */
static void
neighbour_inter(
	unsigned addr,
	int ref_idx,
	int x,
	int y)
{
	unsigned blk;

	/* The neighbour is decoded, in the current slice, and inter. */
	mbs[addr].slice = 0;
	mbs[addr].kind = H264SW_MB_P;
	mbs[addr].motion.intra = 0;

	/* Its four quadrants use list 0, and its sixteen blocks carry the vector. */
	for (blk = 0; blk < 4U; blk++)
		mbs[addr].motion.ref_idx[0][blk] = (int8_t)ref_idx;
	for (blk = 0; blk < 16U; blk++) {
		mbs[addr].motion.mv[0][blk][0] = (int16_t)x;
		mbs[addr].motion.mv[0][blk][1] = (int16_t)y;
	}

	/* Recomputes the neighbours of the current macroblock from the motion just set. */
	neighbours_update();
}

/* Reads a luma sample of the current frame. */
static int
luma_at(
	unsigned x,
	unsigned y)
{
	/* Row y starts y strides after the plane. */
	return frames[FRAME_CURRENT].luma[y * LUMA_STRIDE + x];
}

/* Reads a Cb sample of the current frame. */
static int
cb_at(
	unsigned x,
	unsigned y)
{
	/* Row y starts y strides after the plane. */
	return frames[FRAME_CURRENT].cb[y * CHROMA_STRIDE + x];
}

/* Reads a Cr sample of the current frame. */
static int
cr_at(
	unsigned x,
	unsigned y)
{
	/* Row y starts y strides after the plane. */
	return frames[FRAME_CURRENT].cr[y * CHROMA_STRIDE + x];
}

/* Reads a luma sample of the current macroblock, by column and row inside it. */
static int
mb_luma(
	unsigned column,
	unsigned row)
{
	int sample;

	/* The macroblock's upper-left sample plus the offset. */
	sample = luma_at(16U * (cur_addr % pic_w) + column, 16U * (cur_addr / pic_w) + row);
	return sample;
}

/* Reads a Cb sample of the current macroblock. */
static int
mb_cb(
	unsigned column,
	unsigned row)
{
	int sample;

	/* The macroblock's upper-left chroma sample plus the offset. */
	sample = cb_at(8U * (cur_addr % pic_w) + column, 8U * (cur_addr / pic_w) + row);
	return sample;
}

/* Reads a Cr sample of the current macroblock. */
static int
mb_cr(
	unsigned column,
	unsigned row)
{
	int sample;

	/* The macroblock's upper-left chroma sample plus the offset. */
	sample = cr_at(8U * (cur_addr % pic_w) + column, 8U * (cur_addr / pic_w) + row);
	return sample;
}

/* Tells whether a rectangle of the current macroblock's luma is the given value everywhere. */
static int
luma_is(
	unsigned column,
	unsigned row,
	unsigned width,
	unsigned height,
	int value)
{
	unsigned x;
	unsigned y;
	int luma_actual;

	/* Looks for a sample that differs. */
	for (y = row; y < row + height; y++) {
		for (x = column; x < column + width; x++) {
			luma_actual = mb_luma(x, y);
			if (luma_actual != value)
				return 0;
		}
	}

	/* All samples equal. */
	return 1;
}

/* Tells whether a rectangle of the current macroblock's Cb (plane 1) or Cr (plane 2) is the given value everywhere. */
static int
chroma_is(
	unsigned plane,
	unsigned column,
	unsigned row,
	unsigned width,
	unsigned height,
	int value)
{
	unsigned x;
	unsigned y;
	int sample;

	/* Looks for a sample that differs. */
	for (y = row; y < row + height; y++) {
		for (x = column; x < column + width; x++) {
			sample = mb_cb(x, y);
			if (plane == 2U)
				sample = mb_cr(x, y);
			if (sample != value)
				return 0;
		}
	}

	/* All samples equal. */
	return 1;
}

/* Reads a luma sample of a reference frame with the position clamped into the picture (8-239, 8-240). */
static int
ref_luma(
	const struct test_frame *source,
	int x,
	int y)
{
	/* The columns and rows past the edge repeat the edge. */
	if (x < 0)
		x = 0;
	if (x > (int)source->frame.width - 1)
		x = (int)source->frame.width - 1;
	if (y < 0)
		y = 0;
	if (y > (int)source->frame.height - 1)
		y = (int)source->frame.height - 1;

	/* The sample at the clamped place. */
	return source->luma[y * (int)LUMA_STRIDE + x];
}

/* Reads a chroma sample of a reference frame with the position clamped into the picture (8-262 to 8-269). */
static int
ref_chroma(
	const struct test_frame *source,
	unsigned plane,
	int x,
	int y)
{
	int width;
	int height;

	/* The chroma plane is half the luma size each way. */
	width = (int)source->frame.width / 2;
	height = (int)source->frame.height / 2;
	if (x < 0)
		x = 0;
	if (x > width - 1)
		x = width - 1;
	if (y < 0)
		y = 0;
	if (y > height - 1)
		y = height - 1;

	/* The sample at the clamped place. */
	if (plane == 1U)
		return source->cb[y * (int)CHROMA_STRIDE + x];
	return source->cr[y * (int)CHROMA_STRIDE + x];
}

/* Forms the chroma sample at (x, y) displaced by a vector in eighth samples from the four nearest reference samples (8-270). */
static int
expect_chroma(
	const struct test_frame *source,
	unsigned plane,
	int x,
	int y,
	int mv_x,
	int mv_y)
{
	int whole_x;
	int whole_y;
	int frac_x;
	int frac_y;
	int a;
	int b;
	int c;
	int d;

	/* The whole part rounds down and the fraction is what is left of it. */
	whole_x = x + floor_div8(mv_x);
	whole_y = y + floor_div8(mv_y);
	frac_x = mv_x - 8 * floor_div8(mv_x);
	frac_y = mv_y - 8 * floor_div8(mv_y);

	/* The four samples A, B, C and D around the position. */
	a = ref_chroma(source, plane, whole_x, whole_y);
	b = ref_chroma(source, plane, whole_x + 1, whole_y);
	c = ref_chroma(source, plane, whole_x, whole_y + 1);
	d = ref_chroma(source, plane, whole_x + 1, whole_y + 1);

	/* The weighted sum with rounding. */
	return ((8 - frac_x) * (8 - frac_y) * a + frac_x * (8 - frac_y) * b + (8 - frac_x) * frac_y * c + frac_x * frac_y * d + 32) >> 6;
}

/* Tells whether block blk of list list of the current macroblock holds the vector (x, y). */
static int
mv_is(
	unsigned list,
	unsigned blk,
	int x,
	int y)
{
	/* Both components must equal. */
	if (mbs[cur_addr].motion.mv[list][blk][0] != x)
		return 0;
	if (mbs[cur_addr].motion.mv[list][blk][1] != y)
		return 0;

	/* The vector is the one asked for. */
	return 1;
}

/* Reconstructs the current macroblock and checks the status. */
static void
run_mb(
	int expected_status)
{
	int status;

	/* The function under test. */
	status = h264sw_recon_mb(&ctx);
	CHECK(status == expected_status);
}

/*
 * I_PCM: the 384 samples go to the three planes in raster order, the
 * neighbouring samples stay, the entry says intra with every block
 * coded.
 */
static void
check_pcm(void)
{
	unsigned index;
	unsigned x;
	unsigned y;
	int ok;
	int cb_actual;
	int cr_actual;
	int luma_actual;

	/* Starts a fresh decoding environment for this case. */
	env_make(H264SW_SLICE_I, PIC_W, PIC_H, ADDR, 26);
	set_type(H264SW_SLICE_I, 25U);
	for (index = 0; index < 384U; index++)
		ctx.syntax.pcm[index] = (uint8_t)((index * 7U + 3U) & 255U);
	run_mb(0);

	/* The luma samples. */
	ok = 1;
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			luma_actual = mb_luma(x, y);
			if (luma_actual != (int)((((y * 16U + x) * 7U) + 3U) & 255U))
				ok = 0;
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);

	/* The Cb samples follow the luma ones in the array, and the Cr samples the Cb ones. */
	ok = 1;
	for (y = 0; y < 8U; y++) {
		for (x = 0; x < 8U; x++) {
			cb_actual = mb_cb(x, y);
			if (cb_actual != (int)(((((256U + y * 8U + x)) * 7U) + 3U) & 255U))
				ok = 0;
			cr_actual = mb_cr(x, y);
			if (cr_actual != (int)(((((320U + y * 8U + x)) * 7U) + 3U) & 255U))
				ok = 0;
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);

	/* The samples around the macroblock are untouched. */
	CHECK(luma_at(15U, 16U) == 77);
	CHECK(luma_at(32U, 16U) == 77);
	CHECK(luma_at(16U, 15U) == 77);
	CHECK(luma_at(16U, 32U) == 77);
	CHECK(cb_at(7U, 8U) == 77 && cb_at(16U, 8U) == 77);
	CHECK(cr_at(8U, 7U) == 77 && cr_at(8U, 16U) == 77);

	/* The entry. */
	CHECK(mbs[ADDR].motion.intra == 1U);
	CHECK(mbs[ADDR].nonzero == 0xffffU);
	CHECK(mbs[ADDR].motion.ref_idx[0][0] == -1);
	CHECK(mbs[ADDR].intra4x4_mode[0] == 2U);
	CHECK(ctx.concealed == 0U);
}

/*
 * Intra_16x16 DC (mb_type 3) with no neighbours: every sample is 128
 * (8.3.3.3), and a DC level of 1 adds one value to every sample.  The
 * value follows 8-320 (the inverse transform of a single level 1 is 1
 * in every position), 8-321 and 8-322 and 8-354:
 * - QPY 28: m 4, LevelScale 16 * 16 = 256; ( 256 + 2 ) >> 2 = 64;
 *   ( 64 + 32 ) >> 6 = 1, so 129;
 * - QPY 36: m 0, LevelScale 16 * 10 = 160; 160 << 0 = 160;
 *   ( 160 + 32 ) >> 6 = 3, so 131.
 */
static void
check_i16_dc(void)
{
	/* No residual: a flat 128 in the three planes, and no coefficient anywhere. */
	env_make(H264SW_SLICE_I, PIC_W, PIC_H, ADDR, 26);
	set_type(H264SW_SLICE_I, 3U);
	CHECK(ctx.syntax.type.kind == H264SW_MB_I_16X16);
	CHECK(ctx.syntax.type.intra16x16_mode == H264SW_I16_DC);
	run_mb(0);
	CHECK(luma_is(0U, 0U, 16U, 16U, 128));
	CHECK(chroma_is(1U, 0U, 0U, 8U, 8U, 128));
	CHECK(chroma_is(2U, 0U, 0U, 8U, 8U, 128));
	CHECK(mbs[ADDR].nonzero == 0U);
	CHECK(mbs[ADDR].motion.intra == 1U);
	CHECK(ctx.concealed == 0U);

	/* A DC level of 1 at QPY 28. */
	env_make(H264SW_SLICE_I, PIC_W, PIC_H, ADDR, 28);
	set_type(H264SW_SLICE_I, 3U);
	ctx.syntax.luma_dc[0] = 1;
	run_mb(0);
	CHECK(luma_is(0U, 0U, 16U, 16U, 129));
	CHECK(mbs[ADDR].nonzero == 0xffffU);
	CHECK(chroma_is(1U, 0U, 0U, 8U, 8U, 128));

	/* The same level at QPY 36 uses the other branch of the scaling. */
	env_make(H264SW_SLICE_I, PIC_W, PIC_H, ADDR, 36);
	set_type(H264SW_SLICE_I, 3U);
	ctx.syntax.luma_dc[0] = 1;
	run_mb(0);
	CHECK(luma_is(0U, 0U, 16U, 16U, 131));

	/*
	 * A level at scan position 1 is c01, a horizontal frequency (8-320
	 * gives f = c01 * ( 1, 1, -1, -1 ) along each row): at QPY 28 a
	 * level 8 gives dcY = ( 8 * 256 + 2 ) >> 2 = 512 and ( 512 + 32 ) >> 6
	 * = 8 in the left half (columns 0 to 7), and -512 and
	 * ( -512 + 32 ) >> 6 = -8 in the right half: 136 and 120.  The
	 * blocks are told apart by their place, not their index.
	 */
	env_make(H264SW_SLICE_I, PIC_W, PIC_H, ADDR, 28);
	set_type(H264SW_SLICE_I, 3U);
	ctx.syntax.luma_dc[1] = 8;
	run_mb(0);
	CHECK(luma_is(0U, 0U, 8U, 16U, 136));
	CHECK(luma_is(8U, 0U, 8U, 16U, 120));

	/* A negative level subtracts: at QPY 28, -1 gives ( -64 + 32 ) >> 6 = -1, so 127. */
	env_make(H264SW_SLICE_I, PIC_W, PIC_H, ADDR, 28);
	set_type(H264SW_SLICE_I, 3U);
	ctx.syntax.luma_dc[0] = -1;
	run_mb(0);
	CHECK(luma_is(0U, 0U, 16U, 16U, 127));
}

/*
 * An AC level of Intra_16x16: block 5 (luma4x4BlkIdx, at column 12, row
 * 0) has a level 1 at scan position 1, which is c01 (the zig-zag scan of
 * Table 8-13), a horizontal frequency: the rows of the block are equal
 * and the columns differ, and every other block stays 128.
 */
static void
check_i16_ac(void)
{
	unsigned x;
	unsigned y;
	int rows_equal;
	int varies;
	int luma_actual;
	int luma_other;

	/* Starts a fresh decoding environment for this case. */
	env_make(H264SW_SLICE_I, PIC_W, PIC_H, ADDR, 28);
	set_type(H264SW_SLICE_I, 3U);
	ctx.syntax.luma[5][1] = 20;
	ctx.syntax.cbp_luma = 15U;
	run_mb(0);

	/* The block varies along a row and is the same on every row. */
	rows_equal = 1;
	varies = 0;
	for (y = 0; y < 4U; y++) {
		for (x = 0; x < 4U; x++) {
			luma_actual = mb_luma(12U + x, y);
			luma_other = mb_luma(12U + x, 0U);
			if (luma_actual != luma_other)
				rows_equal = 0;
			luma_actual = mb_luma(12U + x, y);
			luma_other = mb_luma(12U, 0U);
			if (luma_actual != luma_other)
				varies = 1;
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(rows_equal);
	CHECK(varies);

	/* The rest of the macroblock is flat, and only block 5 counts as coded. */
	CHECK(luma_is(0U, 0U, 12U, 16U, 128));
	CHECK(luma_is(12U, 4U, 4U, 12U, 128));
	CHECK(mbs[ADDR].nonzero == (1U << 5));
}

/*
 * Runs an Intra_16x16 DC macroblock whose chroma DC levels are both
 * level, at QPY qp with the two chroma offsets, and checks Cb and Cr
 * against QP'C from Table 8-15: qPI = Clip3( 0, 51, QPY + offset ), QPC
 * = qPI below 30, else the table, and each of the four chroma blocks gets
 * dcC = ( ( level * 16 * v ) << ( QPC / 6 ) ) >> 5 (8-324, 8-326, with
 * f equal to level in all four positions) and a residual ( dcC + 32 ) >> 6.
 */
static void
chroma_qp_case(
	int qp,
	int offset_cb,
	int offset_cr,
	int level)
{
	int qpi;
	int qpc;
	int dc;
	int expected_cb;
	int expected_cr;

	/* Starts a fresh decoding environment for this case. */
	env_make(H264SW_SLICE_I, PIC_W, PIC_H, ADDR, qp);
	picture.chroma_qp_offset[0] = offset_cb;
	picture.chroma_qp_offset[1] = offset_cr;
	set_type(H264SW_SLICE_I, 3U);
	ctx.syntax.cbp_chroma = 1U;
	ctx.syntax.chroma_dc[0][0] = level;
	ctx.syntax.chroma_dc[1][0] = level;
	run_mb(0);

	/* Cb with chroma_qp_index_offset. */
	qpi = qp + offset_cb;
	if (qpi < 0)
		qpi = 0;
	if (qpi > 51)
		qpi = 51;
	qpc = qpi;
	if (qpi >= 30)
		qpc = qpc_table[qpi - 30];
	dc = (level * 16 * norm4x4_dc[qpc % 6] * (1 << (qpc / 6))) / 32;
	expected_cb = clamp255(128 + ((dc + 32) >> 6));
	CHECK(chroma_is(1U, 0U, 0U, 8U, 8U, expected_cb));

	/* Cr with second_chroma_qp_index_offset. */
	qpi = qp + offset_cr;
	if (qpi < 0)
		qpi = 0;
	if (qpi > 51)
		qpi = 51;
	qpc = qpi;
	if (qpi >= 30)
		qpc = qpc_table[qpi - 30];
	dc = (level * 16 * norm4x4_dc[qpc % 6] * (1 << (qpc / 6))) / 32;
	expected_cr = clamp255(128 + ((dc + 32) >> 6));
	CHECK(chroma_is(2U, 0U, 0U, 8U, 8U, expected_cr));
}

/*
 * QP'C: Cb and Cr use their own offsets.  Worked by hand:
 * - QPY 30, offsets +5 (Cb) and -6 (Cr): Cb qPI 35 gives QPC 33 (m 3, v
 *   14: 224; ( 224 << 5 ) >> 5 = 224; ( 224 + 32 ) >> 6 = 4, so 132), Cr
 *   qPI 24 gives QPC 24 (m 0, v 10: 160; ( 160 << 4 ) >> 5 = 80;
 *   ( 80 + 32 ) >> 6 = 1, so 129).
 */
static void
check_chroma_qp(void)
{
	/* The worked case, with literal values. */
	env_make(H264SW_SLICE_I, PIC_W, PIC_H, ADDR, 30);
	picture.chroma_qp_offset[0] = 5;
	picture.chroma_qp_offset[1] = -6;
	set_type(H264SW_SLICE_I, 3U);
	ctx.syntax.cbp_chroma = 1U;
	ctx.syntax.chroma_dc[0][0] = 1;
	ctx.syntax.chroma_dc[1][0] = 1;
	run_mb(0);
	CHECK(chroma_is(1U, 0U, 0U, 8U, 8U, 132));
	CHECK(chroma_is(2U, 0U, 0U, 8U, 8U, 129));

	/*
	 * The chroma DC levels are the 2x2 matrix c00, c01, c10, c11 in
	 * this order (8-304): a level 10 in the second position is c01, f =
	 * c01 * ( 1, -1 ) along each row (8-324), so the blocks of the left
	 * column get +dcC and the right column -dcC.  At QPC 28 (m 4, v 16):
	 * dcC = ( ( 10 * 256 ) << 4 ) >> 5 = 1280 and ( 1280 + 32 ) >> 6 = 20
	 * for the left, ( ( -2560 << 4 ) >> 5 ) = -1280 and ( -1280 + 32 ) >>
	 * 6 = -20 for the right: 148 and 108.  Cr has its level in the
	 * third position (c10: f = c10 * ( 1, 1 ) in the first row and
	 * c10 * ( -1, -1 ) in the second): the upper blocks +20, the lower -20.
	 */
	env_make(H264SW_SLICE_I, PIC_W, PIC_H, ADDR, 28);
	set_type(H264SW_SLICE_I, 3U);
	ctx.syntax.cbp_chroma = 1U;
	ctx.syntax.chroma_dc[0][1] = 10;
	ctx.syntax.chroma_dc[1][2] = 10;
	run_mb(0);
	CHECK(chroma_is(1U, 0U, 0U, 4U, 8U, 148));
	CHECK(chroma_is(1U, 4U, 0U, 4U, 8U, 108));
	CHECK(chroma_is(2U, 0U, 0U, 8U, 4U, 148));
	CHECK(chroma_is(2U, 0U, 4U, 8U, 4U, 108));

	/* The same through the formulas, over the range of qPI including the clips at 0 and 51 and the table's steps. */
	chroma_qp_case(30, 5, -6, 1);
	chroma_qp_case(51, 12, 12, 1);
	chroma_qp_case(5, -12, -12, 20);
	chroma_qp_case(40, 0, 12, 3);
	chroma_qp_case(40, -12, 0, 3);
	chroma_qp_case(29, 1, 0, 2);
	chroma_qp_case(29, 0, 1, 2);
	chroma_qp_case(38, 0, 0, 2);
	chroma_qp_case(51, 0, -3, 1);
	chroma_qp_case(0, 12, 0, 30);
	chroma_qp_case(22, 12, 11, 4);
	chroma_qp_case(45, 3, 4, 1);
}

/*
 * The order of the sixteen 4x4 blocks of Intra_4x4: block 0 is DC with
 * a level 1 at QPY 28 ( 256 << 0 = 256; ( 256 + 32 ) >> 6 = 4, so 132),
 * and every later block predicts from the reconstructed samples of the
 * blocks before it, so the whole macroblock becomes 132: block 1
 * (Horizontal) copies block 0's right column, block 2 (Vertical) its
 * bottom row, block 3 (DC) the mean of both, and so on.  A
 * reconstruction that predicted all blocks before adding any residual
 * would leave them 128.
 */
static void
check_i4x4_order(void)
{
	static const unsigned modes[16] = { 2, 1, 0, 2, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1 };
	unsigned blk;
	unsigned x;
	unsigned y;
	unsigned pred;
	unsigned mode_a;
	unsigned mode_b;
	int ok;

	/* Starts a fresh decoding environment for this case. */
	env_make(H264SW_SLICE_I, PIC_W, PIC_H, ADDR, 28);
	set_type(H264SW_SLICE_I, 0U);
	ctx.syntax.type.kind = H264SW_MB_I_4X4;
	mbs[ADDR].kind = H264SW_MB_I_4X4;

	/* Codes each mode against the mode the neighbours predict: Min( A, B ), or DC when A or B is out of the macroblock (nothing is available outside it here). */
	for (blk = 0; blk < 16U; blk++) {
		x = h264sw_blk4x4_x[blk];
		y = h264sw_blk4x4_y[blk];
		pred = 2U;
		if (x > 0U && y > 0U) {
			mode_a = modes[h264sw_blk4x4_at(x - 4U, y)];
			mode_b = modes[h264sw_blk4x4_at(x, y - 4U)];
			pred = mode_a;
			if (mode_b < pred)
				pred = mode_b;
		}

		/* Codes the mode as the predicted-mode flag when it equals the prediction, else as the remaining mode. */
		if (modes[blk] == pred) {
			ctx.syntax.prev_intra_flag[blk] = 1;
		} else {
			ctx.syntax.prev_intra_flag[blk] = 0;
			if (modes[blk] < pred)
				ctx.syntax.rem_intra_mode[blk] = (uint8_t)modes[blk];
			else
				ctx.syntax.rem_intra_mode[blk] = (uint8_t)(modes[blk] - 1U);
		}
	}

	/* Marks the luma block 0 as having a coefficient and reconstructs the macroblock. */
	ctx.syntax.luma[0][0] = 1;
	run_mb(0);

	/* Every luma sample is 132. */
	CHECK(luma_is(0U, 0U, 16U, 16U, 132));

	/* The modes are kept for the macroblocks after this one, and only block 0 is coded. */
	ok = 1;
	for (blk = 0; blk < 16U; blk++) {
		if (mbs[ADDR].intra4x4_mode[blk] != modes[blk])
			ok = 0;
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);
	CHECK(mbs[ADDR].nonzero == 1U);

	/* The chroma has no neighbours either: DC, 128. */
	CHECK(chroma_is(1U, 0U, 0U, 8U, 8U, 128));
	CHECK(chroma_is(2U, 0U, 0U, 8U, 8U, 128));
	CHECK(ctx.concealed == 0U);
}

/*
 * Intra_8x8 with DC in the four blocks: a level 1 in the first block at
 * QPY 28 gives LevelScale8x8 = 16 * 32 = 512 (8-317, v of 8-318 for m 4
 * is 32), dij = ( 512 + 2 ) >> 2 = 128 (8.5.13.1), the DC-only inverse
 * transform gives 128 in every position and ( 128 + 32 ) >> 6 = 2: the
 * first block is 130, and the three blocks after it predict from it with
 * DC and stay 130.  The four 4x4 bits of block 0 are set.
 */
static void
check_i8x8(void)
{
	unsigned blk;
	unsigned part;
	int ok;

	/* Starts a fresh decoding environment for this case. */
	env_make(H264SW_SLICE_I, PIC_W, PIC_H, ADDR, 28);
	set_type(H264SW_SLICE_I, 0U);
	ctx.syntax.type.kind = H264SW_MB_I_8X8;
	mbs[ADDR].kind = H264SW_MB_I_8X8;
	mbs[ADDR].transform_8x8 = 1;
	for (blk = 0; blk < 4U; blk++)
		ctx.syntax.prev_intra_flag[blk] = 1;
	ctx.syntax.luma8x8[0][0] = 1;
	run_mb(0);

	/* Requires the prediction value 130 everywhere and the first 8x8 block marked as having coefficients. */
	CHECK(luma_is(0U, 0U, 16U, 16U, 130));
	CHECK(mbs[ADDR].nonzero == 0xfU);

	/* Each 8x8 block's mode is in its four 4x4 entries. */
	ok = 1;
	for (blk = 0; blk < 16U; blk++) {
		part = mbs[ADDR].intra4x4_mode[blk];
		if (part != 2U)
			ok = 0;
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);
	CHECK(chroma_is(1U, 0U, 0U, 8U, 8U, 128));
	CHECK(ctx.concealed == 0U);
}

/*
 * A mode that needs samples that are not available (a broken stream):
 * the macroblock still gets samples (128) and counts once as concealed,
 * however many of its blocks and planes fell back; the status is 0.
 */
static void
check_intra_unavailable(void)
{
	/* Intra_16x16 Vertical (mb_type 1) with no neighbours, and chroma Vertical (2) too. */
	env_make(H264SW_SLICE_I, PIC_W, PIC_H, ADDR, 26);
	set_type(H264SW_SLICE_I, 1U);
	CHECK(ctx.syntax.type.intra16x16_mode == H264SW_I16_VERTICAL);
	mbs[ADDR].chroma_pred_mode = H264SW_ICHROMA_VERTICAL;
	run_mb(0);
	CHECK(luma_is(0U, 0U, 16U, 16U, 128));
	CHECK(chroma_is(1U, 0U, 0U, 8U, 8U, 128));
	CHECK(chroma_is(2U, 0U, 0U, 8U, 8U, 128));
	CHECK(ctx.concealed == 1U);

	/* An Intra_4x4 macroblock whose first block is Diagonal_Down_Right (needs the left, top and top-left) counts once as well. */
	env_make(H264SW_SLICE_I, PIC_W, PIC_H, ADDR, 26);
	set_type(H264SW_SLICE_I, 0U);
	ctx.syntax.type.kind = H264SW_MB_I_4X4;
	mbs[ADDR].kind = H264SW_MB_I_4X4;
	ctx.syntax.prev_intra_flag[0] = 0;
	ctx.syntax.rem_intra_mode[0] = 3;
	run_mb(0);
	CHECK(mbs[ADDR].intra4x4_mode[0] == 4U);
	CHECK(luma_is(0U, 0U, 4U, 4U, 128));
	CHECK(ctx.concealed == 1U);
}

/*
 * The scaling list follows the macroblock's intra or inter mode and
 * the component (Table 7-2, 8.5.9): with Intra Y 16 and Inter Y 32
 * (weights; flat is 16) the same level 1 at QPY 28 gives 4 for intra
 * and 8 for inter in a 4x4 block ( LevelScale 32 * 16 = 512, 512 << 0;
 * ( 512 + 32 ) >> 6 = 8); the 8x8 lists likewise (inter 32: LevelScale
 * 32 * 32 = 1024, ( 1024 + 2 ) >> 2 = 256; ( 256 + 32 ) >> 6 = 4).  The
 * chroma lists are Intra Cb 16, Intra Cr 32, Inter Cb 32, Inter Cr 16;
 * a chroma DC level 1 at QPC 28 (m 4, v 16): weight 16 gives 256,
 * ( 256 << 4 ) >> 5 = 128, ( 128 + 32 ) >> 6 = 2; weight 32 gives 512,
 * 256, ( 256 + 32 ) >> 6 = 4.
 */
static void
check_scaling_lists(void)
{
	struct h264sw_scaling scaling;

	/* The lists: Intra Y 16, Intra Cb 16, Intra Cr 32, Inter Y 32, Inter Cb 32, Inter Cr 16; the 8x8 lists Intra 16 and Inter 32. */
	memset(&scaling, 16, sizeof(scaling));
	memset(scaling.list4x4[2], 32, sizeof(scaling.list4x4[2]));
	memset(scaling.list4x4[3], 32, sizeof(scaling.list4x4[3]));
	memset(scaling.list4x4[4], 32, sizeof(scaling.list4x4[4]));
	memset(scaling.list8x8[1], 32, sizeof(scaling.list8x8[1]));

	/* An intra macroblock: the chroma follows the intra lists, Cb 128 + 2 and Cr 128 + 4. */
	env_make(H264SW_SLICE_I, PIC_W, PIC_H, ADDR, 28);
	h264sw_levelscale_init(&picture.levelscale, &scaling);
	set_type(H264SW_SLICE_I, 3U);
	ctx.syntax.chroma_dc[0][0] = 1;
	ctx.syntax.chroma_dc[1][0] = 1;
	run_mb(0);
	CHECK(chroma_is(1U, 0U, 0U, 8U, 8U, 130));
	CHECK(chroma_is(2U, 0U, 0U, 8U, 8U, 132));

	/* An intra 4x4 block uses the Intra Y list: 128 + 4. */
	env_make(H264SW_SLICE_I, PIC_W, PIC_H, ADDR, 28);
	h264sw_levelscale_init(&picture.levelscale, &scaling);
	set_type(H264SW_SLICE_I, 0U);
	ctx.syntax.type.kind = H264SW_MB_I_4X4;
	mbs[ADDR].kind = H264SW_MB_I_4X4;
	ctx.syntax.prev_intra_flag[0] = 1;
	ctx.syntax.luma[0][0] = 1;
	run_mb(0);
	CHECK(luma_is(0U, 0U, 4U, 4U, 132));

	/* An inter macroblock (P_L0_16x16, mv 0, flat reference): the luma 4x4 block is +8; the chroma lists are the inter ones, Cb +4 and Cr +2. */
	env_make(H264SW_SLICE_P, PIC_W, PIC_H, ADDR, 28);
	h264sw_levelscale_init(&picture.levelscale, &scaling);
	memset(frames[FRAME_A].luma, 100, sizeof(frames[FRAME_A].luma));
	memset(frames[FRAME_A].cb, 100, sizeof(frames[FRAME_A].cb));
	memset(frames[FRAME_A].cr, 100, sizeof(frames[FRAME_A].cr));
	set_ref(0, 0, FRAME_A, 0);
	set_type(H264SW_SLICE_P, 0U);
	ctx.syntax.ref_idx[0][0] = 0;
	ctx.syntax.luma[0][0] = 1;
	ctx.syntax.chroma_dc[0][0] = 1;
	ctx.syntax.chroma_dc[1][0] = 1;
	run_mb(0);
	CHECK(luma_is(0U, 0U, 4U, 4U, 108));
	CHECK(luma_is(4U, 0U, 12U, 16U, 100));
	CHECK(luma_is(0U, 4U, 4U, 12U, 100));
	CHECK(chroma_is(1U, 0U, 0U, 8U, 8U, 104));
	CHECK(chroma_is(2U, 0U, 0U, 8U, 8U, 102));

	/* The 8x8 transform of an inter macroblock uses the inter 8x8 list: the second quadrant is 100 + 4. */
	env_make(H264SW_SLICE_P, PIC_W, PIC_H, ADDR, 28);
	h264sw_levelscale_init(&picture.levelscale, &scaling);
	memset(frames[FRAME_A].luma, 100, sizeof(frames[FRAME_A].luma));
	set_ref(0, 0, FRAME_A, 0);
	set_type(H264SW_SLICE_P, 0U);
	ctx.syntax.ref_idx[0][0] = 0;
	mbs[ADDR].transform_8x8 = 1;
	ctx.syntax.luma8x8[1][0] = 1;
	run_mb(0);
	CHECK(luma_is(8U, 0U, 8U, 8U, 104));
	CHECK(luma_is(0U, 0U, 8U, 16U, 100));
	CHECK(luma_is(8U, 8U, 8U, 8U, 100));
	CHECK(mbs[ADDR].nonzero == 0xf0U);
}

/*
 * P_L0_16x16 with an integer vector: (16, 0) in quarter samples is 4
 * samples to the right, the chroma 2 samples; the macroblock at the
 * right edge clamps its positions.  A residual level is added on the
 * prediction (level 1 at QPY 28: +4).
 */
static void
check_p16x16(void)
{
	unsigned x;
	unsigned y;
	int ok_luma;
	int ok_cb;
	int ok_cr;
	unsigned addr;
	int cb_actual;
	int chroma_reference;
	int clamped_sample;
	int cr_actual;
	int luma_actual;
	int luma_reference;

	/* The middle macroblock and the right edge one, whose shifted positions pass the edge of the 48 sample wide picture. */
	for (addr = ADDR; addr <= 5U; addr++) {
		env_make(H264SW_SLICE_P, PIC_W, PIC_H, addr, 26);
		set_ref(0, 0, FRAME_A, 0);
		set_type(H264SW_SLICE_P, 0U);
		CHECK(ctx.syntax.type.kind == H264SW_MB_P);
		ctx.syntax.ref_idx[0][0] = 0;
		ctx.syntax.mvd[0][0][0][0] = 16;
		ctx.syntax.mvd[0][0][0][1] = 0;
		run_mb(0);

		/* Each sample is the reference sample 4 to the right (2 for chroma), clamped. */
		ok_luma = 1;
		ok_cb = 1;
		ok_cr = 1;
		for (y = 0; y < 16U; y++) {
			for (x = 0; x < 16U; x++) {
				luma_actual = mb_luma(x, y);
				luma_reference = ref_luma(&frames[FRAME_A], (int)(16U * (addr % PIC_W) + x) + 4, (int)(16U * (addr / PIC_W) + y));
				if (luma_actual != luma_reference)
					ok_luma = 0;
			}
		}

		/* Compares the samples of each row of the block. */
		for (y = 0; y < 8U; y++) {
			for (x = 0; x < 8U; x++) {
				cb_actual = mb_cb(x, y);
				chroma_reference = ref_chroma(&frames[FRAME_A], 1U, (int)(8U * (addr % PIC_W) + x) + 2, (int)(8U * (addr / PIC_W) + y));
				if (cb_actual != chroma_reference)
					ok_cb = 0;
				cr_actual = mb_cr(x, y);
				chroma_reference = ref_chroma(&frames[FRAME_A], 2U, (int)(8U * (addr % PIC_W) + x) + 2, (int)(8U * (addr / PIC_W) + y));
				if (cr_actual != chroma_reference)
					ok_cr = 0;
			}
		}

		/* Requires that every value compared above matched. */
		CHECK(ok_luma);
		CHECK(ok_cb);
		CHECK(ok_cr);

		/* The motion: every block, the reference index and the id of the frame it named, list 1 unused, not intra. */
		CHECK(mv_is(0, 0, 16, 0) && mv_is(0, 7, 16, 0) && mv_is(0, 15, 16, 0));
		CHECK(mv_is(1, 0, 0, 0) && mv_is(1, 15, 0, 0));
		CHECK(mbs[addr].motion.ref_idx[0][0] == 0 && mbs[addr].motion.ref_idx[0][3] == 0);
		CHECK(mbs[addr].motion.ref_id[0][0] == ID_A && mbs[addr].motion.ref_id[0][3] == ID_A);
		CHECK(mbs[addr].motion.ref_idx[1][0] == -1 && mbs[addr].motion.ref_idx[1][3] == -1);
		CHECK(mbs[addr].motion.intra == 0U);
		CHECK(ctx.decoded_4x4 == 0xffffU);
		CHECK(mbs[addr].nonzero == 0U);
	}

	/* With a level 1 in block 0 at QPY 28, that block is 4 above the prediction and the rest equals it. */
	env_make(H264SW_SLICE_P, PIC_W, PIC_H, ADDR, 28);
	set_ref(0, 0, FRAME_A, 0);
	set_type(H264SW_SLICE_P, 0U);
	ctx.syntax.ref_idx[0][0] = 0;
	ctx.syntax.luma[0][0] = 1;
	run_mb(0);
	ok_luma = 1;
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			luma_actual = mb_luma(x, y);
			luma_reference = ref_luma(&frames[FRAME_A], (int)(16U + x), (int)(16U + y));
			if (x < 4U && y < 4U) {
				clamped_sample = clamp255(luma_reference + 4);
				if (luma_actual != clamped_sample)
					ok_luma = 0;
			} else if (luma_actual != luma_reference) {
				ok_luma = 0;
			}
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok_luma);
	CHECK(mbs[ADDR].nonzero == 1U);
}

/*
 * Fractional vectors, including negative ones (rounding down): a
 * half-sample horizontal vector is the six-tap filter of 8-241 ( E - 5 F
 * + 20 G + 20 H - 5 I + J + 16 ) >> 5 clipped, between G and H; the
 * chroma is the bilinear sum of 8-270.  mv ( 2, 0 ) is half a sample to
 * the right; ( -6, 0 ) is a sample and a half to the left, i.e. the
 * half position after the sample two to the left.
 */
static void
check_p_fraction(void)
{
	static const int vectors[2] = { 2, -6 };
	static const int whole[2] = { 0, -2 };
	unsigned which;
	unsigned x;
	unsigned y;
	int base;
	int sum;
	int expected;
	int ok_luma;
	int ok_cb;
	int cb_actual;
	int chroma_expected;
	int cr_actual;
	int luma_actual;

	/* Runs the case twice: the second time with the weights set explicitly. */
	for (which = 0; which < 2U; which++) {
		env_make(H264SW_SLICE_P, PIC_W, PIC_H, ADDR, 26);
		set_ref(0, 0, FRAME_A, 0);
		set_type(H264SW_SLICE_P, 0U);
		ctx.syntax.ref_idx[0][0] = 0;
		ctx.syntax.mvd[0][0][0][0] = (int16_t)vectors[which];
		run_mb(0);

		/* The whole sample part rounds down: 2 >> 2 is 0 and -6 >> 2 is -2, with the half fraction. */
		ok_luma = 1;
		ok_cb = 1;
		for (y = 0; y < 16U; y++) {
			for (x = 0; x < 16U; x++) {
				base = (int)(16U + x) + whole[which];
				sum = ref_luma(&frames[FRAME_A], base - 2, (int)(16U + y)) - 5 * ref_luma(&frames[FRAME_A], base - 1, (int)(16U + y)) + 20 * ref_luma(&frames[FRAME_A], base, (int)(16U + y)) + 20 * ref_luma(&frames[FRAME_A], base + 1, (int)(16U + y)) - 5 * ref_luma(&frames[FRAME_A], base + 2, (int)(16U + y)) + ref_luma(&frames[FRAME_A], base + 3, (int)(16U + y));
				expected = clamp255((sum + 16) >> 5);
				luma_actual = mb_luma(x, y);
				if (luma_actual != expected)
					ok_luma = 0;
			}
		}

		/* Compares the samples of each row of the block. */
		for (y = 0; y < 8U; y++) {
			for (x = 0; x < 8U; x++) {
				cb_actual = mb_cb(x, y);
				chroma_expected = expect_chroma(&frames[FRAME_A], 1U, (int)(8U + x), (int)(8U + y), vectors[which], 0);
				if (cb_actual != chroma_expected)
					ok_cb = 0;
				cr_actual = mb_cr(x, y);
				chroma_expected = expect_chroma(&frames[FRAME_A], 2U, (int)(8U + x), (int)(8U + y), vectors[which], 0);
				if (cr_actual != chroma_expected)
					ok_cb = 0;
			}
		}

		/* Requires that every value compared above matched. */
		CHECK(ok_luma);
		CHECK(ok_cb);
	}
}

/*
 * Explicit weights (8.4.2.3.2) of a P macroblock: luma logWD 1, weight 3,
 * offset 2: Clip1( ( ( p * 3 + 1 ) >> 1 ) + 2 ); Cb logWD 2, weight 5,
 * offset 1: Clip1( ( ( p * 5 + 2 ) >> 2 ) + 1 ); Cr weight 6, offset
 * -1: Clip1( ( ( p * 6 + 2 ) >> 2 ) - 1 ).
 */
static void
check_p_weighted(void)
{
	unsigned x;
	unsigned y;
	int p;
	int ok_luma;
	int ok_cb;
	int ok_cr;
	int cb_actual;
	int clamped_sample;
	int cr_actual;
	int luma_actual;

	/* Starts a fresh decoding environment for this case. */
	env_make(H264SW_SLICE_P, PIC_W, PIC_H, ADDR, 26);
	set_ref(0, 0, FRAME_A, 0);
	pps.weighted_pred = 1;
	header.weighted = 1;
	header.luma_log2_denom = 1;
	header.chroma_log2_denom = 2;
	header.weight[0][0].luma_weight = 3;
	header.weight[0][0].luma_offset = 2;
	header.weight[0][0].chroma_weight[0] = 5;
	header.weight[0][0].chroma_offset[0] = 1;
	header.weight[0][0].chroma_weight[1] = 6;
	header.weight[0][0].chroma_offset[1] = -1;
	h264sw_weights_setup(&ctx.weights, &pps, &header, &ctx.lists, 0);
	set_type(H264SW_SLICE_P, 0U);
	ctx.syntax.ref_idx[0][0] = 0;
	run_mb(0);

	/* Reconstructs the macroblock, then compares every luma and chroma sample with the weighted prediction. */
	ok_luma = 1;
	ok_cb = 1;
	ok_cr = 1;
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			p = ref_luma(&frames[FRAME_A], (int)(16U + x), (int)(16U + y));
			luma_actual = mb_luma(x, y);
			clamped_sample = clamp255(((p * 3 + 1) >> 1) + 2);
			if (luma_actual != clamped_sample)
				ok_luma = 0;
		}
	}

	/* Compares the samples of each row of the block. */
	for (y = 0; y < 8U; y++) {
		for (x = 0; x < 8U; x++) {
			p = ref_chroma(&frames[FRAME_A], 1U, (int)(8U + x), (int)(8U + y));
			cb_actual = mb_cb(x, y);
			clamped_sample = clamp255(((p * 5 + 2) >> 2) + 1);
			if (cb_actual != clamped_sample)
				ok_cb = 0;
			p = ref_chroma(&frames[FRAME_A], 2U, (int)(8U + x), (int)(8U + y));
			cr_actual = mb_cr(x, y);
			clamped_sample = clamp255(((p * 6 + 2) >> 2) - 1);
			if (cr_actual != clamped_sample)
				ok_cr = 0;
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok_luma);
	CHECK(ok_cb);
	CHECK(ok_cr);
}

/*
 * P_Skip (8.4.1.1): with mbAddrA and mbAddrB available, nonzero
 * vectors and reference index 0, the vector is the median prediction:
 * A at address 3, B at address 1 and C at address 2 all (8, 4) give
 * (8, 4).  With mbAddrB unavailable the vector is 0.  The reference
 * index is 0 and its frame id is that of RefPicList0[ 0 ]; list 1 stays
 * unused.
 */
static void
check_pskip(void)
{
	unsigned x;
	unsigned y;
	unsigned blk;
	int ok;
	int cb_actual;
	int chroma_expected;
	int luma_actual;
	int luma_reference;
	int vector_matches;

	/* The neighbours A (3), B (1) and C (2) all move by (8, 4) from index 0. */
	env_make(H264SW_SLICE_P, PIC_W, PIC_H, ADDR, 26);
	set_ref(0, 0, FRAME_A, 0);
	h264sw_mb_skip_type(H264SW_SLICE_P, &ctx.syntax.type);
	mbs[ADDR].kind = (uint8_t)ctx.syntax.type.kind;
	neighbour_inter(3U, 0, 8, 4);
	neighbour_inter(1U, 0, 8, 4);
	neighbour_inter(2U, 0, 8, 4);
	run_mb(0);
	ok = 1;
	for (blk = 0; blk < 16U; blk++) {
		vector_matches = mv_is(0, blk, 8, 4);
		if (!vector_matches)
			ok = 0;
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);
	CHECK(mbs[ADDR].motion.ref_idx[0][0] == 0 && mbs[ADDR].motion.ref_idx[0][2] == 0);
	CHECK(mbs[ADDR].motion.ref_id[0][1] == ID_A);
	CHECK(mbs[ADDR].motion.ref_idx[1][0] == -1);

	/* The prediction is the reference moved by 2 samples right and 1 down; the chroma by one and a half samples right and a half down. */
	ok = 1;
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			luma_actual = mb_luma(x, y);
			luma_reference = ref_luma(&frames[FRAME_A], (int)(16U + x) + 2, (int)(16U + y) + 1);
			if (luma_actual != luma_reference)
				ok = 0;
		}
	}

	/* Compares the samples of each row of the block. */
	for (y = 0; y < 8U; y++) {
		for (x = 0; x < 8U; x++) {
			cb_actual = mb_cb(x, y);
			chroma_expected = expect_chroma(&frames[FRAME_A], 1U, (int)(8U + x), (int)(8U + y), 8, 4);
			if (cb_actual != chroma_expected)
				ok = 0;
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);

	/* Without B (address 1) the vector is 0 and the macroblock copies the reference. */
	env_make(H264SW_SLICE_P, PIC_W, PIC_H, ADDR, 26);
	set_ref(0, 0, FRAME_A, 0);
	h264sw_mb_skip_type(H264SW_SLICE_P, &ctx.syntax.type);
	mbs[ADDR].kind = (uint8_t)ctx.syntax.type.kind;
	neighbour_inter(3U, 0, 8, 4);
	run_mb(0);
	CHECK(mv_is(0, 0, 0, 0) && mv_is(0, 15, 0, 0));
	ok = 1;
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			luma_actual = mb_luma(x, y);
			luma_reference = ref_luma(&frames[FRAME_A], (int)(16U + x), (int)(16U + y));
			if (luma_actual != luma_reference)
				ok = 0;
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);
}

/*
 * Two macroblock partitions.  P_L0_L0_16x8: the upper vector is the
 * difference (the prediction is 0 with nothing available): (4, -8),
 * the lower partition predicts from B, the upper one, as only B has its
 * reference index (8.4.1.3.1): mvd ( 0, 4 ) gives (4, -4).  P_L0_L0_8x16:
 * the right partition's neighbour C is not available and D is not either,
 * so A (the left partition) stands in for B and C: mvd (4, 4) on
 * (8, 0) gives (12, 4).  Integer vectors move the luma by whole samples.
 */
static void
check_p_parts(void)
{
	unsigned x;
	unsigned y;
	unsigned blk;
	int ok;
	int cb_actual;
	int chroma_expected;
	int luma_actual;
	int luma_reference;
	int vector_matches;

	/* 16x8. */
	env_make(H264SW_SLICE_P, PIC_W, PIC_H, ADDR, 26);
	set_ref(0, 0, FRAME_A, 0);
	set_type(H264SW_SLICE_P, 1U);
	CHECK(ctx.syntax.type.parts == 2U && ctx.syntax.type.part_width == 16U && ctx.syntax.type.part_height == 8U);
	ctx.syntax.ref_idx[0][0] = 0;
	ctx.syntax.ref_idx[0][1] = 0;
	ctx.syntax.mvd[0][0][0][0] = 4;
	ctx.syntax.mvd[0][0][0][1] = -8;
	ctx.syntax.mvd[0][1][0][0] = 0;
	ctx.syntax.mvd[0][1][0][1] = 4;
	run_mb(0);
	ok = 1;
	for (blk = 0; blk < 16U; blk++) {
		vector_matches = mv_is(0, blk, 4, -8);
		if (blk < 8U && !vector_matches)
			ok = 0;
		vector_matches = mv_is(0, blk, 4, -4);
		if (blk >= 8U && !vector_matches)
			ok = 0;
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);
	ok = 1;
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			luma_actual = mb_luma(x, y);
			luma_reference = ref_luma(&frames[FRAME_A], (int)(16U + x) + 1, (int)(16U + y) - 2);
			if (y < 8U && luma_actual != luma_reference)
				ok = 0;
			luma_actual = mb_luma(x, y);
			luma_reference = ref_luma(&frames[FRAME_A], (int)(16U + x) + 1, (int)(16U + y) - 1);
			if (y >= 8U && luma_actual != luma_reference)
				ok = 0;
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);
	ok = 1;
	for (y = 0; y < 8U; y++) {
		for (x = 0; x < 8U; x++) {
			cb_actual = mb_cb(x, y);
			chroma_expected = expect_chroma(&frames[FRAME_A], 1U, (int)(8U + x), (int)(8U + y), 4, -8);
			if (y < 4U && cb_actual != chroma_expected)
				ok = 0;
			cb_actual = mb_cb(x, y);
			chroma_expected = expect_chroma(&frames[FRAME_A], 1U, (int)(8U + x), (int)(8U + y), 4, -4);
			if (y >= 4U && cb_actual != chroma_expected)
				ok = 0;
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);

	/* 8x16. */
	env_make(H264SW_SLICE_P, PIC_W, PIC_H, ADDR, 26);
	set_ref(0, 0, FRAME_A, 0);
	set_type(H264SW_SLICE_P, 2U);
	CHECK(ctx.syntax.type.parts == 2U && ctx.syntax.type.part_width == 8U && ctx.syntax.type.part_height == 16U);
	ctx.syntax.ref_idx[0][0] = 0;
	ctx.syntax.ref_idx[0][1] = 0;
	ctx.syntax.mvd[0][0][0][0] = 8;
	ctx.syntax.mvd[0][1][0][0] = 4;
	ctx.syntax.mvd[0][1][0][1] = 4;
	run_mb(0);
	ok = 1;
	for (blk = 0; blk < 16U; blk++) {
		vector_matches = mv_is(0, blk, 8, 0);
		if (h264sw_blk4x4_x[blk] < 8U && !vector_matches)
			ok = 0;
		vector_matches = mv_is(0, blk, 12, 4);
		if (h264sw_blk4x4_x[blk] >= 8U && !vector_matches)
			ok = 0;
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);
}

/*
 * P_8x8 with a 4x4 split in the first sub-macroblock: the vectors
 * chain through the macroblock (worked by hand, mvd (4, 0) each, all index
 * 0):
 * - block 0: nothing available, mvp 0, mv (4, 0);
 * - block 1: A is block 0, B and C are not available, so A stands for
 *   all three: mvp (4, 0), mv (8, 0);
 * - block 2: A not available, B block 0 (4, 0), C block 1 (8, 0): two of
 *   the three have the index, so the median of (0, 0), (4, 0), (8, 0) is (4, 0):
 *   mv (8, 0);
 * - block 3: A block 2 (8, 0), B block 1 (8, 0), C is in the next
 *   sub-macroblock (not yet derived, so unavailable) and D is block 0
 *   (4, 0): the median of x is 8: mv (12, 0).
 * The other sub-macroblocks are 8x8 with mvd 0: quadrant 1 takes A (8, 0),
 * quadrant 2 the median of (0), (8), (8) = 8, quadrant 3 the median of
 * A (8), B (8), D (12) = 8.
 */
static void
check_p_sub(void)
{
	static const int expected_x[16] = { 4, 8, 8, 12, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8 };
	unsigned quadrant;
	unsigned x;
	unsigned y;
	unsigned blk;
	int ok;
	int luma_actual;
	int luma_reference;
	int vector_matches;

	/* Starts a fresh decoding environment for this case. */
	env_make(H264SW_SLICE_P, PIC_W, PIC_H, ADDR, 26);
	set_ref(0, 0, FRAME_A, 0);
	set_type(H264SW_SLICE_P, 3U);
	CHECK(ctx.syntax.type.kind == H264SW_MB_P_8X8);
	set_sub(H264SW_SLICE_P, 0U, 3U);
	for (quadrant = 1; quadrant < 4U; quadrant++)
		set_sub(H264SW_SLICE_P, quadrant, 0U);
	CHECK(ctx.syntax.sub[0].parts == 4U);
	for (quadrant = 0; quadrant < 4U; quadrant++)
		ctx.syntax.ref_idx[0][quadrant] = 0;
	for (blk = 0; blk < 4U; blk++)
		ctx.syntax.mvd[0][0][blk][0] = 4;
	run_mb(0);

	/* Reconstructs the macroblock, then compares each block's vector with the expected one. */
	ok = 1;
	for (blk = 0; blk < 16U; blk++) {
		vector_matches = mv_is(0, blk, expected_x[blk], 0);
		if (!vector_matches)
			ok = 0;
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);

	/* Each 4x4 block of the first sub-macroblock is predicted from its own vector: 1, 2, 2 and 3 samples. */
	ok = 1;
	for (blk = 0; blk < 4U; blk++) {
		for (y = 0; y < 4U; y++) {
			for (x = 0; x < 4U; x++) {
				luma_actual = mb_luma(h264sw_blk4x4_x[blk] + x, h264sw_blk4x4_y[blk] + y);
				luma_reference = ref_luma(&frames[FRAME_A], (int)(16U + h264sw_blk4x4_x[blk] + x) + expected_x[blk] / 4, (int)(16U + h264sw_blk4x4_y[blk] + y));
				if (luma_actual != luma_reference)
					ok = 0;
			}
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);
	CHECK(ctx.decoded_4x4 == 0xffffU);
}

/*
 * Reference indices per sub-macroblock: P_8x8 with indices 0, 1, 1, 0 of a
 * two-entry list (frames A and B), no vector difference, zero motion
 * everywhere: each quadrant is the co-located samples of its own
 * frame, and the entry keeps the index and the frame id of each.
 */
static void
check_p_refs(void)
{
	static const int refs[4] = { 0, 1, 1, 0 };
	unsigned quadrant;
	unsigned x;
	unsigned y;
	int ok;
	int want;
	int luma_actual;
	int luma_reference;

	/* Starts a fresh decoding environment for this case. */
	env_make(H264SW_SLICE_P, PIC_W, PIC_H, ADDR, 26);
	header.num_ref_idx_active[0] = 2;
	set_ref(0, 0, FRAME_A, 0);
	set_ref(0, 1, FRAME_B, 2);
	set_type(H264SW_SLICE_P, 3U);
	for (quadrant = 0; quadrant < 4U; quadrant++) {
		set_sub(H264SW_SLICE_P, quadrant, 0U);
		ctx.syntax.ref_idx[0][quadrant] = (int8_t)refs[quadrant];
	}

	/* Reconstructs the macroblock with the four references. */
	run_mb(0);

	/* Compares the reference picture each quadrant used with the one its index names. */
	ok = 1;
	for (quadrant = 0; quadrant < 4U; quadrant++) {
		want = (int)ID_A;
		if (refs[quadrant] == 1)
			want = (int)ID_B;
		if (mbs[ADDR].motion.ref_idx[0][quadrant] != refs[quadrant])
			ok = 0;
		if (mbs[ADDR].motion.ref_id[0][quadrant] != (uint32_t)want)
			ok = 0;
		for (y = 0; y < 8U; y++) {
			for (x = 0; x < 8U; x++) {
				luma_actual = mb_luma(8U * (quadrant % 2U) + x, 8U * (quadrant / 2U) + y);
				luma_reference = ref_luma(&frames[FRAME_A], (int)(16U + 8U * (quadrant % 2U) + x), (int)(16U + 8U * (quadrant / 2U) + y));
				if (refs[quadrant] == 0 && luma_actual != luma_reference)
					ok = 0;
				luma_actual = mb_luma(8U * (quadrant % 2U) + x, 8U * (quadrant / 2U) + y);
				luma_reference = ref_luma(&frames[FRAME_B], (int)(16U + 8U * (quadrant % 2U) + x), (int)(16U + 8U * (quadrant / 2U) + y));
				if (refs[quadrant] == 1 && luma_actual != luma_reference)
					ok = 0;
			}
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);
}

/*
 * B_Bi_16x16 (mb_type 3) with default weights: the two predictions
 * are averaged ( a + b + 1 ) >> 1 (8-273): with zero vectors the
 * samples of A and B, then with L0 (8, 0) (2 samples right) and L1 (-4,
 * 0) (1 sample left).  Both lists predict their vectors alone.
 */
static void
check_b_bi(void)
{
	unsigned x;
	unsigned y;
	int ok_luma;
	int ok_chroma;
	int a;
	int b;
	int cb_actual;
	int cr_actual;
	int luma_actual;

	/* Starts a fresh decoding environment for this case. */
	env_make(H264SW_SLICE_B, PIC_W, PIC_H, ADDR, 26);
	set_ref(0, 0, FRAME_A, 0);
	set_ref(1, 0, FRAME_B, 8);
	set_type(H264SW_SLICE_B, 3U);
	CHECK(ctx.syntax.type.pred[0] == H264SW_PRED_BI);
	ctx.syntax.ref_idx[0][0] = 0;
	ctx.syntax.ref_idx[1][0] = 0;
	run_mb(0);
	ok_luma = 1;
	ok_chroma = 1;
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			a = ref_luma(&frames[FRAME_A], (int)(16U + x), (int)(16U + y));
			b = ref_luma(&frames[FRAME_B], (int)(16U + x), (int)(16U + y));
			luma_actual = mb_luma(x, y);
			if (luma_actual != ((a + b + 1) >> 1))
				ok_luma = 0;
		}
	}

	/* Compares the samples of each row of the block. */
	for (y = 0; y < 8U; y++) {
		for (x = 0; x < 8U; x++) {
			a = ref_chroma(&frames[FRAME_A], 1U, (int)(8U + x), (int)(8U + y));
			b = ref_chroma(&frames[FRAME_B], 1U, (int)(8U + x), (int)(8U + y));
			cb_actual = mb_cb(x, y);
			if (cb_actual != ((a + b + 1) >> 1))
				ok_chroma = 0;
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok_luma);
	CHECK(ok_chroma);
	CHECK(mbs[ADDR].motion.ref_id[0][0] == ID_A && mbs[ADDR].motion.ref_id[1][0] == ID_B);

	/* Different vectors for the two lists. */
	env_make(H264SW_SLICE_B, PIC_W, PIC_H, ADDR, 26);
	set_ref(0, 0, FRAME_A, 0);
	set_ref(1, 0, FRAME_B, 8);
	set_type(H264SW_SLICE_B, 3U);
	ctx.syntax.ref_idx[0][0] = 0;
	ctx.syntax.ref_idx[1][0] = 0;
	ctx.syntax.mvd[0][0][0][0] = 8;
	ctx.syntax.mvd[1][0][0][0] = -4;
	run_mb(0);
	CHECK(mv_is(0, 0, 8, 0) && mv_is(1, 0, -4, 0) && mv_is(1, 15, -4, 0));
	ok_luma = 1;
	ok_chroma = 1;
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			a = ref_luma(&frames[FRAME_A], (int)(16U + x) + 2, (int)(16U + y));
			b = ref_luma(&frames[FRAME_B], (int)(16U + x) - 1, (int)(16U + y));
			luma_actual = mb_luma(x, y);
			if (luma_actual != ((a + b + 1) >> 1))
				ok_luma = 0;
		}
	}

	/* Compares the samples of each row of the block. */
	for (y = 0; y < 8U; y++) {
		for (x = 0; x < 8U; x++) {
			a = expect_chroma(&frames[FRAME_A], 2U, (int)(8U + x), (int)(8U + y), 8, 0);
			b = expect_chroma(&frames[FRAME_B], 2U, (int)(8U + x), (int)(8U + y), -4, 0);
			cr_actual = mb_cr(x, y);
			if (cr_actual != ((a + b + 1) >> 1))
				ok_chroma = 0;
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok_luma);
	CHECK(ok_chroma);
}

/*
 * Implicit weights (weighted_bipred_idc 2) of a bi-predicted B
 * macroblock: the current picture has order count 2, A (list 0) 0 and B
 * (list 1) 8: tb 2, td 8, tx 2048, DistScaleFactor ( 2 * 2048 + 32 ) >> 6 = 64;
 * w1 = 64 >> 2 = 16 and w0 = 48 (8-280 to 8-283): ( a * 48 + b * 16 +
 * 32 ) >> 6 (8-276 with logWD 5).
 */
static void
check_b_implicit(void)
{
	unsigned x;
	unsigned y;
	int a;
	int b;
	int ok;
	int cb_actual;
	int clamped_sample;
	int luma_actual;

	/* Starts a fresh decoding environment for this case. */
	env_make(H264SW_SLICE_B, PIC_W, PIC_H, ADDR, 26);
	set_ref(0, 0, FRAME_A, 0);
	set_ref(1, 0, FRAME_B, 8);
	pps.weighted_bipred_idc = 2;
	h264sw_weights_setup(&ctx.weights, &pps, &header, &ctx.lists, 2);
	set_type(H264SW_SLICE_B, 3U);
	ctx.syntax.ref_idx[0][0] = 0;
	ctx.syntax.ref_idx[1][0] = 0;
	run_mb(0);
	ok = 1;
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			a = ref_luma(&frames[FRAME_A], (int)(16U + x), (int)(16U + y));
			b = ref_luma(&frames[FRAME_B], (int)(16U + x), (int)(16U + y));
			luma_actual = mb_luma(x, y);
			clamped_sample = clamp255((a * 48 + b * 16 + 32) >> 6);
			if (luma_actual != clamped_sample)
				ok = 0;
		}
	}

	/* Compares the samples of each row of the block. */
	for (y = 0; y < 8U; y++) {
		for (x = 0; x < 8U; x++) {
			a = ref_chroma(&frames[FRAME_A], 1U, (int)(8U + x), (int)(8U + y));
			b = ref_chroma(&frames[FRAME_B], 1U, (int)(8U + x), (int)(8U + y));
			cb_actual = mb_cb(x, y);
			clamped_sample = clamp255((a * 48 + b * 16 + 32) >> 6);
			if (cb_actual != clamped_sample)
				ok = 0;
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);
}

/*
 * B_L0_L1_16x8 (mb_type 8): the upper partition predicts from list 0
 * (A) and the lower from list 1 (B), each with its own reference.
 */
static void
check_b_mixed(void)
{
	unsigned x;
	unsigned y;
	int ok;
	int luma_actual;
	int luma_reference;

	/* Starts a fresh decoding environment for this case. */
	env_make(H264SW_SLICE_B, PIC_W, PIC_H, ADDR, 26);
	set_ref(0, 0, FRAME_A, 0);
	set_ref(1, 0, FRAME_B, 8);
	set_type(H264SW_SLICE_B, 8U);
	CHECK(ctx.syntax.type.pred[0] == H264SW_PRED_L0);
	CHECK(ctx.syntax.type.pred[1] == H264SW_PRED_L1);
	CHECK(ctx.syntax.type.part_height == 8U);
	ctx.syntax.ref_idx[0][0] = 0;
	ctx.syntax.ref_idx[1][1] = 0;
	run_mb(0);
	ok = 1;
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			luma_actual = mb_luma(x, y);
			luma_reference = ref_luma(&frames[FRAME_A], (int)(16U + x), (int)(16U + y));
			if (y < 8U && luma_actual != luma_reference)
				ok = 0;
			luma_actual = mb_luma(x, y);
			luma_reference = ref_luma(&frames[FRAME_B], (int)(16U + x), (int)(16U + y));
			if (y >= 8U && luma_actual != luma_reference)
				ok = 0;
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);

	/* The upper half uses list 0 only, the lower list 1 only. */
	CHECK(mbs[ADDR].motion.ref_idx[0][0] == 0 && mbs[ADDR].motion.ref_idx[0][1] == 0);
	CHECK(mbs[ADDR].motion.ref_idx[0][2] == -1 && mbs[ADDR].motion.ref_idx[0][3] == -1);
	CHECK(mbs[ADDR].motion.ref_idx[1][0] == -1 && mbs[ADDR].motion.ref_idx[1][1] == -1);
	CHECK(mbs[ADDR].motion.ref_idx[1][2] == 0 && mbs[ADDR].motion.ref_idx[1][3] == 0);
}

/*
 * B_Skip with direct prediction.  Spatial, with no neighbours: both
 * indices are 0 and the vectors 0 (directZeroPredictionFlag, 8-190), so
 * each sample is the average of A (list 0) and the co-located frame C
 * (list 1).  Temporal, with the current order count 4, A 0, C 8 and the
 * co-located macroblock moving (16, 0) from A: tb 4, td 8, tx 2048,
 * DistScaleFactor ( 4 * 2048 + 32 ) >> 6 = 128; mvL0 = ( 128 * 16 + 128 ) >> 8 = 8,
 * mvL1 = 8 - 16 = -8 (8-199, 8-200): A shifted 2 samples right and C 2 left.
 */
static void
check_b_direct(void)
{
	unsigned x;
	unsigned y;
	unsigned blk;
	unsigned quadrant;
	int ok;
	int a;
	int c;
	int luma_actual;

	/* Spatial, B_Skip. */
	env_make(H264SW_SLICE_B, PIC_W, PIC_H, ADDR, 26);
	header.direct_spatial = 1;
	set_ref(0, 0, FRAME_A, 0);
	set_ref(1, 0, FRAME_C, 8);
	h264sw_direct_setup(&ctx.direct, &ctx.lists, 4);
	h264sw_mb_skip_type(H264SW_SLICE_B, &ctx.syntax.type);
	mbs[ADDR].kind = (uint8_t)ctx.syntax.type.kind;
	run_mb(0);
	ok = 1;
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			a = ref_luma(&frames[FRAME_A], (int)(16U + x), (int)(16U + y));
			c = ref_luma(&frames[FRAME_C], (int)(16U + x), (int)(16U + y));
			luma_actual = mb_luma(x, y);
			if (luma_actual != ((a + c + 1) >> 1))
				ok = 0;
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);
	for (quadrant = 0; quadrant < 4U; quadrant++) {
		CHECK(mbs[ADDR].motion.ref_idx[0][quadrant] == 0 && mbs[ADDR].motion.ref_idx[1][quadrant] == 0);
		CHECK(mbs[ADDR].motion.ref_id[0][quadrant] == ID_A && mbs[ADDR].motion.ref_id[1][quadrant] == ID_C);
	}

	/* Requires that the direct flags cover all four quadrants and every 4x4 block was decoded. */
	CHECK(mbs[ADDR].direct == 0xfU);
	CHECK(ctx.decoded_4x4 == 0xffffU);

	/* Temporal, B_Direct_16x16 (mb_type 0). */
	env_make(H264SW_SLICE_B, PIC_W, PIC_H, ADDR, 26);
	header.direct_spatial = 0;
	set_ref(0, 0, FRAME_A, 0);
	set_ref(0, 1, FRAME_B, 2);
	set_ref(1, 0, FRAME_C, 8);
	frames[FRAME_C].motion[ADDR].intra = 0;
	for (quadrant = 0; quadrant < 4U; quadrant++) {
		frames[FRAME_C].motion[ADDR].ref_idx[0][quadrant] = 0;
		frames[FRAME_C].motion[ADDR].ref_id[0][quadrant] = ID_A;
	}

	/* Gives the co-located macroblock a vector of 16 on every block. */
	for (blk = 0; blk < 16U; blk++) {
		frames[FRAME_C].motion[ADDR].mv[0][blk][0] = 16;
		frames[FRAME_C].motion[ADDR].mv[0][blk][1] = 0;
	}

	/* Prepares the direct-mode state for a B picture of picture order count 4. */
	h264sw_direct_setup(&ctx.direct, &ctx.lists, 4);
	set_type(H264SW_SLICE_B, 0U);
	CHECK(ctx.syntax.type.kind == H264SW_MB_B_DIRECT);
	run_mb(0);
	CHECK(mv_is(0, 0, 8, 0) && mv_is(0, 15, 8, 0));
	CHECK(mv_is(1, 0, -8, 0) && mv_is(1, 15, -8, 0));
	ok = 1;
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			a = ref_luma(&frames[FRAME_A], (int)(16U + x) + 2, (int)(16U + y));
			c = ref_luma(&frames[FRAME_C], (int)(16U + x) - 2, (int)(16U + y));
			luma_actual = mb_luma(x, y);
			if (luma_actual != ((a + c + 1) >> 1))
				ok = 0;
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);
	CHECK(mbs[ADDR].motion.ref_id[0][0] == ID_A);
	CHECK(mbs[ADDR].direct == 0xfU);
}

/*
 * B_8x8 with a direct sub-macroblock first, then List 1, Bi and List 0
 * sub-macroblocks (sub_mb_type 0, 2, 3, 1): with no neighbours the
 * direct quadrant is spatial with zero motion, so quadrant 0 is the
 * average of A and the co-located frame C; quadrant 1 predicts from C
 * (list 1, vector 0, as the direct quadrant on its left has index 0 and
 * vector 0); quadrant 2 is the average of A and C; quadrant 3 predicts
 * from list 0 with the difference (8, 0) on a zero prediction (the
 * neighbours' vectors are all 0): A two samples to the right.
 */
static void
check_b_8x8(void)
{
	unsigned x;
	unsigned y;
	int ok;
	int a;
	int c;
	int want;
	int luma_actual;

	/* Starts a fresh decoding environment for this case. */
	env_make(H264SW_SLICE_B, PIC_W, PIC_H, ADDR, 26);
	header.direct_spatial = 1;
	set_ref(0, 0, FRAME_A, 0);
	set_ref(1, 0, FRAME_C, 8);
	h264sw_direct_setup(&ctx.direct, &ctx.lists, 4);
	set_type(H264SW_SLICE_B, 22U);
	CHECK(ctx.syntax.type.kind == H264SW_MB_B_8X8);
	set_sub(H264SW_SLICE_B, 0U, 0U);
	set_sub(H264SW_SLICE_B, 1U, 2U);
	set_sub(H264SW_SLICE_B, 2U, 3U);
	set_sub(H264SW_SLICE_B, 3U, 1U);
	CHECK(ctx.syntax.sub[0].pred == H264SW_PRED_DIRECT);
	CHECK(ctx.syntax.sub[1].pred == H264SW_PRED_L1);
	CHECK(ctx.syntax.sub[2].pred == H264SW_PRED_BI);
	CHECK(ctx.syntax.sub[3].pred == H264SW_PRED_L0);
	ctx.syntax.ref_idx[1][1] = 0;
	ctx.syntax.ref_idx[0][2] = 0;
	ctx.syntax.ref_idx[1][2] = 0;
	ctx.syntax.ref_idx[0][3] = 0;
	ctx.syntax.mvd[0][3][0][0] = 8;
	run_mb(0);

	/* Reconstructs the macroblock, then compares every luma sample with the expected prediction. */
	ok = 1;
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			a = ref_luma(&frames[FRAME_A], (int)(16U + x), (int)(16U + y));
			c = ref_luma(&frames[FRAME_C], (int)(16U + x), (int)(16U + y));
			want = (a + c + 1) >> 1;
			if (x >= 8U && y < 8U)
				want = c;
			if (x >= 8U && y >= 8U)
				want = ref_luma(&frames[FRAME_A], (int)(16U + x) + 2, (int)(16U + y));
			luma_actual = mb_luma(x, y);
			if (luma_actual != want)
				ok = 0;
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);
	CHECK(mbs[ADDR].direct == 1U);
	CHECK(mbs[ADDR].motion.ref_idx[0][0] == 0 && mbs[ADDR].motion.ref_idx[1][0] == 0);
	CHECK(mbs[ADDR].motion.ref_idx[0][1] == -1 && mbs[ADDR].motion.ref_idx[1][1] == 0);
	CHECK(mbs[ADDR].motion.ref_idx[0][3] == 0 && mbs[ADDR].motion.ref_idx[1][3] == -1);
	CHECK(mv_is(0, 12, 8, 0) && mv_is(0, 15, 8, 0));
}

/*
 * mvLX = mvpLX + mvdLX wraps into -32768..32767 by arithmetic: with
 * the neighbour A at (30000, -30000) and the only available one, the
 * prediction is it, and the difference (32767, -32768) gives 62767 -
 * 65536 = -2769 and -62768 + 65536 = 2768.  The prediction with such
 * a vector clamps and stays inside the picture.
 */
static void
check_vector_wrap(void)
{
	env_make(H264SW_SLICE_P, PIC_W, PIC_H, ADDR, 26);
	set_ref(0, 0, FRAME_A, 0);
	neighbour_inter(3U, 0, 30000, -30000);
	set_type(H264SW_SLICE_P, 0U);
	ctx.syntax.ref_idx[0][0] = 0;
	ctx.syntax.mvd[0][0][0][0] = 32767;
	ctx.syntax.mvd[0][0][0][1] = -32768;
	run_mb(0);
	CHECK(mv_is(0, 0, -2769, 2768));
	CHECK(mv_is(0, 15, -2769, 2768));

	/* The most extreme sums: -32768 + -32768 and 32767 + 32767 wrap too. */
	env_make(H264SW_SLICE_P, PIC_W, PIC_H, ADDR, 26);
	set_ref(0, 0, FRAME_A, 0);
	neighbour_inter(3U, 0, -32768, 32767);
	set_type(H264SW_SLICE_P, 0U);
	ctx.syntax.ref_idx[0][0] = 0;
	ctx.syntax.mvd[0][0][0][0] = -32768;
	ctx.syntax.mvd[0][0][0][1] = 32767;
	run_mb(0);
	CHECK(mv_is(0, 0, 0, -2));
}

/*
 * The errors.  A macroblock that cannot be built at all gives EINVAL
 * and does not touch memory outside the frame: a context without a
 * frame or an entry, a macroblock outside the picture, a reference
 * index outside its list, a missing frame, a frame of another size,
 * no co-located frame for direct prediction, an unresolved or unknown
 * kind, a type whose partitions do not fit.  A reference the list marked missing is
 * predicted from its stand-in and counted, with status 0.
 */
static void
check_errors(void)
{
	struct h264sw_frame *saved_frame;
	struct h264sw_mb *saved_current;
	int status;

	/* No context, no picture, no entry, no frame, a macroblock outside the picture. */
	status = h264sw_recon_mb(NULL);
	CHECK(status == EINVAL);
	env_make(H264SW_SLICE_I, PIC_W, PIC_H, ADDR, 26);
	set_type(H264SW_SLICE_I, 3U);
	saved_frame = picture.frame;
	picture.frame = NULL;
	run_mb(EINVAL);
	picture.frame = saved_frame;
	saved_current = ctx.n.current;
	ctx.n.current = NULL;
	run_mb(EINVAL);
	ctx.n.current = saved_current;
	ctx.n.x = pic_w;
	run_mb(EINVAL);
	ctx.n.x = 1;
	ctx.n.y = pic_h;
	run_mb(EINVAL);
	ctx.n.y = 1;
	frames[FRAME_CURRENT].frame.plane[2] = NULL;
	run_mb(EINVAL);
	frames[FRAME_CURRENT].frame.plane[2] = frames[FRAME_CURRENT].cr;
	frames[FRAME_CURRENT].frame.width = 16;
	run_mb(EINVAL);
	frames[FRAME_CURRENT].frame.width = 48;
	run_mb(0);

	/* An unresolved I_NxN and an unknown kind. */
	env_make(H264SW_SLICE_I, PIC_W, PIC_H, ADDR, 26);
	set_type(H264SW_SLICE_I, 0U);
	CHECK(ctx.syntax.type.kind == H264SW_MB_I_NXN);
	run_mb(EINVAL);
	ctx.syntax.type.kind = 99;
	run_mb(EINVAL);

	/* A reference index outside the list, below 0, a missing frame, a frame of another size. */
	env_make(H264SW_SLICE_P, PIC_W, PIC_H, ADDR, 26);
	set_ref(0, 0, FRAME_A, 0);
	set_type(H264SW_SLICE_P, 0U);
	ctx.syntax.ref_idx[0][0] = 1;
	run_mb(EINVAL);
	ctx.syntax.ref_idx[0][0] = -1;
	run_mb(EINVAL);
	ctx.syntax.ref_idx[0][0] = 0;
	ctx.lists.list[0][0].frame = NULL;
	run_mb(EINVAL);
	ctx.lists.list[0][0].frame = &frames[FRAME_A].frame;
	frames[FRAME_A].frame.height = 32;
	run_mb(EINVAL);
	frames[FRAME_A].frame.height = 48;
	frames[FRAME_A].frame.plane[1] = NULL;
	run_mb(EINVAL);
	frames[FRAME_A].frame.plane[1] = frames[FRAME_A].cb;

	/* P_Skip without a first entry in list 0. */
	env_make(H264SW_SLICE_P, PIC_W, PIC_H, ADDR, 26);
	h264sw_mb_skip_type(H264SW_SLICE_P, &ctx.syntax.type);
	mbs[ADDR].kind = (uint8_t)ctx.syntax.type.kind;
	run_mb(EINVAL);
	set_ref(0, 0, FRAME_A, 0);
	ctx.lists.list[0][0].frame = NULL;
	run_mb(EINVAL);

	/* Direct prediction without a co-located frame. */
	env_make(H264SW_SLICE_B, PIC_W, PIC_H, ADDR, 26);
	set_ref(0, 0, FRAME_A, 0);
	h264sw_mb_skip_type(H264SW_SLICE_B, &ctx.syntax.type);
	mbs[ADDR].kind = (uint8_t)ctx.syntax.type.kind;
	run_mb(EINVAL);

	/* A type with three partitions, no width, and a sub-macroblock that has no partitions or an odd size. */
	env_make(H264SW_SLICE_P, PIC_W, PIC_H, ADDR, 26);
	set_ref(0, 0, FRAME_A, 0);
	set_type(H264SW_SLICE_P, 1U);
	ctx.syntax.type.parts = 3;
	run_mb(EINVAL);
	ctx.syntax.type.parts = 2;
	ctx.syntax.type.part_width = 0;
	run_mb(EINVAL);
	ctx.syntax.type.part_width = 16;
	ctx.syntax.type.pred[0] = H264SW_PRED_NONE;
	run_mb(EINVAL);
	env_make(H264SW_SLICE_P, PIC_W, PIC_H, ADDR, 26);
	set_ref(0, 0, FRAME_A, 0);
	set_type(H264SW_SLICE_P, 3U);
	set_sub(H264SW_SLICE_P, 0U, 0U);
	set_sub(H264SW_SLICE_P, 1U, 0U);
	set_sub(H264SW_SLICE_P, 2U, 0U);
	set_sub(H264SW_SLICE_P, 3U, 0U);
	ctx.syntax.sub[2].parts = 0;
	run_mb(EINVAL);
	ctx.syntax.sub[2].parts = 1;
	ctx.syntax.sub[2].part_width = 6;
	run_mb(EINVAL);
	ctx.syntax.sub[2].part_width = 8;
	ctx.syntax.sub[2].part_height = 2;
	run_mb(EINVAL);

	/* A list entry marked missing is predicted from its stand-in: status 0, one concealed macroblock. */
	env_make(H264SW_SLICE_P, PIC_W, PIC_H, ADDR, 26);
	set_ref(0, 0, FRAME_A, 0);
	ctx.lists.list[0][0].missing = 1;
	set_type(H264SW_SLICE_P, 0U);
	ctx.syntax.ref_idx[0][0] = 0;
	run_mb(0);
	CHECK(ctx.concealed == 1U);
	CHECK(mb_luma(0U, 0U) == ref_luma(&frames[FRAME_A], 16, 16));
}

/*
 * Random syntax: every kind with random vectors, indices (including
 * ones outside the lists), levels over the whole range the parser
 * allows and beyond, quantization parameters, modes and neighbours.
 * The status is 0 or EINVAL and the run is clean under the sanitizers;
 * a successful run leaves the motion in the entry within the lists.
 */
static void
fuzz_syntax(void)
{
	static const int32_t extreme[6] = { 32767, -32768, 2147483647, -2147483647 - 1, 100000, -100000 };
	unsigned iteration;
	unsigned slice_type;
	unsigned mb_type;
	unsigned index;
	unsigned list;
	unsigned part;
	unsigned sub;
	unsigned addr;
	int status;
	int raw_type;
	uint32_t draw;
	int fill_neighbour;

	/* Reconstructs 30000 random macroblocks: the call must return 0 or EINVAL and stay inside the picture. */
	for (iteration = 0; iteration < 30000U; iteration++) {
		/* A slice type and a macroblock somewhere in the picture, with random neighbours. */
		slice_type = H264SW_SLICE_I;
		draw = random_next();
		if (draw % 3U == 1U)
			slice_type = H264SW_SLICE_P;
		draw = random_next();
		if (draw % 3U == 2U)
			slice_type = H264SW_SLICE_B;
		addr = random_next() % PIC_MBS;
		env_make(slice_type, PIC_W, PIC_H, addr, (int)(random_next() % 52U));
		mbs[addr].qp = (uint8_t)random_next();
		picture.chroma_qp_offset[0] = (int)(random_next() % 25U) - 12;
		picture.chroma_qp_offset[1] = (int)(random_next() % 25U) - 12;
		for (index = 0; index < PIC_MBS; index++) {
			/* Draws whether this macroblock gets random contents; the current one is never filled and draws nothing. */
			fill_neighbour = 0;
			if (index != addr)
				fill_neighbour = random_next() % 2U == 0U;
			if (fill_neighbour != 0) {
				mbs[index].slice = 0;
				mbs[index].kind = (uint8_t)(random_next() % 13U);
				mbs[index].motion.intra = (uint8_t)(random_next() % 2U);
				for (list = 0; list < 2U; list++) {
					for (part = 0; part < 4U; part++)
						mbs[index].motion.ref_idx[list][part] = (int8_t)((int)(random_next() % 5U) - 1);
					for (part = 0; part < 16U; part++) {
						mbs[index].motion.mv[list][part][0] = (int16_t)random_next();
						mbs[index].motion.mv[list][part][1] = (int16_t)random_next();
					}
				}
			}
		}

		/* Recomputes the neighbours of the macroblock after the random motion was written. */
		neighbours_update();

		/* The lists: some entries present, some missing, some empty. */
		for (list = 0; list < 2U; list++) {
			index = random_next() % 4U;
			for (part = 0; part < index; part++)
				set_ref(list, part, FRAME_A + (random_next() % 3U), (int)(random_next() % 20U));
			draw = random_next();
			if (draw % 8U == 0U && index > 0U)
				ctx.lists.list[list][0].missing = 1;
		}

		/* Prepares the direct-mode state and picks the direct flags at random. */
		h264sw_direct_setup(&ctx.direct, &ctx.lists, 4);
		header.direct_spatial = (int)(random_next() % 2U);
		sps.direct_8x8_inference = (int)(random_next() % 2U);

		/* The macroblock type: a valid one of the slice type, sometimes with its fields spoilt. */
		mb_type = random_next() % 50U;
		status = h264sw_mb_type_of(slice_type, mb_type, &ctx.syntax.type);
		if (status != 0) {
			if (slice_type == H264SW_SLICE_I) {
				h264sw_mb_type_of(slice_type, 3U, &ctx.syntax.type);
			} else {
				h264sw_mb_skip_type(slice_type, &ctx.syntax.type);
			}
		}

		/* Splits an NxN intra macroblock into 4x4 or 8x8 prediction at random. */
		if (ctx.syntax.type.kind == H264SW_MB_I_NXN) {
			ctx.syntax.type.kind = H264SW_MB_I_4X4;
			draw = random_next();
			if (draw % 2U == 0U)
				ctx.syntax.type.kind = H264SW_MB_I_8X8;
		}

		/* Corrupts the macroblock type at random, to meet kinds outside the legal range. */
		raw_type = (int)(random_next() % 10U);
		if (raw_type == 0)
			ctx.syntax.type.kind = random_next() % 20U;
		if (raw_type == 1)
			ctx.syntax.type.parts = random_next() % 6U;
		if (raw_type == 2)
			ctx.syntax.type.part_width = random_next() % 40U;
		if (raw_type == 3)
			ctx.syntax.type.part_height = random_next() % 40U;
		if (raw_type == 4)
			ctx.syntax.type.pred[random_next() % 2U] = random_next() % 6U;
		mbs[addr].kind = (uint8_t)ctx.syntax.type.kind;
		mbs[addr].transform_8x8 = (uint8_t)(random_next() % 2U);
		mbs[addr].chroma_pred_mode = (uint8_t)(random_next() % 4U);
		for (part = 0; part < 4U; part++) {
			status = h264sw_sub_mb_type_of(slice_type, random_next() % 14U, &ctx.syntax.sub[part]);
			if (status != 0) {
				ctx.syntax.sub[part].parts = random_next() % 6U;
				ctx.syntax.sub[part].part_width = random_next() % 12U;
				ctx.syntax.sub[part].part_height = random_next() % 12U;
				ctx.syntax.sub[part].pred = random_next() % 6U;
			}
		}

		/* References, differences, modes and levels. */
		for (list = 0; list < 2U; list++) {
			for (part = 0; part < 4U; part++) {
				ctx.syntax.ref_idx[list][part] = (int8_t)((int)(random_next() % 5U) - 1);
				draw = random_next();
				if (draw % 16U == 0U)
					ctx.syntax.ref_idx[list][part] = (int8_t)random_next();
				for (sub = 0; sub < 4U; sub++) {
					ctx.syntax.mvd[list][part][sub][0] = (int16_t)random_next();
					ctx.syntax.mvd[list][part][sub][1] = (int16_t)random_next();
				}
			}
		}

		/* Fills the intra prediction modes with random values. */
		for (part = 0; part < 16U; part++) {
			ctx.syntax.prev_intra_flag[part] = (uint8_t)(random_next() % 2U);
			ctx.syntax.rem_intra_mode[part] = (uint8_t)(random_next() % 8U);
		}

		/* Fills the I_PCM samples with random bytes. */
		for (part = 0; part < 384U; part++)
			ctx.syntax.pcm[part] = (uint8_t)random_next();
		for (part = 0; part < 16U; part++) {
			draw = random_next();
			if (draw % 4U == 0U)
				ctx.syntax.luma_dc[part] = (int32_t)(random_next() % 65536U) - 32768;
			draw = random_next();
			if (draw % 40U == 0U)
				ctx.syntax.luma_dc[part] = extreme[random_next() % 6U];
			for (sub = 0; sub < 16U; sub++) {
				draw = random_next();
				if (draw % 8U == 0U)
					ctx.syntax.luma[part][sub] = (int32_t)(random_next() % 65536U) - 32768;
				draw = random_next();
				if (draw % 200U == 0U)
					ctx.syntax.luma[part][sub] = extreme[random_next() % 6U];
			}
		}

		/* Fills the 8x8 luma blocks and the chroma DC levels with random levels, some of them extreme. */
		for (part = 0; part < 4U; part++) {
			for (sub = 0; sub < 64U; sub++) {
				draw = random_next();
				if (draw % 8U == 0U)
					ctx.syntax.luma8x8[part][sub] = (int32_t)(random_next() % 65536U) - 32768;
				draw = random_next();
				if (draw % 200U == 0U)
					ctx.syntax.luma8x8[part][sub] = extreme[random_next() % 6U];
			}

			/* Fills the chroma DC levels with random levels, some of them extreme. */
			for (list = 0; list < 2U; list++) {
				ctx.syntax.chroma_dc[list][part] = (int32_t)(random_next() % 65536U) - 32768;
				draw = random_next();
				if (draw % 20U == 0U)
					ctx.syntax.chroma_dc[list][part] = extreme[random_next() % 6U];
				for (sub = 0; sub < 16U; sub++) {
					draw = random_next();
					if (draw % 4U == 0U)
						ctx.syntax.chroma_ac[list][part][sub] = (int32_t)(random_next() % 65536U) - 32768;
					draw = random_next();
					if (draw % 100U == 0U)
						ctx.syntax.chroma_ac[list][part][sub] = extreme[random_next() % 6U];
				}
			}
		}

		/* Reconstructs the macroblock from the random syntax and checks that it reports success or EINVAL. */
		ctx.decoded_4x4 = 0;
		status = h264sw_recon_mb(&ctx);
		CHECK(status == 0 || status == EINVAL);
	}
}

/* Prepares a slice of the 2 by 2 picture for the slice data loop: nothing decoded, slice number 0, the first macroblock 0, one reference in each list. */
static void
slice_env(
	unsigned slice_type)
{
	unsigned index;

	/* The context for the 2 by 2 picture and macroblock 0. */
	env_make(slice_type, 2U, 2U, 0U, 26);
	for (index = 0; index < PIC_MBS; index++)
		h264sw_mb_reset(&mbs[index], -1);
	header.first_mb = 0;
	header.direct_spatial = 1;
	ctx.slice_num = 0;
	ctx.cabac_mode = 0;
	picture.decoded_mbs = 0;
	set_ref(0, 0, FRAME_A, 0);
	if (slice_type == H264SW_SLICE_B)
		set_ref(1, 0, FRAME_C, 8);
	h264sw_direct_setup(&ctx.direct, &ctx.lists, 4);
}

/* Starts the stream of a slice case. */
static void
stream_start(void)
{
	/* The slice data starts empty. */
	bitwriter_init(&writer);
}

/* Ends the stream with the trailing bits, runs the slice data loop over it and checks the status. */
static void
stream_run(
	unsigned expected_status)
{
	int status;

	/* The trailing bits end the slice data; the reader starts at the first bit. */
	bitwriter_trailing(&writer);
	CHECK(writer.overflow == 0);
	vcodec_bitreader_init(&ctx.bits, writer.data, bitwriter_bytes(&writer));

	/* The loop under test, with the real macroblock layer and reconstruction. */
	status = h264sw_slice_data(&ctx);
	CHECK(status == (int)expected_status);
}

/* Writes an I_PCM macroblock whose samples are ( index * 7 + 13 * mb + 3 ) & 255: mb_type 25, the alignment bits, 384 bytes. */
static void
put_pcm_mb(
	unsigned mb)
{
	unsigned index;

	/* mb_type 25 of an I slice, then the zero bits up to the byte boundary, then the samples. */
	bitwriter_ue(&writer, 25U);
	while (writer.bits % 8U != 0U)
		bitwriter_u(&writer, 0U, 1U);
	for (index = 0; index < 384U; index++)
		bitwriter_u(&writer, (index * 7U + 13U * mb + 3U) & 255U, 8U);
}

/* Keeps the decoded picture's samples to be the reference of the next slice case. */
static void
save_picture(void)
{
	/* Copies the three planes of the current frame. */
	memcpy(saved_luma, frames[FRAME_CURRENT].luma, sizeof(saved_luma));
	memcpy(saved_cb, frames[FRAME_CURRENT].cb, sizeof(saved_cb));
	memcpy(saved_cr, frames[FRAME_CURRENT].cr, sizeof(saved_cr));
}

/* Makes frame A the kept picture. */
static void
restore_reference(void)
{
	/* Copies the three planes into frame A. */
	memcpy(frames[FRAME_A].luma, saved_luma, sizeof(saved_luma));
	memcpy(frames[FRAME_A].cb, saved_cb, sizeof(saved_cb));
	memcpy(frames[FRAME_A].cr, saved_cr, sizeof(saved_cr));
}

/*
 * The loop over the macroblocks of I slices of 2 by 2 macroblocks: four
 * Intra_16x16 DC macroblocks without residual are 128 all over; four I_PCM
 * macroblocks are the samples sent; every macroblock is recorded decoded.
 */
static void
check_slice_intra(void)
{
	unsigned mb;
	unsigned index;
	unsigned x;
	unsigned y;
	int ok;
	int cb_actual;
	int cr_actual;
	int luma_actual;

	/* Four Intra_16x16 DC macroblocks: mb_type 3, intra_chroma_pred_mode 0, mb_qp_delta 0, an empty DC block (coeff_token 1 as every nC is 0). */
	slice_env(H264SW_SLICE_I);
	stream_start();
	for (mb = 0; mb < 4U; mb++) {
		bitwriter_ue(&writer, 3U);
		bitwriter_ue(&writer, 0U);
		bitwriter_se(&writer, 0);
		bitwriter_u(&writer, 1U, 1U);
	}

	/* Decodes the slice data and requires that all four macroblocks were decoded. */
	stream_run(0U);
	CHECK(picture.decoded_mbs == 4U);
	ok = 1;
	for (y = 0; y < 32U; y++) {
		for (x = 0; x < 32U; x++) {
			luma_actual = luma_at(x, y);
			if (luma_actual != 128)
				ok = 0;
		}
	}

	/* Compares the whole picture with the flat 128 an I_PCM-less intra picture gives. */
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			cb_actual = cb_at(x, y);
			cr_actual = cr_at(x, y);
			if (cb_actual != 128 || cr_actual != 128)
				ok = 0;
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);
	CHECK(ctx.concealed == 0U);

	/* Four I_PCM macroblocks. */
	slice_env(H264SW_SLICE_I);
	stream_start();
	for (mb = 0; mb < 4U; mb++)
		put_pcm_mb(mb);
	stream_run(0U);
	CHECK(picture.decoded_mbs == 4U);
	ok = 1;
	for (mb = 0; mb < 4U; mb++) {
		for (y = 0; y < 16U; y++) {
			for (x = 0; x < 16U; x++) {
				index = y * 16U + x;
				luma_actual = luma_at(16U * (mb % 2U) + x, 16U * (mb / 2U) + y);
				if (luma_actual != (int)((index * 7U + 13U * mb + 3U) & 255U))
					ok = 0;
			}
		}

		/* Compares the samples of each row of the block. */
		for (y = 0; y < 8U; y++) {
			for (x = 0; x < 8U; x++) {
				index = 256U + y * 8U + x;
				cb_actual = cb_at(8U * (mb % 2U) + x, 8U * (mb / 2U) + y);
				if (cb_actual != (int)((index * 7U + 13U * mb + 3U) & 255U))
					ok = 0;
				index = 320U + y * 8U + x;
				cr_actual = cr_at(8U * (mb % 2U) + x, 8U * (mb / 2U) + y);
				if (cr_actual != (int)((index * 7U + 13U * mb + 3U) & 255U))
					ok = 0;
			}
		}

		/* Requires that the macroblock was recorded as intra with all its coefficients and a zero QP. */
		if (mbs[mb].nonzero != 0xffffU || mbs[mb].qp != 0U || mbs[mb].motion.intra != 1U)
			ok = 0;
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);
	save_picture();
}

/*
 * The P slices that follow, with the I_PCM picture as the reference:
 * four skipped macroblocks (a skip run of 4) give a copy of the
 * reference (every P_Skip vector is 0 as the neighbours have none); four
 * P_L0_16x16 macroblocks with the vector (16, 0) (the first sends the
 * difference, the others predict it from their neighbours and send 0)
 * give the reference moved 4 samples (2 for chroma) with the edge
 * repeated; a B slice of four B_Skip macroblocks (spatial, no motion
 * around) averages A and the co-located frame.
 */
static void
check_slice_inter(void)
{
	unsigned mb;
	unsigned x;
	unsigned y;
	int ok;
	int a;
	int c;
	int cb_actual;
	int chroma_reference;
	int cr_actual;
	int luma_actual;
	int luma_reference;

	/* The skip run. */
	slice_env(H264SW_SLICE_P);
	restore_reference();
	stream_start();
	bitwriter_ue(&writer, 4U);
	stream_run(0U);
	CHECK(picture.decoded_mbs == 4U);
	ok = 1;
	for (y = 0; y < 32U; y++) {
		for (x = 0; x < 32U; x++) {
			luma_actual = luma_at(x, y);
			luma_reference = ref_luma(&frames[FRAME_A], (int)x, (int)y);
			if (luma_actual != luma_reference)
				ok = 0;
		}
	}

	/* Compares the picture with the reference picture copied without motion. */
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			cb_actual = cb_at(x, y);
			chroma_reference = ref_chroma(&frames[FRAME_A], 1U, (int)x, (int)y);
			if (cb_actual != chroma_reference)
				ok = 0;
			cr_actual = cr_at(x, y);
			chroma_reference = ref_chroma(&frames[FRAME_A], 2U, (int)x, (int)y);
			if (cr_actual != chroma_reference)
				ok = 0;
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);

	/* Four P_L0_16x16 macroblocks. */
	slice_env(H264SW_SLICE_P);
	restore_reference();
	stream_start();
	for (mb = 0; mb < 4U; mb++) {
		bitwriter_ue(&writer, 0U);
		bitwriter_ue(&writer, 0U);
		bitwriter_se(&writer, mb == 0U ? 16 : 0);
		bitwriter_se(&writer, 0);
		bitwriter_ue(&writer, 0U);
	}

	/* Decodes the slice data and requires that all four macroblocks were decoded. */
	stream_run(0U);
	CHECK(picture.decoded_mbs == 4U);
	ok = 1;
	for (y = 0; y < 32U; y++) {
		for (x = 0; x < 32U; x++) {
			luma_actual = luma_at(x, y);
			luma_reference = ref_luma(&frames[FRAME_A], (int)x + 4, (int)y);
			if (luma_actual != luma_reference)
				ok = 0;
		}
	}

	/* Compares the picture with the reference picture shifted by the motion vector. */
	for (y = 0; y < 16U; y++) {
		for (x = 0; x < 16U; x++) {
			cb_actual = cb_at(x, y);
			chroma_reference = ref_chroma(&frames[FRAME_A], 1U, (int)x + 2, (int)y);
			if (cb_actual != chroma_reference)
				ok = 0;
			cr_actual = cr_at(x, y);
			chroma_reference = ref_chroma(&frames[FRAME_A], 2U, (int)x + 2, (int)y);
			if (cr_actual != chroma_reference)
				ok = 0;
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);
	CHECK(mv_is(0, 0, 16, 0));
	CHECK(mbs[3].motion.mv[0][15][0] == 16 && mbs[3].motion.mv[0][15][1] == 0);

	/* A B slice of four skipped macroblocks. */
	slice_env(H264SW_SLICE_B);
	restore_reference();
	stream_start();
	bitwriter_ue(&writer, 4U);
	stream_run(0U);
	CHECK(picture.decoded_mbs == 4U);
	ok = 1;
	for (y = 0; y < 32U; y++) {
		for (x = 0; x < 32U; x++) {
			a = ref_luma(&frames[FRAME_A], (int)x, (int)y);
			c = ref_luma(&frames[FRAME_C], (int)x, (int)y);
			luma_actual = luma_at(x, y);
			if (luma_actual != ((a + c + 1) >> 1))
				ok = 0;
		}
	}

	/* Requires that every value compared above matched. */
	CHECK(ok);
}

/*
 * Random and truncated streams through the loop with the real macroblock
 * layer and reconstruction: the status is 0, EINVAL or ENOTSUP, and the
 * run is clean under the sanitizers.  Every prefix of a valid stream, and random
 * bytes, in I, P and B slices.
 */
static void
check_slice_random(void)
{
	unsigned iteration;
	unsigned slice_type;
	unsigned length;
	unsigned index;
	unsigned total;
	int status;
	uint32_t draw;

	/* Every prefix of the valid stream of four I_PCM macroblocks. */
	slice_env(H264SW_SLICE_I);
	stream_start();
	for (index = 0; index < 4U; index++)
		put_pcm_mb(index);
	bitwriter_trailing(&writer);
	total = (unsigned)bitwriter_bytes(&writer);
	for (length = 0; length <= total; length += 7U) {
		slice_env(H264SW_SLICE_I);
		vcodec_bitreader_init(&ctx.bits, writer.data, length);
		status = h264sw_slice_data(&ctx);
		CHECK(status == 0 || status == EINVAL);
	}

	/* Random bytes in each slice type, with the first bytes shaped like small macroblock types. */
	for (iteration = 0; iteration < 3000U; iteration++) {
		slice_type = H264SW_SLICE_I;
		if (iteration % 3U == 1U)
			slice_type = H264SW_SLICE_P;
		if (iteration % 3U == 2U)
			slice_type = H264SW_SLICE_B;
		slice_env(slice_type);
		length = 1U + random_next() % 400U;
		bitwriter_init(&writer);
		for (index = 0; index < length; index++)
			writer.data[index] = (uint8_t)random_next();
		draw = random_next();
		if (draw % 2U == 0U)
			writer.data[0] = (uint8_t)(0x80U | (random_next() % 128U));
		vcodec_bitreader_init(&ctx.bits, writer.data, length);
		status = h264sw_slice_data(&ctx);
		CHECK(status == 0 || status == EINVAL || status == ENOTSUP);
	}
}
