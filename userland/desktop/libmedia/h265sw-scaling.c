/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The scaling lists of H.265: scaling_list_data() and the default lists
 * (7.3.4, 7.4.5).
 */

#include "h265sw-scaling.h"

#include <errno.h>

/*
 * Sets list to the default lists (Tables 7-5 and 7-6).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-scaling.h.
 */
void
h265sw_scaling_default(
	struct h265sw_scaling_list *list)
{
	UNUSED_PARAMETER(list);

	/* Does nothing yet. */
	return;
}

/*
 * Reads scaling_list_data() (7.3.4) into list.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-scaling.h.
 */
int
h265sw_scaling_read(
	struct h265sw_scaling_list *list,
	struct vcodec_bitreader *bits)
{
	UNUSED_PARAMETER(list);
	UNUSED_PARAMETER(bits);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
