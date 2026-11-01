/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoded picture buffer of vkvideo-probe (see dpb.h).
 *
 * The marking is H.264 8.2.5 for frames: an IDR picture makes every other
 * picture unused and is short-term or long-term by its flag; another
 * reference picture follows its memory management operations (8.2.5.4) or
 * the sliding window (8.2.5.3), and is then short-term unless operation 6
 * made it long-term.  Operation 5 makes every other picture unused and the
 * picture's frame_num 0 for the pictures after it.  A gap in frame_num,
 * where the sequence allows one, puts a short-term frame without a picture
 * in for each number skipped, each after the sliding window (8.2.5.2).
 */

#include "dpb.h"

#include <stdint.h>
#include <string.h>

static int32_t dpb_frame_num_wrap(const struct dpb_entry *entry, uint32_t frame_num, uint32_t max_frame_num);
static int dpb_find_short(const struct dpb *dpb, int32_t pic_num, uint32_t frame_num, uint32_t max_frame_num, int32_t skip);
static int dpb_find_long(const struct dpb *dpb, uint32_t index, int32_t skip);
static const char *dpb_operation(struct dpb *dpb, const struct h264_mmco *mmco, uint32_t frame_num, uint32_t max_frame_num, int32_t current, int *current_long);
static const char *dpb_sliding_window(struct dpb *dpb, uint32_t frame_num, uint32_t max_frame_num, int32_t current);
static const char *dpb_fill_gap(struct dpb *dpb, const StdVideoH264SequenceParameterSet *sps, uint32_t frame_num, uint32_t max_frame_num);

/*
 * Starts an empty DPB for a sequence of at most `max_references`
 * reference frames: that many slots and one for the picture decoded.
 */
void
dpb_init(
	struct dpb *dpb,
	uint32_t max_references)
{
	/* Every slot unused and not held by the decoder. */
	memset(dpb, 0, sizeof(*dpb));
	if (max_references > DPB_REFERENCES)
		max_references = DPB_REFERENCES;
	dpb->max_references = max_references;
	dpb->slots = max_references + 1U;
	dpb->max_long_term_frame_idx = -1;
}

/*
 * Plans the decode of a picture: the reset of the first decode, the
 * references (every reference picture of the DPB; an IDR picture has
 * none), the slot the picture is written into (one holding no reference),
 * and the slots the decoder holds that hold no reference any more.
 * Returns NULL, or why the probe cannot decode the picture.
 */
const char *
dpb_plan(
	struct dpb *dpb,
	const StdVideoH264SequenceParameterSet *sps,
	const struct h264_picture *picture,
	struct dpb_plan *plan)
{
	struct dpb_entry *entry;
	const char *reason;
	uint32_t max_frame_num;
	uint32_t expected;
	uint32_t slot;

	/* Nothing planned yet; the first decode resets the decoder. */
	memset(plan, 0, sizeof(*plan));
	plan->setup = -1;
	if (!dpb->started) {
		plan->reset = 1;
		dpb->started = 1;
	}
	max_frame_num = 1U << (sps->log2_max_frame_num_minus4 + 4U);

	/* An IDR picture makes every reference unused before it is decoded (it reads none). */
	if (picture->info.flags.IdrPicFlag) {
		for (slot = 0U; slot < dpb->slots; slot++)
			dpb->entry[slot].reference = DPB_UNUSED;
		dpb->max_long_term_frame_idx = -1;
	} else {
		/* Another picture's frame_num is the last reference picture's or the next, or comes after a gap. */
		expected = (dpb->previous_reference_frame_num + 1U) % max_frame_num;
		if (picture->info.frame_num != dpb->previous_reference_frame_num && picture->info.frame_num != expected) {
			reason = dpb_fill_gap(dpb, sps, picture->info.frame_num, max_frame_num);
			if (reason != NULL)
				return reason;
		}
	}

	/* The references: every slot holding a reference picture, with its information (a frame without a picture is none). */
	for (slot = 0U; slot < dpb->slots; slot++) {
		entry = &dpb->entry[slot];
		if (entry->reference == DPB_UNUSED || entry->missing)
			continue;
		if (plan->reference_count >= DPB_REFERENCES)
			return "more than 16 references";
		plan->references[plan->reference_count] = (int32_t)slot;
		memset(&plan->info[plan->reference_count], 0, sizeof(plan->info[0]));
		plan->info[plan->reference_count].FrameNum = (uint16_t)entry->frame_num;
		if (entry->reference == DPB_LONG) {
			plan->info[plan->reference_count].flags.used_for_long_term_reference = 1;
			plan->info[plan->reference_count].FrameNum = (uint16_t)entry->long_term_frame_idx;
		}
		plan->info[plan->reference_count].PicOrderCnt[0] = entry->poc[0];
		plan->info[plan->reference_count].PicOrderCnt[1] = entry->poc[1];
		plan->reference_count++;
	}

