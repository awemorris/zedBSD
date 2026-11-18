/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The desktop session's side of gpu GPU display: finding the session that
 * holds the display, and asking its compositor for a change of mode
 * (set, keep, revert, reset) with the same requests and trial as Settings.
 * This build does not make the change yet: it says so and fails.
 */

#include "gpu-command.h"

#include <errno.h>

/*
 * Asks the desktop session's compositor for a change of one display's mode.
 *
 * This build refuses the change: no mode is ever sent to the kernel from
 * gpu, and the compositor's side is not here yet.
 */
int
gpu_session_display_change(
	const struct gpu_invocation *command)
{
	/* Says that the change is not in this build. */
	gpu_fail(command, "display", ENOTSUP, "display set, keep, revert and reset are not available in this build");

	/* A refused request. */
	return GPU_EXIT_FAILED;
}
