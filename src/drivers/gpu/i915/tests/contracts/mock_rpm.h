/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A mock device behind struct i915_rpm_ops.
 *
 * It counts resumes and suspends and can be told to fail every resume.
 */

#ifndef DRIVERS_GPU_I915_TESTS_CONTRACTS_MOCK_RPM_H
#define DRIVERS_GPU_I915_TESTS_CONTRACTS_MOCK_RPM_H

#include "../../runtime-pm.h"

/*
 * The resume and suspend record of one mock device.
 *
 * A test owns it for the whole program; mock_rpm_reset() clears it.
 */
struct mock_rpm {
	/* Nonzero makes every resume fail with EIO. */
	int resume_fail;

	/* How many resumes and suspends reached the device. */
	int resume_calls;
	int suspend_calls;
};

const struct i915_rpm_ops *mock_rpm_ops(void);
void mock_rpm_reset(struct mock_rpm *mock);

#endif
