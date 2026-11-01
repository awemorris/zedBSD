/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Tests of h265-dpb: the reference picture marking of 8.3.2, the output
 * order buffer of C.5.2 and the slots of the Vulkan DPB.
 *
 * The hand scenarios give each picture's order count and reference picture
 * set as a short string and the expected result of each step (the pictures
 * bumped before and after the picture, the reference pictures, the slot,
 * the counts) worked by hand from C.5.2.2 to C.5.2.4 and 8.3.2 (the
 * comment of each scenario gives the working).  The random streams are
 * compared against a second, independently written model of the same
 * clauses (kept in plain arrays, with the choices sorted out by simple
 * scans) and against the invariants of the buffer.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "h265write.h"
#include "userland/desktop/libmedia/h265-dpb.h"

/* The state of the pseudo-random generator, fixed so a failure repeats. */
static uint32_t random_state = 0x1F2E3D4CU;

/* The SPS the tests fill (a large structure, so static), the buffer and the plan under test. */
static struct h265sw_sps sps_store;
static struct h265_dpb dpb;
static struct h265_dpb_plan plan;

/* The order count of the picture stored in each slot, kept by the scenario runner to check the Curr lists. */
static int32_t slot_poc[H265_DPB_MAX_SLOTS];

/* The expected slot a scenario step does not check. */
#define SLOT_ANY (-2)

/* One step of a hand scenario: the picture, and what the step must do. */
struct step {
	const char *pic;
	const char *begin_out;
	const char *end_out;
	const char *refs;
	int slot;
	unsigned missing;
	unsigned evicted;
	unsigned discarded;
};

/* A picture parsed from its string. */
struct pic_spec {
	int32_t poc;
	int irap;
	int noout;
	int output;
	struct h265sw_rps_pocs pocs;
};

/* Returns the next pseudo-random number (xorshift32). */
static uint32_t
random_next(void)
{
	random_state ^= random_state << 13;
	random_state ^= random_state >> 17;
	random_state ^= random_state << 5;
	return random_state;
}

/* Fills the SPS fields the buffer reads, for sub-layer 0 and no sub-layers above. */
static void
sps_make(
	unsigned size,
	unsigned reorder,
	unsigned plus1,
	unsigned log2_max_poc_lsb)
{
	memset(&sps_store, 0, sizeof(sps_store));
	sps_store.max_sub_layers_minus1 = 0U;
	sps_store.log2_max_poc_lsb = log2_max_poc_lsb;
	sps_store.max_dec_pic_buffering_minus1[0] = size - 1U;
	sps_store.max_num_reorder_pics[0] = reorder;
	sps_store.max_latency_increase_plus1[0] = plus1;
}

/*
 * Parses a comma list of integers, each with an optional suffix letter 'm'
 * or 'L' that sets its flag, into values and flags; returns the count.
 */
static unsigned
parse_list(
	const char *text,
	int32_t *values,
	uint8_t *flags,
	unsigned max)
{
	unsigned count;
	int sign;
	int32_t value;

	count = 0U;
	while (*text != '\0' && count < max) {
		sign = 1;
		value = 0;
		if (*text == '-') {
			sign = -1;
			text++;
		}
		while (*text >= '0' && *text <= '9') {
			value = value * 10 + (int32_t)(*text - '0');
			text++;
		}
		flags[count] = 0U;
		if (*text == 'm' || *text == 'L') {
			flags[count] = 1U;
			text++;
		}
		values[count] = sign * value;
		count++;
		if (*text == ',')
			text++;
	}
	return count;
}

/*
 * Parses "poc [I] [!] [n] [b<list>] [a<list>] [f<list>] [L<list>] [F<list>]":
 * I is an IRAP picture with NoRaslOutputFlag 1, ! NoOutputOfPriorPicsFlag 1,
 * n PicOutputFlag 0, b, a and f the short-term lists (before, after, foll),
 * L and F the long-term lists (curr, foll; a value with the suffix m has the
 * msb flag, others give the least significant bits).
 */
static void
parse_pic(
	const char *text,
	struct pic_spec *spec)
{
	char token[64];
	uint8_t flags[H265SW_MAX_RPS];
	unsigned length;
	unsigned first;

	memset(spec, 0, sizeof(*spec));
	spec->output = 1;
	first = 1U;
	while (*text != '\0') {
		while (*text == ' ')
			text++;
		length = 0U;
		while (*text != '\0' && *text != ' ' && length < sizeof(token) - 1U) {
			token[length] = *text;
			length++;
			text++;
		}
		token[length] = '\0';
		if (length == 0U)
			break;
		if (first != 0U) {
			parse_list(token, &spec->poc, flags, 1U);
			first = 0U;
			continue;
		}
		switch (token[0]) {
		case 'I':
			spec->irap = 1;
			break;
		case '!':
			spec->noout = 1;
			break;
		case 'n':
			spec->output = 0;
			break;
		case 'b':
			spec->pocs.num_st_before = parse_list(token + 1, spec->pocs.st_before, flags, H265SW_MAX_RPS);
			break;
		case 'a':
			spec->pocs.num_st_after = parse_list(token + 1, spec->pocs.st_after, flags, H265SW_MAX_RPS);
			break;
		case 'f':
			spec->pocs.num_st_foll = parse_list(token + 1, spec->pocs.st_foll, flags, H265SW_MAX_RPS);
			break;
		case 'L':
			spec->pocs.num_lt_curr = parse_list(token + 1, spec->pocs.lt_curr, spec->pocs.lt_curr_msb, H265SW_MAX_RPS);
			break;
		case 'F':
			spec->pocs.num_lt_foll = parse_list(token + 1, spec->pocs.lt_foll, spec->pocs.lt_foll_msb, H265SW_MAX_RPS);
			break;
		default:
			CHECK(0);
			break;
		}
	}
}

/* Writes the order counts of outputs as "a,b,c" (empty for none). */
static void
format_outputs(
	const struct h265_dpb_output *output,
	unsigned count,
	char *text)
{
	unsigned index;
	size_t length;

	text[0] = '\0';
	length = 0U;
	for (index = 0U;
	     index < count;
	     index++) {
		if (index != 0U) {
			text[length] = ',';
			length++;
			text[length] = '\0';
		}
		length += (size_t)sprintf(text + length, "%ld", (long)output[index].poc);
	}
}

/*
 * Writes the reference pictures of a plan as "a,bL,c": ascending order
 * counts, a trailing L for a long-term reference.
 */
static void
format_refs(
	const struct h265_dpb_plan *p,
	char *text)
{
	unsigned order[H265_DPB_PICTURES];
	unsigned index;
	unsigned other;
	unsigned swap;
	size_t length;

	for (index = 0U;
	     index < p->reference_count;
	     index++)
		order[index] = index;
	for (index = 1U;
	     index < p->reference_count;
	     index++) {
		for (other = index;
		     other > 0U && p->reference_poc[order[other - 1U]] > p->reference_poc[order[other]];
		     other--) {
			swap = order[other];
			order[other] = order[other - 1U];
			order[other - 1U] = swap;
		}
	}
	text[0] = '\0';
	length = 0U;
	for (index = 0U;
	     index < p->reference_count;
	     index++) {
		if (index != 0U) {
			text[length] = ',';
			length++;
		}
		length += (size_t)sprintf(text + length, "%ld", (long)p->reference_poc[order[index]]);
		if (p->reference_long[order[index]] != 0U) {
			text[length] = 'L';
			length++;
		}
		text[length] = '\0';
	}
}

/* Checks that two strings are equal, printing both when they are not. */
static void
check_string(
	const char *label,
	unsigned step,
	const char *got,
	const char *want)
{
	if (strcmp(got, want) != 0)
		printf("MISMATCH %s step %u: got \"%s\" want \"%s\"\n", label, step, got, want);
	CHECK(strcmp(got, want) == 0);
}

/*
 * Checks the invariants of a buffer: every used entry holds a distinct slot
 * inside the slots, an empty entry is all zero with no slot, markings and
 * counts are in range, and the busy mask stays inside the slots.
 */
static void
check_state(
	const struct h265_dpb *d)
{
	uint32_t seen;
	unsigned index;
	unsigned used;

	CHECK(d->slots >= H265_DPB_PICTURES);
	CHECK(d->slots <= H265_DPB_MAX_SLOTS);
	if (d->slots < H265_DPB_MAX_SLOTS)
		CHECK((d->busy >> d->slots) == 0U);
	seen = 0U;
	used = 0U;
	for (index = 0U;
	     index < H265_DPB_PICTURES;
	     index++) {
		if (d->entry[index].used == 0) {
			CHECK(d->entry[index].marking == 0U);
			CHECK(d->entry[index].needed == 0);
			CHECK(d->entry[index].latency == 0U);
			CHECK(d->entry[index].poc == 0);
			CHECK(d->entry[index].slot == H265_DPB_NO_SLOT);
			CHECK(d->entry[index].id == 0U);
			continue;
		}
		used++;
		CHECK(d->entry[index].used == 1);
		CHECK(d->entry[index].marking <= H265_DPB_LONG);
		CHECK(d->entry[index].needed == 0 || d->entry[index].needed == 1);
		CHECK(d->entry[index].slot >= 0);
		CHECK((unsigned)d->entry[index].slot < d->slots);
		if (d->entry[index].slot >= 0 && (unsigned)d->entry[index].slot < d->slots) {
			CHECK((seen & ((uint32_t)1U << d->entry[index].slot)) == 0U);
			seen |= (uint32_t)1U << d->entry[index].slot;
		}
	}
	CHECK(used <= H265_DPB_PICTURES);
}

/*
 * Checks the reference lists of a plan against the buffer: each listed slot
 * holds a used reference entry of the same order count and marking, the
 * slots are distinct, the picture's own slot is not among them, and every
 * reference entry is listed.
 */
static void
check_plan_refs(
	const struct h265_dpb *d,
	const struct h265_dpb_plan *p)
{
	unsigned index;
	unsigned other;
	unsigned listed;
	unsigned found;

	CHECK(p->reference_count <= H265_DPB_PICTURES - 1U);
	for (index = 0U;
	     index < p->reference_count && index < H265_DPB_PICTURES;
	     index++) {
		found = 0U;
		for (other = 0U;
		     other < H265_DPB_PICTURES;
		     other++) {
			if (d->entry[other].used == 0)
				continue;
			if (d->entry[other].slot != p->reference_slot[index])
				continue;
			found++;
			CHECK(d->entry[other].marking != H265_DPB_UNUSED);
			CHECK(d->entry[other].poc == p->reference_poc[index]);
			CHECK((d->entry[other].marking == H265_DPB_LONG) == (p->reference_long[index] != 0U));
		}
		CHECK(found == 1U);
		CHECK(p->reference_slot[index] != p->setup_slot);
		for (other = index + 1U;
		     other < p->reference_count && other < H265_DPB_PICTURES;
		     other++)
			CHECK(p->reference_slot[other] != p->reference_slot[index]);
	}
	listed = 0U;
	for (index = 0U;
	     index < H265_DPB_PICTURES;
	     index++) {
		if (d->entry[index].used != 0 && d->entry[index].marking != H265_DPB_UNUSED)
			listed++;
	}
	CHECK(listed == p->reference_count);
}

/* Releases the slots of outputs. */
static void
release_outputs(
	const struct h265_dpb_output *output,
	unsigned count)
{
	unsigned index;

	for (index = 0U;
	     index < count;
	     index++)
		h265_dpb_release(&dpb, output[index].slot);
}

/*
 * Checks one Curr list of a plan against the requested order counts: a
 * listed slot holds the picture the entry names (for a long-term entry
 * without the msb flag, one with the same least significant bits), and the
 * number of entries with no slot is added to *missing.
 */
static void
check_curr_list(
	const int *slots,
	const int32_t *pocs,
	const uint8_t *msb,
	unsigned count,
	unsigned log2_max_poc_lsb,
	unsigned *missing)
{
	unsigned index;
	int64_t modulus;
	int64_t want;
	int64_t got;

	modulus = (int64_t)1 << log2_max_poc_lsb;
	for (index = 0U;
	     index < count;
	     index++) {
		if (slots[index] == H265_DPB_NO_SLOT) {
			*missing += 1U;
			continue;
		}
		CHECK(slots[index] >= 0 && (unsigned)slots[index] < H265_DPB_MAX_SLOTS);
		if (slots[index] < 0 || (unsigned)slots[index] >= H265_DPB_MAX_SLOTS)
			continue;
		want = pocs[index];
		got = slot_poc[slots[index]];
		if (msb != NULL && msb[index] == 0U) {
			got = ((got % modulus) + modulus) % modulus;
			want = ((want % modulus) + modulus) % modulus;
		}
		CHECK(got == want);
	}
}

