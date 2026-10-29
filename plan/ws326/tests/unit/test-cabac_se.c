/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the CABAC syntax elements (h264sw-cabac-se) of the software
 * H.264 decoder.  Each test builds the bin strings of the elements from
 * the binarization tables of 9.3.2 (typed here from Tables 9-36, 9-37
 * and 9-38 and the pseudo-code 9-6), codes them with the informative
 * encoder of 9.3.4 (cabacwriter.h) under context indices worked out
 * from a neighbour state the test sets up by hand, and decodes the
 * bytes with the functions under test.  A decoder that picks another
 * context than the encoder did reads other bins or ends with other
 * context variables, so every stream also compares the context
 * variables of the two sides at the end.  The context variables start
 * from random states, not from the initialization tables, so a wrong
 * context shows at once.  The error cases, truncated streams and random
 * bytes must end with the errors the header gives and no crash.
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
#include "userland/desktop/libmedia/h264sw-context.h"

/* The marker of the terminate bin in the context lists below (ctxIdx 276). */
#define TERMINATE_BIN 0xffffU

/* The macroblock positions in the 3 by 3 picture, with the current macroblock in the middle. */
#define ADDR_D 0U
#define ADDR_B 1U
#define ADDR_C 2U
#define ADDR_A 3U
#define ADDR_CUR 4U

/* A neighbour that is not available, in a table of cases. */
#define NONE 0xffU

/* The macroblock kinds, shortened. */
#define K_I_NXN H264SW_MB_I_NXN
#define K_I_4X4 H264SW_MB_I_4X4
#define K_I_8X8 H264SW_MB_I_8X8
#define K_I_16X16 H264SW_MB_I_16X16
#define K_I_PCM H264SW_MB_I_PCM
#define K_P_SKIP H264SW_MB_P_SKIP
#define K_P H264SW_MB_P
#define K_P_8X8 H264SW_MB_P_8X8
#define K_B_SKIP H264SW_MB_B_SKIP
#define K_B_DIRECT H264SW_MB_B_DIRECT
#define K_B H264SW_MB_B
#define K_B_8X8 H264SW_MB_B_8X8

/* The state shared by the cases: the encoder, its output, and the decoder's slice context with a 3 by 3 picture. */
static struct bitwriter written;
static struct cabacwriter writer;
static uint8_t initial_state[CABACWRITER_CONTEXTS];
static struct h264sw_slice_ctx ctx;
static struct h264sw_slice_header header;
static struct h264sw_picture_ctx picture;
static struct h264sw_mb mbs[9];
static unsigned stream_slice_type;
static uint32_t random_state = 12345U;

/*
 * Table 9-36, the bin strings of the mb_type values of I slices, by value.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-36, binIdx 0 to 6 left to right.
 */
static const char *const bins_i[26] = {
	"0",
	"100000", "100001", "100010", "100011",
	"1001000", "1001001", "1001010", "1001011",
	"1001100", "1001101", "1001110", "1001111",
	"101000", "101001", "101010", "101011",
	"1011000", "1011001", "1011010", "1011011",
	"1011100", "1011101", "1011110", "1011111",
	"11"
};

/*
 * Table 9-37, the bin strings of the mb_type values of P slices (the
 * values 0 to 3; 4 is not allowed and the intra values have the prefix 1)
 * and of B slices (the values 0 to 22; the intra values have the prefix
 * 111101).
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-37.
 */
static const char *const bins_p[4] = { "000", "011", "010", "001" };
static const char *const bins_b[23] = {
	"0", "100", "101",
	"110000", "110001", "110010", "110011", "110100", "110101", "110110", "110111",
	"111110",
	"1110000", "1110001", "1110010", "1110011", "1110100", "1110101", "1110110", "1110111",
	"1111000", "1111001",
	"111111"
};

/*
 * Table 9-38, the bin strings of the sub_mb_type values of P slices
 * (0 to 3) and B slices (0 to 12).
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-38.
 */
static const char *const bins_sub_p[4] = { "1", "00", "011", "010" };
static const char *const bins_sub_b[13] = {
	"0", "100", "101",
	"11000", "11001", "11010", "11011",
	"111000", "111001", "111010", "111011",
	"11110", "11111"
};

/*
 * The ctxIdx of the first context of each block category for
 * coded_block_flag, significant_coeff_flag, last_significant_coeff_flag
 * and coeff_abs_level_minus1: ctxIdxOffset of Table 9-34 plus
 * ctxIdxBlockCatOffset of Table 9-40, added up by hand.  coded_block_flag
 * of category 5 is not read.
 */
static const unsigned ctx_cbf[5] = { 85, 89, 93, 97, 101 };
static const unsigned ctx_sig[6] = { 105, 120, 134, 149, 152, 402 };
static const unsigned ctx_last[6] = { 166, 181, 195, 210, 213, 417 };
static const unsigned ctx_abs[6] = { 227, 237, 247, 257, 266, 426 };

/*
 * ctxIdxInc of Table 9-43 for 8x8 blocks of frame coded macroblocks, by
 * levelListIdx 0 to 62: significant_coeff_flag, then
 * last_significant_coeff_flag.  Extracted from the page's text and
 * checked against its image.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-43.
 */
static const unsigned char table_943_sig[63] = {
	0, 1, 2, 3, 4, 5, 5, 4, 4, 3, 3, 4, 4, 4, 5, 5, 4, 4, 4, 4, 3, 3, 6, 7, 7, 7, 8, 9, 10, 9, 8, 7,
	7, 6, 11, 12, 13, 11, 6, 7, 8, 9, 14, 10, 9, 8, 6, 11, 12, 13, 11, 6, 9, 14, 10, 9, 11, 12, 13, 11, 14, 10, 12
};
static const unsigned char table_943_last[63] = {
	0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8
};

/* A neighbour setup of one case: the availability of A and B and their kinds, and the ctxIdxInc worked out by hand for the first bin. */
struct neighbour_case {
	unsigned a_kind;
	unsigned b_kind;
	unsigned inc;
};

/* Forward declarations of the static functions. */
static uint32_t next_random(void);
static void env_make(unsigned slice_type, unsigned a_kind, unsigned b_kind);
static void stream_begin(void);
static void stream_end(int end_flag);
static void stream_check(int end_flag);
static void put(unsigned ctx_idx, unsigned bin);
static void put_bypass(unsigned bin);
static void put_exp_golomb(unsigned value, unsigned order);
static unsigned table_inc(unsigned offset, unsigned bin_idx, const char *bins, unsigned inc0);
static void put_string(const char *bins, unsigned offset, unsigned inc0);
static void put_mb_type(unsigned slice_type, unsigned value, unsigned inc0);
static void put_unary(unsigned value, unsigned ctx_first, unsigned ctx_second, unsigned ctx_rest);
static void put_mvd(unsigned offset, unsigned inc0, int value);
static void put_abs_level(unsigned cat, unsigned abs_minus1, unsigned eq1, unsigned gt1);
static void put_residual(unsigned cat, unsigned cbf_inc, const int32_t *levels, unsigned start, unsigned end);
static unsigned residual_sig_inc(unsigned cat, unsigned index);
static unsigned residual_last_inc(unsigned cat, unsigned index);
static void residual_case(unsigned cat, unsigned blk, unsigned comp, unsigned start, unsigned end, unsigned max_coeff, unsigned cbf_inc, const int32_t *levels);
static void check_mb_skip(void);
static void check_mb_type_i(void);
static void check_mb_type_p(void);
static void check_mb_type_b(void);
static void check_mb_type_errors(void);
static void check_sub_mb_type(void);
static void check_transform_8x8(void);
static void check_intra_mode(void);
static void check_chroma_mode(void);
static void check_ref_idx(void);
static void check_mvd(void);
static void check_cbp(void);
static void check_qp_delta(void);
static void check_end_of_slice(void);
static void check_residual_levels(void);
static void check_residual_contexts(void);
static void check_residual_errors(void);
static void levels_pattern(int32_t *levels, unsigned start, unsigned end, unsigned pattern);
static void check_fuzz(void);
static unsigned ref_cond_luma(const struct h264sw_mb *mb_n, unsigned bit);
static unsigned ref_cond_chroma(const struct h264sw_mb *mb_n, unsigned bin_idx);

