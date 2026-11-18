/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * gpu GPU vulkan: the Vulkan device of a render node, its properties,
 * features, limits, formats, extensions and queues, read in a child
 * process with libvulkan opened at run time.  This build does not read
 * them yet: it says so and fails.
 */

#include "gpu-command.h"

#include <errno.h>

/*
 * Shows the Vulkan device of a node.
 *
 * This build refuses the request.
 */
int
gpu_vulkan_run(
	const struct gpu_invocation *command)
{
	/* Says that Vulkan is not in this build. */
	gpu_fail(command, "vulkan", ENOTSUP, "vulkan is not available in this build");

	/* A refused request. */
	return GPU_EXIT_FAILED;
}
