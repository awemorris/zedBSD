/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoded picture buffer of the software H.264 decoder: the marking
 * (8.2.5) and the output order (C.4).
 */

#include "h264sw-dpb.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/* The largest value MaxLongTermFrameIdx is kept at (it is an int32_t; the syntax allows more). */
#define DPB_LONG_IDX_LIMIT 2147483647U

/* Static function declarations. */
static unsigned dpb_limit(const struct h264sw_dpb *dpb);
static int dpb_queue_push(struct h264sw_output_queue *queue, struct h264sw_frame *frame);
static void dpb_entry_empty(struct h264sw_dpb_entry *entry);
static void dpb_sweep(struct h264sw_dpb *dpb);
static int dpb_find_empty(const struct h264sw_dpb *dpb);
static unsigned dpb_count_output(const struct h264sw_dpb *dpb);
static int64_t dpb_frame_num_wrap(uint32_t entry_frame_num, uint32_t frame_num, uint32_t max_frame_num);
static int dpb_oldest_short(const struct h264sw_dpb *dpb, uint32_t frame_num);
static int dpb_smallest_long(const struct h264sw_dpb *dpb);
static int dpb_find_short(const struct h264sw_dpb *dpb, int64_t pic_num, uint32_t frame_num, int existing_only);
static int dpb_find_long(const struct h264sw_dpb *dpb, uint32_t long_term_pic_num);
static void dpb_sliding_window(struct h264sw_dpb *dpb, uint32_t frame_num);
static void dpb_mark_all_unused(struct h264sw_dpb *dpb);
static void dpb_unassign_long(struct h264sw_dpb *dpb, uint32_t long_term_frame_idx);
static void dpb_max_long_idx(struct h264sw_dpb *dpb, uint32_t plus1);
static void dpb_mmco_short_unused(struct h264sw_dpb *dpb, uint32_t curr_pic_num, uint32_t difference_minus1);
static void dpb_mmco_short_to_long(struct h264sw_dpb *dpb, uint32_t curr_pic_num, uint32_t difference_minus1, uint32_t long_term_frame_idx);
static int dpb_marking_adaptive(struct h264sw_dpb *dpb, const struct h264sw_slice_header *header, unsigned *current_marking, uint32_t *current_long_idx);
static int dpb_output_all(struct h264sw_dpb *dpb, struct h264sw_output_queue *queue);
static int dpb_is_lowest(const struct h264sw_dpb *dpb, int32_t poc);
static int32_t dpb_last_stored_poc(const struct h264sw_dpb *dpb);
static void dpb_gap_order_count(const struct h264sw_sps *sps, const struct h264sw_slice_header *header, struct h264sw_poc_state *state, uint32_t frame_num, int32_t last_poc, int32_t *top, int32_t *bottom);

/*
 * Empties a queue (its frames must have been taken or released before).
 *
 * The queue then holds nothing and the next frame goes to its first place.
 */
void
h264sw_output_init(
	struct h264sw_output_queue *queue)
{
	unsigned index;

	/* A queue that is not there needs no emptying. */
	if (queue == NULL)
		return;

	/* Forgets every place so no stale frame pointer is left behind. */
	for (index = 0U;
	     index < H264SW_OUTPUT_MAX;
	     index++)
		queue->frame[index] = NULL;

	/* Starts the ring at its first place with no frame in it. */
	queue->head = 0U;
	queue->count = 0U;
}

/*
 * Takes the oldest frame of the queue with its reference (the caller unrefs it), or NULL when empty.
 *
 * The frames come out in the order the bumping process output them.
 */
struct h264sw_frame *
h264sw_output_take(
	struct h264sw_output_queue *queue)
{
	struct h264sw_frame *frame;
	unsigned head;

	/* A queue that is not there holds nothing, and neither does an empty one. */
	if (queue == NULL)
		return NULL;
	if (queue->count == 0U)
		return NULL;

	/* Takes the oldest frame; the queue's reference goes to the caller. */
	head = queue->head % H264SW_OUTPUT_MAX;
	frame = queue->frame[head];
	queue->frame[head] = NULL;

	/* Moves the ring on to the next oldest frame. */
	queue->head = (head + 1U) % H264SW_OUTPUT_MAX;
	queue->count--;

	/* Succeeded: the caller owns the frame's reference. */
	return frame;
}

/*
 * Drops every frame of the queue (unref), for a seek or a close.
 *
 * The queue is empty afterwards.
 */
void
h264sw_output_clear(
	struct h264sw_output_queue *queue)
{
	struct h264sw_frame *frame;
	unsigned remaining;

	/* A queue that is not there holds nothing. */
	if (queue == NULL)
		return;

	/* Releases the queue's reference of every frame, oldest first. */
	remaining = queue->count;
	while (remaining > 0U) {
		frame = h264sw_output_take(queue);
		h264sw_frame_unref(frame);
		remaining--;
	}

	/* Starts the ring over at its first place. */
	h264sw_output_init(queue);
}

/*
 * Prepares an empty DPB for a sequence.
 *
 * The number of buffers and the reorder depth are kept inside the range
 * the entry array and the specification allow, whatever the SPS says.
 */
