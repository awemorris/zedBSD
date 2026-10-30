/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CABAC of one slice of the software H.264 decoder: the context
 * variables and their initialization (9.3.1.1); the arithmetic decoding
 * engine is vcodec-cabac.c's.
 */

#include "h264sw-cabac.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "h264sw-cabac-init.h"
#include "h264sw-slice.h"

/* The most ranges of ctxIdx one slice type's list has. */
#define CABAC_RANGES_MAX 3U

/* A first and last ctxIdx, both included. */
struct cabac_range {
	unsigned first;
	unsigned last;
};

/*
 * The ctxIdx that need an initial value in the slices of one type:
 * the number of ranges and the ranges.  ctxIdx 276 is in none of them,
 * because DecodeTerminate serves it.
 */
struct cabac_listed {
	unsigned count;
	struct cabac_range range[CABAC_RANGES_MAX];
};

/* Static function declarations. */
static int cabac_set_for(unsigned slice_type, unsigned cabac_init_idc, unsigned *set);
static void cabac_contexts_init(struct h264sw_cabac *cabac, const struct cabac_listed *listed, unsigned set, int qp);

/*
 * The ctxIdx that I slices initialize.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-11, column I: mb_type 3..10, then every row with an entry for
 * all slice types (60..275 and 277..459).
 */
static const struct cabac_listed cabac_listed_i = {
	3U, { { 3U, 10U }, { 60U, 275U }, { 277U, 459U } }
};

/*
 * The ctxIdx that P slices initialize.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-11, column P, SP: mb_skip_flag, mb_type and sub_mb_type
 * 11..23, mvd and ref_idx 40..59, then every row with an entry for all
 * slice types (60..275 and 277..459).
 */
static const struct cabac_listed cabac_listed_p = {
	3U, { { 11U, 23U }, { 40U, 275U }, { 277U, 459U } }
};

/*
 * The ctxIdx that B slices initialize.
 *
 * Values referenced: ITU-T H.264 (08/2021)
 * Table 9-11, column B: mb_skip_flag, mb_type and sub_mb_type 24..39,
 * mvd and ref_idx 40..59, then every row with an entry for all slice
 * types (60..275 and 277..459).
 */
static const struct cabac_listed cabac_listed_b = {
	2U, { { 24U, 275U }, { 277U, 459U }, { 0U, 0U } }
};

/*
 * Starts CABAC for a slice: the context variables and then the engine (9.3.1).
 *
 * Every failure leaves error set and the engine unusable, so a caller
 * that does not look at the return value still stops at its next check
 * of error.
 */
int
h264sw_cabac_start(
	struct h264sw_cabac *cabac,
	struct vcodec_bitreader *bits,
	unsigned slice_type,
	unsigned cabac_init_idc,
	int qp)
{
	const struct cabac_listed *listed;
	unsigned set;
	unsigned alignment_bit;
	size_t byte;
	int aligned;
	int status;

	/* An engine that was never started reads nothing and reports an error. */
	memset(cabac, 0, sizeof(*cabac));
	cabac->engine.error = EINVAL;

	/* Refuses a reader that is already broken. */
	if (bits->error != 0)
		return EINVAL;

	/* Refuses a slice type or a cabac_init_idc outside the ranges, and chooses the set of initial values. */
	status = cabac_set_for(slice_type, cabac_init_idc, &set);
	if (status != 0)
		return status;

	/* Chooses the ctxIdx the slice type needs. */
	listed = &cabac_listed_i;
	if (slice_type == H264SW_SLICE_P)
		listed = &cabac_listed_p;
	if (slice_type == H264SW_SLICE_B)
		listed = &cabac_listed_b;

	/* Reads cabac_alignment_one_bit up to the byte boundary; each must be 1. */
	aligned = vcodec_bitreader_aligned(bits);
	while (aligned == 0) {
		alignment_bit = vcodec_bitreader_u1(bits);

		/* A read that failed or a bit that is not 1 breaks the syntax. */
		if (bits->error != 0)
			return EINVAL;
		if (alignment_bit != 1U)
			return EINVAL;

		aligned = vcodec_bitreader_aligned(bits);
	}

	/* Refuses data that ends before the 9 bits of codIOffset do. */
	byte = bits->position / 8U;
	if (byte > bits->size)
		return EINVAL;
	if (bits->size - byte < VCODEC_CABAC_START_BYTES)
		return EINVAL;

	/* Initializes the context variables from the (m, n) tables (9.3.1.1). */
	cabac_contexts_init(cabac, listed, set, qp);

	/* Starts the engine on the bytes from the boundary to the end of the RBSP (9.3.1.2). */
	status = vcodec_cabac_start(&cabac->engine, bits->data + byte, bits->size - byte);
	if (status != 0)
		return status;

	/* Succeeded: the engine is ready to decode the first bin. */
	return 0;
}

/*
 * Decodes one bin with context ctx_idx (DecodeDecision, 9.3.3.2.1).
 *
 * The ctxIdx is checked here; the engine decodes with the context's byte.
 */
unsigned
h264sw_cabac_decision(
	struct h264sw_cabac *cabac,
	unsigned ctx_idx)
{
	unsigned bin;

	/* Refuses a context that is not one of the variables kept, or the terminate context. */
	if (ctx_idx >= H264SW_CABAC_CONTEXTS || ctx_idx == H264SW_CABAC_TERMINATE) {
		cabac->engine.error = EINVAL;
		return 0U;
	}

	/* Decodes the bin and updates the context variable's state. */
	bin = vcodec_cabac_decision(&cabac->engine, &cabac->state[ctx_idx]);

	/* Reports the decoded bin. */
	return bin;
}

/*
 * Maps a slice type and cabac_init_idc to the set of initial values of
 * h264sw_cabac_init_mn: 0 for I, 1 + cabac_init_idc for P and B.
 */
static int
cabac_set_for(
	unsigned slice_type,
	unsigned cabac_init_idc,
	unsigned *set)
{
	/* An I slice has one set of initial values and ignores cabac_init_idc. */
	if (slice_type == H264SW_SLICE_I) {
		*set = H264SW_CABAC_INIT_I;
		return 0;
	}

	/* Only I, P and B slices are decoded. */
	if (slice_type != H264SW_SLICE_P && slice_type != H264SW_SLICE_B)
		return EINVAL;

	/* cabac_init_idc is 0 to 2 (7.4.3). */
	if (cabac_init_idc > 2U)
		return EINVAL;

	/* The sets for P and B slices follow the I set. */
	*set = 1U + cabac_init_idc;

	/* Succeeded: the set is chosen. */
	return 0;
}

/*
 * Initializes the context variables of the slice type's list (9.3.1.1);
 * the others stay at pStateIdx 0 and valMPS 0.
 */
static void
cabac_contexts_init(
	struct h264sw_cabac *cabac,
	const struct cabac_listed *listed,
	unsigned set,
	int qp)
{
	unsigned range_index;
	unsigned ctx_idx;

	/* Walks every range of the list. */
	for (range_index = 0U;
	     range_index < listed->count;
	     range_index++) {
		/* Initializes each ctxIdx of the range from its (m, n) pair. */
		for (ctx_idx = listed->range[range_index].first;
		     ctx_idx <= listed->range[range_index].last;
		     ctx_idx++) {
			cabac->state[ctx_idx] = vcodec_cabac_context_init(
				h264sw_cabac_init_mn[ctx_idx][set][0],
				h264sw_cabac_init_mn[ctx_idx][set][1],
				qp);
		}
	}
}
