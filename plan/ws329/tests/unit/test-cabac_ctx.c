/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the CABAC context selection the software H.264 decoder shares
 * with the encoder: h264sw_cabac_contexts_init (h264sw-cabac.h) and the
 * h264sw_ctx_* functions (h264sw-cabac-se.h).  Each case sets up the
 * current macroblock and its neighbours A and B by hand in a 3 by 3
 * picture and compares the ctxIdx returned with the one worked out here
 * from Tables 9-34, 9-39, 9-40, 9-43 and 9.3.3.1.1 (the expected values
 * are written as offset + increment so each term can be read against the
 * clause).
 */

#include "check.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "userland/desktop/libmedia/vcodec-cabac.h"
#include "userland/desktop/libmedia/h264sw-cabac.h"
#include "userland/desktop/libmedia/h264sw-cabac-init.h"
#include "userland/desktop/libmedia/h264sw-cabac-se.h"
#include "userland/desktop/libmedia/h264sw-context.h"

/* The macroblock positions in the 3 by 3 picture, with the current macroblock in the middle. */
#define ADDR_B 1U
#define ADDR_A 3U
#define ADDR_CUR 4U

/* A neighbour that is not available. */
#define NONE 0xffU

/* The state of the cases: the slice context over a 3 by 3 picture. */
static struct h264sw_slice_ctx ctx;
static struct h264sw_slice_header header;
static struct h264sw_picture_ctx picture;
static struct h264sw_mb mbs[9];

/* Forward declarations of the static functions. */
static void env_make(unsigned slice_type, unsigned cur_kind, unsigned a_kind, unsigned b_kind);
static void check_contexts_init(void);
static void check_mb_skip(void);
static void check_mb_type_inc(void);
static void check_transform_8x8(void);
static void check_chroma_mode(void);
static void check_ref_idx(void);
static void check_mvd(void);
static void check_cbp(void);
static void check_qp_delta(void);
static void check_coded_block_flag(void);
static void check_significance(void);
static void check_abs_level(void);

int
main(void)
{
	check_contexts_init();
	check_mb_skip();
	check_mb_type_inc();
	check_transform_8x8();
	check_chroma_mode();
	check_ref_idx();
	check_mvd();
	check_cbp();
	check_qp_delta();
	check_coded_block_flag();
	check_significance();
	check_abs_level();
	return check_finish("cabac_ctx");
}

/*
 * Sets up a slice of the given type over a 3 by 3 picture: the current
 * macroblock in the middle with kind cur_kind, A to its left and B above
 * it with the given kinds (NONE for one that is not available), every
 * entry reset as the decoder resets it.
 */
static void
env_make(
	unsigned slice_type,
	unsigned cur_kind,
	unsigned a_kind,
	unsigned b_kind)
{
	unsigned index;

	/* Starts from a clean context and picture. */
	memset(&ctx, 0, sizeof(ctx));
	memset(&header, 0, sizeof(header));
	memset(&picture, 0, sizeof(picture));
	header.slice_type = slice_type;
	header.num_ref_idx_active[0] = 4U;
	header.num_ref_idx_active[1] = 4U;

	/* Resets every entry as an undecoded one, then places the current one and the neighbours in slice 0. */
	for (index = 0; index < 9U; index++)
		h264sw_mb_reset(&mbs[index], -1);
	mbs[ADDR_CUR].slice = 0;
	mbs[ADDR_CUR].kind = (uint8_t)cur_kind;
	if (a_kind != NONE) {
		mbs[ADDR_A].slice = 0;
		mbs[ADDR_A].kind = (uint8_t)a_kind;
	}
	if (b_kind != NONE) {
		mbs[ADDR_B].slice = 0;
		mbs[ADDR_B].kind = (uint8_t)b_kind;
	}

	/* Links the slice context to the picture and finds the neighbours as the decoder does. */
	picture.mbs = mbs;
	picture.width_mbs = 3U;
	picture.height_mbs = 3U;
	picture.size_mbs = 9U;
	ctx.picture = &picture;
	ctx.header = &header;
	ctx.prev_mb_addr = -1;
	h264sw_mb_neighbours(mbs, 3U, ADDR_CUR, &ctx.n);
}