	/* The picture's slot: the first one without a reference. */
	for (slot = 0U; slot < dpb->slots; slot++) {
		if (dpb->entry[slot].reference == DPB_UNUSED) {
			plan->setup = (int32_t)slot;
			break;
		}
	}
	if (plan->setup < 0)
		return "no free slot in the DPB";

	/* The picture's own information as the slot will hold it. */
	plan->setup_info.FrameNum = picture->info.frame_num;
	if (picture->info.flags.IdrPicFlag && picture->long_term_reference) {
		plan->setup_info.flags.used_for_long_term_reference = 1;
		plan->setup_info.FrameNum = 0U;
	}
	plan->setup_info.PicOrderCnt[0] = picture->info.PicOrderCnt[0];
	plan->setup_info.PicOrderCnt[1] = picture->info.PicOrderCnt[1];

	/* The slots the decoder holds that hold no reference picture any more (the picture's own slot among them). */
	for (slot = 0U; slot < dpb->slots; slot++) {
		entry = &dpb->entry[slot];
		if (entry->device_active && (entry->reference == DPB_UNUSED || entry->missing)) {
			plan->deactivate[plan->deactivate_count] = (int32_t)slot;
			plan->deactivate_count++;
		}
	}

	/* Succeeded: the decode is planned. */
	return NULL;
}

/*
 * Marks the DPB after a planned decode: the deactivated slots are no longer
 * the decoder's; a reference picture's slot is the decoder's and is marked
 * as 8.2.5 says, a non-reference picture's slot holds nothing.  Returns
 * NULL, or why the probe cannot follow the marking.
 */
const char *
dpb_mark(
	struct dpb *dpb,
	const StdVideoH264SequenceParameterSet *sps,
	const struct h264_picture *picture,
	const struct dpb_plan *plan)
{
	struct dpb_entry *current;
	uint32_t max_frame_num;
	uint32_t index;
	uint32_t slot;
	uint32_t count;
	int current_long;
	const char *reason;

	/* The deactivated slots. */
	for (index = 0U; index < plan->deactivate_count; index++)
		dpb->entry[plan->deactivate[index]].device_active = 0;

	/* A picture that is not a reference leaves its slot empty (the decoder does too). */
	current = &dpb->entry[plan->setup];
	current->missing = 0;
	if (!picture->info.flags.is_reference) {
		current->reference = DPB_UNUSED;
		current->device_active = 0;
		return NULL;
	}
	current->device_active = 1;
	max_frame_num = 1U << (sps->log2_max_frame_num_minus4 + 4U);

	/* An IDR picture: short-term, or long-term with index 0. */
	current_long = 0;
	if (picture->info.flags.IdrPicFlag) {
		if (picture->long_term_reference) {
			current_long = 1;
			dpb->max_long_term_frame_idx = 0;
		}
	} else if (picture->adaptive_marking) {
		/* The memory management operations, in order. */
		for (index = 0U; index < picture->mmco_count; index++) {
			reason = dpb_operation(dpb, &picture->mmco[index], picture->info.frame_num, max_frame_num, plan->setup, &current_long);
			if (reason != NULL)
				return reason;
		}
	} else {
		/* The sliding window. */
		reason = dpb_sliding_window(dpb, picture->info.frame_num, max_frame_num, plan->setup);
		if (reason != NULL)
			return reason;
	}

	/* The picture's own marking, as the pictures after it see it (frame_num 0 and the counts less the smaller after operation 5). */
	current->frame_num = picture->info.frame_num;
	if (picture->restart)
		current->frame_num = 0U;
	current->poc[0] = picture->reference_poc[0];
	current->poc[1] = picture->reference_poc[1];
	if (current_long) {
		current->reference = DPB_LONG;
		if (picture->info.flags.IdrPicFlag)
			current->long_term_frame_idx = 0U;
	} else {
		current->reference = DPB_SHORT;
	}
	dpb->previous_reference_frame_num = current->frame_num;

	/* No more references than the sequence allows. */
	count = 0U;
	for (slot = 0U; slot < dpb->slots; slot++) {
		if (dpb->entry[slot].reference != DPB_UNUSED)
			count++;
	}
	if (count > dpb->max_references && dpb->max_references != 0U)
		return "more references than max_num_ref_frames";

	/* Succeeded: the DPB is marked. */
	return NULL;
}

