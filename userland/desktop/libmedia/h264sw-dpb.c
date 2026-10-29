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

/*
 * Empties a queue (its frames must have been taken or released before).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-dpb.h.
 */
void
h264sw_output_init(
	struct h264sw_output_queue *queue)
{
	UNUSED_PARAMETER(queue);

	/* Does nothing yet. */
	return;
}

/*
 * Takes the oldest frame of the queue with its reference (the caller unrefs it), or NULL when empty.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-dpb.h.
 */
struct h264sw_frame *
h264sw_output_take(
	struct h264sw_output_queue *queue)
{
	UNUSED_PARAMETER(queue);

	/* Reports that nothing is made yet. */
	return NULL;
}

/*
 * Drops every frame of the queue (unref), for a seek or a close.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-dpb.h.
 */
void
h264sw_output_clear(
	struct h264sw_output_queue *queue)
{
	UNUSED_PARAMETER(queue);

	/* Does nothing yet. */
	return;
}

/*
 * Prepares an empty DPB for a sequence.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-dpb.h.
 */
void
h264sw_dpb_init(
	struct h264sw_dpb *dpb,
	const struct h264sw_sps *sps)
{
	UNUSED_PARAMETER(dpb);
	UNUSED_PARAMETER(sps);

	/* Does nothing yet. */
	return;
}

/*
 * Drops every entry without output (unref), for a seek, a close, or an IDR picture with no_output_of_prior_pics_flag (C.4.4 step 3).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-dpb.h.
 */
void
h264sw_dpb_clear(
	struct h264sw_dpb *dpb)
{
	UNUSED_PARAMETER(dpb);

	/* Does nothing yet. */
	return;
}

/*
 * Outputs every frame still needed for output and empties the DPB (the bumping process repeated, C.4.5.3), for the end of a stream.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-dpb.h.
 */
int
h264sw_dpb_flush(
	struct h264sw_dpb *dpb,
	struct h264sw_output_queue *queue)
{
	UNUSED_PARAMETER(dpb);
	UNUSED_PARAMETER(queue);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Stores the non-existing frames of a gap in frame_num (8.2.5.2, C.4.2).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-dpb.h.
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
	UNUSED_PARAMETER(dpb);
	UNUSED_PARAMETER(sps);
	UNUSED_PARAMETER(header);
	UNUSED_PARAMETER(poc);
	UNUSED_PARAMETER(pool);
	UNUSED_PARAMETER(queue);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Marks the references and removes frames before the current picture is stored (C.4.4, 8.2.5).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-dpb.h.
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
	UNUSED_PARAMETER(dpb);
	UNUSED_PARAMETER(sps);
	UNUSED_PARAMETER(header);
	UNUSED_PARAMETER(current);
	UNUSED_PARAMETER(changed);
	UNUSED_PARAMETER(current_marking);
	UNUSED_PARAMETER(current_long_idx);
	UNUSED_PARAMETER(queue);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Stores the current picture (C.4.5).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-dpb.h.
 */
int
h264sw_dpb_store(
	struct h264sw_dpb *dpb,
	struct h264sw_frame *current,
	unsigned marking,
	uint32_t long_idx,
	struct h264sw_output_queue *queue)
{
	UNUSED_PARAMETER(dpb);
	UNUSED_PARAMETER(current);
	UNUSED_PARAMETER(marking);
	UNUSED_PARAMETER(long_idx);
	UNUSED_PARAMETER(queue);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Runs the bumping process once (C.4.5.3).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-dpb.h.
 */
int
h264sw_dpb_bump(
	struct h264sw_dpb *dpb,
	struct h264sw_output_queue *queue)
{
	UNUSED_PARAMETER(dpb);
	UNUSED_PARAMETER(queue);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Counts the entries marked as used for reference (short and long).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-dpb.h.
 */
unsigned
h264sw_dpb_references(
	const struct h264sw_dpb *dpb)
{
	UNUSED_PARAMETER(dpb);

	/* Reports nothing yet. */
	return 0;
}