/* Checks the initialization of the context variables: the listed ctxIdx of each slice type by 9-5, the others 0, and the refusals. */
static void
check_contexts_init(void)
{
	uint8_t state[H264SW_CABAC_CONTEXTS];
	unsigned ctx_idx;
	unsigned wrong;

	/* An I slice at SliceQPY 26: ctxIdx 3..10 and 60..459 but 276 from set 0, the rest 0 (Table 9-11). */
	memset(state, 0xaa, sizeof(state));
	CHECK(h264sw_cabac_contexts_init(state, H264SW_SLICE_I, 2U, 26) == 0);
	wrong = 0;
	for (ctx_idx = 0; ctx_idx < H264SW_CABAC_CONTEXTS; ctx_idx++) {
		uint8_t expected;

		expected = 0;
		if ((ctx_idx >= 3U && ctx_idx <= 10U) || (ctx_idx >= 60U && ctx_idx != 276U))
			expected = vcodec_cabac_context_init(h264sw_cabac_init_mn[ctx_idx][0][0], h264sw_cabac_init_mn[ctx_idx][0][1], 26);
		if (state[ctx_idx] != expected)
			wrong++;
	}
	CHECK(wrong == 0U);

	/* A P slice with cabac_init_idc 1 at SliceQPY 40: ctxIdx 11..23 and 40..459 but 276 from set 2. */
	CHECK(h264sw_cabac_contexts_init(state, H264SW_SLICE_P, 1U, 40) == 0);
	wrong = 0;
	for (ctx_idx = 0; ctx_idx < H264SW_CABAC_CONTEXTS; ctx_idx++) {
		uint8_t expected;

		expected = 0;
		if ((ctx_idx >= 11U && ctx_idx <= 23U) || (ctx_idx >= 40U && ctx_idx != 276U))
			expected = vcodec_cabac_context_init(h264sw_cabac_init_mn[ctx_idx][2][0], h264sw_cabac_init_mn[ctx_idx][2][1], 40);
		if (state[ctx_idx] != expected)
			wrong++;
	}
	CHECK(wrong == 0U);

	/* A B slice with cabac_init_idc 2 and a SliceQPY below 0 (clipped to 0): ctxIdx 24..459 but 276 from set 3. */
	CHECK(h264sw_cabac_contexts_init(state, H264SW_SLICE_B, 2U, -6) == 0);
	wrong = 0;
	for (ctx_idx = 0; ctx_idx < H264SW_CABAC_CONTEXTS; ctx_idx++) {
		uint8_t expected;

		expected = 0;
		if (ctx_idx >= 24U && ctx_idx != 276U)
			expected = vcodec_cabac_context_init(h264sw_cabac_init_mn[ctx_idx][3][0], h264sw_cabac_init_mn[ctx_idx][3][1], 0);
		if (state[ctx_idx] != expected)
			wrong++;
	}
	CHECK(wrong == 0U);

	/* A cabac_init_idc above 2 and a slice type other than I, P and B are refused with every byte 0. */
	memset(state, 0xaa, sizeof(state));
	CHECK(h264sw_cabac_contexts_init(state, H264SW_SLICE_P, 3U, 26) == EINVAL);
	wrong = 0;
	for (ctx_idx = 0; ctx_idx < H264SW_CABAC_CONTEXTS; ctx_idx++) {
		if (state[ctx_idx] != 0U)
			wrong++;
	}
	CHECK(wrong == 0U);
	memset(state, 0xaa, sizeof(state));
	CHECK(h264sw_cabac_contexts_init(state, 3U, 0U, 26) == EINVAL);
	CHECK(state[0] == 0U && state[100] == 0U && state[H264SW_CABAC_CONTEXTS - 1U] == 0U);
}

/* Checks mb_skip_flag: 11 or 24 plus one for each available neighbour that is not skipped (9.3.3.1.1.1). */
static void
check_mb_skip(void)
{
	/* No neighbour in a P slice: 11. */
	env_make(H264SW_SLICE_P, H264SW_MB_P, NONE, NONE);
	CHECK(h264sw_ctx_mb_skip(&ctx) == 11U + 0U);

	/* A coded and B skipped: 11 + 1. */
	env_make(H264SW_SLICE_P, H264SW_MB_P, H264SW_MB_P, H264SW_MB_P_SKIP);
	CHECK(h264sw_ctx_mb_skip(&ctx) == 11U + 1U);

	/* Both coded (one intra): 11 + 2. */
	env_make(H264SW_SLICE_P, H264SW_MB_P, H264SW_MB_I_16X16, H264SW_MB_P_8X8);
	CHECK(h264sw_ctx_mb_skip(&ctx) == 11U + 2U);

	/* A B slice: B_Skip does not count, B_Direct_16x16 does: 24 + 1. */
	env_make(H264SW_SLICE_B, H264SW_MB_B, H264SW_MB_B_SKIP, H264SW_MB_B_DIRECT);
	CHECK(h264sw_ctx_mb_skip(&ctx) == 24U + 1U);
}

