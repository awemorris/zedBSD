/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Checks the decoded picture buffer of the software H.264 decoder: the
 * sliding window and the adaptive operations 1 to 6 (8.2.5), the IDR
 * picture and operation 5 (C.4.4), the frames that stand for gaps in
 * frame_num (8.2.5.2, C.4.2), the storage and the output order of the
 * bumping process (C.4.5, C.4.5.3), the room rule for a DPB full of
 * references, the output queue, and random hostile input.
 *
 * The expected values are worked out by hand from the clauses named at
 * each check; the traces of the output order are written out in the
 * comments of the checks.
 */

#include "check.h"

#include "userland/desktop/libmedia/h264sw-dpb.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* What the state of a frame_num is when no buffer holds a frame with it. */
#define TEST_ABSENT 9U

/* The most output order counts a check lists. */
#define TEST_LIST_MAX 64U

/* The luma sample of a grey frame (non-existing frames are filled with it). */
#define TEST_GREY 128U

/*
 * Everything one DPB check needs: the sequence parameters, the DPB with
 * its output queue and frame pool, the order count state, and what the
 * last stored picture was marked as.
 */
struct env {
	struct h264sw_sps sps;
	struct h264sw_dpb dpb;
	struct h264sw_output_queue queue;
	struct h264sw_frame_pool *pool;
	struct h264sw_poc_state poc;
	unsigned last_marking;
	uint32_t last_long_idx;
};

/*
 * The state of the pseudo-random generator of the hostile-input check.
 *
 * It lives for one run of the program and gives the same sequence each
 * time, so a failure repeats.
 */
static uint32_t rng_state;

/* Forward declarations of the static functions. */
static uint32_t rng_next(void);
static void env_setup(struct env *env, unsigned dpb_frames, unsigned refs, int reorder, unsigned poc_type, unsigned pool_limit);
static unsigned pool_free_count(const struct h264sw_frame_pool *pool);
static void env_teardown(struct env *env);
static uint32_t small_or_any(uint32_t limit);
static void header_make(struct h264sw_slice_header *header, uint32_t frame_num, unsigned ref_idc, int idr);
static void header_mmco(struct h264sw_slice_header *header, unsigned operation, uint32_t difference_minus1, uint32_t long_term_pic_num, uint32_t long_term_frame_idx, uint32_t plus1);
static void decode_at(struct env *env, const struct h264sw_slice_header *header, int32_t poc, int output, int changed);
static int32_t decode_full(struct env *env, const struct h264sw_slice_header *header, int output);
static unsigned drain(struct env *env, int32_t *pocs, unsigned max);
static void check_list(const int32_t *got, unsigned got_count, const int32_t *want, unsigned want_count);
static void check_queue(struct env *env, const int32_t *want, unsigned want_count);
static const struct h264sw_dpb_entry *find_entry(const struct h264sw_dpb *dpb, uint32_t frame_num);
static unsigned state_of(const struct h264sw_dpb *dpb, uint32_t frame_num);
static unsigned filled(const struct h264sw_dpb *dpb);
static void test_queue(void);
static void test_sliding_window(void);
static void test_mmco(void);
static void test_mmco5_output(void);
static void test_idr(void);
static void test_make_room(void);
static void test_output_order(void);
static void check_non_existing(const struct h264sw_dpb_entry *entry, uint32_t frame_num, int32_t poc);
static void test_gaps(void);
static void fill_queue(struct env *env);
static void test_queue_full(void);
static void test_errors(void);
static void test_random(uint32_t seed, unsigned dpb_frames, unsigned refs, int reorder, unsigned poc_type);

/*
 * Runs every check and prints the summary line.
 */
int
main(void)
{
	int status;

	/* Each part of the module in turn. */
	test_queue();
	test_sliding_window();
	test_mmco();
	test_mmco5_output();
	test_idr();
	test_make_room();
	test_output_order();
	test_gaps();
	test_queue_full();
	test_errors();

	/* Hostile input: several shapes of DPB and several orders of numbers. */
	test_random(1U, 1U, 1U, -1, 2U);
	test_random(2U, 2U, 2U, 0, 0U);
	test_random(3U, 4U, 3U, 1, 1U);
	test_random(4U, 4U, 8U, -1, 2U);
	test_random(5U, 6U, 0U, 2, 0U);
	test_random(6U, 16U, 16U, -1, 2U);
	test_random(7U, 16U, 4U, 3, 1U);
	test_random(8U, 3U, 3U, -1, 0U);

	/* Prints the summary and gives the exit status. */
	status = check_finish("dpb");

	/* The exit status says whether every check held. */
	return status;
}

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
 * Gives a pseudo-random operand that is half the time below the limit and
 * otherwise any value, so both ordinary and hostile operands occur.
 */
static uint32_t
small_or_any(
	uint32_t limit)
{
	uint32_t draw;
	uint32_t value;

	/* Half of the draws give a small operand. */
	draw = rng_next();
	if (draw % 2U != 0U)
		value = rng_next() % limit;
	else
		value = rng_next();

	/* The operand is small or arbitrary, as the draw said. */
	return value;
}

/*
 * Prepares an environment: a DPB of dpb_frames buffers, max_num_ref_frames
 * refs, the VUI reorder depth (negative: none sent), the order count type,
 * and a frame pool of pool_limit frames of one macroblock.
 */
static void
env_setup(
	struct env *env,
	unsigned dpb_frames,
	unsigned refs,
	int reorder,
	unsigned poc_type,
	unsigned pool_limit)
{
	int error;

	/* Everything not named stays 0; MaxFrameNum 16 makes frame_num wrap quickly. */
	memset(env, 0, sizeof(*env));
	env->sps.log2_max_frame_num = 4U;
	env->sps.max_frame_num = 16U;
	env->sps.poc_type = poc_type;
	env->sps.log2_max_poc_lsb = 4U;
	env->sps.max_num_ref_frames = refs;
	env->sps.gaps_allowed = 1;
	env->sps.width_mbs = 1U;
	env->sps.height_mbs = 1U;
	env->sps.size_mbs = 1U;
	env->sps.dpb_frames = dpb_frames;
	if (reorder >= 0) {
		env->sps.restriction_present = 1;
		env->sps.max_num_reorder_frames = (unsigned)reorder;
	}

	/* Makes the pool of one-macroblock frames. */
	error = h264sw_frame_pool_create(1U, 1U, 0U, 0U, 16U, 16U, pool_limit, &env->pool);
	CHECK(error == 0);

	/* Prepares the DPB, the queue and the state as for the start of a stream. */
	h264sw_dpb_init(&env->dpb, &env->sps);
	h264sw_output_init(&env->queue);
	h264sw_poc_init(&env->poc);
}

/*
 * Counts the frames on the pool's free list.
 */
static unsigned
pool_free_count(
	const struct h264sw_frame_pool *pool)
{
	const struct h264sw_frame *frame;
	unsigned count;

	/* Walks the list. */
	count = 0U;
	for (frame = pool->free;
	     frame != NULL;
	     frame = frame->next_free)
		count++;

	/* Hands the count to the check. */
	return count;
}

/*
 * Ends an environment: empties the queue and the DPB, checks that every
 * frame went back to the pool (no reference is left), and destroys the pool.
 */
static void
env_teardown(
	struct env *env)
{
	/* Drops what the queue and the DPB hold. */
	h264sw_output_clear(&env->queue);
	h264sw_dpb_clear(&env->dpb);

	/* Every frame the pool made must be free again: nothing leaked a reference. */
	CHECK(pool_free_count(env->pool) == env->pool->allocated);
	CHECK(env->queue.count == 0U);
	CHECK(h264sw_dpb_references(&env->dpb) == 0U);

	/* Frees the pool; the address sanitizer's leak check covers the rest. */
	h264sw_frame_pool_destroy(env->pool);
}

/*
 * Starts a slice header: a picture with frame_num, nal_ref_idc and IDR or not.
 */
static void
header_make(
	struct h264sw_slice_header *header,
	uint32_t frame_num,
	unsigned ref_idc,
	int idr)
{
	/* Everything not named stays 0. */
	memset(header, 0, sizeof(*header));
	header->nal_type = 1U;
	if (idr)
		header->nal_type = 5U;
	header->nal_ref_idc = ref_idc;
	header->idr = idr;
	header->frame_num = frame_num;
	header->slice_type = H264SW_SLICE_P;
	if (idr)
		header->slice_type = H264SW_SLICE_I;
}

/*
 * Appends a memory management control operation to a header.
 */
static void
header_mmco(
	struct h264sw_slice_header *header,
	unsigned operation,
	uint32_t difference_minus1,
	uint32_t long_term_pic_num,
	uint32_t long_term_frame_idx,
	uint32_t plus1)
{
	struct h264sw_mmco *entry;

	/* The picture now sends its marking as operations. */
	header->adaptive_marking = 1;
	entry = &header->mmco[header->mmco_count];
	header->mmco_count++;
	entry->operation = operation;
	entry->difference_of_pic_nums_minus1 = difference_minus1;
	entry->long_term_pic_num = long_term_pic_num;
	entry->long_term_frame_idx = long_term_frame_idx;
	entry->max_long_term_frame_idx_plus1 = plus1;
	if (operation == H264SW_MMCO_ALL_UNUSED)
		header->has_mmco5 = 1;
}

/*
 * Decodes one picture with an order count given by the check: a frame
 * from the pool, the removal and marking, the storage, and the release of
 * the check's own reference.
 */
static void
decode_at(
	struct env *env,
	const struct h264sw_slice_header *header,
	int32_t poc,
	int output,
	int changed)
{
	struct h264sw_frame *frame;
	unsigned marking;
	uint32_t long_idx;
	int error;

	/* Takes the frame the picture is decoded into. */
	frame = h264sw_frame_get(env->pool);
	CHECK(frame != NULL);
	if (frame == NULL)
		return;
	frame->frame_num = header->frame_num;
	frame->poc = poc;
	frame->top_poc = poc;
	frame->bottom_poc = poc;
	frame->idr = header->idr;

	/* Marks the references and removes frames (C.4.4). */
	marking = 99U;
	long_idx = 99U;
	error = h264sw_dpb_remove(&env->dpb, &env->sps, header, frame, changed, &marking, &long_idx, &env->queue);
	CHECK(error == 0);

	/* Stores the picture (C.4.5). */
	error = h264sw_dpb_store(&env->dpb, frame, marking, long_idx, output, &env->queue);
	CHECK(error == 0);

	/* Records the marking for the check, and drops the check's reference. */
	env->last_marking = marking;
	env->last_long_idx = long_idx;
	h264sw_frame_unref(frame);
}

