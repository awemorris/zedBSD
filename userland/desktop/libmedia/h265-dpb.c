/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoded picture buffer of the H.265 Vulkan Video path (8.3.2,
 * C.5.2).
 */

#include "h265-dpb.h"

#include <errno.h>
#include <string.h>

/*
 * The roles an entry has in the reference picture set of the current
 * picture, kept per entry while h265_dpb_begin works: in none of the five
 * lists, in a Curr list (the picture reads it), or in a Foll list (it is
 * only kept).
 */
#define DPB_ROLE_NONE 0
#define DPB_ROLE_CURR 1
#define DPB_ROLE_FOLL 2

/* The range of log2_max_pic_order_cnt_lsb_minus4 + 4 (7.4.3.2.1). */
#define DPB_MIN_POC_LSB_BITS 4U
#define DPB_MAX_POC_LSB_BITS 16U

/*
 * The result of the lookup of 8.3.2 for one picture: for each entry of
 * the three Curr lists the index of the buffer entry found (-1 for "no
 * reference picture"), and for each buffer entry its role.  It lives on
 * the stack of h265_dpb_begin.
 */
struct dpb_sets {
	int st_before[H265SW_MAX_RPS];
	int st_after[H265SW_MAX_RPS];
	int lt_curr[H265SW_MAX_RPS];
	int role[H265_DPB_PICTURES];
};

/* Static function declarations. */
static void dpb_empty_entry(struct h265_dpb_entry *entry);
static unsigned dpb_count_used(const struct h265_dpb *dpb);
static unsigned dpb_count_needed(const struct h265_dpb *dpb);
static int dpb_latency_reached(const struct h265_dpb *dpb);
static int dpb_bump_wanted(const struct h265_dpb *dpb, int check_size);
static int dpb_bump_one(struct h265_dpb *dpb, struct h265_dpb_output *output, unsigned *count);
static void dpb_bump_while_wanted(struct h265_dpb *dpb, int check_size, struct h265_dpb_output *output, unsigned *count);
static void dpb_remove_unneeded(struct h265_dpb *dpb);
static int64_t dpb_poc_lsb(int32_t poc, unsigned log2_max_poc_lsb);
static int dpb_find_long(const struct h265_dpb *dpb, const struct dpb_sets *sets, int32_t poc, int msb_present, unsigned log2_max_poc_lsb);
static int dpb_find_short(const struct h265_dpb *dpb, const struct dpb_sets *sets, int32_t poc);
static void dpb_apply_rps(struct h265_dpb *dpb, const struct h265sw_rps_pocs *pocs, unsigned log2_max_poc_lsb, struct dpb_sets *sets);
static void dpb_flush_for_irap(struct h265_dpb *dpb, int no_output_of_prior_pics, struct h265_dpb_plan *plan);
static int dpb_evict(struct h265_dpb *dpb, const struct dpb_sets *sets, struct h265_dpb_plan *plan);
static void dpb_fill_plan(const struct h265_dpb *dpb, const struct h265sw_rps_pocs *pocs, const struct dpb_sets *sets, struct h265_dpb_plan *plan);

/*
 * Makes an empty buffer with slots Vulkan DPB slots.
 *
 * A slot count outside H265_DPB_PICTURES..H265_DPB_MAX_SLOTS is clamped
 * into that range.
 */
void
h265_dpb_init(
	struct h265_dpb *dpb,
	unsigned slots)
{
	unsigned index;

	/* Nothing to initialise without a buffer. */
	if (dpb == NULL)
		return;

	/* Starts from all zeros, which is the state of an empty buffer. */
	memset(dpb, 0, sizeof(*dpb));

	/* Clamps the slot count into the range a session may use. */
	dpb->slots = slots;
	if (dpb->slots < H265_DPB_PICTURES)
		dpb->slots = H265_DPB_PICTURES;
	if (dpb->slots > H265_DPB_MAX_SLOTS)
		dpb->slots = H265_DPB_MAX_SLOTS;

	/* Until the first picture names an SPS, one picture is all the buffer holds. */
	dpb->size = 1U;
	dpb->current = H265_DPB_NO_SLOT;

	/* Marks every entry empty. */
	for (index = 0U;
	     index < H265_DPB_PICTURES;
	     index++)
		dpb_empty_entry(&dpb->entry[index]);
}

/*
 * Applies the reference picture set of the current picture and the removal
 * and bumping before it (8.3.2, C.5.2.2), and plans its decode.
 *
 * The steps are those documented with the declaration in h265-dpb.h.  The
 * lookup of the reference pictures (8.3.2) runs on the entries first, so
 * the pictures that stay references are known before the removal and the
 * bumping decide which entries can be emptied.
 */
