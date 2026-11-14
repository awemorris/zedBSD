/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The display core's checks of the flags a backend declares and a request carries.
 */

#ifndef DRIVERS_GPU_DISPLAY_FLAGS_H
#define DRIVERS_GPU_DISPLAY_FLAGS_H

#include <drivers/gpu/gpu-display.h>
#include <uapi/errno.h>
#include <stdint.h>

/* Every flag of drv_gpu_display_ops.flags this core honors. */
#define GPU_DISPLAY_OPS_KNOWN_FLAGS	(DRV_GPU_DISPLAY_UNGATED | DRV_GPU_DISPLAY_ROTATE_180)

/* Every flag of gpu_display_plane_state.flags a commit may carry. */
#define GPU_DISPLAY_PLANE_KNOWN_FLAGS	(GPU_DISPLAY_PLANE_SHOWN | GPU_DISPLAY_PLANE_IMAGE | GPU_DISPLAY_PLANE_ROTATE_180)

/*
 * Checks the flags of a backend's display operations at registration.
 *
 * A flag this core does not know would be a promise nobody keeps.
 */
static inline int
gpu_display_ops_flags_check(
	uint32_t ops_flags)
{
	/* Refuses a flag this core cannot honor. */
	if ((ops_flags & ~GPU_DISPLAY_OPS_KNOWN_FLAGS) != 0U)
		return EINVAL;

	/* Succeeded: every declared flag is one the core honors. */
	return 0;
}

/*
 * Checks a present's flags against the backend's display operations.
 *
 * A present is FIFO, optionally BLOB, optionally turned by 180 degrees.
 * The turn reaches only a backend that declares it can show one; for any
 * other the request is refused before the backend sees it, so no backend
 * ignores the flag or answers it differently.
 */
static inline int
gpu_display_present_flags_check(
	uint32_t flags,
	uint32_t ops_flags)
{
	uint32_t mode;

	/* The mode without the turn is FIFO or FIFO with BLOB. */
	mode = flags & ~GPU_DISPLAY_PRESENT_ROTATE_180;
	if (mode != GPU_DISPLAY_PRESENT_FIFO && mode != (GPU_DISPLAY_PRESENT_FIFO | GPU_DISPLAY_PRESENT_BLOB))
		return EINVAL;

	/* A turned frame needs a backend that can show one. */
	if ((flags & GPU_DISPLAY_PRESENT_ROTATE_180) != 0U && (ops_flags & DRV_GPU_DISPLAY_ROTATE_180) == 0U)
		return EOPNOTSUPP;

	/* Succeeded: the backend may take the frame with these flags. */
	return 0;
}

/*
 * Checks one commit plane's flags, without the backend.
 *
 * Only the primary may be turned, and only while it is shown.
 */
static inline int
gpu_display_plane_flags_check(
	uint32_t plane,
	uint32_t flags)
{
	/* Refuses a flag the commit does not define. */
	if ((flags & ~GPU_DISPLAY_PLANE_KNOWN_FLAGS) != 0U)
		return EINVAL;

	/* An unturned plane needs nothing more. */
	if ((flags & GPU_DISPLAY_PLANE_ROTATE_180) == 0U)
		return 0;

	/* Overlays are never turned. */
	if (plane != GPU_DISPLAY_PLANE_PRIMARY)
		return EINVAL;

	/* A hidden primary has no picture to turn. */
	if ((flags & GPU_DISPLAY_PLANE_SHOWN) == 0U)
		return EINVAL;

	/* Succeeded: the turn applies to the shown primary. */
	return 0;
}

/*
 * Checks a commit plane's turn against the backend's display operations.
 *
 * Called once the request's own fields are checked: a turn reaches only a
 * backend that declares it can show one.
 */
static inline int
gpu_display_plane_rotation_check(
	uint32_t flags,
	uint32_t ops_flags)
{
	/* A turned primary needs a backend that can show one. */
	if ((flags & GPU_DISPLAY_PLANE_ROTATE_180) != 0U && (ops_flags & DRV_GPU_DISPLAY_ROTATE_180) == 0U)
		return EOPNOTSUPP;

	/* Succeeded: the backend may take the plane's state. */
	return 0;
}

#endif