/* Reports a short-term frame's FrameNumWrap (8.2.4.1): its frame_num, less MaxFrameNum when above the current one. */
static int32_t
dpb_frame_num_wrap(
	const struct dpb_entry *entry,
	uint32_t frame_num,
	uint32_t max_frame_num)
{
	/* A number above the current picture's was before the wrap. */
	if (entry->frame_num > frame_num)
		return (int32_t)entry->frame_num - (int32_t)max_frame_num;

	/* Succeeded: the number itself. */
	return (int32_t)entry->frame_num;
}

/* Finds the short-term frame of a PicNum, other than the slot skipped; -1 when none. */
static int
dpb_find_short(
	const struct dpb *dpb,
	int32_t pic_num,
	uint32_t frame_num,
	uint32_t max_frame_num,
	int32_t skip)
{
	uint32_t slot;
	int32_t wrap;

	/* Every short-term slot. */
	for (slot = 0U; slot < dpb->slots; slot++) {
		if ((int32_t)slot == skip || dpb->entry[slot].reference != DPB_SHORT)
			continue;
		wrap = dpb_frame_num_wrap(&dpb->entry[slot], frame_num, max_frame_num);
		if (wrap == pic_num)
			return (int)slot;
	}

	/* None. */
	return -1;
}

/* Finds the long-term frame of a LongTermFrameIdx (its LongTermPicNum for frames), other than the slot skipped; -1 when none. */
static int
dpb_find_long(
	const struct dpb *dpb,
	uint32_t index,
	int32_t skip)
{
	uint32_t slot;

	/* Every long-term slot. */
	for (slot = 0U; slot < dpb->slots; slot++) {
		if ((int32_t)slot == skip || dpb->entry[slot].reference != DPB_LONG)
			continue;
		if (dpb->entry[slot].long_term_frame_idx == index)
			return (int)slot;
	}

	/* None. */
	return -1;
}

/* Applies one memory management operation (8.2.5.4) of the picture in slot `current`. */
static const char *
dpb_operation(
	struct dpb *dpb,
	const struct h264_mmco *mmco,
	uint32_t frame_num,
	uint32_t max_frame_num,
	int32_t current,
	int *current_long)
{
	int32_t pic_num;
	uint32_t slot;
	int found;
	int other;

	/* picNumX of operations 1 and 3: CurrPicNum - (difference_of_pic_nums_minus1 + 1). */
	pic_num = (int32_t)frame_num - (int32_t)(mmco->difference_of_pic_nums_minus1 + 1U);

	/* The operation. */
	switch (mmco->operation) {
	case H264_MMCO_SHORT_UNUSED:
		/* A short-term frame becomes unused. */
		found = dpb_find_short(dpb, pic_num, frame_num, max_frame_num, current);
		if (found < 0)
			return "operation 1 names no short-term frame";
		dpb->entry[found].reference = DPB_UNUSED;
		break;
	case H264_MMCO_LONG_UNUSED:
		/* A long-term frame becomes unused. */
		found = dpb_find_long(dpb, mmco->long_term_pic_num, current);
		if (found < 0)
			return "operation 2 names no long-term frame";
		dpb->entry[found].reference = DPB_UNUSED;
		break;
	case H264_MMCO_SHORT_TO_LONG:
		/* A short-term frame becomes long-term, replacing one of the same index. */
		found = dpb_find_short(dpb, pic_num, frame_num, max_frame_num, current);
		if (found < 0)
			return "operation 3 names no short-term frame";
		other = dpb_find_long(dpb, mmco->long_term_frame_idx, found);
		if (other >= 0)
			dpb->entry[other].reference = DPB_UNUSED;
		dpb->entry[found].reference = DPB_LONG;
		dpb->entry[found].long_term_frame_idx = mmco->long_term_frame_idx;
		break;
	case H264_MMCO_MAX_LONG_INDEX:
		/* The long-term frames above the new largest index become unused. */
		dpb->max_long_term_frame_idx = (int32_t)mmco->max_long_term_frame_idx_plus1 - 1;
		for (slot = 0U; slot < dpb->slots; slot++) {
			if (dpb->entry[slot].reference == DPB_LONG &&
			    (int32_t)dpb->entry[slot].long_term_frame_idx > dpb->max_long_term_frame_idx)
				dpb->entry[slot].reference = DPB_UNUSED;
		}
		break;
	case H264_MMCO_CURRENT_TO_LONG:
		/* The current picture becomes long-term, replacing one of the same index. */
		other = dpb_find_long(dpb, mmco->long_term_frame_idx, current);
		if (other >= 0)
			dpb->entry[other].reference = DPB_UNUSED;
		dpb->entry[current].long_term_frame_idx = mmco->long_term_frame_idx;
		*current_long = 1;
		break;
	case H264_MMCO_ALL_UNUSED:
		/* Every other picture becomes unused, and no long-term index is left. */
		for (slot = 0U; slot < dpb->slots; slot++) {
			if ((int32_t)slot != current)
				dpb->entry[slot].reference = DPB_UNUSED;
		}
		dpb->max_long_term_frame_idx = -1;
		break;
	default:
		/* The reader keeps no other operation. */
		return "unknown memory management operation";
	}

	/* Succeeded: the operation is applied. */
	return NULL;
}

