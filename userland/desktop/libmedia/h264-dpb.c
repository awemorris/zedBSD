/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Original progressive reference marking and list construction from ITU-T H.264.
 *
 * Missing references consume logical DPB capacity but never allocate a Vulkan image.
 * Admission compares each slice's normative list against the list a device constructs
 * from its real images. A dropped reference still advances marking and order history.
 */
#include "h264-dpb.h"

#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>

/* A complete initial or modified list exists only during one picture's admission. */
struct h264_reference_list {
	unsigned count;
	int entry[64];
};

static int32_t h264_pic_num(const struct h264_dpb_entry *entry, uint32_t current, uint32_t maximum);
static int32_t h264_reference_poc(const struct h264_dpb_entry *entry);
static int h264_find_short(const struct h264_dpb *dpb, int32_t pic_num, uint32_t current, uint32_t maximum);
static int h264_find_long(const struct h264_dpb *dpb, uint32_t index);
static int h264_dpb_free(const struct h264_dpb *dpb);
static int h264_sliding(struct h264_dpb *dpb, uint32_t current, uint32_t maximum);
static int h264_list_before(const struct h264_dpb *dpb, int left, int right, uint32_t current, uint32_t maximum, int32_t poc, unsigned type, unsigned list);
static void h264_initial_lists(const struct h264_dpb *dpb, const StdVideoH264SequenceParameterSet *sps, const struct h264_picture *picture, unsigned type, int real_only, struct h264_reference_list *lists);
static int h264_modify_list(const struct h264_dpb *dpb, const struct h264_list *syntax, uint32_t current, uint32_t maximum, int real_only, struct h264_reference_list *list);
static int h264_lists_admit(const struct h264_dpb *dpb, const StdVideoH264SequenceParameterSet *sps, const struct h264_picture *picture);
static void h264_reference_info(const struct h264_dpb_entry *entry, StdVideoDecodeH264ReferenceInfo *info);

/*
 * Starts an empty reference epoch for a decode session or a seek.
 */
void
h264_dpb_init(
	struct h264_dpb *dpb,
	unsigned max_references)
{
	unsigned index;

	/* Preserve the normative minimum capacity while reserving a separate current-picture image. */
	memset(dpb, 0, sizeof(*dpb));
	if (max_references == 0U)
		max_references = 1U;
	if (max_references > H264_DPB_REFERENCES)
		max_references = H264_DPB_REFERENCES;
	dpb->max_references = max_references;
	dpb->slots = max_references + 1U;
	dpb->max_long_index = -1;
	dpb->seek = 1;
	for (index = 0U; index < H264_DPB_SLOTS; index++)
		dpb->entry[index].slot = -1;
}

/*
 * Prepares gaps, picture order, per-slice reference admission and standard slot descriptors.
 *
 * A zero decode flag is a recoverable missing-reference drop; syntax errors return errno.
 */
int
h264_dpb_prepare(
	struct h264_dpb *dpb,
	struct h264_stream *stream,
	struct h264_picture *picture,
	struct h264_dpb_plan *plan)
{
	const StdVideoH264SequenceParameterSet *sps;
	struct h264_picture *gap;
	struct h264_dpb_entry *entry;
	uint32_t maximum;
	uint32_t missing;
	uint32_t expected;
	unsigned index;
	unsigned slot;
	int free_index;
	int error;
	int32_t poc;

