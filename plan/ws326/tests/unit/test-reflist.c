/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the reference picture lists of the software H.264 decoder: the
 * picture numbers (8.2.4.1), the initial lists of P and B slices
 * (8.2.4.2.1, 8.2.4.2.3), their cutting and padding (8.2.4.2), the
 * modification by short-term and long-term pictures (8.2.4.3.1,
 * 8.2.4.3.2), the stand-ins for positions without a picture, the search
 * for a frame in list 0, and random hostile input.
 *
 * The expected values are worked out by hand from the clauses named at
 * each check.  The DPB is filled by writing its entries directly: each
 * entry gets a frame of a pool, a marking, a frame_num and a
 * LongTermFrameIdx, and the frame gets its PicOrderCnt.  A check names a
 * frame by the buffer (slot) of the DPB that holds it.
 */

#include "check.h"

#include "userland/desktop/libmedia/h264sw-reflist.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The buffers of the DPB the checks fill. */
#define TEST_SLOTS H264SW_MAX_DPB_FRAMES

/* The most entries a check lists. */
#define TEST_LIST_MAX 40U

/*
 * Everything one check needs: the DPB with its frames, the pool they come
 * from, the slice header being built, and the lists built from them.
 */
struct env {
	struct h264sw_dpb dpb;
	struct h264sw_frame_pool *pool;
	struct h264sw_frame *frame[TEST_SLOTS];
	struct h264sw_slice_header header;
	struct h264sw_reflist lists;
	uint32_t max_frame_num;
	unsigned poc_type;
};

/*
 * What one position of a list must hold: the DPB buffer of its frame,
 * the picture number, whether it is long-term, and whether the position
 * had no picture.
 */
struct expect {
	unsigned slot;
	int32_t pic_num;
	int long_term;
	int missing;
};

/*
 * The state of the pseudo-random generator of the hostile-input check.
 *
 * It lives for one run of the program and gives the same sequence each
 * time, so a failure repeats.
 */
static uint32_t rng_state;

/*
 * Gives the next pseudo-random number (a linear congruential generator).
 */
static uint32_t
rng_next(void)
{
	/* The multiplier and increment of the generator; the high bits are the better ones. */
	rng_state = rng_state * 1664525U + 1013904223U;

	/* Hands out the high half, mixed with the low one so small ranges vary. */
	return (rng_state >> 8) ^ (rng_state << 3);
}

/*
 * Prepares an environment: an empty DPB, a pool of one-macroblock frames,
 * and a header for no slice yet.
 */
static void
env_setup(
	struct env *env,
	uint32_t max_frame_num)
{
	int error;

	/* Everything not named stays 0. */
	memset(env, 0, sizeof(*env));
	env->max_frame_num = max_frame_num;
	env->poc_type = 2U;
	env->dpb.size = H264SW_MAX_DPB_FRAMES;
	env->dpb.max_frame_num = max_frame_num;
	env->dpb.max_long_term_frame_idx = -1;

	/* Makes the pool the frames come from. */
	error = h264sw_frame_pool_create(1U, 1U, 0U, 0U, 16U, 16U, H264SW_FRAME_POOL_MAX, &env->pool);
	CHECK(error == 0);
}

/*
 * Ends an environment: drops the DPB's frames and destroys the pool.
 *
 * The address sanitizer then reports any frame left referenced.
 */
static void
env_teardown(
	struct env *env)
{
	/* Releases the DPB's reference of every frame. */
	h264sw_dpb_clear(&env->dpb);

	/* Destroys the pool; its frames are all free by now. */
	h264sw_frame_pool_destroy(env->pool);
	env->pool = NULL;
}

/*
 * Puts a frame in a buffer of the DPB.
 *
 * The frame gets the frame_num and PicOrderCnt; the entry gets the
 * marking and LongTermFrameIdx.
 */
static void
env_add(
	struct env *env,
	unsigned slot,
	unsigned marking,
	uint32_t frame_num,
	uint32_t long_idx,
	int32_t poc)
{
	struct h264sw_frame *frame;

	/* Takes a frame from the pool. */
	frame = h264sw_frame_get(env->pool);
	CHECK(frame != NULL);
	if (frame == NULL)
		return;

	/* The values the picture left. */
	frame->frame_num = frame_num;
	frame->poc = poc;

	/* The entry holds the frame's only reference. */
	env->dpb.entry[slot].frame = frame;
	env->dpb.entry[slot].reference = marking;
	env->dpb.entry[slot].long_term_frame_idx = long_idx;
	env->dpb.entry[slot].output_needed = 0;
	env->dpb.entry[slot].frame_num = frame_num;
	env->frame[slot] = frame;
}

/*
 * Starts a slice header: the slice type and the length of each list.
 */
static void
header_set(
	struct env *env,
	unsigned slice_type,
	unsigned active0,
	unsigned active1)
{
	/* Clears every modification and sets the fields the lists read. */
	memset(&env->header, 0, sizeof(env->header));
	env->header.slice_type = slice_type;
	env->header.num_ref_idx_active[0] = active0;
	env->header.num_ref_idx_active[1] = active1;
}

/*
 * Appends one modification operation to a list of the header.
 */
static void
header_mod(
	struct env *env,
	unsigned which,
	unsigned idc,
	uint32_t value)
{
	unsigned index;

	/* The operation goes after the ones already there. */
	index = env->header.modification_count[which];
	CHECK(index < H264SW_MAX_MODIFICATIONS);
	if (index >= H264SW_MAX_MODIFICATIONS)
		return;
	env->header.modification[which][index].idc = idc;
	env->header.modification[which][index].value = value;
	env->header.modification_count[which] = index + 1U;
}

/*
 * Builds the lists of the environment for the current picture.
 */
static int
env_build(
	struct env *env,
	uint32_t frame_num,
	int32_t poc)
{
	/* The DPB, the header and the current picture's numbers. */
	return h264sw_reflist_build(&env->dpb, &env->header, frame_num, env->max_frame_num, poc, env->poc_type, &env->lists);
}

/*
 * Checks one built list against the positions it must hold.
 *
 * The name says which check this is, for the message of a failure.
 */
static void
check_list(
	const struct env *env,
	const char *name,
	unsigned which,
	const struct expect *expected,
	unsigned count)
{
	unsigned index;
	int good;

	/* The length of the list. */
	good = (env->lists.count[which] == count);
	if (!good)
		printf("%s: list %u has %u entries, expected %u\n", name, which, env->lists.count[which], count);
	CHECK(good);
	if (!good)
		return;

	/* Every position: the frame, the numbers and the marks. */
	for (index = 0U;
	     index < count;
	     index++) {
		good = 1;
		if (env->lists.list[which][index].frame != env->frame[expected[index].slot])
			good = 0;
		if (env->lists.list[which][index].pic_num != expected[index].pic_num)
			good = 0;
		if (env->lists.list[which][index].long_term != expected[index].long_term)
			good = 0;
		if (env->lists.list[which][index].missing != expected[index].missing)
			good = 0;
		if (env->lists.list[which][index].poc != env->frame[expected[index].slot]->poc)
			good = 0;
		if (!good) {
			printf("%s: list %u position %u differs (slot %u, pic_num %d, long %d, missing %d)\n",
			    name, which, index,
			    expected[index].slot, (int)env->lists.list[which][index].pic_num,
			    env->lists.list[which][index].long_term, env->lists.list[which][index].missing);
		}
		CHECK(good);
	}
}

/*
 * Checks the initial P list of the example in 8.2.4.2.1.
 *
 * Three short-term frames with PicNum 300, 302 and 303 and two long-term
 * frames with LongTermPicNum 0 and 3 give the order 303, 302, 300, long
 * 0, long 3.  A non-existing frame and a frame that is no longer used
 * for reference are placed among them: the first takes its place by
 * PicNum, the second is not in the list.
 */