/*
 * Decodes one picture the way the decoder does: the gaps, the order count
 * from the slice header, the picture, the end of the order count, the
 * marking and the storage.  Returns the picture's PicOrderCnt.
 */
static int32_t
decode_full(
	struct env *env,
	const struct h264sw_slice_header *header,
	int output)
{
	struct h264sw_frame *frame;
	struct h264sw_poc counts;
	unsigned marking;
	uint32_t long_idx;
	int error;

	/* Stores the non-existing frames of a gap (C.4.2). */
	error = h264sw_dpb_gaps(&env->dpb, &env->sps, header, &env->poc, env->pool, &env->queue);
	CHECK(error == 0);

	/* Derives the order count of the picture (8.2.1). */
	error = h264sw_poc_compute(&env->poc, &env->sps, header, &counts);
	CHECK(error == 0);

	/* Takes the frame and records the order counts in it. */
	frame = h264sw_frame_get(env->pool);
	CHECK(frame != NULL);
	if (frame == NULL)
		return 0;
	h264sw_poc_finish(&env->poc, &env->sps, header, &counts);
	frame->frame_num = header->frame_num;
	frame->poc = counts.poc;
	frame->top_poc = counts.top;
	frame->bottom_poc = counts.bottom;
	frame->idr = header->idr;

	/* Marks the references and removes frames, then stores the picture. */
	error = h264sw_dpb_remove(&env->dpb, &env->sps, header, frame, 0, &marking, &long_idx, &env->queue);
	CHECK(error == 0);
	error = h264sw_dpb_store(&env->dpb, frame, marking, long_idx, output, &env->queue);
	CHECK(error == 0);
	env->last_marking = marking;
	env->last_long_idx = long_idx;
	h264sw_frame_unref(frame);

	/* Hands the order count to the check. */
	return counts.poc;
}

/*
 * Takes every frame out of the queue and lists their order counts (at most max).
 * Returns how many frames there were.
 */
static unsigned
drain(
	struct env *env,
	int32_t *pocs,
	unsigned max)
{
	struct h264sw_frame *frame;
	unsigned count;

	/* Takes the frames oldest first and drops the references. */
	count = 0U;
	for (frame = h264sw_output_take(&env->queue);
	     frame != NULL;
	     frame = h264sw_output_take(&env->queue)) {
		if (count < max)
			pocs[count] = frame->poc;
		count++;
		h264sw_frame_unref(frame);
	}

	/* Hands the count to the check. */
	return count;
}

/*
 * Checks that the drained list equals the expected one.
 */
static void
check_list(
	const int32_t *got,
	unsigned got_count,
	const int32_t *want,
	unsigned want_count)
{
	unsigned index;
	int same;

	/* The lists are equal when the lengths and every element are. */
	same = 1;
	if (got_count != want_count)
		same = 0;
	if (got_count > TEST_LIST_MAX)
		same = 0;
	for (index = 0U;
	     same && index < want_count;
	     index++) {
		if (got[index] != want[index])
			same = 0;
	}

	/* Every number in the queue must equal the expected output order. */
	CHECK(same);
}

/*
 * Drains the queue and checks the order counts against a list.
 */
static void
check_queue(
	struct env *env,
	const int32_t *want,
	unsigned want_count)
{
	int32_t got[TEST_LIST_MAX];
	unsigned count;

	/* Takes what the queue holds and compares. */
	count = drain(env, got, TEST_LIST_MAX);
	check_list(got, count, want, want_count);
}

/*
 * Finds the entry of the first non-empty buffer whose frame_num is frame_num.
 */
static const struct h264sw_dpb_entry *
find_entry(
	const struct h264sw_dpb *dpb,
	uint32_t frame_num)
{
	unsigned index;

	/* Looks at every buffer. */
	for (index = 0U;
	     index < H264SW_MAX_DPB_FRAMES;
	     index++) {
		if (dpb->entry[index].frame == NULL)
			continue;
		if (dpb->entry[index].frame_num == frame_num)
			return &dpb->entry[index];
	}

	/* No buffer holds it. */
	return NULL;
}

/*
 * Gives the marking of the frame with frame_num, or TEST_ABSENT when no buffer holds it.
 */
static unsigned
state_of(
	const struct h264sw_dpb *dpb,
	uint32_t frame_num)
{
	const struct h264sw_dpb_entry *entry;

	/* Looks the frame up. */
	entry = find_entry(dpb, frame_num);
	if (entry == NULL)
		return TEST_ABSENT;

	/* Hands the marking to the check. */
	return entry->reference;
}

/*
 * Counts the non-empty buffers.
 */
static unsigned
filled(
	const struct h264sw_dpb *dpb)
{
	unsigned index;
	unsigned count;

	/* Counts the buffers that hold a frame. */
	count = 0U;
	for (index = 0U;
	     index < H264SW_MAX_DPB_FRAMES;
	     index++) {
		if (dpb->entry[index].frame != NULL)
			count++;
	}

	/* Hands the count to the check. */
	return count;
}

/*
 * Checks the output queue by itself: take on an empty queue, the order
 * across the ring's wrap, the full queue (bump and flush answer ENOBUFS and
 * keep their state), and the clear.
 */
static void
test_queue(void)
{
	struct env env;
	struct h264sw_slice_header header;
	struct h264sw_frame *frame;
	int32_t got[TEST_LIST_MAX];
	int32_t want[TEST_LIST_MAX];
	unsigned batch;
	unsigned index;
	unsigned count;
	unsigned total;
	int error;
	int bumped;
	int same;

	/* A DPB of 16 frames and the largest frame pool, so the output queue can be filled to its limit. */
	env_setup(&env, 16U, 16U, -1, 2U, H264SW_FRAME_POOL_MAX);

	/* An empty queue has nothing to take. */
	CHECK(h264sw_output_take(&env.queue) == NULL);
	CHECK(env.queue.count == 0U);

	/* Fills the queue to its size: 16 + 16 + 2 non-reference pictures, each batch flushed from the DPB. */
	total = 0U;
	for (batch = 0U;
	     batch < 3U;
	     batch++) {
		count = 16U;
		if (batch == 2U)
			count = 2U;
		for (index = 0U;
		     index < count;
		     index++) {
			header_make(&header, 1U, 0U, 0);
			decode_at(&env, &header, (int32_t)(total + index), 1, 0);
		}

		/* Flushing bumps every stored picture out into the output queue. */
		error = h264sw_dpb_flush(&env.dpb, &env.queue);
		CHECK(error == 0);
		total += count;
	}

	/* Every batch reached the queue, which is now full. */
	CHECK(total == H264SW_OUTPUT_MAX);
	CHECK(env.queue.count == H264SW_OUTPUT_MAX);

	/* One more frame waits in the DPB; the full queue refuses it and nothing changes. */
	header_make(&header, 1U, 0U, 0);
	decode_at(&env, &header, 100, 1, 0);
	CHECK(filled(&env.dpb) == 1U);
	bumped = h264sw_dpb_bump(&env.dpb, &env.queue);
	CHECK(bumped == -ENOBUFS);
	error = h264sw_dpb_flush(&env.dpb, &env.queue);
	CHECK(error == ENOBUFS);
	CHECK(filled(&env.dpb) == 1U);
	CHECK(env.dpb.entry[0].output_needed == 1);
	CHECK(env.queue.count == H264SW_OUTPUT_MAX);

	/* Takes three frames from the front: the oldest first. */
	for (index = 0U;
	     index < 3U;
	     index++) {
		frame = h264sw_output_take(&env.queue);
		CHECK(frame != NULL);
		if (frame != NULL) {
			CHECK(frame->poc == (int32_t)index);
			h264sw_frame_unref(frame);
		}
	}

	/* The ring has wrapped around by now: the flush appends after the newest frame. */
	error = h264sw_dpb_flush(&env.dpb, &env.queue);
	CHECK(error == 0);
	CHECK(filled(&env.dpb) == 0U);
	count = drain(&env, got, TEST_LIST_MAX);
	for (index = 0U;
	     index < H264SW_OUTPUT_MAX - 3U;
	     index++)
		want[index] = (int32_t)(index + 3U);
	want[H264SW_OUTPUT_MAX - 3U] = 100;
	same = 1;
	if (count != H264SW_OUTPUT_MAX - 2U)
		same = 0;
	for (index = 0U;
	     same && index < count;
	     index++) {
		if (got[index] != want[index])
			same = 0;
	}

	/* The queue holds exactly the numbers that were not taken, in order. */
	CHECK(same);

	/* Clearing a queue drops its references (teardown checks that none is left). */
	header_make(&header, 1U, 0U, 0);
	decode_at(&env, &header, 7, 1, 0);
	error = h264sw_dpb_flush(&env.dpb, &env.queue);
	CHECK(error == 0);
	CHECK(env.queue.count == 1U);
	h264sw_output_clear(&env.queue);
	CHECK(env.queue.count == 0U);
	CHECK(h264sw_output_take(&env.queue) == NULL);

	/* Teardown checks that no frame reference is left. */
	env_teardown(&env);
}

/*
 * Checks the sliding window (8.2.5.3): with 2 reference frames the third
 * drops the one with the smallest FrameNumWrap (8-27), also across the
 * wrap of frame_num; a window of 0 frames holds 1.
 */