void
h264sw_dpb_init(
	struct h264sw_dpb *dpb,
	const struct h264sw_sps *sps)
{
	unsigned size;
	unsigned reorder;
	unsigned index;

	/* Nothing can be prepared without both. */
	if (dpb == NULL)
		return;
	if (sps == NULL)
		return;

	/* The buffers are 1..16 (A.3.1 h). */
	size = sps->dpb_frames;
	if (size < 1U)
		size = 1U;
	if (size > H264SW_MAX_DPB_FRAMES)
		size = H264SW_MAX_DPB_FRAMES;

	/* The VUI's reorder depth when it is sent, else as many as the buffers; never more than the buffers. */
	reorder = size;
	if (sps->restriction_present)
		reorder = sps->max_num_reorder_frames;
	if (reorder > size)
		reorder = size;

	/* Records the sequence's parameters. */
	dpb->size = size;
	dpb->max_num_ref_frames = sps->max_num_ref_frames;
	dpb->reorder = reorder;
	dpb->max_long_term_frame_idx = -1;
	dpb->max_frame_num = sps->max_frame_num;

	/* Every buffer starts empty. */
	for (index = 0U;
	     index < H264SW_MAX_DPB_FRAMES;
	     index++)
		memset(&dpb->entry[index], 0, sizeof(dpb->entry[index]));
}

/*
 * Drops every entry without output (unref), for a seek, a close, or an IDR picture with no_output_of_prior_pics_flag (C.4.4 step 3).
 *
 * MaxLongTermFrameIdx goes back to "no long-term frame indices", as for a DPB that has never held a reference.
 */
void
h264sw_dpb_clear(
	struct h264sw_dpb *dpb)
{
	unsigned index;

	/* A DPB that is not there holds nothing. */
	if (dpb == NULL)
		return;

	/* Releases the DPB's reference of every frame, whatever its marking. */
	for (index = 0U;
	     index < H264SW_MAX_DPB_FRAMES;
	     index++)
		dpb_entry_empty(&dpb->entry[index]);

	/* No long-term frame is left to need an index. */
	dpb->max_long_term_frame_idx = -1;
}

/*
 * Outputs every frame still needed for output and empties the DPB (the bumping process repeated, C.4.5.3), for the end of a stream.
 *
 * When the queue fills, the frames still waiting stay in the DPB, so the caller can empty the queue and call again.
 */
int
h264sw_dpb_flush(
	struct h264sw_dpb *dpb,
	struct h264sw_output_queue *queue)
{
	int error;

	/* Refuses the calls that cannot work. */
	if (dpb == NULL)
		return EINVAL;
	if (queue == NULL)
		return EINVAL;

	/* Outputs the frames in order. */
	error = dpb_output_all(dpb, queue);
	if (error != 0)
		return error;

	/* The references that were not needed for output go too. */
	h264sw_dpb_clear(dpb);

	/* Succeeded: every frame is in the queue and the DPB is empty. */
	return 0;
}

/*
 * Stores the non-existing frames of a gap in frame_num (8.2.5.2, C.4.2).
 *
 * The gap is the run of UnusedShortTermFrameNum values (7-24) between
 * PrevRefFrameNum and frame_num.  Only the last max_num_ref_frames values
 * are made: each non-existing frame goes through the sliding window,
 * which would drop the earlier ones at once.  The order count state
 * moves past each frame made, so the picture that follows continues
 * from the last of them.
 */
int
h264sw_dpb_gaps(
	struct h264sw_dpb *dpb,
	const struct h264sw_sps *sps,
	const struct h264sw_slice_header *header,
	struct h264sw_poc_state *poc,
	struct h264sw_frame_pool *pool,
	struct h264sw_output_queue *queue)
{
	struct h264sw_frame *frame;
	uint32_t max_frame_num;
	uint32_t previous;
	uint32_t first;
	uint32_t count;
	uint32_t keep;
	uint32_t made;
	uint32_t skipped;
	uint32_t step;
	uint32_t number;
	int32_t last_poc;
	int32_t top;
	int32_t bottom;
	int index;
	int error;

	/* Refuses the calls that cannot work. */
	if (dpb == NULL)
		return EINVAL;
	if (sps == NULL)
		return EINVAL;
	if (header == NULL)
		return EINVAL;
	if (poc == NULL)
		return EINVAL;
	if (pool == NULL)
		return EINVAL;
	if (queue == NULL)
		return EINVAL;

	/* An IDR picture starts the frame numbers again; it has no gap. */
	if (header->idr)
		return 0;

	/* Values the modular arithmetic cannot use are not a gap that can be filled. */
	max_frame_num = dpb->max_frame_num;
	previous = poc->prev_ref_frame_num;
	if (max_frame_num == 0U)
		return 0;
	if (header->frame_num >= max_frame_num)
		return 0;
	if (previous >= max_frame_num)
		return 0;

	/* The same frame_num as the last reference picture is no gap (7.4.3). */
	if (header->frame_num == previous)
		return 0;

	/* The next frame_num after the last reference picture is no gap (7.4.3). */
	first = (previous + 1U) % max_frame_num;
	if (header->frame_num == first)
		return 0;

	/* Counts the values UnusedShortTermFrameNum takes before frame_num (7-24); at least one here. */
	count = (header->frame_num + max_frame_num - first) % max_frame_num;

	/* Keeps the last max_num_ref_frames values: the window leaves no more of them. */
	keep = dpb->max_num_ref_frames;
	if (keep < 1U)
		keep = 1U;
	made = count;
	if (made > keep)
		made = keep;
	skipped = count - made;

