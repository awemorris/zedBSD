/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the CABAC arithmetic decoding engine (h264sw-cabac) of the
 * software H.264 decoder: the shape of Tables 9-44 and 9-45, the
 * initialization of the context variables against values worked out by
 * hand from equation 9-5 and against the list of Table 9-11, streams
 * written by the informative encoder of 9.3.4 (cabacwriter.h) and read
 * back bin by bin, I_PCM samples in the middle of a stream, the error
 * cases, and random bytes that must not make the engine fail badly.
 */

#include "check.h"
#include "bitwriter.h"
#include "cabacwriter.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "userland/desktop/libmedia/h264sw-bits.h"
#include "userland/desktop/libmedia/h264sw-cabac.h"
#include "userland/desktop/libmedia/h264sw-cabac-init.h"
#include "userland/desktop/libmedia/h264sw-slice.h"

/* The tables of the engine, which no header declares. */
extern const uint8_t h264sw_cabac_range_lps[64][4];
extern const uint8_t h264sw_cabac_trans_lps[64];
extern const uint8_t h264sw_cabac_trans_mps[64];

/* The slice types the engine is started with, as the first index of the lists below. */
#define KIND_I 0U
#define KIND_P 1U
#define KIND_B 2U
#define KINDS 3U

/* The bits of the kinds a Table 9-11 row applies to. */
#define ROW_I 1U
#define ROW_P 2U
#define ROW_B 4U
#define ROW_ALL (ROW_I | ROW_P | ROW_B)

/* The operations of a test stream. */
#define OP_DECISION 0U
#define OP_BYPASS 1U
#define OP_TERMINATE 2U
#define OP_PCM 3U

/* The most operations one test stream holds. */
#define OPS_MAX 24000U

/* The bytes of the I_PCM samples of a 4:2:0 macroblock (7.3.5). */
#define PCM_BYTES 384U

/* The slice QPs the streams are made with. */
#define QP_COUNT 4U

/* One row of Table 9-11: the ctxIdx range of one syntax element and the slice types that have it. */
struct row {
	unsigned first;
	unsigned last;
	unsigned kinds;
};

/* A context variable value worked out by hand: the context, the set, SliceQPY and the expected state. */
struct hand {
	unsigned ctx;
	unsigned kind;
	unsigned idc;
	int qp;
	unsigned state_idx;
	unsigned value_mps;
};

/* One operation of a test stream: what to code, with which context and which bin. */
struct op {
	unsigned type;
	unsigned ctx;
	unsigned bin;
};

/*
 * The syntax elements of Table 9-11 with their ctxIdx and slice types,
 * typed from the table (SI slices are not decoded and left out).
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-11.
 */
static const struct row rows[] = {
	/* slice_data(): mb_skip_flag and mb_field_decoding_flag. */
	{ 11U, 13U, ROW_P },
	{ 24U, 26U, ROW_B },
	{ 70U, 72U, ROW_ALL },

	/* mb_type, and sub_mb_type of P and B slices. */
	{ 3U, 10U, ROW_I },
	{ 14U, 20U, ROW_P },
	{ 27U, 35U, ROW_B },
	{ 21U, 23U, ROW_P },
	{ 36U, 39U, ROW_B },

	/* macroblock_layer() and mb_pred(). */
	{ 399U, 401U, ROW_ALL },
	{ 73U, 76U, ROW_ALL },
	{ 77U, 84U, ROW_ALL },
	{ 60U, 63U, ROW_ALL },
	{ 68U, 68U, ROW_ALL },
	{ 69U, 69U, ROW_ALL },
	{ 64U, 67U, ROW_ALL },
	{ 54U, 59U, ROW_P | ROW_B },
	{ 40U, 46U, ROW_P | ROW_B },
	{ 47U, 53U, ROW_P | ROW_B },

	/* residual_block_cabac(): coded_block_flag, significant_coeff_flag, last_significant_coeff_flag, coeff_abs_level_minus1. */
	{ 85U, 104U, ROW_ALL },
	{ 105U, 165U, ROW_ALL },
	{ 277U, 337U, ROW_ALL },
	{ 402U, 416U, ROW_ALL },
	{ 436U, 450U, ROW_ALL },
	{ 166U, 226U, ROW_ALL },
	{ 338U, 398U, ROW_ALL },
	{ 417U, 425U, ROW_ALL },
	{ 451U, 459U, ROW_ALL },
	{ 227U, 275U, ROW_ALL },
	{ 426U, 435U, ROW_ALL }
};

/*
 * Context variables worked out by hand from 9-5: preCtxState =
 * Clip3( 1, 126, ( ( m * Clip3( 0, 51, SliceQPY ) ) >> 4 ) + n ), then
 * pStateIdx = 63 - preCtxState and valMPS 0 up to 63, or
 * pStateIdx = preCtxState - 64 and valMPS 1 above.  The (m, n) pair of
 * each is the one of the table named in the comment; idc is
 * cabac_init_idc (ignored for I).
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Equation 9-5 with Tables 9-12 to 9-24.
 */