	/* Only an intra access unit establishes a seek epoch without old references. */
	memset(plan, 0, sizeof(*plan));
	plan->setup = -1;
	sps = &stream->sps[picture->info.seq_parameter_set_id];
	maximum = 1U << (sps->log2_max_frame_num_minus4 + 4U);
	if (!dpb->started && !picture->intra)
		return 0;
	if (picture->info.flags.IdrPicFlag) {
		for (index = 0U; index < H264_DPB_SLOTS; index++)
			dpb->entry[index].reference = H264_DPB_UNUSED;
		dpb->max_long_index = -1;
		dpb->seek = 0;
	} else if (stream->reference_started) {
		/* Gap inference is invoked even for a non-reference current picture. */
		expected = (stream->previous_reference_frame_num + 1U) % maximum;
		if (picture->info.frame_num != stream->previous_reference_frame_num && picture->info.frame_num != expected) {
			gap = calloc(1U, sizeof(*gap));
			if (gap == NULL)
				return ENOMEM;
			missing = expected;
			while (missing != picture->info.frame_num) {
				error = h264_sliding(dpb, missing, maximum);
				if (error != 0) {
					free(gap);
					return error;
				}
				free_index = h264_dpb_free(dpb);
				if (free_index < 0) {
					free(gap);
					return EINVAL;
				}
				entry = &dpb->entry[free_index];
				memset(entry, 0, sizeof(*entry));
				entry->reference = H264_DPB_SHORT;
				entry->slot = -1;
				entry->inferred = 1;
				entry->frame_num = missing;
				memset(&gap->info, 0, sizeof(gap->info));
				gap->info.frame_num = (uint16_t)missing;
				gap->info.flags.is_reference = 1;
				if (sps->pic_order_cnt_type != STD_VIDEO_H264_POC_TYPE_0) {
					error = h264_picture_order(stream, sps, gap);
					if (error != 0) {
						free(gap);
						return error;
					}
					entry->poc[0] = gap->info.PicOrderCnt[0];
					entry->poc[1] = gap->info.PicOrderCnt[1];
				}
				stream->previous_reference_frame_num = missing;
				missing = (missing + 1U) % maximum;
			}
			free(gap);
		}
	}

	/* Picture order is computed once, after every inferred frame has updated the relevant state. */
	error = h264_picture_order(stream, sps, picture);
	if (error != 0)
		return error;
	dpb->started = 1;
	poc = picture->info.PicOrderCnt[0];
	if (picture->info.PicOrderCnt[1] < poc)
		poc = picture->info.PicOrderCnt[1];
	if (dpb->seek == 1) {
		dpb->seek_poc = poc;
		dpb->seek = 2;
	} else if (dpb->seek == 2 && poc < dpb->seek_poc) {
		return 0;
	}
	error = h264_lists_admit(dpb, sps, picture);
	if (error == ENOENT)
		return 0;
	if (error != 0)
		return error;

	/* Reserve an image not held by any real reference; no inferred frame reaches the device. */
	for (slot = 0U; slot < dpb->slots; slot++) {
		free_index = 1;
		for (index = 0U; index < H264_DPB_SLOTS; index++) {
			entry = &dpb->entry[index];
			if (entry->reference != H264_DPB_UNUSED && entry->slot == (int)slot)
				free_index = 0;
		}
		if (free_index != 0 && plan->setup < 0)
			plan->setup = (int32_t)slot;
		if (free_index != 0 && dpb->active[slot]) {
			plan->deactivate[plan->deactivate_count] = (int32_t)slot;
			plan->deactivate_count++;
		}
	}
	if (plan->setup < 0)
		return EINVAL;
	for (index = 0U; index < H264_DPB_SLOTS; index++) {
		entry = &dpb->entry[index];
		if (entry->reference == H264_DPB_UNUSED || entry->slot < 0)
			continue;
		plan->references[plan->reference_count] = entry->slot;
		h264_reference_info(entry, &plan->info[plan->reference_count]);
		plan->reference_count++;
	}

	/* The setup describes the current picture during decode, before its marking changes numbers. */
	plan->setup_info.FrameNum = picture->info.frame_num;
	plan->setup_info.PicOrderCnt[0] = picture->info.PicOrderCnt[0];
	plan->setup_info.PicOrderCnt[1] = picture->info.PicOrderCnt[1];
	if (picture->info.flags.IdrPicFlag && picture->long_term_reference) {
		plan->setup_info.flags.used_for_long_term_reference = 1;
		plan->setup_info.FrameNum = 0U;
	}
	for (index = 0U; index < picture->mmco_count; index++) {
		if (picture->mmco[index].operation == H264_MMCO_CURRENT_TO_LONG) {
			plan->setup_info.flags.used_for_long_term_reference = 1;
			plan->setup_info.FrameNum = (uint16_t)picture->mmco[index].long_term_frame_idx;
		}
	}
	if (!dpb->device_started)
		plan->reset = 1;
	plan->decode = 1;

	/* Succeeded: all slices select only the submitted real reference images. */
	return 0;
}

/*
 * Applies normative reference marking after either a decoded or a dropped picture.
 */