static void
test_p_initial(void)
{
	static const struct expect five[] = {
		{ 1U, 303, 0, 0 },
		{ 3U, 302, 0, 0 },
		{ 0U, 300, 0, 0 },
		{ 4U, 0, 1, 0 },
		{ 2U, 3, 1, 0 }
	};
	static const struct expect three[] = {
		{ 1U, 303, 0, 0 },
		{ 3U, 302, 0, 0 },
		{ 0U, 300, 0, 0 }
	};
	static const struct expect seven[] = {
		{ 1U, 303, 0, 0 },
		{ 3U, 302, 0, 0 },
		{ 0U, 300, 0, 0 },
		{ 4U, 0, 1, 0 },
		{ 2U, 3, 1, 0 },
		{ 1U, 303, 0, 1 },
		{ 1U, 303, 0, 1 }
	};
	static const struct expect with_gap[] = {
		{ 1U, 303, 0, 0 },
		{ 3U, 302, 0, 0 },
		{ 5U, 301, 0, 0 },
		{ 0U, 300, 0, 0 },
		{ 4U, 0, 1, 0 },
		{ 2U, 3, 1, 0 }
	};
	struct env env;
	int error;

	/* MaxFrameNum 512 keeps the numbers of the example without a wrap; the current picture has frame_num 304. */
	env_setup(&env, 512U);
	env_add(&env, 0U, H264SW_REF_SHORT, 300U, 0U, 100);
	env_add(&env, 1U, H264SW_REF_SHORT, 303U, 0U, 130);
	env_add(&env, 2U, H264SW_REF_LONG, 7U, 3U, 50);
	env_add(&env, 3U, H264SW_REF_SHORT, 302U, 0U, 120);
	env_add(&env, 4U, H264SW_REF_LONG, 9U, 0U, 40);
	env_add(&env, 6U, H264SW_REF_UNUSED, 299U, 0U, 90);

	/* All five entries of the example, in the order the clause lists them. */
	header_set(&env, H264SW_SLICE_P, 5U, 0U);
	error = env_build(&env, 304U, 140);
	CHECK(error == 0);
	CHECK(env.lists.count[1] == 0U);
	check_list(&env, "p five", 0U, five, 5U);

	/* A shorter list keeps the first entries (8.2.4.2). */
	header_set(&env, H264SW_SLICE_P, 3U, 0U);
	error = env_build(&env, 304U, 140);
	CHECK(error == 0);
	check_list(&env, "p three", 0U, three, 3U);

	/* A longer list is padded with positions without a picture, which stand in with the first entry. */
	header_set(&env, H264SW_SLICE_P, 7U, 0U);
	error = env_build(&env, 304U, 140);
	CHECK(error == 0);
	check_list(&env, "p seven", 0U, seven, 7U);

	/* A non-existing frame is in the list like the others (the P rule takes no exception). */
	env_add(&env, 5U, H264SW_REF_SHORT, 301U, 0U, 110);
	env.frame[5]->non_existing = 1;
	header_set(&env, H264SW_SLICE_P, 6U, 0U);
	error = env_build(&env, 304U, 140);
	CHECK(error == 0);
	check_list(&env, "p non-existing", 0U, with_gap, 6U);

	/* An SP slice is built like a P slice. */
	header_set(&env, H264SW_SLICE_SP, 3U, 0U);
	error = env_build(&env, 304U, 140);
	CHECK(error == 0);
	CHECK(env.lists.count[0] == 3U);
	CHECK(env.lists.count[1] == 0U);
	CHECK(env.lists.list[0][0].frame == env.frame[1]);

	env_teardown(&env);
}

/*
 * Checks FrameNumWrap across the wrap of frame_num (8-27, 8-28).
 *
 * With MaxFrameNum 16 and the current frame_num 2, the frames with
 * frame_num 14 and 15 were decoded before the wrap and have FrameNumWrap
 * -2 and -1; those with 0 and 1 keep their numbers.
 */
static void
test_p_wrap(void)
{
	static const struct expect wrapped[] = {
		{ 3U, 1, 0, 0 },
		{ 2U, 0, 0, 0 },
		{ 1U, -1, 0, 0 },
		{ 0U, -2, 0, 0 }
	};
	struct env env;
	int error;

	/* Four references with frame_num 14, 15, 0 and 1. */
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_SHORT, 14U, 0U, 20);
	env_add(&env, 1U, H264SW_REF_SHORT, 15U, 0U, 22);
	env_add(&env, 2U, H264SW_REF_SHORT, 0U, 0U, 24);
	env_add(&env, 3U, H264SW_REF_SHORT, 1U, 0U, 26);

	/* The descending PicNum order is 1, 0, -1 (frame_num 15), -2 (frame_num 14). */
	header_set(&env, H264SW_SLICE_P, 4U, 0U);
	error = env_build(&env, 2U, 28);
	CHECK(error == 0);
	check_list(&env, "p wrap", 0U, wrapped, 4U);

	env_teardown(&env);
}

/*
 * Checks the initial lists of a B slice (8.2.4.2.3).
 *
 * The current picture has order count 8.  The short-term frames have
 * counts 0, 2, 4, 6, 10 and 12, the long-term frames LongTermPicNum 1 and
 * 0.  List 0 is 6, 4, 2, 0, 10, 12 and the long-term frames; list 1 is
 * 10, 12, 6, 4, 2, 0 and the long-term frames.
 */
static void
test_b_initial(void)
{
	static const struct expect list0[] = {
		{ 3U, 4, 0, 0 },
		{ 2U, 3, 0, 0 },
		{ 1U, 2, 0, 0 },
		{ 0U, 1, 0, 0 },
		{ 4U, 5, 0, 0 },
		{ 5U, 6, 0, 0 },
		{ 7U, 0, 1, 0 },
		{ 6U, 1, 1, 0 }
	};
	static const struct expect list1[] = {
		{ 4U, 5, 0, 0 },
		{ 5U, 6, 0, 0 },
		{ 3U, 4, 0, 0 },
		{ 2U, 3, 0, 0 },
		{ 1U, 2, 0, 0 },
		{ 0U, 1, 0, 0 },
		{ 7U, 0, 1, 0 },
		{ 6U, 1, 1, 0 }
	};
	static const struct expect cut1[] = {
		{ 4U, 5, 0, 0 },
		{ 5U, 6, 0, 0 },
		{ 3U, 4, 0, 0 },
		{ 2U, 3, 0, 0 }
	};
	static const struct expect cut0[] = {
		{ 3U, 4, 0, 0 },
		{ 2U, 3, 0, 0 },
		{ 1U, 2, 0, 0 }
	};
	struct env env;
	int error;

	/* Frame_num 1..6 for the short-term frames (current frame_num 7), in a scrambled order of buffers. */
	env_setup(&env, 16U);
	env_add(&env, 4U, H264SW_REF_SHORT, 5U, 0U, 10);
	env_add(&env, 0U, H264SW_REF_SHORT, 1U, 0U, 0);
	env_add(&env, 6U, H264SW_REF_LONG, 8U, 1U, 3);
	env_add(&env, 2U, H264SW_REF_SHORT, 3U, 0U, 4);
	env_add(&env, 5U, H264SW_REF_SHORT, 6U, 0U, 12);
	env_add(&env, 7U, H264SW_REF_LONG, 9U, 0U, 5);
	env_add(&env, 1U, H264SW_REF_SHORT, 2U, 0U, 2);
	env_add(&env, 3U, H264SW_REF_SHORT, 4U, 0U, 6);

	/* The two full lists. */
	header_set(&env, H264SW_SLICE_B, 8U, 8U);
	error = env_build(&env, 7U, 8);
	CHECK(error == 0);
	check_list(&env, "b list0", 0U, list0, 8U);
	check_list(&env, "b list1", 1U, list1, 8U);

	/* Each list is cut to its own length. */
	header_set(&env, H264SW_SLICE_B, 3U, 4U);
	error = env_build(&env, 7U, 8);
	CHECK(error == 0);
	check_list(&env, "b cut0", 0U, cut0, 3U);
	check_list(&env, "b cut1", 1U, cut1, 4U);

	/* A list longer than the references is padded with positions that stand in with the list's first entry. */
	header_set(&env, H264SW_SLICE_B, 10U, 1U);
	error = env_build(&env, 7U, 8);
	CHECK(error == 0);
	CHECK(env.lists.count[0] == 10U);
	CHECK(env.lists.list[0][7].missing == 0);
	CHECK(env.lists.list[0][8].missing == 1);
	CHECK(env.lists.list[0][9].missing == 1);
	CHECK(env.lists.list[0][8].frame == env.frame[3]);
	CHECK(env.lists.list[0][9].frame == env.frame[3]);
	CHECK(env.lists.count[1] == 1U);
	CHECK(env.lists.list[1][0].frame == env.frame[4]);

	env_teardown(&env);
}