int
h265_dpb_begin(
	struct h265_dpb *dpb,
	const struct h265sw_sps *sps,
	const struct h265sw_rps_pocs *pocs,
	int irap_no_rasl,
	int no_output_of_prior_pics,
	struct h265_dpb_plan *plan)
{
	struct dpb_sets sets;
	unsigned highest_tid;
	unsigned dec_pic_buffering_minus1;
	unsigned index;
	int first_picture;
	int error;

	/* Refuses arguments no valid caller passes. */
	if (dpb == NULL || sps == NULL)
		return EINVAL;
	if (pocs == NULL || plan == NULL)
		return EINVAL;

	/* A second begin before end or abort is a caller's mistake; nothing changes. */
	if (dpb->in_picture != 0)
		return EINVAL;

	/* HighestTid is the last sub-layer, which the caller decodes (7.4.3.2.1). */
	highest_tid = sps->max_sub_layers_minus1;
	if (highest_tid >= H265SW_MAX_SUB_LAYERS)
		return EINVAL;

	/* A buffer larger than A.4.2 allows cannot be kept in the entries. */
	dec_pic_buffering_minus1 = sps->max_dec_pic_buffering_minus1[highest_tid];
	if (dec_pic_buffering_minus1 >= H265_DPB_PICTURES)
		return EINVAL;

	/* The order count mask of 8-5 needs MaxPicOrderCntLsb as a power of two of 16 to 65536. */
	if (sps->log2_max_poc_lsb < DPB_MIN_POC_LSB_BITS)
		return EINVAL;
	if (sps->log2_max_poc_lsb > DPB_MAX_POC_LSB_BITS)
		return EINVAL;

	/* A set longer than its array would be read outside it. */
	if (pocs->num_st_before > H265SW_MAX_RPS || pocs->num_st_after > H265SW_MAX_RPS)
		return EINVAL;
	if (pocs->num_st_foll > H265SW_MAX_RPS || pocs->num_lt_curr > H265SW_MAX_RPS)
		return EINVAL;
	if (pocs->num_lt_foll > H265SW_MAX_RPS)
		return EINVAL;

	/* Takes the buffer size and the output limits of the SPS (A.4.2, 7-9). */
	dpb->size = dec_pic_buffering_minus1 + 1U;
	dpb->reorder = sps->max_num_reorder_pics[highest_tid];
	dpb->latency_limited = 0;
	dpb->max_latency = 0U;
	if (sps->max_latency_increase_plus1[highest_tid] != 0U) {
		dpb->latency_limited = 1;
		dpb->max_latency = (uint64_t)dpb->reorder + (uint64_t)sps->max_latency_increase_plus1[highest_tid] - 1U;
	}

	/* Starts a plan with nothing to output and no slot; the first picture of a session resets the video session. */
	first_picture = 0;
	if (dpb->started == 0)
		first_picture = 1;
	memset(plan, 0, sizeof(*plan));
	plan->reset = first_picture;
	plan->setup_slot = H265_DPB_NO_SLOT;

	/* An IRAP picture with NoRaslOutputFlag 1 leaves no reference picture behind (8.3.2). */
	if (irap_no_rasl != 0) {
		/* Marks every held picture unused for reference. */
		for (index = 0U;
		     index < H265_DPB_PICTURES;
		     index++)
			dpb->entry[index].marking = H265_DPB_UNUSED;
	}

	/* Finds the pictures of the five lists and marks the buffer (8.3.2 steps 1 to 4). */
	dpb_apply_rps(dpb, pocs, sps->log2_max_poc_lsb, &sets);

	/* Removes and bumps before the picture (C.5.2.2). */
	if (irap_no_rasl != 0 && dpb->started != 0) {
		/* An IRAP picture with NoRaslOutputFlag 1 that is not picture 0 empties the buffer. */
		dpb_flush_for_irap(dpb, no_output_of_prior_pics, plan);
	} else {
		/* Any other picture drops what is finished and bumps while the limits are exceeded. */
		dpb_remove_unneeded(dpb);
		dpb_bump_while_wanted(dpb, 1, plan->output, &plan->output_count);
	}

	/* Makes room in a buffer a broken stream filled with references. */
	error = dpb_evict(dpb, &sets, plan);

	/* Plans the decode from the marking that is left. */
	dpb_fill_plan(dpb, pocs, &sets, plan);