static void
test_sliding_window(void)
{
	struct env env;
	struct h264sw_slice_header header;
	struct h264sw_frame *held;
	unsigned marking;
	uint32_t long_idx;
	int error;
	int32_t want[3] = { 0, 0, 0 };

	/* Frame_num 0, 1, 2 with 2 reference frames. */
	env_setup(&env, 4U, 2U, 0, 2U, 12U);
	header_make(&header, 0U, 1U, 1);
	decode_at(&env, &header, 0, 1, 0);
	CHECK(env.last_marking == H264SW_REF_SHORT);
	header_make(&header, 1U, 1U, 0);
	decode_at(&env, &header, 2, 1, 0);
	CHECK(state_of(&env.dpb, 0U) == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 1U) == H264SW_REF_SHORT);
	CHECK(h264sw_dpb_references(&env.dpb) == 2U);

	/* The third picture finds the window full and drops frame_num 0 (output at once with reorder 0, so its buffer is empty). */
	header_make(&header, 2U, 1U, 0);
	decode_at(&env, &header, 4, 1, 0);
	CHECK(env.last_marking == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 0U) == TEST_ABSENT);
	CHECK(state_of(&env.dpb, 1U) == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 2U) == H264SW_REF_SHORT);
	CHECK(h264sw_dpb_references(&env.dpb) == 2U);
	want[0] = 0;
	want[1] = 2;
	want[2] = 4;
	check_queue(&env, want, 3U);
	env_teardown(&env);

	/*
	 * Across the wrap with MaxFrameNum 16.  The IDR picture is frame_num 0,
	 * then 14, 15, 0 and 1.  Against frame_num 15 the values 0 and 14 are
	 * FrameNumWrap 0 and 14: 0 goes.  Against 0 the values 14 and 15 are -2
	 * and -1: 14 goes.  Against 1 the values 15 and 0 are -1 and 0: 15 goes.
	 */
	env_setup(&env, 4U, 2U, 0, 2U, 12U);
	header_make(&header, 0U, 1U, 1);
	decode_at(&env, &header, 0, 1, 0);
	header_make(&header, 14U, 1U, 0);
	decode_at(&env, &header, 2, 1, 0);
	CHECK(state_of(&env.dpb, 0U) == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 14U) == H264SW_REF_SHORT);
	header_make(&header, 15U, 1U, 0);
	decode_at(&env, &header, 4, 1, 0);
	CHECK(state_of(&env.dpb, 0U) == TEST_ABSENT);
	CHECK(state_of(&env.dpb, 14U) == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 15U) == H264SW_REF_SHORT);
	header_make(&header, 0U, 1U, 0);
	decode_at(&env, &header, 6, 1, 0);
	CHECK(state_of(&env.dpb, 14U) == TEST_ABSENT);
	CHECK(state_of(&env.dpb, 15U) == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 0U) == H264SW_REF_SHORT);
	header_make(&header, 1U, 1U, 0);
	decode_at(&env, &header, 8, 1, 0);
	CHECK(state_of(&env.dpb, 15U) == TEST_ABSENT);
	CHECK(state_of(&env.dpb, 0U) == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 1U) == H264SW_REF_SHORT);
	env_teardown(&env);

	/*
	 * The removal itself empties the buffer of a frame that is neither
	 * needed for output nor a reference any more (C.4.4), before the
	 * current picture is stored.
	 */
	env_setup(&env, 4U, 2U, 0, 2U, 12U);
	header_make(&header, 0U, 1U, 1);
	decode_at(&env, &header, 0, 1, 0);
	header_make(&header, 1U, 1U, 0);
	decode_at(&env, &header, 2, 1, 0);
	CHECK(filled(&env.dpb) == 2U);
	header_make(&header, 2U, 1U, 0);
	held = h264sw_frame_get(env.pool);
	CHECK(held != NULL);
	if (held != NULL) {
		held->frame_num = 2U;
		held->poc = 4;
		error = h264sw_dpb_remove(&env.dpb, &env.sps, &header, held, 0, &marking, &long_idx, &env.queue);
		CHECK(error == 0);
		CHECK(marking == H264SW_REF_SHORT);
		CHECK(filled(&env.dpb) == 1U);
		CHECK(state_of(&env.dpb, 0U) == TEST_ABSENT);
		error = h264sw_dpb_store(&env.dpb, held, marking, long_idx, 1, &env.queue);
		CHECK(error == 0);
		CHECK(filled(&env.dpb) == 2U);
		h264sw_frame_unref(held);
	}

	/* Teardown checks that no frame reference is left. */
	env_teardown(&env);

	/* max_num_ref_frames 0 still keeps one frame (Max( max_num_ref_frames, 1 )). */
	env_setup(&env, 4U, 0U, 0, 2U, 12U);
	header_make(&header, 0U, 1U, 1);
	decode_at(&env, &header, 0, 1, 0);
	header_make(&header, 1U, 1U, 0);
	decode_at(&env, &header, 2, 1, 0);
	CHECK(state_of(&env.dpb, 0U) == TEST_ABSENT);
	CHECK(state_of(&env.dpb, 1U) == H264SW_REF_SHORT);
	CHECK(h264sw_dpb_references(&env.dpb) == 1U);
	env_teardown(&env);
}

/*
 * Checks the operations 1 to 6 of 8.2.5.4 on a run of pictures: each
 * picture's operations are applied in order to the references the earlier
 * pictures left, and the result is worked out from 8-39 (picNumX =
 * CurrPicNum - ( difference_of_pic_nums_minus1 + 1 )), 8-28 and 8-29.
 */
static void
test_mmco(void)
{
	struct env env;
	struct h264sw_slice_header header;
	const struct h264sw_dpb_entry *entry;
	uint32_t number;

	/* 8 buffers, 5 reference frames, outputs at once (reorder 0) so the DPB shows the references only. */
	env_setup(&env, 8U, 5U, 0, 2U, 20U);

	/* Pictures 0 to 3 are plain references (the window is not full). */
	header_make(&header, 0U, 1U, 1);
	decode_at(&env, &header, 0, 1, 0);
	for (number = 1U;
	     number <= 3U;
	     number++) {
		header_make(&header, number, 1U, 0);
		decode_at(&env, &header, (int32_t)(2U * number), 1, 0);
	}

	/* Four references are held: pictures 0 to 3. */
	CHECK(h264sw_dpb_references(&env.dpb) == 4U);

	/*
	 * Picture 4, CurrPicNum 4: operation 4 with plus1 3 (MaxLongTermFrameIdx
	 * 2); operation 3 with difference_of_pic_nums_minus1 2 (picNumX 4 - 3 = 1,
	 * frame_num 1) and index 1; operation 3 with difference 0 (picNumX 3)
	 * and index 2.
	 */
	header_make(&header, 4U, 1U, 0);
	header_mmco(&header, H264SW_MMCO_MAX_LONG_INDEX, 0U, 0U, 0U, 3U);
	header_mmco(&header, H264SW_MMCO_SHORT_TO_LONG, 2U, 0U, 1U, 0U);
	header_mmco(&header, H264SW_MMCO_SHORT_TO_LONG, 0U, 0U, 2U, 0U);
	decode_at(&env, &header, 8, 1, 0);
	CHECK(env.dpb.max_long_term_frame_idx == 2);
	CHECK(env.last_marking == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 0U) == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 1U) == H264SW_REF_LONG);
	CHECK(state_of(&env.dpb, 2U) == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 3U) == H264SW_REF_LONG);
	CHECK(state_of(&env.dpb, 4U) == H264SW_REF_SHORT);
	entry = find_entry(&env.dpb, 1U);
	CHECK(entry != NULL && entry->long_term_frame_idx == 1U);
	entry = find_entry(&env.dpb, 3U);
	CHECK(entry != NULL && entry->long_term_frame_idx == 2U);

	/*
	 * Picture 5, CurrPicNum 5: operation 2 with long_term_pic_num 1 (the
	 * long-term frame with index 1, frame_num 1, goes); operation 1 with
	 * difference 99 (picNumX -95 names nothing and is skipped); operation 1
	 * with difference 2 (picNumX 2, frame_num 2, goes).
	 */
	header_make(&header, 5U, 1U, 0);
	header_mmco(&header, H264SW_MMCO_LONG_UNUSED, 0U, 1U, 0U, 0U);
	header_mmco(&header, H264SW_MMCO_SHORT_UNUSED, 99U, 0U, 0U, 0U);
	header_mmco(&header, H264SW_MMCO_SHORT_UNUSED, 2U, 0U, 0U, 0U);
	decode_at(&env, &header, 10, 1, 0);
	CHECK(state_of(&env.dpb, 1U) == TEST_ABSENT);
	CHECK(state_of(&env.dpb, 2U) == TEST_ABSENT);
	CHECK(state_of(&env.dpb, 0U) == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 3U) == H264SW_REF_LONG);
	CHECK(state_of(&env.dpb, 4U) == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 5U) == H264SW_REF_SHORT);

	/* Picture 6: operation 4 with plus1 2 drops the long-term frame with index 2 (frame_num 3); MaxLongTermFrameIdx becomes 1. */
	header_make(&header, 6U, 1U, 0);
	header_mmco(&header, H264SW_MMCO_MAX_LONG_INDEX, 0U, 0U, 0U, 2U);
	decode_at(&env, &header, 12, 1, 0);
	CHECK(env.dpb.max_long_term_frame_idx == 1);
	CHECK(state_of(&env.dpb, 3U) == TEST_ABSENT);
	CHECK(state_of(&env.dpb, 0U) == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 4U) == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 5U) == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 6U) == H264SW_REF_SHORT);

	/*
	 * Picture 7, CurrPicNum 7: operation 3 with difference 6 (picNumX 0,
	 * frame_num 0) and index 0; operation 6 with index 1 (the picture
	 * itself becomes long-term).
	 */
	header_make(&header, 7U, 1U, 0);
	header_mmco(&header, H264SW_MMCO_SHORT_TO_LONG, 6U, 0U, 0U, 0U);
	header_mmco(&header, H264SW_MMCO_CURRENT_TO_LONG, 0U, 0U, 1U, 0U);
	decode_at(&env, &header, 14, 1, 0);
	CHECK(env.last_marking == H264SW_REF_LONG);
	CHECK(env.last_long_idx == 1U);
	CHECK(state_of(&env.dpb, 0U) == H264SW_REF_LONG);
	CHECK(state_of(&env.dpb, 7U) == H264SW_REF_LONG);
	CHECK(state_of(&env.dpb, 4U) == H264SW_REF_SHORT);
	CHECK(h264sw_dpb_references(&env.dpb) == 5U);

	/*
	 * Picture 8, CurrPicNum 8: operation 3 with difference 5 (picNumX 2
	 * names nothing: skipped); operation 3 with difference 3 (picNumX 4)
	 * and index 1, which the long-term frame of picture 7 holds, so that
	 * frame is marked unused first.
	 */
	header_make(&header, 8U, 1U, 0);
	header_mmco(&header, H264SW_MMCO_SHORT_TO_LONG, 5U, 0U, 3U, 0U);
	header_mmco(&header, H264SW_MMCO_SHORT_TO_LONG, 3U, 0U, 1U, 0U);
	decode_at(&env, &header, 16, 1, 0);
	CHECK(state_of(&env.dpb, 7U) == TEST_ABSENT);
	CHECK(state_of(&env.dpb, 4U) == H264SW_REF_LONG);
	entry = find_entry(&env.dpb, 4U);
	CHECK(entry != NULL && entry->long_term_frame_idx == 1U);
	CHECK(state_of(&env.dpb, 0U) == H264SW_REF_LONG);
	CHECK(state_of(&env.dpb, 5U) == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 6U) == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 8U) == H264SW_REF_SHORT);

	/* Picture 9: operation 6 with index 0, which frame_num 0 holds: that frame goes and the picture is long-term with index 0. */
	header_make(&header, 9U, 1U, 0);
	header_mmco(&header, H264SW_MMCO_CURRENT_TO_LONG, 0U, 0U, 0U, 0U);
	decode_at(&env, &header, 18, 1, 0);
	CHECK(env.last_marking == H264SW_REF_LONG);
	CHECK(env.last_long_idx == 0U);
	CHECK(state_of(&env.dpb, 0U) == TEST_ABSENT);
	CHECK(state_of(&env.dpb, 9U) == H264SW_REF_LONG);
	CHECK(state_of(&env.dpb, 4U) == H264SW_REF_LONG);

	/* Picture 10: operation 5 marks everything unused and takes MaxLongTermFrameIdx back; only the picture itself is left. */
	header_make(&header, 10U, 1U, 0);
	header_mmco(&header, H264SW_MMCO_ALL_UNUSED, 0U, 0U, 0U, 0U);
	decode_at(&env, &header, 20, 1, 0);
	CHECK(env.dpb.max_long_term_frame_idx == -1);
	CHECK(env.last_marking == H264SW_REF_SHORT);
	CHECK(filled(&env.dpb) == 1U);
	CHECK(state_of(&env.dpb, 10U) == H264SW_REF_SHORT);
	CHECK(h264sw_dpb_references(&env.dpb) == 1U);
	env_teardown(&env);
}

