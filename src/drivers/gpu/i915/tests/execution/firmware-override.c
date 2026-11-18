/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The test build's answer to the firmware provider's request checkpoint.
 *
 * A unit test installs a replacement before it starts the code that requests
 * firmware and removes it only after that code has finished, so no request
 * sees it change.  With nothing installed the request is left to the file system.
 */

#include "firmware-override.h"

#include "../../firmware.h"

#include <stddef.h>

int drv_i915_firmware_test_request(struct i915_firmware *firmware, const char *name, int *served);

/*
 * The replacement that serves requests instead of the file system.
 *
 * NULL while no unit test runs.  Set and cleared on the start worker, which
 * is also where the code under test requests its firmware.
 */
static const struct i915_test_firmware_override *i915_test_firmware_override;

/*
 * Installs a replacement for the file system read, or removes it with NULL.
 */
void
drv_i915_test_firmware_set_override(
	const struct i915_test_firmware_override *override)
{
	/* Later requests go to the replacement, or back to the file system. */
	i915_test_firmware_override = override;
}

/*
 * Serves a firmware request when a test has installed a replacement.
 *
 * Sets *served and returns the replacement's answer; leaves *served clear
 * when no replacement is installed, so the provider reads the file.
 */
int
drv_i915_firmware_test_request(
	struct i915_firmware *firmware,
	const char *name,
	int *served)
{
	int error;

	/* Leaves the request to the file system when no test replaced it. */
	if (i915_test_firmware_override == NULL)
		return 0;

	/* Hands the request to the replacement; its answer stands. */
	*served = 1;
	error = i915_test_firmware_override->request(i915_test_firmware_override->context, firmware, name);
	if (error != 0)
		return error;

	/* Succeeded: the replacement handed back an image. */
	return 0;
}