int
h264_dpb_mark(
	struct h264_dpb *dpb,
	const StdVideoH264SequenceParameterSet *sps,
	const struct h264_picture *picture,
	const struct h264_dpb_plan *plan)
{
	const struct h264_mmco *operation;
	struct h264_dpb_entry *entry;
	uint32_t maximum;
	uint32_t long_index;
	int32_t target;
	int32_t minimum;
	int found;
	int free_index;
	int current_long;
	int error;
	unsigned index;
	unsigned count;

	/* Leading non-intra pictures after a seek never establish reference history. */
	if (!dpb->started)
		return 0;

	/* A dropped picture never changes device slot activation. */
	if (plan->decode) {
		dpb->device_started = 1;
		for (index = 0U; index < plan->deactivate_count; index++)
			dpb->active[plan->deactivate[index]] = 0;
		if (picture->info.flags.is_reference)
			dpb->active[plan->setup] = 1;
	}
	if (!picture->info.flags.is_reference)
		return 0;
	maximum = 1U << (sps->log2_max_frame_num_minus4 + 4U);
	current_long = 0;
	long_index = 0U;

	/* IDR, adaptive marking and sliding-window marking have distinct reference lifetimes. */
	if (picture->info.flags.IdrPicFlag) {
		if (picture->long_term_reference) {
			current_long = 1;
			dpb->max_long_index = 0;
		}
	} else if (picture->adaptive_marking) {
		for (index = 0U; index < picture->mmco_count; index++) {
			operation = &picture->mmco[index];
			if (operation->operation == H264_MMCO_SHORT_UNUSED || operation->operation == H264_MMCO_SHORT_TO_LONG) {
				if (operation->difference_of_pic_nums_minus1 >= maximum)
					return EINVAL;
				target = (int32_t)picture->info.frame_num - (int32_t)(operation->difference_of_pic_nums_minus1 + 1U);
				found = h264_find_short(dpb, target, picture->info.frame_num, maximum);
				if (operation->operation == H264_MMCO_SHORT_UNUSED) {
					if (found >= 0)
						dpb->entry[found].reference = H264_DPB_UNUSED;
				} else {
					if (operation->long_term_frame_idx > (uint32_t)dpb->max_long_index || dpb->max_long_index < 0)
						return EINVAL;
					if (found < 0)
						return ENOENT;
					free_index = h264_find_long(dpb, operation->long_term_frame_idx);
					if (free_index >= 0)
						dpb->entry[free_index].reference = H264_DPB_UNUSED;
					dpb->entry[found].reference = H264_DPB_LONG;
					dpb->entry[found].long_index = operation->long_term_frame_idx;
				}
			} else if (operation->operation == H264_MMCO_LONG_UNUSED) {
				found = h264_find_long(dpb, operation->long_term_pic_num);
				if (found >= 0)
					dpb->entry[found].reference = H264_DPB_UNUSED;
			} else if (operation->operation == H264_MMCO_MAX_LONG_INDEX) {
				if (operation->max_long_term_frame_idx_plus1 > dpb->max_references)
					return EINVAL;
				dpb->max_long_index = (int)operation->max_long_term_frame_idx_plus1 - 1;
				for (count = 0U; count < H264_DPB_SLOTS; count++) {
					if (dpb->entry[count].reference == H264_DPB_LONG && (int)dpb->entry[count].long_index > dpb->max_long_index)
						dpb->entry[count].reference = H264_DPB_UNUSED;
				}
			} else if (operation->operation == H264_MMCO_ALL_UNUSED) {
				for (count = 0U; count < H264_DPB_SLOTS; count++)
					dpb->entry[count].reference = H264_DPB_UNUSED;
				dpb->max_long_index = -1;
			} else if (operation->operation == H264_MMCO_CURRENT_TO_LONG) {
				if (dpb->max_long_index < 0 || operation->long_term_frame_idx > (uint32_t)dpb->max_long_index)
					return EINVAL;
				found = h264_find_long(dpb, operation->long_term_frame_idx);
				if (found >= 0)
					dpb->entry[found].reference = H264_DPB_UNUSED;
				current_long = 1;
				long_index = operation->long_term_frame_idx;
			} else {
				return EINVAL;
			}
		}
	} else {
		error = h264_sliding(dpb, picture->info.frame_num, maximum);
		if (error != 0)
			return error;
	}

	/* A real or missing current reference occupies one logical slot after marking the old set. */
	free_index = h264_dpb_free(dpb);
	if (free_index < 0)
		return EINVAL;
	entry = &dpb->entry[free_index];
	memset(entry, 0, sizeof(*entry));
	entry->reference = H264_DPB_SHORT;
	entry->slot = -1;
	if (plan->decode)
		entry->slot = plan->setup;
	entry->frame_num = picture->info.frame_num;
	entry->poc[0] = picture->info.PicOrderCnt[0];
	entry->poc[1] = picture->info.PicOrderCnt[1];
	if (current_long) {
		entry->reference = H264_DPB_LONG;
		entry->long_index = long_index;
	}
	if (picture->mmco5) {
		minimum = entry->poc[0];
		if (entry->poc[1] < minimum)
			minimum = entry->poc[1];
		entry->poc[0] -= minimum;
		entry->poc[1] -= minimum;
		entry->frame_num = 0U;
	}
	count = 0U;
	for (index = 0U; index < H264_DPB_SLOTS; index++) {
		if (dpb->entry[index].reference != H264_DPB_UNUSED)
			count++;
	}
	if (count > dpb->max_references)
		return EINVAL;

	/* Succeeded: logical marking remains complete even when the current picture could not be decoded. */
	return 0;
}