	/* Refuses a picture whose references fill the whole buffer; the caller skips it. */
	if (error != 0)
		return error;

	/* The picture is under way; later pictures of the session are no longer the first. */
	dpb->started = 1;
	dpb->in_picture = 1;
	dpb->current = H265_DPB_NO_SLOT;

	/* Succeeded: the plan is complete and the buffer is ready for the picture. */
	return 0;
}

/*
 * Chooses the slot the current picture is decoded into: the lowest slot
 * held by no used entry and not busy.
 *
 * The same slot is given again when the call is repeated after a success.
 */
int
h265_dpb_take_slot(
	struct h265_dpb *dpb,
	struct h265_dpb_plan *plan)
{
	uint32_t taken;
	unsigned limit;
	unsigned index;
	unsigned slot;

	/* Refuses arguments no valid caller passes. */
	if (dpb == NULL || plan == NULL)
		return EINVAL;

	/* There is no picture to give a slot to. */
	if (dpb->in_picture == 0)
		return EINVAL;

	/* A repeated call answers with the slot already taken. */
	if (dpb->current != H265_DPB_NO_SLOT) {
		plan->setup_slot = dpb->current;

		/* Succeeded: the slot taken before. */
		return 0;
	}

	/* Collects the slots the entries hold and the slots a reader still holds. */
	taken = dpb->busy;
	for (index = 0U;
	     index < H265_DPB_PICTURES;
	     index++) {
		if (dpb->entry[index].used == 0)
			continue;
		if (dpb->entry[index].slot < 0)
			continue;
		if (dpb->entry[index].slot >= (int)H265_DPB_MAX_SLOTS)
			continue;
		taken |= (uint32_t)1U << dpb->entry[index].slot;
	}

	/* Looks for the lowest slot nobody holds. */
	limit = dpb->slots;
	if (limit > H265_DPB_MAX_SLOTS)
		limit = H265_DPB_MAX_SLOTS;
	for (slot = 0U;
	     slot < limit;
	     slot++) {
		if ((taken & ((uint32_t)1U << slot)) == 0U)
			break;
	}

	/* Every slot is held: the caller releases output pictures and tries again. */
	if (slot >= limit)
		return EBUSY;

	/* Remembers the slot for the repeated call and for end. */
	dpb->current = (int)slot;
	plan->setup_slot = (int)slot;

	/* Succeeded: the picture is decoded into this slot. */
	return 0;
}

/*
 * Stores the decoded current picture and bumps after it (C.5.2.3).
 *
 * The caller's mistakes are refused before anything is changed, so a
 * refusal leaves the buffer as it was.
 */
int
h265_dpb_end(
	struct h265_dpb *dpb,
	const struct h265_dpb_plan *plan,
	int32_t poc,
	int pic_output,
	uint64_t id,
	struct h265_dpb_output *output,
	unsigned *count)
{
	struct h265_dpb_entry *entry;
	unsigned index;
	int free_index;

	/* Without a place to report the outputs there is nothing to do. */
	if (count == NULL)
		return EINVAL;
	*count = 0U;

	/* Refuses arguments no valid caller passes. */
	if (dpb == NULL || plan == NULL || output == NULL)
		return EINVAL;

	/* There is no picture under way. */
	if (dpb->in_picture == 0)
		return EINVAL;

	/* The slot of the plan must be the one take_slot gave. */
	if (plan->setup_slot == H265_DPB_NO_SLOT)
		return EINVAL;
	if (plan->setup_slot != dpb->current)
		return EINVAL;

	/* Looks for the entry that will hold the picture; begin made room, so none is a caller's mistake. */
	free_index = -1;
	for (index = 0U;
	     index < H265_DPB_PICTURES;
	     index++) {
		if (dpb->entry[index].used == 0) {
			free_index = (int)index;
			break;
		}
	}
	if (free_index < 0)
		return EINVAL;

	/* A picture to be output delays the pictures that follow it in output order (C.5.2.3). */
	if (pic_output != 0) {
		for (index = 0U;
		     index < H265_DPB_PICTURES;
		     index++) {
			entry = &dpb->entry[index];
			if (entry->used == 0)
				continue;
			if (entry->needed == 0)
				continue;
			if (entry->poc > poc)
				entry->latency += 1U;
		}
	}

	/* Stores the current picture, a short-term reference waiting for output when pic_output is 1 (8.1.3 step 4). */
	entry = &dpb->entry[free_index];
	entry->used = 1;
	entry->marking = H265_DPB_SHORT;
	entry->needed = 0;
	if (pic_output != 0)
		entry->needed = 1;
	entry->latency = 0U;
	entry->poc = poc;
	entry->slot = plan->setup_slot;
	entry->id = id;

	/* The picture is stored; the caller may start the next one. */
	dpb->in_picture = 0;
	dpb->current = H265_DPB_NO_SLOT;

	/* Bumps while the buffer holds more waiting pictures than the SPS allows (C.5.2.3). */
	dpb_bump_while_wanted(dpb, 0, output, count);

	/* Succeeded: the picture is stored and the outputs are listed. */
	return 0;
}

