/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reference picture lists of the software H.264 decoder (8.2.4).
 */

#include "h264sw-reflist.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/* The largest MaxFrameNum a sequence may have (7.4.2.1.1: log2_max_frame_num_minus4 at most 12). */
#define REFLIST_MAX_FRAME_NUM_LIMIT 65536U

/* The most reference entries one list is built from: every buffer of the DPB. */
#define REFLIST_MAX_ENTRIES H264SW_MAX_DPB_FRAMES

/* The range of an int32_t, for the explicit narrowing of a picture number. */
#define REFLIST_INT32_MIN (-2147483647 - 1)
#define REFLIST_INT32_MAX 2147483647

/* The modification_of_pic_nums_idc that ends the loop of a list (Table 7-7). */
#define REFLIST_MOD_END 3U

/* What a candidate's key is: PicNum or LongTermPicNum, or PicOrderCnt. */
#define REFLIST_KEY_PIC_NUM 0
#define REFLIST_KEY_POC 1

/* Which short-term entries a part of a B list takes, by their order count against the current picture's. */
#define REFLIST_SELECT_BELOW 0
#define REFLIST_SELECT_ABOVE_OR_EQUAL 1
#define REFLIST_SELECT_ABOVE 2
#define REFLIST_SELECT_BELOW_OR_EQUAL 3

/*
 * One reference entry of the DPB on its way into an initial list.
 *
 * index is the DPB buffer; key is the value the entries are ordered by.
 * It lives on the stack of one list construction.
 */
struct reflist_candidate {
	unsigned index;
	int32_t key;
};

/* Static function declarations. */
static int32_t reflist_clamp32(int64_t value);
static int32_t reflist_frame_num_wrap(uint32_t entry_frame_num, uint32_t frame_num, uint32_t max_frame_num);
static int32_t reflist_pic_num(const struct h264sw_dpb_entry *entry, uint32_t frame_num, uint32_t max_frame_num);
static void reflist_make_ref(const struct h264sw_dpb_entry *entry, uint32_t frame_num, uint32_t max_frame_num, struct h264sw_ref *ref);
static void reflist_make_missing(struct h264sw_ref *ref);
static unsigned reflist_collect(const struct h264sw_dpb *dpb, unsigned marking, int key_kind, uint32_t frame_num, uint32_t max_frame_num, struct reflist_candidate *out);
static void reflist_sort(struct reflist_candidate *items, unsigned count, int descending);
static unsigned reflist_select(const struct reflist_candidate *items, unsigned count, int32_t poc, int selection, struct reflist_candidate *out);
static void reflist_append(unsigned *order, unsigned *count, const struct reflist_candidate *items, unsigned item_count);
static unsigned reflist_initial_p(const struct h264sw_dpb *dpb, uint32_t frame_num, uint32_t max_frame_num, unsigned *order);
static unsigned reflist_initial_b(const struct h264sw_dpb *dpb, uint32_t frame_num, uint32_t max_frame_num, int32_t poc, unsigned which, unsigned *order);
static void reflist_load(const struct h264sw_dpb *dpb, const unsigned *order, unsigned order_count, unsigned active, uint32_t frame_num, uint32_t max_frame_num, struct h264sw_ref *list);
static int reflist_find_short(const struct h264sw_dpb *dpb, int64_t pic_num, uint32_t frame_num, uint32_t max_frame_num, struct h264sw_ref *ref);
static int reflist_find_long(const struct h264sw_dpb *dpb, uint32_t long_term_pic_num, uint32_t frame_num, uint32_t max_frame_num, struct h264sw_ref *ref);
static int reflist_modify(const struct h264sw_dpb *dpb, const struct h264sw_slice_header *header, unsigned which, uint32_t frame_num, uint32_t max_frame_num, struct h264sw_ref *list);
static int reflist_dpb_standin(const struct h264sw_dpb *dpb, uint32_t frame_num, uint32_t max_frame_num, struct h264sw_ref *ref);
static void reflist_fill_missing(struct h264sw_reflist *lists, unsigned used, const struct h264sw_ref *dpb_standin);