/*
 * Checks that operation 5 outputs every frame waiting in the DPB, in
 * order, before the picture is stored (C.4.4), and that operation 6
 * followed by an operation that removes its index takes the long-term
 * marking back.
 */
static void
test_mmco5_output(void)
{
	struct env env;
	struct h264sw_slice_header header;
	int32_t want[3] = { 0, 0, 0 };

	/* Three frames wait: order counts 0, 8, 4. */
	env_setup(&env, 4U, 4U, -1, 2U, 12U);
	header_make(&header, 0U, 1U, 1);
	decode_at(&env, &header, 0, 1, 0);
	header_make(&header, 1U, 1U, 0);
	decode_at(&env, &header, 8, 1, 0);
	header_make(&header, 2U, 1U, 0);
	decode_at(&env, &header, 4, 1, 0);
	CHECK(env.queue.count == 0U);

	/* The picture with operation 5 pushes all three out in order of PicOrderCnt, and (its counts reset to 0 by the decoder) it is the only frame left. */
	header_make(&header, 3U, 1U, 0);
	header_mmco(&header, H264SW_MMCO_ALL_UNUSED, 0U, 0U, 0U, 0U);
	decode_at(&env, &header, 0, 1, 0);
	want[0] = 0;
	want[1] = 4;
	want[2] = 8;
	check_queue(&env, want, 3U);
	CHECK(filled(&env.dpb) == 1U);
	CHECK(state_of(&env.dpb, 3U) == H264SW_REF_SHORT);

	/* Operation 6 and then operation 4 with a maximum below the index: the picture ends short-term. */
	header_make(&header, 4U, 1U, 0);
	header_mmco(&header, H264SW_MMCO_MAX_LONG_INDEX, 0U, 0U, 0U, 4U);
	header_mmco(&header, H264SW_MMCO_CURRENT_TO_LONG, 0U, 0U, 3U, 0U);
	header_mmco(&header, H264SW_MMCO_MAX_LONG_INDEX, 0U, 0U, 0U, 2U);
	decode_at(&env, &header, 2, 1, 0);
	CHECK(env.last_marking == H264SW_REF_SHORT);
	CHECK(env.dpb.max_long_term_frame_idx == 1);
	env_teardown(&env);
}

/*
 * Checks the IDR picture (8.2.5.1, C.4.4): every reference becomes unused,
 * no_output_of_prior_pics_flag 1 empties the DPB without output, 0 outputs
 * the frames in order (also when the size changed: "changed"), and
 * long_term_reference_flag makes the IDR picture long-term with
 * MaxLongTermFrameIdx 0.
 */
static void
test_idr(void)
{
	struct env env;
	struct h264sw_slice_header header;
	int32_t want[4] = { 0, 0, 0, 0 };
	int variant;

	/* Four frames wait, with order counts 0, 12, 6, 4. */
	for (variant = 0;
	     variant < 3;
	     variant++) {
		env_setup(&env, 4U, 4U, -1, 2U, 12U);
		header_make(&header, 0U, 1U, 1);
		decode_at(&env, &header, 0, 1, 0);
		header_make(&header, 1U, 1U, 0);
		decode_at(&env, &header, 12, 1, 0);
		header_make(&header, 2U, 1U, 0);
		decode_at(&env, &header, 6, 1, 0);
		header_make(&header, 3U, 1U, 0);
		decode_at(&env, &header, 4, 1, 0);
		CHECK(env.queue.count == 0U);
		CHECK(filled(&env.dpb) == 4U);

		/* The next IDR picture: variant 0 discards the old frames, 1 outputs them, 2 outputs them although the size "changed". */
		header_make(&header, 0U, 1U, 1);
		if (variant == 0)
			header.no_output_of_prior_pics = 1;
		decode_at(&env, &header, 0, 1, variant == 2);
		CHECK(env.last_marking == H264SW_REF_SHORT);
		CHECK(env.dpb.max_long_term_frame_idx == -1);
		CHECK(filled(&env.dpb) == 1U);
		CHECK(h264sw_dpb_references(&env.dpb) == 1U);
		if (variant == 0) {
			check_queue(&env, want, 0U);
		} else {
			want[0] = 0;
			want[1] = 4;
			want[2] = 6;
			want[3] = 12;
			check_queue(&env, want, 4U);
		}

		/* Teardown checks that no frame reference is left. */
		env_teardown(&env);
	}

	/* long_term_reference_flag: the IDR picture is long-term with index 0, and an old long-term frame is gone. */
	env_setup(&env, 4U, 4U, 0, 2U, 12U);
	header_make(&header, 0U, 1U, 1);
	header.long_term_reference = 1;
	decode_at(&env, &header, 0, 1, 0);
	CHECK(env.last_marking == H264SW_REF_LONG);
	CHECK(env.last_long_idx == 0U);
	CHECK(env.dpb.max_long_term_frame_idx == 0);
	CHECK(state_of(&env.dpb, 0U) == H264SW_REF_LONG);
	header_make(&header, 0U, 1U, 1);
	decode_at(&env, &header, 0, 1, 0);
	CHECK(env.last_marking == H264SW_REF_SHORT);
	CHECK(env.dpb.max_long_term_frame_idx == -1);
	CHECK(filled(&env.dpb) == 1U);
	CHECK(h264sw_dpb_references(&env.dpb) == 1U);
	env_teardown(&env);
}

/*
 * Checks the room rule of h264sw_dpb_make_room for a DPB whose buffers all
 * hold references no longer needed for output (a stream that keeps more
 * references than the DPB holds): the short-term reference with the
 * smallest FrameNumWrap goes, else the long-term one with the smallest
 * LongTermFrameIdx; the loop ends.  And the output flag 0 of a store.
 */
static void
test_make_room(void)
{
	struct env env;
	struct h264sw_slice_header header;
	struct h264sw_frame *frame;
	int error;

	/* Two short-term frames (frame_num 0 and 1) fill a DPB of 2; storing frame_num 2 frees the one with the smallest FrameNumWrap. */
	env_setup(&env, 2U, 2U, 0, 2U, 10U);
	header_make(&header, 0U, 1U, 1);
	decode_at(&env, &header, 0, 1, 0);
	header_make(&header, 1U, 1U, 0);
	decode_at(&env, &header, 2, 1, 0);
	CHECK(filled(&env.dpb) == 2U);
	(void)drain(&env, NULL, 0U);
	error = h264sw_dpb_make_room(&env.dpb, 2U, &env.queue);
	CHECK(error == 0);
	CHECK(state_of(&env.dpb, 0U) == TEST_ABSENT);
	CHECK(state_of(&env.dpb, 1U) == H264SW_REF_SHORT);
	CHECK(env.queue.count == 0U);

	/* With a buffer free nothing is removed. */
	error = h264sw_dpb_make_room(&env.dpb, 2U, &env.queue);
	CHECK(error == 0);
	CHECK(state_of(&env.dpb, 1U) == H264SW_REF_SHORT);
	env_teardown(&env);

	/* Two long-term frames with indices 0 and 3 (the IDR picture's, and operation 6's): the index 0 goes. */
	env_setup(&env, 2U, 2U, 0, 2U, 10U);
	header_make(&header, 0U, 1U, 1);
	header.long_term_reference = 1;
	decode_at(&env, &header, 0, 1, 0);
	header_make(&header, 1U, 1U, 0);
	header_mmco(&header, H264SW_MMCO_MAX_LONG_INDEX, 0U, 0U, 0U, 4U);
	header_mmco(&header, H264SW_MMCO_CURRENT_TO_LONG, 0U, 0U, 3U, 0U);
	decode_at(&env, &header, 2, 1, 0);
	CHECK(filled(&env.dpb) == 2U);
	CHECK(state_of(&env.dpb, 0U) == H264SW_REF_LONG);
	CHECK(state_of(&env.dpb, 1U) == H264SW_REF_LONG);
	(void)drain(&env, NULL, 0U);
	error = h264sw_dpb_make_room(&env.dpb, 5U, &env.queue);
	CHECK(error == 0);
	CHECK(state_of(&env.dpb, 0U) == TEST_ABSENT);
	CHECK(state_of(&env.dpb, 1U) == H264SW_REF_LONG);
	env_teardown(&env);

	/* One long-term and one short-term frame: the short-term one goes first. */
	env_setup(&env, 2U, 2U, 0, 2U, 10U);
	header_make(&header, 0U, 1U, 1);
	header.long_term_reference = 1;
	decode_at(&env, &header, 0, 1, 0);
	header_make(&header, 1U, 1U, 0);
	decode_at(&env, &header, 2, 1, 0);
	(void)drain(&env, NULL, 0U);
	error = h264sw_dpb_make_room(&env.dpb, 2U, &env.queue);
	CHECK(error == 0);
	CHECK(state_of(&env.dpb, 0U) == H264SW_REF_LONG);
	CHECK(state_of(&env.dpb, 1U) == TEST_ABSENT);
	env_teardown(&env);

	/* A third reference picture into a full DPB (max_num_ref_frames 3 is more than the 2 buffers): storing frees the oldest and succeeds. */
	env_setup(&env, 2U, 3U, 0, 2U, 10U);
	header_make(&header, 0U, 1U, 1);
	decode_at(&env, &header, 0, 1, 0);
	header_make(&header, 1U, 1U, 0);
	decode_at(&env, &header, 2, 1, 0);
	header_make(&header, 2U, 1U, 0);
	decode_at(&env, &header, 4, 1, 0);
	CHECK(state_of(&env.dpb, 0U) == TEST_ABSENT);
	CHECK(state_of(&env.dpb, 1U) == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 2U) == H264SW_REF_SHORT);
	env_teardown(&env);

	/* A reference picture that must not be output is stored not needed for output; a non-reference one is not stored at all. */
	env_setup(&env, 4U, 4U, -1, 2U, 10U);
	header_make(&header, 0U, 1U, 1);
	decode_at(&env, &header, 0, 0, 0);
	CHECK(filled(&env.dpb) == 1U);
	CHECK(env.dpb.entry[0].output_needed == 0);
	header_make(&header, 1U, 0U, 0);
	decode_at(&env, &header, 2, 0, 0);
	CHECK(filled(&env.dpb) == 1U);
	frame = h264sw_frame_get(env.pool);
	CHECK(frame != NULL);
	if (frame != NULL) {
		/* The frame came from the pool's free list: the non-reference picture left no reference behind. */
		CHECK(frame->refs == 1U);
		h264sw_frame_unref(frame);
	}

	/* Flushing empties the DPB, and the non-reference picture leaves nothing to output. */
	error = h264sw_dpb_flush(&env.dpb, &env.queue);
	CHECK(error == 0);
	CHECK(env.queue.count == 0U);
	env_teardown(&env);
}