/*
 * Ends a picture that was not decoded (the caller skipped it or the decode
 * failed).
 *
 * The marking and the outputs of h265_dpb_begin stand; only the slot of
 * the picture is given up.
 */
void
h265_dpb_abort(
	struct h265_dpb *dpb)
{
	/* Nothing to end without a buffer. */
	if (dpb == NULL)
		return;

	/* No picture is under way, so there is nothing to give up. */
	if (dpb->in_picture == 0)
		return;

	/* The current picture is not stored; its slot is free again. */
	dpb->in_picture = 0;
	dpb->current = H265_DPB_NO_SLOT;
}

/*
 * Outputs every picture needed for output in output order and empties the
 * buffer.
 *
 * Every output slot becomes busy; the slots of pictures that were not
 * output are free at once.
 */
int
h265_dpb_flush(
	struct h265_dpb *dpb,
	struct h265_dpb_output *output,
	unsigned *count)
{
	unsigned index;
	int bumped;

	/* Without a place to report the outputs there is nothing to do. */
	if (count == NULL)
		return EINVAL;
	*count = 0U;

	/* Refuses arguments no valid caller passes. */
	if (dpb == NULL || output == NULL)
		return EINVAL;

	/* A picture under way still needs the buffer. */
	if (dpb->in_picture != 0)
		return EINVAL;

	/* Outputs every picture waiting for output, smallest order count first. */
	bumped = 1;
	while (bumped != 0)
		bumped = dpb_bump_one(dpb, output, count);

	/* Empties what is left: the reference pictures are not kept across a flush. */
	for (index = 0U;
	     index < H265_DPB_PICTURES;
	     index++)
		dpb_empty_entry(&dpb->entry[index]);

	/* The next picture starts a new run of the session. */
	dpb->started = 0;

	/* Succeeded: the buffer is empty and the outputs are listed. */
	return 0;
}

/*
 * Empties the buffer without output (a seek).
 *
 * The busy slots stay busy: the caller may still be reading them.
 */
void
h265_dpb_clear(
	struct h265_dpb *dpb)
{
	unsigned index;

	/* Nothing to empty without a buffer. */
	if (dpb == NULL)
		return;

	/* Drops every picture without output. */
	for (index = 0U;
	     index < H265_DPB_PICTURES;
	     index++)
		dpb_empty_entry(&dpb->entry[index]);

	/* The next picture starts a new run of the session and no picture is under way. */
	dpb->started = 0;
	dpb->in_picture = 0;
	dpb->current = H265_DPB_NO_SLOT;
}

/*
 * Marks slot as read: clears its busy bit.
 *
 * A slot an entry still holds stays unavailable through that entry.
 */
void
h265_dpb_release(
	struct h265_dpb *dpb,
	int slot)
{
	/* Nothing to release without a buffer. */
	if (dpb == NULL)
		return;

	/* A slot outside the session's slots is ignored. */
	if (slot < 0)
		return;
	if ((unsigned)slot >= dpb->slots)
		return;
	if ((unsigned)slot >= H265_DPB_MAX_SLOTS)
		return;

	/* The caller has read the picture, so the slot may be written again. */
	dpb->busy &= ~((uint32_t)1U << slot);
}

/* Makes an entry empty: all zeros except the slot, which is "no slot". */
static void
dpb_empty_entry(
	struct h265_dpb_entry *entry)
{
	/* An empty entry holds no picture. */
	memset(entry, 0, sizeof(*entry));
	entry->slot = H265_DPB_NO_SLOT;
}

/* Counts the entries that hold a picture. */
static unsigned
dpb_count_used(
	const struct h265_dpb *dpb)
{
	unsigned count;
	unsigned index;

	/* Counts the used entries. */
	count = 0U;
	for (index = 0U;
	     index < H265_DPB_PICTURES;
	     index++) {
		if (dpb->entry[index].used != 0)
			count++;
	}

	/* Succeeded: the number of pictures in the buffer. */
	return count;
}