/*
 * Builds the reference picture lists of a P or B slice (8.2.4).
 *
 * Both lists are cleared first, so a failure leaves counts of 0.  The
 * initial lists are made from the DPB (8.2.4.2.1, 8.2.4.2.3), cut or
 * padded to num_ref_idx_active (8.2.4.2), modified (8.2.4.3), and the
 * positions without a picture are given a stand-in.
 */
int
h264sw_reflist_build(
	const struct h264sw_dpb *dpb,
	const struct h264sw_slice_header *header,
	uint32_t frame_num,
	uint32_t max_frame_num,
	int32_t poc,
	struct h264sw_reflist *lists)
{
	unsigned order[2][REFLIST_MAX_ENTRIES];
	unsigned order_count[2];
	struct h264sw_ref dpb_standin;
	unsigned used;
	unsigned which;
	unsigned position;
	unsigned slice_type;
	int same;
	int found;
	int error;

	/* Nothing can be written without the output structure. */
	if (lists == NULL)
		return EINVAL;

	/* Clears both lists, so that a failure leaves nothing to follow. */
	memset(lists, 0, sizeof(*lists));

	/* Refuses the calls that cannot work. */
	if (dpb == NULL)
		return EINVAL;
	if (header == NULL)
		return EINVAL;

	/* The picture numbers wrap at MaxFrameNum, and the current frame_num is below it (7.4.2.1.1, 7.4.3). */
	if (max_frame_num == 0U)
		return EINVAL;
	if (max_frame_num > REFLIST_MAX_FRAME_NUM_LIMIT)
		return EINVAL;
	if (frame_num >= max_frame_num)
		return EINVAL;

	/* An intra slice predicts from no list: both counts stay 0. */
	slice_type = header->slice_type;
	if (slice_type == H264SW_SLICE_I)
		return 0;
	if (slice_type == H264SW_SLICE_SI)
		return 0;

	/* Decides how many lists the slice uses: one for a P slice (and SP, which is built alike), two for a B slice. */
	if (slice_type == H264SW_SLICE_P || slice_type == H264SW_SLICE_SP) {
		used = 1U;
	} else if (slice_type == H264SW_SLICE_B) {
		used = 2U;
	} else {
		/* A value above 4 after the modulo is not a slice type. */
		return EINVAL;
	}

	/* The list lengths are num_ref_idx_active, 1 to 32 (7.4.3). */
	for (which = 0U;
	     which < used;
	     which++) {
		if (header->num_ref_idx_active[which] < 1U)
			return EINVAL;
		if (header->num_ref_idx_active[which] > H264SW_MAX_LIST)
			return EINVAL;
	}

	/* Finds a frame to stand in for pictures that are absent; a DPB without any frame cannot predict anything. */
	found = reflist_dpb_standin(dpb, frame_num, max_frame_num, &dpb_standin);
	if (!found)
		return EINVAL;

	/* Orders the DPB's references for the initial lists. */
	order_count[1] = 0U;
	if (slice_type == H264SW_SLICE_B) {
		order_count[0] = reflist_initial_b(dpb, frame_num, max_frame_num, poc, 0U, order[0]);
		order_count[1] = reflist_initial_b(dpb, frame_num, max_frame_num, poc, 1U, order[1]);

		/* Compares list 1 with list 0 as they stand before the lists are cut (8.2.4.2.3 step 3, 8.2.4.2). */
		same = 1;
		if (order_count[0] != order_count[1])
			same = 0;
		for (position = 0U;
		     same && position < order_count[0];
		     position++) {
			if (order[0][position] != order[1][position])
				same = 0;
		}

		/* A list 1 with more than one entry that equals list 0 has its first two entries switched. */
		if (same && order_count[1] > 1U) {
			position = order[1][0];
			order[1][0] = order[1][1];
			order[1][1] = position;
		}
	} else {
		order_count[0] = reflist_initial_p(dpb, frame_num, max_frame_num, order[0]);
	}

