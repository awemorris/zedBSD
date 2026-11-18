/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The firmware provider.
 *
 * It is the driver's request_firmware() and release_firmware(): a request
 * reads /lib/firmware/<name> from the root file system into a buffer of its
 * own, and the release frees that buffer.  The firmware itself is installed by
 * the optional i915-firmware package; the kernel carries no firmware bytes.
 */

#ifndef DRIVERS_GPU_I915_FIRMWARE_H
#define DRIVERS_GPU_I915_FIRMWARE_H

#include <stdint.h>

/*
 * One firmware image a caller holds.
 *
 * The data is NULL until a request succeeds and again after the release.
 * The allocation is the buffer the provider read the file into, which the
 * release frees; it is NULL for an image the test build served from memory
 * of its own, which the release leaves alone.
 */
struct i915_firmware {
	/* The image bytes, or NULL. */
	const uint8_t *data;

	/* The image length in bytes. */
	unsigned size;

	/* The buffer the release frees, or NULL when the provider owns none. */
	void *allocation;
};

int drv_i915_firmware_request(struct i915_firmware *firmware, const char *name);
void drv_i915_firmware_release(struct i915_firmware *firmware);

#endif