/*
 * Runs a hand scenario: for each step begins the picture, takes its slot,
 * ends it, and compares the step's results with the expected strings and
 * counts; release 1 releases every output's slot at the end of each step.
 */
static void
run_scenario(
	const char *name,
	unsigned slots,
	unsigned size,
	unsigned reorder,
	unsigned plus1,
	int release,
	const struct step *steps,
	unsigned count)
{
	struct pic_spec spec;
	struct h265_dpb_output end_output[H265_DPB_MAX_OUTPUT];
	unsigned end_count;
	unsigned index;
	unsigned missing;
	char text[256];
	char want_name[64];
	int rc;

	h265_dpb_init(&dpb, slots);
	sps_make(size, reorder, plus1, 4U);
	memset(slot_poc, 0, sizeof(slot_poc));
	for (index = 0U;
	     index < count;
	     index++) {
		sprintf(want_name, "%s begin", name);
		parse_pic(steps[index].pic, &spec);
		rc = h265_dpb_begin(&dpb, &sps_store, &spec.pocs, spec.irap, spec.noout, &plan);
		CHECK(rc == 0);
		if (rc != 0)
			return;
		CHECK(plan.reset == (index == 0U));
		CHECK(plan.setup_slot == H265_DPB_NO_SLOT);
		CHECK(dpb.in_picture == 1);
		format_outputs(plan.output, plan.output_count, text);
		check_string(want_name, index, text, steps[index].begin_out);
		CHECK(plan.discarded == steps[index].discarded);
		CHECK(plan.evicted == steps[index].evicted);
		CHECK(plan.missing == steps[index].missing);
		missing = 0U;
		check_curr_list(plan.st_before, spec.pocs.st_before, NULL, plan.num_st_before, 4U, &missing);
		check_curr_list(plan.st_after, spec.pocs.st_after, NULL, plan.num_st_after, 4U, &missing);
		check_curr_list(plan.lt_curr, spec.pocs.lt_curr, spec.pocs.lt_curr_msb, plan.num_lt_curr, 4U, &missing);
		CHECK(plan.num_st_before == spec.pocs.num_st_before);
		CHECK(plan.num_st_after == spec.pocs.num_st_after);
		CHECK(plan.num_lt_curr == spec.pocs.num_lt_curr);
		CHECK(missing == plan.missing);
		format_refs(&plan, text);
		check_string(name, index, text, steps[index].refs);
		check_state(&dpb);
		rc = h265_dpb_take_slot(&dpb, &plan);
		CHECK(rc == 0);
		if (rc != 0)
			return;
		if (steps[index].slot != SLOT_ANY)
			CHECK(plan.setup_slot == steps[index].slot);
		check_plan_refs(&dpb, &plan);
		slot_poc[plan.setup_slot] = spec.poc;
		end_count = 99U;
		rc = h265_dpb_end(&dpb, &plan, spec.poc, spec.output, (uint64_t)index, end_output, &end_count);
		CHECK(rc == 0);
		if (rc != 0)
			return;
		sprintf(want_name, "%s end", name);
		format_outputs(end_output, end_count, text);
		check_string(want_name, index, text, steps[index].end_out);
		check_state(&dpb);
		if (release != 0) {
			release_outputs(plan.output, plan.output_count);
			release_outputs(end_output, end_count);
		}
	}
}

/*
 * IDR pictures only (size 1 and 2, reorder 0): every picture is output at
 * once.  Each IDR marks the previous picture unused, which the removal then
 * empties, so with the slots released the same slot is used again, and
 * without release the busy slot is skipped.
 */
static void
check_idr_only(void)
{
	static const struct step released[] = {
		{ "0 I", "", "0", "", 0, 0U, 0U, 0U },
		{ "0 I", "", "0", "", 0, 0U, 0U, 0U },
		{ "0 I", "", "0", "", 0, 0U, 0U, 0U },
	};
	static const struct step held[] = {
		{ "0 I", "", "0", "", 0, 0U, 0U, 0U },
		{ "0 I", "", "0", "", 1, 0U, 0U, 0U },
		{ "0 I", "", "0", "", 2, 0U, 0U, 0U },
	};

	run_scenario("idr size1 released", 16U, 1U, 0U, 0U, 1, released, 3U);
	run_scenario("idr size2 released", 16U, 2U, 0U, 0U, 1, released, 3U);
	run_scenario("idr size1 held", 16U, 1U, 0U, 0U, 0, held, 3U);
	run_scenario("idr size2 held", 17U, 2U, 0U, 0U, 0, held, 3U);
}

/*
 * I P P P with one short-term reference (reorder 0, size 2).  Each picture is
 * output at once; picture n keeps only n - 1, so the entry of n - 2 is
 * emptied at the begin of n.  Slots with release: 0, 1, then the slot of the
 * emptied entry again (0, 1).  Without release the busy slots are skipped:
 * 0, 1, 2, 3.
 */
static void
check_ippp(void)
{
	static const struct step released[] = {
		{ "0 I", "", "0", "", 0, 0U, 0U, 0U },
		{ "1 b0", "", "1", "0", 1, 0U, 0U, 0U },
		{ "2 b1", "", "2", "1", 0, 0U, 0U, 0U },
		{ "3 b2", "", "3", "2", 1, 0U, 0U, 0U },
		{ "4 b3", "", "4", "3", 0, 0U, 0U, 0U },
	};
	static const struct step held[] = {
		{ "0 I", "", "0", "", 0, 0U, 0U, 0U },
		{ "1 b0", "", "1", "0", 1, 0U, 0U, 0U },
		{ "2 b1", "", "2", "1", 2, 0U, 0U, 0U },
		{ "3 b2", "", "3", "2", 3, 0U, 0U, 0U },
	};

	run_scenario("ippp released", 16U, 2U, 0U, 0U, 1, released, 5U);
	run_scenario("ippp held", 16U, 2U, 0U, 0U, 0, held, 4U);
}

/*
 * A hierarchical B group of 8 (decode order 0 8 4 2 1 3 6 5 7, then the next
 * group 16 12 10 9).  The reference picture sets are written out per picture;
 * the pictures an RPS drops stay in the buffer while they wait for output.
 * The expected outputs are worked from C.5.2.3's condition "more pictures
 * waiting than reorder" for reorder 3, 4 and 2 (the last is a stream that
 * breaks the order: its reorder is too small, and the output follows the
 * rules anyway).  The reference lists do not depend on the outputs.
 */
static void
check_hierarchical(void)
{
	static const struct step reorder3[] = {
		{ "0 I", "", "", "", SLOT_ANY, 0U, 0U, 0U },
		{ "8 b0", "", "", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "4 b0 a8", "", "", "0,8", SLOT_ANY, 0U, 0U, 0U },
		{ "2 b0 a4,8", "", "0", "0,4,8", SLOT_ANY, 0U, 0U, 0U },
		{ "1 b0 a2 f4,8", "", "1", "0,2,4,8", SLOT_ANY, 0U, 0U, 0U },
		{ "3 b2 a4 f8", "", "2", "2,4,8", SLOT_ANY, 0U, 0U, 0U },
		{ "6 b4 a8", "", "3", "4,8", SLOT_ANY, 0U, 0U, 0U },
		{ "5 b4 a6 f8", "", "4", "4,6,8", SLOT_ANY, 0U, 0U, 0U },
		{ "7 b6 a8", "", "5", "6,8", SLOT_ANY, 0U, 0U, 0U },
		{ "16 b8", "", "6", "8", SLOT_ANY, 0U, 0U, 0U },
		{ "12 b8 a16", "", "7", "8,16", SLOT_ANY, 0U, 0U, 0U },
		{ "10 b8 a12,16", "", "8", "8,12,16", SLOT_ANY, 0U, 0U, 0U },
		{ "9 b8 a10 f12,16", "", "9", "8,10,12,16", SLOT_ANY, 0U, 0U, 0U },
	};
	static const struct step reorder4[] = {
		{ "0 I", "", "", "", SLOT_ANY, 0U, 0U, 0U },
		{ "8 b0", "", "", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "4 b0 a8", "", "", "0,8", SLOT_ANY, 0U, 0U, 0U },
		{ "2 b0 a4,8", "", "", "0,4,8", SLOT_ANY, 0U, 0U, 0U },
		{ "1 b0 a2 f4,8", "", "0", "0,2,4,8", SLOT_ANY, 0U, 0U, 0U },
		{ "3 b2 a4 f8", "", "1", "2,4,8", SLOT_ANY, 0U, 0U, 0U },
		{ "6 b4 a8", "", "2", "4,8", SLOT_ANY, 0U, 0U, 0U },
		{ "5 b4 a6 f8", "", "3", "4,6,8", SLOT_ANY, 0U, 0U, 0U },
		{ "7 b6 a8", "", "4", "6,8", SLOT_ANY, 0U, 0U, 0U },
		{ "16 b8", "", "5", "8", SLOT_ANY, 0U, 0U, 0U },
		{ "12 b8 a16", "", "6", "8,16", SLOT_ANY, 0U, 0U, 0U },
		{ "10 b8 a12,16", "", "7", "8,12,16", SLOT_ANY, 0U, 0U, 0U },
		{ "9 b8 a10 f12,16", "", "8", "8,10,12,16", SLOT_ANY, 0U, 0U, 0U },
	};
	static const struct step reorder2[] = {
		{ "0 I", "", "", "", SLOT_ANY, 0U, 0U, 0U },
		{ "8 b0", "", "", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "4 b0 a8", "", "0", "0,8", SLOT_ANY, 0U, 0U, 0U },
		{ "2 b0 a4,8", "", "2", "0,4,8", SLOT_ANY, 0U, 0U, 0U },
		{ "1 b0 a2 f4,8", "", "1", "0,2,4,8", SLOT_ANY, 0U, 0U, 0U },
		{ "3 b2 a4 f8", "", "3", "2,4,8", SLOT_ANY, 0U, 0U, 0U },
		{ "6 b4 a8", "", "4", "4,8", SLOT_ANY, 0U, 0U, 0U },
		{ "5 b4 a6 f8", "", "5", "4,6,8", SLOT_ANY, 0U, 0U, 0U },
		{ "7 b6 a8", "", "6", "6,8", SLOT_ANY, 0U, 0U, 0U },
		{ "16 b8", "", "7", "8", SLOT_ANY, 0U, 0U, 0U },
		{ "12 b8 a16", "", "8", "8,16", SLOT_ANY, 0U, 0U, 0U },
		{ "10 b8 a12,16", "", "10", "8,12,16", SLOT_ANY, 0U, 0U, 0U },
		{ "9 b8 a10 f12,16", "", "9", "8,10,12,16", SLOT_ANY, 0U, 0U, 0U },
	};

	run_scenario("hier r3 s5", 16U, 5U, 3U, 0U, 1, reorder3, 13U);
	run_scenario("hier r3 s6", 16U, 6U, 3U, 0U, 1, reorder3, 13U);
	run_scenario("hier r3 s5 held", 16U, 5U, 3U, 0U, 0, reorder3, 13U);
	run_scenario("hier r4 s7", 16U, 7U, 4U, 0U, 1, reorder4, 13U);
	run_scenario("hier r2 s5", 20U, 5U, 2U, 0U, 1, reorder2, 13U);
}

/*
 * The latency condition.  Reorder 3, size 8.  Picture 100 is decoded early
 * and waits; every later picture that is output and has a smaller order
 * count adds 1 to its PicLatencyCount.
 *
 * With sps_max_latency_increase_plus1 1 (SpsMaxLatencyPictures 3 + 1 - 1 =
 * 3): the count of 100 is 1 after picture 1, 2 after picture 2, 3 after
 * picture 3.  After picture 2 four pictures wait (0 100 1 2), which bumps 0;
 * after picture 3 four wait (100 1 2 3), which bumps 1, and then, with 100
 * at count 3 >= 3, the latency condition bumps 2, 3 and 100 (smallest
 * order count first, though 100 is the one at the limit).
 * With plus1 2 (limit 4) picture 3 bumps only 1, and picture 4 (count of
 * 100 now 4) bumps 2 and then 3, 4 and 100.  With plus1 0 (no limit) the
 * pictures are bumped one at a time.
 */