	/* Makes the non-existing frames in the order the values are generated. */
	for (step = 0U;
	     step < made;
	     step++) {
		/* The frame_num this non-existing frame stands for. */
		number = (first + skipped + step) % max_frame_num;

		/* The order count of a type 0 sequence follows the frame stored last; read before the window can drop it. */
		last_poc = dpb_last_stored_poc(dpb);

		/* Marks the oldest short-term frames unused so the new frame fits the window (8.2.5.3). */
		dpb_sliding_window(dpb, number);

		/* Takes a frame for the non-existing picture. */
		frame = h264sw_frame_get(pool);
		if (frame == NULL)
			return ENOMEM;

		/* Empties a buffer for it (C.4.2), bumping while the DPB is full. */
		error = h264sw_dpb_make_room(dpb, number, queue);
		if (error != 0) {
			h264sw_frame_unref(frame);
			return error;
		}
		index = dpb_find_empty(dpb);
		if (index < 0) {
			h264sw_frame_unref(frame);
			return ENOBUFS;
		}

		/* Derives the order counts and moves the state past this frame; nothing can fail after this point, so the state moves only for a frame that is stored. */
		dpb_gap_order_count(sps, header, poc, number, last_poc, &top, &bottom);

		/* Describes the frame: it stands for frame_num, shows grey, and is never output. */
		h264sw_frame_fill_grey(frame);
		frame->frame_num = number;
		frame->top_poc = top;
		frame->bottom_poc = bottom;
		frame->poc = top;
		if (bottom < top)
			frame->poc = bottom;
		frame->idr = 0;
		frame->non_existing = 1;

		/* Stores it as a short-term reference that is not needed for output; the DPB keeps the reference taken above. */
		dpb->entry[index].frame = frame;
		dpb->entry[index].reference = H264SW_REF_SHORT;
		dpb->entry[index].long_term_frame_idx = 0U;
		dpb->entry[index].output_needed = 0;
		dpb->entry[index].frame_num = number;
	}

	/* Succeeded: the DPB holds the non-existing frames of the gap. */
	return 0;
}

/*
 * Marks the references and removes frames before the current picture is stored (C.4.4, 8.2.5).
 *
 * The marking uses the frame_num of the slice header as CurrPicNum
 * (8.2.4.1), so the caller may already have set the frame's own frame_num
 * to 0 for a picture with operation 5.
 */
int
h264sw_dpb_remove(
	struct h264sw_dpb *dpb,
	const struct h264sw_sps *sps,
	const struct h264sw_slice_header *header,
	const struct h264sw_frame *current,
	int changed,
	unsigned *current_marking,
	uint32_t *current_long_idx,
	struct h264sw_output_queue *queue)
{
	int output_all;
	int error;

	UNUSED_PARAMETER(sps);
	UNUSED_PARAMETER(current);
	UNUSED_PARAMETER(changed);

	/* Refuses the calls that cannot work. */
	if (dpb == NULL)
		return EINVAL;
	if (header == NULL)
		return EINVAL;
	if (current_marking == NULL)
		return EINVAL;
	if (current_long_idx == NULL)
		return EINVAL;
	if (queue == NULL)
		return EINVAL;

	/* A non-reference picture stays unmarked until the marking below says otherwise. */
	*current_marking = H264SW_REF_UNUSED;
	*current_long_idx = 0U;
	output_all = 0;

	/* An IDR picture empties the references (8.2.5.1); the other reference pictures run the marking process. */
	if (header->idr) {
		/* Marks every reference picture unused (8.2.5.1 step 2a). */
		dpb_mark_all_unused(dpb);

		/* Drops the old frames unseen, or keeps them to be output below (C.4.4 steps 2 and 3, NOTE 3). */
		output_all = 1;
		if (header->no_output_of_prior_pics) {
			h264sw_dpb_clear(dpb);
			output_all = 0;
		}

		/* Marks the IDR picture itself (8.2.5.1 step 2b). */
		dpb->max_long_term_frame_idx = -1;
		if (header->nal_ref_idc != 0U) {
			*current_marking = H264SW_REF_SHORT;
			if (header->long_term_reference) {
				*current_marking = H264SW_REF_LONG;
				dpb->max_long_term_frame_idx = 0;
			}
		}
	} else if (header->nal_ref_idc != 0U && header->adaptive_marking) {
		/* The adaptive memory control operations, in the order they were sent (8.2.5.4). */
		output_all = dpb_marking_adaptive(dpb, header, current_marking, current_long_idx);
	} else if (header->nal_ref_idc != 0U) {
		/* The sliding window, then the picture is a short-term reference (8.2.5.3, 8.2.5.1 step 3). */
		dpb_sliding_window(dpb, header->frame_num);
		*current_marking = H264SW_REF_SHORT;
	}

	/* Empties the frames neither needed for output nor used for reference (C.4.4). */
	dpb_sweep(dpb);

	/* An IDR picture without the flag, or operation 5, outputs every frame left (C.4.4). */
	if (output_all) {
		error = dpb_output_all(dpb, queue);
		if (error != 0)
			return error;
	}

	/* Succeeded: the DPB is ready to take the current picture. */
	return 0;
}

/*
 * Stores the current picture (C.4.5).
 *
 * The DPB takes its own reference to the frame whenever it keeps it; the
 * caller's reference stays the caller's.
 */
int
h264sw_dpb_store(
	struct h264sw_dpb *dpb,
	struct h264sw_frame *current,
	unsigned marking,
	uint32_t long_idx,
	int output,
	struct h264sw_output_queue *queue)
{
	unsigned pass;
	unsigned limit;
	int index;
	int error;
	int bumped;
	int done;

	/* Refuses the calls that cannot work. */
	if (dpb == NULL)
		return EINVAL;
	if (current == NULL)
		return EINVAL;
	if (queue == NULL)
		return EINVAL;
	if (marking > H264SW_REF_LONG)
		return EINVAL;

	/* The passes of every loop below are bounded by the buffers. */
	limit = dpb_limit(dpb);

