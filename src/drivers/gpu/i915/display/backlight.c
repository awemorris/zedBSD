/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The panel's backlight as a backlight device.
 *
 * The kernel's backlight class calls the provider's operations on the
 * requesting thread; each is handed to the request worker and waited for,
 * because the panel's modeset state belongs to the worker.  The worker
 * answers inside the display window, while the driver lights the panel:
 * outside it the firmware's picture is still up (or the panel is stopped)
 * and the light cannot be reached, which is EBUSY.  The brightness the
 * firmware left is never changed by the driver itself.
 */

#include "internal.h"
#include "backlight.h"

#include "../i915.h"
#include "../worker.h"

#include <kern/backlight.h>
#include <kern/klog.h>

#include <uapi/backlight.h>
#include <uapi/errno.h>
#include <stddef.h>

/* The range of a backlight device's brightness: a percentage. */
#define I915_BACKLIGHT_PERCENT_MAX	100U

static int i915_backlight_get(void *context, uint32_t *percent);
static int i915_backlight_set(void *context, uint32_t percent);
static int i915_backlight_result(int result);

/* The provider's operations. */
static const struct kern_backlight_ops i915_backlight_ops = {
	.get = i915_backlight_get,
	.set = i915_backlight_set
};

/*
 * Registers the panel's backlight, when the node's output is the panel.
 *
 * Returns 0 (also when there is nothing to register), or the error of the
 * registration, which the caller only logs: the node works without it.
 */
int
drv_i915_display_backlight_register(
	struct i915_device *device)
{
	struct i915_display *display;
	struct kern_backlight *backlight;
	int error;

	/* Only a node that drives the eDP panel has its light. */
	display = device->display;
	if (display == NULL || display->absent)
		return 0;
	if (display->rctx.lcd == NULL)
		return 0;
	if (display->output.none || display->output.kind != I915_OUTPUT_KIND_PANEL)
		return 0;

	/* Publishes the backlight device. */
	error = kern_backlight_register("i915", BACKLIGHT_TYPE_PANEL, &i915_backlight_ops, device, &backlight);
	if (error != 0) {
		kern_logf("i915: backlight: the device could not be registered rc=%d\n", error);
		return error;
	}

	/* The withdrawal gives it back. */
	display->backlight = backlight;
	kern_logf("i915: backlight: the panel's light is a backlight device\n");

	/* Succeeded: the panel's light can be asked for. */
	return 0;
}

/*
 * Withdraws the panel's backlight device, if one was registered.
 */
void
drv_i915_display_backlight_unregister(
	struct i915_device *device)
{
	struct i915_display *display;

	/* Nothing was registered without a display or a device. */
	display = device->display;
	if (display == NULL || display->backlight == NULL)
		return;

	/* No request reaches the worker from here on. */
	kern_backlight_unregister(display->backlight);
	display->backlight = NULL;
}

/*
 * Reads or sets the panel's brightness, or switches its light (op:
 * I915_BACKLIGHT_*; *value is the percent, or 1 for on), on the worker.
 *
 * Runs on the request worker.  Returns 0 with the brightness in *value (the
 * light switched: *value unchanged), EBUSY outside the display window or
 * while the panel is not running, or EIO for an error the panel's code
 * reported.
 */
int
drv_i915_display_backlight_serve(
	struct i915_device *device,
	int in_display,
	int op,
	uint32_t *value)
{
	struct i915_display *display;
	uint32_t level;
	int result;
	int error;

	/* The light is the driver's only while it lights the panel. */
	display = device->display;
	if (display == NULL || !in_display)
		return EBUSY;
	if (display->output.none || display->output.kind != I915_OUTPUT_KIND_PANEL)
		return EBUSY;

	/* Switches the light (the panel's power for the display control): the pipe keeps running. */
	if (op == I915_BACKLIGHT_POWER) {
		result = drv_i915_xd_picture_backlight(&display->xd.pictures, I915_XD_PICTURE_RESIDENT, *value != 0U);
		error = i915_backlight_result(result);
		if (error != 0)
			return error;

		/* Succeeded: the light is switched. */
		return 0;
	}

	/* Sets the brightness first, when asked to. */
	if (op == I915_BACKLIGHT_SET) {
		result = drv_i915_xd_picture_brightness(&display->xd.pictures, I915_XD_PICTURE_RESIDENT, *value, I915_BACKLIGHT_PERCENT_MAX);
		error = i915_backlight_result(result);
		if (error != 0)
			return error;
	}

	/* Reads the brightness the panel has now. */
	level = 0U;
	result = drv_i915_xd_picture_brightness_get(&display->xd.pictures, I915_XD_PICTURE_RESIDENT, I915_BACKLIGHT_PERCENT_MAX, &level);
	error = i915_backlight_result(result);
	if (error != 0)
		return error;

	/* Succeeded: the brightness in percent. */
	*value = level;
	return 0;
}

/* Reads the brightness through the worker: the provider's get. */
static int
i915_backlight_get(
	void *context,
	uint32_t *percent)
{
	uint32_t level;
	int error;

	/* Asks the worker. */
	level = 0U;
	error = drv_i915_worker_sync_backlight(context, I915_BACKLIGHT_GET, &level);
	if (error != 0)
		return error;

	/* Succeeded: the brightness. */
	*percent = level;
	return 0;
}

/* Sets the brightness through the worker: the provider's set. */
static int
i915_backlight_set(
	void *context,
	uint32_t percent)
{
	uint32_t level;
	int error;

	/* Asks the worker. */
	level = percent;
	error = drv_i915_worker_sync_backlight(context, I915_BACKLIGHT_SET, &level);
	if (error != 0)
		return error;

	/* Succeeded: the panel has the brightness. */
	return 0;
}

/* Turns a picture result (I915_XD_PICTURE_*) into an errno value. */
static int
i915_backlight_result(
	int result)
{
	/* Says why the panel did not take or give the brightness. */
	if (result == I915_XD_PICTURE_NOT_READY)
		return EBUSY;
	if (result != I915_XD_PICTURE_OK)
		return EIO;

	/* Succeeded. */
	return 0;
}
