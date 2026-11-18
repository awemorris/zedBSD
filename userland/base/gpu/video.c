/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * gpu GPU video: the Vulkan Video codecs, profiles and limits of a render
 * node, read in a child process with libvulkan opened at run time.  This
 * build does not read them yet: it says so and fails.
 */

#include "gpu-command.h"

#include <errno.h>

/*
 * Shows the video codecs of a node.
 *
 * This build refuses the request.
 */
int
gpu_video_run(
	const struct gpu_invocation *command)
{
	/* Says that video is not in this build. */
	gpu_fail(command, "video", ENOTSUP, "video is not available in this build");

	/* A refused request. */
	return GPU_EXIT_FAILED;
}
