/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The video usability information and the hypothetical reference decoder
 * parameters of H.265 (E.2, E.3).
 */

#include "h265sw-vui.h"

#include <errno.h>

/*
 * Reads hrd_parameters( common_present, max_sub_layers_minus1 ) into hrd
 * (E.2.2, E.2.3).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-vui.h.
 */
int
h265sw_vui_read_hrd(
	struct h265sw_hrd *hrd,
	struct vcodec_bitreader *bits,
	int common_present,
	unsigned max_sub_layers_minus1,
	const struct h265sw_hrd *previous)
{
	UNUSED_PARAMETER(hrd);
	UNUSED_PARAMETER(bits);
	UNUSED_PARAMETER(common_present);
	UNUSED_PARAMETER(max_sub_layers_minus1);
	UNUSED_PARAMETER(previous);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reads vui_parameters() into vui (E.2.1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-vui.h.
 */
int
h265sw_vui_read(
	struct h265sw_vui *vui,
	struct vcodec_bitreader *bits,
	unsigned max_sub_layers_minus1)
{
	UNUSED_PARAMETER(vui);
	UNUSED_PARAMETER(bits);
	UNUSED_PARAMETER(max_sub_layers_minus1);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