/* Derive a short-term picture number in the current frame-number epoch. */
static int32_t
h264_pic_num(
	const struct h264_dpb_entry *entry,
	uint32_t current,
	uint32_t maximum)
{
	/* Older numbers beyond the current modulo value belong to the preceding wrap. */
	if (entry->frame_num > current)
		return (int32_t)entry->frame_num - (int32_t)maximum;

	/* Succeeded: no frame-number wrap separates this reference from the current picture. */
	return (int32_t)entry->frame_num;
}

/* Use the earlier field order as the progressive frame's picture order. */
static int32_t
h264_reference_poc(
	const struct h264_dpb_entry *entry)
{
	/* A frame's order is the minimum of its two field orders. */
	if (entry->poc[1] < entry->poc[0])
		return entry->poc[1];

	/* Succeeded: the top field establishes this frame's order. */
	return entry->poc[0];
}

/* Locate a logical short-term reference, including an inferred or dropped reference. */
static int
h264_find_short(
	const struct h264_dpb *dpb,
	int32_t pic_num,
	uint32_t current,
	uint32_t maximum)
{
	unsigned index;
	int32_t number;

	/* Only references still marked short-term may satisfy a short-term modification. */
	for (index = 0U; index < H264_DPB_SLOTS; index++) {
		if (dpb->entry[index].reference != H264_DPB_SHORT)
			continue;
		number = h264_pic_num(&dpb->entry[index], current, maximum);
		if (number == pic_num)
			return (int)index;
	}

	/* No retained logical reference has this picture number. */
	return -1;
}

/* Locate a retained long-term reference by its persistent frame index. */
static int
h264_find_long(
	const struct h264_dpb *dpb,
	uint32_t index)
{
	unsigned current;

	/* Long-term indices are unique within the current marking epoch. */
	for (current = 0U; current < H264_DPB_SLOTS; current++) {
		if (dpb->entry[current].reference == H264_DPB_LONG && dpb->entry[current].long_index == index)
			return (int)current;
	}

	/* No retained logical reference has this long-term index. */
	return -1;
}

/* Find storage for one logical current or inferred picture after the old set has been marked. */
static int
h264_dpb_free(
	const struct h264_dpb *dpb)
{
	unsigned index;

	/* Inactive logical entries do not depend on physical slot activation. */
	for (index = 0U; index < H264_DPB_SLOTS; index++) {
		if (dpb->entry[index].reference == H264_DPB_UNUSED)
			return (int)index;
	}

	/* A full logical array violates the bounded reference capacity. */
	return -1;
}

/* Make room for a reference using the oldest short-term frame-number wrap. */
static int
h264_sliding(
	struct h264_dpb *dpb,
	uint32_t current,
	uint32_t maximum)
{
	unsigned index;
	unsigned count;
	int oldest;
	int32_t number;
	int32_t minimum;

	/* Count all logical references, including frames without a physical image. */
	count = 0U;
	oldest = -1;
	minimum = INT32_MAX;
	for (index = 0U; index < H264_DPB_SLOTS; index++) {
		if (dpb->entry[index].reference == H264_DPB_UNUSED)
			continue;
		count++;
		if (dpb->entry[index].reference != H264_DPB_SHORT)
			continue;
		number = h264_pic_num(&dpb->entry[index], current, maximum);
		if (number < minimum) {
			oldest = (int)index;
			minimum = number;
		}
	}
	if (count < dpb->max_references)
		return 0;
	if (oldest < 0)
		return EINVAL;
	dpb->entry[oldest].reference = H264_DPB_UNUSED;

	/* Succeeded: one reference position is available for the new logical picture. */
	return 0;
}

