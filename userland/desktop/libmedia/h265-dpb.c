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

/*
 * Makes an empty buffer with slots Vulkan DPB slots.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-dpb.h.
 */
void
h265_dpb_init(
	struct h265_dpb *dpb,
	unsigned slots)
{
	UNUSED_PARAMETER(dpb);
	UNUSED_PARAMETER(slots);

	/* Does nothing yet. */
	return;
}

/*
 * Applies the reference picture set of the current picture and the removal
 * and bumping before it (8.3.2, C.5.2.2), and plans its decode.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-dpb.h.
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
	UNUSED_PARAMETER(dpb);
	UNUSED_PARAMETER(sps);
	UNUSED_PARAMETER(pocs);
	UNUSED_PARAMETER(irap_no_rasl);
	UNUSED_PARAMETER(no_output_of_prior_pics);
	UNUSED_PARAMETER(plan);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Chooses the slot the current picture is decoded into: the lowest slot
 * held by no used entry and not busy.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-dpb.h.
 */
int
h265_dpb_take_slot(
	struct h265_dpb *dpb,
	struct h265_dpb_plan *plan)
{
	UNUSED_PARAMETER(dpb);
	UNUSED_PARAMETER(plan);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Stores the decoded current picture and bumps after it (C.5.2.3).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-dpb.h.
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
	UNUSED_PARAMETER(dpb);
	UNUSED_PARAMETER(plan);
	UNUSED_PARAMETER(poc);
	UNUSED_PARAMETER(pic_output);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(output);
	UNUSED_PARAMETER(count);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Ends a picture that was not decoded (the caller skipped it or the decode
 * failed).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-dpb.h.
 */
void
h265_dpb_abort(
	struct h265_dpb *dpb)
{
	UNUSED_PARAMETER(dpb);

	/* Does nothing yet. */
	return;
}

/*
 * Outputs every picture needed for output in output order and empties the
 * buffer.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-dpb.h.
 */
int
h265_dpb_flush(
	struct h265_dpb *dpb,
	struct h265_dpb_output *output,
	unsigned *count)
{
	UNUSED_PARAMETER(dpb);
	UNUSED_PARAMETER(output);
	UNUSED_PARAMETER(count);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Empties the buffer without output (a seek).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-dpb.h.
 */
void
h265_dpb_clear(
	struct h265_dpb *dpb)
{
	UNUSED_PARAMETER(dpb);

	/* Does nothing yet. */
	return;
}

/*
 * Marks slot as read: clears its busy bit.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-dpb.h.
 */
void
h265_dpb_release(
	struct h265_dpb *dpb,
	int slot)
{
	UNUSED_PARAMETER(dpb);
	UNUSED_PARAMETER(slot);

	/* Does nothing yet. */
	return;
}
