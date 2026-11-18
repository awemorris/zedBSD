/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The service requests of a node: gpu GPU reset (GPU_DEVICE_RESET), which
 * ends every program that uses the device, and gpu GPU diagnose
 * (GPU_DIAGNOSE), the driver's reports as raw bytes.  This build does not
 * make them yet: it says so and fails.
 */

#include "gpu-command.h"

#include <errno.h>

/*
 * Resets a node's device.
 *
 * This build refuses the request; the device is not touched.
 */
int
gpu_service_reset(
	const struct gpu_invocation *command)
{
	/* Says that the reset is not in this build. */
	gpu_fail(command, "reset", ENOTSUP, "reset is not available in this build");

	/* A refused request. */
	return GPU_EXIT_FAILED;
}

/*
 * Lists a driver's report kinds, or reads one report.
 *
 * This build refuses the request.
 */
int
gpu_service_diagnose(
	const struct gpu_invocation *command)
{
	/* Says that the reports are not in this build. */
	gpu_fail(command, "diagnose", ENOTSUP, "diagnose is not available in this build");

	/* A refused request. */
	return GPU_EXIT_FAILED;
}