static void
check_latency(void)
{
	static const struct step limit3[] = {
		{ "0 I", "", "", "", SLOT_ANY, 0U, 0U, 0U },
		{ "100 b0", "", "", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "1 b0", "", "", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "2 b0", "", "0", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "3 b0", "", "1,2,3,100", "0", SLOT_ANY, 0U, 0U, 0U },
	};
	static const struct step limit4[] = {
		{ "0 I", "", "", "", SLOT_ANY, 0U, 0U, 0U },
		{ "100 b0", "", "", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "1 b0", "", "", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "2 b0", "", "0", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "3 b0", "", "1", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "4 b0", "", "2,3,4,100", "0", SLOT_ANY, 0U, 0U, 0U },
	};
	static const struct step nolimit[] = {
		{ "0 I", "", "", "", SLOT_ANY, 0U, 0U, 0U },
		{ "100 b0", "", "", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "1 b0", "", "", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "2 b0", "", "0", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "3 b0", "", "1", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "4 b0", "", "2", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "5 b0", "", "3", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "6 b0", "", "4", "0", SLOT_ANY, 0U, 0U, 0U },
	};
	static const struct step nooutput[] = {
		{ "0 I", "", "", "", SLOT_ANY, 0U, 0U, 0U },
		{ "100 b0", "", "", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "1 b0", "", "", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "2 b0 n", "", "", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "3 b0", "", "0", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "4 b0", "", "1,3,4,100", "0", SLOT_ANY, 0U, 0U, 0U },
	};

	run_scenario("latency 3", 16U, 8U, 3U, 1U, 1, limit3, 5U);
	run_scenario("latency 4", 16U, 8U, 3U, 2U, 1, limit4, 6U);
	run_scenario("latency none", 16U, 8U, 3U, 0U, 1, nolimit, 8U);
	run_scenario("latency not-output", 16U, 8U, 3U, 1U, 1, nooutput, 6U);
}

/*
 * The buffer-full condition of C.5.2.2.  Size 3, reorder 4, so only the
 * size can bump.  After pictures 0, 8, 4 the buffer holds three pictures;
 * picture 2 keeps only 0, so 8 and 4 are unused but wait for output.  At
 * its begin three entries are held (3 >= size 3): bump 0 (a reference, the
 * entry stays, still 3), bump 4 (unused, emptied, now 2 < 3).  Picture 1
 * (before 0, after 2) finds 8 and 2 waiting and the buffer at 3 again: bump
 * 2 (a reference, stays), bump 8 (unused, emptied).
 */
static void
check_size_bump(void)
{
	static const struct step steps[] = {
		{ "0 I", "", "", "", SLOT_ANY, 0U, 0U, 0U },
		{ "8 b0", "", "", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "4 b0 a8", "", "", "0,8", SLOT_ANY, 0U, 0U, 0U },
		{ "2 b0", "0,4", "", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "1 b0 a2", "2,8", "", "0,2", SLOT_ANY, 0U, 0U, 0U },
		{ "3 b1", "", "", "1", SLOT_ANY, 0U, 0U, 0U },
	};

	run_scenario("size bump", 16U, 3U, 4U, 0U, 1, steps, 6U);
}

/*
 * A broken stream whose references fill a buffer that is too small.  Size 3,
 * reorder 4: picture 2 uses 0 and 4 and keeps 8.  At its begin 3 >= 3, so
 * 0, 4 and 8 are all bumped (each stays, being a reference), the buffer is
 * still full and nothing waits, so the entry with the smallest order count
 * that is in no Curr list is emptied: 8 (Foll), counted in evicted.  The
 * slot 3 is the lowest not held and not busy (the outputs made 0, 2 and 1
 * busy).  The second scenario evicts the smaller of two Foll pictures.
 */
static void
check_evict(void)
{
	static const struct step steps[] = {
		{ "0 I", "", "", "", 0, 0U, 0U, 0U },
		{ "8 b0", "", "", "0", 1, 0U, 0U, 0U },
		{ "4 b0 a8", "", "", "0,8", 2, 0U, 0U, 0U },
		{ "2 b0 a4 f8", "0,4,8", "", "0,4", 3, 0U, 1U, 0U },
	};
	static const struct step two[] = {
		{ "0 I", "", "", "", SLOT_ANY, 0U, 0U, 0U },
		{ "8 b0", "", "", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "12 b0 f8", "", "", "0,8", SLOT_ANY, 0U, 0U, 0U },
		{ "4 b0 a8,12", "", "", "0,8,12", SLOT_ANY, 0U, 0U, 0U },
		{ "2 b0 a4 f8,12", "0,4,8,12", "", "0,4,12", SLOT_ANY, 0U, 1U, 0U },
	};

	run_scenario("evict", 16U, 3U, 4U, 0U, 0, steps, 4U);
	run_scenario("evict smallest", 16U, 4U, 4U, 0U, 1, two, 5U);
}

/*
 * Long-term references (log2_max_poc_lsb 4, MaxPicOrderCntLsb 16).
 *
 * A: lsb-only matches.  Picture 0 becomes long-term when picture 2 names
 * lsb 0 (it was short-term).  Picture 3 keeps it through a Foll entry; so
 * does picture 16, whose own lsb is also 0 but which is not yet in the
 * buffer.  Picture 17 asks again for lsb 0 as a Curr entry and also names 16
 * as short-term: two pictures match the lsb (0 and 16), the greatest id (16)
 * is taken, so 16 is long-term, the short-term entry 16 finds no picture
 * (the picture is already chosen, 8.3.2 NOTE 5: missing 1) and picture 0 is
 * dropped.
 * B: the same with the msb flag, so picture 0 is found by its whole order
 * count and 16 stays short-term.
 * C: lsb matching of a negative order count (-3 has lsb 13), then a whole
 * count match of a picture that is already long-term.
 * D: a waiting but unused picture is not a reference picture, so a long-term
 * entry does not find it.
 */
static void
check_long_term(void)
{
	static const struct step lsb[] = {
		{ "0 I", "", "0", "", SLOT_ANY, 0U, 0U, 0U },
		{ "1 b0", "", "1", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "2 b1 L0", "", "2", "0L,1", SLOT_ANY, 0U, 0U, 0U },
		{ "3 b2 f1 F0", "", "3", "0L,1,2", SLOT_ANY, 0U, 0U, 0U },
		{ "16 b3 F0", "", "16", "0L,3", SLOT_ANY, 0U, 0U, 0U },
		{ "17 b16 L0", "", "17", "16L", SLOT_ANY, 1U, 0U, 0U },
	};
	static const struct step msb[] = {
		{ "0 I", "", "0", "", SLOT_ANY, 0U, 0U, 0U },
		{ "1 b0", "", "1", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "2 b1 L0", "", "2", "0L,1", SLOT_ANY, 0U, 0U, 0U },
		{ "3 b2 f1 F0", "", "3", "0L,1,2", SLOT_ANY, 0U, 0U, 0U },
		{ "16 b3 F0", "", "16", "0L,3", SLOT_ANY, 0U, 0U, 0U },
		{ "17 b16 L0m", "", "17", "0L,16", SLOT_ANY, 0U, 0U, 0U },
	};
	static const struct step negative[] = {
		{ "0 I", "", "0", "", SLOT_ANY, 0U, 0U, 0U },
		{ "-3 a0", "", "-3", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "5 b0 L13", "", "5", "-3L,0", SLOT_ANY, 0U, 0U, 0U },
		{ "6 b5 L-3m", "", "6", "-3L,5", SLOT_ANY, 0U, 0U, 0U },
	};
	static const struct step unused_waiting[] = {
		{ "0 I", "", "", "", SLOT_ANY, 0U, 0U, 0U },
		{ "8 b0", "", "", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "4 b0", "", "", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "6 L8m", "", "0", "", SLOT_ANY, 1U, 0U, 0U },
	};

	run_scenario("long-term lsb", 16U, 6U, 0U, 0U, 1, lsb, 6U);
	run_scenario("long-term msb", 16U, 6U, 0U, 0U, 1, msb, 6U);
	run_scenario("long-term negative", 16U, 6U, 0U, 0U, 1, negative, 4U);
	run_scenario("long-term unused", 16U, 6U, 3U, 0U, 1, unused_waiting, 4U);
}

/*
 * IRAP pictures with NoRaslOutputFlag 1.  Reorder 3, size 5.  The first
 * picture is picture 0 (nothing to flush; reset).  The second IDR (not
 * picture 0, NoOutputOfPriorPicsFlag 0) outputs the three waiting pictures
 * in order (0, 4, 8) and empties the buffer.  A third IDR with
 * NoOutputOfPriorPicsFlag 1 discards the two waiting pictures (0 and 5), no
 * output.  A CRA-like picture with only Foll entries finds nothing (all
 * references were marked unused first) and counts nothing missing; one with a
 * Curr entry counts it missing.
 */
static void
check_irap(void)
{
	static const struct step steps[] = {
		{ "0 I", "", "", "", SLOT_ANY, 0U, 0U, 0U },
		{ "8 b0", "", "", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "4 b0 a8", "", "", "0,8", SLOT_ANY, 0U, 0U, 0U },
		{ "0 I", "0,4,8", "", "", SLOT_ANY, 0U, 0U, 0U },
		{ "5 b0", "", "", "0", SLOT_ANY, 0U, 0U, 0U },
		{ "0 I !", "", "", "", SLOT_ANY, 0U, 0U, 2U },
		{ "16 I f8", "0", "", "", SLOT_ANY, 0U, 0U, 0U },
		{ "20 I b16", "16", "", "", SLOT_ANY, 1U, 0U, 0U },
	};
	static const struct step first_with_flag[] = {
		{ "0 I !", "", "", "", SLOT_ANY, 0U, 0U, 0U },
		{ "1 b0", "", "", "0", SLOT_ANY, 0U, 0U, 0U },
	};
	static const struct step first_without_irap[] = {
		{ "5 b0 f7", "", "", "", SLOT_ANY, 1U, 0U, 0U },
	};

	run_scenario("irap", 16U, 5U, 3U, 0U, 1, steps, 8U);
	run_scenario("irap first with flag", 16U, 5U, 3U, 0U, 1, first_with_flag, 2U);
	run_scenario("first picture is not an irap", 16U, 5U, 3U, 0U, 1, first_without_irap, 1U);
}

/*
 * Missing Curr entries: a short-term entry and a long-term entry naming
 * pictures that are not in the buffer count as missing and have no slot; a
 * missing Foll entry is not counted.
 */
static void
check_missing(void)
{
	static const struct step steps[] = {
		{ "0 I", "", "0", "", SLOT_ANY, 0U, 0U, 0U },
		{ "1 b0,5 f7", "", "1", "0", SLOT_ANY, 1U, 0U, 0U },
		{ "2 b1,9 a3 L4", "", "2", "1", SLOT_ANY, 3U, 0U, 0U },
	};

	run_scenario("missing", 16U, 4U, 0U, 0U, 1, steps, 3U);
}

/* Builds a minimal valid plan state for the error tests: a begun picture with its slot taken. */
static void
begin_picture(
	int32_t poc)
{
	struct h265sw_rps_pocs pocs;
	int rc;

	memset(&pocs, 0, sizeof(pocs));
	rc = h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan);
	CHECK(rc == 0);
	rc = h265_dpb_take_slot(&dpb, &plan);
	CHECK(rc == 0);
	(void)poc;
}

/*
 * A broken stream in which every held picture is a Curr picture: the buffer
 * (size 2, reorder 4) holds 0 and 1, both waiting; picture 2 names both.  The
 * buffer is full, so 0 and 1 are bumped (they stay), nothing can be
 * evicted and the begin fails with EINVAL, the plan holding the outputs and
 * the buffer not in a picture.  A following picture that keeps only one of
 * them succeeds.
 */
