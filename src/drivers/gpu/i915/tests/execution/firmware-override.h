/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A replacement for the firmware provider's file system read, for unit tests.
 *
 * The test build answers the provider's request checkpoint here.  While a test
 * has installed a replacement, every firmware request goes to it; otherwise
 * the provider reads /lib/firmware as in production.  The image a replacement
 * hands back stays the test's memory: the release does not free it.
 */

#ifndef DRIVERS_GPU_I915_TESTS_EXECUTION_FIRMWARE_OVERRIDE_H
#define DRIVERS_GPU_I915_TESTS_EXECUTION_FIRMWARE_OVERRIDE_H

struct i915_firmware;

/*
 * One replacement a test installs.
 *
 * The test owns it and keeps it alive until it removes it again.
 */
struct i915_test_firmware_override {
	/* Serves one request; returns 0 or a positive errno, like drv_i915_firmware_request(). */
	int (*request)(void *context, struct i915_firmware *firmware, const char *name);

	/* The context the request is given. */
	void *context;
};

void drv_i915_test_firmware_set_override(const struct i915_test_firmware_override *override);

#endif