/* Checks the first bin's ctxIdxInc of mb_type in I and B slices (9.3.3.1.1.3), and 0 for P. */
static void
check_mb_type_inc(void)
{
	/* I: I_NxN in each of its forms does not count, Intra_16x16 and I_PCM do. */
	env_make(H264SW_SLICE_I, H264SW_MB_I_16X16, NONE, NONE);
	CHECK(h264sw_ctx_mb_type_inc(&ctx, H264SW_SLICE_I) == 0U);
	env_make(H264SW_SLICE_I, H264SW_MB_I_16X16, H264SW_MB_I_16X16, H264SW_MB_I_4X4);
	CHECK(h264sw_ctx_mb_type_inc(&ctx, H264SW_SLICE_I) == 1U);
	env_make(H264SW_SLICE_I, H264SW_MB_I_16X16, H264SW_MB_I_NXN, H264SW_MB_I_8X8);
	CHECK(h264sw_ctx_mb_type_inc(&ctx, H264SW_SLICE_I) == 0U);
	env_make(H264SW_SLICE_I, H264SW_MB_I_16X16, H264SW_MB_I_PCM, H264SW_MB_I_16X16);
	CHECK(h264sw_ctx_mb_type_inc(&ctx, H264SW_SLICE_I) == 2U);

	/* B: B_Skip and B_Direct_16x16 do not count, the others do (intra included). */
	env_make(H264SW_SLICE_B, H264SW_MB_B, H264SW_MB_B_SKIP, H264SW_MB_B_DIRECT);
	CHECK(h264sw_ctx_mb_type_inc(&ctx, H264SW_SLICE_B) == 0U);
	env_make(H264SW_SLICE_B, H264SW_MB_B, H264SW_MB_B_8X8, H264SW_MB_B_DIRECT);
	CHECK(h264sw_ctx_mb_type_inc(&ctx, H264SW_SLICE_B) == 1U);
	env_make(H264SW_SLICE_B, H264SW_MB_B, H264SW_MB_B, H264SW_MB_I_4X4);
	CHECK(h264sw_ctx_mb_type_inc(&ctx, H264SW_SLICE_B) == 2U);

	/* P: the prefix's first bin has a fixed context. */
	env_make(H264SW_SLICE_P, H264SW_MB_P, H264SW_MB_P, H264SW_MB_P);
	CHECK(h264sw_ctx_mb_type_inc(&ctx, H264SW_SLICE_P) == 0U);
}

/* Checks transform_size_8x8_flag: 399 plus one for each available neighbour with the flag (9.3.3.1.1.10). */
static void
check_transform_8x8(void)
{
	/* No neighbour: 399. */
	env_make(H264SW_SLICE_I, H264SW_MB_I_NXN, NONE, NONE);
	CHECK(h264sw_ctx_transform_8x8(&ctx) == 399U + 0U);

	/* A with the 8x8 transform, B without: 399 + 1. */
	env_make(H264SW_SLICE_I, H264SW_MB_I_NXN, H264SW_MB_I_8X8, H264SW_MB_I_4X4);
	mbs[ADDR_A].transform_8x8 = 1U;
	CHECK(h264sw_ctx_transform_8x8(&ctx) == 399U + 1U);

	/* Both: 399 + 2. */
	mbs[ADDR_B].transform_8x8 = 1U;
	CHECK(h264sw_ctx_transform_8x8(&ctx) == 399U + 2U);
}

