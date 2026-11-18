/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The park of the request worker for a suspend (see park.h).
 *
 * Every function runs with the device's IRQ lock held by the caller, and
 * none sleeps, so the protocol is tested on the host.
 */

#include <uapi/errno.h>

#include "park.h"

/*
 * Starts a worker with no park asked for.
 */
void
drv_i915_park_init(
	struct i915_park *park)
{
	/* Nothing asked, nothing parked, no park begun. */
	park->generation = 0U;
	park->requested = 0U;
	park->parked = 0U;
}

/*
 * Asks the worker to park.
 *
 * It reports EBUSY while a park is already asked for.  The caller wakes
 * the worker after it, and waits for drv_i915_park_reached() with the
 * generation the park has.
 */
int
drv_i915_park_begin(
	struct i915_park *park)
{
	/* Refuses a park inside a park. */
	if (park->requested)
		return EBUSY;

	/* requested makes the worker leave the window and park at its next look; generation names this park. */
	park->requested = 1U;
	park->generation++;

	/* Succeeded: the worker parks at its next look. */
	return 0;
}

/*
 * Decides what the worker does at a look at its queues.
 *
 * Without a park it serves.  A stop wins over a park: the worker
 * finishes the queued work and withdraws the node, and the waiter sees it
 * never parked.  Inside the display window it leaves the window first
 * (the output is stopped); outside it, it parks.
 */
enum i915_park_action
drv_i915_park_action(
	const struct i915_park *park,
	int in_display,
	int stop)
{
	/* No park is asked for. */
	if (!park->requested)
		return I915_PARK_SERVE;

	/* A stop goes on to its end instead. */
	if (stop)
		return I915_PARK_SERVE;

	/* The window is left before the worker parks. */
	if (in_display)
		return I915_PARK_LEAVE_WINDOW;

	/* Parks. */
	return I915_PARK_PARK;
}

/*
 * Records that the worker sleeps parked, with the GT idle.
 */
void
drv_i915_park_entered(
	struct i915_park *park)
{
	/* parked tells the suspend that the worker and the GT are idle. */
	park->parked = 1U;
}

/*
 * Tells whether the parked worker keeps sleeping: while the park is asked
 * for and no stop came.
 */
int
drv_i915_park_stays(
	const struct i915_park *park,
	int stop)
{
	/* A stop ends the park: the worker goes to withdraw the node. */
	if (stop)
		return 0;

	/* The resume ended the park. */
	if (!park->requested)
		return 0;

	/* The park goes on. */
	return 1;
}

/*
 * Records that the worker left the park and serves again.
 */
void
drv_i915_park_left(
	struct i915_park *park)
{
	/* The worker and the GT run again. */
	park->parked = 0U;
}

/*
 * Ends the park, for the resume or for a suspend that gave up.
 *
 * It reports nonzero when the worker had parked, which the suspend that
 * gave up uses to tell a worker that was late from one that never came.
 */
int
drv_i915_park_end(
	struct i915_park *park)
{
	int was_parked;

	/* Remembers whether the worker had parked. */
	was_parked = 0;
	if (park->parked)
		was_parked = 1;

	/* Clearing requested lets the parked worker go, and a worker on its way does not park. */
	park->requested = 0U;

	/* Succeeded: reports whether the worker had parked. */
	return was_parked;
}

/*
 * Tells whether the park of a generation was reached: it is still the
 * park asked for, and the worker sleeps parked.
 */
int
drv_i915_park_reached(
	const struct i915_park *park,
	uint32_t generation)
{
	/* A later park, or one that ended, is not the waiter's. */
	if (!park->requested || park->generation != generation)
		return 0;

	/* The worker has not parked yet. */
	if (!park->parked)
		return 0;

	/* The worker sleeps parked. */
	return 1;
}
