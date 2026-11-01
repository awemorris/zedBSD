/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The short-term reference picture sets of H.265 (7.3.7, 7.4.8).
 */

#include "h265sw-rps.h"

#include <errno.h>

/*
 * Reads st_ref_pic_set( index ) (7.3.7) and derives the set (7.4.8).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-rps.h.
 */
int
h265sw_rps_read(
	struct vcodec_bitreader *bits,
	unsigned index,
	unsigned num_sets,
	const struct h265sw_st_rps *sets,
	unsigned max_pictures,
	struct h265sw_st_rps *out,
	struct h265sw_st_rps_syntax *syntax)
{
	UNUSED_PARAMETER(bits);
	UNUSED_PARAMETER(index);
	UNUSED_PARAMETER(num_sets);
	UNUSED_PARAMETER(sets);
	UNUSED_PARAMETER(max_pictures);
	UNUSED_PARAMETER(out);
	UNUSED_PARAMETER(syntax);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
