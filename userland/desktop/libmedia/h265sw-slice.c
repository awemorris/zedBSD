/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The slice segment header of H.265 (7.3.6, 7.4.7).
 */

#include "h265sw-slice.h"

#include <errno.h>

/*
 * Reads the first fields of a slice segment header, up to and including
 * slice_pic_parameter_set_id, so the caller can find the PPS and SPS (and
 * decide whether the segment starts a picture) before the rest.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-slice.h.
 */
int
h265sw_slice_read_start(
	struct h265sw_slice_header *out,
	struct vcodec_bitreader *bits,
	const struct h265sw_nal *nal)
{
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(bits);
	UNUSED_PARAMETER(nal);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reads the rest of a slice segment header after h265sw_slice_read_start
 * (7.3.6.1 to 7.3.6.3).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-slice.h.
 */
int
h265sw_slice_read_rest(
	struct h265sw_slice_header *out,
	struct vcodec_bitreader *bits,
	const struct h265sw_sps *sps,
	const struct h265sw_pps *pps,
	const struct h265sw_slice_header *independent)
{
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(bits);
	UNUSED_PARAMETER(sps);
	UNUSED_PARAMETER(pps);
	UNUSED_PARAMETER(independent);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