/* Counts the pictures marked needed for output. */
static unsigned
dpb_count_needed(
	const struct h265_dpb *dpb)
{
	unsigned count;
	unsigned index;

	/* Counts the used entries that wait for output. */
	count = 0U;
	for (index = 0U;
	     index < H265_DPB_PICTURES;
	     index++) {
		if (dpb->entry[index].used == 0)
			continue;
		if (dpb->entry[index].needed != 0)
			count++;
	}

	/* Succeeded: the number of pictures waiting for output. */
	return count;
}

/* Tells whether the latency limit applies and a picture waiting for output has reached it (C.5.2.2). */
static int
dpb_latency_reached(
	const struct h265_dpb *dpb)
{
	unsigned index;

	/* Without a limit no picture has reached it. */
	if (dpb->latency_limited == 0)
		return 0;

	/* Looks for a waiting picture whose PicLatencyCount is at the limit or beyond. */
	for (index = 0U;
	     index < H265_DPB_PICTURES;
	     index++) {
		if (dpb->entry[index].used == 0)
			continue;
		if (dpb->entry[index].needed == 0)
			continue;
		if (dpb->entry[index].latency >= dpb->max_latency)
			return 1;
	}

	/* No waiting picture has reached the limit. */
	return 0;
}

/*
 * Tells whether one of the bumping conditions of C.5.2.2 or C.5.2.3 holds:
 * more pictures waiting than reorder, a picture at the latency limit, and,
 * when check_size is 1, a buffer holding at least size pictures.
 */
static int
dpb_bump_wanted(
	const struct h265_dpb *dpb,
	int check_size)
{
	unsigned needed;
	unsigned used;
	int reached;

	/* More pictures wait for output than the SPS lets the output order lag. */
	needed = dpb_count_needed(dpb);
	if (needed > dpb->reorder)
		return 1;

	/* A picture waited as long as the SPS allows. */
	reached = dpb_latency_reached(dpb);
	if (reached != 0)
		return 1;

	/* The buffer is full: C.5.2.2 bumps before a picture is added, C.5.2.3 does not look. */
	if (check_size != 0) {
		used = dpb_count_used(dpb);
		if (used >= dpb->size)
			return 1;
	}

	/* No condition holds. */
	return 0;
}

/*
 * Runs the bumping process once (C.5.2.4): outputs the waiting picture with
 * the smallest PicOrderCntVal, makes its slot busy and, when it is no
 * reference picture, empties its entry.  Returns 0 when nothing waits.
 */
static int
dpb_bump_one(
	struct h265_dpb *dpb,
	struct h265_dpb_output *output,
	unsigned *count)
{
	struct h265_dpb_entry *entry;
	unsigned index;
	int best;

	/* Looks for the waiting picture that comes first in output order; equal orders (broken streams) go in decode order. */
	best = -1;
	for (index = 0U;
	     index < H265_DPB_PICTURES;
	     index++) {
		entry = &dpb->entry[index];
		if (entry->used == 0)
			continue;
		if (entry->needed == 0)
			continue;
		if (best < 0) {
			best = (int)index;
			continue;
		}
		if (entry->poc < dpb->entry[best].poc) {
			best = (int)index;
			continue;
		}
		if (entry->poc == dpb->entry[best].poc && entry->id < dpb->entry[best].id)
			best = (int)index;
	}

	/* Nothing waits for output. */
	if (best < 0)
		return 0;

	/* The output list has no room for another picture (not reachable with the sizes of the header). */
	if (*count >= H265_DPB_MAX_OUTPUT)
		return 0;

	/* Reports the picture. */
	entry = &dpb->entry[best];
	output[*count].slot = entry->slot;
	output[*count].poc = entry->poc;
	output[*count].id = entry->id;
	*count += 1U;

	/* The caller reads the image of this slot until it releases it, so nothing may write it meanwhile. */
	if (entry->slot >= 0 && entry->slot < (int)H265_DPB_MAX_SLOTS)
		dpb->busy |= (uint32_t)1U << entry->slot;

	/* The picture no longer waits for output. */
	entry->needed = 0;

	/* A picture that is no reference picture has nothing more to do in the buffer (C.5.2.4). */
	if (entry->marking == H265_DPB_UNUSED)
		dpb_empty_entry(entry);

	/* Succeeded: one picture was output. */
	return 1;
}