/* Gives the next pseudo-random number (xorshift), for test data. */
static uint32_t
next_random(
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
 * Sets up a slice of the given type over a 3 by 3 picture: the current
 * macroblock is in the middle, A and B are its neighbours with the given
 * kinds (NONE for one that is not available).  Every entry is reset as the
 * decoder resets it, and the current macroblock's kind is an intra one.
 */
static void
env_make(
	unsigned slice_type,
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
	stream_slice_type = slice_type;

	/* Resets each macroblock entry as an undecoded one, then places the current one and the neighbours in slice 0. */
	for (index = 0; index < 9U; index++)
		h264sw_mb_reset(&mbs[index], -1);
	mbs[ADDR_CUR].slice = 0;
	mbs[ADDR_CUR].kind = K_I_16X16;
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

/* Starts an encoded stream: random context variables (the same ones the decoder will be given), and an engine on an empty output. */
static void
stream_begin(
	void)
{
	unsigned ctx_idx;
	uint32_t number;

	/* Gives every context variable a random state: pStateIdx 0 to 62 and valMPS 0 or 1. */
	memset(&writer, 0, sizeof(writer));
	for (ctx_idx = 0; ctx_idx < CABACWRITER_CONTEXTS; ctx_idx++) {
		number = next_random();
		writer.state[ctx_idx] = (uint8_t)((((number >> 8) & 1U) << 6) | (number % 63U));
	}
	memcpy(initial_state, writer.state, sizeof(initial_state));

	/* Starts the encoder on an empty output. */
	bitwriter_init(&written);
	cabacwriter_engine_init(&writer, &written);
}

/*
 * Finishes the stream: end_of_slice_flag 1 when end_flag is set (a stream
 * that ended with the I_PCM terminate bin has its flush already), the
 * bits to the byte boundary, and starts the decoder's engine on the
 * bytes with the encoder's initial context variables.
 */
static void
stream_end(
	int end_flag)
{
	struct h264sw_bits bits;
	int status;

	/* Puts the flag that ends the slice. */
	if (end_flag)
		cabacwriter_terminate(&writer, 1U);

	/* Pads the last byte with zero bits. */
	while (written.bits % 8U != 0U)
		bitwriter_u(&written, 0U, 1U);
	CHECK(written.overflow == 0);

	/* Starts the decoder on the bytes. */
	h264sw_bits_init(&bits, written.data, bitwriter_bytes(&written));
	status = h264sw_cabac_start(&ctx.cabac, &bits, stream_slice_type, 0U, 26);
	CHECK(status == 0);

	/* Gives the decoder the same context variables the encoder began with. */
	memcpy(ctx.cabac.state, initial_state, sizeof(initial_state));
}

/* Checks, after a stream is decoded, that no read went past the end, the slice ends where the encoder ended it, and the decoder's context variables equal the encoder's. */
static void
stream_check(
	int end_flag)
{
	unsigned flag;

	/* The terminate bin that ends the slice is 1. */
	if (end_flag) {
		flag = h264sw_cabac_end_of_slice(&ctx);
		CHECK(flag == 1U);
	}

	/* No read ran past the data. */
	CHECK(ctx.cabac.error == 0);

	/* The contexts the decoder used are the contexts the encoder used: their variables ended the same. */
	CHECK(memcmp(ctx.cabac.state, writer.state, CABACWRITER_CONTEXTS) == 0);
}

/* Encodes one decision. */
static void
put(
	unsigned ctx_idx,
	unsigned bin)
{
	/* Codes the bin with its context. */
	cabacwriter_decision(&writer, ctx_idx, bin);
}

/* Encodes one bypass bin. */
static void
put_bypass(
	unsigned bin)
{
	/* Codes the bin with equal probabilities. */
	cabacwriter_bypass(&writer, bin);
}

/* Encodes the k-th order Exp-Golomb suffix of value as the pseudo-code 9-6 gives it, with bypass bins. */
static void
put_exp_golomb(
	unsigned value,
	unsigned order)
{
	unsigned stop;

	/* A 1 and a larger order for each 2^k taken off, then a 0 and the k remaining bits, most significant first. */
	stop = 0;
	while (stop == 0U) {
		if (value >= (1U << order)) {
			put_bypass(1U);
			value -= 1U << order;
			order++;
		} else {
			put_bypass(0U);
			while (order > 0U) {
				order--;
				put_bypass((value >> order) & 1U);
			}
			stop = 1;
		}
	}
}

/*
 * Gives ctxIdxInc of bin bin_idx of a bin string for the ctxIdxOffset
 * of an mb_type or sub_mb_type, from Tables 9-39 and 9-41; bins is the
 * string (for the bins it depends on), inc0 the ctxIdxInc of bin 0 where
 * the neighbours give it.  TERMINATE_BIN is bin 1 of an intra mb_type.
 */
static unsigned
table_inc(
	unsigned offset,
	unsigned bin_idx,
	const char *bins,
	unsigned inc0)
{
	/* The first bin of the types with neighbour contexts. */
	if (bin_idx == 0U && (offset == 3U || offset == 27U))
		return inc0;

	/* Each offset has its row of Table 9-39, with the bin-dependent entries of Table 9-41. */
	switch (offset) {
	case 3:
		if (bin_idx == 1U)
			return TERMINATE_BIN;
		if (bin_idx == 2U)
			return 3;
		if (bin_idx == 3U)
			return 4;
		if (bin_idx == 4U)
			return bins[3] != '0' ? 5U : 6U;
		if (bin_idx == 5U)
			return bins[3] != '0' ? 6U : 7U;
		return 7;
	case 14:
		if (bin_idx == 2U)
			return bins[1] != '1' ? 2U : 3U;
		return bin_idx;
	case 17:
	case 32:
		if (bin_idx == 0U)
			return 0;
		if (bin_idx == 1U)
			return TERMINATE_BIN;
		if (bin_idx == 2U)
			return 1;
		if (bin_idx == 3U)
			return 2;
		if (bin_idx == 4U)
			return bins[3] != '0' ? 2U : 3U;
		return 3;
	case 27:
		if (bin_idx == 1U)
			return 3;
		if (bin_idx == 2U)
			return bins[1] != '0' ? 4U : 5U;
		return 5;
	case 36:
		if (bin_idx == 0U)
			return 0;
		if (bin_idx == 1U)
			return 1;
		if (bin_idx == 2U)
			return bins[1] != '0' ? 2U : 3U;
		return 3;
	default:
		/* The ctxIdxOffset 21 has the increments 0, 1 and 2 for its three bins. */
		return bin_idx;
	}
}

/* Encodes a bin string with the contexts of the ctxIdxOffset, the terminate bin being coded with the terminate procedure. */
static void
put_string(
	const char *bins,
	unsigned offset,
	unsigned inc0)
{
	unsigned bin_idx;
	unsigned inc;
	unsigned bin;

	/* Codes each bin of the string with its context. */
	for (bin_idx = 0; bins[bin_idx] != '\0'; bin_idx++) {
		bin = (unsigned)(bins[bin_idx] - '0');
		inc = table_inc(offset, bin_idx, bins, inc0);
		if (inc == TERMINATE_BIN) {
			cabacwriter_terminate(&writer, bin);
		} else {
			put(offset + inc, bin);
		}
	}
}

/* Encodes the mb_type value of a slice type: the prefix and the suffix as 9.3.2.5 composes them. */
static void
put_mb_type(
	unsigned slice_type,
	unsigned value,
	unsigned inc0)
{
	/* An I slice codes Table 9-36 with ctxIdxOffset 3. */
	if (slice_type == H264SW_SLICE_I) {
		put_string(bins_i[value], 3U, inc0);
		return;
	}

	/* A P slice codes a P type with offset 14, or the prefix 1 and an I suffix with offset 17. */
	if (slice_type == H264SW_SLICE_P) {
		if (value < 5U) {
			put_string(bins_p[value], 14U, 0U);
		} else {
			put_string("1", 14U, 0U);
			put_string(bins_i[value - 5U], 17U, 0U);
		}
		return;
	}

	/* A B slice codes a B type with offset 27, or the prefix 111101 and an I suffix with offset 32. */
	if (value < 23U) {
		put_string(bins_b[value], 27U, inc0);
	} else {
		put_string("111101", 27U, inc0);
		put_string(bins_i[value - 23U], 32U, 0U);
	}
}

/* Encodes a U binarization of value with three contexts: bin 0, bin 1 and the later bins. */
static void
put_unary(
	unsigned value,
	unsigned ctx_first,
	unsigned ctx_second,
	unsigned ctx_rest)
{
	unsigned bin_idx;
	unsigned ctx_idx;

	/* Codes value bins of 1, then a 0. */
	for (bin_idx = 0; bin_idx <= value; bin_idx++) {
		ctx_idx = ctx_rest;
		if (bin_idx == 0U)
			ctx_idx = ctx_first;
		else if (bin_idx == 1U)
			ctx_idx = ctx_second;
		put(ctx_idx, bin_idx < value ? 1U : 0U);
	}
}

/* Encodes one mvd component as UEG3 with signedValFlag 1 and uCoff 9 (9.3.2.3): the prefix with the contexts of Table 9-39, the suffix and the sign bypass. */
static void
put_mvd(
	unsigned offset,
	unsigned inc0,
	int value)
{
	unsigned magnitude;
	unsigned prefix;
	unsigned bin_idx;
	unsigned inc;

	/* The magnitude and the length of the truncated unary prefix, at most 9. */
	magnitude = value < 0 ? (unsigned)(-(int64_t)value) : (unsigned)value;
	prefix = magnitude < 9U ? magnitude : 9U;

	/* Codes the prefix bins of 1 and, below 9, the 0 that ends them: the increments are inc0, 3, 4, 5, 6, 6, ... */
	for (bin_idx = 0; bin_idx < prefix; bin_idx++) {
		inc = bin_idx == 0U ? inc0 : (bin_idx < 4U ? bin_idx + 2U : 6U);
		put(offset + inc, 1U);
	}
	if (magnitude < 9U) {
		inc = prefix == 0U ? inc0 : (prefix < 4U ? prefix + 2U : 6U);
		put(offset + inc, 0U);
	} else {
		put_exp_golomb(magnitude - 9U, 3U);
	}

	/* The sign, for a value that is not 0. */
	if (value != 0)
		put_bypass(value < 0 ? 1U : 0U);
}

/* Encodes coeff_abs_level_minus1 as UEG0 with uCoff 14, with the contexts of 9-23 and 9-24 for the numbers of levels of absolute value 1 and above 1 so far. */
static void
put_abs_level(
	unsigned cat,
	unsigned abs_minus1,
	unsigned eq1,
	unsigned gt1)
{
	unsigned inc_first;
	unsigned inc_rest;
	unsigned limit;
	unsigned prefix;
	unsigned bin_idx;

	/* 9-23: bin 0 is 0 once a level above 1 was seen, else Min( 4, 1 + eq1 ). */
	if (gt1 != 0U) {
		inc_first = 0;
	} else {
		inc_first = 1U + eq1;
		if (inc_first > 4U)
			inc_first = 4;
	}

	/* 9-24: the other bins are 5 + Min( 4 - (cat == 3), gt1 ). */
	limit = cat == 3U ? 3U : 4U;
	inc_rest = 5U + (gt1 < limit ? gt1 : limit);

	/* The prefix: min( 14, value ) bins of 1 and, below 14, a 0. */
	prefix = abs_minus1 < 14U ? abs_minus1 : 14U;
	for (bin_idx = 0; bin_idx < prefix; bin_idx++)
		put(ctx_abs[cat] + (bin_idx == 0U ? inc_first : inc_rest), 1U);
	if (abs_minus1 < 14U) {
		put(ctx_abs[cat] + (prefix == 0U ? inc_first : inc_rest), 0U);
	} else {
		/* The suffix of 9-6 with k 0 for the part above 14. */
		put_exp_golomb(abs_minus1 - 14U, 0U);
	}
}

/* Gives ctxIdxInc of significant_coeff_flag of a block of the category at the position (9-21, 9-22, Table 9-43). */
static unsigned
residual_sig_inc(
	unsigned cat,
	unsigned index)
{
	/* The 8x8 block has its table. */
	if (cat == 5U)
		return table_943_sig[index];

	/* The chroma DC is Min( index / 1, 2 ). */
	if (cat == 3U)
		return index < 2U ? index : 2U;

	/* The others use the position. */
	return index;
}

/* Gives ctxIdxInc of last_significant_coeff_flag of a block of the category at the position (9-21, 9-22, Table 9-43). */
static unsigned
residual_last_inc(
	unsigned cat,
	unsigned index)
{
	/* The 8x8 block has its table. */
	if (cat == 5U)
		return table_943_last[index];

	/* The chroma DC is Min( index / 1, 2 ). */
	if (cat == 3U)
		return index < 2U ? index : 2U;

	/* The others use the position. */
	return index;
}

/*
 * Encodes residual_block_cabac (7.3.5.3.3) for the levels, which are
 * zero outside start..end.  A block with no level other than 0 is coded
 * with coded_block_flag 0; category 5 has no flag and must have a level.
 */
static void
put_residual(
	unsigned cat,
	unsigned cbf_inc,
	const int32_t *levels,
	unsigned start,
	unsigned end)
{
	unsigned index;
	unsigned count;
	unsigned last_pos;
	unsigned eq1;
	unsigned gt1;
	unsigned abs_level;

	/* Finds the last non-zero level and how many there are. */
	count = 0;
	last_pos = start;
	for (index = start; index <= end; index++) {
		if (levels[index] != 0) {
			count++;
			last_pos = index;
		}
	}

	/* coded_block_flag, except for the 8x8 block. */
	if (cat != 5U)
		put(ctx_cbf[cat] + cbf_inc, count != 0U ? 1U : 0U);
	if (count == 0U)
		return;

	/* The significance map: a flag for each position before the last one, and a last flag after each significant one; the final position needs none when nothing ended the map before it. */
	for (index = start; index < end; index++) {
		put(ctx_sig[cat] + residual_sig_inc(cat, index), levels[index] != 0 ? 1U : 0U);
		if (levels[index] != 0) {
			put(ctx_last[cat] + residual_last_inc(cat, index), index == last_pos ? 1U : 0U);
			if (index == last_pos)
				break;
		}
	}

	/* The levels from the last one down: the absolute value minus 1, then the sign. */
	eq1 = 0;
	gt1 = 0;
	for (index = last_pos + 1U; index > start; index--) {
		if (levels[index - 1U] == 0)
			continue;
		abs_level = levels[index - 1U] < 0 ? (unsigned)(-(int64_t)levels[index - 1U]) : (unsigned)levels[index - 1U];
		put_abs_level(cat, abs_level - 1U, eq1, gt1);
		put_bypass(levels[index - 1U] < 0 ? 1U : 0U);
		if (abs_level == 1U)
			eq1++;
		else
			gt1++;
	}
}

/* Encodes a block, decodes it, and checks the levels, coded_block_flag and context variables; the neighbour state is already set up. */
static void
residual_case(
	unsigned cat,
	unsigned blk,
	unsigned comp,
	unsigned start,
	unsigned end,
	unsigned max_coeff,
	unsigned cbf_inc,
	const int32_t *levels)
{
	int32_t got[64];
	unsigned coded;
	unsigned expected_coded;
	unsigned index;
	int status;

	/* A block coded without coded_block_flag has a level; the others are coded when some level is not 0. */
	expected_coded = 0;
	for (index = start; index <= end; index++) {
		if (levels[index] != 0)
			expected_coded = 1;
	}
	if (cat == 5U)
		expected_coded = 1;

	/* A block of category 5 with no level cannot be coded (its coded_block_flag is not sent), so there is nothing to check. */
	if (cat == 5U && expected_coded != 0U) {
		for (index = start; index <= end; index++) {
			if (levels[index] != 0)
				break;
		}
		if (index > end)
			return;
	}

	/* Encodes, then decodes the block. */
	stream_begin();
	put_residual(cat, cbf_inc, levels, start, end);
	stream_end(1);
	memset(got, 0x55, sizeof(got));
	coded = 7;
	status = h264sw_cabac_residual_block(&ctx, cat, blk, comp, start, end, max_coeff, got, &coded);

	/* The levels, the flag and the contexts all agree. */
	CHECK(status == 0);
	CHECK(coded == expected_coded);
	CHECK(memcmp(got, levels, max_coeff * sizeof(int32_t)) == 0);
	stream_check(1);
}

/* Checks mb_skip_flag: the context from the skipped state of the neighbours in P and B slices, and a slice with no skip. */
static void
check_mb_skip(
	void)
{
	static const struct neighbour_case cases[] = {
		/* Neither neighbour is available: 0. */
		{ NONE, NONE, 0 },
		/* A skipped neighbour does not count. */
		{ K_P_SKIP, NONE, 0 },
		{ K_P_SKIP, K_P_SKIP, 0 },
		{ K_B_SKIP, K_B_SKIP, 0 },
		/* An available neighbour that is not skipped counts: 1 each. */
		{ K_P, NONE, 1 },
		{ NONE, K_I_4X4, 1 },
		{ K_P, K_P_SKIP, 1 },
		{ K_B_SKIP, K_B, 1 },
		{ K_P, K_I_16X16, 2 },
		{ K_B_DIRECT, K_I_PCM, 2 }
	};
	unsigned slice_types[2];
	unsigned offsets[2];
	unsigned index;
	unsigned slice_index;
	unsigned value;
	unsigned got;
	unsigned repeat;

	/* P slices use ctxIdxOffset 11 and B slices 24. */
	slice_types[0] = H264SW_SLICE_P;
	slice_types[1] = H264SW_SLICE_B;
	offsets[0] = 11U;
	offsets[1] = 24U;

	/* Each neighbour setup, with both flag values, in a P and in a B slice. */
	for (slice_index = 0; slice_index < 2U; slice_index++) {
		for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
			env_make(slice_types[slice_index], cases[index].a_kind, cases[index].b_kind);
			stream_begin();
			for (repeat = 0; repeat < 6U; repeat++)
				put(offsets[slice_index] + cases[index].inc, (repeat * 5U + 1U) % 3U == 0U ? 1U : 0U);
			stream_end(1);
			for (repeat = 0; repeat < 6U; repeat++) {
				value = (repeat * 5U + 1U) % 3U == 0U ? 1U : 0U;
				got = h264sw_cabac_mb_skip(&ctx);
				CHECK(got == value);
			}
			stream_check(1);
		}
	}

	/* An I slice has no mb_skip_flag: the call marks the error and returns 0 without reading. */
	env_make(H264SW_SLICE_I, NONE, NONE);
	stream_begin();
	put(11U, 1U);
	stream_end(1);
	got = h264sw_cabac_mb_skip(&ctx);
	CHECK(got == 0U);
	CHECK(ctx.cabac.error != 0);
}

/* Checks mb_type of I slices: all 26 values under each neighbour setup. */
static void
check_mb_type_i(
	void)
{
	static const struct neighbour_case cases[] = {
		/* No neighbour: 0. */
		{ NONE, NONE, 0 },
		/* I_NxN neighbours do not count, in any form. */
		{ K_I_NXN, NONE, 0 },
		{ K_I_4X4, K_I_8X8, 0 },
		{ NONE, K_I_8X8, 0 },
		/* Any other available neighbour counts. */
		{ K_I_16X16, NONE, 1 },
		{ NONE, K_I_16X16, 1 },
		{ K_I_4X4, K_I_16X16, 1 },
		{ K_I_PCM, K_I_16X16, 2 },
		{ K_I_16X16, K_I_PCM, 2 },
		{ K_I_PCM, K_I_NXN, 1 }
	};
	unsigned index;
	unsigned value;
	unsigned got;
	int status;

	/* Every neighbour setup, with each value that is not I_PCM in one stream. */
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		env_make(H264SW_SLICE_I, cases[index].a_kind, cases[index].b_kind);
		stream_begin();
		for (value = 0; value < 25U; value++)
			put_mb_type(H264SW_SLICE_I, value, cases[index].inc);
		stream_end(1);
		for (value = 0; value < 25U; value++) {
			got = 99;
			status = h264sw_cabac_mb_type(&ctx, H264SW_SLICE_I, &got);
			CHECK(status == 0);
			CHECK(got == value);
		}
		stream_check(1);

		/* I_PCM uses the terminate bin, after a value that is not I_PCM. */
		env_make(H264SW_SLICE_I, cases[index].a_kind, cases[index].b_kind);
		stream_begin();
		put_mb_type(H264SW_SLICE_I, 12U, cases[index].inc);
		put_mb_type(H264SW_SLICE_I, 25U, cases[index].inc);
		stream_end(0);
		got = 99;
		status = h264sw_cabac_mb_type(&ctx, H264SW_SLICE_I, &got);
		CHECK(status == 0 && got == 12U);
		status = h264sw_cabac_mb_type(&ctx, H264SW_SLICE_I, &got);
		CHECK(status == 0 && got == 25U);
		stream_check(0);
	}
}