/*
 * Applies the sliding window (8.2.5.3): with the DPB full of references,
 * the short-term frame of the smallest FrameNumWrap becomes unused.
 */
static const char *
dpb_sliding_window(
	struct dpb *dpb,
	uint32_t frame_num,
	uint32_t max_frame_num,
	int32_t current)
{
	uint32_t slot;
	uint32_t count;
	uint32_t limit;
	int32_t wrap;
	int32_t smallest;
	int oldest;

	/* The references besides the current picture. */
	count = 0U;
	for (slot = 0U; slot < dpb->slots; slot++) {
		if ((int32_t)slot != current && dpb->entry[slot].reference != DPB_UNUSED)
			count++;
	}

	/* A DPB below Max(max_num_ref_frames, 1) keeps them all. */
	limit = dpb->max_references;
	if (limit == 0U)
		limit = 1U;
	if (count < limit)
		return NULL;

	/* The oldest short-term frame goes. */
	oldest = -1;
	smallest = 0;
	for (slot = 0U; slot < dpb->slots; slot++) {
		if ((int32_t)slot == current || dpb->entry[slot].reference != DPB_SHORT)
			continue;
		wrap = dpb_frame_num_wrap(&dpb->entry[slot], frame_num, max_frame_num);
		if (oldest < 0 || wrap < smallest) {
			oldest = (int)slot;
			smallest = wrap;
		}
	}
	if (oldest < 0)
		return "a full DPB without a short-term frame";
	dpb->entry[oldest].reference = DPB_UNUSED;

	/* Succeeded: room for the current picture. */
	return NULL;
}

/*
 * Puts in the frames a gap in frame_num implies (8.2.5.2): for each number
 * skipped after the last reference picture's, the sliding window, then a
 * short-term frame without a picture in a free slot.  Returns NULL, or why
 * the probe cannot follow the gap (a sequence that does not allow one).
 */
static const char *
dpb_fill_gap(
	struct dpb *dpb,
	const StdVideoH264SequenceParameterSet *sps,
	uint32_t frame_num,
	uint32_t max_frame_num)
{
	struct dpb_entry *entry;
	const char *reason;
	uint32_t unused;
	uint32_t slot;
	int found;

	/* A gap the sequence does not allow is a lost picture, which the probe does not conceal. */
	if (!sps->flags.gaps_in_frame_num_value_allowed_flag)
		return "a gap in frame_num the sequence does not allow";

	/* Each number skipped. */
	unused = (dpb->previous_reference_frame_num + 1U) % max_frame_num;
	while (unused != frame_num) {
		/* Room by the sliding window. */
		reason = dpb_sliding_window(dpb, unused, max_frame_num, -1);
		if (reason != NULL)
			return reason;

		/* The first slot without a reference holds the frame. */
		found = -1;
		for (slot = 0U; slot < dpb->slots; slot++) {
			if (dpb->entry[slot].reference == DPB_UNUSED) {
				found = (int)slot;
				break;
			}
		}
		if (found < 0)
			return "no free slot in the DPB for a gap in frame_num";
		entry = &dpb->entry[found];
		entry->reference = DPB_SHORT;
		entry->missing = 1;
		entry->frame_num = unused;
		entry->poc[0] = 0;
		entry->poc[1] = 0;
		dpb->previous_reference_frame_num = unused;
		unused = (unused + 1U) % max_frame_num;
	}

	/* Succeeded: the gap is filled. */
	return NULL;
}