	/* A reference picture takes a buffer, bumping for it (C.4.5.1); a non-reference picture is stored or output (C.4.5.2). */
	if (marking != H264SW_REF_UNUSED) {
		/* Empties a buffer for the picture. */
		error = h264sw_dpb_make_room(dpb, current->frame_num, queue);
		if (error != 0)
			return error;
		index = dpb_find_empty(dpb);
		if (index < 0)
			return ENOBUFS;

		/* Stores it with its marking, needed for output unless it must not be shown. */
		h264sw_frame_ref(current);
		dpb->entry[index].frame = current;
		dpb->entry[index].reference = marking;
		dpb->entry[index].long_term_frame_idx = long_idx;
		dpb->entry[index].output_needed = 0;
		if (output)
			dpb->entry[index].output_needed = 1;
		dpb->entry[index].frame_num = current->frame_num;
	} else if (output) {
		/* Stores the picture in a free buffer, or outputs it at once when it comes first, or bumps and tries again (C.4.5.2). */
		done = 0;
		for (pass = 0U;
		     pass <= limit + 1U && !done;
		     pass++) {
			/* Empties the buffers that hold nothing needed. */
			dpb_sweep(dpb);

			/* Chooses between a free buffer, output at once, and bumping. */
			index = dpb_find_empty(dpb);
			if (index >= 0) {
				/* A buffer is free: the picture waits there for output. */
				h264sw_frame_ref(current);
				dpb->entry[index].frame = current;
				dpb->entry[index].reference = H264SW_REF_UNUSED;
				dpb->entry[index].long_term_frame_idx = 0U;
				dpb->entry[index].output_needed = 1;
				dpb->entry[index].frame_num = current->frame_num;
				done = 1;
			} else if (dpb_is_lowest(dpb, current->poc)) {
				/* The picture precedes every waiting frame in output order: it is output without being stored. */
				error = dpb_queue_push(queue, current);
				if (error != 0)
					return ENOBUFS;
				done = 1;
			} else {
				/* A frame waiting before the picture is output first, which may free a buffer. */
				bumped = h264sw_dpb_bump(dpb, queue);
				if (bumped <= 0)
					return ENOBUFS;
			}
		}

		/* The loop ends in a decision on every path; reaching its bound would be a broken DPB. */
		if (!done)
			return ENOBUFS;
	}

	/* Outputs frames while more wait than the stream's reorder depth allows. */
	for (pass = 0U;
	     pass <= limit && dpb_count_output(dpb) > dpb->reorder;
	     pass++) {
		/* Runs the bumping process once. */
		bumped = h264sw_dpb_bump(dpb, queue);
		if (bumped < 0)
			return ENOBUFS;
		if (bumped == 0)
			break;
	}

	/* Succeeded: the picture is stored or output. */
	return 0;
}

/*
 * Runs the bumping process once (C.4.5.3).
 *
 * Frames with equal PicOrderCnt (a broken stream) go out in the order of their buffers.
 */
int
h264sw_dpb_bump(
	struct h264sw_dpb *dpb,
	struct h264sw_output_queue *queue)
{
	unsigned index;
	unsigned limit;
	int best;
	int error;

	/* Refuses the calls that cannot work. */
	if (dpb == NULL)
		return -EINVAL;
	if (queue == NULL)
		return -EINVAL;

	/* Selects the frame needed for output with the smallest PicOrderCnt (C.4.5.3 step 1). */
	best = -1;
	limit = dpb_limit(dpb);
	for (index = 0U;
	     index < limit;
	     index++) {
		if (dpb->entry[index].frame == NULL)
			continue;
		if (!dpb->entry[index].output_needed)
			continue;
		if (best < 0 ||
		    dpb->entry[index].frame->poc < dpb->entry[best].frame->poc)
			best = (int)index;
	}

	/* No frame is needed for output. */
	if (best < 0)
		return 0;

	/* Outputs the frame (step 2); the queue takes its own reference. */
	error = dpb_queue_push(queue, dpb->entry[best].frame);
	if (error != 0)
		return -ENOBUFS;
	dpb->entry[best].output_needed = 0;

	/* Empties the buffer when the frame is not used for reference either (step 4). */
	if (dpb->entry[best].reference == H264SW_REF_UNUSED)
		dpb_entry_empty(&dpb->entry[best]);

	/* Succeeded: one frame was output. */
	return 1;
}

/*
 * Empties a frame buffer for a frame to be stored (C.4.2, C.4.5.1, C.4.5.2).
 *
 * Each pass outputs a frame or frees a reference, and there are at most
 * as many frames as buffers, so a few passes more than the buffers bound
 * the loop.
 */
int
h264sw_dpb_make_room(
	struct h264sw_dpb *dpb,
	uint32_t frame_num,
	struct h264sw_output_queue *queue)
{
	unsigned pass;
	unsigned limit;
	int index;
	int bumped;

	/* Refuses the calls that cannot work. */
	if (dpb == NULL)
		return EINVAL;
	if (queue == NULL)
		return EINVAL;

	/* Frees buffers until one is empty. */
	limit = dpb_limit(dpb);
	for (pass = 0U;
	     pass <= 2U * limit + 1U;
	     pass++) {
		/* Empties the buffers that hold nothing needed. */
		dpb_sweep(dpb);

		/* A buffer is free: the loop is done. */
		index = dpb_find_empty(dpb);
		if (index >= 0)
			return 0;

		/* Outputs the next frame in output order, which frees its buffer when it is no reference. */
		bumped = h264sw_dpb_bump(dpb, queue);
		if (bumped < 0)
			return ENOBUFS;
		if (bumped > 0)
			continue;

		/* Nothing is waiting for output: a stream that keeps more references than the DPB holds loses its oldest short-term one, else its smallest long-term one. */
		index = dpb_oldest_short(dpb, frame_num);
		if (index < 0)
			index = dpb_smallest_long(dpb);
		if (index < 0)
			break;
		dpb_entry_empty(&dpb->entry[index]);
	}