/* Compare two initial-list entries using frame order, PicNum or long-term index. */
static int
h264_list_before(
	const struct h264_dpb *dpb,
	int left,
	int right,
	uint32_t current,
	uint32_t maximum,
	int32_t poc,
	unsigned type,
	unsigned list)
{
	const struct h264_dpb_entry *a;
	const struct h264_dpb_entry *b;
	int32_t ap;
	int32_t bp;
	int a_group;
	int b_group;

	/* Long-term references always follow short-term references, sorted by increasing index. */
	a = &dpb->entry[left];
	b = &dpb->entry[right];
	if (a->reference == H264_DPB_SHORT && b->reference == H264_DPB_LONG)
		return 1;
	if (a->reference == H264_DPB_LONG && b->reference == H264_DPB_SHORT)
		return 0;
	if (a->reference == H264_DPB_LONG) {
		if (a->long_index < b->long_index)
			return 1;
		return 0;
	}
	if (type != H264_SLICE_B) {
		ap = h264_pic_num(a, current, maximum);
		bp = h264_pic_num(b, current, maximum);
		if (ap > bp)
			return 1;
		return 0;
	}

	/* B list zero puts earlier POC first descending; list one puts later POC first ascending. */
	ap = h264_reference_poc(a);
	bp = h264_reference_poc(b);
	a_group = 1;
	b_group = 1;
	if (list == 0U) {
		if (ap < poc)
			a_group = 0;
		if (bp < poc)
			b_group = 0;
	} else {
		if (ap > poc)
			a_group = 0;
		if (bp > poc)
			b_group = 0;
	}
	if (a_group < b_group)
		return 1;
	if (a_group > b_group)
		return 0;
	if ((list == 0U && a_group == 0) || (list == 1U && a_group == 1)) {
		if (ap > bp)
			return 1;
	} else {
		if (ap < bp)
			return 1;
	}

	/* Preserve existing entry order only when the normative ordering key is equal. */
	return 0;
}

/* Build both complete initial lists and apply the identical-list-one swap before any truncation. */
static void
h264_initial_lists(
	const struct h264_dpb *dpb,
	const StdVideoH264SequenceParameterSet *sps,
	const struct h264_picture *picture,
	unsigned type,
	int real_only,
	struct h264_reference_list *lists)
{
	unsigned index;
	unsigned list;
	unsigned cursor;
	unsigned position;
	int moving;
	int before;
	int same;
	int32_t poc;
	uint32_t maximum;
	const struct h264_dpb_entry *entry;

	/* Each list begins with every eligible logical or physical reference. */
	memset(lists, 0, sizeof(*lists) * 2U);
	maximum = 1U << (sps->log2_max_frame_num_minus4 + 4U);
	poc = picture->info.PicOrderCnt[0];
	if (picture->info.PicOrderCnt[1] < poc)
		poc = picture->info.PicOrderCnt[1];
	for (list = 0U; list < 2U; list++) {
		for (index = 0U; index < H264_DPB_SLOTS; index++) {
			entry = &dpb->entry[index];
			if (entry->reference == H264_DPB_UNUSED)
				continue;
			if (real_only && entry->slot < 0)
				continue;
			if (type == H264_SLICE_B && sps->pic_order_cnt_type == STD_VIDEO_H264_POC_TYPE_0 && entry->inferred)
				continue;
			lists[list].entry[lists[list].count] = (int)index;
			lists[list].count++;
		}
		for (cursor = 1U; cursor < lists[list].count; cursor++) {
			moving = lists[list].entry[cursor];
			position = cursor;
			while (position > 0U) {
				before = h264_list_before(dpb, moving, lists[list].entry[position - 1U], picture->info.frame_num, maximum, poc, type, list);
				if (!before)
					break;
				lists[list].entry[position] = lists[list].entry[position - 1U];
				position--;
			}
			lists[list].entry[position] = moving;
		}
	}

	/* For B pictures identical full initial lists swap the first two entries of list one. */
	if (type == H264_SLICE_B && lists[0].count > 1U) {
		same = 1;
		for (index = 0U; index < lists[0].count; index++) {
			if (lists[0].entry[index] != lists[1].entry[index])
				same = 0;
		}
		if (same) {
			moving = lists[1].entry[0];
			lists[1].entry[0] = lists[1].entry[1];
			lists[1].entry[1] = moving;
		}
	}
}