/*
 * Checks the frames of a gap in frame_num in the lists of a B slice.
 *
 * With pic_order_cnt_type 0 a non-existing frame has no order count, so
 * 8.2.4.2.3 leaves it out of both initial lists; with types 1 and 2 it
 * has one and is ordered like any other frame.  Frames at counts 2
 * (frame_num 1), 6 (frame_num 2, non-existing) and 12 (frame_num 3),
 * current count 8: type 0 gives list 0 = 2, 12 and list 1 = 12, 2;
 * type 2 gives list 0 = 6, 2, 12 and list 1 = 12, 6, 2.  A P slice keeps
 * the non-existing frame whatever the type (8.2.4.2.1 takes no exception).
 */
static void
test_b_non_existing(void)
{
	static const struct expect type0_list0[] = {
		{ 0U, 1, 0, 0 },
		{ 2U, 3, 0, 0 }
	};
	static const struct expect type0_list1[] = {
		{ 2U, 3, 0, 0 },
		{ 0U, 1, 0, 0 }
	};
	static const struct expect type2_list0[] = {
		{ 1U, 2, 0, 0 },
		{ 0U, 1, 0, 0 },
		{ 2U, 3, 0, 0 }
	};
	static const struct expect type2_list1[] = {
		{ 2U, 3, 0, 0 },
		{ 1U, 2, 0, 0 },
		{ 0U, 1, 0, 0 }
	};
	static const struct expect p_list[] = {
		{ 2U, 3, 0, 0 },
		{ 1U, 2, 0, 0 },
		{ 0U, 1, 0, 0 }
	};
	struct env env;
	int error;

	/* Three short-term frames, the middle one standing for a gap. */
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_SHORT, 1U, 0U, 2);
	env_add(&env, 1U, H264SW_REF_SHORT, 2U, 0U, 6);
	env.frame[1]->non_existing = 1;
	env_add(&env, 2U, H264SW_REF_SHORT, 3U, 0U, 12);

	/* Type 0: the non-existing frame is in neither list. */
	env.poc_type = 0U;
	header_set(&env, H264SW_SLICE_B, 2U, 2U);
	error = env_build(&env, 4U, 8);
	CHECK(error == 0);
	check_list(&env, "b type0 list0", 0U, type0_list0, 2U);
	check_list(&env, "b type0 list1", 1U, type0_list1, 2U);

	/* Type 0 with a longer list: the place the left-out frame would take is padded as missing. */
	header_set(&env, H264SW_SLICE_B, 3U, 3U);
	error = env_build(&env, 4U, 8);
	CHECK(error == 0);
	CHECK(env.lists.list[0][2].missing == 1);
	CHECK(env.lists.list[1][2].missing == 1);

	/* Types 1 and 2: the frame is ordered by its count like the others. */
	env.poc_type = 2U;
	header_set(&env, H264SW_SLICE_B, 3U, 3U);
	error = env_build(&env, 4U, 8);
	CHECK(error == 0);
	check_list(&env, "b type2 list0", 0U, type2_list0, 3U);
	check_list(&env, "b type2 list1", 1U, type2_list1, 3U);
	env.poc_type = 1U;
	error = env_build(&env, 4U, 8);
	CHECK(error == 0);
	check_list(&env, "b type1 list0", 0U, type2_list0, 3U);

	/* A P slice keeps the frame with type 0 too. */
	env.poc_type = 0U;
	header_set(&env, H264SW_SLICE_P, 3U, 0U);
	error = env_build(&env, 4U, 8);
	CHECK(error == 0);
	check_list(&env, "p type0", 0U, p_list, 3U);

	env_teardown(&env);
}

/*
 * Checks a short-term frame with the same count as the current picture.
 *
 * The clause puts the frames with a count below the current one first
 * and "all of the remaining" ones after them in ascending order (list 0),
 * and the frames above the current one first and the remaining ones
 * after them in descending order (list 1).  So a frame with an equal
 * count follows the lower frames in list 0 and ends list 1's ascending
 * part.  Counts 4, 8 and 12 with the current count 8: list 0 is 4, 8,
 * 12 and list 1 is 12, 8, 4.
 */
static void
test_b_equal_poc(void)
{
	static const struct expect list0[] = {
		{ 0U, 1, 0, 0 },
		{ 1U, 2, 0, 0 },
		{ 2U, 3, 0, 0 }
	};
	static const struct expect list1[] = {
		{ 2U, 3, 0, 0 },
		{ 1U, 2, 0, 0 },
		{ 0U, 1, 0, 0 }
	};
	struct env env;
	int error;

	/* Three frames whose counts are below, equal to and above the current one. */
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_SHORT, 1U, 0U, 4);
	env_add(&env, 1U, H264SW_REF_SHORT, 2U, 0U, 8);
	env_add(&env, 2U, H264SW_REF_SHORT, 3U, 0U, 12);

	/* The two lists. */
	header_set(&env, H264SW_SLICE_B, 3U, 3U);
	error = env_build(&env, 4U, 8);
	CHECK(error == 0);
	check_list(&env, "b equal list0", 0U, list0, 3U);
	check_list(&env, "b equal list1", 1U, list1, 3U);

	env_teardown(&env);
}

/*
 * Checks the switch of the first two entries of list 1 (8.2.4.2.3 step 3).
 *
 * When every reference is before the current picture, list 1 comes out
 * equal to list 0 and has its first two entries switched.  The
 * comparison is made on the whole lists, before they are cut to
 * num_ref_idx_active (8.2.4.2).  One reference, or lists that differ, are
 * not switched.
 */
static void
test_b_switch(void)
{
	static const struct expect list0[] = {
		{ 1U, 2, 0, 0 },
		{ 0U, 1, 0, 0 }
	};
	static const struct expect list1[] = {
		{ 0U, 1, 0, 0 },
		{ 1U, 2, 0, 0 }
	};
	static const struct expect cut1[] = {
		{ 0U, 1, 0, 0 }
	};
	static const struct expect single[] = {
		{ 0U, 1, 0, 0 }
	};
	static const struct expect long0[] = {
		{ 0U, 0, 1, 0 },
		{ 1U, 1, 1, 0 }
	};
	static const struct expect long1[] = {
		{ 1U, 1, 1, 0 },
		{ 0U, 0, 1, 0 }
	};
	static const struct expect differ0[] = {
		{ 0U, 1, 0, 0 },
		{ 1U, 2, 0, 0 }
	};
	static const struct expect differ1[] = {
		{ 1U, 2, 0, 0 },
		{ 0U, 1, 0, 0 }
	};
	struct env env;
	int error;

	/* Two references before the current picture (counts 2 and 4, current 8). */
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_SHORT, 1U, 0U, 2);
	env_add(&env, 1U, H264SW_REF_SHORT, 2U, 0U, 4);

	/* List 0 is 4, 2; list 1 is first 4, 2 and then switched to 2, 4. */
	header_set(&env, H264SW_SLICE_B, 2U, 2U);
	error = env_build(&env, 3U, 8);
	CHECK(error == 0);
	check_list(&env, "switch list0", 0U, list0, 2U);
	check_list(&env, "switch list1", 1U, list1, 2U);

	/* With one position in list 1 the switched first entry is the one kept. */
	header_set(&env, H264SW_SLICE_B, 2U, 1U);
	error = env_build(&env, 3U, 8);
	CHECK(error == 0);
	check_list(&env, "switch cut1", 1U, cut1, 1U);
	env_teardown(&env);

	/* One reference only: the lists are equal but have one entry, so there is nothing to switch. */
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_SHORT, 1U, 0U, 2);
	header_set(&env, H264SW_SLICE_B, 1U, 1U);
	error = env_build(&env, 2U, 8);
	CHECK(error == 0);
	check_list(&env, "switch single0", 0U, single, 1U);
	check_list(&env, "switch single1", 1U, single, 1U);
	env_teardown(&env);

	/* Long-term references only: both lists are ascending LongTermPicNum and the first two of list 1 are switched. */
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_LONG, 1U, 0U, 2);
	env_add(&env, 1U, H264SW_REF_LONG, 2U, 1U, 4);
	header_set(&env, H264SW_SLICE_B, 2U, 2U);
	error = env_build(&env, 3U, 8);
	CHECK(error == 0);
	check_list(&env, "switch long0", 0U, long0, 2U);
	check_list(&env, "switch long1", 1U, long1, 2U);
	env_teardown(&env);

	/* One reference on each side: the lists differ and stay as they are. */
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_SHORT, 1U, 0U, 4);
	env_add(&env, 1U, H264SW_REF_SHORT, 2U, 0U, 12);
	header_set(&env, H264SW_SLICE_B, 2U, 2U);
	error = env_build(&env, 3U, 8);
	CHECK(error == 0);
	check_list(&env, "differ list0", 0U, differ0, 2U);
	check_list(&env, "differ list1", 1U, differ1, 2U);
	env_teardown(&env);
}