	/* The buffers could not be freed: the DPB has no buffer or the passes ran out. */
	return ENOBUFS;
}

/*
 * Counts the entries marked as used for reference (short and long).
 *
 * An empty buffer counts for nothing, whatever its marking field says.
 */
unsigned
h264sw_dpb_references(
	const struct h264sw_dpb *dpb)
{
	unsigned index;
	unsigned limit;
	unsigned count;

	/* A DPB that is not there holds no reference. */
	if (dpb == NULL)
		return 0U;

	/* Counts the frames marked short-term or long-term. */
	count = 0U;
	limit = dpb_limit(dpb);
	for (index = 0U;
	     index < limit;
	     index++) {
		if (dpb->entry[index].frame == NULL)
			continue;
		if (dpb->entry[index].reference == H264SW_REF_UNUSED)
			continue;
		count++;
	}

	/* Succeeded: the number of references. */
	return count;
}

/*
 * Gives the number of buffers in use, never more than the entry array holds.
 */
static unsigned
dpb_limit(
	const struct h264sw_dpb *dpb)
{
	/* A DPB the decoder sized wrongly still stays inside its array. */
	if (dpb->size > H264SW_MAX_DPB_FRAMES)
		return H264SW_MAX_DPB_FRAMES;

	/* Succeeded: the size is within the array. */
	return dpb->size;
}

/*
 * Appends a frame to the queue with a new reference, or reports that the queue is full.
 */
static int
dpb_queue_push(
	struct h264sw_output_queue *queue,
	struct h264sw_frame *frame)
{
	unsigned place;

	/* A full queue takes no more; the caller reports it. */
	if (queue->count >= H264SW_OUTPUT_MAX)
		return ENOBUFS;

	/* Puts the frame after the newest one, with the reference the queue holds. */
	place = ((queue->head % H264SW_OUTPUT_MAX) + queue->count) % H264SW_OUTPUT_MAX;
	h264sw_frame_ref(frame);
	queue->frame[place] = frame;
	queue->count++;

	/* Succeeded: the frame is queued. */
	return 0;
}

/*
 * Releases the frame of a buffer and makes the buffer empty.
 */
static void
dpb_entry_empty(
	struct h264sw_dpb_entry *entry)
{
	/* Drops the DPB's reference; a buffer with no frame has none. */
	h264sw_frame_unref(entry->frame);

	/* A buffer holding nothing has no marking and is not waiting for output. */
	entry->frame = NULL;
	entry->reference = H264SW_REF_UNUSED;
	entry->long_term_frame_idx = 0U;
	entry->output_needed = 0;
	entry->frame_num = 0U;
}

/*
 * Empties the buffers whose frame is neither needed for output nor used for reference.
 */
static void
dpb_sweep(
	struct h264sw_dpb *dpb)
{
	unsigned index;
	unsigned limit;

	/* Looks at every buffer. */
	limit = dpb_limit(dpb);
	for (index = 0U;
	     index < limit;
	     index++) {
		if (dpb->entry[index].frame == NULL)
			continue;
		if (dpb->entry[index].reference != H264SW_REF_UNUSED)
			continue;
		if (dpb->entry[index].output_needed)
			continue;
		dpb_entry_empty(&dpb->entry[index]);
	}
}

/*
 * Finds the first empty buffer, or gives -1 when every buffer holds a frame.
 */
static int
dpb_find_empty(
	const struct h264sw_dpb *dpb)
{
	unsigned index;
	unsigned limit;

	/* Looks for a buffer with no frame. */
	limit = dpb_limit(dpb);
	for (index = 0U;
	     index < limit;
	     index++) {
		if (dpb->entry[index].frame == NULL)
			return (int)index;
	}

	/* Every buffer is in use. */
	return -1;
}

/*
 * Counts the frames marked as needed for output.
 */
static unsigned
dpb_count_output(
	const struct h264sw_dpb *dpb)
{
	unsigned index;
	unsigned limit;
	unsigned count;

	/* Counts the frames that wait for output. */
	count = 0U;
	limit = dpb_limit(dpb);
	for (index = 0U;
	     index < limit;
	     index++) {
		if (dpb->entry[index].frame == NULL)
			continue;
		if (!dpb->entry[index].output_needed)
			continue;
		count++;
	}

	/* Succeeded: the number of waiting frames. */
	return count;
}

/*
 * Derives FrameNumWrap of a short-term frame against the current frame_num (8-27).
 */
static int64_t
dpb_frame_num_wrap(
	uint32_t entry_frame_num,
	uint32_t frame_num,
	uint32_t max_frame_num)
{
	/* A frame_num above the current one belongs to the previous cycle of frame_num. */
	if (entry_frame_num > frame_num)
		return (int64_t)entry_frame_num - (int64_t)max_frame_num;

	/* Succeeded: in the current cycle FrameNumWrap is FrameNum. */
	return (int64_t)entry_frame_num;
}

/*
 * Finds the short-term reference with the smallest FrameNumWrap, or gives -1 when there is none.
 */