	/* Cuts or pads each initial list to num_ref_idx_active and modifies it (8.2.4.2, 8.2.4.3). */
	for (which = 0U;
	     which < used;
	     which++) {
		lists->count[which] = header->num_ref_idx_active[which];
		reflist_load(dpb, order[which], order_count[which], lists->count[which], frame_num, max_frame_num, lists->list[which]);

		error = reflist_modify(dpb, header, which, frame_num, max_frame_num, lists->list[which]);
		if (error != 0) {
			/* A broken modification leaves no usable list. */
			memset(lists, 0, sizeof(*lists));
			return error;
		}
	}

	/* Gives every position without a picture the frame that stands in for it. */
	reflist_fill_missing(lists, used, &dpb_standin);

	/* Succeeded: the lists hold a frame in every position. */
	return 0;
}

/*
 * Finds the lowest index of list 0 whose frame is the frame of a given id (8.4.1.2.3, refIdxL0).
 *
 * A position that had no picture holds a stand-in, which is not the
 * frame the stream meant, so it is not looked at.
 */
int
h264sw_reflist_find(
	const struct h264sw_reflist *lists,
	uint32_t frame_id)
{
	unsigned count;
	unsigned index;

	/* No lists, no index. */
	if (lists == NULL)
		return -1;

	/* The count is bounded by the array, whatever the structure holds. */
	count = lists->count[0];
	if (count > H264SW_MAX_LIST)
		count = H264SW_MAX_LIST;

	/* Looks for the first entry that is a real picture with this frame. */
	for (index = 0U;
	     index < count;
	     index++) {
		if (lists->list[0][index].missing)
			continue;
		if (lists->list[0][index].frame == NULL)
			continue;
		if (lists->list[0][index].frame->id == frame_id)
			return (int)index;
	}

	/* No entry of list 0 holds that frame. */
	return -1;
}

/*
 * Narrows a value to the range of an int32_t by clamping it.
 */
static int32_t
reflist_clamp32(
	int64_t value)
{
	/* A value below the range becomes the lowest. */
	if (value < REFLIST_INT32_MIN)
		return REFLIST_INT32_MIN;

	/* A value above the range becomes the highest. */
	if (value > REFLIST_INT32_MAX)
		return REFLIST_INT32_MAX;

	/* The value is inside the range. */
	return (int32_t)value;
}

/*
 * Derives FrameNumWrap of a short-term frame (8-27).
 *
 * A frame whose frame_num is above the current picture's was decoded
 * before the counter wrapped, so it counts as MaxFrameNum below.
 */
static int32_t
reflist_frame_num_wrap(
	uint32_t entry_frame_num,
	uint32_t frame_num,
	uint32_t max_frame_num)
{
	int64_t wrap;

	/* The frame_num of the frame, taken as a signed number. */
	wrap = (int64_t)entry_frame_num;

	/* A frame_num above the current one has wrapped. */
	if (entry_frame_num > frame_num)
		wrap = wrap - (int64_t)max_frame_num;

	/* The result is far inside the range for a valid DPB; clamps a broken one. */
	return reflist_clamp32(wrap);
}

/*
 * Derives PicNum of a short-term entry (8-28) or LongTermPicNum of a long-term one (8-29).
 */
static int32_t
reflist_pic_num(
	const struct h264sw_dpb_entry *entry,
	uint32_t frame_num,
	uint32_t max_frame_num)
{
	/* A long-term frame is numbered by its LongTermFrameIdx. */
	if (entry->reference == H264SW_REF_LONG)
		return reflist_clamp32((int64_t)entry->long_term_frame_idx);

	/* A short-term frame is numbered by FrameNumWrap. */
	return reflist_frame_num_wrap(entry->frame_num, frame_num, max_frame_num);
}

/*
 * Describes a DPB entry as a list entry that holds a real picture.
 */
static void
reflist_make_ref(
	const struct h264sw_dpb_entry *entry,
	uint32_t frame_num,
	uint32_t max_frame_num,
	struct h264sw_ref *ref)
{
	/* The frame is borrowed from the DPB. */
	ref->frame = entry->frame;