/* Apply explicit modifications without allowing a missing target to silently shift an active reference. */
static int
h264_modify_list(
	const struct h264_dpb *dpb,
	const struct h264_list *syntax,
	uint32_t current,
	uint32_t maximum,
	int real_only,
	struct h264_reference_list *list)
{
	unsigned operation;
	unsigned cursor;
	unsigned write;
	uint32_t prediction;
	uint32_t difference;
	int32_t number;
	int found;

	/* PicNumPred is updated by short-term operations only and wraps in MaxPicNum. */
	prediction = current;
	for (operation = 0U; operation < syntax->count; operation++) {
		if (syntax->operation[operation] < 2U) {
			if (syntax->argument[operation] >= maximum)
				return EINVAL;
			difference = syntax->argument[operation] + 1U;
			if (syntax->operation[operation] == 0U)
				prediction = (prediction + maximum - difference) % maximum;
			else
				prediction = (prediction + difference) % maximum;
			number = (int32_t)prediction;
			if (prediction > current)
				number -= (int32_t)maximum;
			found = h264_find_short(dpb, number, current, maximum);
		} else {
			found = h264_find_long(dpb, syntax->argument[operation]);
		}
		if (found < 0)
			return ENOENT;
		if (real_only && dpb->entry[found].slot < 0)
			return ENOENT;
		if (operation > list->count || list->count >= 63U)
			return ENOENT;
		for (cursor = list->count; cursor > operation; cursor--)
			list->entry[cursor] = list->entry[cursor - 1U];
		list->entry[operation] = found;
		list->count++;
		write = operation + 1U;
		for (cursor = operation + 1U; cursor < list->count; cursor++) {
			if (list->entry[cursor] != found) {
				list->entry[write] = list->entry[cursor];
				write++;
			}
		}
		list->count = write;
	}

	/* Succeeded: the complete modified list retains the normative insertion and duplicate rules. */
	return 0;
}

/* Verify every slice's active prefix against the reference state a standard decoder will reconstruct. */
static int
h264_lists_admit(
	const struct h264_dpb *dpb,
	const StdVideoH264SequenceParameterSet *sps,
	const struct h264_picture *picture)
{
	struct h264_reference_list logical[2];
	struct h264_reference_list real[2];
	const struct h264_slice *slice;
	unsigned index;
	unsigned list;
	unsigned active;
	unsigned reference;
	uint32_t maximum;
	int error;

	/* Independently construct and modify both logical and actual-image lists for each slice. */
	maximum = 1U << (sps->log2_max_frame_num_minus4 + 4U);
	for (index = 0U; index < picture->slice_count; index++) {
		slice = &picture->slices[index];
		h264_initial_lists(dpb, sps, picture, slice->type, 0, logical);
		h264_initial_lists(dpb, sps, picture, slice->type, 1, real);
		for (list = 0U; list < 2U; list++) {
			active = slice->list[list].active;
			if (active == 0U)
				continue;
			error = h264_modify_list(dpb, &slice->list[list], picture->info.frame_num, maximum, 0, &logical[list]);
			if (error != 0)
				return error;
			error = h264_modify_list(dpb, &slice->list[list], picture->info.frame_num, maximum, 1, &real[list]);
			if (error != 0)
				return error;
			if (active > logical[list].count || active > real[list].count)
				return ENOENT;
			for (reference = 0U; reference < active; reference++) {
				if (logical[list].entry[reference] != real[list].entry[reference])
					return ENOENT;
			}
		}
	}

	/* Succeeded: every active reference index selects the intended available decoded image. */
	return 0;
}

/* Fill the progressive standard reference descriptor from a real logical reference. */
static void
h264_reference_info(
	const struct h264_dpb_entry *entry,
	StdVideoDecodeH264ReferenceInfo *info)
{
	/* Long-term references carry their persistent index instead of modulo frame number. */
	memset(info, 0, sizeof(*info));
	info->FrameNum = (uint16_t)entry->frame_num;
	if (entry->reference == H264_DPB_LONG) {
		info->flags.used_for_long_term_reference = 1;
		info->FrameNum = (uint16_t)entry->long_index;
	}
	info->PicOrderCnt[0] = entry->poc[0];
	info->PicOrderCnt[1] = entry->poc[1];
}
