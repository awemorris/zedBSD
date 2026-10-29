/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The direct prediction of B slices of the software H.264 decoder
 * (8.4.1.2).
 */

#include "h264sw-direct.h"
#include "h264sw-context.h"

#include <errno.h>
#include <stddef.h>

/*
 * Prepares the direct prediction of a B slice.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-direct.h.
 */
void
h264sw_direct_setup(
	struct h264sw_direct *direct,
	const struct h264sw_reflist *lists,
	int32_t poc)
{
	UNUSED_PARAMETER(direct);
	UNUSED_PARAMETER(lists);
	UNUSED_PARAMETER(poc);

	/* Does nothing yet. */
	return;
}

/*
 * Derives the motion of one 8x8 quadrant of the current macroblock in direct mode (8.4.1.2).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-direct.h.
 */
int
h264sw_direct_quadrant(
	struct h264sw_slice_ctx *ctx,
	unsigned quadrant)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(quadrant);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