	/* The marking decides whether the number is a PicNum or a LongTermPicNum. */
	ref->long_term = 0;
	if (entry->reference == H264SW_REF_LONG)
		ref->long_term = 1;

	/* The order count of the frame, and its picture number. */
	ref->poc = entry->frame->poc;
	ref->pic_num = reflist_pic_num(entry, frame_num, max_frame_num);

	/* The position holds a picture. */
	ref->missing = 0;
}

/*
 * Describes a position that has no picture yet.
 *
 * Its stand-in is given once every list is known.
 */
static void
reflist_make_missing(
	struct h264sw_ref *ref)
{
	/* Nothing is referred to until a stand-in is chosen. */
	ref->frame = NULL;
	ref->long_term = 0;
	ref->poc = 0;
	ref->pic_num = 0;

	/* The position had no picture. */
	ref->missing = 1;
}

/*
 * Collects the DPB entries with one marking, with the key they are ordered by.
 *
 * The key is the order count for REFLIST_KEY_POC, else the picture
 * number of the marking.  Returns the number of candidates written to
 * out, at most one per buffer of the DPB.
 */
static unsigned
reflist_collect(
	const struct h264sw_dpb *dpb,
	unsigned marking,
	int key_kind,
	uint32_t frame_num,
	uint32_t max_frame_num,
	struct reflist_candidate *out)
{
	const struct h264sw_dpb_entry *entry;
	unsigned count;
	unsigned index;

	/* Walks every buffer of the DPB once. */
	count = 0U;
	for (index = 0U;
	     index < H264SW_MAX_DPB_FRAMES;
	     index++) {
		entry = &dpb->entry[index];

		/* An empty buffer or a frame with another marking is not a candidate. */
		if (entry->frame == NULL)
			continue;
		if (entry->reference != marking)
			continue;

		/* Records the buffer with the key of the order it is sorted in. */
		out[count].index = index;
		if (key_kind == REFLIST_KEY_POC)
			out[count].key = entry->frame->poc;
		else
			out[count].key = reflist_pic_num(entry, frame_num, max_frame_num);
		count++;
	}

	/* Reports how many candidates there are. */
	return count;
}

/*
 * Sorts candidates by key, keeping the DPB order among equal keys.
 *
 * There are at most 16 candidates, so an insertion sort is short.
 */
static void
reflist_sort(
	struct reflist_candidate *items,
	unsigned count,
	int descending)
{
	struct reflist_candidate held;
	unsigned index;
	unsigned position;
	int in_place;

	/* Inserts each candidate after the ones that must precede it. */
	for (index = 1U;
	     index < count;
	     index++) {
		held = items[index];
		position = index;

		/* Moves the candidates that must follow the held one up a place, until the held one fits. */
		while (position > 0U) {
			in_place = 0;
			if (descending && items[position - 1U].key >= held.key)
				in_place = 1;
			if (!descending && items[position - 1U].key <= held.key)
				in_place = 1;
			if (in_place)
				break;

			items[position] = items[position - 1U];
			position--;
		}

		items[position] = held;
	}
}

/*
 * Takes the candidates whose key stands in a given relation to the current order count.
 *
 * Returns how many were written to out.
 */
static unsigned
reflist_select(
	const struct reflist_candidate *items,
	unsigned count,
	int32_t poc,
	int selection,
	struct reflist_candidate *out)
{
	unsigned taken;
	unsigned index;
	int takes;

	/* Looks at each candidate in turn. */
	taken = 0U;
	for (index = 0U;
	     index < count;
	     index++) {
		takes = 0;
		if (selection == REFLIST_SELECT_BELOW && items[index].key < poc)
			takes = 1;
		if (selection == REFLIST_SELECT_ABOVE_OR_EQUAL && items[index].key >= poc)
			takes = 1;
		if (selection == REFLIST_SELECT_ABOVE && items[index].key > poc)
			takes = 1;
		if (selection == REFLIST_SELECT_BELOW_OR_EQUAL && items[index].key <= poc)
			takes = 1;

		if (takes) {
			out[taken] = items[index];
			taken++;
		}
	}