/* Checks bin 0 of intra_chroma_pred_mode: 64 plus one for each available intra neighbour, not I_PCM, with a mode other than 0 (9.3.3.1.1.8). */
static void
check_chroma_mode(void)
{
	/* A intra with mode 2 counts; B I_PCM with mode 1 does not: 64 + 1. */
	env_make(H264SW_SLICE_I, H264SW_MB_I_16X16, H264SW_MB_I_16X16, H264SW_MB_I_PCM);
	mbs[ADDR_A].chroma_pred_mode = 2U;
	mbs[ADDR_B].chroma_pred_mode = 1U;
	CHECK(h264sw_ctx_chroma_mode(&ctx) == 64U + 1U);

	/* An inter neighbour does not count whatever its entry holds; an intra one with mode 0 does not either: 64. */
	env_make(H264SW_SLICE_P, H264SW_MB_I_16X16, H264SW_MB_P, H264SW_MB_I_4X4);
	mbs[ADDR_A].chroma_pred_mode = 3U;
	mbs[ADDR_B].chroma_pred_mode = 0U;
	CHECK(h264sw_ctx_chroma_mode(&ctx) == 64U + 0U);

	/* Both intra with modes 1 and 3: 64 + 2. */
	env_make(H264SW_SLICE_I, H264SW_MB_I_4X4, H264SW_MB_I_8X8, H264SW_MB_I_4X4);
	mbs[ADDR_A].chroma_pred_mode = 1U;
	mbs[ADDR_B].chroma_pred_mode = 3U;
	CHECK(h264sw_ctx_chroma_mode(&ctx) == 64U + 2U);
}

/*
 * Checks bin 0 of ref_idx: 54 + condTermFlagA + 2 * condTermFlagB, a flag
 * being 1 for an available, non-skipped, inter, non-direct partition with
 * refIdx above 0 (9.3.3.1.1.6).
 */
static void
check_ref_idx(void)
{
	/* No neighbour: 54. */
	env_make(H264SW_SLICE_P, H264SW_MB_P, NONE, NONE);
	CHECK(h264sw_ctx_ref_idx(&ctx, 0U, 0U, 0U) == 54U + 0U);

	/* At (0, 0): A's block 5 is in its quadrant 1, B's block 10 in its quadrant 2.  A has refIdx 2, B refIdx 0: 54 + 1. */
	env_make(H264SW_SLICE_P, H264SW_MB_P, H264SW_MB_P, H264SW_MB_P);
	mbs[ADDR_A].motion.ref_idx[0][1] = 2;
	mbs[ADDR_B].motion.ref_idx[0][2] = 0;
	CHECK(h264sw_ctx_ref_idx(&ctx, 0U, 0U, 0U) == 54U + 1U);

	/* B with refIdx 1 too: 54 + 3; list 1, which neither uses (refIdx -1), gives 54. */
	mbs[ADDR_B].motion.ref_idx[0][2] = 1;
	CHECK(h264sw_ctx_ref_idx(&ctx, 0U, 0U, 0U) == 54U + 3U);
	mbs[ADDR_A].motion.ref_idx[1][1] = -1;
	mbs[ADDR_B].motion.ref_idx[1][2] = -1;
	CHECK(h264sw_ctx_ref_idx(&ctx, 1U, 0U, 0U) == 54U + 0U);

	/* A skipped, B intra: neither counts. */
	env_make(H264SW_SLICE_P, H264SW_MB_P, H264SW_MB_P_SKIP, H264SW_MB_I_16X16);
	mbs[ADDR_A].motion.ref_idx[0][1] = 2;
	mbs[ADDR_B].motion.ref_idx[0][2] = 2;
	CHECK(h264sw_ctx_ref_idx(&ctx, 0U, 0U, 0U) == 54U + 0U);

	/* A B_8x8 neighbour whose quadrant 1 is direct does not count; B_Direct_16x16 does not count. */
	env_make(H264SW_SLICE_B, H264SW_MB_B, H264SW_MB_B_8X8, H264SW_MB_B_DIRECT);
	mbs[ADDR_A].direct = 2U;
	mbs[ADDR_A].motion.ref_idx[0][1] = 3;
	mbs[ADDR_B].motion.ref_idx[0][2] = 3;
	CHECK(h264sw_ctx_ref_idx(&ctx, 0U, 0U, 0U) == 54U + 0U);
	mbs[ADDR_A].direct = 1U;
	CHECK(h264sw_ctx_ref_idx(&ctx, 0U, 0U, 0U) == 54U + 1U);

	/* At (8, 0) the left partition is the current macroblock's quadrant 0 (refIdx 1), the upper one B's block 14 in quadrant 3 (refIdx 0): 54 + 1. */
	env_make(H264SW_SLICE_P, H264SW_MB_P, H264SW_MB_P, H264SW_MB_P);
	mbs[ADDR_CUR].motion.ref_idx[0][0] = 1;
	mbs[ADDR_B].motion.ref_idx[0][3] = 0;
	CHECK(h264sw_ctx_ref_idx(&ctx, 0U, 8U, 0U) == 54U + 1U);

	/* At (0, 8) the upper partition is the current quadrant 0 (refIdx 1), the left one A's block 13 in quadrant 3 (refIdx 2): 54 + 3. */
	mbs[ADDR_A].motion.ref_idx[0][3] = 2;
	CHECK(h264sw_ctx_ref_idx(&ctx, 0U, 0U, 8U) == 54U + 3U);
}