static int
dpb_oldest_short(
	const struct h264sw_dpb *dpb,
	uint32_t frame_num)
{
	unsigned index;
	unsigned limit;
	int64_t wrap;
	int64_t best_wrap;
	int best;

	/* Keeps the smallest value seen; a tie keeps the first buffer. */
	best = -1;
	best_wrap = 0;
	limit = dpb_limit(dpb);
	for (index = 0U;
	     index < limit;
	     index++) {
		if (dpb->entry[index].frame == NULL)
			continue;
		if (dpb->entry[index].reference != H264SW_REF_SHORT)
			continue;
		wrap = dpb_frame_num_wrap(dpb->entry[index].frame_num, frame_num, dpb->max_frame_num);
		if (best < 0 ||
		    wrap < best_wrap) {
			best = (int)index;
			best_wrap = wrap;
		}
	}

	/* Succeeded: the oldest short-term reference, or -1. */
	return best;
}

/*
 * Finds the long-term reference with the smallest LongTermFrameIdx, or gives -1 when there is none.
 */
static int
dpb_smallest_long(
	const struct h264sw_dpb *dpb)
{
	unsigned index;
	unsigned limit;
	int best;

	/* Keeps the smallest index seen; a tie keeps the first buffer. */
	best = -1;
	limit = dpb_limit(dpb);
	for (index = 0U;
	     index < limit;
	     index++) {
		if (dpb->entry[index].frame == NULL)
			continue;
		if (dpb->entry[index].reference != H264SW_REF_LONG)
			continue;
		if (best < 0 ||
		    dpb->entry[index].long_term_frame_idx < dpb->entry[best].long_term_frame_idx)
			best = (int)index;
	}

	/* Succeeded: the long-term reference with the smallest index, or -1. */
	return best;
}

/*
 * Finds the short-term reference whose PicNum is pic_num (8-28), or gives -1.
 *
 * A non-existing frame is skipped when existing_only is set.
 */
static int
dpb_find_short(
	const struct h264sw_dpb *dpb,
	int64_t pic_num,
	uint32_t frame_num,
	int existing_only)
{
	unsigned index;
	unsigned limit;
	int64_t wrap;

	/* PicNum of a frame is its FrameNumWrap. */
	limit = dpb_limit(dpb);
	for (index = 0U;
	     index < limit;
	     index++) {
		if (dpb->entry[index].frame == NULL)
			continue;
		if (dpb->entry[index].reference != H264SW_REF_SHORT)
			continue;
		if (existing_only && dpb->entry[index].frame->non_existing)
			continue;
		wrap = dpb_frame_num_wrap(dpb->entry[index].frame_num, frame_num, dpb->max_frame_num);
		if (wrap == pic_num)
			return (int)index;
	}

	/* No short-term reference has that PicNum. */
	return -1;
}

/*
 * Finds the long-term reference whose LongTermPicNum is long_term_pic_num (8-29), or gives -1.
 */
static int
dpb_find_long(
	const struct h264sw_dpb *dpb,
	uint32_t long_term_pic_num)
{
	unsigned index;
	unsigned limit;

	/* LongTermPicNum of a frame is its LongTermFrameIdx. */
	limit = dpb_limit(dpb);
	for (index = 0U;
	     index < limit;
	     index++) {
		if (dpb->entry[index].frame == NULL)
			continue;
		if (dpb->entry[index].reference != H264SW_REF_LONG)
			continue;
		if (dpb->entry[index].long_term_frame_idx == long_term_pic_num)
			return (int)index;
	}

	/* No long-term reference has that number. */
	return -1;
}

/*
 * Marks the oldest short-term frames unused while the references fill the window (8.2.5.3).
 *
 * The window holds Max( max_num_ref_frames, 1 ) frames.  A stream that
 * keeps more than the window (a broken one) is brought back to it.
 */
static void
dpb_sliding_window(
	struct h264sw_dpb *dpb,
	uint32_t frame_num)
{
	unsigned window;
	unsigned count;
	int index;

	/* A window holds at least one frame. */
	window = dpb->max_num_ref_frames;
	if (window < 1U)
		window = 1U;

	/* Drops the frame with the smallest FrameNumWrap until the current picture fits. */
	count = h264sw_dpb_references(dpb);
	while (count >= window) {
		index = dpb_oldest_short(dpb, frame_num);
		if (index < 0)
			break;
		dpb->entry[index].reference = H264SW_REF_UNUSED;
		count = h264sw_dpb_references(dpb);
	}
}

/*
 * Marks every reference picture unused (8.2.5.1 step 2a, 8.2.5.4.5).
 */
static void
dpb_mark_all_unused(
	struct h264sw_dpb *dpb)
{
	unsigned index;
	unsigned limit;

	/* Short-term and long-term alike. */
	limit = dpb_limit(dpb);
	for (index = 0U;
	     index < limit;
	     index++)
		dpb->entry[index].reference = H264SW_REF_UNUSED;
}

/*
 * Marks the long-term frames that hold a LongTermFrameIdx unused, so the index can be given to another picture (8.2.5.4.3, 8.2.5.4.6).
 */
static void
dpb_unassign_long(
	struct h264sw_dpb *dpb,
	uint32_t long_term_frame_idx)
{
	unsigned index;
	unsigned limit;

	/* Looks at every long-term frame. */
	limit = dpb_limit(dpb);
	for (index = 0U;
	     index < limit;
	     index++) {
		if (dpb->entry[index].frame == NULL)
			continue;
		if (dpb->entry[index].reference != H264SW_REF_LONG)
			continue;
		if (dpb->entry[index].long_term_frame_idx != long_term_frame_idx)
			continue;
		dpb->entry[index].reference = H264SW_REF_UNUSED;
	}
}

/*
 * Applies operation 4: drops the long-term frames above the new maximum and records it (8.2.5.4.4).
 */