	/* Reports how many candidates were taken. */
	return taken;
}

/*
 * Appends the DPB buffers of some candidates to the order of an initial list.
 */
static void
reflist_append(
	unsigned *order,
	unsigned *count,
	const struct reflist_candidate *items,
	unsigned item_count)
{
	unsigned index;

	/* Adds the buffers in the order the candidates are in. */
	for (index = 0U;
	     index < item_count;
	     index++) {
		order[*count] = items[index].index;
		(*count)++;
	}
}

/*
 * Orders the references of a P slice's initial list (8.2.4.2.1).
 *
 * Short-term frames by descending PicNum, then long-term frames by
 * ascending LongTermPicNum.  Returns how many buffers are in order.
 */
static unsigned
reflist_initial_p(
	const struct h264sw_dpb *dpb,
	uint32_t frame_num,
	uint32_t max_frame_num,
	unsigned *order)
{
	struct reflist_candidate shorts[REFLIST_MAX_ENTRIES];
	struct reflist_candidate longs[REFLIST_MAX_ENTRIES];
	unsigned short_count;
	unsigned long_count;
	unsigned count;

	/* Collects the two kinds of reference. */
	short_count = reflist_collect(dpb, H264SW_REF_SHORT, REFLIST_KEY_PIC_NUM, frame_num, max_frame_num, shorts);
	long_count = reflist_collect(dpb, H264SW_REF_LONG, REFLIST_KEY_PIC_NUM, frame_num, max_frame_num, longs);

	/* Short-term frames come first, highest PicNum first. */
	reflist_sort(shorts, short_count, 1);

	/* Long-term frames follow, lowest LongTermPicNum first. */
	reflist_sort(longs, long_count, 0);

	/* Joins the two parts. */
	count = 0U;
	reflist_append(order, &count, shorts, short_count);
	reflist_append(order, &count, longs, long_count);

	/* Reports the length of the whole initial list. */
	return count;
}

/*
 * Orders the references of one initial list of a B slice (8.2.4.2.3).
 *
 * List 0 takes the short-term frames with an order count below the
 * current one by descending order count, then the others by ascending
 * order count; list 1 takes those above the current one by ascending
 * order count, then the others by descending order count.  The
 * long-term frames follow by ascending LongTermPicNum.  Returns how
 * many buffers are in order.
 */
static unsigned
reflist_initial_b(
	const struct h264sw_dpb *dpb,
	uint32_t frame_num,
	uint32_t max_frame_num,
	int32_t poc,
	unsigned which,
	unsigned *order)
{
	struct reflist_candidate shorts[REFLIST_MAX_ENTRIES];
	struct reflist_candidate longs[REFLIST_MAX_ENTRIES];
	struct reflist_candidate part[REFLIST_MAX_ENTRIES];
	unsigned short_count;
	unsigned long_count;
	unsigned part_count;
	unsigned count;

	/* Collects the two kinds of reference. */
	short_count = reflist_collect(dpb, H264SW_REF_SHORT, REFLIST_KEY_POC, frame_num, max_frame_num, shorts);
	long_count = reflist_collect(dpb, H264SW_REF_LONG, REFLIST_KEY_PIC_NUM, frame_num, max_frame_num, longs);
	count = 0U;

	/* Builds the short-term part of the list. */
	if (which == 0U) {
		/* List 0: the frames before the current picture in output order, nearest first. */
		part_count = reflist_select(shorts, short_count, poc, REFLIST_SELECT_BELOW, part);
		reflist_sort(part, part_count, 1);
		reflist_append(order, &count, part, part_count);

		/* List 0: then the frames after it (or at the same count), nearest first. */
		part_count = reflist_select(shorts, short_count, poc, REFLIST_SELECT_ABOVE_OR_EQUAL, part);
		reflist_sort(part, part_count, 0);
		reflist_append(order, &count, part, part_count);
	} else {
		/* List 1: the frames after the current picture in output order, nearest first. */
		part_count = reflist_select(shorts, short_count, poc, REFLIST_SELECT_ABOVE, part);
		reflist_sort(part, part_count, 0);
		reflist_append(order, &count, part, part_count);

		/* List 1: then the frames before it (or at the same count), nearest first. */
		part_count = reflist_select(shorts, short_count, poc, REFLIST_SELECT_BELOW_OR_EQUAL, part);
		reflist_sort(part, part_count, 1);
		reflist_append(order, &count, part, part_count);
	}

	/* The long-term frames follow, lowest LongTermPicNum first. */
	reflist_sort(longs, long_count, 0);
	reflist_append(order, &count, longs, long_count);

	/* Reports the length of the whole initial list. */
	return count;
}