/* Checks mb_type of P slices: the four inter types and the intra values 5 to 30, which no neighbour affects. */
static void
check_mb_type_p(
	void)
{
	unsigned value;
	unsigned got;
	unsigned setup;
	int status;

	/* Three neighbour setups, to show that the first bin does not depend on them. */
	for (setup = 0; setup < 3U; setup++) {
		env_make(H264SW_SLICE_P, setup == 0U ? NONE : K_I_16X16, setup == 2U ? K_P_SKIP : NONE);
		stream_begin();
		for (value = 0; value < 30U; value++) {
			if (value != 4U)
				put_mb_type(H264SW_SLICE_P, value, 0U);
		}
		stream_end(1);
		for (value = 0; value < 30U; value++) {
			if (value == 4U)
				continue;
			got = 99;
			status = h264sw_cabac_mb_type(&ctx, H264SW_SLICE_P, &got);
			CHECK(status == 0);
			CHECK(got == value);
		}
		stream_check(1);
	}

	/* The I_PCM suffix (mb_type 30) ends the arithmetic code with the terminate bin. */
	env_make(H264SW_SLICE_P, NONE, NONE);
	stream_begin();
	put_mb_type(H264SW_SLICE_P, 3U, 0U);
	put_mb_type(H264SW_SLICE_P, 30U, 0U);
	stream_end(0);
	status = h264sw_cabac_mb_type(&ctx, H264SW_SLICE_P, &got);
	CHECK(status == 0 && got == 3U);
	status = h264sw_cabac_mb_type(&ctx, H264SW_SLICE_P, &got);
	CHECK(status == 0 && got == 30U);
	stream_check(0);
}

/* Checks mb_type of B slices: the values 0 to 48 under each neighbour setup. */
static void
check_mb_type_b(
	void)
{
	static const struct neighbour_case cases[] = {
		/* No neighbour: 0. */
		{ NONE, NONE, 0 },
		/* B_Skip and B_Direct_16x16 neighbours do not count. */
		{ K_B_SKIP, NONE, 0 },
		{ K_B_DIRECT, K_B_SKIP, 0 },
		{ NONE, K_B_DIRECT, 0 },
		/* Any other available neighbour counts, even an intra one. */
		{ K_B, NONE, 1 },
		{ NONE, K_B_8X8, 1 },
		{ K_I_4X4, K_B_DIRECT, 1 },
		{ K_B_SKIP, K_I_16X16, 1 },
		{ K_B_8X8, K_B, 2 },
		{ K_I_PCM, K_I_NXN, 2 }
	};
	unsigned index;
	unsigned value;
	unsigned got;
	int status;

	/* Every neighbour setup, with each value that is not I_PCM in one stream. */
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		env_make(H264SW_SLICE_B, cases[index].a_kind, cases[index].b_kind);
		stream_begin();
		for (value = 0; value < 48U; value++)
			put_mb_type(H264SW_SLICE_B, value, cases[index].inc);
		stream_end(1);
		for (value = 0; value < 48U; value++) {
			got = 99;
			status = h264sw_cabac_mb_type(&ctx, H264SW_SLICE_B, &got);
			CHECK(status == 0);
			CHECK(got == value);
		}
		stream_check(1);

		/* I_PCM (mb_type 48) uses the terminate bin. */
		env_make(H264SW_SLICE_B, cases[index].a_kind, cases[index].b_kind);
		stream_begin();
		put_mb_type(H264SW_SLICE_B, 22U, cases[index].inc);
		put_mb_type(H264SW_SLICE_B, 48U, cases[index].inc);
		stream_end(0);
		status = h264sw_cabac_mb_type(&ctx, H264SW_SLICE_B, &got);
		CHECK(status == 0 && got == 22U);
		status = h264sw_cabac_mb_type(&ctx, H264SW_SLICE_B, &got);
		CHECK(status == 0 && got == 48U);
		stream_check(0);
	}
}

/* Checks that mb_type and sub_mb_type refuse a slice type that has none, without reading. */
static void
check_mb_type_errors(
	void)
{
	unsigned got;
	int status;

	/* SP, SI and a value that is no slice type are refused, and the answer is cleared. */
	env_make(H264SW_SLICE_P, NONE, NONE);
	stream_begin();
	put(14U, 1U);
	stream_end(1);
	got = 99;
	status = h264sw_cabac_mb_type(&ctx, H264SW_SLICE_SP, &got);
	CHECK(status == EINVAL && got == 0U);
	got = 99;
	status = h264sw_cabac_mb_type(&ctx, H264SW_SLICE_SI, &got);
	CHECK(status == EINVAL && got == 0U);
	got = 99;
	status = h264sw_cabac_mb_type(&ctx, 9U, &got);
	CHECK(status == EINVAL && got == 0U);

	/* An I slice has no sub_mb_type. */
	got = 99;
	status = h264sw_cabac_sub_mb_type(&ctx, H264SW_SLICE_I, &got);
	CHECK(status == EINVAL && got == 0U);

	/* Nothing was read: the first bin is still there. */
	CHECK(ctx.cabac.error == 0);
	CHECK(memcmp(ctx.cabac.state, initial_state, sizeof(initial_state)) == 0);
}

/* Checks sub_mb_type: every value of P (0 to 3) and B (0 to 12) slices. */
static void
check_sub_mb_type(
	void)
{
	unsigned value;
	unsigned got;
	unsigned round;
	int status;

	/* The values of P slices, in several orders so each context is in several states. */
	env_make(H264SW_SLICE_P, NONE, NONE);
	stream_begin();
	for (round = 0; round < 3U; round++) {
		for (value = 0; value < 4U; value++)
			put_string(bins_sub_p[(value + round) % 4U], 21U, 0U);
	}
	stream_end(1);
	for (round = 0; round < 3U; round++) {
		for (value = 0; value < 4U; value++) {
			got = 99;
			status = h264sw_cabac_sub_mb_type(&ctx, H264SW_SLICE_P, &got);
			CHECK(status == 0);
			CHECK(got == (value + round) % 4U);
		}
	}
	stream_check(1);

	/* The values of B slices. */
	env_make(H264SW_SLICE_B, NONE, NONE);
	stream_begin();
	for (round = 0; round < 3U; round++) {
		for (value = 0; value < 13U; value++)
			put_string(bins_sub_b[(value + 5U * round) % 13U], 36U, 0U);
	}
	stream_end(1);
	for (round = 0; round < 3U; round++) {
		for (value = 0; value < 13U; value++) {
			got = 99;
			status = h264sw_cabac_sub_mb_type(&ctx, H264SW_SLICE_B, &got);
			CHECK(status == 0);
			CHECK(got == (value + 5U * round) % 13U);
		}
	}
	stream_check(1);
}

/* Checks transform_size_8x8_flag with the neighbours' flags. */
static void
check_transform_8x8(
	void)
{
	unsigned a_flag;
	unsigned b_flag;
	unsigned a_avail;
	unsigned b_avail;
	unsigned expected_inc;
	unsigned repeat;
	unsigned got;

	/* Every combination of availability and flag: the context counts the neighbours that are available and have the flag. */
	for (a_avail = 0; a_avail < 2U; a_avail++) {
		for (b_avail = 0; b_avail < 2U; b_avail++) {
			for (a_flag = 0; a_flag < 2U; a_flag++) {
				for (b_flag = 0; b_flag < 2U; b_flag++) {
					env_make(H264SW_SLICE_I, a_avail ? K_I_4X4 : NONE, b_avail ? K_I_8X8 : NONE);
					mbs[ADDR_A].transform_8x8 = (uint8_t)a_flag;
					mbs[ADDR_B].transform_8x8 = (uint8_t)b_flag;

					/* The unavailable neighbour's entry keeps the flag but must not count. */
					expected_inc = (a_avail && a_flag ? 1U : 0U) + (b_avail && b_flag ? 1U : 0U);
					stream_begin();
					for (repeat = 0; repeat < 5U; repeat++)
						put(399U + expected_inc, repeat & 1U);
					stream_end(1);
					for (repeat = 0; repeat < 5U; repeat++) {
						got = h264sw_cabac_transform_8x8(&ctx);
						CHECK(got == (repeat & 1U));
					}
					stream_check(1);
				}
			}
		}
	}
}