static void
dpb_max_long_idx(
	struct h264sw_dpb *dpb,
	uint32_t plus1)
{
	unsigned index;
	unsigned limit;

	/* A LongTermFrameIdx greater than max_long_term_frame_idx_plus1 - 1 is one not below plus1. */
	limit = dpb_limit(dpb);
	for (index = 0U;
	     index < limit;
	     index++) {
		if (dpb->entry[index].frame == NULL)
			continue;
		if (dpb->entry[index].reference != H264SW_REF_LONG)
			continue;
		if (dpb->entry[index].long_term_frame_idx < plus1)
			continue;
		dpb->entry[index].reference = H264SW_REF_UNUSED;
	}

	/* 0 means "no long-term frame indices"; a larger value is kept inside int32_t. */
	if (plus1 == 0U) {
		dpb->max_long_term_frame_idx = -1;
	} else if (plus1 - 1U > DPB_LONG_IDX_LIMIT) {
		dpb->max_long_term_frame_idx = (int32_t)DPB_LONG_IDX_LIMIT;
	} else {
		dpb->max_long_term_frame_idx = (int32_t)(plus1 - 1U);
	}
}

/*
 * Applies operation 1: marks the short-term frame picNumX names unused (8-39, 8.2.5.4.1).
 *
 * A picNumX that names no frame leaves the DPB as it is.
 */
static void
dpb_mmco_short_unused(
	struct h264sw_dpb *dpb,
	uint32_t curr_pic_num,
	uint32_t difference_minus1)
{
	int64_t pic_num_x;
	int index;

	/* picNumX = CurrPicNum - ( difference_of_pic_nums_minus1 + 1 ), without overflow. */
	pic_num_x = (int64_t)curr_pic_num - ((int64_t)difference_minus1 + 1);

	/* Marks the frame it names, a non-existing one included (8.2.5.2 NOTE 4). */
	index = dpb_find_short(dpb, pic_num_x, curr_pic_num, 0);
	if (index < 0)
		return;
	dpb->entry[index].reference = H264SW_REF_UNUSED;
}

/*
 * Applies operation 3: gives the short-term frame picNumX names a LongTermFrameIdx (8.2.5.4.3).
 *
 * A picNumX that names no existing short-term frame leaves the DPB as
 * it is: the stream may not name a non-existing frame here.
 */
static void
dpb_mmco_short_to_long(
	struct h264sw_dpb *dpb,
	uint32_t curr_pic_num,
	uint32_t difference_minus1,
	uint32_t long_term_frame_idx)
{
	int64_t pic_num_x;
	int index;

	/* picNumX = CurrPicNum - ( difference_of_pic_nums_minus1 + 1 ), without overflow. */
	pic_num_x = (int64_t)curr_pic_num - ((int64_t)difference_minus1 + 1);

	/* Finds the frame; one that is absent or non-existing is not converted. */
	index = dpb_find_short(dpb, pic_num_x, curr_pic_num, 1);
	if (index < 0)
		return;

	/* A frame that already holds the index gives it up. */
	dpb_unassign_long(dpb, long_term_frame_idx);

	/* The frame becomes a long-term reference with the index. */
	dpb->entry[index].reference = H264SW_REF_LONG;
	dpb->entry[index].long_term_frame_idx = long_term_frame_idx;
}

/*
 * Runs the adaptive memory control operations of a picture in the order sent (8.2.5.4).
 *
 * *current_marking becomes short-term unless operation 6 marked the
 * picture long-term (8.2.5.1 step 3); an operation 5 or 4 that removes
 * what operation 6 gave the picture takes that back.  An operation that
 * names no picture is skipped.  Returns 1 when the picture had operation
 * 5 (every frame is then output), else 0.
 */
static int
dpb_marking_adaptive(
	struct h264sw_dpb *dpb,
	const struct h264sw_slice_header *header,
	unsigned *current_marking,
	uint32_t *current_long_idx)
{
	const struct h264sw_mmco *operation;
	unsigned count;
	unsigned index;
	int found;
	int saw_all_unused;

	/* Without operation 6 the picture is a short-term reference (8.2.5.1 step 3). */
	*current_marking = H264SW_REF_SHORT;
	*current_long_idx = 0U;
	saw_all_unused = 0;

	/* Never reads past the operations the header can hold. */
	count = header->mmco_count;
	if (count > H264SW_MAX_MMCO)
		count = H264SW_MAX_MMCO;

	/* Applies each operation to the DPB. */
	for (index = 0U;
	     index < count;
	     index++) {
		operation = &header->mmco[index];

		/* One case for each operation of Table 7-9; an unknown operation is skipped. */
		switch (operation->operation) {
		case H264SW_MMCO_SHORT_UNUSED:
			dpb_mmco_short_unused(dpb, header->frame_num, operation->difference_of_pic_nums_minus1);
			break;

		case H264SW_MMCO_LONG_UNUSED:
			/* Marks the long-term frame LongTermPicNum names unused (8.2.5.4.2); a number that names none changes nothing. */
			found = dpb_find_long(dpb, operation->long_term_pic_num);
			if (found >= 0)
				dpb->entry[found].reference = H264SW_REF_UNUSED;
			break;

		case H264SW_MMCO_SHORT_TO_LONG:
			dpb_mmco_short_to_long(dpb, header->frame_num, operation->difference_of_pic_nums_minus1, operation->long_term_frame_idx);
			break;

		case H264SW_MMCO_MAX_LONG_INDEX:
			dpb_max_long_idx(dpb, operation->max_long_term_frame_idx_plus1);

			/* The current picture, if operation 6 gave it an index above the new maximum, is no longer long-term. */
			if (*current_marking == H264SW_REF_LONG &&
			    *current_long_idx >= operation->max_long_term_frame_idx_plus1)
				*current_marking = H264SW_REF_SHORT;
			break;

		case H264SW_MMCO_ALL_UNUSED:
			/* Every reference picture is unused and no long-term index is left (8.2.5.4.5). */
			dpb_mark_all_unused(dpb);
			dpb->max_long_term_frame_idx = -1;
			*current_marking = H264SW_REF_SHORT;
			saw_all_unused = 1;
			break;

		case H264SW_MMCO_CURRENT_TO_LONG:
			/* The current picture takes the index; its former holder gives it up (8.2.5.4.6). */
			dpb_unassign_long(dpb, operation->long_term_frame_idx);
			*current_marking = H264SW_REF_LONG;
			*current_long_idx = operation->long_term_frame_idx;
			break;

		default:
			break;
		}
	}

	/* Succeeded: whether operation 5 asks for every frame to be output. */
	return saw_all_unused;
}