/*
 * Checks the modification by short-term pictures (8.2.4.3.1).
 *
 * MaxFrameNum 16, current frame_num 5, references with PicNum 1 to 4.
 * The initial list is 4, 3, 2, 1.
 *   idc 0, abs_diff_pic_num_minus1 2: picNumPred 5 - 3 = 2 (8-34); 2 goes
 *     to index 0 and its later copy is removed: 2, 4, 3, 1.
 *   idc 1, 0: picNumPred 2 + 1 = 3 (8-35); 3 goes to index 1: 2, 3, 4, 1.
 *   idc 0, 0: picNumPred 3 - 1 = 2; 2 goes to index 2, the copy at index 0
 *     stays (a picture may appear twice): 2, 3, 2, 4, and 1 falls off.
 * With a list of three the same operations give 2, 3, 2.
 */
static void
test_modify_short(void)
{
	static const struct expect four[] = {
		{ 1U, 2, 0, 0 },
		{ 2U, 3, 0, 0 },
		{ 1U, 2, 0, 0 },
		{ 3U, 4, 0, 0 }
	};
	static const struct expect three[] = {
		{ 1U, 2, 0, 0 },
		{ 2U, 3, 0, 0 },
		{ 1U, 2, 0, 0 }
	};
	static const struct expect after_two[] = {
		{ 1U, 2, 0, 0 },
		{ 2U, 3, 0, 0 },
		{ 3U, 4, 0, 0 },
		{ 0U, 1, 0, 0 }
	};
	static const struct expect first_only[] = {
		{ 1U, 2, 0, 0 },
		{ 3U, 4, 0, 0 },
		{ 2U, 3, 0, 0 },
		{ 0U, 1, 0, 0 }
	};
	static const struct expect ended[] = {
		{ 1U, 2, 0, 0 },
		{ 3U, 4, 0, 0 },
		{ 2U, 3, 0, 0 },
		{ 0U, 1, 0, 0 }
	};
	struct env env;
	int error;

	/* Frame_num 1 to 4 in buffers 0 to 3. */
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_SHORT, 1U, 0U, 2);
	env_add(&env, 1U, H264SW_REF_SHORT, 2U, 0U, 4);
	env_add(&env, 2U, H264SW_REF_SHORT, 3U, 0U, 6);
	env_add(&env, 3U, H264SW_REF_SHORT, 4U, 0U, 8);

	/* The first operation alone. */
	header_set(&env, H264SW_SLICE_P, 4U, 0U);
	header_mod(&env, 0U, H264SW_MOD_SUBTRACT, 2U);
	error = env_build(&env, 5U, 10);
	CHECK(error == 0);
	check_list(&env, "mod short first", 0U, first_only, 4U);

	/* The first two. */
	header_mod(&env, 0U, H264SW_MOD_ADD, 0U);
	error = env_build(&env, 5U, 10);
	CHECK(error == 0);
	check_list(&env, "mod short two", 0U, after_two, 4U);

	/* All three. */
	header_mod(&env, 0U, H264SW_MOD_SUBTRACT, 0U);
	error = env_build(&env, 5U, 10);
	CHECK(error == 0);
	check_list(&env, "mod short four", 0U, four, 4U);

	/* The same operations on a list of three entries. */
	env.header.num_ref_idx_active[0] = 3U;
	error = env_build(&env, 5U, 10);
	CHECK(error == 0);
	check_list(&env, "mod short three", 0U, three, 3U);

	/* An end marker (idc 3) in the array stops the loop: only the first operation applies. */
	header_set(&env, H264SW_SLICE_P, 4U, 0U);
	header_mod(&env, 0U, H264SW_MOD_SUBTRACT, 2U);
	header_mod(&env, 0U, 3U, 0U);
	header_mod(&env, 0U, H264SW_MOD_ADD, 0U);
	error = env_build(&env, 5U, 10);
	CHECK(error == 0);
	check_list(&env, "mod short ended", 0U, ended, 4U);

	env_teardown(&env);
}

/*
 * Checks the modification across the wrap of the picture numbers (8-34 to 8-36).
 *
 * MaxFrameNum 16, current frame_num 2, references with frame_num 14, 15,
 * 0 and 1 (PicNum -2, -1, 0, 1).  The initial list is 1, 0, -1, -2.
 *   idc 0, 3: picNumPred 2 - 4 = -2 < 0, so picNumNoWrap -2 + 16 = 14
 *     (8-34); 14 > CurrPicNum, so picNum 14 - 16 = -2 (8-36): the frame
 *     with frame_num 14 goes first: -2, 1, 0, -1.
 *   idc 1, 1: picNumPred 14 + 2 = 16 >= 16, so picNumNoWrap 0 (8-35);
 *     picNum 0 goes to index 1 and its later copy is removed: -2, 0, 1, -1.
 */
static void
test_modify_wrap(void)
{
	static const struct expect result[] = {
		{ 0U, -2, 0, 0 },
		{ 2U, 0, 0, 0 },
		{ 3U, 1, 0, 0 },
		{ 1U, -1, 0, 0 }
	};
	struct env env;
	int error;

	/* The references of the wrap. */
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_SHORT, 14U, 0U, 20);
	env_add(&env, 1U, H264SW_REF_SHORT, 15U, 0U, 22);
	env_add(&env, 2U, H264SW_REF_SHORT, 0U, 0U, 24);
	env_add(&env, 3U, H264SW_REF_SHORT, 1U, 0U, 26);

	/* The two operations. */
	header_set(&env, H264SW_SLICE_P, 4U, 0U);
	header_mod(&env, 0U, H264SW_MOD_SUBTRACT, 3U);
	header_mod(&env, 0U, H264SW_MOD_ADD, 1U);
	error = env_build(&env, 2U, 28);
	CHECK(error == 0);
	check_list(&env, "mod wrap", 0U, result, 4U);

	env_teardown(&env);
}

/*
 * Checks the modification by long-term pictures (8.2.4.3.2).
 *
 * Current frame_num 3, short-term frames with PicNum 1 and 2 and
 * long-term frames with LongTermPicNum 0 and 1.  The initial list is
 * 2, 1, long 0, long 1.
 *   idc 2, 1: long 1 goes to index 0: long 1, 2, 1, long 0.
 *   idc 2, 0: long 0 goes to index 1: long 1, long 0, 2, 1.
 * A short-term frame whose PicNum equals the LongTermPicNum is not
 * removed by a long-term operation, nor the long-term frame by a
 * short-term one (PicNumF and LongTermPicNumF give a value that cannot
 * match, 8-37 and 8-38).
 */