static void
check_all_curr(void)
{
	struct pic_spec spec;
	struct h265_dpb_output end_output[H265_DPB_MAX_OUTPUT];
	unsigned end_count;
	char text[64];
	int rc;

	h265_dpb_init(&dpb, 16U);
	sps_make(2U, 4U, 0U, 4U);

	parse_pic("0 I", &spec);
	rc = h265_dpb_begin(&dpb, &sps_store, &spec.pocs, 1, 0, &plan);
	CHECK(rc == 0);
	rc = h265_dpb_take_slot(&dpb, &plan);
	CHECK(rc == 0);
	rc = h265_dpb_end(&dpb, &plan, 0, 1, 0U, end_output, &end_count);
	CHECK(rc == 0);
	CHECK(end_count == 0U);

	parse_pic("1 b0", &spec);
	rc = h265_dpb_begin(&dpb, &sps_store, &spec.pocs, 0, 0, &plan);
	CHECK(rc == 0);
	rc = h265_dpb_take_slot(&dpb, &plan);
	CHECK(rc == 0);
	rc = h265_dpb_end(&dpb, &plan, 1, 1, 1U, end_output, &end_count);
	CHECK(rc == 0);
	CHECK(end_count == 0U);

	parse_pic("2 b0,1", &spec);
	rc = h265_dpb_begin(&dpb, &sps_store, &spec.pocs, 0, 0, &plan);
	CHECK(rc == EINVAL);
	CHECK(dpb.in_picture == 0);
	CHECK(dpb.started == 1);
	CHECK(plan.setup_slot == H265_DPB_NO_SLOT);
	CHECK(plan.evicted == 0U);
	format_outputs(plan.output, plan.output_count, text);
	check_string("all curr", 0U, text, "0,1");
	CHECK(plan.reference_count == 2U);
	CHECK(plan.missing == 0U);
	check_state(&dpb);

	/* No picture is under way, so the later calls refuse. */
	rc = h265_dpb_take_slot(&dpb, &plan);
	CHECK(rc == EINVAL);
	end_count = 7U;
	rc = h265_dpb_end(&dpb, &plan, 2, 1, 2U, end_output, &end_count);
	CHECK(rc == EINVAL);
	CHECK(end_count == 0U);

	/* The caller skips the picture; the next one keeps only picture 1. */
	h265_dpb_abort(&dpb);
	release_outputs(plan.output, plan.output_count);
	parse_pic("3 b1", &spec);
	rc = h265_dpb_begin(&dpb, &sps_store, &spec.pocs, 0, 0, &plan);
	CHECK(rc == 0);
	CHECK(plan.output_count == 0U);
	CHECK(plan.evicted == 0U);
	format_refs(&plan, text);
	check_string("all curr next", 0U, text, "1");
}

/*
 * Slots 16 with a buffer of 16 pictures: every picture is output at once and
 * its slot stays busy until the test releases it, so take_slot reports EBUSY
 * and works again after a release.  Each picture references the six
 * previous ones; the reference list never exceeds 15 entries.
 */
static void
check_sixteen_slots(void)
{
	struct pic_spec spec;
	struct h265_dpb_output end_output[H265_DPB_MAX_OUTPUT];
	unsigned end_count;
	unsigned index;
	unsigned busy_events;
	unsigned slot;
	int32_t poc;
	int rc;

	h265_dpb_init(&dpb, 16U);
	sps_make(16U, 0U, 0U, 8U);
	busy_events = 0U;
	for (index = 0U;
	     index < 300U;
	     index++) {
		memset(&spec, 0, sizeof(spec));
		poc = (int32_t)index;
		spec.irap = 0;
		if (index == 0U)
			spec.irap = 1;
		spec.pocs.num_st_before = 0U;
		while (spec.pocs.num_st_before < 6U && spec.pocs.num_st_before < index) {
			spec.pocs.st_before[spec.pocs.num_st_before] = poc - 1 - (int32_t)spec.pocs.num_st_before;
			spec.pocs.num_st_before++;
		}
		rc = h265_dpb_begin(&dpb, &sps_store, &spec.pocs, spec.irap, 0, &plan);
		CHECK(rc == 0);
		if (rc != 0)
			return;
		CHECK(plan.missing == 0U);
		CHECK(plan.reference_count <= 15U);
		CHECK(plan.reference_count == spec.pocs.num_st_before);
		check_state(&dpb);

		/* Waits for a slot: while all are busy or held, releases the lowest busy slot. */
		rc = h265_dpb_take_slot(&dpb, &plan);
		while (rc == EBUSY) {
			busy_events++;
			CHECK(dpb.busy != 0U);
			for (slot = 0U;
			     slot < 16U;
			     slot++) {
				if ((dpb.busy & ((uint32_t)1U << slot)) != 0U)
					break;
			}
			h265_dpb_release(&dpb, (int)slot);
			rc = h265_dpb_take_slot(&dpb, &plan);
		}
		CHECK(rc == 0);
		check_plan_refs(&dpb, &plan);
		rc = h265_dpb_end(&dpb, &plan, poc, 1, (uint64_t)index, end_output, &end_count);
		CHECK(rc == 0);
		CHECK(end_count == 1U);
		check_state(&dpb);
	}
	CHECK(busy_events > 0U);
}

/*
 * The slot rules: the lowest free slot, EBUSY when every slot is busy (and
 * nothing changes), success after a release, the same slot again on a second
 * call, the busy slot of an output reference picture staying held after the
 * release, the clamping of the slot count, and release ignoring slots out of
 * range.
 */
static void
check_slots(void)
{
	struct h265_dpb copy;
	struct h265_dpb_plan plan_copy;
	struct h265sw_rps_pocs pocs;
	struct h265_dpb_output end_output[H265_DPB_MAX_OUTPUT];
	unsigned end_count;
	int rc;

	/* The slot count is clamped into 16..32. */
	h265_dpb_init(&dpb, 0U);
	CHECK(dpb.slots == 16U);
	h265_dpb_init(&dpb, 15U);
	CHECK(dpb.slots == 16U);
	h265_dpb_init(&dpb, 16U);
	CHECK(dpb.slots == 16U);
	h265_dpb_init(&dpb, 17U);
	CHECK(dpb.slots == 17U);
	h265_dpb_init(&dpb, 32U);
	CHECK(dpb.slots == 32U);
	h265_dpb_init(&dpb, 33U);
	CHECK(dpb.slots == 32U);
	h265_dpb_init(&dpb, 1000U);
	CHECK(dpb.slots == 32U);
	CHECK(dpb.busy == 0U);
	CHECK(dpb.size == 1U);
	CHECK(dpb.started == 0);
	CHECK(dpb.in_picture == 0);
	CHECK(dpb.current == H265_DPB_NO_SLOT);
	check_state(&dpb);

	/* All 16 slots busy: EBUSY, nothing changes; releasing one gives that slot. */
	h265_dpb_init(&dpb, 16U);
	sps_make(4U, 0U, 0U, 4U);
	memset(&pocs, 0, sizeof(pocs));
	rc = h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan);
	CHECK(rc == 0);
	dpb.busy = 0xFFFFU;
	copy = dpb;
	plan_copy = plan;
	rc = h265_dpb_take_slot(&dpb, &plan);
	CHECK(rc == EBUSY);
	CHECK(memcmp(&dpb, &copy, sizeof(dpb)) == 0);
	CHECK(memcmp(&plan, &plan_copy, sizeof(plan)) == 0);
	h265_dpb_release(&dpb, 11);
	CHECK(dpb.busy == (0xFFFFU & ~((uint32_t)1U << 11)));
	rc = h265_dpb_take_slot(&dpb, &plan);
	CHECK(rc == 0);
	CHECK(plan.setup_slot == 11);
	CHECK(dpb.current == 11);

	/* A second call after success gives the same slot, even when a lower slot has been released since. */
	h265_dpb_release(&dpb, 2);
	rc = h265_dpb_take_slot(&dpb, &plan);
	CHECK(rc == 0);
	CHECK(plan.setup_slot == 11);

	/* The slot of the picture is stored with the entry at end. */
	rc = h265_dpb_end(&dpb, &plan, 0, 1, 5U, end_output, &end_count);
	CHECK(rc == 0);
	CHECK(end_count == 1U);
	CHECK(end_output[0].slot == 11);
	CHECK(end_output[0].poc == 0);
	CHECK(end_output[0].id == 5U);
	CHECK((dpb.busy & ((uint32_t)1U << 11)) != 0U);

	/* The picture 0 is a reference; releasing its slot does not free it for the next picture. */
	h265_dpb_release(&dpb, 11);
	CHECK((dpb.busy & ((uint32_t)1U << 11)) == 0U);
	memset(&pocs, 0, sizeof(pocs));
	pocs.num_st_before = 1U;
	pocs.st_before[0] = 0;
	dpb.busy = 0U;
	rc = h265_dpb_begin(&dpb, &sps_store, &pocs, 0, 0, &plan);
	CHECK(rc == 0);
	rc = h265_dpb_take_slot(&dpb, &plan);
	CHECK(rc == 0);
	CHECK(plan.setup_slot == 0);
	CHECK(plan.reference_count == 1U);
	CHECK(plan.reference_slot[0] == 11);

	/* A slot held by an entry is skipped even when it is the lowest. */
	h265_dpb_abort(&dpb);
	h265_dpb_clear(&dpb);
	dpb.entry[0].used = 1;
	dpb.entry[0].marking = H265_DPB_SHORT;
	dpb.entry[0].slot = 0;
	dpb.entry[1].used = 1;
	dpb.entry[1].marking = H265_DPB_SHORT;
	dpb.entry[1].slot = 1;
	dpb.entry[1].poc = 1;
	dpb.busy = (uint32_t)1U << 2;
	dpb.in_picture = 1;
	rc = h265_dpb_take_slot(&dpb, &plan);
	CHECK(rc == 0);
	CHECK(plan.setup_slot == 3);

	/* Out of range releases are ignored: below 0, at slots and above. */
	h265_dpb_init(&dpb, 20U);
	dpb.busy = 0xFFFFFFFFU & ~(0xFFFFFU);
	h265_dpb_release(&dpb, -1);
	h265_dpb_release(&dpb, 20);
	h265_dpb_release(&dpb, 21);
	h265_dpb_release(&dpb, 31);
	h265_dpb_release(&dpb, 32);
	h265_dpb_release(&dpb, 1000);
	h265_dpb_release(&dpb, -1000);
	CHECK(dpb.busy == (0xFFFFFFFFU & ~(0xFFFFFU)));
	dpb.busy = 0xFFFFFU;
	h265_dpb_release(&dpb, 19);
	CHECK(dpb.busy == 0x7FFFFU);
	h265_dpb_release(&dpb, 0);
	CHECK(dpb.busy == 0x7FFFEU);
	h265_dpb_release(&dpb, 0);
	CHECK(dpb.busy == 0x7FFFEU);
	h265_dpb_release(NULL, 0);
}

/*
 * The begin errors (nothing changes), the picture steps out of order, abort,
 * the end errors, flush and clear.
 */