/* Checks prev_intra_pred_mode_flag and rem_intra_pred_mode, every remaining mode. */
static void
check_intra_mode(
	void)
{
	unsigned rem;
	unsigned flag;
	unsigned got_flag;
	unsigned got_rem;
	unsigned round;

	/* The flag 1 alone, and the flag 0 with each of the eight remaining modes (three bins, least significant first). */
	env_make(H264SW_SLICE_I, NONE, NONE);
	stream_begin();
	for (round = 0; round < 3U; round++) {
		put(68U, 1U);
		for (rem = 0; rem < 8U; rem++) {
			put(68U, 0U);
			put(69U, rem & 1U);
			put(69U, (rem >> 1) & 1U);
			put(69U, (rem >> 2) & 1U);
		}
	}
	stream_end(1);
	for (round = 0; round < 3U; round++) {
		got_flag = 9;
		got_rem = 9;
		h264sw_cabac_intra_mode(&ctx, &got_flag, &got_rem);
		CHECK(got_flag == 1U && got_rem == 0U);
		for (rem = 0; rem < 8U; rem++) {
			flag = 9;
			got_rem = 9;
			h264sw_cabac_intra_mode(&ctx, &flag, &got_rem);
			CHECK(flag == 0U && got_rem == rem);
		}
	}
	stream_check(1);
}

/* Checks intra_chroma_pred_mode: the four values under each neighbour setup. */
static void
check_chroma_mode(
	void)
{
	struct chroma_case {
		unsigned a_kind;
		unsigned a_mode;
		unsigned b_kind;
		unsigned b_mode;
		unsigned inc;
	};
	static const struct chroma_case cases[] = {
		/* Nothing available: 0. */
		{ NONE, 0, NONE, 0, 0 },
		/* An intra neighbour with a chroma mode other than 0 counts. */
		{ K_I_4X4, 1, NONE, 0, 1 },
		{ NONE, 0, K_I_16X16, 3, 1 },
		{ K_I_8X8, 2, K_I_4X4, 1, 2 },
		/* A neighbour with chroma mode 0 does not count. */
		{ K_I_4X4, 0, K_I_16X16, 2, 1 },
		{ K_I_4X4, 0, K_I_16X16, 0, 0 },
		/* An I_PCM neighbour does not count, whatever its entry holds. */
		{ K_I_PCM, 2, K_I_16X16, 1, 1 },
		{ K_I_16X16, 1, K_I_PCM, 3, 1 },
		/* An inter neighbour does not count, whatever its entry holds. */
		{ K_P, 2, K_I_4X4, 2, 1 },
		{ K_I_4X4, 3, K_B_SKIP, 1, 1 },
		{ K_P_8X8, 1, K_B, 1, 0 }
	};
	unsigned index;
	unsigned value;
	unsigned got;

	/* Each neighbour setup, with the four values twice. */
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		env_make(H264SW_SLICE_I, cases[index].a_kind, cases[index].b_kind);
		mbs[ADDR_A].chroma_pred_mode = (uint8_t)cases[index].a_mode;
		mbs[ADDR_B].chroma_pred_mode = (uint8_t)cases[index].b_mode;
		stream_begin();
		for (value = 0; value < 8U; value++) {
			/* TU with cMax 3: bin 0 with the neighbours' context, the others with ctxIdx 67. */
			put(64U + cases[index].inc, (value % 4U) > 0U ? 1U : 0U);
			if ((value % 4U) > 0U)
				put(67U, (value % 4U) > 1U ? 1U : 0U);
			if ((value % 4U) > 1U)
				put(67U, (value % 4U) > 2U ? 1U : 0U);
		}
		stream_end(1);
		for (value = 0; value < 8U; value++) {
			got = h264sw_cabac_chroma_mode(&ctx);
			CHECK(got == value % 4U);
		}
		stream_check(1);
	}
}

/* Checks ref_idx: the values up to the limit, the contexts from the neighbouring partitions, and the values above the limit. */
static void
check_ref_idx(
	void)
{
	struct ref_case {
		unsigned x;
		unsigned y;
		unsigned list;
		unsigned a_kind;
		int a_ref;
		unsigned a_direct;
		unsigned b_kind;
		int b_ref;
		unsigned b_direct;
		unsigned inc;
	};
	/*
	 * The partition at (0, 0) has A in the left macroblock at (15, 0),
	 * which is in quadrant 1, and B in the macroblock above at (0, 15),
	 * which is in quadrant 2.  The refIdx of the entry is the reference
	 * index of that quadrant for the list.
	 */
	static const struct ref_case cases[] = {
		/* Nothing available: 0. */
		{ 0, 0, 0, NONE, 0, 0, NONE, 0, 0, 0 },
		/* A reference index above 0 counts: A is 1, B is 2. */
		{ 0, 0, 0, K_P, 1, 0, NONE, 0, 0, 1 },
		{ 0, 0, 0, NONE, 0, 0, K_P, 1, 0, 2 },
		{ 0, 0, 0, K_P, 3, 0, K_B, 2, 0, 3 },
		/* Reference index 0 and a list that is not used (-1) do not count. */
		{ 0, 0, 0, K_P, 0, 0, K_P, 0, 0, 0 },
		{ 0, 0, 0, K_B, -1, 0, K_B, -1, 0, 0 },
		{ 0, 0, 0, K_B, -1, 0, K_B, 2, 0, 2 },
		/* A skipped neighbour, an intra neighbour and a direct-predicted quadrant do not count. */
		{ 0, 0, 0, K_P_SKIP, 2, 0, K_P, 2, 0, 2 },
		{ 0, 0, 0, K_B_SKIP, 2, 0, K_B_SKIP, 2, 0, 0 },
		{ 0, 0, 0, K_I_4X4, 2, 0, K_I_16X16, 2, 0, 0 },
		{ 0, 0, 0, K_I_PCM, 2, 0, K_P, 1, 0, 2 },
		{ 0, 0, 0, K_B_DIRECT, 2, 0xf, K_B, 2, 0, 2 },
		{ 0, 0, 0, K_B_8X8, 2, 0x2, K_B_8X8, 2, 0x4, 0 },
		{ 0, 0, 0, K_B_8X8, 2, 0x1, K_B_8X8, 2, 0x1, 3 },
		/* The same partitions in list 1. */
		{ 0, 0, 1, K_B, 1, 0, K_B, 1, 0, 3 },
		{ 0, 0, 1, K_P, 0, 0, K_B, 1, 0, 2 },
		/* The partition at (8, 8): A is at (7, 8) and B at (8, 7) in the current macroblock, quadrants 2 and 1, whose reference indices the case gives. */
		{ 8, 8, 0, K_P, 1, 0, K_P, 1, 0, 3 }
	};
	unsigned index;
	unsigned value;
	unsigned active;
	unsigned got;
	int status;
	int quadrant_a;
	int quadrant_b;

	/* Each setup with every value up to the limit of 4 reference indices. */
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		env_make(H264SW_SLICE_B, cases[index].a_kind, cases[index].b_kind);

		/* The neighbours' reference indices: for the partition at (0, 0) in the left and the upper macroblock, at (8, 8) in the current one. */
		if (cases[index].x == 0U) {
			quadrant_a = 1;
			quadrant_b = 2;
			if (cases[index].a_kind != NONE) {
				mbs[ADDR_A].motion.ref_idx[cases[index].list][quadrant_a] = (int8_t)cases[index].a_ref;
				mbs[ADDR_A].direct = (uint8_t)cases[index].a_direct;
			}
			if (cases[index].b_kind != NONE) {
				mbs[ADDR_B].motion.ref_idx[cases[index].list][quadrant_b] = (int8_t)cases[index].b_ref;
				mbs[ADDR_B].direct = (uint8_t)cases[index].b_direct;
			}
		} else {
			/* The current macroblock is an inter one with its quadrants 2 and 1 decoded before quadrant 3. */
			mbs[ADDR_CUR].kind = K_B_8X8;
			mbs[ADDR_CUR].motion.ref_idx[cases[index].list][2] = (int8_t)cases[index].a_ref;
			mbs[ADDR_CUR].motion.ref_idx[cases[index].list][1] = (int8_t)cases[index].b_ref;
		}

		stream_begin();
		for (value = 0; value < 4U; value++)
			put_unary(value, 54U + cases[index].inc, 58U, 59U);
		stream_end(1);
		for (value = 0; value < 4U; value++) {
			got = 99;
			status = h264sw_cabac_ref_idx(&ctx, cases[index].list, cases[index].x, cases[index].y, &got);
			CHECK(status == 0);
			CHECK(got == value);
		}
		stream_check(1);
	}

	/* The limit: values up to num_ref_idx_active - 1 are read, for several limits including 32. */
	for (active = 2; active <= 32U; active = active < 4U ? active + 1U : active * 2U) {
		env_make(H264SW_SLICE_P, NONE, NONE);
		header.num_ref_idx_active[0] = active;
		stream_begin();
		for (value = 0; value < active; value++)
			put_unary(value, 54U, 58U, 59U);
		stream_end(1);
		for (value = 0; value < active; value++) {
			got = 99;
			status = h264sw_cabac_ref_idx(&ctx, 0U, 0U, 0U, &got);
			CHECK(status == 0);
			CHECK(got == value);
		}
		stream_check(1);

		/* A value equal to the limit is refused as soon as its last 1 is read. */
		env_make(H264SW_SLICE_P, NONE, NONE);
		header.num_ref_idx_active[0] = active;
		stream_begin();
		put_unary(active, 54U, 58U, 59U);
		stream_end(1);
		got = 99;
		status = h264sw_cabac_ref_idx(&ctx, 0U, 0U, 0U, &got);
		CHECK(status == EINVAL);
		CHECK(got == 0U);
	}

	/* A long run of 1 bins ends at the limit and does not loop on. */
	env_make(H264SW_SLICE_P, NONE, NONE);
	header.num_ref_idx_active[0] = 32U;
	stream_begin();
	for (value = 0; value < 200U; value++)
		put(value == 0U ? 54U : (value == 1U ? 58U : 59U), 1U);
	stream_end(1);
	status = h264sw_cabac_ref_idx(&ctx, 0U, 0U, 0U, &got);
	CHECK(status == EINVAL);

	/* Arguments that no partition has: list 2, a location outside the macroblock, no reference index in the list. */
	env_make(H264SW_SLICE_P, NONE, NONE);
	stream_begin();
	put(54U, 0U);
	stream_end(1);
	status = h264sw_cabac_ref_idx(&ctx, 2U, 0U, 0U, &got);
	CHECK(status == EINVAL);
	status = h264sw_cabac_ref_idx(&ctx, 0U, 16U, 0U, &got);
	CHECK(status == EINVAL);
	status = h264sw_cabac_ref_idx(&ctx, 0U, 0U, 16U, &got);
	CHECK(status == EINVAL);
	header.num_ref_idx_active[1] = 0U;
	status = h264sw_cabac_ref_idx(&ctx, 1U, 0U, 0U, &got);
	CHECK(status == EINVAL);

	/* Those refusals read nothing: the first bin is still there. */
	status = h264sw_cabac_ref_idx(&ctx, 0U, 0U, 0U, &got);
	CHECK(status == 0 && got == 0U);
	stream_check(1);
}

