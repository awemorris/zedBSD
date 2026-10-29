/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The slice header of the software H.264 decoder (7.3.3) and the first
 * slice of a picture (7.4.1.2.4).
 */

#include "h264sw-slice.h"

#include <errno.h>
#include <stddef.h>

/*
 * Reads a slice header from a slice RBSP (7.3.3).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-slice.h.
 */
int
h264sw_slice_read(
	struct h264sw_bits *bits,
	const struct h264sw_nal *nal,
	const struct h264sw_ps_store *store,
	struct h264sw_slice_header *header)
{
	UNUSED_PARAMETER(bits);
	UNUSED_PARAMETER(nal);
	UNUSED_PARAMETER(store);
	UNUSED_PARAMETER(header);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reports whether a slice starts a new picture (7.4.1.2.4).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-slice.h.
 */
int
h264sw_slice_new_picture(
	const struct h264sw_slice_header *previous,
	const struct h264sw_slice_header *current,
	const struct h264sw_sps *sps)
{
	UNUSED_PARAMETER(previous);
	UNUSED_PARAMETER(current);
	UNUSED_PARAMETER(sps);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