static void
check_calls(void)
{
	struct h265_dpb copy;
	struct h265_dpb_plan plan_copy;
	struct h265_dpb_plan other_plan;
	struct h265sw_rps_pocs pocs;
	struct h265_dpb_output end_output[H265_DPB_MAX_OUTPUT];
	unsigned end_count;
	unsigned index;
	int rc;

	/* begin refuses a second begin and leaves the buffer and the plan alone. */
	h265_dpb_init(&dpb, 16U);
	sps_make(4U, 2U, 0U, 4U);
	memset(&pocs, 0, sizeof(pocs));
	rc = h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan);
	CHECK(rc == 0);
	CHECK(dpb.in_picture == 1);
	copy = dpb;
	plan_copy = plan;
	rc = h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan);
	CHECK(rc == EINVAL);
	CHECK(memcmp(&dpb, &copy, sizeof(dpb)) == 0);
	CHECK(memcmp(&plan, &plan_copy, sizeof(plan)) == 0);

	/* abort before take_slot ends the picture; abort again does nothing. */
	h265_dpb_abort(&dpb);
	CHECK(dpb.in_picture == 0);
	CHECK(dpb.current == H265_DPB_NO_SLOT);
	copy = dpb;
	h265_dpb_abort(&dpb);
	CHECK(memcmp(&dpb, &copy, sizeof(dpb)) == 0);
	h265_dpb_abort(NULL);

	/* The bad arguments of begin change nothing. */
	copy = dpb;
	CHECK(h265_dpb_begin(NULL, &sps_store, &pocs, 1, 0, &plan) == EINVAL);
	CHECK(h265_dpb_begin(&dpb, NULL, &pocs, 1, 0, &plan) == EINVAL);
	CHECK(h265_dpb_begin(&dpb, &sps_store, NULL, 1, 0, &plan) == EINVAL);
	CHECK(h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, NULL) == EINVAL);
	sps_make(17U, 2U, 0U, 4U);
	CHECK(h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan) == EINVAL);
	sps_make(16U, 2U, 0U, 4U);
	sps_store.max_dec_pic_buffering_minus1[0] = 0xFFFFFFFFU;
	CHECK(h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan) == EINVAL);
	sps_make(4U, 2U, 0U, 3U);
	CHECK(h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan) == EINVAL);
	sps_make(4U, 2U, 0U, 17U);
	CHECK(h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan) == EINVAL);
	sps_make(4U, 2U, 0U, 4U);
	sps_store.max_sub_layers_minus1 = 7U;
	CHECK(h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan) == EINVAL);
	sps_make(4U, 2U, 0U, 4U);
	pocs.num_st_before = 17U;
	CHECK(h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan) == EINVAL);
	pocs.num_st_before = 0U;
	pocs.num_st_after = 17U;
	CHECK(h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan) == EINVAL);
	pocs.num_st_after = 0U;
	pocs.num_st_foll = 17U;
	CHECK(h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan) == EINVAL);
	pocs.num_st_foll = 0U;
	pocs.num_lt_curr = 17U;
	CHECK(h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan) == EINVAL);
	pocs.num_lt_curr = 0U;
	pocs.num_lt_foll = 17U;
	CHECK(h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan) == EINVAL);
	pocs.num_lt_foll = 0U;
	CHECK(memcmp(&dpb, &copy, sizeof(dpb)) == 0);

	/* The sizes the SPS gives at its largest legal values are accepted. */
	sps_make(16U, 15U, 0U, 16U);
	rc = h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan);
	CHECK(rc == 0);
	CHECK(dpb.size == 16U);
	CHECK(dpb.reorder == 15U);
	h265_dpb_abort(&dpb);

	/* HighestTid is the last sub-layer: its values are used, not those of sub-layer 0. */
	sps_make(4U, 2U, 3U, 4U);
	sps_store.max_sub_layers_minus1 = 2U;
	sps_store.max_dec_pic_buffering_minus1[2] = 7U;
	sps_store.max_num_reorder_pics[2] = 5U;
	sps_store.max_latency_increase_plus1[2] = 4U;
	sps_store.max_dec_pic_buffering_minus1[6] = 1U;
	sps_store.max_num_reorder_pics[6] = 1U;
	sps_store.max_latency_increase_plus1[6] = 1U;
	rc = h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan);
	CHECK(rc == 0);
	CHECK(dpb.size == 8U);
	CHECK(dpb.reorder == 5U);
	CHECK(dpb.latency_limited == 1);
	CHECK(dpb.max_latency == 8U);
	h265_dpb_abort(&dpb);
	sps_store.max_latency_increase_plus1[2] = 0U;
	rc = h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan);
	CHECK(rc == 0);
	CHECK(dpb.latency_limited == 0);
	h265_dpb_abort(&dpb);
	sps_store.max_latency_increase_plus1[2] = 0xFFFFFFFFU;
	rc = h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan);
	CHECK(rc == 0);
	CHECK(dpb.latency_limited == 1);
	CHECK(dpb.max_latency == (uint64_t)5U + (uint64_t)0xFFFFFFFFU - 1U);
	h265_dpb_abort(&dpb);

	/* take_slot and end refuse when no picture is under way. */
	h265_dpb_init(&dpb, 16U);
	sps_make(4U, 0U, 0U, 4U);
	rc = h265_dpb_take_slot(&dpb, &plan);
	CHECK(rc == EINVAL);
	end_count = 9U;
	rc = h265_dpb_end(&dpb, &plan, 0, 1, 0U, end_output, &end_count);
	CHECK(rc == EINVAL);
	CHECK(end_count == 0U);
	CHECK(h265_dpb_take_slot(NULL, &plan) == EINVAL);
	CHECK(h265_dpb_take_slot(&dpb, NULL) == EINVAL);

	/* end refuses before take_slot, with the wrong slot, and with bad arguments; nothing changes. */
	begin_picture(0);
	copy = dpb;
	other_plan = plan;
	other_plan.setup_slot = H265_DPB_NO_SLOT;
	end_count = 9U;
	rc = h265_dpb_end(&dpb, &other_plan, 0, 1, 0U, end_output, &end_count);
	CHECK(rc == EINVAL);
	CHECK(end_count == 0U);
	other_plan.setup_slot = plan.setup_slot + 1;
	end_count = 9U;
	rc = h265_dpb_end(&dpb, &other_plan, 0, 1, 0U, end_output, &end_count);
	CHECK(rc == EINVAL);
	CHECK(end_count == 0U);
	CHECK(h265_dpb_end(&dpb, &plan, 0, 1, 0U, end_output, NULL) == EINVAL);
	end_count = 9U;
	CHECK(h265_dpb_end(&dpb, NULL, 0, 1, 0U, end_output, &end_count) == EINVAL);
	end_count = 9U;
	CHECK(h265_dpb_end(&dpb, &plan, 0, 1, 0U, NULL, &end_count) == EINVAL);
	CHECK(h265_dpb_end(NULL, &plan, 0, 1, 0U, end_output, &end_count) == EINVAL);
	CHECK(memcmp(&dpb, &copy, sizeof(dpb)) == 0);

	/* end with a plan that has no slot while no take_slot happened (current is "no slot") is refused. */
	h265_dpb_init(&dpb, 16U);
	rc = h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan);
	CHECK(rc == 0);
	copy = dpb;
	end_count = 9U;
	rc = h265_dpb_end(&dpb, &plan, 0, 1, 0U, end_output, &end_count);
	CHECK(rc == EINVAL);
	CHECK(end_count == 0U);
	CHECK(memcmp(&dpb, &copy, sizeof(dpb)) == 0);

	/* end with every entry used is refused and changes nothing (a begin that did not make room). */
	rc = h265_dpb_take_slot(&dpb, &plan);
	CHECK(rc == 0);
	for (index = 0U;
	     index < H265_DPB_PICTURES;
	     index++) {
		dpb.entry[index].used = 1;
		dpb.entry[index].marking = H265_DPB_SHORT;
		dpb.entry[index].needed = 1;
		dpb.entry[index].poc = (int32_t)index;
		dpb.entry[index].slot = 16 + (int)index;
		dpb.entry[index].id = index;
	}
	dpb.slots = 32U;
	copy = dpb;
	end_count = 9U;
	rc = h265_dpb_end(&dpb, &plan, 5, 1, 0U, end_output, &end_count);
	CHECK(rc == EINVAL);
	CHECK(end_count == 0U);
	CHECK(memcmp(&dpb, &copy, sizeof(dpb)) == 0);

}

/*
 * The longest lists: each of the five lists with 16 entries is accepted (the
 * Curr ones all missing here, the Foll ones not counted), and all five full
 * at once; take_slot called again with another plan structure still reports
 * the slot of the picture; a buffer with all 32 slots busy reports EBUSY.
 */
static void
check_limits(void)
{
	struct h265sw_rps_pocs pocs;
	struct h265_dpb_plan other_plan;
	unsigned list;
	unsigned index;
	int rc;

	h265_dpb_init(&dpb, 32U);
	sps_make(16U, 0U, 0U, 4U);
	for (list = 0U;
	     list < 6U;
	     list++) {
		memset(&pocs, 0, sizeof(pocs));
		for (index = 0U;
		     index < 16U;
		     index++) {
			if (list == 0U || list == 5U) {
				pocs.st_before[index] = 100 + (int32_t)index;
				pocs.num_st_before = 16U;
			}
			if (list == 1U || list == 5U) {
				pocs.st_after[index] = 200 + (int32_t)index;
				pocs.num_st_after = 16U;
			}
			if (list == 2U || list == 5U) {
				pocs.st_foll[index] = 300 + (int32_t)index;
				pocs.num_st_foll = 16U;
			}
			if (list == 3U || list == 5U) {
				pocs.lt_curr[index] = (int32_t)index;
				pocs.lt_curr_msb[index] = 0U;
				pocs.num_lt_curr = 16U;
			}
			if (list == 4U || list == 5U) {
				pocs.lt_foll[index] = (int32_t)index;
				pocs.lt_foll_msb[index] = 1U;
				pocs.num_lt_foll = 16U;
			}
		}
		rc = h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan);
		CHECK(rc == 0);
		if (list == 0U || list == 1U || list == 3U)
			CHECK(plan.missing == 16U);
		if (list == 2U || list == 4U)
			CHECK(plan.missing == 0U);
		if (list == 5U)
			CHECK(plan.missing == 48U);
		if (list == 0U || list == 5U)
			CHECK(plan.num_st_before == 16U);
		if (list == 1U || list == 5U)
			CHECK(plan.num_st_after == 16U);
		if (list == 3U || list == 5U)
			CHECK(plan.num_lt_curr == 16U);
		h265_dpb_abort(&dpb);
	}

	/* A second take_slot with a plan structure that has no slot yet fills it in. */
	memset(&pocs, 0, sizeof(pocs));
	rc = h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan);
	CHECK(rc == 0);
	rc = h265_dpb_take_slot(&dpb, &plan);
	CHECK(rc == 0);
	memset(&other_plan, 0, sizeof(other_plan));
	other_plan.setup_slot = H265_DPB_NO_SLOT;
	rc = h265_dpb_take_slot(&dpb, &other_plan);
	CHECK(rc == 0);
	CHECK(other_plan.setup_slot == plan.setup_slot);
	h265_dpb_abort(&dpb);

	/* All 32 slots busy. */
	dpb.busy = 0xFFFFFFFFU;
	rc = h265_dpb_begin(&dpb, &sps_store, &pocs, 1, 0, &plan);
	CHECK(rc == 0);
	rc = h265_dpb_take_slot(&dpb, &plan);
	CHECK(rc == EBUSY);
	CHECK(dpb.current == H265_DPB_NO_SLOT);
	h265_dpb_release(&dpb, 31);
	rc = h265_dpb_take_slot(&dpb, &plan);
	CHECK(rc == 0);
	CHECK(plan.setup_slot == 31);
}

/*
 * A buffer whose entries hold slots outside the session (a caller that wrote
 * the structure itself): take_slot ignores them, and bumping reports them
 * without touching a busy bit or reading outside the mask.
 */
static void
check_foreign_slots(void)
{
	struct h265_dpb_output out[H265_DPB_MAX_OUTPUT];
	struct h265sw_rps_pocs pocs;
	unsigned count;
	unsigned index;
	int rc;

	h265_dpb_init(&dpb, 32U);
	sps_make(8U, 0U, 0U, 4U);
	dpb.entry[0].used = 1;
	dpb.entry[0].marking = H265_DPB_SHORT;
	dpb.entry[0].slot = 32;
	dpb.entry[1].used = 1;
	dpb.entry[1].marking = H265_DPB_SHORT;
	dpb.entry[1].slot = 40;
	dpb.entry[1].poc = 1;
	dpb.entry[2].used = 1;
	dpb.entry[2].marking = H265_DPB_SHORT;
	dpb.entry[2].slot = H265_DPB_NO_SLOT;
	dpb.entry[2].poc = 2;
	dpb.in_picture = 1;
	rc = h265_dpb_take_slot(&dpb, &plan);
	CHECK(rc == 0);
	CHECK(plan.setup_slot == 0);
	dpb.in_picture = 0;
	dpb.current = H265_DPB_NO_SLOT;
	for (index = 0U;
	     index < 3U;
	     index++)
		dpb.entry[index].needed = 1;
	count = 0U;
	rc = h265_dpb_flush(&dpb, out, &count);
	CHECK(rc == 0);
	CHECK(count == 3U);
	CHECK(out[0].slot == 32);
	CHECK(out[1].slot == 40);
	CHECK(out[2].slot == H265_DPB_NO_SLOT);
	CHECK(dpb.busy == 0U);
	memset(&pocs, 0, sizeof(pocs));
	h265_dpb_release(&dpb, 32);
	h265_dpb_release(&dpb, 40);
}

