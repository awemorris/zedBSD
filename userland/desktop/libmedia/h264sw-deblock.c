/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The deblocking filter of the software H.264 decoder (8.7).
 */

#include "h264sw-deblock.h"
#include "h264sw-context.h"

#include <errno.h>
#include <stddef.h>

/*
 * Filters a decoded picture in place (8.7).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-deblock.h.
 */
void
h264sw_deblock_picture(
	struct h264sw_picture_ctx *picture)
{
	UNUSED_PARAMETER(picture);

	/* Does nothing yet. */
	return;
}

/*
 * Derives the boundary strength of the four segments of one edge of a macroblock (8.7.2.1, frames).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-deblock.h.
 */
void
h264sw_deblock_bs(
	const struct h264sw_picture_ctx *picture,
	unsigned mb_addr,
	int vertical,
	unsigned edge,
	uint8_t *bs)
{
	UNUSED_PARAMETER(picture);
	UNUSED_PARAMETER(mb_addr);
	UNUSED_PARAMETER(vertical);
	UNUSED_PARAMETER(edge);
	UNUSED_PARAMETER(bs);

	/* Does nothing yet. */
	return;
}

/*
 * Filters one luma edge of 16 samples (8.7.2.2 to 8.7.2.4, chromaEdgeFlag 0).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-deblock.h.
 */
void
h264sw_deblock_luma_edge(
	uint8_t *q0,
	ptrdiff_t across,
	ptrdiff_t along,
	const uint8_t *bs,
	int qp_av,
	int offset_a,
	int offset_b)
{
	UNUSED_PARAMETER(q0);
	UNUSED_PARAMETER(across);
	UNUSED_PARAMETER(along);
	UNUSED_PARAMETER(bs);
	UNUSED_PARAMETER(qp_av);
	UNUSED_PARAMETER(offset_a);
	UNUSED_PARAMETER(offset_b);

	/* Does nothing yet. */
	return;
}

/*
 * Filters one chroma edge of 8 samples of 4:2:0 (8.7.2.2 to 8.7.2.4, chromaEdgeFlag 1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-deblock.h.
 */
void
h264sw_deblock_chroma_edge(
	uint8_t *q0,
	ptrdiff_t across,
	ptrdiff_t along,
	const uint8_t *bs,
	int qp_av,
	int offset_a,
	int offset_b)
{
	UNUSED_PARAMETER(q0);
	UNUSED_PARAMETER(across);
	UNUSED_PARAMETER(along);
	UNUSED_PARAMETER(bs);
	UNUSED_PARAMETER(qp_av);
	UNUSED_PARAMETER(offset_a);
	UNUSED_PARAMETER(offset_b);

	/* Does nothing yet. */
	return;
}
