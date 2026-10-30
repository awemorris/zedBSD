/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the macroblock layer syntax and the slice data loop of the
 * software H.264 decoder (7.3.4, 7.3.5, 7.3.5.1 to 7.3.5.3.1, 7.4.5).
 *
 * The streams are written here with the bit writer from the syntax tables
 * of the specification; the code words of the residual blocks and of
 * coded_block_pattern are those of Tables 9-4, 9-5, 9-7 and 9-9 read from
 * the page images, and the blocks are kept to at most one coefficient so
 * that every nC stays in the first column of Table 9-5.  The CABAC
 * streams are coded with the informative encoder of 9.3.4
 * (cabacwriter.h) under the context indices worked out by hand from
 * Tables 9-34 and 9-39 and 9.3.3.1.1, and the context variables of the
 * encoder and the decoder must end equal, which fails when the parser
 * reads an element with another context than the one the stream was
 * written with.  Expected values are worked out on paper from the tables
 * of 7.4.5 and the equations 7-36 and 7-37.
 *
 * The module's source is included a second time with its slice
 * reconstruction replaced by a stub, so that the slice data loop runs
 * without the reconstruction; the macroblock parsing is also tested
 * through the module linked in alongside.
 */

#include "check.h"
#include "bitwriter.h"
#include "cabacwriter.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "userland/desktop/libmedia/h264sw-bits.h"
#include "userland/desktop/libmedia/h264sw-cabac.h"
#include "userland/desktop/libmedia/h264sw-cabac-se.h"
#include "userland/desktop/libmedia/h264sw-cavlc.h"
#include "userland/desktop/libmedia/h264sw-context.h"
#include "userland/desktop/libmedia/h264sw-macroblock.h"

/* The macroblock parsing and slice data of the included copy, and its stub of the reconstruction. */
#define h264sw_slice_data copy_slice_data
#define h264sw_macroblock_parse copy_macroblock_parse
#define h264sw_recon_mb stub_recon_mb
int copy_macroblock_parse(struct h264sw_slice_ctx *ctx, int skipped);
#include "userland/desktop/libmedia/h264sw-macroblock.c"
#undef h264sw_slice_data
#undef h264sw_macroblock_parse
#undef h264sw_recon_mb

/* The test picture: 3 by 3 macroblocks, and the address of the macroblock most cases parse. */
#define PIC_W 3U
#define PIC_H 3U
#define PIC_MBS 9U
#define ADDR 4U

/* The samples of an I_PCM macroblock. */
#define PCM_BYTES 384U

/* The context shared by the cases: the slice, its header, the picture, its parameter sets and macroblocks. */
static struct h264sw_slice_ctx ctx;
static struct h264sw_slice_header header;
static struct h264sw_picture_ctx picture;
static struct h264sw_sps sps;
static struct h264sw_pps pps;
static struct h264sw_mb mbs[PIC_MBS];

/* The stream being written, and the number of bits of it before the trailing bits. */
static struct bitwriter writer;
static size_t content_bits;

/* The expected levels of the case being checked. */
static struct h264sw_mb_syntax want;

/* The state of the random generator of the fuzz cases. */
static uint32_t random_state = 20261030U;

/* What the stub of the reconstruction saw: the address and kind of each macroblock, its I_PCM samples, and when to fail. */
static unsigned recon_count;
static unsigned recon_addr[PIC_MBS + 1U];
static unsigned recon_kind[PIC_MBS + 1U];
static uint8_t recon_pcm[PIC_MBS + 1U][PCM_BYTES];
static int recon_fail_at = -1;
static int recon_fail_status;

/* The encoder of the CABAC cases and the initial context variables both sides start from. */
static struct cabacwriter encoder;
static uint8_t initial_state[CABACWRITER_CONTEXTS];

/* Forward declarations of the static functions. */
static uint32_t random_next(void);
static void env_make(unsigned slice_type, int cabac, unsigned active0, unsigned active1, int t8x8, int d8inf, int qp, unsigned addr);
static void put_text(const char *text);
static void stream_begin(void);
static void stream_end(void);
static void check_consumed(void);
static void put_block_one(unsigned negative, unsigned pos);
static void put_block_two(unsigned negative);
static void put_chroma_dc_one(unsigned negative, unsigned pos);
static void want_clear(void);
static void check_levels(void);
static void check_unused_motion(unsigned list, unsigned from);
static void put_16x16_empty(int delta);
static void check_i4x4(void);
static void check_i16x16(void);
static void check_pcm(void);
static void check_i8x8_in_p(void);
static void check_p_types(void);
static void check_p_8x8(void);
static void check_transform_flag(void);
static void check_b_types(void);
static void check_b_8x8(void);
static void check_skipped(void);
static void check_qp(void);
static void qp_case(int start, int delta, int expected, int status);
static void check_nc(void);
static void check_errors(void);
static void check_truncated(void);
static void check_fuzz_cavlc(void);
static void check_fuzz_cabac(void);
static void check_slice_cavlc(void);
static void check_slice_errors(void);
static void slice_env(unsigned slice_type, int cabac, unsigned first_mb);
static void cabac_begin(void);
static int cabac_end(unsigned slice_type);
static void check_cabac_pcm(void);
static void check_cabac_skip(void);
static void check_cabac_inter(void);