/*
 * abort after take_slot gives the slot back and keeps what begin did: size 3,
 * reorder 4, pictures 0, 8, 4, then picture 2 (before 0) outputs 0 and 4 at
 * its begin and empties the entry of 4 (unused).  After the abort the entry
 * of 0 is still a reference, the entry of 8 still waits, the slots of 0 and 4
 * are still busy, and the picture's slot (3) is free: a later picture gets it
 * again once the output slots are held.
 */
static void
check_abort(void)
{
	struct pic_spec spec;
	struct h265_dpb_output end_output[H265_DPB_MAX_OUTPUT];
	unsigned end_count;
	unsigned index;
	char text[64];
	int rc;

	h265_dpb_init(&dpb, 16U);
	sps_make(3U, 4U, 0U, 4U);
	parse_pic("0 I", &spec);
	rc = h265_dpb_begin(&dpb, &sps_store, &spec.pocs, 1, 0, &plan);
	CHECK(rc == 0);
	h265_dpb_take_slot(&dpb, &plan);
	h265_dpb_end(&dpb, &plan, 0, 1, 0U, end_output, &end_count);
	parse_pic("8 b0", &spec);
	h265_dpb_begin(&dpb, &sps_store, &spec.pocs, 0, 0, &plan);
	h265_dpb_take_slot(&dpb, &plan);
	h265_dpb_end(&dpb, &plan, 8, 1, 1U, end_output, &end_count);
	parse_pic("4 b0 a8", &spec);
	h265_dpb_begin(&dpb, &sps_store, &spec.pocs, 0, 0, &plan);
	h265_dpb_take_slot(&dpb, &plan);
	h265_dpb_end(&dpb, &plan, 4, 1, 2U, end_output, &end_count);

	parse_pic("2 b0", &spec);
	rc = h265_dpb_begin(&dpb, &sps_store, &spec.pocs, 0, 0, &plan);
	CHECK(rc == 0);
	format_outputs(plan.output, plan.output_count, text);
	check_string("abort begin", 0U, text, "0,4");
	rc = h265_dpb_take_slot(&dpb, &plan);
	CHECK(rc == 0);
	CHECK(plan.setup_slot == 3);
	h265_dpb_abort(&dpb);
	CHECK(dpb.in_picture == 0);
	CHECK(dpb.current == H265_DPB_NO_SLOT);
	CHECK(dpb.busy == (((uint32_t)1U << 0) | ((uint32_t)1U << 2)));
	CHECK(dpb.started == 1);
	check_state(&dpb);
	end_count = 3U;
	rc = h265_dpb_end(&dpb, &plan, 2, 1, 3U, end_output, &end_count);
	CHECK(rc == EINVAL);
	CHECK(end_count == 0U);
	for (index = 0U;
	     index < H265_DPB_PICTURES;
	     index++) {
		if (dpb.entry[index].used == 0)
			continue;
		CHECK(dpb.entry[index].poc == 0 || dpb.entry[index].poc == 8);
		if (dpb.entry[index].poc == 0) {
			CHECK(dpb.entry[index].marking == H265_DPB_SHORT);
			CHECK(dpb.entry[index].needed == 0);
		} else {
			CHECK(dpb.entry[index].marking == H265_DPB_UNUSED);
			CHECK(dpb.entry[index].needed == 1);
		}
	}

	/* The next picture starts from this buffer and gets the slot the aborted one had. */
	parse_pic("3 b0", &spec);
	rc = h265_dpb_begin(&dpb, &sps_store, &spec.pocs, 0, 0, &plan);
	CHECK(rc == 0);
	CHECK(plan.output_count == 0U);
	rc = h265_dpb_take_slot(&dpb, &plan);
	CHECK(rc == 0);
	CHECK(plan.setup_slot == 3);
}

/*
 * flush outputs the waiting pictures in order, makes their slots busy and
 * empties the buffer (references too); the next picture is the first of a
 * run again.  clear empties without output and keeps the busy slots.  The
 * "reset" flag of the plan is 1 for the first picture and after each.
 */
static void
check_flush_clear(void)
{
	struct pic_spec spec;
	struct h265_dpb_output end_output[H265_DPB_MAX_OUTPUT];
	struct h265_dpb copy;
	unsigned end_count;
	unsigned flush_count;
	char text[64];
	int rc;

	h265_dpb_init(&dpb, 16U);
	sps_make(5U, 4U, 0U, 4U);
	parse_pic("0 I", &spec);
	rc = h265_dpb_begin(&dpb, &sps_store, &spec.pocs, 1, 0, &plan);
	CHECK(rc == 0);
	CHECK(plan.reset == 1);
	h265_dpb_take_slot(&dpb, &plan);
	h265_dpb_end(&dpb, &plan, 0, 1, 10U, end_output, &end_count);
	parse_pic("8 b0", &spec);
	rc = h265_dpb_begin(&dpb, &sps_store, &spec.pocs, 0, 0, &plan);
	CHECK(rc == 0);
	CHECK(plan.reset == 0);
	h265_dpb_take_slot(&dpb, &plan);
	h265_dpb_end(&dpb, &plan, 8, 1, 11U, end_output, &end_count);
	parse_pic("4 b0 a8", &spec);
	h265_dpb_begin(&dpb, &sps_store, &spec.pocs, 0, 0, &plan);
	h265_dpb_take_slot(&dpb, &plan);
	h265_dpb_end(&dpb, &plan, 4, 1, 12U, end_output, &end_count);
	CHECK(dpb.started == 1);

	/* flush refuses while a picture is under way; nothing changes. */
	parse_pic("2 b0 a4,8", &spec);
	rc = h265_dpb_begin(&dpb, &sps_store, &spec.pocs, 0, 0, &plan);
	CHECK(rc == 0);
	copy = dpb;
	flush_count = 5U;
	rc = h265_dpb_flush(&dpb, end_output, &flush_count);
	CHECK(rc == EINVAL);
	CHECK(flush_count == 0U);
	CHECK(memcmp(&dpb, &copy, sizeof(dpb)) == 0);
	h265_dpb_abort(&dpb);
	CHECK(h265_dpb_flush(&dpb, end_output, NULL) == EINVAL);
	CHECK(h265_dpb_flush(&dpb, NULL, &flush_count) == EINVAL);
	CHECK(h265_dpb_flush(NULL, end_output, &flush_count) == EINVAL);

	/* The flush outputs 0, 4, 8 with their ids and slots, and everything is empty after it. */
	flush_count = 0U;
	rc = h265_dpb_flush(&dpb, end_output, &flush_count);
	CHECK(rc == 0);
	format_outputs(end_output, flush_count, text);
	check_string("flush", 0U, text, "0,4,8");
	CHECK(end_output[0].id == 10U);
	CHECK(end_output[1].id == 12U);
	CHECK(end_output[2].id == 11U);
	CHECK(end_output[0].slot == 0);
	CHECK(end_output[1].slot == 2);
	CHECK(end_output[2].slot == 1);
	CHECK(dpb.busy == 7U);
	CHECK(dpb.started == 0);
	CHECK(dpb.in_picture == 0);
	CHECK(dpb.current == H265_DPB_NO_SLOT);
	check_state(&dpb);
	flush_count = 0U;
	rc = h265_dpb_flush(&dpb, end_output, &flush_count);
	CHECK(rc == 0);
	CHECK(flush_count == 0U);
	CHECK(dpb.started == 0);

	/* The next picture is the first of a run again, and the busy slots are skipped. */
	parse_pic("0 I", &spec);
	rc = h265_dpb_begin(&dpb, &sps_store, &spec.pocs, 1, 0, &plan);
	CHECK(rc == 0);
	CHECK(plan.reset == 1);
	CHECK(plan.reference_count == 0U);
	rc = h265_dpb_take_slot(&dpb, &plan);
	CHECK(rc == 0);
	CHECK(plan.setup_slot == 3);
	h265_dpb_end(&dpb, &plan, 0, 1, 20U, end_output, &end_count);
	parse_pic("1 b0", &spec);
	rc = h265_dpb_begin(&dpb, &sps_store, &spec.pocs, 0, 0, &plan);
	CHECK(rc == 0);
	CHECK(plan.reset == 0);
	h265_dpb_abort(&dpb);

	/* clear empties without output, keeps busy, and ends a picture under way. */
	copy = dpb;
	h265_dpb_clear(&dpb);
	CHECK(dpb.busy == copy.busy);
	CHECK(dpb.started == 0);
	CHECK(dpb.in_picture == 0);
	CHECK(dpb.current == H265_DPB_NO_SLOT);
	CHECK(dpb.slots == copy.slots);
	CHECK(dpb.size == copy.size);
	check_state(&dpb);
	flush_count = 0U;
	rc = h265_dpb_flush(&dpb, end_output, &flush_count);
	CHECK(rc == 0);
	CHECK(flush_count == 0U);
	parse_pic("5 b0", &spec);
	rc = h265_dpb_begin(&dpb, &sps_store, &spec.pocs, 0, 0, &plan);
	CHECK(rc == 0);
	CHECK(plan.reset == 1);
	CHECK(plan.missing == 1U);
	rc = h265_dpb_take_slot(&dpb, &plan);
	CHECK(rc == 0);
	CHECK(dpb.in_picture == 1);
	h265_dpb_clear(&dpb);
	CHECK(dpb.in_picture == 0);
	CHECK(dpb.current == H265_DPB_NO_SLOT);
	h265_dpb_clear(NULL);
	h265_dpb_init(NULL, 16U);
}

/* One picture of the model: the clauses written as plain arrays, independent of the code under test. */
struct mpic {
	int present;
	int ref;
	int needed;
	uint64_t lat;
	int32_t poc;
	uint64_t id;
	int slot;
};

/* The model buffer: sixteen pictures, the busy slots and the SPS values of the last begin. */
struct model {
	struct mpic p[H265_DPB_PICTURES];
	uint32_t busy;
	unsigned slots;
	int started;
	unsigned size;
	unsigned reorder;
	int limited;
	uint64_t maxlat;
	int in_pic;
	int cur;
};

/* What the model says one begin does. */
struct mplan {
	int rc;
	int reset;
	unsigned nout;
	struct h265_dpb_output out[H265_DPB_MAX_OUTPUT];
	unsigned discarded;
	unsigned evicted;
	unsigned missing;
	unsigned nb;
	unsigned na;
	unsigned nl;
	int sb[H265SW_MAX_RPS];
	int sa[H265SW_MAX_RPS];
	int sl[H265SW_MAX_RPS];
	unsigned nref;
	int rslot[H265_DPB_PICTURES];
	int32_t rpoc[H265_DPB_PICTURES];
	int rlong[H265_DPB_PICTURES];
};

/* The model's floor remainder of an order count by 2^bits. */
static int64_t
m_lsb(
	int32_t poc,
	unsigned bits)
{
	int64_t modulus;
	int64_t r;

	modulus = (int64_t)1 << bits;
	r = (int64_t)poc % modulus;
	if (r < 0)
		r += modulus;
	return r;
}

/* Counts the pictures present in the model. */
static unsigned
m_present(const struct model *m)
{
	unsigned i;
	unsigned n;

	n = 0U;
	for (i = 0U; i < H265_DPB_PICTURES; i++)
		n += (unsigned)(m->p[i].present != 0);
	return n;
}

/* Empties a model picture. */
static void
m_clear(struct mpic *x)
{
	x->present = 0;
	x->ref = 0;
	x->needed = 0;
	x->lat = 0U;
	x->poc = 0;
	x->id = 0U;
	x->slot = -1;
}

/* Initialises the model. */
static void
m_init(struct model *m, unsigned slots)
{
	unsigned i;

	memset(m, 0, sizeof(*m));
	m->slots = slots;
	m->size = 1U;
	m->cur = -1;
	for (i = 0U; i < H265_DPB_PICTURES; i++)
		m_clear(&m->p[i]);
}

/* Returns the index of the waiting picture that is first in output order (smaller id on a tie), or -1. */
static int
m_first(const struct model *m)
{
	int best;
	int i;

	best = -1;
	for (i = 0; i < (int)H265_DPB_PICTURES; i++) {
		if (m->p[i].present == 0 || m->p[i].needed == 0)
			continue;
		if (best < 0 || m->p[i].poc < m->p[best].poc || (m->p[i].poc == m->p[best].poc && m->p[i].id < m->p[best].id))
			best = i;
	}
	return best;
}