/*
 * Fills a list from an initial order: the first active entries, padded with "no reference picture" (8.2.4.2).
 */
static void
reflist_load(
	const struct h264sw_dpb *dpb,
	const unsigned *order,
	unsigned order_count,
	unsigned active,
	uint32_t frame_num,
	uint32_t max_frame_num,
	struct h264sw_ref *list)
{
	unsigned index;

	/* Takes the entries of the initial list that fit and leaves the rest empty. */
	for (index = 0U;
	     index < active;
	     index++) {
		if (index < order_count)
			reflist_make_ref(&dpb->entry[order[index]], frame_num, max_frame_num, &list[index]);
		else
			reflist_make_missing(&list[index]);
	}
}

/*
 * Finds the short-term frame with a given PicNum.
 *
 * Returns 1 and describes it in ref, or 0 when no short-term frame has
 * that number.
 */
static int
reflist_find_short(
	const struct h264sw_dpb *dpb,
	int64_t pic_num,
	uint32_t frame_num,
	uint32_t max_frame_num,
	struct h264sw_ref *ref)
{
	const struct h264sw_dpb_entry *entry;
	unsigned index;

	/* Looks at each short-term frame for the number. */
	for (index = 0U;
	     index < H264SW_MAX_DPB_FRAMES;
	     index++) {
		entry = &dpb->entry[index];
		if (entry->frame == NULL)
			continue;
		if (entry->reference != H264SW_REF_SHORT)
			continue;
		if ((int64_t)reflist_pic_num(entry, frame_num, max_frame_num) != pic_num)
			continue;

		/* Found the picture the modification names. */
		reflist_make_ref(entry, frame_num, max_frame_num, ref);
		return 1;
	}

	/* The DPB has no such picture. */
	return 0;
}

/*
 * Finds the long-term frame with a given LongTermPicNum.
 *
 * Returns 1 and describes it in ref, or 0 when no long-term frame has
 * that number.
 */
static int
reflist_find_long(
	const struct h264sw_dpb *dpb,
	uint32_t long_term_pic_num,
	uint32_t frame_num,
	uint32_t max_frame_num,
	struct h264sw_ref *ref)
{
	const struct h264sw_dpb_entry *entry;
	unsigned index;

	/* Looks at each long-term frame for the number (a frame's LongTermPicNum is its LongTermFrameIdx, 8-29). */
	for (index = 0U;
	     index < H264SW_MAX_DPB_FRAMES;
	     index++) {
		entry = &dpb->entry[index];
		if (entry->frame == NULL)
			continue;
		if (entry->reference != H264SW_REF_LONG)
			continue;
		if (entry->long_term_frame_idx != long_term_pic_num)
			continue;

		/* Found the picture the modification names. */
		reflist_make_ref(entry, frame_num, max_frame_num, ref);
		return 1;
	}

	/* The DPB has no such picture. */
	return 0;
}

/*
 * Modifies one list as its modification operations say (8.2.4.3, 8.2.4.3.1, 8.2.4.3.2).
 *
 * Works on a copy one entry longer than the list, as the clauses'
 * pseudo-code does, and keeps the first active entries.  A named picture
 * the DPB lacks is put in as a position without a picture.  Operations
 * past the end of the list change nothing that is kept, so they stop the
 * loop.  Returns 0, or EINVAL for a count above the header's limit or an
 * idc above 3.
 */