static void
test_modify_long(void)
{
	static const struct expect one[] = {
		{ 3U, 1, 1, 0 },
		{ 1U, 2, 0, 0 },
		{ 0U, 1, 0, 0 },
		{ 2U, 0, 1, 0 }
	};
	static const struct expect two[] = {
		{ 3U, 1, 1, 0 },
		{ 2U, 0, 1, 0 },
		{ 1U, 2, 0, 0 },
		{ 0U, 1, 0, 0 }
	};
	static const struct expect same_number_long[] = {
		{ 1U, 1, 1, 0 },
		{ 0U, 1, 0, 0 }
	};
	static const struct expect same_number_short[] = {
		{ 0U, 1, 0, 0 },
		{ 1U, 1, 1, 0 }
	};
	struct env env;
	int error;

	/* The four references. */
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_SHORT, 1U, 0U, 2);
	env_add(&env, 1U, H264SW_REF_SHORT, 2U, 0U, 4);
	env_add(&env, 2U, H264SW_REF_LONG, 9U, 0U, 6);
	env_add(&env, 3U, H264SW_REF_LONG, 8U, 1U, 8);

	/* One long-term operation. */
	header_set(&env, H264SW_SLICE_P, 4U, 0U);
	header_mod(&env, 0U, H264SW_MOD_LONG_TERM, 1U);
	error = env_build(&env, 3U, 10);
	CHECK(error == 0);
	check_list(&env, "mod long one", 0U, one, 4U);

	/* Two. */
	header_mod(&env, 0U, H264SW_MOD_LONG_TERM, 0U);
	error = env_build(&env, 3U, 10);
	CHECK(error == 0);
	check_list(&env, "mod long two", 0U, two, 4U);
	env_teardown(&env);

	/* A short-term frame (PicNum 1) and a long-term frame (LongTermPicNum 1) with the same number. */
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_SHORT, 1U, 0U, 2);
	env_add(&env, 1U, H264SW_REF_LONG, 9U, 1U, 4);

	/* The long-term operation moves the long-term frame; the short-term one stays. */
	header_set(&env, H264SW_SLICE_P, 2U, 0U);
	header_mod(&env, 0U, H264SW_MOD_LONG_TERM, 1U);
	error = env_build(&env, 2U, 10);
	CHECK(error == 0);
	check_list(&env, "same number long", 0U, same_number_long, 2U);

	/* The short-term operation (picNum 2 - 1) moves the short-term frame; the long-term one stays. */
	header_set(&env, H264SW_SLICE_P, 2U, 0U);
	header_mod(&env, 0U, H264SW_MOD_SUBTRACT, 0U);
	error = env_build(&env, 2U, 10);
	CHECK(error == 0);
	check_list(&env, "same number short", 0U, same_number_short, 2U);
	env_teardown(&env);
}

/*
 * Checks the modification of list 1, which has its own picNumL1Pred.
 *
 * Current frame_num 4, short-term frames with PicNum 1, 2 and 3 and
 * counts 2, 4 and 6, current count 8.  Lists: list 0 is 3, 2, 1 and list 1
 * (equal, so switched) is 2, 3, 1.  List 1 idc 0, 0: picNumL1Pred 4 - 1 = 3,
 * the frame with PicNum 3 goes first: 3, 2, 1.  List 0 idc 0, 2: picNumL0Pred
 * 4 - 3 = 1: 1, 3, 2.
 */
static void
test_modify_b(void)
{
	static const struct expect list0[] = {
		{ 0U, 1, 0, 0 },
		{ 2U, 3, 0, 0 },
		{ 1U, 2, 0, 0 }
	};
	static const struct expect list1[] = {
		{ 2U, 3, 0, 0 },
		{ 1U, 2, 0, 0 },
		{ 0U, 1, 0, 0 }
	};
	struct env env;
	int error;

	/* Three references before the current picture. */
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_SHORT, 1U, 0U, 2);
	env_add(&env, 1U, H264SW_REF_SHORT, 2U, 0U, 4);
	env_add(&env, 2U, H264SW_REF_SHORT, 3U, 0U, 6);

	/* One operation for each list. */
	header_set(&env, H264SW_SLICE_B, 3U, 3U);
	header_mod(&env, 0U, H264SW_MOD_SUBTRACT, 2U);
	header_mod(&env, 1U, H264SW_MOD_SUBTRACT, 0U);
	error = env_build(&env, 4U, 8);
	CHECK(error == 0);
	check_list(&env, "mod b list0", 0U, list0, 3U);
	check_list(&env, "mod b list1", 1U, list1, 3U);

	env_teardown(&env);
}

/*
 * Checks the positions without a picture and their stand-ins.
 *
 * A modification may name a picture the DPB lacks; the list may be
 * shorter than num_ref_idx_active.  Such a position is marked missing and
 * holds the first entry of its list that has a picture, else the first of
 * the other list, else a frame of the DPB.  A DPB without any frame is an
 * error for P and B slices; an I slice needs none.
 */