/*
 * Outputs every frame needed for output by repeated bumping (C.4.4).
 *
 * Returns 0, or ENOBUFS when the queue fills.
 */
static int
dpb_output_all(
	struct h264sw_dpb *dpb,
	struct h264sw_output_queue *queue)
{
	unsigned pass;
	unsigned limit;
	int bumped;

	/* Each pass outputs one frame, so the buffers bound the passes. */
	limit = dpb_limit(dpb);
	for (pass = 0U;
	     pass <= limit;
	     pass++) {
		/* Runs the bumping process once. */
		bumped = h264sw_dpb_bump(dpb, queue);
		if (bumped < 0)
			return ENOBUFS;
		if (bumped == 0)
			break;
	}

	/* Succeeded: nothing waits for output. */
	return 0;
}

/*
 * Reports whether poc is lower than the PicOrderCnt of every frame needed for output (C.4.5.2); true when none is.
 */
static int
dpb_is_lowest(
	const struct h264sw_dpb *dpb,
	int32_t poc)
{
	unsigned index;
	unsigned limit;

	/* A waiting frame that is not above the picture takes the answer away. */
	limit = dpb_limit(dpb);
	for (index = 0U;
	     index < limit;
	     index++) {
		if (dpb->entry[index].frame == NULL)
			continue;
		if (!dpb->entry[index].output_needed)
			continue;
		if (dpb->entry[index].frame->poc <= poc)
			return 0;
	}

	/* Every waiting frame follows the picture. */
	return 1;
}

/*
 * Gives the PicOrderCnt of the frame stored last, or 0 for an empty DPB.
 *
 * The frames come from one pool whose ids rise with every frame handed
 * out, so the frame with the largest id is the one decoded last.
 */
static int32_t
dpb_last_stored_poc(
	const struct h264sw_dpb *dpb)
{
	unsigned index;
	unsigned limit;
	int best;

	/* Finds the frame with the largest id. */
	best = -1;
	limit = dpb_limit(dpb);
	for (index = 0U;
	     index < limit;
	     index++) {
		if (dpb->entry[index].frame == NULL)
			continue;
		if (best < 0 ||
		    dpb->entry[index].frame->id > dpb->entry[best].frame->id)
			best = (int)index;
	}

	/* An empty DPB has no earlier frame; counting starts at 0. */
	if (best < 0)
		return 0;

	/* Succeeded: the order count of the frame stored last. */
	return dpb->entry[best].frame->poc;
}

/*
 * Derives the order counts of one non-existing frame and moves the state past it (8.2.5.2).
 *
 * Types 1 and 2 derive the counts with a frame that has frame_num, a
 * reference picture, no IDR, no operation 5 and zero delta_pic_order_cnt
 * values, and finish the state as for any reference picture.  Type 0
 * defines none: the frame takes last_poc, and the state keeps
 * PicOrderCntMsb and pic_order_cnt_lsb of the previous reference picture
 * (8.2.1.1 does not count a non-existing frame as one) and moves only
 * PrevRefFrameNum.  A type 1 or 2 derivation that overflows (a broken
 * stream) is treated alike.
 */
static void
dpb_gap_order_count(
	const struct h264sw_sps *sps,
	const struct h264sw_slice_header *header,
	struct h264sw_poc_state *state,
	uint32_t frame_num,
	int32_t last_poc,
	int32_t *top,
	int32_t *bottom)
{
	struct h264sw_slice_header made;
	struct h264sw_poc counts;
	int error;

	/* Type 0 and an unknown type have nothing to derive. */
	memset(&counts, 0, sizeof(counts));
	error = EINVAL;
	if (sps->poc_type == 1U ||
	    sps->poc_type == 2U) {
		/* The header of the non-existing frame, as 8.2.5.2 infers it. */
		made = *header;
		made.frame_num = frame_num;
		made.nal_ref_idc = 1U;
		made.idr = 0;
		made.delta_poc[0] = 0;
		made.delta_poc[1] = 0;
		made.delta_poc_bottom = 0;
		made.poc_lsb = 0U;
		made.has_mmco5 = 0;
		made.adaptive_marking = 0;
		made.mmco_count = 0U;

		/* Derives the counts, and moves the state past the frame when that worked. */
		error = h264sw_poc_compute(state, sps, &made, &counts);
		if (error == 0)
			h264sw_poc_finish(state, sps, &made, &counts);
	}

	/* Derived: takes the counts of the frame. */
	if (error == 0) {
		*top = counts.top;
		*bottom = counts.bottom;
		return;
	}

	/* Not derived: the frame follows the one stored last, and only PrevRefFrameNum moves. */
	*top = last_poc;
	*bottom = last_poc;
	state->prev_ref_frame_num = frame_num;
}