static int
reflist_modify(
	const struct h264sw_dpb *dpb,
	const struct h264sw_slice_header *header,
	unsigned which,
	uint32_t frame_num,
	uint32_t max_frame_num,
	struct h264sw_ref *list)
{
	struct h264sw_ref work[H264SW_MAX_LIST + 1U];
	const struct h264sw_modification *operation;
	struct h264sw_ref wanted;
	unsigned count;
	unsigned active;
	unsigned operation_index;
	unsigned ref_idx;
	unsigned position;
	unsigned source;
	unsigned target;
	int64_t pic_num_pred;
	int64_t pic_num_no_wrap;
	int64_t pic_num;
	int64_t step;
	int found;
	int removes;

	/* Nothing is modified when no operation is sent. */
	count = header->modification_count[which];
	if (count > H264SW_MAX_MODIFICATIONS)
		return EINVAL;
	if (count == 0U)
		return 0;

	/* Copies the list into the working list, which has one more entry (8-37, 8-38 NOTE 2). */
	active = header->num_ref_idx_active[which];
	for (position = 0U;
	     position < active;
	     position++)
		work[position] = list[position];
	reflist_make_missing(&work[active]);

	/* picNumLXPred starts at CurrPicNum, and refIdxLX at 0 (8.2.4.3, 8.2.4.3.1). */
	pic_num_pred = (int64_t)frame_num;
	ref_idx = 0U;

	/* Applies the operations in the order they were sent. */
	for (operation_index = 0U;
	     operation_index < count;
	     operation_index++) {
		operation = &header->modification[which][operation_index];

		/* An idc of 3 ends the loop; larger values are not a modification. */
		if (operation->idc == REFLIST_MOD_END)
			break;
		if (operation->idc > REFLIST_MOD_END)
			return EINVAL;

		/* Once the list is full, further operations leave the kept entries as they are. */
		if (ref_idx >= active)
			break;

		/* Starts from "no picture", which stays when the DPB lacks the one named. */
		found = 0;
		reflist_make_missing(&wanted);

		/* Finds the picture the operation names. */
		if (operation->idc == H264SW_MOD_LONG_TERM) {
			/* idc 2 names a long-term picture by LongTermPicNum (8.2.4.3.2). */
			found = reflist_find_long(dpb, operation->value, frame_num, max_frame_num, &wanted);
		} else if (operation->value < max_frame_num) {
			/* idc 0 and 1 name a short-term picture by its distance from picNumLXPred (8-34, 8-35). */
			step = (int64_t)operation->value + 1;
			if (operation->idc == H264SW_MOD_SUBTRACT) {
				pic_num_no_wrap = pic_num_pred - step;
				if (pic_num_no_wrap < 0)
					pic_num_no_wrap = pic_num_no_wrap + (int64_t)max_frame_num;
			} else {
				pic_num_no_wrap = pic_num_pred + step;
				if (pic_num_no_wrap >= (int64_t)max_frame_num)
					pic_num_no_wrap = pic_num_no_wrap - (int64_t)max_frame_num;
			}

			/* picNumLXNoWrap becomes the prediction for the next operation. */
			pic_num_pred = pic_num_no_wrap;

			/* A number above CurrPicNum is one from before the wrap (8-36). */
			pic_num = pic_num_no_wrap;
			if (pic_num_no_wrap > (int64_t)frame_num)
				pic_num = pic_num_no_wrap - (int64_t)max_frame_num;

			found = reflist_find_short(dpb, pic_num, frame_num, max_frame_num, &wanted);
		} else {
			/* A difference of MaxFrameNum or more is out of range (7.4.3.1): no picture. */
			found = 0;
		}

		/* Makes room at refIdxLX by moving the later entries one place down (8-37, 8-38). */
		for (position = active;
		     position > ref_idx;
		     position--)
			work[position] = work[position - 1U];

		/* Puts the picture at refIdxLX and advances it. */
		work[ref_idx] = wanted;
		ref_idx++;

		/* Takes the other copies of the picture out of the entries after it: a short-term entry with the same PicNum, or a long-term one with the same LongTermPicNum. */
		target = ref_idx;
		for (source = ref_idx;
		     source <= active;
		     source++) {
			removes = 0;
			if (found && !work[source].missing) {
				if (operation->idc == H264SW_MOD_LONG_TERM) {
					if (work[source].long_term && work[source].pic_num == wanted.pic_num)
						removes = 1;
				} else {
					if (!work[source].long_term && work[source].pic_num == wanted.pic_num)
						removes = 1;
				}
			}

			if (!removes) {
				work[target] = work[source];
				target++;
			}
		}
	}