/* Checks mvd: values from 0 to the ends of the range under each neighbour setup, both components, and the values and codes out of range. */
static void
check_mvd(
	void)
{
	struct mvd_case {
		unsigned a_kind;
		unsigned a_mvd;
		unsigned b_kind;
		unsigned b_mvd;
		unsigned inc;
	};
	/*
	 * The partition at (0, 0) has A in the left macroblock at (15, 0),
	 * the 4x4 block 5, and B in the macroblock above at (0, 15), the 4x4
	 * block 10.  absMvdCompA + absMvdCompB selects the increment: 0 to 2
	 * is 0, 3 to 32 is 1, above 32 is 2.
	 */
	static const struct mvd_case cases[] = {
		/* Nothing available: 0. */
		{ NONE, 0, NONE, 0, 0 },
		/* The sums at the edges of the three ranges. */
		{ K_P, 2, NONE, 0, 0 },
		{ K_P, 1, K_P, 1, 0 },
		{ K_P, 2, K_P, 1, 1 },
		{ NONE, 0, K_B, 3, 1 },
		{ K_P, 16, K_P, 16, 1 },
		{ K_P, 16, K_P, 17, 2 },
		{ K_P, 33, NONE, 0, 2 },
		{ NONE, 0, K_B, 255, 2 },
		{ K_B, 255, K_B, 255, 2 },
		/* A skipped neighbour and an intra neighbour count as 0, whatever the entry holds. */
		{ K_P_SKIP, 200, K_P, 1, 0 },
		{ K_I_4X4, 200, K_B_SKIP, 200, 0 },
		{ K_I_PCM, 200, K_P, 4, 1 },
		/* A direct-predicted neighbour counts as 0. */
		{ K_B_DIRECT, 200, K_B, 5, 1 }
	};
	static const int values[] = {
		0, 1, -1, 2, -2, 3, -3, 7, -7, 8, -8, 9, -9, 10, -10, 11, 12, -13, 20, -21,
		31, 32, -33, 64, -100, 100, 127, 128, -255, 256, -1000, 4096, 8191, -8191, 8192, -8192,
		16383, 32766, 32767, -32767, -32768
	};
	unsigned index;
	unsigned value_index;
	unsigned component;
	unsigned list;
	unsigned offset;
	int got;
	int status;

	/* Each neighbour setup in both components and both lists, with every value of the list. */
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		for (component = 0; component < 2U; component++) {
			for (list = 0; list < 2U; list++) {
				env_make(H264SW_SLICE_B, cases[index].a_kind, cases[index].b_kind);

				/* Puts the neighbours' mvd in the entries for this list and component only; the other component and list stay 0. */
				mbs[ADDR_A].mvd[list][5][component] = (uint8_t)cases[index].a_mvd;
				mbs[ADDR_B].mvd[list][10][component] = (uint8_t)cases[index].b_mvd;
				if (cases[index].a_kind == K_B_DIRECT)
					mbs[ADDR_A].direct = 0xf;

				offset = component == 0U ? 40U : 47U;
				stream_begin();
				for (value_index = 0; value_index < sizeof(values) / sizeof(values[0]); value_index++)
					put_mvd(offset, cases[index].inc, values[value_index]);
				stream_end(1);
				for (value_index = 0; value_index < sizeof(values) / sizeof(values[0]); value_index++) {
					got = 12345;
					status = h264sw_cabac_mvd(&ctx, list, 0U, 0U, component, &got);
					CHECK(status == 0);
					CHECK(got == values[value_index]);
				}
				stream_check(1);
			}
		}
	}

	/* The mvd of the other component or of the other list in a neighbour does not matter: each read sees only its own list and component. */
	env_make(H264SW_SLICE_B, K_P, K_P);
	mbs[ADDR_A].mvd[0][5][1] = 100;
	mbs[ADDR_A].mvd[1][5][0] = 100;
	mbs[ADDR_B].mvd[1][10][1] = 100;
	stream_begin();
	put_mvd(40U, 0U, 5);
	put_mvd(47U, 2U, -5);
	put_mvd(40U, 2U, 6);
	put_mvd(47U, 2U, -6);
	stream_end(1);
	status = h264sw_cabac_mvd(&ctx, 0U, 0U, 0U, 0U, &got);
	CHECK(status == 0 && got == 5);
	status = h264sw_cabac_mvd(&ctx, 0U, 0U, 0U, 1U, &got);
	CHECK(status == 0 && got == -5);
	status = h264sw_cabac_mvd(&ctx, 1U, 0U, 0U, 0U, &got);
	CHECK(status == 0 && got == 6);
	status = h264sw_cabac_mvd(&ctx, 1U, 0U, 0U, 1U, &got);
	CHECK(status == 0 && got == -6);
	stream_check(1);

	/* A partition inside the current macroblock reads the earlier blocks of the same macroblock: at (8, 4) A is at (7, 4), the 4x4 block 3 (mvd 10), and B is at (8, 3), the block 4 (mvd 25); 35 is above 32, so the increment is 2. */
	env_make(H264SW_SLICE_B, NONE, NONE);
	mbs[ADDR_CUR].kind = K_B_8X8;
	mbs[ADDR_CUR].mvd[0][3][0] = 10;
	mbs[ADDR_CUR].mvd[0][4][0] = 25;
	stream_begin();
	put_mvd(40U, 2U, 17);
	stream_end(1);
	got = 0;
	status = h264sw_cabac_mvd(&ctx, 0U, 8U, 4U, 0U, &got);
	CHECK(status == 0 && got == 17);
	stream_check(1);

	/* One past each end of the range is refused (+32768 and -32769), and so is a magnitude far above it. */
	env_make(H264SW_SLICE_B, NONE, NONE);
	stream_begin();
	put_mvd(40U, 0U, 32767);
	stream_end(1);
	got = 12345;
	status = h264sw_cabac_mvd(&ctx, 0U, 0U, 0U, 0U, &got);
	CHECK(status == 0 && got == 32767);
	stream_check(1);

	/* The values above are put by hand: 32768 and 32769 as magnitudes with each sign. */
	for (index = 0; index < 6U; index++) {
		env_make(H264SW_SLICE_B, NONE, NONE);
		stream_begin();
		put(40U, 1U);
		put(43U, 1U);
		put(44U, 1U);
		put(45U, 1U);
		put(46U, 1U);
		put(46U, 1U);
		put(46U, 1U);
		put(46U, 1U);
		put(46U, 1U);
		if (index < 2U)
			put_exp_golomb(32768U - 9U, 3U);
		else if (index < 4U)
			put_exp_golomb(32769U - 9U, 3U);
		else
			put_exp_golomb(1000000U - 9U, 3U);
		put_bypass(index & 1U);
		stream_end(1);
		got = 12345;
		status = h264sw_cabac_mvd(&ctx, 0U, 0U, 0U, 0U, &got);

		/* Only -32768 is in range among them (index 1). */
		if (index == 1U) {
			CHECK(status == 0 && got == -32768);
		} else {
			CHECK(status == EINVAL);
			CHECK(got == 0);
		}
	}

	/* An Exp-Golomb prefix of more than 24 bins of 1 is refused, whatever follows. */
	env_make(H264SW_SLICE_B, NONE, NONE);
	stream_begin();
	put(40U, 1U);
	put(43U, 1U);
	put(44U, 1U);
	put(45U, 1U);
	put(46U, 1U);
	put(46U, 1U);
	put(46U, 1U);
	put(46U, 1U);
	put(46U, 1U);
	for (index = 0; index < 40U; index++)
		put_bypass(1U);
	stream_end(1);
	got = 12345;
	status = h264sw_cabac_mvd(&ctx, 0U, 0U, 0U, 0U, &got);
	CHECK(status == EINVAL);
	CHECK(got == 0);

	/* Exactly 24 bins of 1 are not refused by the length bound (the value is out of range instead, which is also EINVAL, but with the right number of bins read). */
	env_make(H264SW_SLICE_B, NONE, NONE);
	stream_begin();
	put(40U, 1U);
	put(43U, 1U);
	put(44U, 1U);
	put(45U, 1U);
	put(46U, 1U);
	put(46U, 1U);
	put(46U, 1U);
	put(46U, 1U);
	put(46U, 1U);
	put_exp_golomb(8U * ((1U << 24) - 1U) + 12345U, 3U);
	put_bypass(0U);
	stream_end(1);
	got = 12345;
	status = h264sw_cabac_mvd(&ctx, 0U, 0U, 0U, 0U, &got);
	CHECK(status == EINVAL);
	CHECK(ctx.cabac.error == 0);
	CHECK(h264sw_cabac_end_of_slice(&ctx) == 1U);

	/* Arguments that no partition has: list 2, component 2, a location outside the macroblock. */
	env_make(H264SW_SLICE_B, NONE, NONE);
	stream_begin();
	put(40U, 0U);
	stream_end(1);
	status = h264sw_cabac_mvd(&ctx, 2U, 0U, 0U, 0U, &got);
	CHECK(status == EINVAL && got == 0);
	status = h264sw_cabac_mvd(&ctx, 0U, 0U, 0U, 2U, &got);
	CHECK(status == EINVAL && got == 0);
	status = h264sw_cabac_mvd(&ctx, 0U, 16U, 0U, 0U, &got);
	CHECK(status == EINVAL && got == 0);
	status = h264sw_cabac_mvd(&ctx, 0U, 0U, 16U, 0U, &got);
	CHECK(status == EINVAL && got == 0);

	/* They read nothing. */
	status = h264sw_cabac_mvd(&ctx, 0U, 0U, 0U, 0U, &got);
	CHECK(status == 0 && got == 0);
	stream_check(1);
}

/* Gives condTermFlagN of the luma part of coded_block_pattern for a macroblock other than the current one, as 9.3.3.1.1.4 words it. */
static unsigned
ref_cond_luma(
	const struct h264sw_mb *mb_n,
	unsigned bit)
{
	/* Not available: 0.  I_PCM: 0. */
	if (mb_n == NULL)
		return 0;
	if (mb_n->kind == K_I_PCM)
		return 0;

	/* A skipped macroblock has no coded bit set, so the term is 1. */
	if (mb_n->kind == K_P_SKIP || mb_n->kind == K_B_SKIP)
		return 1;

	/* A set bit makes the term 0, a clear bit 1. */
	if (((mb_n->cbp >> bit) & 1U) != 0U)
		return 0;
	return 1;
}

/* Gives condTermFlagN of the chroma part of coded_block_pattern for bin bin_idx (0 or 1), as 9.3.3.1.1.4 words it. */
static unsigned
ref_cond_chroma(
	const struct h264sw_mb *mb_n,
	unsigned bin_idx)
{
	unsigned chroma;

	/* Not available: 0.  I_PCM: 1.  Skipped: 0. */
	if (mb_n == NULL)
		return 0;
	if (mb_n->kind == K_I_PCM)
		return 1;
	if (mb_n->kind == K_P_SKIP || mb_n->kind == K_B_SKIP)
		return 0;

	/* Bin 0 asks for any chroma residual, bin 1 for AC. */
	chroma = (mb_n->cbp >> 4) & 3U;
	if (bin_idx == 0U)
		return chroma != 0U ? 1U : 0U;
	return chroma == 2U ? 1U : 0U;
}