int
main(
	void)
{
	/* The CAVLC macroblock syntax by kind. */
	check_i4x4();
	check_i16x16();
	check_pcm();
	check_i8x8_in_p();
	check_p_types();
	check_p_8x8();
	check_transform_flag();
	check_b_types();
	check_b_8x8();
	check_skipped();
	check_qp();
	check_nc();

	/* The errors and hostile input. */
	check_errors();
	check_truncated();
	check_fuzz_cavlc();

	/* The slice data loop with the stubbed reconstruction. */
	check_slice_cavlc();
	check_slice_errors();

	/* The CABAC macroblocks and slices. */
	check_cabac_pcm();
	check_cabac_skip();
	check_cabac_inter();
	check_fuzz_cabac();

	/* Reports the result. */
	return check_finish("macroblock");
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

/*
 * Sets up a slice over the 3 by 3 picture: every macroblock is not yet
 * decoded except the one at addr, which belongs to slice 0, and its
 * neighbours are found as the decoder finds them.
 */
static void
env_make(
	unsigned slice_type,
	int cabac,
	unsigned active0,
	unsigned active1,
	int t8x8,
	int d8inf,
	int qp,
	unsigned addr)
{
	unsigned index;

	/* Starts from clean structures. */
	memset(&ctx, 0, sizeof(ctx));
	memset(&header, 0, sizeof(header));
	memset(&picture, 0, sizeof(picture));
	memset(&sps, 0, sizeof(sps));
	memset(&pps, 0, sizeof(pps));
	header.slice_type = slice_type;
	header.num_ref_idx_active[0] = active0;
	header.num_ref_idx_active[1] = active1;
	header.qp = qp;
	pps.transform_8x8_mode = t8x8;
	sps.direct_8x8_inference = d8inf;

	/* Resets every macroblock entry as an undecoded one and puts the current one in slice 0. */
	for (index = 0; index < PIC_MBS; index++)
		h264sw_mb_reset(&mbs[index], -1);
	mbs[addr].slice = 0;

	/* Links the picture and the slice context. */
	picture.mbs = mbs;
	picture.width_mbs = PIC_W;
	picture.height_mbs = PIC_H;
	picture.size_mbs = PIC_MBS;
	picture.pps = &pps;
	picture.sps = &sps;
	ctx.picture = &picture;
	ctx.header = &header;
	ctx.slice_num = 0;
	ctx.cabac_mode = cabac;
	ctx.qp = qp;
	ctx.prev_mb_addr = -1;
	ctx.mb_addr = addr;
	h264sw_mb_neighbours(mbs, PIC_W, addr, &ctx.n);
}

/* Writes a bit string given as characters 0 and 1 (other characters are ignored). */
static void
put_text(
	const char *text)
{
	size_t index;

	/* Writes each 0 or 1 as one bit. */
	for (index = 0; text[index] != '\0'; index++) {
		if (text[index] == '0')
			bitwriter_u(&writer, 0U, 1U);
		if (text[index] == '1')
			bitwriter_u(&writer, 1U, 1U);
	}
}

/* Empties the writer for a new stream. */
static void
stream_begin(
	void)
{
	/* The stream starts empty. */
	bitwriter_init(&writer);
}

/* Ends the stream with the trailing bits and starts the context's bit reader on it. */
static void
stream_end(
	void)
{
	/* The number of bits before the trailing bits is what a complete parse reads. */
	content_bits = writer.bits;
	bitwriter_trailing(&writer);
	CHECK(writer.overflow == 0);

	/* The slice data starts at the first bit. */
	h264sw_bits_init(&ctx.bits, writer.data, bitwriter_bytes(&writer));
}

/* Checks that a parse read exactly the bits of the stream's content and no more. */
static void
check_consumed(void)
{
	/* A parse that reads another number of bits than the stream holds has misread an element. */
	CHECK(ctx.bits.error == 0);
	CHECK(ctx.bits.position == content_bits);
}

/*
 * Writes a residual block with one coefficient of level 1 or -1 at index
 * pos (0..2) of a block of 15 or 16 levels, for nC of 0 or 1:
 * coeff_token TotalCoeff 1 and TrailingOnes 1 is 01 in the first column
 * of Table 9-5, then trailing_ones_sign_flag, then total_zeros with
 * tzVlcIndex 1 from Table 9-7 (0 is 1, 1 is 011, 2 is 010).
 */
static void
put_block_one(
	unsigned negative,
	unsigned pos)
{
	static const char *const zeros[3] = { "1", "011", "010" };

	/* coeff_token, the sign, and the number of zeros before the coefficient. */
	put_text("01");
	bitwriter_u(&writer, negative, 1U);
	put_text(zeros[pos]);
}

/*
 * Writes a block with one coefficient of level 2 or -2 at index 0
 * (nC 0 or 1): coeff_token TotalCoeff 1 and TrailingOnes 0 is 000101;
 * level_prefix 0 gives levelCode 0, +2 for the first level after fewer
 * than three trailing ones, so the level is 2; level_prefix 1 gives
 * levelCode 1 + 2 = 3, so the level is -2 (9.2.2.1); then total_zeros 0.
 */
static void
put_block_two(
	unsigned negative)
{
	/* coeff_token, level_prefix, and total_zeros 0. */
	put_text("000101");
	if (negative != 0U) {
		put_text("01");
	} else {
		put_text("1");
	}
	put_text("1");
}

/*
 * Writes a chroma DC block (nC -1) with one trailing one of level 1 or
 * -1 at index pos (0..3): coeff_token 1 (Table 9-5, nC -1, TotalCoeff 1,
 * TrailingOnes 1), the sign, and total_zeros from Table 9-9 (a) with
 * tzVlcIndex 1 (0 is 1, 1 is 01, 2 is 001, 3 is 000).
 */
static void
put_chroma_dc_one(
	unsigned negative,
	unsigned pos)
{
	static const char *const zeros[4] = { "1", "01", "001", "000" };

	/* coeff_token, the sign, and the number of zeros before the coefficient. */
	put_text("1");
	bitwriter_u(&writer, negative, 1U);
	put_text(zeros[pos]);
}

/* Clears the expected levels. */
static void
want_clear(void)
{
	/* Every level not set by the case is 0. */
	memset(&want, 0, sizeof(want));
}

/* Checks every level array of the syntax against the expected levels. */
static void
check_levels(void)
{
	/* All five level arrays, so a stale or misplaced level shows. */
	CHECK(memcmp(ctx.syntax.luma_dc, want.luma_dc, sizeof(want.luma_dc)) == 0);
	CHECK(memcmp(ctx.syntax.luma, want.luma, sizeof(want.luma)) == 0);
	CHECK(memcmp(ctx.syntax.luma8x8, want.luma8x8, sizeof(want.luma8x8)) == 0);
	CHECK(memcmp(ctx.syntax.chroma_dc, want.chroma_dc, sizeof(want.chroma_dc)) == 0);
	CHECK(memcmp(ctx.syntax.chroma_ac, want.chroma_ac, sizeof(want.chroma_ac)) == 0);
}

/* Checks that the entry's motion of a list is unused (-1) from the given quadrant on and has no vectors. */
static void
check_unused_motion(
	unsigned list,
	unsigned from)
{
	unsigned quadrant;

	/* A list the macroblock does not use keeps ref_idx -1. */
	for (quadrant = from; quadrant < 4U; quadrant++)
		CHECK(mbs[ADDR].motion.ref_idx[list][quadrant] == -1);
}

/* Writes an Intra_16x16 macroblock (mb_type 1: mode 0, no chroma, no luma AC) with an empty DC block, for the QP and nC cases. */
static void
put_16x16_empty(
	int delta)
{
	/* mb_type 1, intra_chroma_pred_mode 0, mb_qp_delta, and the Intra_16x16 DC block with no coefficients. */
	bitwriter_ue(&writer, 1U);
	bitwriter_ue(&writer, 0U);
	bitwriter_se(&writer, delta);
	put_text("1");
}

/*
 * An I_NxN macroblock without transform_size_8x8_flag in the PPS: 16
 * prev_intra4x4_pred_mode_flag and rem_intra4x4_pred_mode values, the
 * chroma mode, coded_block_pattern with codeNum 29 (Intra column of Table
 * 9-4: 1, the first 8x8 block only), mb_qp_delta -3 and the four 4x4 blocks.
 */
static void
check_i4x4(void)
{
	unsigned index;
	int status;

	env_make(H264SW_SLICE_I, 0, 1, 0, 0, 0, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 0U);
	for (index = 0; index < 16U; index++) {
		/* Even blocks use the predicted mode, odd blocks send rem = index / 2. */
		if (index % 2U == 0U) {
			bitwriter_u(&writer, 1U, 1U);
		} else {
			bitwriter_u(&writer, 0U, 1U);
			bitwriter_u(&writer, index >> 1, 3U);
		}
	}
	bitwriter_ue(&writer, 2U);
	bitwriter_ue(&writer, 29U);
	bitwriter_se(&writer, -3);

	/* Block 0: level 1; block 1: none; block 2: level -2; block 3: level 1 at index 2. */
	put_block_one(0U, 0U);
	put_text("1");
	put_block_two(1U);
	put_block_one(0U, 2U);
	stream_end();

	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();

	/* The kind: I_NxN without the 8x8 flag is Intra_4x4, in the entry and the syntax. */
	CHECK(ctx.syntax.mb_type == 0U);
	CHECK(ctx.syntax.type.kind == H264SW_MB_I_4X4);
	CHECK(mbs[ADDR].kind == H264SW_MB_I_4X4);
	CHECK(mbs[ADDR].transform_8x8 == 0U);
	CHECK(mbs[ADDR].motion.intra == 1U);

	/* The prediction modes as written. */
	for (index = 0; index < 16U; index++) {
		if (index % 2U == 0U) {
			CHECK(ctx.syntax.prev_intra_flag[index] == 1U);
			CHECK(ctx.syntax.rem_intra_mode[index] == 0U);
		} else {
			CHECK(ctx.syntax.prev_intra_flag[index] == 0U);
			CHECK(ctx.syntax.rem_intra_mode[index] == (index >> 1));
		}
	}
	CHECK(mbs[ADDR].chroma_pred_mode == 2U);

	/* The pattern, the quantizer (26 - 3 by 7-37), and the counts for the nC of the next blocks. */
	CHECK(ctx.syntax.cbp_luma == 1U);
	CHECK(ctx.syntax.cbp_chroma == 0U);
	CHECK(mbs[ADDR].cbp == 1U);
	CHECK(ctx.syntax.qp_delta == -3);
	CHECK(mbs[ADDR].qp == 23U);
	CHECK(ctx.qp == 23);
	CHECK(ctx.prev_qp_delta_nonzero == 1);
	CHECK(mbs[ADDR].total_coeff[0] == 1U);
	CHECK(mbs[ADDR].total_coeff[1] == 0U);
	CHECK(mbs[ADDR].total_coeff[2] == 1U);
	CHECK(mbs[ADDR].total_coeff[3] == 1U);
	CHECK(mbs[ADDR].total_coeff[4] == 0U);

	/* The levels. */
	want_clear();
	want.luma[0][0] = 1;
	want.luma[2][0] = -2;
	want.luma[3][2] = 1;
	check_levels();

	/* Nothing is predicted from another picture. */
	check_unused_motion(0U, 0U);
	check_unused_motion(1U, 0U);
}

/*
 * An Intra_16x16 macroblock: mb_type 18 is I_16x16_1_1_1 (7-... Table
 * 7-11: 1 + mode 1 + 4 * chroma 1 + 12 for the luma AC), so the pattern is
 * luma 15 and chroma 1 without being sent.  The DC block has a level at
 * index 1, block 0 of the AC has one at its first position, Cb's DC one
 * at index 0 and Cr's DC none.
 */
static void
check_i16x16(void)
{
	unsigned index;
	int status;

	env_make(H264SW_SLICE_I, 0, 1, 0, 1, 0, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 18U);
	bitwriter_ue(&writer, 1U);
	bitwriter_se(&writer, 2);
	put_block_one(0U, 1U);
	put_block_one(1U, 0U);
	for (index = 1; index < 16U; index++)
		put_text("1");
	put_chroma_dc_one(0U, 0U);
	put_text("01");
	stream_end();

	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();

	/* The kind, the mode and the pattern come from mb_type; the 8x8 flag is never sent for Intra_16x16 even when the PPS allows it. */
	CHECK(ctx.syntax.type.kind == H264SW_MB_I_16X16);
	CHECK(ctx.syntax.type.intra16x16_mode == 1U);
	CHECK(ctx.syntax.cbp_luma == 15U);
	CHECK(ctx.syntax.cbp_chroma == 1U);
	CHECK(mbs[ADDR].kind == H264SW_MB_I_16X16);
	CHECK(mbs[ADDR].cbp == 0x1fU);
	CHECK(mbs[ADDR].transform_8x8 == 0U);
	CHECK(mbs[ADDR].chroma_pred_mode == 1U);
	CHECK(mbs[ADDR].qp == 28U);
	CHECK(ctx.qp == 28);

	/* The DC block has no entry among the counts; the AC block's count is recorded. */
	CHECK(mbs[ADDR].total_coeff[0] == 1U);
	CHECK(mbs[ADDR].total_coeff[1] == 0U);

	/* The AC levels sit at 1..15 of their blocks, entry 0 stays 0. */
	want_clear();
	want.luma_dc[1] = 1;
	want.luma[0][1] = -1;
	want.chroma_dc[0][0] = 1;
	check_levels();
}

/*
 * An I_PCM macroblock: mb_type 25, the alignment bits, 384 samples.  The
 * entry records what the neighbours' contexts need of one, QP 0 for the
 * deblocking filter, and the slice's QP carries over.
 */
static void
check_pcm(void)
{
	unsigned index;
	int status;

	env_make(H264SW_SLICE_I, 0, 1, 0, 0, 0, 30, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 25U);
	while (writer.bits % 8U != 0U)
		bitwriter_u(&writer, 0U, 1U);
	for (index = 0; index < PCM_BYTES; index++)
		bitwriter_u(&writer, (index * 7U + 3U) & 255U, 8U);
	stream_end();
	ctx.prev_qp_delta_nonzero = 1;

	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();

	/* The samples in raster order, as written. */
	CHECK(ctx.syntax.type.kind == H264SW_MB_I_PCM);
	for (index = 0; index < PCM_BYTES; index++)
		CHECK(ctx.syntax.pcm[index] == ((index * 7U + 3U) & 255U));

	/* The entry: all coded for the contexts, 16 coefficients in every block for nC, QP 0, intra. */
	CHECK(mbs[ADDR].kind == H264SW_MB_I_PCM);
	CHECK(mbs[ADDR].cbp == 0x2fU);
	CHECK(mbs[ADDR].qp == 0U);
	CHECK(mbs[ADDR].coded_flags == 0x07ffffffU);
	for (index = 0; index < 24U; index++)
		CHECK(mbs[ADDR].total_coeff[index] == 16U);
	CHECK(mbs[ADDR].motion.intra == 1U);

	/* QPY carries over, and the next mb_qp_delta sees none. */
	CHECK(ctx.qp == 30);
	CHECK(ctx.prev_qp_delta_nonzero == 0);
}

/*
 * An I_NxN macroblock in a P slice (mb_type 5) with the 8x8 transform
 * allowed: transform_size_8x8_flag follows mb_type, so the macroblock is
 * Intra_8x8 with four mode flags; coded_block_pattern codeNum 4 is 23 (luma
 * 7, chroma 1); the 8x8 blocks of CAVLC are four interleaved 4x4 lists.
 */
static void
check_i8x8_in_p(void)
{
	unsigned index;
	int status;

	env_make(H264SW_SLICE_P, 0, 1, 0, 1, 0, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 5U);
	bitwriter_u(&writer, 1U, 1U);
	put_text("1");
	put_text("0101");
	put_text("1");
	put_text("0111");
	bitwriter_ue(&writer, 3U);
	bitwriter_ue(&writer, 4U);
	bitwriter_se(&writer, 1);

	/* Quadrant 0: lists 0 to 3 (list 1 has level -1 at index 0); quadrant 1: all empty; quadrant 2: list 3 has level 1 at index 1. */
	put_text("1");
	put_block_one(1U, 0U);
	put_text("1");
	put_text("1");
	put_text("1111");
	put_text("111");
	put_block_one(0U, 1U);

	/* Chroma DC: Cb has none, Cr a level 1 at index 3. */
	put_text("01");
	put_chroma_dc_one(0U, 3U);
	stream_end();

	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();

	CHECK(ctx.syntax.mb_type == 5U);
	CHECK(ctx.syntax.type.kind == H264SW_MB_I_8X8);
	CHECK(mbs[ADDR].kind == H264SW_MB_I_8X8);
	CHECK(mbs[ADDR].transform_8x8 == 1U);
	CHECK(mbs[ADDR].motion.intra == 1U);

	/* The four mode flags: 1, then rem 5, 1, then rem 7. */
	CHECK(ctx.syntax.prev_intra_flag[0] == 1U);
	CHECK(ctx.syntax.prev_intra_flag[1] == 0U);
	CHECK(ctx.syntax.rem_intra_mode[1] == 5U);
	CHECK(ctx.syntax.prev_intra_flag[2] == 1U);
	CHECK(ctx.syntax.prev_intra_flag[3] == 0U);
	CHECK(ctx.syntax.rem_intra_mode[3] == 7U);
	CHECK(mbs[ADDR].chroma_pred_mode == 3U);
	CHECK(ctx.syntax.cbp_luma == 7U);
	CHECK(ctx.syntax.cbp_chroma == 1U);
	CHECK(mbs[ADDR].cbp == 23U);
	CHECK(mbs[ADDR].qp == 27U);

	/* The counts of the 4x4 parts, and the interleaving: list k's level i is level 4 * i + k of the 8x8 block. */
	CHECK(mbs[ADDR].total_coeff[1] == 1U);
	CHECK(mbs[ADDR].total_coeff[11] == 1U);
	for (index = 0; index < 16U; index++) {
		if (index != 1U && index != 11U)
			CHECK(mbs[ADDR].total_coeff[index] == 0U);
	}
	want_clear();
	want.luma8x8[0][1] = -1;
	want.luma8x8[2][7] = 1;
	want.chroma_dc[1][3] = 1;
	check_levels();
}

/*
 * The P macroblock types with partitions of 16x16, 16x8 and 8x16 and
 * their reference indices and motion vector differences, in the order of
 * 7.3.5.1 (all ref_idx_l0, then all mvd_l0).
 */
static void
check_p_types(void)
{
	unsigned blk;
	int status;

	/* P_L0_16x16 with one reference: no ref_idx is sent, and the index used is 0. */
	env_make(H264SW_SLICE_P, 0, 1, 0, 0, 0, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 0U);
	bitwriter_se(&writer, 3);
	bitwriter_se(&writer, -2);
	bitwriter_ue(&writer, 0U);
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();
	CHECK(ctx.syntax.type.kind == H264SW_MB_P);
	CHECK(mbs[ADDR].kind == H264SW_MB_P);
	CHECK(ctx.syntax.ref_idx[0][0] == 0);
	CHECK(ctx.syntax.ref_idx[0][1] == -1);
	CHECK(ctx.syntax.ref_idx[1][0] == -1);
	CHECK(ctx.syntax.mvd[0][0][0][0] == 3);
	CHECK(ctx.syntax.mvd[0][0][0][1] == -2);
	for (blk = 0; blk < 4U; blk++)
		CHECK(mbs[ADDR].motion.ref_idx[0][blk] == 0);
	check_unused_motion(1U, 0U);
	for (blk = 0; blk < 16U; blk++) {
		CHECK(mbs[ADDR].mvd[0][blk][0] == 3U);
		CHECK(mbs[ADDR].mvd[0][blk][1] == 2U);
		CHECK(mbs[ADDR].mvd[1][blk][0] == 0U);
	}
	CHECK(mbs[ADDR].cbp == 0U);
	CHECK(mbs[ADDR].qp == 26U);
	CHECK(ctx.syntax.qp_delta == 0);
	CHECK(ctx.prev_qp_delta_nonzero == 0);
	CHECK(mbs[ADDR].motion.intra == 0U);

	/* P_L0_L0_16x8 with three references: te(v) is ue(v) for a largest value of 2; the second difference is capped at 255 in the entry; cbp 1 (inter codeNum 2) brings mb_qp_delta and the first 8x8 block's four blocks. */
	env_make(H264SW_SLICE_P, 0, 3, 0, 0, 0, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 1U);
	bitwriter_ue(&writer, 2U);
	bitwriter_ue(&writer, 1U);
	bitwriter_se(&writer, 1);
	bitwriter_se(&writer, 1);
	bitwriter_se(&writer, -1);
	bitwriter_se(&writer, 300);
	bitwriter_ue(&writer, 2U);
	bitwriter_se(&writer, 1);
	put_text("1111");
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();
	CHECK(ctx.syntax.type.parts == 2U);
	CHECK(ctx.syntax.ref_idx[0][0] == 2);
	CHECK(ctx.syntax.ref_idx[0][1] == 1);
	CHECK(mbs[ADDR].motion.ref_idx[0][0] == 2);
	CHECK(mbs[ADDR].motion.ref_idx[0][1] == 2);
	CHECK(mbs[ADDR].motion.ref_idx[0][2] == 1);
	CHECK(mbs[ADDR].motion.ref_idx[0][3] == 1);
	CHECK(ctx.syntax.mvd[0][0][0][0] == 1);
	CHECK(ctx.syntax.mvd[0][0][0][1] == 1);
	CHECK(ctx.syntax.mvd[0][1][0][0] == -1);
	CHECK(ctx.syntax.mvd[0][1][0][1] == 300);
	for (blk = 0; blk < 16U; blk++) {
		CHECK(mbs[ADDR].mvd[0][blk][0] == 1U);
		if (h264sw_blk4x4_y[blk] < 8U) {
			CHECK(mbs[ADDR].mvd[0][blk][1] == 1U);
		} else {
			CHECK(mbs[ADDR].mvd[0][blk][1] == 255U);
		}
	}
	CHECK(mbs[ADDR].cbp == 1U);
	CHECK(mbs[ADDR].qp == 27U);
	CHECK(ctx.prev_qp_delta_nonzero == 1);

	/* P_L0_L0_8x16 with two references: te(v) is one inverted bit for a largest value of 1 (0 is 1, 1 is 0). */
	env_make(H264SW_SLICE_P, 0, 2, 0, 0, 0, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 2U);
	put_text("01");
	bitwriter_se(&writer, 2);
	bitwriter_se(&writer, 3);
	bitwriter_se(&writer, -3);
	bitwriter_se(&writer, -4);
	bitwriter_ue(&writer, 0U);
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();
	CHECK(ctx.syntax.ref_idx[0][0] == 1);
	CHECK(ctx.syntax.ref_idx[0][1] == 0);
	CHECK(mbs[ADDR].motion.ref_idx[0][0] == 1);
	CHECK(mbs[ADDR].motion.ref_idx[0][1] == 0);
	CHECK(mbs[ADDR].motion.ref_idx[0][2] == 1);
	CHECK(mbs[ADDR].motion.ref_idx[0][3] == 0);
	for (blk = 0; blk < 16U; blk++) {
		if (h264sw_blk4x4_x[blk] < 8U) {
			CHECK(mbs[ADDR].mvd[0][blk][0] == 2U);
			CHECK(mbs[ADDR].mvd[0][blk][1] == 3U);
		} else {
			CHECK(mbs[ADDR].mvd[0][blk][0] == 3U);
			CHECK(mbs[ADDR].mvd[0][blk][1] == 4U);
		}
	}

	/* P_8x8ref0 (mb_type 4) sends no ref_idx even with three references, and uses index 0. */
	env_make(H264SW_SLICE_P, 0, 3, 0, 0, 0, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 4U);
	for (blk = 0; blk < 4U; blk++)
		bitwriter_ue(&writer, 0U);
	for (blk = 0; blk < 8U; blk++)
		bitwriter_se(&writer, 0);
	bitwriter_ue(&writer, 0U);
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();
	CHECK(ctx.syntax.type.kind == H264SW_MB_P_8X8REF0);
	CHECK(mbs[ADDR].kind == H264SW_MB_P_8X8REF0);
	for (blk = 0; blk < 4U; blk++) {
		CHECK(ctx.syntax.ref_idx[0][blk] == 0);
		CHECK(mbs[ADDR].motion.ref_idx[0][blk] == 0);
	}
}

/*
 * P_8x8 with sub-macroblocks of every shape: 8x8, 8x4, 4x8 and 4x4 (Table
 * 7-17), each partition's mvd at its place, and the 8x8 transform flag
 * which is read after coded_block_pattern only when no sub-macroblock is
 * split below 8x8.
 */
static void
check_p_8x8(void)
{
	static const unsigned blocks[16][2] = {
		{ 1U, 2U }, { 1U, 2U }, { 1U, 2U }, { 1U, 2U },
		{ 3U, 4U }, { 3U, 4U }, { 5U, 6U }, { 5U, 6U },
		{ 1U, 2U }, { 3U, 4U }, { 1U, 2U }, { 3U, 4U },
		{ 7U, 8U }, { 9U, 10U }, { 11U, 12U }, { 13U, 14U }
	};
	unsigned blk;
	unsigned index;
	int status;

	/* The four shapes, with their mvds as written below; the absolute values are what the entry keeps in each 4x4 block (blocks 4, 5 then 6, 7 for the 8x4 halves; 8, 10 then 9, 11 for the 4x8 halves). */
	env_make(H264SW_SLICE_P, 0, 1, 0, 0, 0, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 3U);
	for (index = 0; index < 4U; index++)
		bitwriter_ue(&writer, index);
	bitwriter_se(&writer, 1);
	bitwriter_se(&writer, 2);
	bitwriter_se(&writer, 3);
	bitwriter_se(&writer, 4);
	bitwriter_se(&writer, 5);
	bitwriter_se(&writer, 6);
	bitwriter_se(&writer, -1);
	bitwriter_se(&writer, -2);
	bitwriter_se(&writer, -3);
	bitwriter_se(&writer, -4);
	for (index = 7; index < 15U; index++)
		bitwriter_se(&writer, (int32_t)index);
	bitwriter_ue(&writer, 0U);
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();
	CHECK(ctx.syntax.type.kind == H264SW_MB_P_8X8);
	CHECK(ctx.syntax.sub_mb_type[0] == 0U);
	CHECK(ctx.syntax.sub_mb_type[3] == 3U);
	CHECK(ctx.syntax.sub[1].parts == 2U);
	CHECK(ctx.syntax.sub[1].part_width == 8U);
	CHECK(ctx.syntax.sub[1].part_height == 4U);
	CHECK(ctx.syntax.sub[2].part_width == 4U);
	CHECK(ctx.syntax.sub[3].parts == 4U);
	CHECK(ctx.syntax.mvd[0][0][0][0] == 1);
	CHECK(ctx.syntax.mvd[0][1][0][1] == 4);
	CHECK(ctx.syntax.mvd[0][1][1][0] == 5);
	CHECK(ctx.syntax.mvd[0][2][0][0] == -1);
	CHECK(ctx.syntax.mvd[0][2][1][1] == -4);
	CHECK(ctx.syntax.mvd[0][3][0][0] == 7);
	CHECK(ctx.syntax.mvd[0][3][1][0] == 9);
	CHECK(ctx.syntax.mvd[0][3][2][0] == 11);
	CHECK(ctx.syntax.mvd[0][3][3][1] == 14);
	for (blk = 0; blk < 16U; blk++) {
		CHECK(mbs[ADDR].mvd[0][blk][0] == blocks[blk][0]);
		CHECK(mbs[ADDR].mvd[0][blk][1] == blocks[blk][1]);
	}
	for (blk = 0; blk < 4U; blk++) {
		CHECK(ctx.syntax.ref_idx[0][blk] == 0);
		CHECK(mbs[ADDR].motion.ref_idx[0][blk] == 0);
	}
	CHECK(mbs[ADDR].direct == 0U);

	/* A P_8x8 of four 8x8 sub-macroblocks with the 8x8 transform allowed and luma coded: the flag follows the pattern (here 1), then mb_qp_delta 0 and the first quadrant's four interleaved lists (levels at list 0 index 1, list 1 index 0, list 3 index 2). */
	env_make(H264SW_SLICE_P, 0, 1, 0, 1, 0, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 3U);
	for (index = 0; index < 4U; index++)
		bitwriter_ue(&writer, 0U);
	for (index = 0; index < 8U; index++)
		bitwriter_se(&writer, 0);
	bitwriter_ue(&writer, 2U);
	put_text("1");
	bitwriter_se(&writer, 0);
	put_block_one(0U, 1U);
	put_block_one(1U, 0U);
	put_text("1");
	put_block_one(0U, 2U);
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();
	CHECK(mbs[ADDR].transform_8x8 == 1U);
	CHECK(mbs[ADDR].cbp == 1U);
	want_clear();
	want.luma8x8[0][4] = 1;
	want.luma8x8[0][1] = -1;
	want.luma8x8[0][11] = 1;
	check_levels();

	/* The same with one sub-macroblock split in 8x4: the flag is not sent, so the first quadrant's four blocks are 4x4 blocks. */
	env_make(H264SW_SLICE_P, 0, 1, 0, 1, 0, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 3U);
	bitwriter_ue(&writer, 0U);
	bitwriter_ue(&writer, 1U);
	bitwriter_ue(&writer, 0U);
	bitwriter_ue(&writer, 0U);
	for (index = 0; index < 10U; index++)
		bitwriter_se(&writer, 0);
	bitwriter_ue(&writer, 2U);
	bitwriter_se(&writer, 0);
	put_text("1111");
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();
	CHECK(mbs[ADDR].transform_8x8 == 0U);
}

/*
 * transform_size_8x8_flag for an inter 16x16 macroblock: read after the
 * pattern when luma is coded and transform_8x8_mode_flag is 1 (7.3.5), and
 * not read for a pattern with chroma only or when the PPS does not allow it.
 */
static void
check_transform_flag(void)
{
	int status;

	/* Luma coded, mode allowed: the flag is read (1) and the quadrant's four lists follow. */
	env_make(H264SW_SLICE_P, 0, 1, 0, 1, 0, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 0U);
	bitwriter_se(&writer, 0);
	bitwriter_se(&writer, 0);
	bitwriter_ue(&writer, 2U);
	put_text("1");
	bitwriter_se(&writer, 0);
	put_text("1111");
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();
	CHECK(mbs[ADDR].transform_8x8 == 1U);

	/* The same stream with the flag 0: four 4x4 blocks. */
	env_make(H264SW_SLICE_P, 0, 1, 0, 1, 0, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 0U);
	bitwriter_se(&writer, 0);
	bitwriter_se(&writer, 0);
	bitwriter_ue(&writer, 2U);
	put_text("0");
	bitwriter_se(&writer, 0);
	put_text("1111");
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();
	CHECK(mbs[ADDR].transform_8x8 == 0U);

	/* Chroma coded and no luma (inter codeNum 1 is pattern 16): no flag even though the mode is allowed; the two chroma DC blocks follow mb_qp_delta. */
	env_make(H264SW_SLICE_P, 0, 1, 0, 1, 0, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 0U);
	bitwriter_se(&writer, 0);
	bitwriter_se(&writer, 0);
	bitwriter_ue(&writer, 1U);
	bitwriter_se(&writer, 0);
	put_text("01");
	put_text("01");
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();
	CHECK(mbs[ADDR].transform_8x8 == 0U);
	CHECK(ctx.syntax.cbp_chroma == 1U);

	/* Luma coded but the PPS does not allow the 8x8 transform: no flag. */
	env_make(H264SW_SLICE_P, 0, 1, 0, 0, 0, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 0U);
	bitwriter_se(&writer, 0);
	bitwriter_se(&writer, 0);
	bitwriter_ue(&writer, 2U);
	bitwriter_se(&writer, 0);
	put_text("1111");
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();
	CHECK(mbs[ADDR].transform_8x8 == 0U);
}

/*
 * The B macroblock types: B_Direct_16x16 (direct quadrants, and the 8x8
 * flag needing direct_8x8_inference_flag) and a type with a bi-predicted
 * second partition, whose lists are read in the order of 7.3.5.1.
 */
static void
check_b_types(void)
{
	unsigned blk;
	int status;

	/* B_Direct_16x16 with direct_8x8_inference_flag 1 and luma coded: the flag is read. */
	env_make(H264SW_SLICE_B, 0, 1, 1, 1, 1, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 0U);
	bitwriter_ue(&writer, 2U);
	put_text("1");
	bitwriter_se(&writer, 0);
	put_text("1111");
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();
	CHECK(ctx.syntax.type.kind == H264SW_MB_B_DIRECT);
	CHECK(mbs[ADDR].kind == H264SW_MB_B_DIRECT);
	CHECK(mbs[ADDR].direct == 0x0fU);
	CHECK(mbs[ADDR].transform_8x8 == 1U);
	for (blk = 0; blk < 4U; blk++) {
		CHECK(ctx.syntax.ref_idx[0][blk] == -1);
		CHECK(ctx.syntax.ref_idx[1][blk] == -1);
	}
	check_unused_motion(0U, 0U);
	check_unused_motion(1U, 0U);

	/* With direct_8x8_inference_flag 0 the flag is not read: four 4x4 blocks. */
	env_make(H264SW_SLICE_B, 0, 1, 1, 1, 0, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 0U);
	bitwriter_ue(&writer, 2U);
	bitwriter_se(&writer, 0);
	put_text("1111");
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();
	CHECK(mbs[ADDR].transform_8x8 == 0U);
	CHECK(mbs[ADDR].direct == 0x0fU);

	/* B_L0_Bi_16x8 (mb_type 12) with two references per list: ref_idx_l0 of both partitions, ref_idx_l1 of the second, then mvd_l0 of both and mvd_l1 of the second. */
	env_make(H264SW_SLICE_B, 0, 2, 2, 0, 0, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 12U);
	put_text("0");
	put_text("1");
	put_text("0");
	bitwriter_se(&writer, 1);
	bitwriter_se(&writer, 2);
	bitwriter_se(&writer, 3);
	bitwriter_se(&writer, 4);
	bitwriter_se(&writer, 5);
	bitwriter_se(&writer, 6);
	bitwriter_ue(&writer, 0U);
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();
	CHECK(ctx.syntax.type.kind == H264SW_MB_B);
	CHECK(ctx.syntax.ref_idx[0][0] == 1);
	CHECK(ctx.syntax.ref_idx[0][1] == 0);
	CHECK(ctx.syntax.ref_idx[0][2] == -1);
	CHECK(ctx.syntax.ref_idx[1][0] == -1);
	CHECK(ctx.syntax.ref_idx[1][1] == 1);
	CHECK(mbs[ADDR].motion.ref_idx[0][0] == 1);
	CHECK(mbs[ADDR].motion.ref_idx[0][1] == 1);
	CHECK(mbs[ADDR].motion.ref_idx[0][2] == 0);
	CHECK(mbs[ADDR].motion.ref_idx[0][3] == 0);
	CHECK(mbs[ADDR].motion.ref_idx[1][0] == -1);
	CHECK(mbs[ADDR].motion.ref_idx[1][1] == -1);
	CHECK(mbs[ADDR].motion.ref_idx[1][2] == 1);
	CHECK(mbs[ADDR].motion.ref_idx[1][3] == 1);
	CHECK(ctx.syntax.mvd[0][0][0][0] == 1);
	CHECK(ctx.syntax.mvd[0][0][0][1] == 2);
	CHECK(ctx.syntax.mvd[0][1][0][0] == 3);
	CHECK(ctx.syntax.mvd[0][1][0][1] == 4);
	CHECK(ctx.syntax.mvd[1][0][0][0] == 0);
	CHECK(ctx.syntax.mvd[1][1][0][0] == 5);
	CHECK(ctx.syntax.mvd[1][1][0][1] == 6);
	for (blk = 0; blk < 16U; blk++) {
		if (h264sw_blk4x4_y[blk] < 8U) {
			CHECK(mbs[ADDR].mvd[0][blk][0] == 1U);
			CHECK(mbs[ADDR].mvd[1][blk][0] == 0U);
		} else {
			CHECK(mbs[ADDR].mvd[0][blk][0] == 3U);
			CHECK(mbs[ADDR].mvd[1][blk][0] == 5U);
			CHECK(mbs[ADDR].mvd[1][blk][1] == 6U);
		}
	}
}

/*
 * B_8x8 with a direct sub-macroblock, a list 1, a bi-predicted and a
 * list 0 4x4 sub-macroblock (sub_mb_type 0, 2, 3 and 10 of Table 7-18):
 * what is read for each list and in which order, the direct quadrant
 * left alone, and the 8x8 transform flag which depends on
 * direct_8x8_inference_flag when a sub-macroblock is direct.
 */
static void
check_b_8x8(void)
{
	unsigned blk;
	int status;

	/* The four sub-macroblocks as above, two references per list. */
	env_make(H264SW_SLICE_B, 0, 2, 2, 0, 0, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 22U);
	bitwriter_ue(&writer, 0U);
	bitwriter_ue(&writer, 2U);
	bitwriter_ue(&writer, 3U);
	bitwriter_ue(&writer, 10U);
	put_text("01");
	put_text("01");
	bitwriter_se(&writer, 1);
	bitwriter_se(&writer, -1);
	bitwriter_se(&writer, 2);
	bitwriter_se(&writer, 3);
	bitwriter_se(&writer, 4);
	bitwriter_se(&writer, 5);
	bitwriter_se(&writer, 6);
	bitwriter_se(&writer, 7);
	bitwriter_se(&writer, 8);
	bitwriter_se(&writer, 9);
	bitwriter_se(&writer, 10);
	bitwriter_se(&writer, 11);
	bitwriter_se(&writer, 12);
	bitwriter_se(&writer, 13);
	bitwriter_ue(&writer, 0U);
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();
	CHECK(ctx.syntax.type.kind == H264SW_MB_B_8X8);
	CHECK(mbs[ADDR].kind == H264SW_MB_B_8X8);
	CHECK(ctx.syntax.sub[0].pred == H264SW_PRED_DIRECT);
	CHECK(ctx.syntax.sub[1].pred == H264SW_PRED_L1);
	CHECK(ctx.syntax.sub[2].pred == H264SW_PRED_BI);
	CHECK(ctx.syntax.sub[3].parts == 4U);
	CHECK(mbs[ADDR].direct == 1U);

	/* ref_idx_l0: the bi-predicted (index 1) and the list 0 4x4 (index 0); ref_idx_l1: the list 1 (index 1) and the bi-predicted (index 0); the direct quadrant has none. */
	CHECK(ctx.syntax.ref_idx[0][0] == -1);
	CHECK(ctx.syntax.ref_idx[0][1] == -1);
	CHECK(ctx.syntax.ref_idx[0][2] == 1);
	CHECK(ctx.syntax.ref_idx[0][3] == 0);
	CHECK(ctx.syntax.ref_idx[1][0] == -1);
	CHECK(ctx.syntax.ref_idx[1][1] == 1);
	CHECK(ctx.syntax.ref_idx[1][2] == 0);
	CHECK(ctx.syntax.ref_idx[1][3] == -1);
	CHECK(mbs[ADDR].motion.ref_idx[0][0] == -1);
	CHECK(mbs[ADDR].motion.ref_idx[0][1] == -1);
	CHECK(mbs[ADDR].motion.ref_idx[0][2] == 1);
	CHECK(mbs[ADDR].motion.ref_idx[0][3] == 0);
	CHECK(mbs[ADDR].motion.ref_idx[1][0] == -1);
	CHECK(mbs[ADDR].motion.ref_idx[1][1] == 1);
	CHECK(mbs[ADDR].motion.ref_idx[1][2] == 0);
	CHECK(mbs[ADDR].motion.ref_idx[1][3] == -1);

	/* mvd_l0: quadrant 2 (one partition), quadrant 3 (four); mvd_l1: quadrants 1 and 2. */
	CHECK(ctx.syntax.mvd[0][2][0][0] == 1);
	CHECK(ctx.syntax.mvd[0][2][0][1] == -1);
	CHECK(ctx.syntax.mvd[0][3][0][0] == 2);
	CHECK(ctx.syntax.mvd[0][3][3][1] == 9);
	CHECK(ctx.syntax.mvd[1][1][0][0] == 10);
	CHECK(ctx.syntax.mvd[1][1][0][1] == 11);
	CHECK(ctx.syntax.mvd[1][2][0][0] == 12);
	CHECK(ctx.syntax.mvd[1][2][0][1] == 13);
	CHECK(ctx.syntax.mvd[0][0][0][0] == 0);
	for (blk = 0; blk < 16U; blk++) {
		if (blk >= 8U && blk < 12U) {
			CHECK(mbs[ADDR].mvd[0][blk][0] == 1U);
			CHECK(mbs[ADDR].mvd[0][blk][1] == 1U);
			CHECK(mbs[ADDR].mvd[1][blk][0] == 12U);
		} else if (blk >= 12U) {
			CHECK(mbs[ADDR].mvd[0][blk][0] == 2U + 2U * (blk - 12U));
			CHECK(mbs[ADDR].mvd[1][blk][0] == 0U);
		} else if (blk >= 4U && blk < 8U) {
			CHECK(mbs[ADDR].mvd[1][blk][0] == 10U);
			CHECK(mbs[ADDR].mvd[1][blk][1] == 11U);
		} else {
			CHECK(mbs[ADDR].mvd[0][blk][0] == 0U);
		}
	}

	/* A direct sub-macroblock with direct_8x8_inference_flag 1 and the other three 8x8: the flag is read after the pattern. */
	env_make(H264SW_SLICE_B, 0, 1, 1, 1, 1, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 22U);
	bitwriter_ue(&writer, 0U);
	bitwriter_ue(&writer, 1U);
	bitwriter_ue(&writer, 1U);
	bitwriter_ue(&writer, 1U);
	bitwriter_se(&writer, 0);
	bitwriter_se(&writer, 0);
	bitwriter_se(&writer, 0);
	bitwriter_se(&writer, 0);
	bitwriter_se(&writer, 0);
	bitwriter_se(&writer, 0);
	bitwriter_ue(&writer, 2U);
	put_text("1");
	bitwriter_se(&writer, 0);
	put_text("1111");
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();
	CHECK(mbs[ADDR].transform_8x8 == 1U);
	CHECK(mbs[ADDR].direct == 1U);

	/* The same with direct_8x8_inference_flag 0: the flag is not read. */
	env_make(H264SW_SLICE_B, 0, 1, 1, 1, 0, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 22U);
	bitwriter_ue(&writer, 0U);
	bitwriter_ue(&writer, 1U);
	bitwriter_ue(&writer, 1U);
	bitwriter_ue(&writer, 1U);
	for (blk = 0; blk < 6U; blk++)
		bitwriter_se(&writer, 0);
	bitwriter_ue(&writer, 2U);
	bitwriter_se(&writer, 0);
	put_text("1111");
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();
	CHECK(mbs[ADDR].transform_8x8 == 0U);
}

/*
 * Skipped macroblocks: only the inferred values are set (7.4.4), the
 * quantizer carries over, and an I slice cannot have one.
 */
static void
check_skipped(void)
{
	unsigned blk;
	int status;

	/* P_Skip: one 16x16 list 0 partition, pattern 0, the previous quantizer, no delta seen by the next context. */
	env_make(H264SW_SLICE_P, 0, 1, 0, 0, 0, 30, ADDR);
	ctx.prev_qp_delta_nonzero = 1;
	status = h264sw_macroblock_parse(&ctx, 1);
	CHECK(status == 0);
	CHECK(ctx.syntax.type.kind == H264SW_MB_P_SKIP);
	CHECK(ctx.syntax.type.parts == 1U);
	CHECK(ctx.syntax.type.pred[0] == H264SW_PRED_L0);
	CHECK(mbs[ADDR].kind == H264SW_MB_P_SKIP);
	CHECK(mbs[ADDR].cbp == 0U);
	CHECK(mbs[ADDR].qp == 30U);
	CHECK(mbs[ADDR].direct == 0U);
	CHECK(mbs[ADDR].motion.intra == 0U);
	CHECK(ctx.syntax.qp_delta == 0);
	CHECK(ctx.qp == 30);
	CHECK(ctx.prev_qp_delta_nonzero == 0);
	want_clear();
	check_levels();

	/* B_Skip: all four quadrants direct. */
	env_make(H264SW_SLICE_B, 0, 1, 1, 0, 1, 40, ADDR);
	status = h264sw_macroblock_parse(&ctx, 1);
	CHECK(status == 0);
	CHECK(ctx.syntax.type.kind == H264SW_MB_B_SKIP);
	CHECK(ctx.syntax.type.pred[0] == H264SW_PRED_DIRECT);
	CHECK(mbs[ADDR].kind == H264SW_MB_B_SKIP);
	CHECK(mbs[ADDR].direct == 0x0fU);
	CHECK(mbs[ADDR].qp == 40U);
	for (blk = 0; blk < 4U; blk++)
		CHECK(ctx.syntax.ref_idx[0][blk] == -1);

	/* An I slice has no skipped macroblock. */
	env_make(H264SW_SLICE_I, 0, 1, 0, 0, 0, 26, ADDR);
	status = h264sw_macroblock_parse(&ctx, 1);
	CHECK(status == EINVAL);
}

/*
 * Runs one Intra_16x16 macroblock (mb_type 1) with mb_qp_delta delta from
 * the quantizer start and checks the resulting QPY (7-37) or the status.
 */
static void
qp_case(
	int start,
	int delta,
	int expected,
	int status)
{
	int result;

	env_make(H264SW_SLICE_I, 0, 1, 0, 0, 0, start, ADDR);
	stream_begin();
	put_16x16_empty(delta);
	stream_end();
	result = h264sw_macroblock_parse(&ctx, 0);
	CHECK(result == status);

	/* A refused delta leaves nothing to compare. */
	if (status != 0)
		return;
	check_consumed();
	CHECK(ctx.syntax.qp_delta == delta);
	CHECK(ctx.qp == expected);
	CHECK(mbs[ADDR].qp == (unsigned)expected);
	CHECK(ctx.prev_qp_delta_nonzero == (delta != 0));
}

/* QPY = ( QPY,PREV + mb_qp_delta + 52 ) % 52 at the ends of its range, and the deltas outside -26..25. */
static void
check_qp(void)
{
	/* The wrap upward and downward. */
	qp_case(51, 1, 0, 0);
	qp_case(0, -1, 51, 0);
	qp_case(26, 25, 51, 0);
	qp_case(26, -26, 0, 0);
	qp_case(51, 25, 24, 0);
	qp_case(0, -26, 26, 0);
	qp_case(10, 0, 10, 0);

	/* A delta outside the range is an error. */
	qp_case(26, 26, 0, EINVAL);
	qp_case(26, -27, 0, EINVAL);

	/* A quantizer outside 0..51 in the slice state is refused before it is used. */
	env_make(H264SW_SLICE_I, 0, 1, 0, 0, 0, 26, ADDR);
	stream_begin();
	put_16x16_empty(0);
	stream_end();
	ctx.qp = 52;
	CHECK(h264sw_macroblock_parse(&ctx, 0) == EINVAL);
	env_make(H264SW_SLICE_I, 0, 1, 0, 0, 0, 26, ADDR);
	stream_begin();
	put_16x16_empty(0);
	stream_end();
	ctx.qp = -1;
	CHECK(h264sw_macroblock_parse(&ctx, 0) == EINVAL);
}

/*
 * nC from the neighbours: the counts of the left and upper macroblocks
 * (9.2.1) select the column of Table 9-5 the coeff_token of the first
 * block is read with, so a wrong nC misreads the stream.
 */
static void
check_nc(void)
{
	int status;

	/* A left I_PCM-like neighbour (16 in every block) and no upper one: nC 16, the 6 bit code of Table 9-5, TotalCoeff 0 is 000011. */
	env_make(H264SW_SLICE_I, 0, 1, 0, 0, 0, 26, ADDR);
	mbs[ADDR - 1U].slice = 0;
	memset(mbs[ADDR - 1U].total_coeff, 16, sizeof(mbs[ADDR - 1U].total_coeff));
	h264sw_mb_neighbours(mbs, PIC_W, ADDR, &ctx.n);
	stream_begin();
	bitwriter_ue(&writer, 1U);
	bitwriter_ue(&writer, 0U);
	bitwriter_se(&writer, 0);
	put_text("000011");
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();

	/* Left count 3 of block 5 and upper count 4 of block 10: nC ( 3 + 4 + 1 ) >> 1 = 4, the third column, TotalCoeff 0 is 1111. */
	env_make(H264SW_SLICE_I, 0, 1, 0, 0, 0, 26, ADDR);
	mbs[ADDR - 1U].slice = 0;
	mbs[ADDR - 1U].total_coeff[5] = 3;
	mbs[ADDR - PIC_W].slice = 0;
	mbs[ADDR - PIC_W].total_coeff[10] = 4;
	h264sw_mb_neighbours(mbs, PIC_W, ADDR, &ctx.n);
	stream_begin();
	bitwriter_ue(&writer, 1U);
	bitwriter_ue(&writer, 0U);
	bitwriter_se(&writer, 0);
	put_text("1111");
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();

	/* Chroma AC blocks (Intra_16x16 mb_type 9: no luma AC, chroma 2): the first Cb block's nC is the left macroblock's Cb count 8 (6 bit code, TotalCoeff 0 is 000011) and the first Cr block's is its Cr count 5 (third column, 1111); the other blocks read the counts of this macroblock's earlier blocks (0, so 1). */
	env_make(H264SW_SLICE_I, 0, 1, 0, 0, 0, 26, ADDR);
	mbs[ADDR - 1U].slice = 0;
	mbs[ADDR - 1U].total_coeff[17] = 8;
	mbs[ADDR - 1U].total_coeff[21] = 5;
	h264sw_mb_neighbours(mbs, PIC_W, ADDR, &ctx.n);
	stream_begin();
	bitwriter_ue(&writer, 9U);
	bitwriter_ue(&writer, 0U);
	bitwriter_se(&writer, 0);
	put_text("1");
	put_text("01");
	put_text("01");
	put_text("000011");
	put_text("111");
	put_text("1111");
	put_text("111");
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();
	CHECK(ctx.syntax.cbp_chroma == 2U);

	/* A left count 2 for block 0 only: nC 2 is the second column (TotalCoeff 0 is 11); block 1 sees block 0's count 0, so nC 0 (1). */
	env_make(H264SW_SLICE_I, 0, 1, 0, 0, 0, 26, ADDR);
	mbs[ADDR - 1U].slice = 0;
	mbs[ADDR - 1U].total_coeff[5] = 2;
	h264sw_mb_neighbours(mbs, PIC_W, ADDR, &ctx.n);
	stream_begin();
	bitwriter_ue(&writer, 0U);
	put_text("1111111111111111");
	bitwriter_ue(&writer, 0U);
	bitwriter_ue(&writer, 29U);
	bitwriter_se(&writer, 0);
	put_text("11");
	put_text("1");
	put_text("1");
	put_text("1");
	stream_end();
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	check_consumed();
}

/* Builds a stream with fn and parses it; returns the status. */
static int
parse_error_case(
	unsigned slice_type,
	unsigned active0,
	unsigned active1,
	int t8x8,
	void (*build)(void))
{
	int status;

	/* A fresh context and the stream the builder writes. */
	env_make(slice_type, 0, active0, active1, t8x8, 0, 26, ADDR);
	stream_begin();
	build();
	stream_end();

	/* Parses it. */
	status = h264sw_macroblock_parse(&ctx, 0);
	return status;
}

/* The builders of the error streams. */
static void
build_i_26(void)
{
	bitwriter_ue(&writer, 26U);
	put_text("1111111111111111");
}

static void
build_p_31(void)
{
	bitwriter_ue(&writer, 31U);
	put_text("1111111111111111");
}

static void
build_b_49(void)
{
	bitwriter_ue(&writer, 49U);
	put_text("1111111111111111");
}

static void
build_i_pcm_short(void)
{
	bitwriter_ue(&writer, 25U);
	while (writer.bits % 8U != 0U)
		bitwriter_u(&writer, 0U, 1U);
	put_text("1111111111111111");
}

static void
build_p_sub_4(void)
{
	bitwriter_ue(&writer, 3U);
	bitwriter_ue(&writer, 0U);
	bitwriter_ue(&writer, 4U);
	put_text("1111111111111111");
}

static void
build_b_sub_13(void)
{
	bitwriter_ue(&writer, 22U);
	bitwriter_ue(&writer, 0U);
	bitwriter_ue(&writer, 13U);
	put_text("1111111111111111");
}

static void
build_ref_3(void)
{
	bitwriter_ue(&writer, 0U);
	bitwriter_ue(&writer, 3U);
	put_text("1111111111111111");
}

static void
build_chroma_4(void)
{
	bitwriter_ue(&writer, 1U);
	bitwriter_ue(&writer, 4U);
	put_text("1111111111111111");
}

static void
build_cbp_48(void)
{
	bitwriter_ue(&writer, 0U);
	bitwriter_se(&writer, 0);
	bitwriter_se(&writer, 0);
	bitwriter_ue(&writer, 48U);
	put_text("1111111111111111");
}

static void
build_mvd_32768(void)
{
	bitwriter_ue(&writer, 0U);
	bitwriter_se(&writer, 32768);
	bitwriter_se(&writer, 0);
	put_text("1111111111111111");
}

static void
build_mvd_neg_32769(void)
{
	bitwriter_ue(&writer, 0U);
	bitwriter_se(&writer, 0);
	bitwriter_se(&writer, -32769);
	put_text("1111111111111111");
}

static void
build_mvd_huge(void)
{
	bitwriter_ue(&writer, 0U);
	bitwriter_se(&writer, 0x7fffffff);
	put_text("1111111111111111");
}

static void
build_ue_overlong(void)
{
	bitwriter_ue(&writer, 0U);
	bitwriter_u(&writer, 0U, 32U);
	bitwriter_u(&writer, 0U, 8U);
	put_text("1111111111111111");
}

static void
build_mvd_max(void)
{
	bitwriter_ue(&writer, 0U);
	bitwriter_se(&writer, 32767);
	bitwriter_se(&writer, -32768);
	bitwriter_ue(&writer, 0U);
}

/* The errors the header names, and the limits of the ranges just inside them. */
static void
check_errors(void)
{
	int status;

	/* mb_type outside the table of its slice type. */
	CHECK(parse_error_case(H264SW_SLICE_I, 1, 0, 0, build_i_26) == EINVAL);
	CHECK(parse_error_case(H264SW_SLICE_P, 1, 0, 0, build_p_31) == EINVAL);
	CHECK(parse_error_case(H264SW_SLICE_B, 1, 1, 0, build_b_49) == EINVAL);

	/* An I_PCM whose samples are not all there. */
	CHECK(parse_error_case(H264SW_SLICE_I, 1, 0, 0, build_i_pcm_short) == EINVAL);

	/* sub_mb_type outside Table 7-17 and Table 7-18. */
	CHECK(parse_error_case(H264SW_SLICE_P, 1, 0, 0, build_p_sub_4) == EINVAL);
	CHECK(parse_error_case(H264SW_SLICE_B, 1, 1, 0, build_b_sub_13) == EINVAL);

	/* ref_idx above num_ref_idx_active - 1 (three references: 3 is too large). */
	CHECK(parse_error_case(H264SW_SLICE_P, 3, 0, 0, build_ref_3) == EINVAL);

	/* intra_chroma_pred_mode above 3, coded_block_pattern codeNum above 47. */
	CHECK(parse_error_case(H264SW_SLICE_I, 1, 0, 0, build_chroma_4) == EINVAL);
	CHECK(parse_error_case(H264SW_SLICE_P, 1, 0, 0, build_cbp_48) == EINVAL);

	/* A motion vector difference outside -32768..32767 (which se(v) can say), an se(v) of the largest magnitude, and an Exp-Golomb code of 32 leading zeros. */
	CHECK(parse_error_case(H264SW_SLICE_P, 1, 0, 0, build_mvd_32768) == EINVAL);
	CHECK(parse_error_case(H264SW_SLICE_P, 1, 0, 0, build_mvd_neg_32769) == EINVAL);
	CHECK(parse_error_case(H264SW_SLICE_P, 1, 0, 0, build_mvd_huge) == EINVAL);
	CHECK(parse_error_case(H264SW_SLICE_P, 1, 0, 0, build_ue_overlong) == EINVAL);

	/* The ends of the allowed range are accepted: their absolute values are capped at 255 in the entry. */
	status = parse_error_case(H264SW_SLICE_P, 1, 0, 0, build_mvd_max);
	CHECK(status == 0);
	CHECK(ctx.syntax.mvd[0][0][0][0] == 32767);
	CHECK(ctx.syntax.mvd[0][0][0][1] == -32768);
	CHECK(mbs[ADDR].mvd[0][0][0] == 255U);
	CHECK(mbs[ADDR].mvd[0][0][1] == 255U);

	/* A slice type with no macroblock types here, and a context without a current entry. */
	env_make(H264SW_SLICE_SP, 0, 1, 0, 0, 0, 26, ADDR);
	stream_begin();
	put_text("1111111111111111");
	stream_end();
	CHECK(h264sw_macroblock_parse(&ctx, 0) == EINVAL);
	env_make(H264SW_SLICE_I, 0, 1, 0, 0, 0, 26, ADDR);
	ctx.n.current = NULL;
	CHECK(h264sw_macroblock_parse(&ctx, 0) == EINVAL);
}

/* Every prefix of a complete stream: no crash, and a stream cut inside its content is an error. */
static void
check_truncated(void)
{
	size_t bytes;
	size_t length;
	unsigned index;
	int status;

	/* The I_4x4 stream of check_i4x4, written once. */
	env_make(H264SW_SLICE_I, 0, 1, 0, 0, 0, 26, ADDR);
	stream_begin();
	bitwriter_ue(&writer, 0U);
	for (index = 0; index < 16U; index++) {
		if (index % 2U == 0U) {
			bitwriter_u(&writer, 1U, 1U);
		} else {
			bitwriter_u(&writer, 0U, 1U);
			bitwriter_u(&writer, index >> 1, 3U);
		}
	}
	bitwriter_ue(&writer, 2U);
	bitwriter_ue(&writer, 29U);
	bitwriter_se(&writer, -3);
	put_block_one(0U, 0U);
	put_text("1");
	put_block_two(1U);
	put_block_one(0U, 2U);
	content_bits = writer.bits;
	bitwriter_trailing(&writer);
	length = bitwriter_bytes(&writer);

	/* Parses every prefix; one that ends before the content does is an error. */
	for (bytes = 0; bytes <= length; bytes++) {
		env_make(H264SW_SLICE_I, 0, 1, 0, 0, 0, 26, ADDR);
		h264sw_bits_init(&ctx.bits, writer.data, bytes);
		status = h264sw_macroblock_parse(&ctx, 0);
		if (bytes * 8U < content_bits)
			CHECK(status == EINVAL);
		else
			CHECK(status == 0);
	}
}

/* Checks what must hold of the entry and the slice state after any parse that succeeded. */
static void
check_sane_after_parse(
	unsigned slice_type)
{
	unsigned list;
	unsigned quadrant;

	/* The quantizer is within 0..51 and so is the entry's. */
	CHECK(ctx.qp >= 0 && ctx.qp <= 51);
	CHECK(mbs[ctx.mb_addr].qp <= 51U);

	/* The pattern is at most 47 (or the I_PCM value). */
	CHECK(mbs[ctx.mb_addr].cbp <= 0x2fU);

	/* Every reference index is unused or inside the list the header names. */
	for (list = 0; list < 2U; list++) {
		for (quadrant = 0; quadrant < 4U; quadrant++) {
			CHECK(mbs[ctx.mb_addr].motion.ref_idx[list][quadrant] >= -1);
			CHECK(mbs[ctx.mb_addr].motion.ref_idx[list][quadrant] < (int)header.num_ref_idx_active[list] || header.num_ref_idx_active[list] == 0U);
		}
	}

	/* An intra kind is marked intra and an inter kind is not. */
	if (mbs[ctx.mb_addr].kind < H264SW_MB_P_SKIP) {
		CHECK(mbs[ctx.mb_addr].motion.intra == 1U);
	} else {
		CHECK(mbs[ctx.mb_addr].motion.intra == 0U);
	}
	CHECK(slice_type == header.slice_type);
}

/* Random bytes as CAVLC slice data of every slice type: no crash, only the errors of the header, and sane results. */
static void
check_fuzz_cavlc(void)
{
	static uint8_t data[256];
	unsigned round;
	unsigned slice_type;
	unsigned index;
	unsigned length;
	unsigned successes;
	int status;

	/* Many random streams; each parses one macroblock, and the next one when the first succeeded. */
	successes = 0;
	for (round = 0; round < 20000U; round++) {
		slice_type = round % 3U;
		if (slice_type == 0U) {
			slice_type = H264SW_SLICE_I;
		} else if (slice_type == 1U) {
			slice_type = H264SW_SLICE_P;
		} else {
			slice_type = H264SW_SLICE_B;
		}
		length = 1U + random_next() % 200U;
		for (index = 0; index < length; index++)
			data[index] = (uint8_t)random_next();

		/* Every other stream starts with bits that make a macroblock likely: an mb_type of a small value. */
		if (round % 2U == 0U)
			data[0] = (uint8_t)(0x80U | (random_next() & 0x7fU));
		env_make(slice_type, 0, 1U + random_next() % 4U, 1U + random_next() % 4U, (int)(random_next() & 1U), (int)(random_next() & 1U), (int)(random_next() % 52U), ADDR);
		h264sw_bits_init(&ctx.bits, data, length);
		status = h264sw_macroblock_parse(&ctx, 0);
		CHECK(status == 0 || status == EINVAL);
		if (status == 0) {
			successes++;
			check_sane_after_parse(slice_type);
		}
	}

	/* Some streams must have succeeded, or the fuzz proves little. */
	CHECK(successes > 100U);
}

/* Prepares a slice for the slice data loop: nothing decoded, slice number 5, the first macroblock given. */
static void
slice_env(
	unsigned slice_type,
	int cabac,
	unsigned first_mb)
{
	unsigned index;

	/* The picture with every macroblock undecoded. */
	env_make(slice_type, cabac, 1, 1, 0, 0, 26, 0);
	for (index = 0; index < PIC_MBS; index++)
		h264sw_mb_reset(&mbs[index], -1);
	header.first_mb = first_mb;
	ctx.slice_num = 5;
	picture.decoded_mbs = 0;

	/* The stub of the reconstruction starts with nothing seen and no failure. */
	recon_count = 0;
	recon_fail_at = -1;
	recon_fail_status = 0;
	memset(recon_pcm, 0, sizeof(recon_pcm));
}

/* The slice data loop of CAVLC slices: skip runs, the ends of the data, and the state the loop keeps. */
static void
check_slice_cavlc(void)
{
	int status;

	/* An I slice of three Intra_16x16 macroblocks from address 2, with mb_qp_delta 2, -3 and 0. */
	slice_env(H264SW_SLICE_I, 0, 2U);
	stream_begin();
	put_16x16_empty(2);
	put_16x16_empty(-3);
	put_16x16_empty(0);
	stream_end();
	status = copy_slice_data(&ctx);
	CHECK(status == 0);
	CHECK(recon_count == 3U);
	CHECK(recon_addr[0] == 2U && recon_addr[1] == 3U && recon_addr[2] == 4U);
	CHECK(recon_kind[0] == H264SW_MB_I_16X16);
	CHECK(picture.decoded_mbs == 3U);
	CHECK(mbs[2].slice == 5 && mbs[3].slice == 5 && mbs[4].slice == 5);
	CHECK(mbs[1].slice == -1 && mbs[5].slice == -1);
	CHECK(mbs[2].qp == 28U && mbs[3].qp == 25U && mbs[4].qp == 25U);
	CHECK(ctx.qp == 25);
	CHECK(ctx.prev_mb_addr == 4);
	CHECK(ctx.bits.position == content_bits);

	/* A P slice: a run of 2 skipped macroblocks, a coded P_L0_16x16, a run of 1 skipped one, and then the data ends. */
	slice_env(H264SW_SLICE_P, 0, 0U);
	stream_begin();
	bitwriter_ue(&writer, 2U);
	bitwriter_ue(&writer, 0U);
	bitwriter_se(&writer, 0);
	bitwriter_se(&writer, 0);
	bitwriter_ue(&writer, 0U);
	bitwriter_ue(&writer, 1U);
	stream_end();
	status = copy_slice_data(&ctx);
	CHECK(status == 0);
	CHECK(recon_count == 4U);
	CHECK(recon_kind[0] == H264SW_MB_P_SKIP);
	CHECK(recon_kind[1] == H264SW_MB_P_SKIP);
	CHECK(recon_kind[2] == H264SW_MB_P);
	CHECK(recon_kind[3] == H264SW_MB_P_SKIP);
	CHECK(picture.decoded_mbs == 4U);
	CHECK(mbs[3].slice == 5 && mbs[4].slice == -1);
	CHECK(mbs[0].qp == 26U);

	/* Runs of 0 before each coded macroblock, and the data ending right after a coded one. */
	slice_env(H264SW_SLICE_P, 0, 0U);
	stream_begin();
	bitwriter_ue(&writer, 0U);
	bitwriter_ue(&writer, 0U);
	bitwriter_se(&writer, 1);
	bitwriter_se(&writer, 1);
	bitwriter_ue(&writer, 0U);
	bitwriter_ue(&writer, 0U);
	bitwriter_ue(&writer, 0U);
	bitwriter_se(&writer, 1);
	bitwriter_se(&writer, 1);
	bitwriter_ue(&writer, 0U);
	stream_end();
	status = copy_slice_data(&ctx);
	CHECK(status == 0);
	CHECK(recon_count == 2U);
	CHECK(recon_kind[0] == H264SW_MB_P && recon_kind[1] == H264SW_MB_P);

	/* A skip run that ends at the last macroblock of the picture. */
	slice_env(H264SW_SLICE_P, 0, 7U);
	stream_begin();
	bitwriter_ue(&writer, 2U);
	stream_end();
	status = copy_slice_data(&ctx);
	CHECK(status == 0);
	CHECK(recon_count == 2U);
	CHECK(mbs[7].slice == 5 && mbs[8].slice == 5);

	/* A B slice's skipped macroblocks are B_Skip. */
	slice_env(H264SW_SLICE_B, 0, 0U);
	stream_begin();
	bitwriter_ue(&writer, 3U);
	stream_end();
	status = copy_slice_data(&ctx);
	CHECK(status == 0);
	CHECK(recon_kind[0] == H264SW_MB_B_SKIP);
	CHECK(mbs[1].direct == 0x0fU);
}

/* The errors of the slice data loop: an overlapping slice, a run or a slice past the picture, a failing reconstruction or parse. */
static void
check_slice_errors(void)
{
	int status;

	/* A run larger than the macroblocks left: the macroblock whose run was refused is not decoded. */
	slice_env(H264SW_SLICE_P, 0, 8U);
	stream_begin();
	bitwriter_ue(&writer, 2U);
	stream_end();
	status = copy_slice_data(&ctx);
	CHECK(status == EINVAL);
	CHECK(mbs[8].slice == -1);
	CHECK(picture.decoded_mbs == 0U);
	CHECK(recon_count == 0U);

	/* A macroblock that already belongs to a slice: refused, and its entry is not touched. */
	slice_env(H264SW_SLICE_I, 0, 4U);
	mbs[4].slice = 3;
	mbs[4].kind = H264SW_MB_P;
	mbs[4].qp = 17;
	stream_begin();
	put_16x16_empty(0);
	stream_end();
	status = copy_slice_data(&ctx);
	CHECK(status == EINVAL);
	CHECK(mbs[4].slice == 3);
	CHECK(mbs[4].kind == H264SW_MB_P);
	CHECK(mbs[4].qp == 17U);
	CHECK(recon_count == 0U);

	/* A slice with more macroblocks than the picture has left. */
	slice_env(H264SW_SLICE_I, 0, 7U);
	stream_begin();
	put_16x16_empty(0);
	put_16x16_empty(0);
	put_16x16_empty(0);
	stream_end();
	status = copy_slice_data(&ctx);
	CHECK(status == EINVAL);
	CHECK(picture.decoded_mbs == 2U);
	CHECK(mbs[7].slice == 5 && mbs[8].slice == 5);

	/* A first macroblock outside the picture. */
	slice_env(H264SW_SLICE_I, 0, 9U);
	stream_begin();
	put_16x16_empty(0);
	stream_end();
	CHECK(copy_slice_data(&ctx) == EINVAL);

	/* A reconstruction that fails at the second macroblock: the first stays decoded, the second's entry is reset with slice -1. */
	slice_env(H264SW_SLICE_I, 0, 0U);
	recon_fail_at = 1;
	recon_fail_status = ENOTSUP;
	stream_begin();
	put_16x16_empty(1);
	put_16x16_empty(1);
	put_16x16_empty(1);
	stream_end();
	status = copy_slice_data(&ctx);
	CHECK(status == ENOTSUP);
	CHECK(picture.decoded_mbs == 1U);
	CHECK(mbs[0].slice == 5);
	CHECK(mbs[1].slice == -1);
	CHECK(mbs[1].kind == H264SW_MB_I_NXN);
	CHECK(mbs[1].qp == 0U);
	CHECK(mbs[2].slice == -1);

	/* A macroblock that fails to parse (mb_type 26 in an I slice): the macroblocks before it stay. */
	slice_env(H264SW_SLICE_I, 0, 0U);
	stream_begin();
	put_16x16_empty(0);
	bitwriter_ue(&writer, 26U);
	stream_end();
	status = copy_slice_data(&ctx);
	CHECK(status == EINVAL);
	CHECK(picture.decoded_mbs == 1U);
	CHECK(mbs[1].slice == -1);

	/* Data that ends inside a macroblock. */
	slice_env(H264SW_SLICE_I, 0, 0U);
	stream_begin();
	bitwriter_ue(&writer, 1U);
	stream_end();
	status = copy_slice_data(&ctx);
	CHECK(status == EINVAL);
	CHECK(mbs[0].slice == -1);

	/* A slice type of the tools this decoder does not have. */
	slice_env(H264SW_SLICE_SP, 0, 0U);
	stream_begin();
	stream_end();
	CHECK(copy_slice_data(&ctx) == ENOTSUP);
	slice_env(H264SW_SLICE_SI, 0, 0U);
	stream_begin();
	stream_end();
	CHECK(copy_slice_data(&ctx) == ENOTSUP);
}

/* Starts the encoder with random context variables and an empty output (the decoder is given the same ones later). */
static void
cabac_begin(void)
{
	unsigned ctx_idx;
	uint32_t number;

	/* Every context variable gets a random state: pStateIdx 0 to 62 and valMPS 0 or 1. */
	memset(&encoder, 0, sizeof(encoder));
	for (ctx_idx = 0; ctx_idx < CABACWRITER_CONTEXTS; ctx_idx++) {
		number = random_next();
		encoder.state[ctx_idx] = (uint8_t)((((number >> 8) & 1U) << 6) | (number % 63U));
	}
	memcpy(initial_state, encoder.state, sizeof(initial_state));

	/* The encoder writes into an empty stream. */
	bitwriter_init(&writer);
	cabacwriter_engine_init(&encoder, &writer);
}

/*
 * Pads the stream to a byte boundary and starts the decoder's engine on
 * it with the encoder's initial context variables; returns the status of
 * the engine's start.
 */
static int
cabac_end(
	unsigned slice_type)
{
	struct h264sw_bits bits;
	int status;

	/* The encoder's last bit is the stop bit when the last bin was end_of_slice_flag 1; the rest of the byte is zero. */
	while (writer.bits % 8U != 0U)
		bitwriter_u(&writer, 0U, 1U);
	CHECK(writer.overflow == 0);

	/* Starts the decoder on the bytes and gives it the encoder's context variables. */
	h264sw_bits_init(&bits, writer.data, bitwriter_bytes(&writer));
	status = h264sw_cabac_start(&ctx.cabac, &bits, slice_type, 0U, 26);
	CHECK(status == 0);
	memcpy(ctx.cabac.state, initial_state, sizeof(initial_state));
	return status;
}

/*
 * A CABAC I slice of two I_PCM macroblocks: mb_type bin 0 with ctxIdx 3 +
 * condTermFlagA + condTermFlagB (9.3.3.1.1.3: the first has no neighbour,
 * the second's left neighbour is not I_NxN so its flag is 1 and ctxIdx 4),
 * the terminate bin 1, the samples, and end_of_slice_flag 0 then 1.  The
 * second macroblock's context shows that the first's kind was recorded.
 */
static void
check_cabac_pcm(void)
{
	uint8_t samples[PCM_BYTES];
	unsigned index;
	int status;

	/* Encodes the two macroblocks. */
	slice_env(H264SW_SLICE_I, 1, 0U);
	cabac_begin();
	for (index = 0; index < PCM_BYTES; index++)
		samples[index] = (uint8_t)(index * 5U + 1U);
	cabacwriter_decision(&encoder, 3U, 1U);
	cabacwriter_terminate(&encoder, 1U);
	cabacwriter_pcm(&encoder, samples, PCM_BYTES);
	cabacwriter_terminate(&encoder, 0U);
	for (index = 0; index < PCM_BYTES; index++)
		samples[index] = (uint8_t)(index * 3U + 200U);
	cabacwriter_decision(&encoder, 4U, 1U);
	cabacwriter_terminate(&encoder, 1U);
	cabacwriter_pcm(&encoder, samples, PCM_BYTES);
	cabacwriter_terminate(&encoder, 1U);
	status = cabac_end(H264SW_SLICE_I);
	if (status != 0)
		return;

	/* Decodes the slice through the loop. */
	status = copy_slice_data(&ctx);
	CHECK(status == 0);
	CHECK(picture.decoded_mbs == 2U);
	CHECK(recon_count == 2U);
	CHECK(recon_kind[0] == H264SW_MB_I_PCM && recon_kind[1] == H264SW_MB_I_PCM);
	CHECK(mbs[0].slice == 5 && mbs[1].slice == 5 && mbs[2].slice == -1);
	CHECK(mbs[0].coded_flags == 0x07ffffffU);
	CHECK(mbs[0].cbp == 0x2fU && mbs[0].qp == 0U);
	CHECK(ctx.qp == 26);
	for (index = 0; index < PCM_BYTES; index++) {
		CHECK(recon_pcm[0][index] == (uint8_t)(index * 5U + 1U));
		CHECK(recon_pcm[1][index] == (uint8_t)(index * 3U + 200U));
	}

	/* The two sides ended with the same context variables and no read went past the data. */
	CHECK(memcmp(ctx.cabac.state, encoder.state, CABACWRITER_CONTEXTS) == 0);
	CHECK(ctx.cabac.error == 0);
}

/*
 * A CABAC P slice: two skipped macroblocks (mb_skip_flag 1 with ctxIdx 11
 * + condTermFlagA + condTermFlagB, where a skipped or missing neighbour
 * counts 0) and then an I_PCM macroblock in the P slice: mb_skip_flag 0,
 * the prefix bin 1 of Table 9-37 with ctxIdx 14, the suffix bin 0 with
 * ctxIdx 17 and the terminate bin 1; the end_of_slice_flag follows each.
 */
static void
check_cabac_skip(void)
{
	uint8_t samples[PCM_BYTES];
	unsigned index;
	int status;

	/* Encodes the three macroblocks. */
	slice_env(H264SW_SLICE_P, 1, 0U);
	cabac_begin();
	for (index = 0; index < PCM_BYTES; index++)
		samples[index] = (uint8_t)(255U - index);
	cabacwriter_decision(&encoder, 11U, 1U);
	cabacwriter_terminate(&encoder, 0U);
	cabacwriter_decision(&encoder, 11U, 1U);
	cabacwriter_terminate(&encoder, 0U);
	cabacwriter_decision(&encoder, 11U, 0U);
	cabacwriter_decision(&encoder, 14U, 1U);
	cabacwriter_decision(&encoder, 17U, 1U);
	cabacwriter_terminate(&encoder, 1U);
	cabacwriter_pcm(&encoder, samples, PCM_BYTES);
	cabacwriter_terminate(&encoder, 1U);
	status = cabac_end(H264SW_SLICE_P);
	if (status != 0)
		return;

	/* Decodes the slice through the loop. */
	status = copy_slice_data(&ctx);
	CHECK(status == 0);
	CHECK(picture.decoded_mbs == 3U);
	CHECK(recon_kind[0] == H264SW_MB_P_SKIP);
	CHECK(recon_kind[1] == H264SW_MB_P_SKIP);
	CHECK(recon_kind[2] == H264SW_MB_I_PCM);
	CHECK(mbs[2].slice == 5);
	for (index = 0; index < PCM_BYTES; index++)
		CHECK(recon_pcm[2][index] == (uint8_t)(255U - index));
	CHECK(memcmp(ctx.cabac.state, encoder.state, CABACWRITER_CONTEXTS) == 0);
	CHECK(ctx.cabac.error == 0);
}

/*
 * Two CABAC P macroblocks in a row, parsed one after the other: a
 * P_L0_L0_16x8 with two references (the second partition's ref_idx and mvd
 * contexts read the first's, recorded just before) and then a P_L0_16x16
 * whose contexts read that macroblock.  The bins and contexts are worked
 * out by hand from Tables 9-34, 9-37 and 9-39 and 9.3.3.1.1.4, 9.3.3.1.1.6
 * and 9.3.3.1.1.7; the context variables of the two sides must end equal.
 */
static void
check_cabac_inter(void)
{
	unsigned blk;
	int status;

	/* The first macroblock is at address 3 (no neighbour in this slice), the second at 4 (left neighbour 3). */
	env_make(H264SW_SLICE_P, 1, 2, 0, 0, 0, 26, 3U);
	cabac_begin();

	/* mb_type P_L0_L0_16x8: bins 0 1 1 with ctxIdx 14, 15 and 17. */
	cabacwriter_decision(&encoder, 14U, 0U);
	cabacwriter_decision(&encoder, 15U, 1U);
	cabacwriter_decision(&encoder, 17U, 1U);

	/* ref_idx_l0 of partition 0 is 1 (bins 1 0; ctxIdx 54, 58); of partition 1 is 0 with the first partition above it having ref_idx above 0, so ctxIdx 54 + 2. */
	cabacwriter_decision(&encoder, 54U, 1U);
	cabacwriter_decision(&encoder, 58U, 0U);
	cabacwriter_decision(&encoder, 56U, 0U);

	/* mvd_l0 of partition 0: x 3 (prefix 1110: ctxIdx 40, 43, 44, 45; sign +), y 0 (ctxIdx 47). */
	cabacwriter_decision(&encoder, 40U, 1U);
	cabacwriter_decision(&encoder, 43U, 1U);
	cabacwriter_decision(&encoder, 44U, 1U);
	cabacwriter_decision(&encoder, 45U, 0U);
	cabacwriter_bypass(&encoder, 0U);
	cabacwriter_decision(&encoder, 47U, 0U);

	/* mvd_l0 of partition 1: x -1 with absMvdCompB 3 above it, so ctxIdxInc 1 (ctxIdx 41, then 43; sign -), y 0 (ctxIdx 47). */
	cabacwriter_decision(&encoder, 41U, 1U);
	cabacwriter_decision(&encoder, 43U, 0U);
	cabacwriter_bypass(&encoder, 1U);
	cabacwriter_decision(&encoder, 47U, 0U);

	/* coded_block_pattern 0: four luma bins whose ctxIdxInc counts the 8x8 blocks of this macroblock already known to be 0 (73, then 73 + 1, 73 + 2, 73 + 3), and chroma bin 0 with ctxIdx 77. */
	cabacwriter_decision(&encoder, 73U, 0U);
	cabacwriter_decision(&encoder, 74U, 0U);
	cabacwriter_decision(&encoder, 75U, 0U);
	cabacwriter_decision(&encoder, 76U, 0U);
	cabacwriter_decision(&encoder, 77U, 0U);

	/* The second macroblock, P_L0_16x16: bins 0 0 0 with ctxIdx 14, 15, 16. */
	cabacwriter_decision(&encoder, 14U, 0U);
	cabacwriter_decision(&encoder, 15U, 0U);
	cabacwriter_decision(&encoder, 16U, 0U);

	/* ref_idx_l0 1 with the left neighbour's ref_idx above 0, ctxIdx 55 then 58. */
	cabacwriter_decision(&encoder, 55U, 1U);
	cabacwriter_decision(&encoder, 58U, 0U);

	/* mvd_l0 x -2 with absMvdCompA 3 (ctxIdx 41, 43, 44; sign -), y 0 with absMvdCompA 0 (ctxIdx 47). */
	cabacwriter_decision(&encoder, 41U, 1U);
	cabacwriter_decision(&encoder, 43U, 1U);
	cabacwriter_decision(&encoder, 44U, 0U);
	cabacwriter_bypass(&encoder, 1U);
	cabacwriter_decision(&encoder, 47U, 0U);

	/* coded_block_pattern 0 with the left macroblock's pattern 0 counting as condTermFlag 1 where the bin is on its side: ctxIdx 74, 74, 76, 76, then chroma bin 0 with ctxIdx 77. */
	cabacwriter_decision(&encoder, 74U, 0U);
	cabacwriter_decision(&encoder, 74U, 0U);
	cabacwriter_decision(&encoder, 76U, 0U);
	cabacwriter_decision(&encoder, 76U, 0U);
	cabacwriter_decision(&encoder, 77U, 0U);

	/* Ends the slice data with end_of_slice_flag 1 and starts the decoder. */
	cabacwriter_terminate(&encoder, 1U);
	status = cabac_end(H264SW_SLICE_P);
	if (status != 0)
		return;

	/* Parses the first macroblock. */
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	CHECK(ctx.syntax.type.kind == H264SW_MB_P);
	CHECK(ctx.syntax.type.parts == 2U);
	CHECK(ctx.syntax.ref_idx[0][0] == 1);
	CHECK(ctx.syntax.ref_idx[0][1] == 0);
	CHECK(ctx.syntax.mvd[0][0][0][0] == 3);
	CHECK(ctx.syntax.mvd[0][0][0][1] == 0);
	CHECK(ctx.syntax.mvd[0][1][0][0] == -1);
	CHECK(ctx.syntax.mvd[0][1][0][1] == 0);
	CHECK(mbs[3].motion.ref_idx[0][0] == 1);
	CHECK(mbs[3].motion.ref_idx[0][1] == 1);
	CHECK(mbs[3].motion.ref_idx[0][2] == 0);
	CHECK(mbs[3].motion.ref_idx[0][3] == 0);
	for (blk = 0; blk < 16U; blk++) {
		if (h264sw_blk4x4_y[blk] < 8U) {
			CHECK(mbs[3].mvd[0][blk][0] == 3U);
		} else {
			CHECK(mbs[3].mvd[0][blk][0] == 1U);
		}
	}
	CHECK(mbs[3].cbp == 0U);
	CHECK(ctx.cabac.error == 0);

	/* Parses the second macroblock, whose left neighbour is the first. */
	mbs[4].slice = 0;
	ctx.mb_addr = 4U;
	h264sw_mb_neighbours(mbs, PIC_W, 4U, &ctx.n);
	ctx.prev_mb_addr = 3;
	status = h264sw_macroblock_parse(&ctx, 0);
	CHECK(status == 0);
	CHECK(ctx.syntax.type.kind == H264SW_MB_P);
	CHECK(ctx.syntax.type.parts == 1U);
	CHECK(ctx.syntax.ref_idx[0][0] == 1);
	CHECK(ctx.syntax.mvd[0][0][0][0] == -2);
	CHECK(ctx.syntax.mvd[0][0][0][1] == 0);
	CHECK(mbs[4].cbp == 0U);
	CHECK(mbs[4].qp == 26U);
	CHECK(ctx.cabac.error == 0);

	/* Both sides ended with the same context variables, so every element was read with the context it was written with, and the slice ends where the encoder ended it. */
	CHECK(memcmp(ctx.cabac.state, encoder.state, CABACWRITER_CONTEXTS) == 0);
	CHECK(h264sw_cabac_end_of_slice(&ctx) == 1U);
}

/* Random bytes as CABAC slice data: no crash, only the errors of the header, and sane results; also through the slice data loop. */
static void
check_fuzz_cabac(void)
{
	static uint8_t data[300];
	struct h264sw_bits bits;
	unsigned round;
	unsigned slice_type;
	unsigned index;
	unsigned length;
	unsigned successes;
	unsigned count;
	int status;

	/* Many random streams of every slice type. */
	successes = 0;
	for (round = 0; round < 20000U; round++) {
		slice_type = round % 3U;
		if (slice_type == 0U) {
			slice_type = H264SW_SLICE_I;
		} else if (slice_type == 1U) {
			slice_type = H264SW_SLICE_P;
		} else {
			slice_type = H264SW_SLICE_B;
		}
		length = 2U + random_next() % 250U;
		for (index = 0; index < length; index++)
			data[index] = (uint8_t)random_next();
		env_make(slice_type, 1, 1U + random_next() % 4U, 1U + random_next() % 4U, (int)(random_next() & 1U), (int)(random_next() & 1U), (int)(random_next() % 52U), ADDR);
		h264sw_bits_init(&bits, data, length);
		status = h264sw_cabac_start(&ctx.cabac, &bits, slice_type, random_next() % 3U, ctx.qp);
		if (status != 0)
			continue;

		/* Two macroblocks in a row, the second next to the first. */
		for (count = 0; count < 2U; count++) {
			status = h264sw_macroblock_parse(&ctx, (int)(random_next() & 1U) && slice_type != H264SW_SLICE_I);
			CHECK(status == 0 || status == EINVAL);
			if (status != 0)
				break;
			successes++;
			check_sane_after_parse(slice_type);
			mbs[ADDR + 1U].slice = 0;
			ctx.mb_addr = ADDR + 1U;
			h264sw_mb_neighbours(mbs, PIC_W, ADDR + 1U, &ctx.n);
		}
	}
	CHECK(successes > 100U);

	/* The slice data loop on random bytes of both entropy coders (with a reconstruction that always succeeds): it ends, and every entry is either decoded in the slice or reset. */
	for (round = 0; round < 6000U; round++) {
		slice_type = H264SW_SLICE_I;
		if (round % 3U == 1U)
			slice_type = H264SW_SLICE_P;
		if (round % 3U == 2U)
			slice_type = H264SW_SLICE_B;
		length = 2U + random_next() % 250U;
		for (index = 0; index < length; index++)
			data[index] = (uint8_t)random_next();
		slice_env(slice_type, (int)(round & 1U), random_next() % 9U);
		header.num_ref_idx_active[0] = 1U + random_next() % 3U;
		header.num_ref_idx_active[1] = 1U + random_next() % 3U;
		h264sw_bits_init(&bits, data, length);
		if ((round & 1U) != 0U) {
			status = h264sw_cabac_start(&ctx.cabac, &bits, slice_type, 0U, 26);
			if (status != 0)
				continue;
		} else {
			ctx.bits = bits;
		}
		status = copy_slice_data(&ctx);
		CHECK(status == 0 || status == EINVAL || status == ENOTSUP);
		for (index = 0; index < PIC_MBS; index++)
			CHECK(mbs[index].slice == -1 || mbs[index].slice == 5);
		CHECK(picture.decoded_mbs <= PIC_MBS);
		CHECK(recon_count == picture.decoded_mbs);
	}
}

/* The stub of the reconstruction used by the included copy: it records what the loop gives it and fails where asked. */
int
stub_recon_mb(
	struct h264sw_slice_ctx *slice)
{
	/* Records the macroblock the loop reconstructs. */
	if (recon_count < PIC_MBS) {
		recon_addr[recon_count] = slice->mb_addr;
		recon_kind[recon_count] = slice->n.current->kind;
		memcpy(recon_pcm[recon_count], slice->syntax.pcm, PCM_BYTES);
	}
	recon_count++;

	/* Fails at the address the case asked for. */
	if ((int)slice->mb_addr == recon_fail_at)
		return recon_fail_status;

	/* Succeeded: the macroblock is reconstructed. */
	return 0;
}