static void
test_missing(void)
{
	static const struct expect named_absent[] = {
		{ 1U, 4, 0, 1 },
		{ 1U, 4, 0, 0 },
		{ 0U, 3, 0, 0 }
	};
	static const struct expect two_absent[] = {
		{ 1U, 4, 0, 1 },
		{ 1U, 4, 0, 1 },
		{ 1U, 4, 0, 0 },
		{ 0U, 3, 0, 0 }
	};
	static const struct expect shorter[] = {
		{ 1U, 4, 0, 0 },
		{ 0U, 3, 0, 0 },
		{ 1U, 4, 0, 1 },
		{ 1U, 4, 0, 1 }
	};
	static const struct expect b_list0[] = {
		{ 1U, 2, 0, 1 }
	};
	static const struct expect b_list1[] = {
		{ 1U, 2, 0, 0 },
		{ 0U, 1, 0, 0 }
	};
	static const struct expect unused_standin[] = {
		{ 0U, 0, 0, 1 },
		{ 0U, 0, 0, 1 }
	};
	struct env env;
	int error;

	/* Two references (frame_num 3 and 4), current frame_num 5: a modification names PicNum 1, which the DPB lacks. */
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_SHORT, 3U, 0U, 6);
	env_add(&env, 1U, H264SW_REF_SHORT, 4U, 0U, 8);
	header_set(&env, H264SW_SLICE_P, 3U, 0U);
	header_mod(&env, 0U, H264SW_MOD_SUBTRACT, 3U);
	error = env_build(&env, 5U, 10);
	CHECK(error == 0);
	check_list(&env, "missing named", 0U, named_absent, 3U);

	/* Two absent pictures in a row: PicNum 1, then picNumPred 1 + 1 = 2. Both positions are missing and the later entries move down. */
	header_set(&env, H264SW_SLICE_P, 4U, 0U);
	header_mod(&env, 0U, H264SW_MOD_SUBTRACT, 3U);
	header_mod(&env, 0U, H264SW_MOD_ADD, 0U);
	error = env_build(&env, 5U, 10);
	CHECK(error == 0);
	check_list(&env, "missing two", 0U, two_absent, 4U);

	/* A list longer than the references: the extra positions are missing. */
	header_set(&env, H264SW_SLICE_P, 4U, 0U);
	error = env_build(&env, 5U, 10);
	CHECK(error == 0);
	check_list(&env, "missing shorter", 0U, shorter, 4U);

	/* The picture numbers of a long-term operation that names no picture. */
	header_set(&env, H264SW_SLICE_P, 2U, 0U);
	header_mod(&env, 0U, H264SW_MOD_LONG_TERM, 7U);
	error = env_build(&env, 5U, 10);
	CHECK(error == 0);
	CHECK(env.lists.list[0][0].missing == 1);
	CHECK(env.lists.list[0][0].frame == env.frame[1]);
	CHECK(env.lists.list[0][1].missing == 0);
	CHECK(env.lists.list[0][1].frame == env.frame[1]);
	CHECK(env.lists.list[0][1].pic_num == 4);
	env_teardown(&env);

	/* A B slice whose list 0 ends up with nothing but a missing position takes the stand-in from list 1. */
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_SHORT, 1U, 0U, 4);
	env_add(&env, 1U, H264SW_REF_SHORT, 2U, 0U, 12);
	header_set(&env, H264SW_SLICE_B, 1U, 2U);
	header_mod(&env, 0U, H264SW_MOD_SUBTRACT, 4U);
	error = env_build(&env, 3U, 8);
	CHECK(error == 0);
	check_list(&env, "missing b list0", 0U, b_list0, 1U);
	check_list(&env, "missing b list1", 1U, b_list1, 2U);
	env_teardown(&env);

	/* Only a long-term frame: a short-term operation names nothing; the stand-in is the long-term frame. */
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_LONG, 4U, 4U, 8);
	header_set(&env, H264SW_SLICE_P, 2U, 0U);
	header_mod(&env, 0U, H264SW_MOD_SUBTRACT, 0U);
	error = env_build(&env, 5U, 10);
	CHECK(error == 0);
	CHECK(env.lists.list[0][0].missing == 1);
	CHECK(env.lists.list[0][0].frame == env.frame[0]);
	CHECK(env.lists.list[0][0].long_term == 1);
	CHECK(env.lists.list[0][1].missing == 0);
	CHECK(env.lists.list[0][1].frame == env.frame[0]);
	CHECK(env.lists.list[0][1].long_term == 1);
	CHECK(env.lists.list[0][1].pic_num == 4);
	env_teardown(&env);

	/* A modification that names no picture removes nothing: the short-term frame with PicNum 0 and the long-term frame with LongTermPicNum 0 stay in the list. */
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_SHORT, 0U, 0U, 6);
	env_add(&env, 1U, H264SW_REF_SHORT, 1U, 0U, 8);
	env_add(&env, 2U, H264SW_REF_LONG, 7U, 0U, 4);
	header_set(&env, H264SW_SLICE_P, 4U, 0U);
	header_mod(&env, 0U, H264SW_MOD_SUBTRACT, 5U);
	error = env_build(&env, 2U, 10);
	CHECK(error == 0);
	CHECK(env.lists.list[0][0].missing == 1);
	CHECK(env.lists.list[0][1].frame == env.frame[1]);
	CHECK(env.lists.list[0][2].frame == env.frame[0]);
	CHECK(env.lists.list[0][3].frame == env.frame[2]);
	header_set(&env, H264SW_SLICE_P, 4U, 0U);
	header_mod(&env, 0U, H264SW_MOD_LONG_TERM, 5U);
	error = env_build(&env, 2U, 10);
	CHECK(error == 0);
	CHECK(env.lists.list[0][0].missing == 1);
	CHECK(env.lists.list[0][1].frame == env.frame[1]);
	CHECK(env.lists.list[0][2].frame == env.frame[0]);
	CHECK(env.lists.list[0][3].frame == env.frame[2]);
	env_teardown(&env);

	/* No frame is a reference: the stand-in is a frame that waits for output, with no picture number. */
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_UNUSED, 3U, 0U, 6);
	header_set(&env, H264SW_SLICE_P, 2U, 0U);
	error = env_build(&env, 5U, 10);
	CHECK(error == 0);
	check_list(&env, "missing unused", 0U, unused_standin, 2U);
	env_teardown(&env);

	/* A non-existing frame is chosen only when the DPB has no decoded frame; a decoded one is preferred. */
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_UNUSED, 3U, 0U, 6);
	env.frame[0]->non_existing = 1;
	env_add(&env, 1U, H264SW_REF_UNUSED, 4U, 0U, 8);
	header_set(&env, H264SW_SLICE_P, 1U, 0U);
	error = env_build(&env, 5U, 10);
	CHECK(error == 0);
	CHECK(env.lists.list[0][0].missing == 1);
	CHECK(env.lists.list[0][0].frame == env.frame[1]);
	env.frame[1]->non_existing = 1;
	error = env_build(&env, 5U, 10);
	CHECK(error == 0);
	CHECK(env.lists.list[0][0].frame == env.frame[0]);
	env_teardown(&env);

	/* An empty DPB cannot predict a P or a B slice; an intra slice does not need it. */
	env_setup(&env, 16U);
	header_set(&env, H264SW_SLICE_P, 1U, 0U);
	error = env_build(&env, 5U, 10);
	CHECK(error == EINVAL);
	CHECK(env.lists.count[0] == 0U);
	header_set(&env, H264SW_SLICE_B, 1U, 1U);
	error = env_build(&env, 5U, 10);
	CHECK(error == EINVAL);
	CHECK(env.lists.count[0] == 0U);
	CHECK(env.lists.count[1] == 0U);
	header_set(&env, H264SW_SLICE_I, 0U, 0U);
	error = env_build(&env, 5U, 10);
	CHECK(error == 0);
	CHECK(env.lists.count[0] == 0U);
	CHECK(env.lists.count[1] == 0U);
	header_set(&env, H264SW_SLICE_SI, 0U, 0U);
	error = env_build(&env, 5U, 10);
	CHECK(error == 0);
	CHECK(env.lists.count[0] == 0U);
	env_teardown(&env);
}

/*
 * Checks the search for a frame in list 0 (8.4.1.2.3).
 *
 * The lowest index wins when the frame is in the list twice; a frame not
 * in the list, an unknown id, and an id held only by a stand-in give -1.
 */
static void
test_find(void)
{
	struct env env;
	int error;

	/* The lists of test_modify_short: 2, 3, 2, 4 (frame_num 1 to 4 in buffers 0 to 3). */
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_SHORT, 1U, 0U, 2);
	env_add(&env, 1U, H264SW_REF_SHORT, 2U, 0U, 4);
	env_add(&env, 2U, H264SW_REF_SHORT, 3U, 0U, 6);
	env_add(&env, 3U, H264SW_REF_SHORT, 4U, 0U, 8);
	header_set(&env, H264SW_SLICE_P, 4U, 0U);
	header_mod(&env, 0U, H264SW_MOD_SUBTRACT, 2U);
	header_mod(&env, 0U, H264SW_MOD_ADD, 0U);
	header_mod(&env, 0U, H264SW_MOD_SUBTRACT, 0U);
	error = env_build(&env, 5U, 10);
	CHECK(error == 0);

	/* Frame_num 2 is at 0 and 2, 3 at 1, 4 at 3; frame_num 1 fell off the list. */
	CHECK(h264sw_reflist_find(&env.lists, env.frame[1]->id) == 0);
	CHECK(h264sw_reflist_find(&env.lists, env.frame[2]->id) == 1);
	CHECK(h264sw_reflist_find(&env.lists, env.frame[3]->id) == 3);
	CHECK(h264sw_reflist_find(&env.lists, env.frame[0]->id) == -1);
	CHECK(h264sw_reflist_find(&env.lists, 0U) == -1);
	CHECK(h264sw_reflist_find(&env.lists, 0xFFFFFFFFU) == -1);
	CHECK(h264sw_reflist_find(NULL, env.frame[1]->id) == -1);

	/* A stand-in is not the frame the stream named: PicNum 0 names no frame, so position 0 is missing and holds the frame of position 1. */
	header_set(&env, H264SW_SLICE_P, 3U, 0U);
	header_mod(&env, 0U, H264SW_MOD_SUBTRACT, 4U);
	error = env_build(&env, 5U, 10);
	CHECK(error == 0);
	CHECK(env.lists.list[0][0].missing == 1);
	CHECK(env.lists.list[0][0].frame == env.frame[3]);
	CHECK(h264sw_reflist_find(&env.lists, env.frame[3]->id) == 1);

	/* A list 0 whose every position is a stand-in finds nothing. */
	env_teardown(&env);
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_UNUSED, 1U, 0U, 2);
	header_set(&env, H264SW_SLICE_P, 2U, 0U);
	error = env_build(&env, 2U, 10);
	CHECK(error == 0);
	CHECK(h264sw_reflist_find(&env.lists, env.frame[0]->id) == -1);

	/* A count above the array is bounded, whatever the structure holds. */
	env.lists.count[0] = 0xFFFFFFFFU;
	CHECK(h264sw_reflist_find(&env.lists, 0xFFFFFFFFU) == -1);
	env_teardown(&env);
}

