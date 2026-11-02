/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The coefficient tokens of a lossy macroblock and their dequantization.
 */

#include "vp8-tokens.h"

#include <errno.h>

/*
 * Reads a macroblock's tokens from its token partition (RFC 6386 13, 13.2,
 * 13.3, 20.16) and dequantizes them with dequant (14.1).
 */
void
webpc_vp8_read_residue(
	struct webpc_bool *tokens,
	const struct webpc_vp8_header *header,
	const struct webpc_vp8_dequant *dequant,
	int has_y2,
	uint8_t left[WEBPC_VP8_CONTEXTS],
	uint8_t above[WEBPC_VP8_CONTEXTS],
	struct webpc_vp8_residue *residue)
{
	UNUSED_PARAMETER(tokens);
	UNUSED_PARAMETER(header);
	UNUSED_PARAMETER(dequant);
	UNUSED_PARAMETER(has_y2);
	UNUSED_PARAMETER(left);
	UNUSED_PARAMETER(above);
	UNUSED_PARAMETER(residue);

	/* Not implemented yet. */
}

/*
 * Sets the contexts of a macroblock whose mb_skip_coeff is 1 (no tokens
 * read; RFC 6386 20.16 reset_mb_context): the Y, U and V entries of left and
 * above become 0, and the Y2 entries too unless has_y2 is 0 (a B_PRED
 * macroblock leaves the Y2 contexts as they were).
 */
void
webpc_vp8_skip_residue(
	int has_y2,
	uint8_t left[WEBPC_VP8_CONTEXTS],
	uint8_t above[WEBPC_VP8_CONTEXTS])
{
	UNUSED_PARAMETER(has_y2);
	UNUSED_PARAMETER(left);
	UNUSED_PARAMETER(above);

	/* Not implemented yet. */
}