static const struct hand hands[] = {
	/* ctxIdx 3 (Table 9-12, m 20 n -15): qp 0 clips to 1; 20 * 26 >> 4 = 32, so 17; 20 * 51 >> 4 = 63, so 48. */
	{ 3U, KIND_I, 0U, 0, 62U, 0U },
	{ 3U, KIND_I, 0U, 26, 46U, 0U },
	{ 3U, KIND_I, 0U, 51, 15U, 0U },

	/* ctxIdx 6 (m -28 n 127): -728 >> 4 = -46 (rounded down), so 81; -1428 >> 4 = -90, so 37; qp 0 clips to 126. */
	{ 6U, KIND_I, 0U, 0, 62U, 1U },
	{ 6U, KIND_I, 0U, 26, 17U, 1U },
	{ 6U, KIND_I, 0U, 51, 26U, 0U },

	/* ctxIdx 11 (Table 9-13; idc 0 m 23 n 33, idc 1 m 22 n 25, idc 2 m 29 n 16): 70, 60 and 63 at qp 26. */
	{ 11U, KIND_P, 0U, 0, 30U, 0U },
	{ 11U, KIND_P, 0U, 26, 6U, 1U },
	{ 11U, KIND_P, 0U, 51, 42U, 1U },
	{ 11U, KIND_P, 1U, 26, 3U, 0U },
	{ 11U, KIND_P, 2U, 26, 0U, 0U },
	{ 11U, KIND_P, 2U, 51, 44U, 1U },

	/* ctxIdx 24 (Table 9-14, idc 0 m 18 n 64): 29 + 64 = 93 at qp 26. */
	{ 24U, KIND_B, 0U, 26, 29U, 1U },
	{ 24U, KIND_B, 1U, 26, 12U, 1U },

	/* ctxIdx 40 (Table 9-15, idc 0 m -3 n 69): -78 >> 4 = -5, so 64, the first valMPS 1 state. */
	{ 40U, KIND_P, 0U, 26, 0U, 1U },
	{ 40U, KIND_B, 0U, 26, 0U, 1U },
	{ 40U, KIND_P, 2U, 51, 10U, 0U },

	/* ctxIdx 54 (Table 9-16, idc 0 m -7 n 67): -182 >> 4 = -12, so 55. */
	{ 54U, KIND_P, 0U, 26, 8U, 0U },
	{ 54U, KIND_B, 0U, 26, 8U, 0U },
	{ 54U, KIND_B, 2U, 51, 0U, 1U },

	/* ctxIdx 60 and 69 (Table 9-17, m 0 n 41 and m 3 n 62): the same for every slice type. */
	{ 60U, KIND_I, 0U, 0, 22U, 0U },
	{ 60U, KIND_B, 2U, 51, 22U, 0U },
	{ 69U, KIND_I, 0U, 26, 2U, 1U },
	{ 69U, KIND_P, 1U, 51, 7U, 1U },

	/* ctxIdx 70 (Table 9-18, I m 0 n 11; idc 1 m 13 n 15): 11 at any qp; 338 >> 4 = 21, so 36 at qp 26. */
	{ 70U, KIND_I, 0U, 26, 52U, 0U },
	{ 70U, KIND_P, 1U, 26, 27U, 0U },

	/* ctxIdx 105 (Table 9-19, I m -7 n 93) and 165 (idc 1 m 0 n 89). */
	{ 105U, KIND_I, 0U, 26, 17U, 1U },
	{ 165U, KIND_B, 1U, 26, 25U, 1U },

	/* ctxIdx 166 (Table 9-20, I m 24 n 0): 624 >> 4 = 39. ctxIdx 226 (I m 2 n 97): 3 + 97 = 100. */
	{ 166U, KIND_I, 0U, 26, 24U, 0U },
	{ 226U, KIND_I, 0U, 26, 36U, 1U },

	/* ctxIdx 227 (Table 9-21, I m -3 n 71): -78 >> 4 = -5, so 66. */
	{ 227U, KIND_I, 0U, 26, 2U, 1U },

	/* ctxIdx 277, 338 (Tables 9-22 and 9-23): idc 2 m -22 n 127 and I m 15 n 6. */
	{ 277U, KIND_P, 2U, 51, 7U, 0U },
	{ 338U, KIND_I, 0U, 26, 33U, 0U },

	/* ctxIdx 399 (Table 9-16, I m 31 n 21): 806 >> 4 = 50, so 71. */
	{ 399U, KIND_I, 0U, 26, 7U, 1U },

	/* ctxIdx 402, 417, 426, 436, 451 and 459 (Table 9-24). */
	{ 402U, KIND_I, 0U, 26, 28U, 1U },
	{ 417U, KIND_I, 0U, 51, 3U, 0U },
	{ 426U, KIND_P, 1U, 26, 1U, 0U },
	{ 436U, KIND_B, 2U, 26, 9U, 1U },
	{ 451U, KIND_P, 0U, 0, 62U, 0U },
	{ 459U, KIND_P, 0U, 26, 62U, 1U }
};

/* The SliceQPY values the streams are coded with: the lowest, two middle ones and the highest. */
static const int stream_qp[QP_COUNT] = { 0, 18, 26, 51 };

/* The state of the pseudo-random generator that makes the bin sequences. */
static uint32_t random_state;

/* The operations of the stream being tested and the bytes written for it. */
static struct op ops[OPS_MAX];
static struct bitwriter written;

/* Which pStateIdx the engine was in before a least probable symbol, and which (pStateIdx, qCodIRangeIdx) pairs a decision saw. */
static unsigned seen_lps_from[64];
static unsigned seen_pairs[64][4];

/* Draws the next pseudo-random number (xorshift32). */
static uint32_t random_next(void);

/* Tells whether a Table 9-11 row list has the ctxIdx for the slice kind. */
static int listed(unsigned kind, unsigned ctx_idx);

/* Gives the slice type of the engine for a kind. */
static unsigned kind_slice_type(unsigned kind);