/*
 * Checks the output order of the bumping process (C.4.5, C.4.5.3) with
 * the order counts of a pyramid of B pictures in decoding order 0, 8, 4,
 * 2, 6, traced by hand in the comments.
 */
static void
test_output_order(void)
{
	struct env env;
	struct h264sw_slice_header header;
	struct h264sw_frame *frame;
	int32_t want[4] = { 0, 0, 0, 0 };

	/*
	 * Three buffers, three reference frames.  Pictures: 0 (IDR), 8 and 4
	 * are references, 2 and 6 are not.
	 *   0, 8, 4: buffers free, stored.  DPB { 0, 8, 4 }, nothing output.
	 *   2: no free buffer.  Not lower than 0: bump outputs 0 (it stays, a
	 *      reference).  Still no buffer; 2 is lower than {8, 4}: output at once.
	 *      Output so far: 0, 2.
	 *   6: no free buffer.  Not lower than 4: bump outputs 4 (stays).  6 is
	 *      lower than {8}: output at once.  Output: 4, 6.
	 *   flush: 8.
	 */
	env_setup(&env, 3U, 3U, -1, 2U, 12U);
	header_make(&header, 0U, 1U, 1);
	decode_at(&env, &header, 0, 1, 0);
	header_make(&header, 1U, 1U, 0);
	decode_at(&env, &header, 8, 1, 0);
	header_make(&header, 2U, 1U, 0);
	decode_at(&env, &header, 4, 1, 0);
	check_queue(&env, want, 0U);
	CHECK(filled(&env.dpb) == 3U);
	header_make(&header, 3U, 0U, 0);
	decode_at(&env, &header, 2, 1, 0);
	want[0] = 0;
	want[1] = 2;
	check_queue(&env, want, 2U);
	CHECK(filled(&env.dpb) == 3U);
	header_make(&header, 3U, 0U, 0);
	decode_at(&env, &header, 6, 1, 0);
	want[0] = 4;
	want[1] = 6;
	check_queue(&env, want, 2U);
	CHECK(filled(&env.dpb) == 3U);
	CHECK(h264sw_dpb_flush(&env.dpb, &env.queue) == 0);
	want[0] = 8;
	check_queue(&env, want, 1U);
	CHECK(filled(&env.dpb) == 0U);
	env_teardown(&env);

	/*
	 * Three buffers, two reference frames (0 and 8); 4, 2 and 6 are not.
	 *   0, 8, 4: stored (a free buffer exists for 4).  DPB { 0, 8, 4 }.
	 *   2: bump outputs 0 (stays); then 2 is lower than {8, 4}: output at once.
	 *   6: not lower than 4: bump outputs 4, a non-reference frame, so its
	 *      buffer is free; 6 is stored.  DPB { 0, 8, 6 }.
	 *   flush: 6, 8.
	 * Output: 0, 2, 4, 6, 8.
	 */
	env_setup(&env, 3U, 2U, -1, 2U, 12U);
	header_make(&header, 0U, 1U, 1);
	decode_at(&env, &header, 0, 1, 0);
	header_make(&header, 1U, 1U, 0);
	decode_at(&env, &header, 8, 1, 0);
	header_make(&header, 2U, 0U, 0);
	decode_at(&env, &header, 4, 1, 0);
	check_queue(&env, want, 0U);
	CHECK(filled(&env.dpb) == 3U);
	header_make(&header, 2U, 0U, 0);
	decode_at(&env, &header, 2, 1, 0);
	want[0] = 0;
	want[1] = 2;
	check_queue(&env, want, 2U);
	header_make(&header, 2U, 0U, 0);
	decode_at(&env, &header, 6, 1, 0);
	want[0] = 4;
	check_queue(&env, want, 1U);
	CHECK(filled(&env.dpb) == 3U);
	CHECK(h264sw_dpb_flush(&env.dpb, &env.queue) == 0);
	want[0] = 6;
	want[1] = 8;
	check_queue(&env, want, 2U);
	env_teardown(&env);

	/*
	 * A reorder depth of 1 (max_num_reorder_frames) in a DPB of 4: a frame
	 * is output as soon as more than 1 wait.  Order counts 0, 4, 2, 8.
	 *   0: 1 waits.  4: 2 wait, output 0.  2: {4, 2} wait, output 2.
	 *   8: {4, 8} wait, output 4.  flush: 8.
	 */
	env_setup(&env, 4U, 4U, 1, 2U, 12U);
	CHECK(env.dpb.reorder == 1U);
	header_make(&header, 0U, 1U, 1);
	decode_at(&env, &header, 0, 1, 0);
	check_queue(&env, want, 0U);
	header_make(&header, 1U, 1U, 0);
	decode_at(&env, &header, 4, 1, 0);
	want[0] = 0;
	check_queue(&env, want, 1U);
	header_make(&header, 2U, 1U, 0);
	decode_at(&env, &header, 2, 1, 0);
	want[0] = 2;
	check_queue(&env, want, 1U);
	header_make(&header, 3U, 1U, 0);
	decode_at(&env, &header, 8, 1, 0);
	want[0] = 4;
	check_queue(&env, want, 1U);
	CHECK(h264sw_dpb_flush(&env.dpb, &env.queue) == 0);
	want[0] = 8;
	check_queue(&env, want, 1U);
	env_teardown(&env);

	/*
	 * Equal order counts (a broken stream): a non-reference picture with the
	 * count of a waiting frame is not lower than all of them, so the
	 * waiting frame is bumped first.  Two buffers, references with counts 4
	 * and 6, then a non-reference picture with count 4: frame ids 1 and 3
	 * come out in this order.
	 */
	env_setup(&env, 2U, 2U, -1, 2U, 12U);
	header_make(&header, 0U, 1U, 1);
	decode_at(&env, &header, 4, 1, 0);
	header_make(&header, 1U, 1U, 0);
	decode_at(&env, &header, 6, 1, 0);
	header_make(&header, 2U, 0U, 0);
	decode_at(&env, &header, 4, 1, 0);
	CHECK(env.queue.count == 2U);
	frame = h264sw_output_take(&env.queue);
	CHECK(frame != NULL && frame->id == 1U);
	h264sw_frame_unref(frame);
	frame = h264sw_output_take(&env.queue);
	CHECK(frame != NULL && frame->id == 3U);
	h264sw_frame_unref(frame);
	env_teardown(&env);

	/* One bump outputs a non-reference frame and empties its buffer at once (C.4.5.3 step 4). */
	env_setup(&env, 4U, 4U, -1, 2U, 12U);
	header_make(&header, 1U, 0U, 0);
	decode_at(&env, &header, 3, 1, 0);
	CHECK(filled(&env.dpb) == 1U);
	CHECK(h264sw_dpb_bump(&env.dpb, &env.queue) == 1);
	CHECK(filled(&env.dpb) == 0U);
	CHECK(env.queue.count == 1U);
	CHECK(h264sw_dpb_bump(&env.dpb, &env.queue) == 0);
	env_teardown(&env);

	/* A reorder depth of 0 outputs every picture as it arrives. */
	env_setup(&env, 4U, 4U, 0, 2U, 12U);
	header_make(&header, 0U, 1U, 1);
	decode_at(&env, &header, 0, 1, 0);
	want[0] = 0;
	check_queue(&env, want, 1U);
	header_make(&header, 1U, 0U, 0);
	decode_at(&env, &header, 6, 1, 0);
	want[0] = 6;
	check_queue(&env, want, 1U);
	env_teardown(&env);

	/* The VUI's reorder depth is limited to the buffers; the init also bounds the sizes. */
	env_setup(&env, 4U, 4U, 99, 2U, 12U);
	CHECK(env.dpb.reorder == 4U);
	CHECK(env.dpb.size == 4U);
	env_teardown(&env);
	env_setup(&env, 0U, 4U, -1, 2U, 12U);
	CHECK(env.dpb.size == 1U);
	CHECK(env.dpb.reorder == 1U);
	env_teardown(&env);
	env_setup(&env, 99U, 4U, -1, 2U, 12U);
	CHECK(env.dpb.size == H264SW_MAX_DPB_FRAMES);
	env_teardown(&env);
}

/*
 * Checks the picture buffer parts of a non-existing frame: grey samples,
 * motion all intra, not needed for output, short-term.
 */