/* Bumps one picture of the model; returns 0 when none waits. */
static int
m_bump(struct model *m, struct h265_dpb_output *out, unsigned *n)
{
	int i;

	i = m_first(m);
	if (i < 0)
		return 0;
	out[*n].slot = m->p[i].slot;
	out[*n].poc = m->p[i].poc;
	out[*n].id = m->p[i].id;
	*n += 1U;
	m->busy |= (uint32_t)1U << m->p[i].slot;
	m->p[i].needed = 0;
	if (m->p[i].ref == 0)
		m_clear(&m->p[i]);
	return 1;
}

/* Tells whether C.5.2.2 (check_size 1) or C.5.2.3 (0) wants a bump. */
static int
m_wanted(const struct model *m, int check_size)
{
	unsigned waiting;
	unsigned i;

	waiting = 0U;
	for (i = 0U; i < H265_DPB_PICTURES; i++) {
		if (m->p[i].present != 0 && m->p[i].needed != 0) {
			waiting++;
			if (m->limited != 0 && m->p[i].lat >= m->maxlat)
				return 1;
		}
	}
	if (waiting > m->reorder)
		return 1;
	if (check_size != 0 && m_present(m) >= m->size)
		return 1;
	return 0;
}

/* Bumps while wanted and possible. */
static void
m_bump_loop(struct model *m, int check_size, struct h265_dpb_output *out, unsigned *n)
{
	while (m_wanted(m, check_size) != 0) {
		if (m_bump(m, out, n) == 0)
			break;
	}
}

/* Removes the pictures that are neither waiting nor references. */
static void
m_remove(struct model *m)
{
	unsigned i;

	for (i = 0U; i < H265_DPB_PICTURES; i++) {
		if (m->p[i].present != 0 && m->p[i].needed == 0 && m->p[i].ref == 0)
			m_clear(&m->p[i]);
	}
}

/*
 * Finds the picture for one entry among the candidates not yet taken: kind 0
 * short-term by whole count, 1 long-term by low bits, 2 long-term by whole
 * count; the candidate with the greatest id.
 */
static int
m_find(const struct model *m, const int *taken, int32_t poc, int kind, unsigned bits)
{
	int best;
	int i;
	int ok;

	best = -1;
	for (i = 0; i < (int)H265_DPB_PICTURES; i++) {
		if (m->p[i].present == 0 || taken[i] != 0)
			continue;
		ok = 0;
		if (kind == 0 && m->p[i].ref == 1 && m->p[i].poc == poc)
			ok = 1;
		if (kind == 1 && m->p[i].ref != 0 && m_lsb(m->p[i].poc, bits) == (int64_t)poc)
			ok = 1;
		if (kind == 2 && m->p[i].ref != 0 && m->p[i].poc == poc)
			ok = 1;
		if (ok == 0)
			continue;
		if (best < 0 || m->p[i].id > m->p[best].id)
			best = i;
	}
	return best;
}

/* The model of h265_dpb_begin. */
static void
m_begin(
	struct model *m,
	unsigned size,
	unsigned reorder,
	unsigned plus1,
	unsigned bits,
	const struct h265sw_rps_pocs *pocs,
	int irap,
	int noout,
	struct mplan *r)
{
	int taken[H265_DPB_PICTURES];
	int curr[H265_DPB_PICTURES];
	int ltc[H265SW_MAX_RPS];
	int stb[H265SW_MAX_RPS];
	int sta[H265SW_MAX_RPS];
	unsigned i;
	unsigned slot;
	int idx;
	int victim;
	int k;

	memset(r, 0, sizeof(*r));
	r->reset = (m->started == 0);
	m->size = size;
	m->reorder = reorder;
	m->limited = (plus1 != 0U);
	m->maxlat = 0U;
	if (plus1 != 0U)
		m->maxlat = (uint64_t)reorder + (uint64_t)plus1 - 1U;
	memset(taken, 0, sizeof(taken));
	memset(curr, 0, sizeof(curr));
	if (irap != 0) {
		for (i = 0U; i < H265_DPB_PICTURES; i++)
			m->p[i].ref = 0;
	}

	for (i = 0U; i < pocs->num_lt_curr; i++) {
		ltc[i] = m_find(m, taken, pocs->lt_curr[i], pocs->lt_curr_msb[i] != 0U ? 2 : 1, bits);
		if (ltc[i] >= 0) {
			taken[ltc[i]] = 1;
			curr[ltc[i]] = 1;
		}
	}
	for (i = 0U; i < pocs->num_lt_foll; i++) {
		idx = m_find(m, taken, pocs->lt_foll[i], pocs->lt_foll_msb[i] != 0U ? 2 : 1, bits);
		if (idx >= 0)
			taken[idx] = 1;
	}
	for (k = 0; k < (int)H265_DPB_PICTURES; k++) {
		if (taken[k] != 0)
			m->p[k].ref = 2;
	}
	for (i = 0U; i < pocs->num_st_before; i++) {
		stb[i] = m_find(m, taken, pocs->st_before[i], 0, bits);
		if (stb[i] >= 0) {
			taken[stb[i]] = 1;
			curr[stb[i]] = 1;
		}
	}
	for (i = 0U; i < pocs->num_st_after; i++) {
		sta[i] = m_find(m, taken, pocs->st_after[i], 0, bits);
		if (sta[i] >= 0) {
			taken[sta[i]] = 1;
			curr[sta[i]] = 1;
		}
	}
	for (i = 0U; i < pocs->num_st_foll; i++) {
		idx = m_find(m, taken, pocs->st_foll[i], 0, bits);
		if (idx >= 0)
			taken[idx] = 1;
	}
	for (k = 0; k < (int)H265_DPB_PICTURES; k++) {
		if (taken[k] == 0)
			m->p[k].ref = 0;
	}

	if (irap != 0 && m->started != 0) {
		for (k = 0; k < (int)H265_DPB_PICTURES; k++) {
			if (m->p[k].present == 0)
				continue;
			if (noout != 0) {
				if (m->p[k].needed != 0)
					r->discarded++;
				m_clear(&m->p[k]);
			}
		}
		if (noout == 0) {
			m_remove(m);
			while (m_bump(m, r->out, &r->nout) != 0) {
			}
			for (k = 0; k < (int)H265_DPB_PICTURES; k++)
				m_clear(&m->p[k]);
		}
	} else {
		m_remove(m);
		m_bump_loop(m, 1, r->out, &r->nout);
	}

	while (m_present(m) >= m->size) {
		victim = -1;
		for (k = 0; k < (int)H265_DPB_PICTURES; k++) {
			if (m->p[k].present == 0 || curr[k] != 0)
				continue;
			if (victim < 0 || m->p[k].poc < m->p[victim].poc || (m->p[k].poc == m->p[victim].poc && m->p[k].id < m->p[victim].id))
				victim = k;
		}
		if (victim < 0)
			break;
		m_clear(&m->p[victim]);
		r->evicted++;
	}

	r->nb = pocs->num_st_before;
	r->na = pocs->num_st_after;
	r->nl = pocs->num_lt_curr;
	for (i = 0U; i < r->nb; i++) {
		r->sb[i] = stb[i] >= 0 ? m->p[stb[i]].slot : -1;
		r->missing += (unsigned)(stb[i] < 0);
	}
	for (i = 0U; i < r->na; i++) {
		r->sa[i] = sta[i] >= 0 ? m->p[sta[i]].slot : -1;
		r->missing += (unsigned)(sta[i] < 0);
	}
	for (i = 0U; i < r->nl; i++) {
		r->sl[i] = ltc[i] >= 0 ? m->p[ltc[i]].slot : -1;
		r->missing += (unsigned)(ltc[i] < 0);
	}
	for (slot = 0U; slot < H265_DPB_MAX_SLOTS; slot++) {
		for (k = 0; k < (int)H265_DPB_PICTURES; k++) {
			if (m->p[k].present != 0 && m->p[k].ref != 0 && m->p[k].slot == (int)slot) {
				r->rslot[r->nref] = m->p[k].slot;
				r->rpoc[r->nref] = m->p[k].poc;
				r->rlong[r->nref] = (m->p[k].ref == 2);
				r->nref++;
			}
		}
	}

	r->rc = 0;
	if (m_present(m) >= m->size) {
		r->rc = EINVAL;
		return;
	}
	m->started = 1;
	m->in_pic = 1;
	m->cur = -1;
}

/* The model of take_slot: the lowest slot no picture holds and no reader holds, or -1 (EBUSY). */
static int
m_take(struct model *m)
{
	uint32_t held;
	unsigned i;
	unsigned s;

	if (m->cur >= 0)
		return m->cur;
	held = m->busy;
	for (i = 0U; i < H265_DPB_PICTURES; i++) {
		if (m->p[i].present != 0)
			held |= (uint32_t)1U << m->p[i].slot;
	}
	for (s = 0U; s < m->slots; s++) {
		if ((held & ((uint32_t)1U << s)) == 0U) {
			m->cur = (int)s;
			return m->cur;
		}
	}
	return -1;
}

/* The model of end. */
static void
m_end(
	struct model *m,
	int32_t poc,
	int output,
	uint64_t id,
	struct h265_dpb_output *out,
	unsigned *n)
{
	unsigned i;
	unsigned free_index;

	*n = 0U;
	free_index = H265_DPB_PICTURES;
	for (i = 0U; i < H265_DPB_PICTURES; i++) {
		if (m->p[i].present == 0) {
			free_index = i;
			break;
		}
	}
	if (output != 0) {
		for (i = 0U; i < H265_DPB_PICTURES; i++) {
			if (m->p[i].present != 0 && m->p[i].needed != 0 && m->p[i].poc > poc)
				m->p[i].lat++;
		}
	}
	m->p[free_index].present = 1;
	m->p[free_index].ref = 1;
	m->p[free_index].needed = (output != 0);
	m->p[free_index].lat = 0U;
	m->p[free_index].poc = poc;
	m->p[free_index].id = id;
	m->p[free_index].slot = m->cur;
	m->in_pic = 0;
	m->cur = -1;
	m_bump_loop(m, 0, out, n);
}

/* Compares the buffer under test with the model: the entry of each slot, busy, started and the picture state. */
static void
compare_state(const struct h265_dpb *d, const struct model *m)
{
	unsigned s;
	unsigned i;
	const struct h265_dpb_entry *de;
	const struct mpic *me;

	CHECK(d->busy == m->busy);
	CHECK(d->started == m->started);
	CHECK(d->in_picture == m->in_pic);
	CHECK(d->current == m->cur);
	for (s = 0U; s < H265_DPB_MAX_SLOTS; s++) {
		de = NULL;
		me = NULL;
		for (i = 0U; i < H265_DPB_PICTURES; i++) {
			if (d->entry[i].used != 0 && d->entry[i].slot == (int)s)
				de = &d->entry[i];
			if (m->p[i].present != 0 && m->p[i].slot == (int)s)
				me = &m->p[i];
		}
		CHECK((de == NULL) == (me == NULL));
		if (de != NULL && me != NULL) {
			CHECK((int)de->marking == me->ref);
			CHECK(de->needed == me->needed);
			CHECK(de->latency == me->lat);
			CHECK(de->poc == me->poc);
			CHECK(de->id == me->id);
		}
	}
}

/* Compares the plan under test with the model's begin. */
static void
compare_plan(const struct h265_dpb_plan *p, const struct mplan *r)
{
	unsigned i;

	CHECK(p->reset == r->reset);
	CHECK(p->output_count == r->nout);
	for (i = 0U; i < r->nout && i < p->output_count; i++) {
		CHECK(p->output[i].slot == r->out[i].slot);
		CHECK(p->output[i].poc == r->out[i].poc);
		CHECK(p->output[i].id == r->out[i].id);
	}
	CHECK(p->discarded == r->discarded);
	CHECK(p->evicted == r->evicted);
	CHECK(p->missing == r->missing);
	CHECK(p->num_st_before == r->nb);
	CHECK(p->num_st_after == r->na);
	CHECK(p->num_lt_curr == r->nl);
	for (i = 0U; i < r->nb; i++)
		CHECK(p->st_before[i] == r->sb[i]);
	for (i = 0U; i < r->na; i++)
		CHECK(p->st_after[i] == r->sa[i]);
	for (i = 0U; i < r->nl; i++)
		CHECK(p->lt_curr[i] == r->sl[i]);
	CHECK(p->reference_count == r->nref);
	for (i = 0U; i < r->nref && i < p->reference_count; i++) {
		CHECK(p->reference_slot[i] == r->rslot[i]);
		CHECK(p->reference_poc[i] == r->rpoc[i]);
		CHECK((int)p->reference_long[i] == r->rlong[i]);
	}
	CHECK(p->setup_slot == H265_DPB_NO_SLOT);
}