/* Makes the engine's data and bits for a buffer: aligned, as the slice data starts. */
static int engine_start(struct h264sw_cabac *cabac, const uint8_t *data, size_t size, unsigned kind, unsigned idc, int qp);

/* Gives the byte pcm sample index of operation number op_index holds. */
static uint8_t pcm_byte(uint32_t seed, unsigned op_index, size_t index);

/* Fills ops with a random operation list; pcm_percent of them are I_PCM macroblocks. */
static unsigned make_ops(unsigned kind, unsigned count, unsigned pcm_percent);

/* Encodes the operations into written and returns the bytes written. */
static size_t encode_ops(unsigned kind, unsigned idc, int qp, unsigned count, uint32_t pcm_seed, unsigned prefix_bits);

/* Decodes the bytes written for the operations and compares every bin and I_PCM sample. */
static void decode_and_compare(unsigned kind, unsigned idc, int qp, unsigned count, uint32_t pcm_seed, unsigned prefix_bits, size_t bytes);

/* Checks the shape of the tables and a few values read from the pages. */
static void check_tables(void);

/* Checks the initialization of the context variables. */
static void check_init(void);

/* Checks streams without I_PCM, read back bin by bin. */
static void check_roundtrip(void);

/* Checks I_PCM samples in the middle of streams. */
static void check_pcm(void);

/* Checks the error cases. */
static void check_errors(void);

/* Feeds random bytes and random calls. */
static void check_garbage(void);

/* Draws the next pseudo-random number (xorshift32). */
static uint32_t
random_next(
	void)
{
	uint32_t value;

	/* The three shifts of xorshift32, which never reaches 0 from a nonzero seed. */
	value = random_state;
	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	random_state = value;

	/* Reports the new state as the number. */
	return value;
}

/* Tells whether a Table 9-11 row list has the ctxIdx for the slice kind. */
static int
listed(
	unsigned kind,
	unsigned ctx_idx)
{
	size_t index;
	unsigned bit;

	/* The bit of the kind in a row's kinds. */
	bit = ROW_I;
	if (kind == KIND_P)
		bit = ROW_P;
	if (kind == KIND_B)
		bit = ROW_B;

	/* Looks for a row of the kind that holds the ctxIdx. */
	for (index = 0U; index < sizeof(rows) / sizeof(rows[0]); index++) {
		if ((rows[index].kinds & bit) == 0U)
			continue;
		if (ctx_idx >= rows[index].first && ctx_idx <= rows[index].last)
			return 1;
	}

	/* No row of the kind has it. */
	return 0;
}

/* Gives the slice type of the engine for a kind. */
static unsigned
kind_slice_type(
	unsigned kind)
{
	/* The types of the three kinds. */
	if (kind == KIND_P)
		return H264SW_SLICE_P;
	if (kind == KIND_B)
		return H264SW_SLICE_B;

	/* The default is the I slice. */
	return H264SW_SLICE_I;
}

/* Makes the engine's data and bits for a buffer: aligned, as the slice data starts. */
static int
engine_start(
	struct h264sw_cabac *cabac,
	const uint8_t *data,
	size_t size,
	unsigned kind,
	unsigned idc,
	int qp)
{
	struct h264sw_bits bits;
	int status;

	/* A reader over the buffer at its first bit. */
	h264sw_bits_init(&bits, data, size);

	/* Starts the engine as the slice data begins. */
	status = h264sw_cabac_start(cabac, &bits, kind_slice_type(kind), idc, qp);

	/* Reports the engine's answer. */
	return status;
}

/* Gives the byte pcm sample index of operation number op_index holds. */
static uint8_t
pcm_byte(
	uint32_t seed,
	unsigned op_index,
	size_t index)
{
	uint32_t value;

	/* A mix of the three numbers that the encoder and the decoder side both compute. */
	value = seed + (uint32_t)op_index * 2654435761U + (uint32_t)index * 40503U;
	value ^= value >> 15;
	value *= 2246822519U;
	value ^= value >> 13;

	/* One byte of the mix. */
	return (uint8_t)(value >> 3);
}

/* Fills ops with a random operation list; pcm_percent of them are I_PCM macroblocks. */
static unsigned
make_ops(
	unsigned kind,
	unsigned count,
	unsigned pcm_percent)
{
	static const unsigned bias_pool[] = { 1U, 6U, 30U, 64U, 128U, 192U, 226U, 250U, 255U };
	unsigned bias[H264SW_CABAC_CONTEXTS];
	unsigned usable[H264SW_CABAC_CONTEXTS];
	unsigned usable_count;
	unsigned ctx_idx;
	unsigned index;
	unsigned pick;
	unsigned roll;

	/* The contexts the slice kind initializes, each with a first bias toward a bin of 1. */
	usable_count = 0U;
	for (ctx_idx = 0U; ctx_idx < H264SW_CABAC_CONTEXTS; ctx_idx++) {
		bias[ctx_idx] = bias_pool[random_next() % (sizeof(bias_pool) / sizeof(bias_pool[0]))];
		if (ctx_idx == H264SW_CABAC_TERMINATE)
			continue;
		if (!listed(kind, ctx_idx))
			continue;
		usable[usable_count] = ctx_idx;
		usable_count++;
	}

	/* Draws the operations: mostly decisions, some bypass bins, a few end_of_slice_flag bins of 0 and I_PCM macroblocks. */
	for (index = 0U; index < count; index++) {
		roll = random_next() % 100U;
		pick = usable[random_next() % usable_count];

		/* A context's bias changes now and then, so its state can cross from one end of the table to the other. */
		if (random_next() % 700U == 0U)
			bias[pick] = bias_pool[random_next() % (sizeof(bias_pool) / sizeof(bias_pool[0]))];

		/* Chooses the kind of operation by the roll. */
		if (roll < pcm_percent) {
			ops[index].type = OP_PCM;
			ops[index].ctx = 0U;
			ops[index].bin = 0U;
		} else if (roll < 70U) {
			ops[index].type = OP_DECISION;
			ops[index].ctx = pick;
			ops[index].bin = 0U;
			if ((random_next() & 255U) < bias[pick])
				ops[index].bin = 1U;
		} else if (roll < 96U) {
			ops[index].type = OP_BYPASS;
			ops[index].ctx = 0U;
			ops[index].bin = random_next() & 1U;
		} else {
			ops[index].type = OP_TERMINATE;
			ops[index].ctx = 0U;
			ops[index].bin = 0U;
		}
	}

	/* Reports how many operations were made. */
	return count;
}

