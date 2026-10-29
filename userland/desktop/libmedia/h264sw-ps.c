/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The sequence and picture parameter sets of the software H.264 decoder
 * (7.3.2.1.1, 7.3.2.2) and their scaling lists (7.3.2.1.1.1).
 */

#include "h264sw-ps.h"
#include "h264sw-tables.h"

#include <errno.h>
#include <stddef.h>

/*
 * Empties a store: no set present, every generation 0.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-ps.h.
 */
void
h264sw_ps_init(
	struct h264sw_ps_store *store)
{
	UNUSED_PARAMETER(store);

	/* Does nothing yet. */
	return;
}

/*
 * Reads a sequence parameter set RBSP and keeps it in store (7.3.2.1.1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-ps.h.
 */
int
h264sw_ps_read_sps(
	struct h264sw_ps_store *store,
	struct h264sw_bits *bits)
{
	UNUSED_PARAMETER(store);
	UNUSED_PARAMETER(bits);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reads a picture parameter set RBSP and keeps it in store (7.3.2.2).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-ps.h.
 */
int
h264sw_ps_read_pps(
	struct h264sw_ps_store *store,
	struct h264sw_bits *bits)
{
	UNUSED_PARAMETER(store);
	UNUSED_PARAMETER(bits);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Resolves the scaling lists a picture uses from its SPS and PPS (7.4.2.1.1, 7.4.2.2, Table 7-2).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h264sw-ps.h.
 */
void
h264sw_ps_scaling(
	const struct h264sw_sps *sps,
	const struct h264sw_pps *pps,
	struct h264sw_scaling *scaling)
{
	UNUSED_PARAMETER(sps);
	UNUSED_PARAMETER(pps);
	UNUSED_PARAMETER(scaling);

	/* Does nothing yet. */
	return;
}
