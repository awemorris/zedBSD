/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A mock device behind struct i915_rpm_ops (see mock_rpm.h).
 */

#include "mock_rpm.h"

#include <errno.h>

static int mock_rpm_resume(void *context);
static void mock_rpm_suspend(void *context);

/*
 * Returns the operations that reach a mock device.
 *
 * The context those operations receive is a struct mock_rpm.
 */
const struct i915_rpm_ops *
mock_rpm_ops(void)
{
	static const struct i915_rpm_ops ops = {
		mock_rpm_resume,
		mock_rpm_suspend
	};

	/* Succeeded: the operations reach the mock. */
	return &ops;
}

/*
 * Clears the mock's failure switch and call counts.
 */
void
mock_rpm_reset(
	struct mock_rpm *mock)
{
	/* Resumes succeed and nothing has been called. */
	mock->resume_fail = 0;
	mock->resume_calls = 0;
	mock->suspend_calls = 0;
}

/* Counts a resume and fails it when asked. */
static int
mock_rpm_resume(
	void *context)
{
	struct mock_rpm *mock;

	/* Counts the resume whatever its outcome. */
	mock = context;
	mock->resume_calls++;

	/* A device told to fail stays suspended. */
	if (mock->resume_fail != 0)
		return EIO;

	/* Succeeded: the device is in D0. */
	return 0;
}

/* Counts a suspend. */
static void
mock_rpm_suspend(
	void *context)
{
	struct mock_rpm *mock;

	/* Counts the suspend. */
	mock = context;
	mock->suspend_calls++;
}