/* Encodes the operations into written and returns the bytes written. */
static size_t
encode_ops(
	unsigned kind,
	unsigned idc,
	int qp,
	unsigned count,
	uint32_t pcm_seed,
	unsigned prefix_bits)
{
	static struct cabacwriter writer;
	uint8_t samples[PCM_BYTES];
	unsigned set;
	unsigned ctx_idx;
	unsigned index;
	size_t sample_index;

	/* The context variables start from the (m, n) of the set, as 9.3.1.1 says. */
	set = 0U;
	if (kind != KIND_I)
		set = 1U + idc;
	memset(&writer, 0, sizeof(writer));
	for (ctx_idx = 0U; ctx_idx < H264SW_CABAC_CONTEXTS; ctx_idx++) {
		if (ctx_idx == H264SW_CABAC_TERMINATE)
			continue;
		if (!listed(kind, ctx_idx))
			continue;
		cabacwriter_context_set(&writer, ctx_idx,
		    h264sw_cabac_init_mn[ctx_idx][set][0],
		    h264sw_cabac_init_mn[ctx_idx][set][1],
		    qp);
	}

	/* Some bits of slice header, then cabac_alignment_one_bit up to the byte boundary. */
	bitwriter_init(&written);
	bitwriter_u(&written, 0x15U & ((1U << prefix_bits) - 1U), prefix_bits);
	while (written.bits % 8U != 0U)
		bitwriter_u(&written, 1U, 1U);

	/* Encodes every operation. */
	cabacwriter_engine_init(&writer, &written);
	for (index = 0U; index < count; index++) {
		if (ops[index].type == OP_DECISION) {
			cabacwriter_decision(&writer, ops[index].ctx, ops[index].bin);
		} else if (ops[index].type == OP_BYPASS) {
			cabacwriter_bypass(&writer, ops[index].bin);
		} else if (ops[index].type == OP_TERMINATE) {
			cabacwriter_terminate(&writer, 0U);
		} else {
			/* An I_PCM macroblock: the terminate bin of 1, then the samples and a new engine. */
			cabacwriter_terminate(&writer, 1U);
			for (sample_index = 0U; sample_index < PCM_BYTES; sample_index++)
				samples[sample_index] = pcm_byte(pcm_seed, index, sample_index);
			cabacwriter_pcm(&writer, samples, PCM_BYTES);
		}
	}

	/* The end of the slice: end_of_slice_flag 1, whose last bit is the stop bit, then zero bits to the byte boundary. */
	cabacwriter_terminate(&writer, 1U);
	while (written.bits % 8U != 0U)
		bitwriter_u(&written, 0U, 1U);

	/* Reports the bytes written. */
	return bitwriter_bytes(&written);
}

/* Decodes the bytes written for the operations and compares every bin and I_PCM sample. */
static void
decode_and_compare(
	unsigned kind,
	unsigned idc,
	int qp,
	unsigned count,
	uint32_t pcm_seed,
	unsigned prefix_bits,
	size_t bytes)
{
	struct h264sw_cabac cabac;
	struct h264sw_bits bits;
	uint8_t samples[PCM_BYTES];
	unsigned expected;
	unsigned bin;
	unsigned before;
	unsigned quantized;
	unsigned index;
	unsigned mismatches;
	unsigned first_mismatch;
	size_t sample_index;
	uint32_t prefix_value;
	int status;

	/* Reads the slice header bits and starts the engine. */
	h264sw_bits_init(&bits, written.data, bytes);
	prefix_value = h264sw_bits_u(&bits, prefix_bits);
	CHECK(prefix_value == (0x15U & ((1U << prefix_bits) - 1U)));
	status = h264sw_cabac_start(&cabac, &bits, kind_slice_type(kind), idc, qp);
	CHECK(status == 0);
	if (status != 0)
		return;

	/* Decodes every operation and compares it with what was coded. */
	mismatches = 0U;
	first_mismatch = 0U;
	for (index = 0U; index < count; index++) {
		expected = ops[index].bin;
		bin = 2U;
		if (ops[index].type == OP_DECISION) {
			before = cabac.state[ops[index].ctx];
			quantized = (cabac.range >> 6) & 3U;
			bin = h264sw_cabac_decision(&cabac, ops[index].ctx);
			seen_pairs[before & 63U][quantized]++;
			if (bin != (before >> 6))
				seen_lps_from[before & 63U]++;
		} else if (ops[index].type == OP_BYPASS) {
			bin = h264sw_cabac_bypass(&cabac);
		} else if (ops[index].type == OP_TERMINATE) {
			bin = h264sw_cabac_terminate(&cabac);
		} else {
			/* An I_PCM macroblock: the terminate bin is 1 and the samples follow. */
			expected = 1U;
			bin = h264sw_cabac_terminate(&cabac);
			status = h264sw_cabac_pcm(&cabac, samples, PCM_BYTES);
			if (status != 0)
				bin = 2U;
			for (sample_index = 0U; sample_index < PCM_BYTES; sample_index++) {
				if (samples[sample_index] != pcm_byte(pcm_seed, index, sample_index))
					bin = 2U;
			}
		}

		/* The first bin that differs is remembered. */
		if (bin != expected) {
			if (mismatches == 0U)
				first_mismatch = index;
			mismatches++;
		}
	}
	if (mismatches != 0U)
		printf("stream kind %u idc %u qp %d: first mismatch at operation %u of %u\n", kind, idc, qp, first_mismatch, count);
	CHECK(mismatches == 0U);

	/* The end_of_slice_flag is 1 where it was coded, and no read went past the end. */
	bin = h264sw_cabac_terminate(&cabac);
	CHECK(bin == 1U);
	CHECK(cabac.error == 0);
}