/* Checks coded_block_pattern: all 48 values under neighbour setups that change the contexts, and contexts worked out by hand. */
static void
check_cbp(
	void)
{
	struct cbp_state {
		unsigned kind;
		unsigned cbp;
	};
	static const struct cbp_state states[] = {
		/* Not available. */
		{ NONE, 0 },
		/* Coded neighbours with different luma bits and chroma values. */
		{ K_P, 0x00 },
		{ K_P, 0x2f },
		{ K_P, 0x1a },
		{ K_B, 0x25 },
		{ K_I_16X16, 0x0f },
		{ K_I_16X16, 0x10 },
		{ K_I_4X4, 0x0a },
		/* I_PCM counts as all coded for the chroma and as a set bit for the luma; a skipped macroblock is the reverse. */
		{ K_I_PCM, 0x2f },
		{ K_P_SKIP, 0x00 },
		{ K_B_SKIP, 0x3f }
	};
	unsigned a_state;
	unsigned b_state;
	unsigned value;
	unsigned luma;
	unsigned chroma;
	unsigned blk;
	unsigned inc;
	unsigned cond_a;
	unsigned cond_b;
	unsigned got;
	unsigned bins[4];

	/* Every pair of neighbour states with all 48 values in one stream; the encoder side works out each context from the neighbours' entries with the layout of the four 8x8 blocks written out by hand. */
	for (a_state = 0; a_state < sizeof(states) / sizeof(states[0]); a_state++) {
		for (b_state = 0; b_state < sizeof(states) / sizeof(states[0]); b_state++) {
			env_make(H264SW_SLICE_I, states[a_state].kind, states[b_state].kind);
			mbs[ADDR_A].cbp = (uint8_t)states[a_state].cbp;
			mbs[ADDR_B].cbp = (uint8_t)states[b_state].cbp;
			stream_begin();
			for (value = 0; value < 48U; value++) {
				luma = value % 16U;
				chroma = value / 16U;

				/* The four luma bins, bin k for 8x8 block k.  Block 0 has A in block 1 of the left macroblock and B in block 2 of the upper one, block 1 has A in block 0 here and B in block 3 above, block 2 has A in block 3 on the left and B in block 0 here, block 3 has A and B in blocks 2 and 1 here. */
				for (blk = 0; blk < 4U; blk++)
					bins[blk] = (luma >> blk) & 1U;
				for (blk = 0; blk < 4U; blk++) {
					if (blk == 0U) {
						cond_a = ref_cond_luma(ctx.n.a, 1U);
						cond_b = ref_cond_luma(ctx.n.b, 2U);
					} else if (blk == 1U) {
						cond_a = bins[0] != 0U ? 0U : 1U;
						cond_b = ref_cond_luma(ctx.n.b, 3U);
					} else if (blk == 2U) {
						cond_a = ref_cond_luma(ctx.n.a, 3U);
						cond_b = bins[0] != 0U ? 0U : 1U;
					} else {
						cond_a = bins[2] != 0U ? 0U : 1U;
						cond_b = bins[1] != 0U ? 0U : 1U;
					}
					put(73U + cond_a + 2U * cond_b, bins[blk]);
				}

				/* The chroma bins: TU with cMax 2, the contexts from the macroblocks A and B. */
				inc = ref_cond_chroma(ctx.n.a, 0U) + 2U * ref_cond_chroma(ctx.n.b, 0U);
				put(77U + inc, chroma > 0U ? 1U : 0U);
				if (chroma > 0U) {
					inc = 4U + ref_cond_chroma(ctx.n.a, 1U) + 2U * ref_cond_chroma(ctx.n.b, 1U);
					put(77U + inc, chroma > 1U ? 1U : 0U);
				}
			}
			stream_end(1);
			for (value = 0; value < 48U; value++) {
				got = h264sw_cabac_cbp(&ctx);
				CHECK(got == ((value / 16U) << 4 | (value % 16U)));
			}
			stream_check(1);
		}
	}

	/*
	 * Contexts worked out by hand.  The left macroblock has cbp luma 0b0010
	 * (8x8 block 1 coded) and the upper one 0b1000 (block 3 coded), both
	 * without chroma.  Value 0: bin 0 has A in block 1 of the left (set,
	 * 0) and B in block 2 of the upper (clear, 1): 73 + 0 + 2 = 75; bin 1
	 * has A in block 0 here (bin 0, clear, 1) and B in block 3 above (set,
	 * 0): 73 + 1 = 74; bin 2 has A in block 3 of the left (clear, 1) and B
	 * in block 0 here (clear, 1): 73 + 3 = 76; bin 3 has A in block 2 here
	 * and B in block 1 here (both clear): 76; chroma bin 0 has both
	 * neighbours at 0: 77.
	 */
	env_make(H264SW_SLICE_I, K_P, K_P);
	mbs[ADDR_A].cbp = 0x02;
	mbs[ADDR_B].cbp = 0x08;
	stream_begin();
	put(75U, 0U);
	put(74U, 0U);
	put(76U, 0U);
	put(76U, 0U);
	put(77U, 0U);
	stream_end(1);
	got = h264sw_cabac_cbp(&ctx);
	CHECK(got == 0U);
	stream_check(1);

	/*
	 * Value luma 15 and chroma 2 with the same neighbours: bin 0: 75 (as
	 * before); bin 1: A is bin 0 here (1, so 0), B is block 3 above (set,
	 * 0): 73; bin 2: A in block 3 of the left (clear, 1), B is bin 0 here
	 * (1, so 0): 74; bin 3: A is bin 2 here (1, 0), B is bin 1 here (1, 0):
	 * 73; chroma bin 0: 77, chroma bin 1: 77 + 4 = 81.
	 */
	env_make(H264SW_SLICE_I, K_P, K_P);
	mbs[ADDR_A].cbp = 0x02;
	mbs[ADDR_B].cbp = 0x08;
	stream_begin();
	put(75U, 1U);
	put(73U, 1U);
	put(74U, 1U);
	put(73U, 1U);
	put(77U, 1U);
	put(81U, 1U);
	stream_end(1);
	got = h264sw_cabac_cbp(&ctx);
	CHECK(got == (2U << 4 | 15U));
	stream_check(1);

	/*
	 * Chroma contexts by hand: the left macroblock has chroma 1 (0x10)
	 * and the upper one chroma 2 (0x20).  Bin 0: A has chroma (1), B has
	 * chroma (1): 77 + 1 + 2 = 80; bin 1: A is not 2 (0), B is 2 (1):
	 * 77 + 4 + 0 + 2 = 83.  The luma bins have A and B unset, so their
	 * terms are 1 where the bit is clear: bin 0 is 73 + 1 + 2 = 76, and
	 * with luma value 0 the others see clear bins: bin 1: A is bin 0 (1),
	 * B is block 3 above (clear, 1): 76; bin 2: A block 3 left (1), B bin
	 * 0 (1): 76; bin 3: 76.
	 */
	env_make(H264SW_SLICE_I, K_I_4X4, K_I_16X16);
	mbs[ADDR_A].cbp = 0x10;
	mbs[ADDR_B].cbp = 0x20;
	stream_begin();
	put(76U, 0U);
	put(76U, 0U);
	put(76U, 0U);
	put(76U, 0U);
	put(80U, 1U);
	put(83U, 0U);
	stream_end(1);
	got = h264sw_cabac_cbp(&ctx);
	CHECK(got == (1U << 4));
	stream_check(1);
}

/* Checks mb_qp_delta: every value of -26..25 with the context from the previous macroblock, and the values and codes out of range. */
static void
check_qp_delta(
	void)
{
	struct qp_case {
		int prev_addr;
		unsigned prev_kind;
		unsigned prev_cbp;
		int prev_nonzero;
		unsigned inc;
	};
	/*
	 * The previous macroblock counts (ctxIdxInc 1) only when it is
	 * Intra_16x16 or has some coded block pattern, is not skipped and
	 * not I_PCM, and its mb_qp_delta was not 0.
	 */
	static const struct qp_case cases[] = {
		/* No previous macroblock (the first of a slice): 0. */
		{ -1, 0, 0, 1, 0 },
		/* A skipped previous macroblock and an I_PCM one: 0. */
		{ 3, K_P_SKIP, 0, 1, 0 },
		{ 3, K_B_SKIP, 0, 1, 0 },
		{ 3, K_I_PCM, 0x2f, 1, 0 },
		/* A previous macroblock with no residual that is not Intra_16x16: 0, even if a delta is recorded. */
		{ 3, K_I_4X4, 0, 1, 0 },
		{ 3, K_P, 0, 1, 0 },
		/* Intra_16x16 without a coded pattern still has an mb_qp_delta. */
		{ 3, K_I_16X16, 0, 1, 1 },
		{ 3, K_I_16X16, 0, 0, 0 },
		/* A coded pattern of luma or of chroma only. */
		{ 3, K_P, 0x01, 1, 1 },
		{ 3, K_B, 0x10, 1, 1 },
		{ 3, K_I_8X8, 0x2f, 1, 1 },
		{ 3, K_I_8X8, 0x2f, 0, 0 },
		/* Another address in the picture. */
		{ 8, K_P, 0x05, 1, 1 }
	};
	unsigned index;
	int value;
	int got;
	int status;
	unsigned mapped;
	unsigned bin_idx;
	unsigned ctx_idx;

	/* Each setup with every value of the range in one stream. */
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		env_make(H264SW_SLICE_I, NONE, NONE);
		if (cases[index].prev_addr >= 0) {
			mbs[cases[index].prev_addr].kind = (uint8_t)cases[index].prev_kind;
			mbs[cases[index].prev_addr].cbp = (uint8_t)cases[index].prev_cbp;
		}
		ctx.prev_mb_addr = cases[index].prev_addr;
		ctx.prev_qp_delta_nonzero = cases[index].prev_nonzero;

		stream_begin();
		for (value = -26; value <= 25; value++) {
			/* Table 9-3 maps k to 2k - 1 for positive values and to -2k for the others, and U codes the mapped value. */
			mapped = value > 0 ? (unsigned)(2 * value - 1) : (unsigned)(-2 * value);
			put_unary(mapped, 60U + cases[index].inc, 62U, 63U);
		}
		stream_end(1);
		for (value = -26; value <= 25; value++) {
			got = 999;
			status = h264sw_cabac_qp_delta(&ctx, &got);
			CHECK(status == 0);
			CHECK(got == value);
		}
		stream_check(1);
	}

	/* A previous macroblock address outside the picture counts as no macroblock. */
	env_make(H264SW_SLICE_I, NONE, NONE);
	ctx.prev_mb_addr = 9;
	ctx.prev_qp_delta_nonzero = 1;
	stream_begin();
	put_unary(5U, 60U, 62U, 63U);
	stream_end(1);
	got = 999;
	status = h264sw_cabac_qp_delta(&ctx, &got);
	CHECK(status == 0 && got == 3);
	stream_check(1);

	/* Mapped values 51 (+26), 53 (+27) and 54 (-27) are out of range, and so is a run of 1 bins of any length. */
	for (index = 0; index < 5U; index++) {
		env_make(H264SW_SLICE_I, NONE, NONE);
		stream_begin();
		if (index == 0U)
			mapped = 51;
		else if (index == 1U)
			mapped = 53;
		else if (index == 2U)
			mapped = 54;
		else if (index == 3U)
			mapped = 100;
		else
			mapped = 5000;
		for (bin_idx = 0; bin_idx <= mapped; bin_idx++) {
			ctx_idx = 63U;
			if (bin_idx == 0U)
				ctx_idx = 60U;
			else if (bin_idx == 1U)
				ctx_idx = 62U;
			put(ctx_idx, bin_idx < mapped ? 1U : 0U);
		}
		stream_end(1);
		got = 999;
		status = h264sw_cabac_qp_delta(&ctx, &got);
		CHECK(status == EINVAL);
		CHECK(got == 0);
	}
}

/* Checks end_of_slice_flag: bins of 0 and then 1. */
static void
check_end_of_slice(
	void)
{
	unsigned flag;
	unsigned index;

	/* Three flags of 0 and then the 1 that ends the slice: the stream's end flag is the fourth. */
	env_make(H264SW_SLICE_I, NONE, NONE);
	stream_begin();
	for (index = 0; index < 3U; index++)
		cabacwriter_terminate(&writer, 0U);
	stream_end(1);
	for (index = 0; index < 3U; index++) {
		flag = h264sw_cabac_end_of_slice(&ctx);
		CHECK(flag == 0U);
	}
	stream_check(1);
}

/* Makes a level list for a category from a pattern number: levels only inside start..end, the others 0. */
static void
levels_pattern(
	int32_t *levels,
	unsigned start,
	unsigned end,
	unsigned pattern)
{
	static const int32_t magnitudes[] = {
		1, 2, 3, 5, 9, 13, 14, 15, 16, 17, 29, 30, 31, 100, 255, 256, 1000, 4095, 32766, 32767
	};
	unsigned index;
	uint32_t number;

	/* Starts with every level 0. */
	for (index = 0; index < 64U; index++)
		levels[index] = 0;

	/* Pattern 0 puts one level at the first position, 1 at the last, 2 in the middle. */
	if (pattern == 0U) {
		levels[start] = 7;
		return;
	}
	if (pattern == 1U) {
		levels[end] = -3;
		return;
	}
	if (pattern == 2U) {
		levels[start + (end - start) / 2U] = 1;
		return;
	}

	/* Pattern 3 is every position at 1 with signs alternating, pattern 4 every position with the magnitudes of the list in turn. */
	if (pattern == 3U) {
		for (index = start; index <= end; index++)
			levels[index] = (index & 1U) != 0U ? -1 : 1;
		return;
	}
	if (pattern == 4U) {
		for (index = start; index <= end; index++) {
			levels[index] = magnitudes[(index - start) % (sizeof(magnitudes) / sizeof(magnitudes[0]))];
			if ((index & 2U) != 0U)
				levels[index] = -levels[index];
		}
		return;
	}

	/* Pattern 5 puts the largest level at the last position and -2^15 at the first. */
	if (pattern == 5U) {
		levels[end] = 32767;
		levels[start] = -32768;
		return;
	}

	/* The other patterns are random: about a third of the positions have a level with a random magnitude, and the first and last coded positions vary. */
	for (index = start; index <= end; index++) {
		number = next_random();
		if (number % 3U == 0U) {
			levels[index] = (int32_t)(1U + (next_random() % magnitudes[next_random() % (sizeof(magnitudes) / sizeof(magnitudes[0]))]));
			if ((next_random() & 1U) != 0U)
				levels[index] = -levels[index];
		}
	}
	if (pattern % 5U == 0U)
		levels[end] = (int32_t)(1U + next_random() % 40U);
	if (pattern % 7U == 0U)
		levels[start] = -(int32_t)(1U + next_random() % 40U);
}

