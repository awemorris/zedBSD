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

/*
 * Builds the reference picture lists of a P or B slice (8.2.4).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-reflist.h.
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
	UNUSED_PARAMETER(dpb);
	UNUSED_PARAMETER(header);
	UNUSED_PARAMETER(frame_num);
	UNUSED_PARAMETER(max_frame_num);
	UNUSED_PARAMETER(poc);
	UNUSED_PARAMETER(lists);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Finds the lowest index of list 0 whose frame is the frame of a given id (8.4.1.2.3, refIdxL0).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-reflist.h.
 */
int
h264sw_reflist_find(
	const struct h264sw_reflist *lists,
	uint32_t frame_id)
{
	UNUSED_PARAMETER(lists);
	UNUSED_PARAMETER(frame_id);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
