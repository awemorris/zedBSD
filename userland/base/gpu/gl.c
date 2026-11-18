/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * gpu GPU gl: the OpenGL ES and desktop OpenGL versions, extensions and
 * limits of a render node, each read in a child process of its own with
 * libEGL and the GL library opened at run time.  This build does not read
 * them yet: it says so and fails.
 */

#include "gpu-command.h"

#include <errno.h>

/*
 * Shows the GL and GLES of a node.
 *
 * This build refuses the request.
 */
int
gpu_gl_run(
	const struct gpu_invocation *command)
{
	/* Says that GL is not in this build. */
	gpu_fail(command, "gl", ENOTSUP, "gl is not available in this build");

	/* A refused request. */
	return GPU_EXIT_FAILED;
}