static void
check_non_existing(
	const struct h264sw_dpb_entry *entry,
	uint32_t frame_num,
	int32_t poc)
{
	const struct h264sw_frame *frame;

	/* The entry exists. */
	CHECK(entry != NULL);
	if (entry == NULL)
		return;

	/* The marking and the output flag. */
	CHECK(entry->reference == H264SW_REF_SHORT);
	CHECK(entry->output_needed == 0);
	CHECK(entry->frame_num == frame_num);

	/* The frame: non-existing, with its counts, grey, and intra everywhere. */
	frame = entry->frame;
	CHECK(frame != NULL);
	if (frame == NULL)
		return;
	CHECK(frame->non_existing == 1);
	CHECK(frame->frame_num == frame_num);
	CHECK(frame->poc == poc);
	CHECK(frame->top_poc == poc);
	CHECK(frame->bottom_poc == poc);
	CHECK(frame->plane[0][0] == TEST_GREY);
	CHECK(frame->plane[1][0] == TEST_GREY);
	CHECK(frame->plane[2][0] == TEST_GREY);
	CHECK(frame->motion[0].intra == 1U);
	CHECK(frame->motion[0].ref_idx[0][0] == -1);
	CHECK(frame->motion[0].ref_idx[1][3] == -1);
}

/*
 * Checks the gaps in frame_num (8.2.5.2): the frames for 0, 1, 4 with
 * 2 reference frames are frame_num 2 and 3; a longer gap makes the last
 * max_num_ref_frames only; the order counts of type 2 across the wrap of
 * frame_num; type 0 keeps its state; the cases that are no gap.
 */
static void
test_gaps(void)
{
	struct env env;
	struct h264sw_slice_header header;
	struct h264sw_frame *held;
	int32_t want[4] = { 0, 0, 0, 0 };
	int32_t value;
	unsigned allocated;
	unsigned number;
	int error;

	/*
	 * Type 2, 4 buffers, 2 reference frames: frame_num 0 (IDR), 1, then 4.
	 * Order counts of type 2 (8-12) are 2 * ( FrameNumOffset + frame_num ):
	 * 0, 2, and for the gap frames 2 and 3: 4 and 6; the picture with
	 * frame_num 4 gets 8.
	 */
	env_setup(&env, 4U, 2U, -1, 2U, 12U);
	header_make(&header, 0U, 1U, 1);
	value = decode_full(&env, &header, 1);
	CHECK(value == 0);
	header_make(&header, 1U, 1U, 0);
	value = decode_full(&env, &header, 1);
	CHECK(value == 2);
	CHECK(env.poc.prev_ref_frame_num == 1U);
	header_make(&header, 4U, 1U, 0);
	error = h264sw_dpb_gaps(&env.dpb, &env.sps, &header, &env.poc, env.pool, &env.queue);
	CHECK(error == 0);
	CHECK(env.poc.prev_ref_frame_num == 3U);
	check_non_existing(find_entry(&env.dpb, 2U), 2U, 4);
	check_non_existing(find_entry(&env.dpb, 3U), 3U, 6);
	CHECK(state_of(&env.dpb, 0U) == H264SW_REF_UNUSED);
	CHECK(state_of(&env.dpb, 1U) == H264SW_REF_UNUSED);
	CHECK(filled(&env.dpb) == 4U);
	CHECK(env.queue.count == 0U);

	/* Calling again is no gap any more; the picture then continues from the gap's last frame. */
	error = h264sw_dpb_gaps(&env.dpb, &env.sps, &header, &env.poc, env.pool, &env.queue);
	CHECK(error == 0);
	CHECK(filled(&env.dpb) == 4U);
	value = decode_full(&env, &header, 1);
	CHECK(value == 8);
	CHECK(state_of(&env.dpb, 2U) == TEST_ABSENT);
	CHECK(state_of(&env.dpb, 3U) == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 4U) == H264SW_REF_SHORT);
	CHECK(env.queue.count == 0U);

	/* The non-existing frames are never output: the flush gives the three real ones. */
	CHECK(h264sw_dpb_flush(&env.dpb, &env.queue) == 0);
	want[0] = 0;
	want[1] = 2;
	want[2] = 8;
	check_queue(&env, want, 3U);
	env_teardown(&env);

	/* With gaps_in_frame_num_value_allowed_flag 0 the same frames are made (concealment). */
	env_setup(&env, 4U, 2U, -1, 2U, 12U);
	env.sps.gaps_allowed = 0;
	header_make(&header, 0U, 1U, 1);
	(void)decode_full(&env, &header, 1);
	header_make(&header, 1U, 1U, 0);
	(void)decode_full(&env, &header, 1);
	header_make(&header, 4U, 1U, 0);
	error = h264sw_dpb_gaps(&env.dpb, &env.sps, &header, &env.poc, env.pool, &env.queue);
	CHECK(error == 0);
	check_non_existing(find_entry(&env.dpb, 2U), 2U, 4);
	check_non_existing(find_entry(&env.dpb, 3U), 3U, 6);
	env_teardown(&env);

	/*
	 * A gap of 4 (frame_num 2, 3, 4, 5 between 1 and 6) with a window of 2:
	 * only 4 and 5 are made, with order counts 8 and 10.
	 */
	env_setup(&env, 4U, 2U, -1, 2U, 12U);
	header_make(&header, 0U, 1U, 1);
	(void)decode_full(&env, &header, 1);
	header_make(&header, 1U, 1U, 0);
	(void)decode_full(&env, &header, 1);
	header_make(&header, 6U, 1U, 0);
	error = h264sw_dpb_gaps(&env.dpb, &env.sps, &header, &env.poc, env.pool, &env.queue);
	CHECK(error == 0);
	CHECK(state_of(&env.dpb, 2U) == TEST_ABSENT);
	CHECK(state_of(&env.dpb, 3U) == TEST_ABSENT);
	check_non_existing(find_entry(&env.dpb, 4U), 4U, 8);
	check_non_existing(find_entry(&env.dpb, 5U), 5U, 10);
	CHECK(env.poc.prev_ref_frame_num == 5U);
	CHECK(env.pool->allocated <= 4U);
	env_teardown(&env);

	/*
	 * The gap across the wrap of frame_num (MaxFrameNum 16): references
	 * 0 to 13, then frame_num 2.  The values 14, 15, 0, 1 are the gap; the
	 * last two (0 and 1) are made.  FrameNumOffset becomes 16 at frame_num
	 * 0 (8-11), so their counts are 2 * ( 16 + 0 ) = 32 and 34, and the
	 * picture with frame_num 2 gets 36.  Frame_num 13 had 26.
	 */
	env_setup(&env, 4U, 2U, 0, 2U, 12U);
	header_make(&header, 0U, 1U, 1);
	(void)decode_full(&env, &header, 1);
	for (number = 1U;
	     number <= 13U;
	     number++) {
		header_make(&header, number, 1U, 0);
		value = decode_full(&env, &header, 1);
		CHECK(value == (int32_t)(2U * number));
		(void)drain(&env, NULL, 0U);
	}

	/* A picture whose frame_num is below the last one wraps, and the gap process runs for it. */
	header_make(&header, 2U, 1U, 0);
	error = h264sw_dpb_gaps(&env.dpb, &env.sps, &header, &env.poc, env.pool, &env.queue);
	CHECK(error == 0);
	check_non_existing(find_entry(&env.dpb, 0U), 0U, 32);
	check_non_existing(find_entry(&env.dpb, 1U), 1U, 34);
	CHECK(env.poc.prev_ref_frame_num == 1U);
	CHECK(env.poc.prev_frame_num_offset == 16);
	value = decode_full(&env, &header, 1);
	CHECK(value == 36);
	CHECK(state_of(&env.dpb, 0U) == TEST_ABSENT);
	CHECK(state_of(&env.dpb, 1U) == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 2U) == H264SW_REF_SHORT);
	CHECK(env.queue.count == 1U);
	env_teardown(&env);

	/*
	 * Type 0 defines no order count for these frames: they take the count of
	 * the frame stored last (the frame_num 1 picture, 4), and the state
	 * keeps PicOrderCntMsb 0 and pic_order_cnt_lsb 4 of the last real
	 * reference picture (8.2.1.1), moving only PrevRefFrameNum.
	 */
	env_setup(&env, 4U, 2U, -1, 0U, 12U);
	header_make(&header, 0U, 1U, 1);
	header.poc_lsb = 0U;
	value = decode_full(&env, &header, 1);
	CHECK(value == 0);
	header_make(&header, 1U, 1U, 0);
	header.poc_lsb = 4U;
	value = decode_full(&env, &header, 1);
	CHECK(value == 4);
	CHECK(env.poc.prev_msb == 0);
	CHECK(env.poc.prev_lsb == 4);
	header_make(&header, 4U, 1U, 0);
	header.poc_lsb = 8U;
	error = h264sw_dpb_gaps(&env.dpb, &env.sps, &header, &env.poc, env.pool, &env.queue);
	CHECK(error == 0);
	check_non_existing(find_entry(&env.dpb, 2U), 2U, 4);
	check_non_existing(find_entry(&env.dpb, 3U), 3U, 4);
	CHECK(env.poc.prev_msb == 0);
	CHECK(env.poc.prev_lsb == 4);
	CHECK(env.poc.prev_ref_frame_num == 3U);
	value = decode_full(&env, &header, 1);
	CHECK(value == 8);
	env_teardown(&env);

	/*
	 * Operations naming non-existing frames: operation 3 may not name one
	 * (8.2.5.4.3), so it is skipped and the frame stays short-term;
	 * operation 1 may (8.2.5.2 NOTE 4) and removes it.  Frames 0 and 1,
	 * then 4 after the gap 2 and 3 (4 reference frames fit).
	 */
	env_setup(&env, 6U, 4U, -1, 2U, 14U);
	header_make(&header, 0U, 1U, 1);
	(void)decode_full(&env, &header, 1);
	header_make(&header, 1U, 1U, 0);
	(void)decode_full(&env, &header, 1);
	header_make(&header, 4U, 1U, 0);
	header_mmco(&header, H264SW_MMCO_MAX_LONG_INDEX, 0U, 0U, 0U, 1U);
	header_mmco(&header, H264SW_MMCO_SHORT_TO_LONG, 1U, 0U, 0U, 0U);
	(void)decode_full(&env, &header, 1);
	CHECK(state_of(&env.dpb, 2U) == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 3U) == H264SW_REF_SHORT);
	header_make(&header, 5U, 1U, 0);
	header_mmco(&header, H264SW_MMCO_SHORT_UNUSED, 1U, 0U, 0U, 0U);
	(void)decode_full(&env, &header, 1);
	CHECK(state_of(&env.dpb, 3U) == TEST_ABSENT);
	CHECK(state_of(&env.dpb, 2U) == H264SW_REF_SHORT);
	env_teardown(&env);

	/* No gap: the same frame_num as the last reference, the next one, an IDR picture, and a frame_num beyond MaxFrameNum. */
	env_setup(&env, 4U, 2U, -1, 2U, 12U);
	env.poc.prev_ref_frame_num = 3U;
	header_make(&header, 3U, 0U, 0);
	CHECK(h264sw_dpb_gaps(&env.dpb, &env.sps, &header, &env.poc, env.pool, &env.queue) == 0);
	header_make(&header, 4U, 1U, 0);
	CHECK(h264sw_dpb_gaps(&env.dpb, &env.sps, &header, &env.poc, env.pool, &env.queue) == 0);
	header_make(&header, 9U, 1U, 1);
	CHECK(h264sw_dpb_gaps(&env.dpb, &env.sps, &header, &env.poc, env.pool, &env.queue) == 0);
	header_make(&header, 0xFFFFFFFFU, 1U, 0);
	CHECK(h264sw_dpb_gaps(&env.dpb, &env.sps, &header, &env.poc, env.pool, &env.queue) == 0);
	env.poc.prev_ref_frame_num = 0xFFFFFFFFU;
	header_make(&header, 5U, 1U, 0);
	CHECK(h264sw_dpb_gaps(&env.dpb, &env.sps, &header, &env.poc, env.pool, &env.queue) == 0);
	CHECK(filled(&env.dpb) == 0U);
	CHECK(env.pool->allocated == 0U);
	env_teardown(&env);

	/* The longest gap MaxFrameNum 65536 allows (65534 values) makes only the window's frames, at once. */
	env_setup(&env, 4U, 2U, -1, 2U, 12U);
	env.sps.log2_max_frame_num = 16U;
	env.sps.max_frame_num = 65536U;
	h264sw_dpb_init(&env.dpb, &env.sps);
	env.poc.prev_ref_frame_num = 0U;
	header_make(&header, 65535U, 1U, 0);
	error = h264sw_dpb_gaps(&env.dpb, &env.sps, &header, &env.poc, env.pool, &env.queue);
	CHECK(error == 0);
	CHECK(filled(&env.dpb) == 2U);
	CHECK(state_of(&env.dpb, 65533U) == H264SW_REF_SHORT);
	CHECK(state_of(&env.dpb, 65534U) == H264SW_REF_SHORT);
	env_teardown(&env);

	/*
	 * A full DPB: 2 buffers, 2 reference frames, frame_num 0, 1 waiting for
	 * output (reorder 2), then frame_num 3.  The frame for 2 first drops
	 * frame_num 0 from the window; its buffer is only free after the bumping
	 * outputs it (order count 0).
	 */
	env_setup(&env, 2U, 2U, 2, 2U, 12U);
	header_make(&header, 0U, 1U, 1);
	(void)decode_full(&env, &header, 1);
	header_make(&header, 1U, 1U, 0);
	(void)decode_full(&env, &header, 1);
	CHECK(env.queue.count == 0U);
	header_make(&header, 3U, 1U, 0);
	error = h264sw_dpb_gaps(&env.dpb, &env.sps, &header, &env.poc, env.pool, &env.queue);
	CHECK(error == 0);
	want[0] = 0;
	check_queue(&env, want, 1U);
	check_non_existing(find_entry(&env.dpb, 2U), 2U, 4);
	CHECK(state_of(&env.dpb, 0U) == TEST_ABSENT);
	env_teardown(&env);

	/* The pool gives no frame: ENOMEM (both frames of a pool of 2 wait in the DPB). */
	env_setup(&env, 4U, 2U, -1, 2U, 2U);
	header_make(&header, 0U, 1U, 1);
	(void)decode_full(&env, &header, 1);
	header_make(&header, 1U, 1U, 0);
	(void)decode_full(&env, &header, 1);
	header_make(&header, 4U, 1U, 0);
	error = h264sw_dpb_gaps(&env.dpb, &env.sps, &header, &env.poc, env.pool, &env.queue);
	CHECK(error == ENOMEM);
	CHECK(env.poc.prev_ref_frame_num == 1U);
	env_teardown(&env);

	/* The pool is used up by a frame the caller holds: ENOMEM again, with nothing leaked. */
	env_setup(&env, 4U, 2U, 0, 2U, 3U);
	held = h264sw_frame_get(env.pool);
	CHECK(held != NULL);
	header_make(&header, 0U, 1U, 1);
	(void)decode_full(&env, &header, 1);
	header_make(&header, 1U, 1U, 0);
	(void)decode_full(&env, &header, 1);
	allocated = env.pool->allocated;
	(void)drain(&env, NULL, 0U);
	header_make(&header, 9U, 1U, 0);
	error = h264sw_dpb_gaps(&env.dpb, &env.sps, &header, &env.poc, env.pool, &env.queue);
	CHECK(error == ENOMEM);
	CHECK(env.pool->allocated == allocated);
	h264sw_frame_unref(held);
	env_teardown(&env);
}

