/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The slice data and the macroblock layer syntax of the software H.264
 * decoder (7.3.4, 7.3.5).
 */

#include "h264sw-macroblock.h"
#include "h264sw-context.h"

#include <errno.h>
#include <stddef.h>

/*
 * Decodes the macroblocks of a slice (7.3.4).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-macroblock.h.
 */
int
h264sw_slice_data(
	struct h264sw_slice_ctx *ctx)
{
	UNUSED_PARAMETER(ctx);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Parses one macroblock (7.3.5 and below) into ctx->syntax.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-macroblock.h.
 */
int
h264sw_macroblock_parse(
	struct h264sw_slice_ctx *ctx,
	int skipped)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(skipped);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