/*
 * Checks the refusals and the out of range input.
 *
 * Missing arguments, a slice type that is none, lengths and counts
 * outside what the syntax allows, an operation with an idc above 3, a
 * MaxFrameNum or frame_num outside its range: EINVAL with both counts 0.
 * Operations that name a number far out of range name no picture and
 * leave a missing position; operations past the end of the list change
 * nothing.
 */
static void
test_errors(void)
{
	struct env env;
	unsigned index;
	unsigned idc;
	int error;

	/* A DPB with two references. */
	env_setup(&env, 16U);
	env_add(&env, 0U, H264SW_REF_SHORT, 3U, 0U, 6);
	env_add(&env, 1U, H264SW_REF_LONG, 4U, 0U, 8);

	/* Missing arguments. */
	header_set(&env, H264SW_SLICE_P, 1U, 0U);
	CHECK(h264sw_reflist_build(NULL, &env.header, 4U, 16U, 8, 2U, &env.lists) == EINVAL);
	CHECK(h264sw_reflist_build(&env.dpb, NULL, 4U, 16U, 8, 2U, &env.lists) == EINVAL);
	CHECK(h264sw_reflist_build(&env.dpb, &env.header, 4U, 16U, 8, 2U, NULL) == EINVAL);

	/* Slice types: SP is like P; the types above 4 are not slice types. */
	header_set(&env, H264SW_SLICE_P, 1U, 0U);
	env.header.slice_type = 5U;
	CHECK(env_build(&env, 4U, 8) == EINVAL);
	env.header.slice_type = 0xFFFFFFFFU;
	CHECK(env_build(&env, 4U, 8) == EINVAL);
	CHECK(env.lists.count[0] == 0U);

	/* The list lengths: 0 and above 32 are out of range, for either list of a B slice. */
	header_set(&env, H264SW_SLICE_P, 0U, 0U);
	CHECK(env_build(&env, 4U, 8) == EINVAL);
	header_set(&env, H264SW_SLICE_P, 33U, 0U);
	CHECK(env_build(&env, 4U, 8) == EINVAL);
	header_set(&env, H264SW_SLICE_P, 0xFFFFFFFFU, 0U);
	CHECK(env_build(&env, 4U, 8) == EINVAL);
	header_set(&env, H264SW_SLICE_B, 2U, 0U);
	CHECK(env_build(&env, 4U, 8) == EINVAL);
	header_set(&env, H264SW_SLICE_B, 2U, 33U);
	CHECK(env_build(&env, 4U, 8) == EINVAL);
	header_set(&env, H264SW_SLICE_B, 32U, 32U);
	CHECK(env_build(&env, 4U, 8) == 0);
	CHECK(env.lists.count[0] == 32U);
	CHECK(env.lists.count[1] == 32U);

	/* MaxFrameNum and frame_num out of range. */
	header_set(&env, H264SW_SLICE_P, 1U, 0U);
	CHECK(h264sw_reflist_build(&env.dpb, &env.header, 0U, 0U, 8, 2U, &env.lists) == EINVAL);
	CHECK(h264sw_reflist_build(&env.dpb, &env.header, 4U, 65537U, 8, 2U, &env.lists) == EINVAL);
	CHECK(h264sw_reflist_build(&env.dpb, &env.header, 4U, 0xFFFFFFFFU, 8, 2U, &env.lists) == EINVAL);
	CHECK(h264sw_reflist_build(&env.dpb, &env.header, 16U, 16U, 8, 2U, &env.lists) == EINVAL);
	CHECK(h264sw_reflist_build(&env.dpb, &env.header, 0xFFFFFFFFU, 16U, 8, 2U, &env.lists) == EINVAL);
	CHECK(h264sw_reflist_build(&env.dpb, &env.header, 65535U, 65536U, 8, 2U, &env.lists) == 0);
	CHECK(h264sw_reflist_build(&env.dpb, &env.header, 15U, 16U, 8, 2U, &env.lists) == 0);

	/* More operations than the header keeps. */
	header_set(&env, H264SW_SLICE_P, 1U, 0U);
	env.header.modification_count[0] = H264SW_MAX_MODIFICATIONS + 1U;
	CHECK(env_build(&env, 4U, 8) == EINVAL);
	CHECK(env.lists.count[0] == 0U);
	env.header.modification_count[0] = 0xFFFFFFFFU;
	CHECK(env_build(&env, 4U, 8) == EINVAL);

	/* An idc above 3, in the first or a later position. */
	header_set(&env, H264SW_SLICE_P, 2U, 0U);
	header_mod(&env, 0U, 4U, 0U);
	CHECK(env_build(&env, 4U, 8) == EINVAL);
	CHECK(env.lists.count[0] == 0U);
	header_set(&env, H264SW_SLICE_P, 2U, 0U);
	header_mod(&env, 0U, H264SW_MOD_SUBTRACT, 0U);
	header_mod(&env, 0U, 0xFFFFFFFFU, 0U);
	CHECK(env_build(&env, 4U, 8) == EINVAL);
	CHECK(env.lists.count[0] == 0U);

	/* A difference of MaxFrameNum or more, and the largest value, name no picture. */
	header_set(&env, H264SW_SLICE_P, 2U, 0U);
	header_mod(&env, 0U, H264SW_MOD_SUBTRACT, 16U);
	CHECK(env_build(&env, 4U, 8) == 0);
	CHECK(env.lists.list[0][0].missing == 1);
	CHECK(env.lists.list[0][1].missing == 0);
	header_set(&env, H264SW_SLICE_P, 2U, 0U);
	header_mod(&env, 0U, H264SW_MOD_ADD, 0xFFFFFFFFU);
	CHECK(env_build(&env, 4U, 8) == 0);
	CHECK(env.lists.list[0][0].missing == 1);
	header_set(&env, H264SW_SLICE_P, 2U, 0U);
	header_mod(&env, 0U, H264SW_MOD_LONG_TERM, 0xFFFFFFFFU);
	CHECK(env_build(&env, 4U, 8) == 0);
	CHECK(env.lists.list[0][0].missing == 1);

	/* The largest difference that is allowed (MaxFrameNum - 1) goes round to the same number: picNum 3 - 16 + 16. */
	header_set(&env, H264SW_SLICE_P, 2U, 0U);
	header_mod(&env, 0U, H264SW_MOD_SUBTRACT, 15U);
	CHECK(env_build(&env, 3U, 8) == 0);
	CHECK(env.lists.list[0][0].missing == 0);
	CHECK(env.lists.list[0][0].frame == env.frame[0]);

	/* More operations than positions: the extra ones change nothing that is kept. */
	header_set(&env, H264SW_SLICE_P, 1U, 0U);
	for (index = 0U;
	     index < H264SW_MAX_MODIFICATIONS;
	     index++) {
		idc = H264SW_MOD_SUBTRACT;
		if (index % 2U != 0U)
			idc = H264SW_MOD_LONG_TERM;
		header_mod(&env, 0U, idc, 0U);
	}
	error = env_build(&env, 4U, 8);
	CHECK(error == 0);
	CHECK(env.lists.count[0] == 1U);
	CHECK(env.lists.list[0][0].frame != NULL);

	/* The full number of operations on the longest list. */
	header_set(&env, H264SW_SLICE_P, H264SW_MAX_LIST, 0U);
	for (index = 0U;
	     index < H264SW_MAX_MODIFICATIONS;
	     index++) {
		idc = H264SW_MOD_LONG_TERM;
		if (index % 3U == 0U)
			idc = H264SW_MOD_SUBTRACT;
		if (index % 3U == 1U)
			idc = H264SW_MOD_ADD;
		header_mod(&env, 0U, idc, index);
	}
	error = env_build(&env, 4U, 8);
	CHECK(error == 0);
	CHECK(env.lists.count[0] == H264SW_MAX_LIST);
	for (index = 0U;
	     index < H264SW_MAX_LIST;
	     index++)
		CHECK(env.lists.list[0][index].frame != NULL);

	env_teardown(&env);
}

