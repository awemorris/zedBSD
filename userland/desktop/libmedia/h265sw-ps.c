/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The video, sequence and picture parameter sets of H.265 and their store
 * (7.3.2, 7.3.3, 7.4.3, 7.4.4).
 */

#include "h265sw-ps.h"

#include <errno.h>

/*
 * Empties a store (every flag and counter 0).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-ps.h.
 */
void
h265sw_ps_init(
	struct h265sw_ps_store *store)
{
	UNUSED_PARAMETER(store);

	/* Does nothing yet. */
	return;
}

/*
 * Reads a profile_tier_level( 1, max_sub_layers_minus1 ) (7.3.3) into
 * *ptl.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-ps.h.
 */
int
h265sw_ps_read_ptl(
	struct h265sw_ptl *ptl,
	struct vcodec_bitreader *bits,
	unsigned max_sub_layers_minus1)
{
	UNUSED_PARAMETER(ptl);
	UNUSED_PARAMETER(bits);
	UNUSED_PARAMETER(max_sub_layers_minus1);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reads a video parameter set RBSP and keeps it in store (7.3.2.1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-ps.h.
 */
int
h265sw_ps_read_vps(
	struct h265sw_ps_store *store,
	struct vcodec_bitreader *bits)
{
	UNUSED_PARAMETER(store);
	UNUSED_PARAMETER(bits);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reads a sequence parameter set RBSP and keeps it in store (7.3.2.2).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-ps.h.
 */
int
h265sw_ps_read_sps(
	struct h265sw_ps_store *store,
	struct vcodec_bitreader *bits)
{
	UNUSED_PARAMETER(store);
	UNUSED_PARAMETER(bits);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reports whether the software decoder takes a sequence.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-ps.h.
 */
int
h265sw_ps_sps_supported(
	const struct h265sw_sps *sps)
{
	UNUSED_PARAMETER(sps);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Reads a picture parameter set RBSP and keeps it in store (7.3.2.3).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-ps.h.
 */
int
h265sw_ps_read_pps(
	struct h265sw_ps_store *store,
	struct vcodec_bitreader *bits)
{
	UNUSED_PARAMETER(store);
	UNUSED_PARAMETER(bits);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Checks a PPS against the SPS it names, for the values whose ranges
 * depend on the SPS (7.4.3.3.1).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265sw-ps.h.
 */
int
h265sw_ps_check_pair(
	const struct h265sw_sps *sps,
	const struct h265sw_pps *pps)
{
	UNUSED_PARAMETER(sps);
	UNUSED_PARAMETER(pps);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}