/* Bumps while one of the conditions of dpb_bump_wanted holds and a picture can still be output. */
static void
dpb_bump_while_wanted(
	struct h265_dpb *dpb,
	int check_size,
	struct h265_dpb_output *output,
	unsigned *count)
{
	int wanted;
	int bumped;

	/* Each bump outputs a different picture, so the loop ends when the pictures run out. */
	bumped = 1;
	while (bumped != 0) {
		wanted = dpb_bump_wanted(dpb, check_size);
		if (wanted == 0)
			break;
		bumped = dpb_bump_one(dpb, output, count);
	}
}

/* Empties the entries that are not needed for output and unused for reference. */
static void
dpb_remove_unneeded(
	struct h265_dpb *dpb)
{
	unsigned index;

	/* Looks at every held picture. */
	for (index = 0U;
	     index < H265_DPB_PICTURES;
	     index++) {
		if (dpb->entry[index].used == 0)
			continue;
		if (dpb->entry[index].needed != 0)
			continue;
		if (dpb->entry[index].marking != H265_DPB_UNUSED)
			continue;
		dpb_empty_entry(&dpb->entry[index]);
	}
}

/* Computes poc & ( MaxPicOrderCntLsb - 1 ) for an order count that can be negative, as the floor remainder (8-5, h265sw.h). */
static int64_t
dpb_poc_lsb(
	int32_t poc,
	unsigned log2_max_poc_lsb)
{
	int64_t modulus;
	int64_t remainder;

	/* The remainder of a negative count is negative in C, so it is brought into 0..modulus - 1. */
	modulus = (int64_t)1 << log2_max_poc_lsb;
	remainder = (int64_t)poc % modulus;
	if (remainder < 0)
		remainder += modulus;

	/* Succeeded: the least significant bits of the order count. */
	return remainder;
}

/*
 * Finds the reference picture of one long-term entry (8-6): with the msb
 * flag 0 by the least significant bits of PicOrderCntVal, else by the whole
 * count.  A picture already in a set is not taken again (8.3.2 NOTE 5);
 * among several matches the greatest id wins.  Returns the entry's index,
 * or -1 for "no reference picture".
 */
static int
dpb_find_long(
	const struct h265_dpb *dpb,
	const struct dpb_sets *sets,
	int32_t poc,
	int msb_present,
	unsigned log2_max_poc_lsb)
{
	const struct h265_dpb_entry *entry;
	int64_t entry_poc;
	unsigned index;
	int best;

	/* Looks at every picture that is a reference picture and not yet in a set. */
	best = -1;
	for (index = 0U;
	     index < H265_DPB_PICTURES;
	     index++) {
		entry = &dpb->entry[index];
		if (entry->used == 0)
			continue;
		if (entry->marking == H265_DPB_UNUSED)
			continue;
		if (sets->role[index] != DPB_ROLE_NONE)
			continue;

		/* The whole order count when the msb is signalled, else only its low bits. */
		entry_poc = (int64_t)entry->poc;
		if (msb_present == 0)
			entry_poc = dpb_poc_lsb(entry->poc, log2_max_poc_lsb);
		if (entry_poc != (int64_t)poc)
			continue;

		/* Keeps the match that was decoded last. */
		if (best < 0 || entry->id > dpb->entry[best].id)
			best = (int)index;
	}

	/* Succeeded: the entry found, or -1. */
	return best;
}

/*
 * Finds the short-term reference picture of one short-term entry (8-7) by
 * its PicOrderCntVal.  A picture already in a set is not taken again
 * (8.3.2 NOTE 5); among several matches the greatest id wins.  Returns the
 * entry's index, or -1 for "no reference picture".
 */
static int
dpb_find_short(
	const struct h265_dpb *dpb,
	const struct dpb_sets *sets,
	int32_t poc)
{
	const struct h265_dpb_entry *entry;
	unsigned index;
	int best;

	/* Looks at every short-term reference picture not yet in a set. */
	best = -1;
	for (index = 0U;
	     index < H265_DPB_PICTURES;
	     index++) {
		entry = &dpb->entry[index];
		if (entry->used == 0)
			continue;
		if (entry->marking != H265_DPB_SHORT)
			continue;
		if (sets->role[index] != DPB_ROLE_NONE)
			continue;
		if (entry->poc != poc)
			continue;

		/* Keeps the match that was decoded last. */
		if (best < 0 || entry->id > dpb->entry[best].id)
			best = (int)index;
	}

	/* Succeeded: the entry found, or -1. */
	return best;
}

/*
 * Applies steps 1 to 4 of the reference picture set decoding (8.3.2): finds
 * the pictures of the long-term lists and marks them long-term, finds those
 * of the short-term lists, and marks every other reference picture unused.
 * *sets gets the entry of each Curr list entry and the role of each entry.
 */