/* Checks residual blocks of every category with coefficient lists that are all zero, one level, all levels, the extreme levels and random ones. */
static void
check_residual_levels(
	void)
{
	struct cat_shape {
		unsigned cat;
		unsigned max_coeff;
		unsigned comp;
	};
	static const struct cat_shape shapes[] = {
		{ 0, 16, 0 },
		{ 1, 15, 0 },
		{ 2, 16, 0 },
		{ 3, 4, 1 },
		{ 3, 4, 2 },
		{ 4, 15, 1 },
		{ 4, 15, 2 },
		{ 5, 64, 0 }
	};
	int32_t levels[64];
	unsigned shape;
	unsigned pattern;
	unsigned start;
	unsigned end;
	unsigned blk;
	unsigned index;
	unsigned all_zero;

	/* Each category with every pattern over the whole block. */
	for (shape = 0; shape < sizeof(shapes) / sizeof(shapes[0]); shape++) {
		for (pattern = 0; pattern < 60U; pattern++) {
			env_make(H264SW_SLICE_I, K_I_16X16, K_I_16X16);
			start = 0;
			end = shapes[shape].max_coeff - 1U;
			levels_pattern(levels, start, end, pattern);

			/* Category 5 has no coded_block_flag, so its block must have a level; the zero list is the one case the pattern list does not have. */
			all_zero = 1;
			for (index = start; index <= end; index++) {
				if (levels[index] != 0)
					all_zero = 0;
			}
			if (all_zero != 0U && shapes[shape].cat == 5U)
				continue;

			blk = shape % shapes[shape].max_coeff % 4U;
			if (shapes[shape].cat == 0U || shapes[shape].cat == 3U)
				blk = 0;
			residual_case(shapes[shape].cat, blk, shapes[shape].comp, start, end, shapes[shape].max_coeff, 0U, levels);
		}
	}

	/* A block with no level, for the categories that have coded_block_flag. */
	for (shape = 0; shape < sizeof(shapes) / sizeof(shapes[0]); shape++) {
		if (shapes[shape].cat == 5U)
			continue;
		env_make(H264SW_SLICE_I, K_I_16X16, K_I_16X16);
		for (index = 0; index < 64U; index++)
			levels[index] = 0;
		residual_case(shapes[shape].cat, 0U, shapes[shape].comp, 0U, shapes[shape].max_coeff - 1U, shapes[shape].max_coeff, 0U, levels);
	}

	/* Blocks that read a part of the list (startIdx and endIdx inside it): the other levels stay 0. */
	for (shape = 0; shape < sizeof(shapes) / sizeof(shapes[0]); shape++) {
		for (pattern = 0; pattern < 25U; pattern++) {
			env_make(H264SW_SLICE_I, K_I_16X16, K_I_16X16);
			start = 1U + pattern % 2U;
			end = shapes[shape].max_coeff - 1U - (pattern / 2U) % 2U;
			if (end <= start + 1U)
				continue;
			levels_pattern(levels, start, end, 3U + pattern);
			blk = 0;
			residual_case(shapes[shape].cat, blk, shapes[shape].comp, start, end, shapes[shape].max_coeff, 0U, levels);
		}
	}

	/* A block of a single position (startIdx equal to endIdx): its level is implied significant. */
	for (shape = 0; shape < sizeof(shapes) / sizeof(shapes[0]); shape++) {
		env_make(H264SW_SLICE_I, K_I_16X16, K_I_16X16);
		for (index = 0; index < 64U; index++)
			levels[index] = 0;
		levels[shapes[shape].max_coeff - 1U] = -9;
		residual_case(shapes[shape].cat, 0U, shapes[shape].comp, shapes[shape].max_coeff - 1U, shapes[shape].max_coeff - 1U, shapes[shape].max_coeff, 0U, levels);
	}
}

/* Checks the contexts of coded_block_flag from the neighbouring blocks, with the contexts worked out by hand. */
static void
check_residual_contexts(
	void)
{
	struct cbf_case {
		unsigned cat;
		unsigned blk;
		unsigned comp;
		unsigned cur_kind;
		unsigned a_kind;
		uint32_t a_flags;
		unsigned b_kind;
		uint32_t b_flags;
		uint32_t cur_flags;
		unsigned inc;
	};
	/*
	 * coded_flags bits: 0 to 15 the luma 4x4 blocks, 16 the luma DC, 17
	 * and 18 the DC of Cb and Cr, 19 to 22 the AC blocks of Cb and 23 to
	 * 26 those of Cr.  Block 0 has A at block 5 of the left macroblock
	 * and B at block 10 of the upper one; chroma block 0 has A at chroma
	 * block 1 and B at chroma block 2.
	 */
#define F(n) (1UL << (n))
	static const struct cbf_case cases[] = {
		/* Luma DC (category 0): bit 16 of the macroblocks A and B. */
		{ 0, 0, 0, K_I_16X16, K_I_16X16, F(16), K_I_16X16, F(16), 0, 3 },
		{ 0, 0, 0, K_I_16X16, K_I_16X16, F(16), K_I_16X16, 0, 0, 1 },
		{ 0, 0, 0, K_I_16X16, K_I_4X4, 0, K_I_16X16, F(16), 0, 2 },
		{ 0, 0, 0, K_I_16X16, K_I_4X4, F(15) | F(17), K_I_4X4, F(5), 0, 0 },
		/* An unavailable neighbour counts as 1 for an intra macroblock. */
		{ 0, 0, 0, K_I_16X16, NONE, 0, NONE, 0, 0, 3 },
		{ 0, 0, 0, K_I_16X16, NONE, 0, K_I_16X16, 0, 0, 1 },
		{ 0, 0, 0, K_I_16X16, K_I_4X4, 0, NONE, 0, 0, 2 },
		/* An I_PCM neighbour counts as 1, a skipped one as 0, whatever the flags. */
		{ 0, 0, 0, K_I_16X16, K_I_PCM, 0, K_P_SKIP, 0xffffffffUL, 0, 1 },
		/* Luma blocks (categories 1 and 2): block 5 of A and block 10 of B for block 0. */
		{ 2, 0, 0, K_P, K_P, F(5), K_P, F(10), 0, 3 },
		{ 2, 0, 0, K_P, K_P, F(5), K_P, 0, 0, 1 },
		{ 2, 0, 0, K_P, K_P, F(4) | F(6) | F(10), K_P, F(10), 0, 2 },
		{ 2, 0, 0, K_P, K_P, F(4) | F(6), K_P, F(9) | F(11), 0, 0 },
		{ 1, 0, 0, K_I_16X16, K_I_16X16, F(5), K_I_16X16, F(10), 0, 3 },
		/* An unavailable neighbour counts as 0 for an inter macroblock and 1 for an intra one. */
		{ 2, 0, 0, K_P, NONE, 0, NONE, 0, 0, 0 },
		{ 2, 0, 0, K_B, NONE, 0, K_P, F(10), 0, 2 },
		{ 2, 0, 0, K_I_4X4, NONE, 0, NONE, 0, 0, 3 },
		{ 2, 0, 0, K_I_8X8, K_P, 0, NONE, 0, 0, 2 },
		/* An I_PCM neighbour counts as 1 and a skipped one as 0, whatever the flags; an inter macroblock next to them. */
		{ 2, 0, 0, K_P, K_I_PCM, 0, K_P_SKIP, 0xffffffffUL, 0, 1 },
		{ 2, 0, 0, K_B, K_B_SKIP, 0xffffffffUL, K_I_PCM, 0, 0, 2 },
		/* Blocks inside the current macroblock: block 3 has A at block 2 and B at block 1. */
		{ 2, 3, 0, K_P, NONE, 0, NONE, 0, F(2), 1 },
		{ 2, 3, 0, K_P, NONE, 0, NONE, 0, F(1), 2 },
		{ 2, 3, 0, K_P, NONE, 0, NONE, 0, F(1) | F(2), 3 },
		{ 2, 3, 0, K_P, NONE, 0, NONE, 0, F(0) | F(3), 0 },
		/* Block 15 has A at block 14 and B at block 13. */
		{ 2, 15, 0, K_P, NONE, 0, NONE, 0, F(14), 1 },
		{ 2, 15, 0, K_P, NONE, 0, NONE, 0, F(13), 2 },
		{ 2, 15, 0, K_B, NONE, 0, NONE, 0, F(12) | F(15), 0 },
		/* Block 4 (at 8, 0) has A at block 1 here and B at block 14 of the upper macroblock. */
		{ 2, 4, 0, K_P, NONE, 0, K_P, F(14), F(1), 3 },
		{ 2, 4, 0, K_P, NONE, 0, K_P, F(13), F(1), 1 },
		/* Chroma DC (category 3): bit 17 for Cb and 18 for Cr. */
		{ 3, 0, 1, K_P, K_P, F(17), K_P, F(17), 0, 3 },
		{ 3, 0, 1, K_P, K_P, F(18), K_P, F(18), 0, 0 },
		{ 3, 0, 2, K_P, K_P, F(17) | F(18), K_P, F(17), 0, 1 },
		{ 3, 0, 2, K_P, K_P, 0, K_P, F(18), 0, 2 },
		{ 3, 0, 1, K_I_4X4, NONE, 0, K_P, 0, 0, 1 },
		{ 3, 0, 2, K_P, NONE, 0, K_I_PCM, 0, 0, 2 },
		/* Chroma AC (category 4): chroma block 0 has A at chroma block 1 and B at chroma block 2, bits 19 + n for Cb and 23 + n for Cr. */
		{ 4, 0, 1, K_P, K_P, F(20), K_P, F(21), 0, 3 },
		{ 4, 0, 1, K_P, K_P, F(19) | F(21), K_P, F(20) | F(22), 0, 0 },
		{ 4, 0, 2, K_P, K_P, F(24), K_P, F(25), 0, 3 },
		{ 4, 0, 2, K_P, K_P, F(20) | F(23), K_P, F(21) | F(26), 0, 0 },
		{ 4, 0, 2, K_P, K_P, F(24), K_P, 0, 0, 1 },
		{ 4, 0, 1, K_P, K_P, 0, K_P, F(21), 0, 2 },
		/* Chroma blocks inside the macroblock: block 1 has A at block 0 here and B at block 3 above; block 2 has A at block 3 left and B at block 0 here; block 3 has A at block 2 and B at block 1 here. */
		{ 4, 1, 1, K_P, NONE, 0, K_P, F(22), F(19), 3 },
		{ 4, 1, 1, K_P, NONE, 0, K_P, F(19), F(20), 0 },
		{ 4, 2, 1, K_P, K_P, F(22), NONE, 0, F(19), 3 },
		{ 4, 2, 2, K_P, K_P, F(22), NONE, 0, F(23), 2 },
		{ 4, 3, 1, K_P, NONE, 0, NONE, 0, F(21), 1 },
		{ 4, 3, 1, K_P, NONE, 0, NONE, 0, F(20), 2 },
		{ 4, 3, 2, K_P, NONE, 0, NONE, 0, F(25) | F(24), 3 },
		{ 4, 3, 2, K_P, NONE, 0, NONE, 0, F(21) | F(20), 0 }
	};
#undef F
	int32_t levels[64];
	unsigned index;
	unsigned max_coeff;
	unsigned round;

	/* Each case with a block that has coefficients and one that has none (coded_block_flag 0). */
	for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		max_coeff = 16;
		if (cases[index].cat == 1U || cases[index].cat == 4U)
			max_coeff = 15;
		if (cases[index].cat == 3U)
			max_coeff = 4;
		for (round = 0; round < 2U; round++) {
			env_make(H264SW_SLICE_I, cases[index].a_kind, cases[index].b_kind);
			mbs[ADDR_CUR].kind = (uint8_t)cases[index].cur_kind;
			mbs[ADDR_CUR].coded_flags = cases[index].cur_flags;
			mbs[ADDR_A].coded_flags = cases[index].a_flags;
			mbs[ADDR_B].coded_flags = cases[index].b_flags;
			memset(levels, 0, sizeof(levels));
			if (round == 0U) {
				levels[0] = 2;
				levels[max_coeff - 1U] = -1;
			}
			residual_case(cases[index].cat, cases[index].blk, cases[index].comp, 0U, max_coeff - 1U, max_coeff, cases[index].inc, levels);
		}
	}
}

