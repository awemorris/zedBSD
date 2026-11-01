/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The Vulkan Video StdVideoH265 structures of the H.265 decode path.
 */

#include "h265-std.h"

#include <errno.h>

/*
 * Makes the StdVideo form of a profile, tier and level.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-std.h.
 */
int
h265_std_ptl(
	const struct h265sw_ptl *ptl,
	StdVideoH265ProfileTierLevel *out)
{
	UNUSED_PARAMETER(ptl);
	UNUSED_PARAMETER(out);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Makes the StdVideo form of a VPS (every field of struct h265sw_vps that
 * StdVideoH265VideoParameterSet names; the reserved fields 0).
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-std.h.
 */
int
h265_std_vps(
	const struct h265sw_vps *vps,
	struct h265_std_vps *out)
{
	UNUSED_PARAMETER(vps);
	UNUSED_PARAMETER(out);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Makes the StdVideo form of an SPS.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-std.h.
 */
int
h265_std_sps(
	const struct h265sw_sps *sps,
	struct h265_std_sps *out)
{
	UNUSED_PARAMETER(sps);
	UNUSED_PARAMETER(out);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Makes the StdVideo form of a PPS.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-std.h.
 */
int
h265_std_pps(
	const struct h265sw_pps *pps,
	const struct h265sw_sps *sps,
	struct h265_std_pps *out)
{
	UNUSED_PARAMETER(pps);
	UNUSED_PARAMETER(sps);
	UNUSED_PARAMETER(out);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Makes the picture information of one decode.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-std.h.
 */
int
h265_std_picture(
	const struct h265_picture *picture,
	const struct h265_dpb_plan *plan,
	StdVideoDecodeH265PictureInfo *info)
{
	UNUSED_PARAMETER(picture);
	UNUSED_PARAMETER(plan);
	UNUSED_PARAMETER(info);

	/* Reports that the function is not implemented yet. */
	return ENOSYS;
}

/*
 * Makes the reference information of the plan's references.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-std.h.
 */
void
h265_std_references(
	const struct h265_dpb_plan *plan,
	StdVideoDecodeH265ReferenceInfo *info)
{
	UNUSED_PARAMETER(plan);
	UNUSED_PARAMETER(info);

	/* Does nothing yet. */
	return;
}

/*
 * Makes the reference information of the picture being decoded.
 *
 * Not implemented yet: the specification is the comment of the
 * declaration in h265-std.h.
 */
void
h265_std_setup(
	int32_t poc,
	StdVideoDecodeH265ReferenceInfo *info)
{
	UNUSED_PARAMETER(poc);
	UNUSED_PARAMETER(info);

	/* Does nothing yet. */
	return;
}