/* Checks the shape of the tables and a few values read from the pages. */
static void
check_tables(
	void)
{
	unsigned state_idx;
	unsigned column;

	/* Table 9-44 entries read from the page: the first row, a middle one, the last rows. */
	CHECK(h264sw_cabac_range_lps[0][0] == 128U && h264sw_cabac_range_lps[0][3] == 240U);
	CHECK(h264sw_cabac_range_lps[12][0] == 77U && h264sw_cabac_range_lps[12][3] == 128U);
	CHECK(h264sw_cabac_range_lps[31][0] == 29U && h264sw_cabac_range_lps[31][3] == 48U);
	CHECK(h264sw_cabac_range_lps[32][0] == 27U && h264sw_cabac_range_lps[32][3] == 45U);
	CHECK(h264sw_cabac_range_lps[45][1] == 17U && h264sw_cabac_range_lps[45][2] == 20U);
	CHECK(h264sw_cabac_range_lps[62][0] == 6U && h264sw_cabac_range_lps[62][3] == 9U);
	CHECK(h264sw_cabac_range_lps[63][0] == 2U && h264sw_cabac_range_lps[63][3] == 2U);

	/* Table 9-45 entries read from the page. */
	CHECK(h264sw_cabac_trans_lps[5] == 4U && h264sw_cabac_trans_lps[13] == 11U);
	CHECK(h264sw_cabac_trans_lps[24] == 19U && h264sw_cabac_trans_lps[31] == 24U);
	CHECK(h264sw_cabac_trans_lps[47] == 33U && h264sw_cabac_trans_lps[61] == 38U);
	CHECK(h264sw_cabac_trans_lps[63] == 63U);
	CHECK(h264sw_cabac_trans_mps[0] == 1U && h264sw_cabac_trans_mps[61] == 62U);
	CHECK(h264sw_cabac_trans_mps[62] == 62U && h264sw_cabac_trans_mps[63] == 63U);

	/* Every row grows with qCodIRangeIdx (the last row is 2, 2, 2, 2), and every column does not grow with pStateIdx. */
	for (state_idx = 0U; state_idx < 64U; state_idx++) {
		for (column = 0U; column < 3U; column++) {
			if (state_idx < 63U) {
				CHECK(h264sw_cabac_range_lps[state_idx][column] < h264sw_cabac_range_lps[state_idx][column + 1U]);
			} else {
				CHECK(h264sw_cabac_range_lps[state_idx][column] == h264sw_cabac_range_lps[state_idx][column + 1U]);
			}
		}
		for (column = 0U; column < 4U; column++) {
			if (state_idx > 0U)
				CHECK(h264sw_cabac_range_lps[state_idx][column] <= h264sw_cabac_range_lps[state_idx - 1U][column]);
		}
	}
	for (column = 0U; column < 4U; column++)
		CHECK(h264sw_cabac_range_lps[63][column] == 2U);

	/* transIdxMPS is Min( pStateIdx + 1, 62 ) with 63 staying 63; transIdxLPS never rises, never falls from state to state, and 0 stays 0. */
	for (state_idx = 0U; state_idx < 64U; state_idx++) {
		if (state_idx < 63U) {
			CHECK(h264sw_cabac_trans_mps[state_idx] == (state_idx + 1U < 62U ? state_idx + 1U : 62U));
		} else {
			CHECK(h264sw_cabac_trans_mps[state_idx] == 63U);
		}
		CHECK(h264sw_cabac_trans_lps[state_idx] <= state_idx);
		if (state_idx > 0U && state_idx < 63U)
			CHECK(h264sw_cabac_trans_lps[state_idx] >= h264sw_cabac_trans_lps[state_idx - 1U]);
	}
	CHECK(h264sw_cabac_trans_lps[0] == 0U);

	/* The decoder's tables equal the encoder's own copy of the tables, entry by entry. */
	for (state_idx = 0U; state_idx < 64U; state_idx++) {
		for (column = 0U; column < 4U; column++)
			CHECK(h264sw_cabac_range_lps[state_idx][column] == cabacwriter_range_lps[state_idx][column]);
		CHECK(h264sw_cabac_trans_lps[state_idx] == cabacwriter_trans_lps[state_idx]);
		CHECK(h264sw_cabac_trans_mps[state_idx] == cabacwriter_trans_mps[state_idx]);
	}
}

