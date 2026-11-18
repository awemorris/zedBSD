/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The backlight on zedBSD (ws113-p013): the built-in panel's light is
 * /dev/backlight/backlight0, which the GPU driver registers while it lights
 * the panel, with the requests of FreeBSD's backlight(9)
 * (uapi/backlight.h).  sessiond gives the device to the seat's user, so the
 * compositor opens it for writing.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <uapi/backlight.h>

/* The built-in panel's backlight. */
#define BACKLIGHT_PATH		"/dev/backlight/backlight0"

/* The most a brightness may be (a percentage). */
#define BACKLIGHT_PERCENT_MAX	100U

/* One open backlight: its descriptor. */
struct kl_backend_backlight {
	int descriptor;
};

/*
 * Opens the built-in panel's backlight for reading and writing.
 */
int
kl_backend_backlight_open(
	struct kl_backend_backlight **backlight)
{
	struct kl_backend_backlight *opened;
	int descriptor;
	int error;

	/* Somewhere to put it. */
	if (backlight == NULL)
		return EINVAL;

	/* The device; a machine without a lit panel has none. */
	descriptor = open(BACKLIGHT_PATH, O_RDWR | O_CLOEXEC);
	if (descriptor < 0) {
		error = errno;
		return error;
	}

	/* The record. */
	opened = calloc(1U, sizeof(*opened));
	if (opened == NULL) {
		(void)close(descriptor);
		return ENOMEM;
	}

	/* The record owns the descriptor from here on. */
	opened->descriptor = descriptor;

	/* Succeeded: the caller has the backlight. */
	*backlight = opened;
	return 0;
}

/*
 * Reads the brightness: BACKLIGHTGETSTATUS.
 */
int
kl_backend_backlight_get(
	struct kl_backend_backlight *backlight,
	unsigned *percent)
{
	struct backlight_props props;
	int status;
	int error;

	/* An open backlight and somewhere to put the brightness. */
	if (backlight == NULL || percent == NULL)
		return EINVAL;

	/* Asks the device. */
	memset(&props, 0, sizeof(props));
	status = ioctl(backlight->descriptor, BACKLIGHTGETSTATUS, &props);
	if (status != 0) {
		error = errno;
		return error;
	}

	/* Succeeded: the brightness in percent. */
	*percent = props.brightness;
	return 0;
}

/*
 * Sets the brightness: BACKLIGHTUPDATESTATUS.
 */
int
kl_backend_backlight_set(
	struct kl_backend_backlight *backlight,
	unsigned percent)
{
	struct backlight_props props;
	int status;
	int error;

	/* An open backlight and a percentage. */
	if (backlight == NULL || percent > BACKLIGHT_PERCENT_MAX)
		return EINVAL;

	/* Asks the device. */
	memset(&props, 0, sizeof(props));
	props.brightness = percent;
	status = ioctl(backlight->descriptor, BACKLIGHTUPDATESTATUS, &props);
	if (status != 0) {
		error = errno;
		return error;
	}

	/* Succeeded: the panel has the brightness. */
	return 0;
}

/*
 * Closes the backlight.
 */
void
kl_backend_backlight_close(
	struct kl_backend_backlight *backlight)
{
	/* Nothing was opened. */
	if (backlight == NULL)
		return;

	/* The descriptor and the record. */
	(void)close(backlight->descriptor);
	free(backlight);
}