/* Checks bin 0 of an mvd component: 40 or 47 plus 0, 1 or 2 by absMvdCompA + absMvdCompB (below 3, 3..32, above 32; 9.3.3.1.1.7). */
static void
check_mvd(void)
{
	/* No neighbour: 40 and 47. */
	env_make(H264SW_SLICE_P, H264SW_MB_P, NONE, NONE);
	CHECK(h264sw_ctx_mvd(&ctx, 0U, 0U, 0U, 0U) == 40U + 0U);
	CHECK(h264sw_ctx_mvd(&ctx, 0U, 0U, 0U, 1U) == 47U + 0U);

	/* At (0, 0): A's block 5 and B's block 10.  Horizontal 2 + 1 = 3 gives 1; vertical 20 + 13 = 33 gives 2. */
	env_make(H264SW_SLICE_P, H264SW_MB_P, H264SW_MB_P, H264SW_MB_P);
	mbs[ADDR_A].mvd[0][5][0] = 2U;
	mbs[ADDR_B].mvd[0][10][0] = 1U;
	mbs[ADDR_A].mvd[0][5][1] = 20U;
	mbs[ADDR_B].mvd[0][10][1] = 13U;
	CHECK(h264sw_ctx_mvd(&ctx, 0U, 0U, 0U, 0U) == 40U + 1U);
	CHECK(h264sw_ctx_mvd(&ctx, 0U, 0U, 0U, 1U) == 47U + 2U);

	/* The edges of the ranges: 2 gives 0, 32 gives 1. */
	mbs[ADDR_B].mvd[0][10][0] = 0U;
	CHECK(h264sw_ctx_mvd(&ctx, 0U, 0U, 0U, 0U) == 40U + 0U);
	mbs[ADDR_B].mvd[0][10][1] = 12U;
	CHECK(h264sw_ctx_mvd(&ctx, 0U, 0U, 0U, 1U) == 47U + 1U);

	/* The other list's values are not read. */
	CHECK(h264sw_ctx_mvd(&ctx, 1U, 0U, 0U, 1U) == 47U + 0U);

	/* A skipped neighbour sent no mvd: A's 20 alone gives 1, and nothing once A is skipped. */
	mbs[ADDR_B].mvd[0][10][1] = 0U;
	CHECK(h264sw_ctx_mvd(&ctx, 0U, 0U, 0U, 1U) == 47U + 1U);
	mbs[ADDR_A].kind = H264SW_MB_P_SKIP;
	CHECK(h264sw_ctx_mvd(&ctx, 0U, 0U, 0U, 1U) == 47U + 0U);

	/* Inside the macroblock: at (4, 4) (block 3) the left block is the current block 2 and the upper one block 1. */
	env_make(H264SW_SLICE_P, H264SW_MB_P_8X8, H264SW_MB_P, H264SW_MB_P);
	mbs[ADDR_CUR].mvd[0][2][0] = 40U;
	mbs[ADDR_CUR].mvd[0][1][0] = 0U;
	CHECK(h264sw_ctx_mvd(&ctx, 0U, 4U, 4U, 0U) == 40U + 2U);
}