/*
 * Checks one built list against the properties every list has.
 *
 * Every position holds a frame of the DPB; a position that has a picture
 * agrees with the entry that frame is in (the marking, the order count,
 * the picture number); a stand-in is marked missing.
 */
static void
check_properties(
	const struct env *env,
	uint32_t frame_num,
	unsigned which)
{
	const struct h264sw_ref *ref;
	const struct h264sw_dpb_entry *entry;
	unsigned index;
	unsigned slot;
	int found;
	int good;

	/* Each position of the list. */
	for (index = 0U;
	     index < env->lists.count[which];
	     index++) {
		ref = &env->lists.list[which][index];
		CHECK(ref->frame != NULL);
		if (ref->frame == NULL)
			continue;

		/* The frame is held by some entry of the DPB. */
		found = 0;
		entry = NULL;
		for (slot = 0U;
		     slot < TEST_SLOTS;
		     slot++) {
			if (env->dpb.entry[slot].frame == ref->frame && !found) {
				found = 1;
				entry = &env->dpb.entry[slot];
			}
		}
		CHECK(found);
		if (!found)
			continue;

		/* A position with a picture agrees with the entry; the numbers follow 8-27, 8-28 and 8-29. */
		if (!ref->missing) {
			good = 1;
			if (ref->poc != ref->frame->poc)
				good = 0;
			if (entry->reference == H264SW_REF_UNUSED)
				good = 0;
			if (ref->long_term != (entry->reference == H264SW_REF_LONG))
				good = 0;
			if (ref->long_term && (uint32_t)ref->pic_num != entry->long_term_frame_idx)
				good = 0;
			if (!ref->long_term && entry->frame_num > frame_num && ref->pic_num != (int32_t)entry->frame_num - (int32_t)env->max_frame_num)
				good = 0;
			if (!ref->long_term && entry->frame_num <= frame_num && ref->pic_num != (int32_t)entry->frame_num)
				good = 0;
			CHECK(good);
		}
	}
}

/*
 * Checks random hostile input.
 *
 * Random DPBs (any marking, repeated frame_num and LongTermFrameIdx,
 * extreme counts), random slice headers with random operations, random
 * frame_num and MaxFrameNum.  The call must not fail except by the
 * refusals above; a list built is full and consistent; without
 * operations the lists have no frame twice (P) and P lists are ordered by
 * descending PicNum before the long-term frames.
 */
static void
test_random(void)
{
	static const int32_t extreme[] = {
		0, 1, -1, 2147483647, (-2147483647 - 1), 100, -100
	};
	struct env env;
	unsigned iteration;
	unsigned slot;
	unsigned count;
	unsigned index;
	unsigned which;
	unsigned slice_type;
	unsigned marking;
	uint32_t max_frame_num;
	uint32_t frame_num;
	uint32_t value;
	int32_t poc;
	int32_t previous;
	int error;
	int expect_ok;
	int any_frame;
	int twice;
	unsigned other;
	int good;

	/* The sequence repeats on every run. */
	rng_state = 0x1234567U;

	/* Many random slices. */
	for (iteration = 0U;
	     iteration < 4000U;
	     iteration++) {
		max_frame_num = 16U;
		if (rng_next() % 4U == 0U)
			max_frame_num = 1U + rng_next() % 65536U;
		env_setup(&env, max_frame_num);

		/* A random DPB, a few buffers or all of them. */
		any_frame = 0;
		count = rng_next() % (TEST_SLOTS + 1U);
		for (slot = 0U;
		     slot < count;
		     slot++) {
			marking = rng_next() % 3U;
			value = rng_next() % 8U;
			if (value == 0U)
				poc = extreme[rng_next() % 7U];
			else
				poc = (int32_t)(rng_next() % 41U) - 20;
			env_add(&env, slot, marking, rng_next() % max_frame_num, rng_next() % 18U, poc);
			if (rng_next() % 8U == 0U)
				env.frame[slot]->non_existing = 1;
			any_frame = 1;
		}

		/* A random slice header: type, lengths, operations; and the SPS's order count type. */
		env.poc_type = rng_next() % 3U;
		slice_type = rng_next() % 3U;
		header_set(&env, slice_type, 1U + rng_next() % H264SW_MAX_LIST, 1U + rng_next() % H264SW_MAX_LIST);
		if (rng_next() % 3U != 0U) {
			for (which = 0U;
			     which < 2U;
			     which++) {
				count = rng_next() % (H264SW_MAX_MODIFICATIONS + 1U);
				for (index = 0U;
				     index < count;
				     index++) {
					value = rng_next() % 8U;
					if (value == 0U)
						value = rng_next();
					else
						value = rng_next() % (max_frame_num + 2U);
					header_mod(&env, which, rng_next() % 4U, value);
				}
			}
		}
		frame_num = rng_next() % max_frame_num;
		poc = (int32_t)(rng_next() % 41U) - 20;
		if (rng_next() % 16U == 0U)
			poc = extreme[rng_next() % 7U];

		/* Builds; only an intra slice or an empty DPB may refuse. */
		error = env_build(&env, frame_num, poc);
		expect_ok = (slice_type == H264SW_SLICE_I) || any_frame;
		if (expect_ok)
			CHECK(error == 0);
		else
			CHECK(error == EINVAL);
		if (error != 0) {
			CHECK(env.lists.count[0] == 0U);
			CHECK(env.lists.count[1] == 0U);
		}

		/* A list built is full and consistent. */
		if (error == 0 && slice_type != H264SW_SLICE_I) {
			CHECK(env.lists.count[0] == env.header.num_ref_idx_active[0]);
			if (slice_type == H264SW_SLICE_B)
				CHECK(env.lists.count[1] == env.header.num_ref_idx_active[1]);
			else
				CHECK(env.lists.count[1] == 0U);
			check_properties(&env, frame_num, 0U);
			check_properties(&env, frame_num, 1U);

			/* Without operations, a P list holds no frame twice among the real positions and is ordered by PicNum. */
			if (slice_type == H264SW_SLICE_P && env.header.modification_count[0] == 0U) {
				twice = 0;
				good = 1;
				for (index = 0U;
				     index < env.lists.count[0];
				     index++) {
					if (env.lists.list[0][index].missing)
						continue;
					for (other = index + 1U;
					     other < env.lists.count[0];
					     other++) {
						if (!env.lists.list[0][other].missing && env.lists.list[0][other].frame == env.lists.list[0][index].frame)
							twice = 1;
					}
				}
				CHECK(!twice);
				for (index = 1U;
				     index < env.lists.count[0];
				     index++) {
					if (env.lists.list[0][index].missing)
						continue;
					previous = env.lists.list[0][index - 1U].pic_num;
					if (env.lists.list[0][index - 1U].long_term == env.lists.list[0][index].long_term) {
						if (env.lists.list[0][index].long_term) {
							if (previous > env.lists.list[0][index].pic_num)
								good = 0;
						} else {
							if (previous < env.lists.list[0][index].pic_num)
								good = 0;
						}
					}
					if (env.lists.list[0][index - 1U].long_term && !env.lists.list[0][index].long_term)
						good = 0;
				}
				CHECK(good);
			}
		}

		env_teardown(&env);
	}
}

/*
 * Runs every check and prints the summary line.
 */
int
main(void)
{
	/* The initial lists. */
	test_p_initial();
	test_p_wrap();
	test_b_initial();
	test_b_equal_poc();
	test_b_switch();
	test_b_non_existing();

	/* The modifications. */
	test_modify_short();
	test_modify_wrap();
	test_modify_long();
	test_modify_b();

	/* The absent pictures, the search, the refusals and random input. */
	test_missing();
	test_find();
	test_errors();
	test_random();

	/* The summary. */
	return check_finish("reflist");
}
