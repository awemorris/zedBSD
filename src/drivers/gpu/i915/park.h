/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The park of the request worker for a suspend.
 *
 * A suspend asks the worker to park: the worker leaves the display window
 * (the output is stopped through the display's stop path, the lease is
 * kept), finishes the request it runs, lets the GT go idle (RPS stopped,
 * forcewake given back) and sleeps until the resume ends the park; the
 * requests that come meanwhile wait in their queues.  This file is the
 * protocol's state and decisions; the worker (worker.c) takes the device's
 * IRQ lock around every call and does the sleeping and the waking.
 */

#ifndef DRIVERS_GPU_I915_PARK_H
#define DRIVERS_GPU_I915_PARK_H

#include <stdint.h>

/*
 * What the worker does next, as drv_i915_park_action() decides: go on
 * serving, leave the display window, or park.
 */
enum i915_park_action {
	I915_PARK_SERVE = 0,
	I915_PARK_LEAVE_WINDOW = 1,
	I915_PARK_PARK = 2
};

/*
 * The park of one worker.
 *
 * requested is set by drv_i915_park_begin() and cleared by
 * drv_i915_park_end(); parked is set while the worker sleeps parked.
 * generation counts the parks begun, so that a waiter can tell its own
 * park from a later one.  The device's IRQ lock protects all of it.
 */
struct i915_park {
	uint32_t generation;
	uint8_t requested;
	uint8_t parked;
};

void
drv_i915_park_init(
	struct i915_park *park);

int
drv_i915_park_begin(
	struct i915_park *park);

enum i915_park_action
drv_i915_park_action(
	const struct i915_park *park,
	int in_display,
	int stop);

void
drv_i915_park_entered(
	struct i915_park *park);

int
drv_i915_park_stays(
	const struct i915_park *park,
	int stop);

void
drv_i915_park_left(
	struct i915_park *park);

int
drv_i915_park_end(
	struct i915_park *park);

int
drv_i915_park_reached(
	const struct i915_park *park,
	uint32_t generation);

#endif