/*
 * Fills the queue of an environment to its size with frames of another
 * pool (flushed from a scratch DPB), so the next output answers ENOBUFS.
 */
static void
fill_queue(
	struct env *env)
{
	struct env scratch;
	struct h264sw_slice_header header;
	unsigned total;
	unsigned count;
	unsigned index;
	struct h264sw_frame *moved;

	/* Moves frames in batches of at most 16 through a scratch DPB. */
	env_setup(&scratch, 16U, 16U, -1, 2U, H264SW_FRAME_POOL_MAX);
	total = 0U;
	while (total < H264SW_OUTPUT_MAX) {
		count = H264SW_OUTPUT_MAX - total;
		if (count > 16U)
			count = 16U;
		for (index = 0U;
		     index < count;
		     index++) {
			header_make(&header, 1U, 0U, 0);
			decode_at(&scratch, &header, (int32_t)(1000U + total + index), 1, 0);
		}

		/* The batch is flushed into the scratch queue. */
		CHECK(h264sw_dpb_flush(&scratch.dpb, &scratch.queue) == 0);
		while (scratch.queue.count > 0U) {
			moved = h264sw_output_take(&scratch.queue);
			env->queue.frame[(env->queue.head + env->queue.count) % H264SW_OUTPUT_MAX] = moved;
			env->queue.count++;
		}

		/* The batch is moved to the queue under test, so the count of moved frames grows. */
		total += count;
	}

	/* The queue under test is full now. */
	CHECK(env->queue.count == H264SW_OUTPUT_MAX);

	/* The scratch pool lives until its last frame is released (the pool's rule for a dead pool). */
	h264sw_frame_pool_destroy(scratch.pool);
}

/*
 * Checks the answers of a full queue: gaps, store and remove report
 * ENOBUFS and leave the DPB consistent, and the call can be repeated once
 * the queue is empty.
 */
static void
test_queue_full(void)
{
	struct env env;
	struct h264sw_slice_header header;
	struct h264sw_frame *frame;
	unsigned marking;
	uint32_t long_idx;
	int error;

	/* A DPB of 2 holds frame_num 0 and 1; the gap frame for 2 needs the oldest output first. */
	env_setup(&env, 2U, 2U, 2, 2U, 12U);
	header_make(&header, 0U, 1U, 1);
	(void)decode_full(&env, &header, 1);
	header_make(&header, 1U, 1U, 0);
	(void)decode_full(&env, &header, 1);
	fill_queue(&env);
	header_make(&header, 3U, 1U, 0);
	error = h264sw_dpb_gaps(&env.dpb, &env.sps, &header, &env.poc, env.pool, &env.queue);
	CHECK(error == ENOBUFS);
	CHECK(env.poc.prev_ref_frame_num == 1U);
	CHECK(state_of(&env.dpb, 2U) == TEST_ABSENT);

	/* After the caller empties the queue the same call makes the frame. */
	(void)drain(&env, NULL, 0U);
	error = h264sw_dpb_gaps(&env.dpb, &env.sps, &header, &env.poc, env.pool, &env.queue);
	CHECK(error == 0);
	CHECK(state_of(&env.dpb, 2U) == H264SW_REF_SHORT);
	CHECK(env.poc.prev_ref_frame_num == 2U);
	env_teardown(&env);

	/* A store that needs a bump answers ENOBUFS and keeps the picture out of the DPB. */
	env_setup(&env, 1U, 1U, 1, 2U, 12U);
	header_make(&header, 0U, 1U, 1);
	decode_at(&env, &header, 0, 1, 0);
	fill_queue(&env);
	frame = h264sw_frame_get(env.pool);
	CHECK(frame != NULL);
	if (frame != NULL) {
		frame->frame_num = 1U;
		frame->poc = 2;
		header_make(&header, 1U, 1U, 0);
		error = h264sw_dpb_remove(&env.dpb, &env.sps, &header, frame, 0, &marking, &long_idx, &env.queue);
		CHECK(error == 0);
		error = h264sw_dpb_store(&env.dpb, frame, marking, long_idx, 1, &env.queue);
		CHECK(error == ENOBUFS);
		CHECK(filled(&env.dpb) == 1U);
		CHECK(frame->refs == 1U);
		h264sw_frame_unref(frame);
	}

	/* An IDR picture that must output the old frames answers ENOBUFS when the queue is full. */
	frame = h264sw_frame_get(env.pool);
	CHECK(frame != NULL);
	if (frame != NULL) {
		header_make(&header, 0U, 1U, 1);
		error = h264sw_dpb_remove(&env.dpb, &env.sps, &header, frame, 0, &marking, &long_idx, &env.queue);
		CHECK(error == ENOBUFS);
		h264sw_frame_unref(frame);
	}

	/* Teardown checks that no frame reference is left. */
	env_teardown(&env);
}

/*
 * Checks the calls that cannot work (missing arguments, a marking that is
 * none of the three) and the calls on an empty DPB.
 */