/* Checks the bins of coded_block_pattern: the luma prefix (73 + condTermFlagA + 2 * condTermFlagB) and the chroma suffix (77, + 4 for bin 1) (9.3.3.1.1.4). */
static void
check_cbp(void)
{
	/* No neighbour: block 0 gives 73; the chroma bins 77 and 81. */
	env_make(H264SW_SLICE_P, H264SW_MB_P, NONE, NONE);
	CHECK(h264sw_ctx_cbp_luma(&ctx, 0U, 0U) == 73U + 0U);
	CHECK(h264sw_ctx_cbp_chroma(&ctx, 0U) == 77U + 0U);
	CHECK(h264sw_ctx_cbp_chroma(&ctx, 1U) == 77U + 4U);

	/* Block 0's left is A's 8x8 block 1 (coded: 0), its upper B's block 2 (not coded: 1): 73 + 2. */
	env_make(H264SW_SLICE_P, H264SW_MB_P, H264SW_MB_P, H264SW_MB_P);
	mbs[ADDR_A].cbp = 0x02U;
	mbs[ADDR_B].cbp = 0x00U;
	CHECK(h264sw_ctx_cbp_luma(&ctx, 0U, 0U) == 73U + 2U);

	/* Block 1's left is the current block 0, by the bin coded for it; its upper B's block 3 (not coded). */
	CHECK(h264sw_ctx_cbp_luma(&ctx, 1U, 0x1U) == 73U + 0U + 2U);
	CHECK(h264sw_ctx_cbp_luma(&ctx, 1U, 0x0U) == 73U + 1U + 2U);

	/* Block 3's neighbours are both current: blocks 2 (left) and 1 (upper). */
	CHECK(h264sw_ctx_cbp_luma(&ctx, 3U, 0x2U) == 73U + 1U + 0U);
	CHECK(h264sw_ctx_cbp_luma(&ctx, 3U, 0x4U) == 73U + 0U + 2U);

	/* An I_PCM neighbour counts as coded (0), a skipped one as not coded (1). */
	env_make(H264SW_SLICE_P, H264SW_MB_P, H264SW_MB_I_PCM, H264SW_MB_P_SKIP);
	CHECK(h264sw_ctx_cbp_luma(&ctx, 0U, 0U) == 73U + 0U + 2U);

	/* Chroma: A with CodedBlockPatternChroma 1 counts for bin 0 only, B with 2 for both. */
	env_make(H264SW_SLICE_P, H264SW_MB_P, H264SW_MB_P, H264SW_MB_P);
	mbs[ADDR_A].cbp = 0x10U;
	mbs[ADDR_B].cbp = 0x20U;
	CHECK(h264sw_ctx_cbp_chroma(&ctx, 0U) == 77U + 1U + 2U);
	CHECK(h264sw_ctx_cbp_chroma(&ctx, 1U) == 77U + 4U + 0U + 2U);

	/* I_PCM counts for both bins, a skipped neighbour for neither. */
	env_make(H264SW_SLICE_P, H264SW_MB_P, H264SW_MB_I_PCM, H264SW_MB_P_SKIP);
	mbs[ADDR_B].cbp = 0x20U;
	CHECK(h264sw_ctx_cbp_chroma(&ctx, 0U) == 77U + 1U);
	CHECK(h264sw_ctx_cbp_chroma(&ctx, 1U) == 77U + 4U + 1U);
}

/* Checks bin 0 of mb_qp_delta: 60 + 1 when the previous macroblock in decoding order had an mb_qp_delta other than 0 (9.3.3.1.1.5). */
static void
check_qp_delta(void)
{
	/* The first macroblock of a slice: 60. */
	env_make(H264SW_SLICE_P, H264SW_MB_P, H264SW_MB_P, NONE);
	CHECK(h264sw_ctx_qp_delta(&ctx) == 60U + 0U);

	/* The previous one (A) inter with residual and a delta other than 0: 60 + 1; with a delta of 0: 60. */
	ctx.prev_mb_addr = (int32_t)ADDR_A;
	mbs[ADDR_A].cbp = 0x01U;
	ctx.prev_qp_delta_nonzero = 1;
	CHECK(h264sw_ctx_qp_delta(&ctx) == 60U + 1U);
	ctx.prev_qp_delta_nonzero = 0;
	CHECK(h264sw_ctx_qp_delta(&ctx) == 60U + 0U);

	/* Without residual an inter macroblock sent no delta; an Intra_16x16 one always sends it. */
	ctx.prev_qp_delta_nonzero = 1;
	mbs[ADDR_A].cbp = 0x00U;
	CHECK(h264sw_ctx_qp_delta(&ctx) == 60U + 0U);
	mbs[ADDR_A].kind = H264SW_MB_I_16X16;
	CHECK(h264sw_ctx_qp_delta(&ctx) == 60U + 1U);

	/* Skipped and I_PCM macroblocks sent none. */
	mbs[ADDR_A].cbp = 0x2fU;
	mbs[ADDR_A].kind = H264SW_MB_P_SKIP;
	CHECK(h264sw_ctx_qp_delta(&ctx) == 60U + 0U);
	mbs[ADDR_A].kind = H264SW_MB_I_PCM;
	CHECK(h264sw_ctx_qp_delta(&ctx) == 60U + 0U);
}

