/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The GHCI driver's checks on words: the ring's room and the padding
 * before its end.  Nothing here touches the kernel or the hardware.
 *
 * Values referenced: amaterrace-gpu d9252e4 docs/ghci.md (section 6.1,
 * the ring's indices in dwords and the one dword left unused; section
 * 6.2, a packet never crosses the end of the ring).
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "ghci-words.h"

/*
 * Plans where the next run of packets goes in the ring.
 */
int
ghci_ring_plan(
	uint32_t wptr,
	uint32_t rptr,
	uint32_t ring_dwords,
	uint32_t need,
	uint32_t *pad)
{
	uint32_t free_dwords;
	uint32_t to_end;
	uint32_t padding;

	/* Refuses a ring of no size and positions outside the ring. */
	if (ring_dwords == 0U)
		return EINVAL;
	if (wptr >= ring_dwords)
		return EINVAL;
	if (rptr >= ring_dwords)
		return EINVAL;

	/* Refuses an empty run, and one that could never fit beside the unused dword. */
	if (need == 0U)
		return EINVAL;
	if (need >= ring_dwords)
		return EINVAL;

	/* The room is the distance back to the reader, less the one dword the ring leaves unused. */
	free_dwords = (rptr + ring_dwords - wptr - 1U) % ring_dwords;

	/* A packet never crosses the end: the rest of the ring is padded when it does not hold the run. */
	to_end = ring_dwords - wptr;
	padding = 0U;
	if (need > to_end)
		padding = to_end;

	/* Waits for the reader while the padding and the run do not both fit. */
	if (free_dwords < padding + need)
		return EAGAIN;

	/* Succeeded: the padding, if any, and the run fit. */
	*pad = padding;
	return 0;
}