	/* Keeps the first num_ref_idx_active entries of the modified list. */
	for (position = 0U;
	     position < active;
	     position++)
		list[position] = work[position];

	/* Succeeded: the list is modified. */
	return 0;
}

/*
 * Finds a frame of the DPB to stand in when nothing better is known.
 *
 * Prefers a frame that was decoded over a non-existing one.  Returns 1
 * and describes the frame in ref, or 0 when the DPB holds no frame.
 */
static int
reflist_dpb_standin(
	const struct h264sw_dpb *dpb,
	uint32_t frame_num,
	uint32_t max_frame_num,
	struct h264sw_ref *ref)
{
	const struct h264sw_dpb_entry *entry;
	int chosen;
	unsigned index;

	/* Takes the first decoded frame, and failing that the first frame of any kind. */
	chosen = -1;
	for (index = 0U;
	     index < H264SW_MAX_DPB_FRAMES;
	     index++) {
		entry = &dpb->entry[index];
		if (entry->frame == NULL)
			continue;
		if (chosen < 0)
			chosen = (int)index;
		if (!entry->frame->non_existing) {
			chosen = (int)index;
			break;
		}
	}

	/* A DPB with no frame has nothing to stand in. */
	if (chosen < 0)
		return 0;

	/* Describes the chosen frame; a frame no longer used for reference has no picture number. */
	entry = &dpb->entry[chosen];
	reflist_make_ref(entry, frame_num, max_frame_num, ref);
	if (entry->reference == H264SW_REF_UNUSED) {
		ref->long_term = 0;
		ref->pic_num = 0;
	}

	/* Succeeded: a stand-in exists. */
	return 1;
}

/*
 * Gives each position without a picture a stand-in frame.
 *
 * The stand-in is the first entry of the same list that has a picture,
 * else the first of the other list, else the frame of the DPB.  The
 * position stays marked missing.
 */
static void
reflist_fill_missing(
	struct h264sw_reflist *lists,
	unsigned used,
	const struct h264sw_ref *dpb_standin)
{
	struct h264sw_ref standin[2];
	unsigned which;
	unsigned other;
	unsigned index;
	int chosen;

	/* Chooses each list's stand-in before any position is changed, so that both lists are seen as the modification left them. */
	for (which = 0U;
	     which < used;
	     which++) {
		standin[which] = *dpb_standin;
		chosen = 0;

		/* The first entry of the same list that has a picture. */
		for (index = 0U;
		     index < lists->count[which];
		     index++) {
			if (chosen)
				break;
			if (!lists->list[which][index].missing) {
				standin[which] = lists->list[which][index];
				chosen = 1;
			}
		}

		/* Otherwise the first entry of the other list that has a picture. */
		if (!chosen && used > 1U) {
			other = 1U - which;
			for (index = 0U;
			     index < lists->count[other];
			     index++) {
				if (chosen)
					break;
				if (!lists->list[other][index].missing) {
					standin[which] = lists->list[other][index];
					chosen = 1;
				}
			}
		}
	}

	/* Replaces the empty positions, keeping the mark that they are missing. */
	for (which = 0U;
	     which < used;
	     which++) {
		for (index = 0U;
		     index < lists->count[which];
		     index++) {
			if (lists->list[which][index].missing) {
				lists->list[which][index] = standin[which];
				lists->list[which][index].missing = 1;
			}
		}
	}
}