/* Checks the initialization of the context variables. */
static void
check_init(
	void)
{
	static const uint8_t data[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	struct h264sw_cabac cabac;
	struct cabacwriter reference;
	unsigned kind;
	unsigned idc;
	unsigned set;
	unsigned ctx_idx;
	unsigned expected;
	unsigned index;
	int qp;
	int status;

	/* The context variables worked out by hand. */
	for (index = 0U; index < sizeof(hands) / sizeof(hands[0]); index++) {
		status = engine_start(&cabac, data, sizeof(data), hands[index].kind, hands[index].idc, hands[index].qp);
		CHECK(status == 0);
		CHECK((cabac.state[hands[index].ctx] & 63U) == hands[index].state_idx);
		CHECK((cabac.state[hands[index].ctx] >> 6) == hands[index].value_mps);
		CHECK(listed(hands[index].kind, hands[index].ctx));
	}

	/* Every slice type, cabac_init_idc and SliceQPY: the listed contexts have the state of 9-5, the others are 0, and 276 is never set. */
	for (kind = 0U; kind < KINDS; kind++) {
		for (idc = 0U; idc < (kind == KIND_I ? 1U : 3U); idc++) {
			for (qp = -3; qp <= 54; qp++) {
				status = engine_start(&cabac, data, sizeof(data), kind, idc, qp);
				CHECK(status == 0);
				CHECK(cabac.error == 0);
				CHECK(cabac.range == 510U);

				set = 0U;
				if (kind != KIND_I)
					set = 1U + idc;
				for (ctx_idx = 0U; ctx_idx < H264SW_CABAC_CONTEXTS; ctx_idx++) {
					expected = 0U;
					if (ctx_idx != H264SW_CABAC_TERMINATE && listed(kind, ctx_idx)) {
						cabacwriter_context_set(&reference, ctx_idx,
						    h264sw_cabac_init_mn[ctx_idx][set][0],
						    h264sw_cabac_init_mn[ctx_idx][set][1],
						    qp);
						expected = reference.state[ctx_idx];
					}
					CHECK(cabac.state[ctx_idx] == expected);
				}
			}
		}
	}

	/* An I slice ignores cabac_init_idc, even a value that P and B slices refuse. */
	status = engine_start(&cabac, data, sizeof(data), KIND_I, 7U, 26);
	CHECK(status == 0);

	/* SliceQPY outside 0..51 gives the state of the nearest end. */
	status = engine_start(&cabac, data, sizeof(data), KIND_P, 1U, -1000);
	CHECK(status == 0);
	CHECK(cabac.state[11] == 38U);
	status = engine_start(&cabac, data, sizeof(data), KIND_P, 1U, 100000);
	CHECK(status == 0);
	CHECK(cabac.state[11] == (64U | 31U));
}

/* Checks streams without I_PCM, read back bin by bin. */
static void
check_roundtrip(
	void)
{
	unsigned kind;
	unsigned idc;
	unsigned qp_index;
	unsigned count;
	unsigned prefix_bits;
	unsigned operations;
	unsigned state_idx;
	unsigned column;
	unsigned lps_missing;
	unsigned pairs_seen;
	size_t bytes;
	uint32_t pcm_seed;

	/* Every slice type and QP, with two of the three cabac_init_idc for P and B slices, and 24000 operations each. */
	operations = 0U;
	prefix_bits = 0U;
	for (kind = 0U; kind < KINDS; kind++) {
		for (idc = 0U; idc < (kind == KIND_I ? 1U : 3U); idc++) {
			for (qp_index = 0U; qp_index < QP_COUNT; qp_index++) {
				random_state = 0x9e3779b9U + kind * 7919U + idc * 104729U + qp_index * 1299709U;
				count = make_ops(kind, OPS_MAX, 0U);
				pcm_seed = 0U;

				/* The slice header before the data has from 0 to 7 bits, so every byte alignment is met. */
				prefix_bits = (prefix_bits + 3U) % 8U;
				bytes = encode_ops(kind, idc, stream_qp[qp_index], count, pcm_seed, prefix_bits);
				CHECK(written.overflow == 0);
				decode_and_compare(kind, idc, stream_qp[qp_index], count, pcm_seed, prefix_bits, bytes);
				operations += count;
			}
		}
	}
	CHECK(operations >= 100000U);

	/* Every pStateIdx from 0 to 62 was left by a least probable symbol, and the table rows and columns were all met. */
	lps_missing = 0U;
	pairs_seen = 0U;
	for (state_idx = 0U; state_idx < 63U; state_idx++) {
		if (seen_lps_from[state_idx] == 0U)
			lps_missing++;
		for (column = 0U; column < 4U; column++) {
			if (seen_pairs[state_idx][column] != 0U)
				pairs_seen++;
		}
	}
	printf("coverage: %u of 63 states left by an LPS, %u of 252 (pStateIdx, qCodIRangeIdx) pairs decoded\n", 63U - lps_missing, pairs_seen);
	CHECK(lps_missing == 0U);
	CHECK(pairs_seen >= 240U);
}

/* Checks I_PCM samples in the middle of streams. */
static void
check_pcm(
	void)
{
	unsigned round;
	unsigned kind;
	unsigned idc;
	unsigned count;
	unsigned prefix_bits;
	unsigned pcm_ops;
	unsigned index;
	size_t bytes;
	uint32_t pcm_seed;

	/* Many short streams, so that the end of the arithmetic decoding meets every bit position of a byte. */
	pcm_ops = 0U;
	for (round = 0U; round < 400U; round++) {
		random_state = (0x13198a2eU + round * 2654435761U) | 1U;
		kind = round % KINDS;
		idc = (round / KINDS) % 3U;
		if (kind == KIND_I)
			idc = 0U;
		prefix_bits = round % 8U;
		pcm_seed = random_next();

		/* From 1 to 40 operations with a fair share of I_PCM macroblocks, back to back ones among them. */
		count = make_ops(kind, 1U + (random_next() % 40U), 30U);
		bytes = encode_ops(kind, idc, 20 + (int)(round % 20U), count, pcm_seed, prefix_bits);
		CHECK(written.overflow == 0);
		decode_and_compare(kind, idc, 20 + (int)(round % 20U), count, pcm_seed, prefix_bits, bytes);
		for (index = 0U; index < count; index++) {
			if (ops[index].type == OP_PCM)
				pcm_ops++;
		}
	}
	CHECK(pcm_ops >= 1000U);

	/* An I_PCM macroblock first in the slice and nothing else: terminate 1 right after the start. */
	random_state = 0x2545f491U;
	ops[0].type = OP_PCM;
	ops[0].ctx = 0U;
	ops[0].bin = 0U;
	bytes = encode_ops(KIND_I, 0U, 26, 1U, 77U, 0U);
	decode_and_compare(KIND_I, 0U, 26, 1U, 77U, 0U, bytes);
}

/* Checks the error cases. */
static void
check_errors(
	void)
{
	static const uint8_t zeros[2] = { 0x00U, 0x00U };
	static const uint8_t forbidden_510[2] = { 0xffU, 0x00U };
	static const uint8_t forbidden_511[2] = { 0xffU, 0x80U };
	static const uint8_t allowed_509[2] = { 0xfeU, 0x80U };
	static const uint8_t alignment_good[3] = { 0x1fU, 0x00U, 0x00U };
	static const uint8_t alignment_bad[3] = { 0x1eU, 0x00U, 0x00U };
	static const uint8_t one_byte[1] = { 0x00U };
	static const uint8_t pcm_short[4] = { 0x00U, 0x00U, 0x00U, 0x00U };
	uint8_t samples[PCM_BYTES];
	struct h264sw_cabac cabac;
	struct h264sw_bits bits;
	unsigned index;
	unsigned bin;
	int status;

	/* A read past the end: 9 bits are taken at the start, so 7 more of 2 bytes are fine and the 8th is an error. */
	status = engine_start(&cabac, zeros, sizeof(zeros), KIND_I, 0U, 26);
	CHECK(status == 0);
	for (index = 0U; index < 7U; index++) {
		bin = h264sw_cabac_bypass(&cabac);
		CHECK(bin == 0U);
		CHECK(cabac.error == 0);
	}
	bin = h264sw_cabac_bypass(&cabac);
	CHECK(bin == 0U);
	CHECK(cabac.error != 0);

	/* Going on after the error stays defined. */
	for (index = 0U; index < 100U; index++) {
		(void)h264sw_cabac_decision(&cabac, 3U);
		(void)h264sw_cabac_bypass(&cabac);
		(void)h264sw_cabac_terminate(&cabac);
	}
	CHECK(cabac.error != 0);

	/* A decision that renormalizes past the end sets the error too. */
	status = engine_start(&cabac, zeros, sizeof(zeros), KIND_I, 0U, 26);
	CHECK(status == 0);
	for (index = 0U; index < 3000U; index++)
		(void)h264sw_cabac_decision(&cabac, 3U);
	CHECK(cabac.error != 0);

	/* codIOffset 510 and 511 at the start are refused, 509 is not. */
	status = engine_start(&cabac, forbidden_510, sizeof(forbidden_510), KIND_I, 0U, 26);
	CHECK(status == EINVAL);
	CHECK(cabac.error != 0);
	status = engine_start(&cabac, forbidden_511, sizeof(forbidden_511), KIND_I, 0U, 26);
	CHECK(status == EINVAL);
	status = engine_start(&cabac, allowed_509, sizeof(allowed_509), KIND_I, 0U, 26);
	CHECK(status == 0);

	/* Data too short for the 9 bits, or none at all. */
	status = engine_start(&cabac, one_byte, sizeof(one_byte), KIND_I, 0U, 26);
	CHECK(status == EINVAL);
	status = engine_start(&cabac, NULL, 0U, KIND_I, 0U, 26);
	CHECK(status == EINVAL);

	/* The alignment bits must be 1: 3 bits are read, then 1 1 1 1 1 is good and 1 1 1 1 0 is not. */
	h264sw_bits_init(&bits, alignment_good, sizeof(alignment_good));
	(void)h264sw_bits_u(&bits, 3U);
	status = h264sw_cabac_start(&cabac, &bits, H264SW_SLICE_I, 0U, 26);
	CHECK(status == 0);
	h264sw_bits_init(&bits, alignment_bad, sizeof(alignment_bad));
	(void)h264sw_bits_u(&bits, 3U);
	status = h264sw_cabac_start(&cabac, &bits, H264SW_SLICE_I, 0U, 26);
	CHECK(status == EINVAL);

	/* A reader that already failed is refused. */
	h264sw_bits_init(&bits, zeros, sizeof(zeros));
	(void)h264sw_bits_u(&bits, 24U);
	CHECK(bits.error != 0);
	status = h264sw_cabac_start(&cabac, &bits, H264SW_SLICE_I, 0U, 26);
	CHECK(status == EINVAL);

	/* A slice type or cabac_init_idc outside the ranges. */
	h264sw_bits_init(&bits, zeros, sizeof(zeros));
	status = h264sw_cabac_start(&cabac, &bits, H264SW_SLICE_SP, 0U, 26);
	CHECK(status == EINVAL);
	h264sw_bits_init(&bits, zeros, sizeof(zeros));
	status = h264sw_cabac_start(&cabac, &bits, H264SW_SLICE_SI, 0U, 26);
	CHECK(status == EINVAL);
	h264sw_bits_init(&bits, zeros, sizeof(zeros));
	status = h264sw_cabac_start(&cabac, &bits, 9U, 0U, 26);
	CHECK(status == EINVAL);
	h264sw_bits_init(&bits, zeros, sizeof(zeros));
	status = h264sw_cabac_start(&cabac, &bits, H264SW_SLICE_P, 3U, 26);
	CHECK(status == EINVAL);
	h264sw_bits_init(&bits, zeros, sizeof(zeros));
	status = h264sw_cabac_start(&cabac, &bits, H264SW_SLICE_B, 0xffffffffU, 26);
	CHECK(status == EINVAL);

	/* A context outside the variables kept, or the terminate context, is a caller's mistake: 0 and error. */
	status = engine_start(&cabac, zeros, sizeof(zeros), KIND_I, 0U, 26);
	CHECK(status == 0);
	bin = h264sw_cabac_decision(&cabac, H264SW_CABAC_CONTEXTS);
	CHECK(bin == 0U);
	CHECK(cabac.error != 0);
	status = engine_start(&cabac, zeros, sizeof(zeros), KIND_I, 0U, 26);
	CHECK(status == 0);
	bin = h264sw_cabac_decision(&cabac, H264SW_CABAC_TERMINATE);
	CHECK(bin == 0U);
	CHECK(cabac.error != 0);
	status = engine_start(&cabac, zeros, sizeof(zeros), KIND_I, 0U, 26);
	CHECK(status == 0);
	bin = h264sw_cabac_decision(&cabac, 0xffffffffU);
	CHECK(bin == 0U);
	CHECK(cabac.error != 0);

	/* I_PCM: samples that do not fit in the data are refused, and so is an engine that has failed. */
	status = engine_start(&cabac, pcm_short, sizeof(pcm_short), KIND_I, 0U, 26);
	CHECK(status == 0);
	status = h264sw_cabac_pcm(&cabac, samples, PCM_BYTES);
	CHECK(status == EINVAL);
	CHECK(cabac.error != 0);
	status = h264sw_cabac_pcm(&cabac, samples, 1U);
	CHECK(status == EINVAL);
	status = engine_start(&cabac, pcm_short, sizeof(pcm_short), KIND_I, 0U, 26);
	CHECK(status == 0);
	status = h264sw_cabac_pcm(&cabac, samples, (size_t)-1);
	CHECK(status == EINVAL);
	status = engine_start(&cabac, pcm_short, sizeof(pcm_short), KIND_I, 0U, 26);
	CHECK(status == 0);
	status = h264sw_cabac_pcm(&cabac, NULL, 1U);
	CHECK(status == EINVAL);
}

/* Feeds random bytes and random calls. */
static void
check_garbage(
	void)
{
	uint8_t data[40];
	uint8_t samples[64];
	struct h264sw_cabac cabac;
	unsigned round;
	unsigned length;
	unsigned index;
	unsigned operation;
	unsigned kind;
	unsigned idc;
	int status;

	/* Random streams from 0 to 39 bytes, random slice types, QPs and calls, with I_PCM reads of random sizes. */
	random_state = 0xdeadbeefU;
	for (round = 0U; round < 3000U; round++) {
		length = random_next() % 40U;
		for (index = 0U; index < length; index++)
			data[index] = (uint8_t)random_next();
		if ((round % 4U) == 0U) {
			for (index = 0U; index < length; index++)
				data[index] = (uint8_t)(random_next() % 3U == 0U ? 0xffU : 0x00U);
		}
		kind = random_next() % KINDS;
		idc = random_next() % 4U;

		/* The start may be refused; either way the engine is called on, as a careless caller would. */
		status = engine_start(&cabac, data, length, kind, idc, (int)(random_next() % 120U) - 30);
		CHECK(status == 0 || status == EINVAL);
		if (status != 0)
			CHECK(cabac.error != 0);

		/* Random calls, including bad context numbers; the engine must stay defined. */
		for (index = 0U; index < 200U; index++) {
			operation = random_next() % 8U;
			if (operation < 3U) {
				(void)h264sw_cabac_decision(&cabac, random_next() % 500U);
			} else if (operation < 5U) {
				(void)h264sw_cabac_bypass(&cabac);
			} else if (operation < 7U) {
				(void)h264sw_cabac_terminate(&cabac);
			} else {
				(void)h264sw_cabac_pcm(&cabac, samples, random_next() % 64U);
			}
		}
	}
}

/* Runs every check and prints the summary line. */
int
main(
	void)
{
	int result;

	/* Runs the checks in the order of the engine's use: tables, start, decoding, I_PCM, errors. */
	check_tables();
	check_init();
	check_roundtrip();
	check_pcm();
	check_errors();
	check_garbage();

	/* Reports the summary line. */
	result = check_finish("cabac");
	return result;
}