/*
 * Checks coded_block_flag: 85 + ctxIdxBlockCatOffset (0, 4, 8, 12, 16) +
 * condTermFlagA + 2 * condTermFlagB, a missing neighbour counting 1 for
 * an intra current macroblock and 0 for an inter one (9.3.3.1.1.9).
 */
static void
check_coded_block_flag(void)
{
	/* No neighbours, a luma 4x4 block: intra 85 + 8 + 3, inter 85 + 8. */
	env_make(H264SW_SLICE_I, H264SW_MB_I_4X4, NONE, NONE);
	CHECK(h264sw_ctx_coded_block_flag(&ctx, H264SW_CAT_LUMA_4X4, 0U, 0U) == 85U + 8U + 3U);
	env_make(H264SW_SLICE_P, H264SW_MB_P, NONE, NONE);
	CHECK(h264sw_ctx_coded_block_flag(&ctx, H264SW_CAT_LUMA_4X4, 0U, 0U) == 85U + 8U + 0U);

	/* Block 0: A's block 5 coded, B's block 10 not: 85 + 8 + 1.  Cat 1 the same with offset 4. */
	env_make(H264SW_SLICE_P, H264SW_MB_P, H264SW_MB_P, H264SW_MB_P);
	mbs[ADDR_A].coded_flags = 1U << 5;
	mbs[ADDR_B].coded_flags = 1U << 9;
	CHECK(h264sw_ctx_coded_block_flag(&ctx, H264SW_CAT_LUMA_4X4, 0U, 0U) == 85U + 8U + 1U);
	CHECK(h264sw_ctx_coded_block_flag(&ctx, H264SW_CAT_LUMA_AC, 0U, 0U) == 85U + 4U + 1U);

	/* Block 1: the left one is the current block 0. */
	mbs[ADDR_CUR].coded_flags = 1U << 0;
	CHECK(h264sw_ctx_coded_block_flag(&ctx, H264SW_CAT_LUMA_4X4, 1U, 0U) == 85U + 8U + 1U);

	/* The luma DC (bit 16) of the neighbouring macroblocks: A coded, B skipped: 85 + 0 + 1. */
	env_make(H264SW_SLICE_I, H264SW_MB_I_16X16, H264SW_MB_I_16X16, H264SW_MB_P_SKIP);
	mbs[ADDR_A].coded_flags = 1U << 16;
	CHECK(h264sw_ctx_coded_block_flag(&ctx, H264SW_CAT_LUMA_DC, 0U, 0U) == 85U + 0U + 1U);

	/* The chroma DC of Cb (bit 17) and Cr (bit 18): B coded in Cr only, A I_PCM (always coded). */
	env_make(H264SW_SLICE_P, H264SW_MB_P, H264SW_MB_I_PCM, H264SW_MB_P);
	mbs[ADDR_B].coded_flags = 1U << 18;
	CHECK(h264sw_ctx_coded_block_flag(&ctx, H264SW_CAT_CHROMA_DC, 0U, 1U) == 85U + 12U + 1U);
	CHECK(h264sw_ctx_coded_block_flag(&ctx, H264SW_CAT_CHROMA_DC, 0U, 2U) == 85U + 12U + 3U);

	/* The chroma AC of Cr block 0: its left is A's Cr block 1 (bit 19 + 4 + 1), its upper B's Cr block 2 (bit 19 + 4 + 2). */
	env_make(H264SW_SLICE_P, H264SW_MB_P, H264SW_MB_P, H264SW_MB_P);
	mbs[ADDR_A].coded_flags = 1U << (19 + 4 + 1);
	mbs[ADDR_B].coded_flags = 1U << (19 + 4 + 2);
	CHECK(h264sw_ctx_coded_block_flag(&ctx, H264SW_CAT_CHROMA_AC, 0U, 2U) == 85U + 16U + 3U);
	CHECK(h264sw_ctx_coded_block_flag(&ctx, H264SW_CAT_CHROMA_AC, 0U, 1U) == 85U + 16U + 0U);
}