static void
dpb_apply_rps(
	struct h265_dpb *dpb,
	const struct h265sw_rps_pocs *pocs,
	unsigned log2_max_poc_lsb,
	struct dpb_sets *sets)
{
	unsigned index;
	int found;

	/* Starts with no entry in any list. */
	memset(sets, 0, sizeof(*sets));
	for (index = 0U;
	     index < H265SW_MAX_RPS;
	     index++) {
		sets->st_before[index] = -1;
		sets->st_after[index] = -1;
		sets->lt_curr[index] = -1;
	}

	/* RefPicSetLtCurr (8-6): the pictures the current picture reads as long-term references. */
	for (index = 0U;
	     index < pocs->num_lt_curr;
	     index++) {
		found = dpb_find_long(dpb, sets, pocs->lt_curr[index], (int)pocs->lt_curr_msb[index], log2_max_poc_lsb);
		sets->lt_curr[index] = found;
		if (found >= 0)
			sets->role[found] = DPB_ROLE_CURR;
	}

	/* RefPicSetLtFoll (8-6): the long-term references that are only kept. */
	for (index = 0U;
	     index < pocs->num_lt_foll;
	     index++) {
		found = dpb_find_long(dpb, sets, pocs->lt_foll[index], (int)pocs->lt_foll_msb[index], log2_max_poc_lsb);
		if (found >= 0)
			sets->role[found] = DPB_ROLE_FOLL;
	}

	/* Every picture found so far becomes a long-term reference picture (8.3.2 step 2). */
	for (index = 0U;
	     index < H265_DPB_PICTURES;
	     index++) {
		if (sets->role[index] != DPB_ROLE_NONE)
			dpb->entry[index].marking = H265_DPB_LONG;
	}

	/* RefPicSetStCurrBefore (8-7): short-term references before the current picture in output order. */
	for (index = 0U;
	     index < pocs->num_st_before;
	     index++) {
		found = dpb_find_short(dpb, sets, pocs->st_before[index]);
		sets->st_before[index] = found;
		if (found >= 0)
			sets->role[found] = DPB_ROLE_CURR;
	}

	/* RefPicSetStCurrAfter (8-7): short-term references after the current picture in output order. */
	for (index = 0U;
	     index < pocs->num_st_after;
	     index++) {
		found = dpb_find_short(dpb, sets, pocs->st_after[index]);
		sets->st_after[index] = found;
		if (found >= 0)
			sets->role[found] = DPB_ROLE_CURR;
	}

	/* RefPicSetStFoll (8-7): short-term references that are only kept. */
	for (index = 0U;
	     index < pocs->num_st_foll;
	     index++) {
		found = dpb_find_short(dpb, sets, pocs->st_foll[index]);
		if (found >= 0)
			sets->role[found] = DPB_ROLE_FOLL;
	}

	/* A reference picture in none of the five lists is no reference picture any more (8.3.2 step 4). */
	for (index = 0U;
	     index < H265_DPB_PICTURES;
	     index++) {
		if (dpb->entry[index].used == 0)
			continue;
		if (sets->role[index] == DPB_ROLE_NONE)
			dpb->entry[index].marking = H265_DPB_UNUSED;
	}
}

/*
 * Empties the buffer before an IRAP picture with NoRaslOutputFlag 1 that is
 * not picture 0 (C.5.2.2): without output when no_output_of_prior_pics is 1
 * (counting the pictures that were still waiting in plan->discarded), else
 * after outputting every waiting picture in output order.
 */
static void
dpb_flush_for_irap(
	struct h265_dpb *dpb,
	int no_output_of_prior_pics,
	struct h265_dpb_plan *plan)
{
	unsigned index;
	int bumped;

	/* The pictures of the buffer are dropped unseen. */
	if (no_output_of_prior_pics != 0) {
		/* Counts the pictures that were still to be shown, then empties every entry. */
		for (index = 0U;
		     index < H265_DPB_PICTURES;
		     index++) {
			if (dpb->entry[index].used != 0 && dpb->entry[index].needed != 0)
				plan->discarded += 1U;
			dpb_empty_entry(&dpb->entry[index]);
		}

		/* Nothing is left to output. */
		return;
	}

	/* Drops the pictures that are finished, then outputs the waiting ones in order. */
	dpb_remove_unneeded(dpb);
	bumped = 1;
	while (bumped != 0)
		bumped = dpb_bump_one(dpb, plan->output, &plan->output_count);

