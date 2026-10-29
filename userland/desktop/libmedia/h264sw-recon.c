/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The reconstruction of a macroblock in the software H.264 decoder.
 */

#include "h264sw-recon.h"
#include "h264sw-context.h"

#include <errno.h>
#include <stddef.h>

/*
 * Reconstructs the current macroblock (ctx->mb_addr, ctx->n, ctx->syntax).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-recon.h.
 */
int
h264sw_recon_mb(
	struct h264sw_slice_ctx *ctx)
{
	UNUSED_PARAMETER(ctx);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