/* Checks significant_coeff_flag and last_significant_coeff_flag: the offsets of Tables 9-34 and 9-40 and the increments of 9.3.3.1.3 and Table 9-43. */
static void
check_significance(void)
{
	/* Cat 0 and 2: the position itself. */
	CHECK(h264sw_ctx_significance(H264SW_CAT_LUMA_DC, 3U, 0) == 105U + 0U + 3U);
	CHECK(h264sw_ctx_significance(H264SW_CAT_LUMA_DC, 3U, 1) == 166U + 0U + 3U);
	CHECK(h264sw_ctx_significance(H264SW_CAT_LUMA_4X4, 14U, 0) == 105U + 29U + 14U);

	/* Cat 1 (positions counted from startIdx 0 of the 15-coefficient list) and 4. */
	CHECK(h264sw_ctx_significance(H264SW_CAT_LUMA_AC, 5U, 0) == 105U + 15U + 5U);
	CHECK(h264sw_ctx_significance(H264SW_CAT_LUMA_AC, 5U, 1) == 166U + 15U + 5U);
	CHECK(h264sw_ctx_significance(H264SW_CAT_CHROMA_AC, 7U, 0) == 105U + 47U + 7U);

	/* Cat 3 (NumC8x8 1): Min( idx, 2 ). */
	CHECK(h264sw_ctx_significance(H264SW_CAT_CHROMA_DC, 0U, 0) == 105U + 44U + 0U);
	CHECK(h264sw_ctx_significance(H264SW_CAT_CHROMA_DC, 1U, 1) == 166U + 44U + 1U);
	CHECK(h264sw_ctx_significance(H264SW_CAT_CHROMA_DC, 2U, 0) == 105U + 44U + 2U);

	/* Cat 5, Table 9-43 (frame coded): positions 0, 22 and 62. */
	CHECK(h264sw_ctx_significance(H264SW_CAT_LUMA_8X8, 0U, 0) == 402U + 0U);
	CHECK(h264sw_ctx_significance(H264SW_CAT_LUMA_8X8, 0U, 1) == 417U + 0U);
	CHECK(h264sw_ctx_significance(H264SW_CAT_LUMA_8X8, 22U, 0) == 402U + 6U);
	CHECK(h264sw_ctx_significance(H264SW_CAT_LUMA_8X8, 22U, 1) == 417U + 2U);
	CHECK(h264sw_ctx_significance(H264SW_CAT_LUMA_8X8, 62U, 0) == 402U + 12U);
	CHECK(h264sw_ctx_significance(H264SW_CAT_LUMA_8X8, 62U, 1) == 417U + 8U);
}

/* Checks the prefix bins of coeff_abs_level_minus1: 227 or 426 + ctxIdxBlockCatOffset + the increments 9-23 (bin 0) and 9-24 (later bins). */
static void
check_abs_level(void)
{
	/* Bin 0: 1 + eq1 up to 4 while no level above 1 was coded, then 0. */
	CHECK(h264sw_ctx_abs_level(H264SW_CAT_LUMA_DC, 0U, 0U, 0U) == 227U + 0U + 1U);
	CHECK(h264sw_ctx_abs_level(H264SW_CAT_LUMA_DC, 0U, 2U, 0U) == 227U + 0U + 3U);
	CHECK(h264sw_ctx_abs_level(H264SW_CAT_LUMA_DC, 0U, 5U, 0U) == 227U + 0U + 4U);
	CHECK(h264sw_ctx_abs_level(H264SW_CAT_LUMA_4X4, 0U, 3U, 1U) == 227U + 20U + 0U);

	/* Later bins: 5 + Min( gt1, 4 ), every bin the same. */
	CHECK(h264sw_ctx_abs_level(H264SW_CAT_LUMA_AC, 1U, 4U, 0U) == 227U + 10U + 5U);
	CHECK(h264sw_ctx_abs_level(H264SW_CAT_LUMA_AC, 7U, 0U, 3U) == 227U + 10U + 8U);
	CHECK(h264sw_ctx_abs_level(H264SW_CAT_CHROMA_AC, 13U, 0U, 6U) == 227U + 39U + 9U);

	/* The chroma DC stops at 5 + 3. */
	CHECK(h264sw_ctx_abs_level(H264SW_CAT_CHROMA_DC, 1U, 0U, 6U) == 227U + 30U + 8U);
	CHECK(h264sw_ctx_abs_level(H264SW_CAT_CHROMA_DC, 0U, 1U, 0U) == 227U + 30U + 2U);

	/* Cat 5 has its own offset 426. */
	CHECK(h264sw_ctx_abs_level(H264SW_CAT_LUMA_8X8, 0U, 2U, 0U) == 426U + 3U);
	CHECK(h264sw_ctx_abs_level(H264SW_CAT_LUMA_8X8, 3U, 2U, 2U) == 426U + 7U);
}