static void
test_errors(void)
{
	struct env env;
	struct h264sw_slice_header header;
	struct h264sw_frame *frame;
	unsigned marking;
	uint32_t long_idx;

	/* A small DPB and a frame pool of 8 frames. */
	env_setup(&env, 4U, 2U, -1, 2U, 8U);
	header_make(&header, 1U, 1U, 0);
	frame = h264sw_frame_get(env.pool);
	CHECK(frame != NULL);

	/* Missing arguments are EINVAL, and nothing is dereferenced. */
	CHECK(h264sw_dpb_flush(NULL, &env.queue) == EINVAL);
	CHECK(h264sw_dpb_flush(&env.dpb, NULL) == EINVAL);
	CHECK(h264sw_dpb_gaps(NULL, &env.sps, &header, &env.poc, env.pool, &env.queue) == EINVAL);
	CHECK(h264sw_dpb_gaps(&env.dpb, NULL, &header, &env.poc, env.pool, &env.queue) == EINVAL);
	CHECK(h264sw_dpb_gaps(&env.dpb, &env.sps, NULL, &env.poc, env.pool, &env.queue) == EINVAL);
	CHECK(h264sw_dpb_gaps(&env.dpb, &env.sps, &header, NULL, env.pool, &env.queue) == EINVAL);
	CHECK(h264sw_dpb_gaps(&env.dpb, &env.sps, &header, &env.poc, NULL, &env.queue) == EINVAL);
	CHECK(h264sw_dpb_gaps(&env.dpb, &env.sps, &header, &env.poc, env.pool, NULL) == EINVAL);
	CHECK(h264sw_dpb_remove(NULL, &env.sps, &header, frame, 0, &marking, &long_idx, &env.queue) == EINVAL);
	CHECK(h264sw_dpb_remove(&env.dpb, &env.sps, NULL, frame, 0, &marking, &long_idx, &env.queue) == EINVAL);
	CHECK(h264sw_dpb_remove(&env.dpb, &env.sps, &header, frame, 0, NULL, &long_idx, &env.queue) == EINVAL);
	CHECK(h264sw_dpb_remove(&env.dpb, &env.sps, &header, frame, 0, &marking, NULL, &env.queue) == EINVAL);
	CHECK(h264sw_dpb_remove(&env.dpb, &env.sps, &header, frame, 0, &marking, &long_idx, NULL) == EINVAL);
	CHECK(h264sw_dpb_store(NULL, frame, H264SW_REF_SHORT, 0U, 1, &env.queue) == EINVAL);
	CHECK(h264sw_dpb_store(&env.dpb, NULL, H264SW_REF_SHORT, 0U, 1, &env.queue) == EINVAL);
	CHECK(h264sw_dpb_store(&env.dpb, frame, H264SW_REF_SHORT, 0U, 1, NULL) == EINVAL);
	CHECK(h264sw_dpb_store(&env.dpb, frame, 7U, 0U, 1, &env.queue) == EINVAL);
	CHECK(h264sw_dpb_bump(NULL, &env.queue) < 0);
	CHECK(h264sw_dpb_bump(&env.dpb, NULL) < 0);
	CHECK(h264sw_dpb_make_room(NULL, 0U, &env.queue) == EINVAL);
	CHECK(h264sw_dpb_make_room(&env.dpb, 0U, NULL) == EINVAL);
	CHECK(h264sw_dpb_references(NULL) == 0U);
	CHECK(filled(&env.dpb) == 0U);
	h264sw_dpb_clear(NULL);
	h264sw_dpb_init(NULL, &env.sps);
	h264sw_dpb_init(&env.dpb, NULL);
	h264sw_output_init(NULL);
	h264sw_output_clear(NULL);
	CHECK(h264sw_output_take(NULL) == NULL);
	h264sw_frame_unref(frame);

	/* An empty DPB has nothing to bump, flush, or remove. */
	CHECK(h264sw_dpb_bump(&env.dpb, &env.queue) == 0);
	CHECK(h264sw_dpb_flush(&env.dpb, &env.queue) == 0);
	CHECK(env.queue.count == 0U);
	CHECK(h264sw_dpb_make_room(&env.dpb, 0U, &env.queue) == 0);
	h264sw_dpb_clear(&env.dpb);
	h264sw_dpb_clear(&env.dpb);
	CHECK(env.dpb.max_long_term_frame_idx == -1);

	/* A header whose operations are all nonsense (an operation 99, huge numbers) leaves the DPB alone and still answers 0. */
	header_make(&header, 1U, 1U, 0);
	header_mmco(&header, 99U, 0U, 0U, 0U, 0U);
	header_mmco(&header, H264SW_MMCO_SHORT_UNUSED, 0xFFFFFFFFU, 0U, 0U, 0U);
	header_mmco(&header, H264SW_MMCO_LONG_UNUSED, 0U, 0xFFFFFFFFU, 0U, 0U);
	header_mmco(&header, H264SW_MMCO_SHORT_TO_LONG, 0xFFFFFFFFU, 0U, 0xFFFFFFFFU, 0U);
	header_mmco(&header, H264SW_MMCO_MAX_LONG_INDEX, 0U, 0U, 0U, 0xFFFFFFFFU);
	header_mmco(&header, H264SW_MMCO_CURRENT_TO_LONG, 0U, 0U, 0xFFFFFFFFU, 0U);
	header.mmco_count = 0xFFFFFFFFU;
	decode_at(&env, &header, 3, 1, 0);
	CHECK(env.dpb.max_long_term_frame_idx == 2147483647);
	CHECK(env.last_marking == H264SW_REF_LONG);
	CHECK(env.last_long_idx == 0xFFFFFFFFU);
	env_teardown(&env);
}

/*
 * Runs random pictures through the DPB: random sizes, windows, frame
 * numbers (also beyond MaxFrameNum), order counts with ties, operations
 * with huge values, IDR pictures, gaps, output flags.  The checks are that
 * nothing crashes (the sanitizers watch), the invariants of the entries hold
 * after every picture, and every frame is returned at the end.
 */
static void
test_random(
	uint32_t seed,
	unsigned dpb_frames,
	unsigned refs,
	int reorder,
	unsigned poc_type)
{
	struct env env;
	struct h264sw_slice_header header;
	struct h264sw_frame *frame;
	unsigned iteration;
	unsigned index;
	unsigned count;
	unsigned operation;
	unsigned filled_count;
	unsigned marking;
	uint32_t long_idx;
	uint32_t frame_num;
	uint32_t value;
	uint32_t draw;
	uint32_t difference_minus1;
	uint32_t long_term_pic_num;
	uint32_t long_term_frame_idx;
	uint32_t plus1;
	unsigned references;
	int with_operations;
	int idr;
	int ok;
	int error;

	/* The same seed gives the same sequence on every run. */
	rng_state = seed;
	env_setup(&env, dpb_frames, refs, reorder, poc_type, 2U * dpb_frames + 4U);
	if (env.pool == NULL)
		return;

	/* Pictures with random fields. */
	frame_num = 0U;
	for (iteration = 0U;
	     iteration < 4000U;
	     iteration++) {
		/* The kind of picture: mostly the next frame_num, sometimes a jump or a nonsense value. */
		idr = (rng_next() % 20U) == 0U;
		value = rng_next() % 16U;
		if (value < 11U)
			frame_num = (env.poc.prev_ref_frame_num + 1U) % 16U;
		else if (value < 15U)
			frame_num = rng_next() % 16U;
		else
			frame_num = rng_next();
		if (idr)
			frame_num = 0U;
		header_make(&header, frame_num, rng_next() % 3U, idr);
		if (idr)
			header.nal_ref_idc = 1U;
		header.no_output_of_prior_pics = (int)(rng_next() % 3U == 0U);
		header.long_term_reference = (int)(rng_next() % 3U == 0U);

		/* Operations: sometimes many, with small or huge values. */
		with_operations = 0;
		if (!idr && header.nal_ref_idc != 0U) {
			draw = rng_next();
			if (draw % 3U == 0U)
				with_operations = 1;
		}

		/* Many operations, with small or huge values, when the draw asked for them. */
		if (with_operations) {
			count = rng_next() % (H264SW_MAX_MMCO + 1U);
			for (index = 0U;
			     index < count;
			     index++) {
				operation = rng_next() % 8U;

				/* The four operands are drawn from the last to the first, which fixes the random sequence. */
				plus1 = small_or_any(6U);
				long_term_frame_idx = small_or_any(4U);
				long_term_pic_num = small_or_any(4U);
				difference_minus1 = small_or_any(20U);
				header_mmco(&header, operation, difference_minus1, long_term_pic_num, long_term_frame_idx, plus1);
			}
		}

		/* The gaps, then the picture, as the decoder orders them. */
		error = h264sw_dpb_gaps(&env.dpb, &env.sps, &header, &env.poc, env.pool, &env.queue);
		CHECK(error == 0 || error == ENOMEM || error == ENOBUFS);
		frame = h264sw_frame_get(env.pool);
		if (frame == NULL) {
			(void)drain(&env, NULL, 0U);
			continue;
		}

		/* The frame stands for the picture, with its numbers from the random draw. */
		frame->frame_num = frame_num;
		frame->poc = (int32_t)(rng_next() % 41U) - 20;
		frame->top_poc = frame->poc;
		frame->bottom_poc = frame->poc;
		frame->idr = idr;
		draw = rng_next();
		error = h264sw_dpb_remove(&env.dpb, &env.sps, &header, frame, (int)(draw % 2U), &marking, &long_idx, &env.queue);
		CHECK(error == 0 || error == ENOBUFS);
		if (error == 0) {
			draw = rng_next();
			error = h264sw_dpb_store(&env.dpb, frame, marking, long_idx, (int)(draw % 5U != 0U), &env.queue);
			CHECK(error == 0 || error == ENOBUFS);
		}

		/* The DPB holds its own reference, so this one is given up. */
		h264sw_frame_unref(frame);

		/* Follows the reference frame_num like the order count state does. */
		if (header.nal_ref_idc != 0U && frame_num < 16U && !header.has_mmco5)
			env.poc.prev_ref_frame_num = frame_num;
		if (idr || header.has_mmco5)
			env.poc.prev_ref_frame_num = 0U;
		(void)drain(&env, NULL, 0U);

		/* The invariants of the entries. */
		ok = 1;
		filled_count = 0U;
		for (index = 0U;
		     index < H264SW_MAX_DPB_FRAMES;
		     index++) {
			if (env.dpb.entry[index].frame == NULL) {
				if (env.dpb.entry[index].reference != H264SW_REF_UNUSED)
					ok = 0;
				if (env.dpb.entry[index].output_needed != 0)
					ok = 0;
				continue;
			}

			/* Entries that hold a frame are counted and checked. */
			filled_count++;
			if (index >= env.dpb.size)
				ok = 0;
			if (env.dpb.entry[index].frame->refs == 0U)
				ok = 0;
			if (env.dpb.entry[index].reference > H264SW_REF_LONG)
				ok = 0;
		}

		/* The counts of filled entries, references and allocated frames must stay within their limits. */
		if (filled_count > env.dpb.size)
			ok = 0;
		references = h264sw_dpb_references(&env.dpb);
		if (references > filled_count)
			ok = 0;
		if (env.pool->allocated > env.pool->limit)
			ok = 0;
		CHECK(ok);
	}

	/* The end of the stream empties everything. */
	error = h264sw_dpb_flush(&env.dpb, &env.queue);
	CHECK(error == 0);
	(void)drain(&env, NULL, 0U);
	CHECK(filled(&env.dpb) == 0U);
	env_teardown(&env);
}