/* Checks the arguments and the values that residual blocks refuse, and truncated streams. */
static void
check_residual_errors(
	void)
{
	int32_t levels[64];
	int32_t untouched[64];
	int32_t expected[16];
	int32_t block[64];
	struct h264sw_bits bits;
	unsigned coded;
	unsigned index;
	unsigned cat;
	unsigned max_coeff;
	int status;

	/* Arguments that describe no block of 4:2:0 are refused without reading, and the levels are not touched. */
	env_make(H264SW_SLICE_I, K_I_16X16, K_I_16X16);
	stream_begin();
	put(ctx_cbf[2], 0U);
	stream_end(1);
	memset(levels, 0x5a, sizeof(levels));
	memset(untouched, 0x5a, sizeof(untouched));
	coded = 9;
	status = h264sw_cabac_residual_block(&ctx, 6U, 0U, 0U, 0U, 15U, 16U, levels, &coded);
	CHECK(status == EINVAL && coded == 0U);
	status = h264sw_cabac_residual_block(&ctx, 2U, 16U, 0U, 0U, 15U, 16U, levels, &coded);
	CHECK(status == EINVAL);
	status = h264sw_cabac_residual_block(&ctx, 2U, 0U, 1U, 0U, 15U, 16U, levels, &coded);
	CHECK(status == EINVAL);
	status = h264sw_cabac_residual_block(&ctx, 3U, 0U, 0U, 0U, 3U, 4U, levels, &coded);
	CHECK(status == EINVAL);
	status = h264sw_cabac_residual_block(&ctx, 3U, 0U, 3U, 0U, 3U, 4U, levels, &coded);
	CHECK(status == EINVAL);
	status = h264sw_cabac_residual_block(&ctx, 4U, 4U, 1U, 0U, 14U, 15U, levels, &coded);
	CHECK(status == EINVAL);
	status = h264sw_cabac_residual_block(&ctx, 5U, 4U, 0U, 0U, 63U, 64U, levels, &coded);
	CHECK(status == EINVAL);
	status = h264sw_cabac_residual_block(&ctx, 2U, 0U, 0U, 0U, 14U, 15U, levels, &coded);
	CHECK(status == EINVAL);
	status = h264sw_cabac_residual_block(&ctx, 1U, 0U, 0U, 0U, 15U, 15U, levels, &coded);
	CHECK(status == EINVAL);
	status = h264sw_cabac_residual_block(&ctx, 2U, 0U, 0U, 5U, 4U, 16U, levels, &coded);
	CHECK(status == EINVAL);
	status = h264sw_cabac_residual_block(&ctx, 2U, 0U, 0U, 0U, 16U, 16U, levels, &coded);
	CHECK(status == EINVAL);
	CHECK(memcmp(levels, untouched, sizeof(levels)) == 0);

	/* None of those refusals read a bin: the flag of 0 is still the next bin. */
	status = h264sw_cabac_residual_block(&ctx, 2U, 0U, 0U, 0U, 15U, 16U, levels, &coded);
	CHECK(status == 0 && coded == 0U);
	stream_check(1);

	/* A level of +32768 and of -32769 is out of range; -32768 and +32767 are not. */
	for (index = 0; index < 4U; index++) {
		env_make(H264SW_SLICE_I, K_I_16X16, K_I_16X16);
		memset(expected, 0, sizeof(expected));
		if (index == 0U)
			expected[0] = 32768;
		else if (index == 1U)
			expected[0] = -32769;
		else if (index == 2U)
			expected[0] = -32768;
		else
			expected[0] = 32767;
		stream_begin();
		put_residual(2U, 0U, expected, 0U, 15U);
		stream_end(1);
		status = h264sw_cabac_residual_block(&ctx, 2U, 0U, 0U, 0U, 15U, 16U, block, &coded);
		if (index < 2U) {
			CHECK(status == EINVAL);
		} else {
			CHECK(status == 0);
			CHECK(block[0] == expected[0]);
			stream_check(1);
		}
	}

	/* An Exp-Golomb suffix with more than 24 bins of 1 is refused, in every category. */
	for (cat = 0; cat < 6U; cat++) {
		max_coeff = 64;
		if (cat == 0U || cat == 2U)
			max_coeff = 16;
		else if (cat == 1U || cat == 4U)
			max_coeff = 15;
		else if (cat == 3U)
			max_coeff = 4;

		/* A coded block whose first position is the only level: the flag, the significance and last flags, a prefix of fourteen 1 bins, and forty bypass bins of 1. */
		env_make(H264SW_SLICE_I, K_I_16X16, K_I_16X16);
		stream_begin();
		if (cat != 5U)
			put(ctx_cbf[cat], 1U);
		put(ctx_sig[cat] + residual_sig_inc(cat, 0U), 1U);
		put(ctx_last[cat] + residual_last_inc(cat, 0U), 1U);
		put(ctx_abs[cat] + 1U, 1U);
		for (index = 1; index < 14U; index++)
			put(ctx_abs[cat] + 5U, 1U);
		for (index = 0; index < 40U; index++)
			put_bypass(1U);
		stream_end(1);
		status = h264sw_cabac_residual_block(&ctx, cat, 0U, cat == 3U || cat == 4U ? 1U : 0U, 0U, max_coeff - 1U, max_coeff, block, &coded);
		CHECK(status == EINVAL);
	}

	/* A stream cut short: the decode reads zero bits, marks the error and ends. */
	env_make(H264SW_SLICE_I, K_I_16X16, K_I_16X16);
	for (index = 0; index < 64U; index++)
		levels[index] = (int32_t)(index + 1U) * ((index & 1U) != 0U ? -1 : 1);
	stream_begin();
	put_residual(5U, 0U, levels, 0U, 63U);
	stream_end(1);

	/* Only the first three bytes of what was written are given to the decoder. */
	h264sw_bits_init(&bits, written.data, 3U);
	status = h264sw_cabac_start(&ctx.cabac, &bits, H264SW_SLICE_I, 0U, 26);
	if (status == 0) {
		memcpy(ctx.cabac.state, initial_state, sizeof(initial_state));
		status = h264sw_cabac_residual_block(&ctx, 5U, 0U, 0U, 0U, 63U, 64U, block, &coded);
		CHECK(ctx.cabac.error != 0 || status != 0);
	}
}

/* Runs every element over random bytes and random neighbours: the values stay in range, the loops end, nothing reads outside its arrays. */
static void
check_fuzz(
	void)
{
	uint8_t data[96];
	struct h264sw_bits bits;
	unsigned iteration;
	unsigned index;
	unsigned slice_type;
	unsigned value;
	unsigned flag;
	unsigned rem;
	unsigned coded;
	unsigned cat;
	unsigned max_coeff;
	unsigned start;
	unsigned end;
	int32_t levels[64];
	int delta;
	int status;
	uint32_t number;

	/* Many random streams and neighbour states. */
	for (iteration = 0; iteration < 1500U; iteration++) {
		number = next_random();
		for (index = 0; index < sizeof(data); index++) {
			data[index] = (uint8_t)next_random();
			if (iteration % 7U == 0U)
				data[index] = 0xff;
			if (iteration % 11U == 0U)
				data[index] = 0x00;
		}
		data[sizeof(data) - 1U] |= 0x80U;
		slice_type = number % 3U == 0U ? H264SW_SLICE_I : (number % 3U == 1U ? H264SW_SLICE_P : H264SW_SLICE_B);
		env_make(slice_type, K_I_4X4, K_P);

		/* Random neighbour entries. */
		for (index = 0; index < 9U; index++) {
			mbs[index].slice = (index == ADDR_CUR || (next_random() & 3U) != 0U) ? 0 : -1;
			mbs[index].kind = (uint8_t)(next_random() % 13U);
			mbs[index].cbp = (uint8_t)next_random();
			mbs[index].transform_8x8 = (uint8_t)(next_random() & 1U);
			mbs[index].chroma_pred_mode = (uint8_t)(next_random() % 4U);
			mbs[index].direct = (uint8_t)next_random();
			mbs[index].coded_flags = next_random() & 0x7ffffffU;
			mbs[index].mvd[0][5][0] = (uint8_t)next_random();
			mbs[index].mvd[1][10][1] = (uint8_t)next_random();
			mbs[index].motion.ref_idx[0][1] = (int8_t)((int)(next_random() % 6U) - 1);
			mbs[index].motion.ref_idx[1][2] = (int8_t)((int)(next_random() % 6U) - 1);
		}
		h264sw_mb_neighbours(mbs, 3U, ADDR_CUR, &ctx.n);
		ctx.prev_mb_addr = (int32_t)(next_random() % 11U) - 2;
		ctx.prev_qp_delta_nonzero = (int)(next_random() & 1U);
		header.num_ref_idx_active[0] = 1U + next_random() % 32U;
		header.num_ref_idx_active[1] = 1U + next_random() % 32U;

		/* Starts the engine; a start the engine refuses is skipped. */
		h264sw_bits_init(&bits, data, sizeof(data));
		status = h264sw_cabac_start(&ctx.cabac, &bits, slice_type, 0U, (int)(number % 52U));
		if (status != 0)
			continue;

		/* Each element a few times; the values come back within their ranges. */
		for (index = 0; index < 3U; index++) {
			if (slice_type != H264SW_SLICE_I) {
				flag = h264sw_cabac_mb_skip(&ctx);
				CHECK(flag <= 1U);
			}
			status = h264sw_cabac_mb_type(&ctx, slice_type, &value);
			CHECK(status == 0);
			CHECK(value <= (slice_type == H264SW_SLICE_I ? 25U : (slice_type == H264SW_SLICE_P ? 30U : 48U)));
			if (slice_type != H264SW_SLICE_I) {
				status = h264sw_cabac_sub_mb_type(&ctx, slice_type, &value);
				CHECK(status == 0);
				CHECK(value <= (slice_type == H264SW_SLICE_P ? 3U : 12U));
			}
			flag = h264sw_cabac_transform_8x8(&ctx);
			CHECK(flag <= 1U);
			h264sw_cabac_intra_mode(&ctx, &flag, &rem);
			CHECK(flag <= 1U && rem <= 7U);
			value = h264sw_cabac_chroma_mode(&ctx);
			CHECK(value <= 3U);
			if (slice_type != H264SW_SLICE_I) {
				status = h264sw_cabac_ref_idx(&ctx, next_random() & 1U, 4U * (next_random() % 4U), 4U * (next_random() % 4U), &value);
				CHECK(status == 0 || status == EINVAL);
				CHECK(status != 0 || value < 32U);
				status = h264sw_cabac_mvd(&ctx, next_random() & 1U, 4U * (next_random() % 4U), 4U * (next_random() % 4U), next_random() & 1U, &delta);
				CHECK(status == 0 || status == EINVAL);
				CHECK(status != 0 || (delta >= -32768 && delta <= 32767));
			}
			value = h264sw_cabac_cbp(&ctx);
			CHECK(value < 48U);
			status = h264sw_cabac_qp_delta(&ctx, &delta);
			CHECK(status == 0 || status == EINVAL);
			CHECK(status != 0 || (delta >= -26 && delta <= 25));

			/* A residual block of a random category with a random range inside it. */
			cat = next_random() % 6U;
			max_coeff = cat == 0U ? 16U : (cat == 1U ? 15U : (cat == 2U ? 16U : (cat == 3U ? 4U : (cat == 4U ? 15U : 64U))));
			start = next_random() % max_coeff;
			end = start + next_random() % (max_coeff - start);
			status = h264sw_cabac_residual_block(&ctx, cat, cat == 0U || cat == 3U ? 0U : next_random() % (cat == 4U || cat == 5U ? 4U : 16U), cat == 3U || cat == 4U ? 1U + (next_random() & 1U) : 0U, start, end, max_coeff, levels, &coded);
			CHECK(status == 0 || status == EINVAL);
			CHECK(coded <= 1U);
			for (value = 0; value < max_coeff; value++)
				CHECK(levels[value] >= -32768 && levels[value] <= 32767);
			flag = h264sw_cabac_end_of_slice(&ctx);
			CHECK(flag <= 1U);
		}
	}
}

/* Runs every test. */
int
main(
	void)
{
	/* The elements, the neighbour contexts and the error cases. */
	check_mb_skip();
	check_mb_type_i();
	check_mb_type_p();
	check_mb_type_b();
	check_mb_type_errors();
	check_sub_mb_type();
	check_transform_8x8();
	check_intra_mode();
	check_chroma_mode();
	check_ref_idx();
	check_mvd();
	check_cbp();
	check_qp_delta();
	check_end_of_slice();
	check_residual_levels();
	check_residual_contexts();
	check_residual_errors();
	check_fuzz();

	/* Reports the checks. */
	return check_finish("cabac_se");
}