	/* Every picture was unused for reference, so each output emptied its entry; this clears any other. */
	for (index = 0U;
	     index < H265_DPB_PICTURES;
	     index++)
		dpb_empty_entry(&dpb->entry[index]);
}

/*
 * Empties the entry with the smallest PicOrderCntVal that is in no Curr
 * list until the buffer holds fewer than size pictures; each one is counted
 * in plan->evicted.  Returns EINVAL when every held picture is in a Curr
 * list, so no room can be made.
 */
static int
dpb_evict(
	struct h265_dpb *dpb,
	const struct dpb_sets *sets,
	struct h265_dpb_plan *plan)
{
	struct h265_dpb_entry *entry;
	unsigned used;
	unsigned index;
	int victim;

	/* Makes room one picture at a time. */
	used = dpb_count_used(dpb);
	while (used >= dpb->size) {
		/* Looks for the held picture that comes first in output order and is no Curr picture. */
		victim = -1;
		for (index = 0U;
		     index < H265_DPB_PICTURES;
		     index++) {
			entry = &dpb->entry[index];
			if (entry->used == 0)
				continue;
			if (sets->role[index] == DPB_ROLE_CURR)
				continue;
			if (victim < 0) {
				victim = (int)index;
				continue;
			}
			if (entry->poc < dpb->entry[victim].poc) {
				victim = (int)index;
				continue;
			}
			if (entry->poc == dpb->entry[victim].poc && entry->id < dpb->entry[victim].id)
				victim = (int)index;
		}

		/* Every held picture is read by the current picture: it cannot be decoded. */
		if (victim < 0)
			return EINVAL;

		/* Drops the victim without output. */
		dpb_empty_entry(&dpb->entry[victim]);
		plan->evicted += 1U;
		used = dpb_count_used(dpb);
	}

	/* Succeeded: the buffer holds fewer than size pictures. */
	return 0;
}

/* Fills the Curr lists as slots, the number of missing references and the list of every reference picture. */
static void
dpb_fill_plan(
	const struct h265_dpb *dpb,
	const struct h265sw_rps_pocs *pocs,
	const struct dpb_sets *sets,
	struct h265_dpb_plan *plan)
{
	const struct h265_dpb_entry *entry;
	unsigned index;
	unsigned slot;

	/* RefPicSetStCurrBefore as slots; a missing picture is counted. */
	plan->num_st_before = pocs->num_st_before;
	for (index = 0U;
	     index < pocs->num_st_before;
	     index++) {
		plan->st_before[index] = H265_DPB_NO_SLOT;
		if (sets->st_before[index] >= 0)
			plan->st_before[index] = dpb->entry[sets->st_before[index]].slot;
		else
			plan->missing += 1U;
	}

	/* RefPicSetStCurrAfter as slots. */
	plan->num_st_after = pocs->num_st_after;
	for (index = 0U;
	     index < pocs->num_st_after;
	     index++) {
		plan->st_after[index] = H265_DPB_NO_SLOT;
		if (sets->st_after[index] >= 0)
			plan->st_after[index] = dpb->entry[sets->st_after[index]].slot;
		else
			plan->missing += 1U;
	}

	/* RefPicSetLtCurr as slots. */
	plan->num_lt_curr = pocs->num_lt_curr;
	for (index = 0U;
	     index < pocs->num_lt_curr;
	     index++) {
		plan->lt_curr[index] = H265_DPB_NO_SLOT;
		if (sets->lt_curr[index] >= 0)
			plan->lt_curr[index] = dpb->entry[sets->lt_curr[index]].slot;
		else
			plan->missing += 1U;
	}

	/* Lists every reference picture by slot, in the order of the slots. */
	plan->reference_count = 0U;
	for (slot = 0U;
	     slot < H265_DPB_MAX_SLOTS;
	     slot++) {
		for (index = 0U;
		     index < H265_DPB_PICTURES;
		     index++) {
			entry = &dpb->entry[index];
			if (entry->used == 0)
				continue;
			if (entry->marking == H265_DPB_UNUSED)
				continue;
			if (entry->slot != (int)slot)
				continue;
			if (plan->reference_count >= H265_DPB_PICTURES)
				continue;
			plan->reference_slot[plan->reference_count] = entry->slot;
			plan->reference_poc[plan->reference_count] = entry->poc;
			plan->reference_long[plan->reference_count] = 0U;
			if (entry->marking == H265_DPB_LONG)
				plan->reference_long[plan->reference_count] = 1U;
			plan->reference_count += 1U;
		}
	}
}
