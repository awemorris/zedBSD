/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The CAVLC entropy decoding of the software H.264 decoder (9.2, 9.1.2).
 */

#include "h264sw-cavlc.h"
#include "h264sw-context.h"

#include <errno.h>
#include <stddef.h>

/*
 * Reads one residual block (residual_block_cavlc, 7.3.5.3.2, and 9.2).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-cavlc.h.
 */
int
h264sw_cavlc_residual_block(
	struct h264sw_bits *bits,
	int nc,
	unsigned start,
	unsigned end,
	unsigned max_coeff,
	int32_t *levels,
	unsigned *total_coeff)
{
	UNUSED_PARAMETER(bits);
	UNUSED_PARAMETER(nc);
	UNUSED_PARAMETER(start);
	UNUSED_PARAMETER(end);
	UNUSED_PARAMETER(max_coeff);
	UNUSED_PARAMETER(levels);
	UNUSED_PARAMETER(total_coeff);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Derives nC for a block of the current macroblock (9.2.1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-cavlc.h.
 */
int
h264sw_cavlc_nc(
	const struct h264sw_slice_ctx *ctx,
	unsigned block)
{
	UNUSED_PARAMETER(ctx);
	UNUSED_PARAMETER(block);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Maps the codeNum of coded_block_pattern me(v) to its value (9.1.2, Table 9-4).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-cavlc.h.
 */
int
h264sw_cavlc_cbp(
	uint32_t code_num,
	int intra,
	unsigned *cbp)
{
	UNUSED_PARAMETER(code_num);
	UNUSED_PARAMETER(intra);
	UNUSED_PARAMETER(cbp);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