/* Adds one entry to a list of a generated reference picture set (a list at its capacity takes none). */
static void
gen_add(struct h265sw_rps_pocs *pocs, unsigned category, int32_t poc, unsigned bits)
{
	int32_t lsb;

	lsb = (int32_t)m_lsb(poc, bits);
	switch (category) {
	case 0:
		if (pocs->num_st_before < H265SW_MAX_RPS)
			pocs->st_before[pocs->num_st_before++] = poc;
		break;
	case 1:
		if (pocs->num_st_after < H265SW_MAX_RPS)
			pocs->st_after[pocs->num_st_after++] = poc;
		break;
	case 2:
		if (pocs->num_st_foll < H265SW_MAX_RPS)
			pocs->st_foll[pocs->num_st_foll++] = poc;
		break;
	case 3:
		if (pocs->num_lt_curr < H265SW_MAX_RPS) {
			pocs->lt_curr[pocs->num_lt_curr] = lsb;
			pocs->lt_curr_msb[pocs->num_lt_curr++] = 0U;
		}
		break;
	case 4:
		if (pocs->num_lt_curr < H265SW_MAX_RPS) {
			pocs->lt_curr[pocs->num_lt_curr] = poc;
			pocs->lt_curr_msb[pocs->num_lt_curr++] = 1U;
		}
		break;
	case 5:
		if (pocs->num_lt_foll < H265SW_MAX_RPS) {
			pocs->lt_foll[pocs->num_lt_foll] = lsb;
			pocs->lt_foll_msb[pocs->num_lt_foll++] = 0U;
		}
		break;
	default:
		if (pocs->num_lt_foll < H265SW_MAX_RPS) {
			pocs->lt_foll[pocs->num_lt_foll] = poc;
			pocs->lt_foll_msb[pocs->num_lt_foll++] = 1U;
		}
		break;
	}
}

/*
 * Generates a random reference picture set from the pictures the model
 * holds: each reference is dropped, or put in a random list (long-term
 * pictures mostly in the long-term lists), with some entries naming
 * pictures that are not there and some naming one twice.
 */
static void
gen_pocs(struct h265sw_rps_pocs *pocs, const struct model *m, unsigned bits)
{
	unsigned i;
	unsigned category;
	unsigned extras;
	int32_t poc;
	int32_t held[H265_DPB_PICTURES];
	unsigned held_count;

	memset(pocs, 0, sizeof(*pocs));
	held_count = 0U;
	for (i = 0U; i < H265_DPB_PICTURES; i++) {
		if (m->p[i].present == 0 || m->p[i].ref == 0)
			continue;
		held[held_count++] = m->p[i].poc;
		if (random_next() % 100U < 10U)
			continue;
		category = random_next() % 7U;
		if (m->p[i].ref == 2 && random_next() % 100U < 80U)
			category = 3U + random_next() % 4U;
		gen_add(pocs, category, m->p[i].poc, bits);
		if (random_next() % 100U < 4U)
			gen_add(pocs, random_next() % 7U, m->p[i].poc, bits);
	}
	extras = 0U;
	if (random_next() % 4U == 0U)
		extras = 1U + random_next() % 3U;
	while (extras > 0U) {
		poc = (int32_t)(random_next() % 200U) - 100;
		if (held_count != 0U && random_next() % 2U == 0U)
			poc = held[random_next() % held_count] + (int32_t)(random_next() % 5U) - 2;
		gen_add(pocs, random_next() % 7U, poc, bits);
		extras--;
	}
}

/* The slots of outputs waiting for the test to release them, and how many there are. */
static int pending[256];
static unsigned pending_count;

/* Remembers the slots of outputs as not yet released. */
static void
pending_add(const struct h265_dpb_output *out, unsigned n)
{
	unsigned i;

	for (i = 0U; i < n; i++) {
		if (pending_count < sizeof(pending) / sizeof(pending[0]))
			pending[pending_count++] = out[i].slot;
	}
}

/* Releases each pending slot with a probability, in both the buffer and the model (all of them when all is 1). */
static void
pending_release(struct h265_dpb *d, struct model *m, int all)
{
	unsigned i;

	i = 0U;
	while (i < pending_count) {
		if (all != 0 || random_next() % 100U < 40U) {
			h265_dpb_release(d, pending[i]);
			if (pending[i] >= 0 && (unsigned)pending[i] < m->slots)
				m->busy &= ~((uint32_t)1U << pending[i]);
			pending[i] = pending[pending_count - 1U];
			pending_count--;
		} else {
			i++;
		}
	}
}

/*
 * Runs a random stream of pictures through the buffer and the model: random
 * SPS values (size, reorder, latency, sub-layer, POC bits), IRAP pictures
 * with and without NoOutputOfPriorPicsFlag, random reference picture sets
 * including broken ones, random PicOutputFlag, random slot releases (so
 * EBUSY happens), skipped pictures, flushes and seeks.  Each step's
 * results and the whole buffer are compared with the model, and the
 * invariants are checked.
 */
static void
check_random(
	unsigned pictures)
{
	struct model m;
	struct mplan mp;
	struct h265sw_rps_pocs pocs;
	struct h265_dpb_output end_output[H265_DPB_MAX_OUTPUT];
	struct h265_dpb_output model_output[H265_DPB_MAX_OUTPUT];
	unsigned end_count;
	unsigned model_count;
	unsigned iteration;
	unsigned slots;
	unsigned size;
	unsigned reorder;
	unsigned plus1;
	unsigned bits;
	unsigned tries;
	unsigned layers;
	unsigned i;
	unsigned roll;
	uint64_t id;
	int32_t poc;
	int32_t last_poc;
	int irap;
	int noout;
	int output;
	int first;
	int rc;
	int slot;
	int model_slot;

	slots = 16U + random_next() % 17U;
	h265_dpb_init(&dpb, slots);
	m_init(&m, slots);
	pending_count = 0U;
	size = 4U;
	reorder = 2U;
	plus1 = 0U;
	bits = 4U;
	layers = 0U;
	id = 0U;
	last_poc = 0;
	first = 1;
	for (iteration = 0U;
	     iteration < pictures;
	     iteration++) {
		roll = random_next() % 1000U;
		if (roll < 4U) {
			rc = h265_dpb_flush(&dpb, end_output, &end_count);
			CHECK(rc == 0);
			model_count = 0U;
			while (m_bump(&m, model_output, &model_count) != 0) {
			}
			for (i = 0U; i < H265_DPB_PICTURES; i++)
				m_clear(&m.p[i]);
			m.started = 0;
			CHECK(end_count == model_count);
			for (i = 0U; i < model_count && i < end_count; i++) {
				CHECK(end_output[i].slot == model_output[i].slot);
				CHECK(end_output[i].poc == model_output[i].poc);
				CHECK(end_output[i].id == model_output[i].id);
			}
			pending_add(end_output, end_count);
			compare_state(&dpb, &m);
			check_state(&dpb);
			first = 1;
			continue;
		}
		if (roll < 6U) {
			h265_dpb_clear(&dpb);
			for (i = 0U; i < H265_DPB_PICTURES; i++)
				m_clear(&m.p[i]);
			m.started = 0;
			m.in_pic = 0;
			m.cur = -1;
			compare_state(&dpb, &m);
			check_state(&dpb);
			first = 1;
			continue;
		}

		/* The picture: IRAP pictures start a run with new SPS values; others change them rarely. */
		irap = (first != 0 || random_next() % 12U == 0U);
		noout = (random_next() % 4U == 0U);
		output = (random_next() % 10U != 0U);
		if (irap != 0 || random_next() % 50U == 0U) {
			size = 1U + random_next() % 16U;
			reorder = random_next() % (size + 1U);
			if (random_next() % 10U == 0U)
				reorder = random_next() % 16U;
			plus1 = 0U;
			if (random_next() % 3U == 0U)
				plus1 = random_next() % 5U;
			bits = 4U + random_next() % 5U;
			layers = random_next() % 7U;
		}
		memset(&sps_store, 0, sizeof(sps_store));
		for (i = 0U; i < H265SW_MAX_SUB_LAYERS; i++) {
			sps_store.max_dec_pic_buffering_minus1[i] = random_next() % 16U;
			sps_store.max_num_reorder_pics[i] = random_next() % 16U;
			sps_store.max_latency_increase_plus1[i] = random_next() % 6U;
		}
		sps_store.max_sub_layers_minus1 = layers;
		sps_store.log2_max_poc_lsb = bits;
		sps_store.max_dec_pic_buffering_minus1[layers] = size - 1U;
		sps_store.max_num_reorder_pics[layers] = reorder;
		sps_store.max_latency_increase_plus1[layers] = plus1;
		if (irap != 0) {
			poc = (int32_t)(random_next() % 200U) - 50;
		} else {
			poc = last_poc + (int32_t)(random_next() % 25U) - 8;
		}
		last_poc = poc;
		gen_pocs(&pocs, &m, bits);
		id++;
		first = 0;

		m_begin(&m, size, reorder, plus1, bits, &pocs, irap, noout, &mp);
		rc = h265_dpb_begin(&dpb, &sps_store, &pocs, irap, noout, &plan);
		CHECK(rc == mp.rc);
		compare_plan(&plan, &mp);
		compare_state(&dpb, &m);
		check_state(&dpb);
		pending_add(plan.output, plan.output_count);
		if (rc != 0) {
			CHECK(dpb.in_picture == 0);
			pending_release(&dpb, &m, 0);
			continue;
		}
		CHECK(dpb.size == size);
		CHECK(dpb.reorder == reorder);

		/* Takes a slot, releasing outputs while every slot is held. */
		tries = 0U;
		for (;;) {
			slot = (int)H265_DPB_NO_SLOT;
			rc = h265_dpb_take_slot(&dpb, &plan);
			model_slot = m_take(&m);
			if (rc == 0) {
				CHECK(model_slot >= 0);
				CHECK(plan.setup_slot == model_slot);
				slot = plan.setup_slot;
				break;
			}
			CHECK(rc == EBUSY);
			CHECK(model_slot < 0);
			tries++;
			if (tries > 40U || pending_count == 0U) {
				CHECK(0);
				break;
			}
			pending_release(&dpb, &m, 1);
		}
		if (slot < 0) {
			h265_dpb_abort(&dpb);
			m.in_pic = 0;
			m.cur = -1;
			continue;
		}
		compare_state(&dpb, &m);
		check_plan_refs(&dpb, &plan);
		if (random_next() % 5U == 0U) {
			rc = h265_dpb_take_slot(&dpb, &plan);
			CHECK(rc == 0);
			CHECK(plan.setup_slot == slot);
		}

		/* Ends the picture, or skips it. */
		if (random_next() % 30U == 0U) {
			h265_dpb_abort(&dpb);
			m.in_pic = 0;
			m.cur = -1;
		} else {
			rc = h265_dpb_end(&dpb, &plan, poc, output, id, end_output, &end_count);
			CHECK(rc == 0);
			m_end(&m, poc, output, id, model_output, &model_count);
			CHECK(end_count == model_count);
			for (i = 0U; i < model_count && i < end_count; i++) {
				CHECK(end_output[i].slot == model_output[i].slot);
				CHECK(end_output[i].poc == model_output[i].poc);
				CHECK(end_output[i].id == model_output[i].id);
			}
			pending_add(end_output, end_count);
		}
		compare_state(&dpb, &m);
		check_state(&dpb);
		CHECK(dpb.in_picture == 0);
		pending_release(&dpb, &m, 0);
	}
}

/* Entry point of the test. */
int
main(void)
{
	unsigned run;

	check_idr_only();
	check_ippp();
	check_hierarchical();
	check_latency();
	check_size_bump();
	check_evict();
	check_long_term();
	check_irap();
	check_missing();
	check_all_curr();
	check_sixteen_slots();
	check_slots();
	check_calls();
	check_limits();
	check_foreign_slots();
	check_abort();
	check_flush_clear();
	for (run = 0U;
	     run < 10U;
	     run++)
		check_random(10000U);
	return check_finish("dpb");
}
